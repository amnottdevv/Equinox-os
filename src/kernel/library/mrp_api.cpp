/*
 * ============================================================================
 *  mrp_api.cpp — Kernel-side implementation of the .mrp syscall table (v2)
 * ----------------------------------------------------------------------------
 *  This file contains:
 *    1. Wrapper functions for the new APIs (Batch 1 strings, Batch 2
 *       numbers, Batch 3 vector) exposed to .mrp programs via mrp_api_t.
 *    2. mrp_build_api() -- the function that assembles the struct
 *       mrp_api_t containing all kernel-side function pointers. Called
 *       from mrp_run().
 *
 *  Wrappers matter because:
 *    - Some kernel functions have signatures that do not exactly match the
 *      API promised to userland (e.g. the kernel's malloc() takes size_t,
 *      mrp_api.alloc takes uint32_t). Casting raw function pointers can
 *      be UB when type sizes differ.
 *    - Some kernel functions use a DIFFERENT memory arena (kernel heap
 *      vs MRP arena). The kernel Vector defaults to the kernel malloc() --
 *      if exposed directly, a .mrp program would silently eat the kernel
 *      heap instead of its own. An MRP-aware variant is needed.
 *    - Some functions need NULL / argument validation before delegating
 *      (defense in depth, since a buggy .mrp program must not crash the
 *      kernel).
 *
 *  Note about Ring 0: this file runs in full Ring 0 (no user/kernel split
 *  yet). There is no memory protection; a .mrp program could technically
 *      call other kernel functions via pointer arithmetic. But that is
 *      "forbidden" by convention -- the official .mrp contract is ONLY via
 *      mrp_api_t.
 *  When Ring 3 is active (TARGETS.md Stage 3), the calling mechanism
 *  changes to int 0x80 + a syscall number; the function signatures here
 *  stay the same, so old programs need no rewrite.
 * ============================================================================
 */

#include "header/mrp_loader.h"   // mrp_api_t, mrp_build_api
#include "header/mrp_api.h"      // (shim -> mrp_user/mrp_api.h)
#include "header/libstring.h"    // strlen, strcmp, strcpy, strcat, split
#include "header/itoa_atoi.h"    // atoi, itoa
#include "header/vector.h"       // Vector, vector_*
#include "header/malloc.h"       // mrp_alloc, mrp_free_all
#include "header/stdio.h"        // print_string, print_int, gets, getkey
#include "header/timer.h"        // get_tick

#include <stdint.h>
#include <stddef.h>

// ============================================================================
//  Wrapper Batch 1 — string manipulation
//  Delegates straight to libstring.cpp. No extra logic, just a cast to
//  the signature promised in mrp_api_t.
//  (libstring uses size_t; mrp_api_t also uses size_t for str_len -- match.)
// ============================================================================

static size_t mrp_str_len(const char* s) {
    if (!s) return 0;
    return strlen(s);
}

static int mrp_str_cmp(const char* a, const char* b) {
    // Libstring strcmp does not handle NULL well; guard here so a .mrp
    // program that forgets a null-check does not crash the kernel.
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return strcmp(a, b);
}

static char* mrp_str_cpy(char* dest, const char* src) {
    if (!dest || !src) return dest;
    return strcpy(dest, src);
}

static char* mrp_str_cat(char* dest, const char* src) {
    if (!dest || !src) return dest;
    return strcat(dest, src);
}

static int mrp_str_split(char* str, const char* delim,
                         char** tokens, int max_tokens) {
    if (!str || !delim || !tokens || max_tokens <= 0) return 0;
    return split(str, delim, tokens, max_tokens);
}

// ============================================================================
//  Wrapper Batch 2 — number conversion
// ============================================================================

static int mrp_to_int(const char* str) {
    if (!str) return 0;
    return atoi(str);
}

static void mrp_int_to_str(int num, char* buf, int base) {
    // The kernel itoa does not handle a NULL buf -- guard so a buggy
    // program does not crash the kernel.
    if (!buf) return;
    if (base < 2 || base > 36) base = 10;
    itoa(num, buf, base);
}

// ============================================================================
//  Wrapper Batch 3 — dynamic array (MRP-aware)
// ----------------------------------------------------------------------------
//  The key difference vs the kernel vector_*(): this allocates from the
//  MRP arena, NOT the kernel heap. How: build an internal vector variant
//  constructed manually with mrp_alloc() for the header & data array.
//
//  Minimal implementation: we do not re-use the kernel vector.cpp (which
//  hardcodes malloc()); instead we reimplement 4 operations
//  (create/push/get/size) inline here with mrp_alloc. Simpler & it
//  guarantees arena isolation.
//
//  Internal layout (opaque to userland):
//    struct MrpVector {
//        void*  data;        // dynamic element array
//        size_t elem_size;
//        size_t size;
//        size_t capacity;
//    };
//  Userland never touches these fields directly -- access is only via
//  the vec_*() entries in mrp_api_t.
// ============================================================================

struct MrpVector {
    void*  data;
    size_t elem_size;
    size_t size;
    size_t capacity;
};

// Small initial capacity (4 elements) so the first alloc stays small.
// Standard 2x growth factor (like std::vector).
#define MRP_VEC_INITIAL_CAP 4

static struct MrpVector* mrp_vec_create(size_t elem_size) {
    if (elem_size == 0) return nullptr;

    // The MrpVector header is also allocated from the MRP arena, so when
    // mrp_free_all() is called it is cleaned up automatically.
    struct MrpVector* v = (struct MrpVector*)mrp_alloc(sizeof(struct MrpVector));
    if (!v) return nullptr;

    v->elem_size = elem_size;
    v->size      = 0;
    v->capacity  = MRP_VEC_INITIAL_CAP;

    // Overflow check before the alloc (defensive; MRP_VEC_INITIAL_CAP is
    // small so it almost never triggers, but a very large elem_size can).
    if (v->capacity > ((size_t)-1) / v->elem_size) {
        return nullptr;  // size overflow
    }
    v->data = mrp_alloc((uint32_t)(v->capacity * v->elem_size));
    if (!v->data) {
        // Cannot mrp_free(v) because the v2 allocator has no granular free.
        // This case only happens when the MRP arena is nearly full at create
        // time. Since mrp_free_all() will clean up later, we just return NULL.
        return nullptr;
    }
    return v;
}

static int mrp_vec_push(struct MrpVector* v, const void* elem) {
    if (!v || !elem) return -1;

    if (v->size >= v->capacity) {
        // Grow 2x. Allocate a new buffer, copy the old one, drop the old
        // one (cannot free it, so the old buffer becomes a "ghost" until
        // mrp_free_all(). This is the v2 trade-off: an edge case for
        // programs that push many times. With a granular free() in v3,
        // this becomes a proper realloc.
        size_t new_cap  = v->capacity * 2;
        if (new_cap <= v->capacity) return -1;  // capacity overflow

        // Overflow check for the product new_cap * elem_size BEFORE the
        // alloc. SIZE_MAX / elem_size = the maximum safe capacity. If
        // new_cap exceeds it, the alloc would request a wrongly-small
        // (wrapped) buffer & corrupt data.
        if (v->elem_size > 0 && new_cap > ((size_t)-1) / v->elem_size) {
            return -1;  // size overflow
        }
        size_t new_bytes = new_cap * v->elem_size;

        void* new_data = mrp_alloc((uint32_t)new_bytes);
        if (!new_data) return -1;

        // Manual memcpy (the kernel has memcpy in libstring.cpp).
        // Byte-wise for now to avoid extra dependencies.
        uint8_t* dst = (uint8_t*)new_data;
        const uint8_t* src = (const uint8_t*)v->data;
        for (size_t i = 0; i < v->size * v->elem_size; i++) {
            dst[i] = src[i];
        }
        v->data     = new_data;
        v->capacity = new_cap;
    }

    // Copy the element into the size++ slot
    uint8_t* dst = (uint8_t*)v->data + (v->size * v->elem_size);
    const uint8_t* src = (const uint8_t*)elem;
    for (size_t i = 0; i < v->elem_size; i++) {
        dst[i] = src[i];
    }
    v->size++;
    return 0;
}

static void* mrp_vec_get(const struct MrpVector* v, size_t index) {
    if (!v || index >= v->size) return nullptr;
    return (uint8_t*)v->data + (index * v->elem_size);
}

static size_t mrp_vec_size(const struct MrpVector* v) {
    if (!v) return 0;
    return v->size;
}

// v2 no-op: the arena is reset when the program exits. Provided in the API
// so a program that wants an explicit tear-down before exit can call it
// without an error. Implementation: set size=0 (the data stays in the
// arena, no granular free).
static void mrp_vec_free(struct MrpVector* v) {
    if (!v) return;
    v->size = 0;
    v->capacity = 0;
    v->data = nullptr;  // ghost allocation, cleaned up by mrp_free_all()
}

// ============================================================================
//  Input wrapper (v1) — read_line needs a helper because the kernel gets()
//  does not return a length; mrp_api_t promises an int string length.
// ============================================================================

static int mrp_read_line(char* buf, int max_len) {
    if (!buf || max_len <= 0) return 0;
    gets(buf, max_len);
    int len = 0;
    while (buf[len] != '\0' && len < max_len) len++;
    return len;
}

// ============================================================================
//  Memory wrapper (v1) — cast uint32_t to size_t (same width on i686).
//  Keep the explicit cast so the compiler does not warn under -Wconversion.
// ============================================================================

static void* mrp_alloc_wrapper(uint32_t size) {
    return mrp_alloc((size_t)size);
}

// ============================================================================
//  mrp_build_api — rakit syscall table
// ----------------------------------------------------------------------------
//  Single source of truth for the "kernel function -> api field" mapping.
//  Called from mrp_run() right before calling the program entry.
//  When a new kernel function needs to be exposed, edit it here, NOT in
//  the mrp_run() body.
// ============================================================================

extern "C" struct mrp_api_t mrp_build_api(void) {
    struct mrp_api_t api;

    // ----- Mandatory header -----
    api.api_version = MRP_API_VERSION;

    // ----- v1: I/O & memory -----
    api.print_text = print_string;
    api.print_int   = print_int;
    api.read_line   = mrp_read_line;
    api.get_key     = getkey;
    api.alloc       = mrp_alloc_wrapper;
    api.get_tick    = get_tick;

    // ----- v2 Batch 1: string -----
    api.str_len    = mrp_str_len;
    api.str_cmp    = mrp_str_cmp;
    api.str_cpy    = mrp_str_cpy;
    api.str_cat    = mrp_str_cat;
    api.str_split  = mrp_str_split;

    // ----- v2 Batch 2: numbers -----
    api.to_int     = mrp_to_int;
    api.int_to_str = mrp_int_to_str;

    // ----- v2 Batch 3: vector (MRP-aware) -----
    api.vec_create = (struct Vector* (*)(size_t))mrp_vec_create;
    api.vec_push   = (int  (*)(struct Vector*, const void*))mrp_vec_push;
    api.vec_get    = (void* (*)(const struct Vector*, size_t))mrp_vec_get;
    api.vec_size   = (size_t (*)(const struct Vector*))mrp_vec_size;
    api.vec_free   = (void  (*)(struct Vector*))mrp_vec_free;

    return api;
}
