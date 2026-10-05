#ifndef MRP_API_H
#define MRP_API_H

/*
 * ============================================================================
 *  mrp_api.h — Single Source of Truth for the .mrp program API
 * ----------------------------------------------------------------------------
 *  This file is the ONE AND ONLY place where `struct mrp_api_t` is defined.
 *  The kernel (kernel/library/header/mrp_api.h) is ONLY a shim that
 *  #includes this file. Userland programs also #include this file directly.
 *  With just one definition, kernel & userland CANNOT drift apart like the
 *  old version that kept 2 separate copies in mrp_loader.h & mrp_api.h.
 *
 *  Version history:
 *    v1 (Stage 0)        : print_text, print_int, read_line, get_key, alloc,
 *                          get_tick (6 functions).
 *    v2 (Stage 1, this)  : adds Batch 1 (string), Batch 2 (number),
 *                          Batch 3 (vector) — see workflow_mrp.md Part B.
 *
 *  ABI note: still called through the function-pointer table (not int 0x80),
 *  until Ring 3 goes live (TARGETS.md Stage 3). Since v5, the int 0x80 layer
 *  is ALREADY alive in the kernel (kernel/library/header/syscall.h — 16
 *  stably-numbered syscalls: exit/exec/getpid/write/read/open/close/getkey/
 *  readline/print/printint/malloc/gettick/sleep + v6: getargs/mkfile for
 *  mtcc). Old .mrp programs stay compatible through this table; new programs
 *  (including future tcc-compiled ones) are encouraged to call the syscall
 *  numbers directly — the order & signatures in this struct are deliberately
 *  kept semantically similar, so migrating only swaps the call mechanism,
 *  not the program logic.
 * ============================================================================
 */

#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
//  Versioning -- a .mrp program may check the API version at runtime if it
//  wants to use new features conditionally. The kernel SETS this number when
//  filling api.api_version = MRP_API_VERSION before calling the program entry.
// ----------------------------------------------------------------------------
#define MRP_API_VERSION 2

// Forward declaration: kernel vector (see kernel/library/header/vector.h).
// In userland this struct is treated as opaque -- programs only hold a
// Vector* pointer and access it through the vec_*() calls below. If a .mrp
// program needs to read the size field directly, use vec_size() (not
// v->size), so the kernel stays free to refactor the Vector layout without
// breaking the userland ABI.
struct Vector;

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  mrp_api_t — v2 syscall table
// ----------------------------------------------------------------------------
//  IMPORTANT: add new functions ONLY at the end of the struct; never insert
//  in the middle or reorder existing fields. Old fields are accessed at fixed
//  offsets by .mrp programs compiled against older versions — an insertion
//  shifts the offsets & crashes old binaries.
//
//  The grouping is made explicit through comments so a reader of this
//  struct can scan by category without tracing kernel code.
// ============================================================================
struct mrp_api_t {

    // ----- Mandatory header (all versions) -----
    uint32_t api_version;     // == MRP_API_VERSION, filled by the kernel before the call

    // ----- Output (v1) -----
    void (*print_text)(const char* text);   // plain string, no automatic newline
    void (*print_int)(uint32_t num);        // unsigned decimal

    // ----- Input (v1) -----
    int  (*read_line)(char* buf, int max_len);  // blocking, returns the length
    int  (*get_key)(void);                       // non-blocking, -1 if empty

    // ----- Memory (v1) -- from the MRP arena, not the kernel heap -----
    void* (*alloc)(uint32_t size);
    // Note: there is no per-allocation free() in v1/v2. The MRP arena is
    // fully reset on every program exit (mrp_free_all()). Programs that
    // need repeated alloc/free WITHIN one run can use vec_*() (Batch 3,
    // which internally uses mrp_alloc too), or wait for granular free()
    // in v3.

    // ----- Misc (v1) -----
    uint32_t (*get_tick)(void);   // timer ticks since boot

    // ========================================================================
    //  Batch 1 — string manipulation (v2, workflow_mrp.md Part B.1)
    //  All of these wrap kernel libstring.cpp functions; no new logic.
    // ========================================================================
    size_t   (*str_len)(const char* s);
    int      (*str_cmp)(const char* a, const char* b);
    char*    (*str_cpy)(char* dest, const char* src);
    char*    (*str_cat)(char* dest, const char* src);
    int      (*str_split)(char* str, const char* delim,
                          char** tokens, int max_tokens);

    // ========================================================================
    //  Batch 2 — number conversion (v2)
    //  Wraps the kernel's atoi() & itoa().
    // ========================================================================
    int      (*to_int)(const char* str);              // wraps atoi()
    void     (*int_to_str)(int num, char* buf, int base);  // wraps itoa()

    // ========================================================================
    //  Batch 3 — dynamic array (v2, MRP-aware)
    //  Internally uses mrp_alloc(), not the kernel malloc(). Since the MRP
    //  arena is reset on exit, programs are not required to call vec_free()
    //  -- but it is still provided for programs that alloc/dealloc
    //  repeatedly within one run.
    // ========================================================================
    struct Vector* (*vec_create)(size_t elem_size);
    int            (*vec_push)(struct Vector* v, const void* elem);
    void*          (*vec_get)(const struct Vector* v, size_t index);
    size_t         (*vec_size)(const struct Vector* v);
    void           (*vec_free)(struct Vector* v);   // optional, no-op effect on exit
};

#ifdef __cplusplus
}
#endif

/*
 *  MRP_ENTRY macro -- declares the .mrp program entry point.
 *  The ".start" section makes the linker script (link_mrp.ld) place _start
 *  at offset 0 -> the packer (mrp_pack.py) always uses entry_offset = 0
 *  without parsing the ELF symbol table.
 */
#define MRP_ENTRY \
    extern "C" __attribute__((section(".start"))) \
    void _start(struct mrp_api_t* api)

/*
 *  Optional helper: check API compatibility at runtime.
 *  Returns 1 if the kernel provides api_version >= what the program asked
 *  for. Old programs (compiled against v1) do not call this -- no problem,
 *  because the kernel still fills the api_version field even if the program
 *  never reads it.
 */
static inline int mrp_api_check_version(const struct mrp_api_t* api,
                                         uint32_t required) {
    return api && api->api_version >= required;
}

#endif /* MRP_API_H */
