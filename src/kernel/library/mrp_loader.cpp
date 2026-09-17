#include "header/mrp_loader.h"
#include "header/mrp_api.h"      // (shim -> mrp_user/mrp_api.h, mrp_api_t)
#include "header/mrp_format.h"
#include "header/fs_ram.h"
#include "header/stdio.h"
#include "header/malloc.h"
#include "header/timer.h"
#include "header/usermode.h"     // user3_launch / USER_STACK_TOP (v10.7)
#include <stdint.h>
#include <stddef.h>

/*
 * ============================================================================
 *  mrp_run() implementation - v2
 * ----------------------------------------------------------------------------
 *  Flow (v10.7 - RING 3):
 *   1. Find the file in the fs; validate: not a directory, not empty.
 *   2. Validate the .mrp header with is_valid_mrp() (magic, version,
 *      size, checksum). On failure, print the reason & exit.
 *   3. Reset the MRP heap arena (mrp_heap_init) -- any previous
 *      program is discarded HERE, not at the end. So even if the
 *      previous program crashed/hung and forgot to clean up, the
 *      next one still gets a clean arena.
 *   4. Allocate a code_size buffer from the MRP arena; copy the code
 *      (NOT the header) there with a byte-wise memcpy (NOT strcpy --
 *      the code may contain embedded 0x00 bytes).
 *   5. Compute the entry address = base_alloc + entry_offset.
 *   6. user3_launch(entry, USER_STACK_TOP, 0): an exit stub is placed
 *      on the user trampoline page, the kernel context is saved, then
 *      iret into CPL 3 (CS=0x1B, DS=0x23, a 64KB user stack). The
 *      program runs ISOLATED by paging (the arena = user pages); all
 *      kernel access goes through int 0x80 only. exit() and faults
 *      return to the point after launch via kern_longjmp - the shell lives.
 *   7. Clean up the arena + return to the caller (mrp_run / shell).
 *
 *  Security: the MRP arena = USER pages (U/S=1); everything else is
 *  supervisor. Syscall pointers from programs are validated (uaccess,
 *  SYS_EFAULT). Program faults (div 0, null deref, writing kernel
 *  memory, bad opcodes, stack overflow into a guard page) = the program
 *  is killed with a clean report; kernel panic is for CPL 0 bugs only.
 *
 *  v2 changes vs v1:
 *    - mrp_api_t is built via mrp_build_api(), not inline. Less duplication.
 *    - Support entry_offset != 0 (defensive - the packer always uses 0
 *      today, but if it later puts a trampoline or prolog in front of
 *      _start, the loader needs no changes).
 *
 *  v3 changes (audit V3 bug #1 - re-entrant exec):
 *    - mrp_run() is split: mrp_run_inner() (the old body) + a wrapper
 *      that REFUSES nested exec with MRP_RUN_ERR_BUSY. Without this
 *      guard, exec() from a still-running .mrp program (int 0x80 ->
 *      sys_exec -> mrp_run) would make mrp_heap_init() reset the arena
 *      that holds the caller's own code -> the CPU executes
 *      overwritten memory -> #GP/#UD -> kernel panic.
 * ============================================================================ */

// ============================================================================
//  FIX(audit V3 #1) - mrp_run() re-entrancy state.
//  mrp_exec_depth != 0 means an mrp_run() is still active on the
//  kernel call stack (= a .mrp program whose CODE is alive in the MRP
//  arena 0x500000-0x900000, including tcc.mrp + its compiled code).
// ============================================================================
static int mrp_exec_depth = 0;

// ============================================================================
//  QUIET MODE - used by the shell's global tool dispatch (`mtcc main.c`,
//  `hello.mrp` etc). Without it, global tools print loader lines
//  ("mrp: running ..." / "mrp: finished ...") around the program's
//  own output, which feels noisy. The `run` / `./` commands do NOT
//  use quiet - their output stays exactly as before (same as run).
// ============================================================================
static int mrp_quiet = 0;

extern "C" void mrp_set_quiet(int quiet) {
    mrp_quiet = quiet ? 1 : 0;
}

extern "C" int mrp_exec_active(void) {
    return mrp_exec_depth;
}

/* The old mrp_run() body. May ONLY be called by the mrp_run() wrapper
 * below, after the depth check proved safe (mrp_exec_depth == 0). */
static int mrp_run_inner(struct fs_node* parent, const char* name) {
    // ----- 1. Lookup file -----
    struct fs_node* file = fs_find_child(parent, name);
    if (!file) {
        printf("mrp: '%s' not found\n", name);
        return MRP_RUN_ERR_NOT_FOUND;
    }
    if (file->is_dir) {
        printf("mrp: '%s' is a directory\n", name);
        return MRP_RUN_ERR_IS_DIR;
    }
    if (!file->content || file->size == 0) {
        printf("mrp: '%s' is empty\n", name);
        return MRP_RUN_ERR_INVALID;
    }

    const uint8_t* file_data = (const uint8_t*)file->content;
    uint32_t total_len = file->size;

    // ----- 2. Validate header -----
    enum mrp_validate_reason reason;
    if (!is_valid_mrp(file_data, total_len, &reason)) {
        printf("mrp: '%s' is not a valid .mrp — %s\n", name, mrp_reason_str(reason));
        return MRP_RUN_ERR_INVALID;
    }

    const struct mrp_header* hdr = (const struct mrp_header*)file_data;
    const uint8_t* code_src = file_data + MRP_HEADER_SIZE;

    // ----- 3. Reset the MRP arena BEFORE alloc (see the file header) -----
    mrp_heap_init();

    // ----- 4. Alloc & copy code -----
    void* exec_buf = mrp_alloc(hdr->code_size);
    if (!exec_buf) {
        printf("mrp: failed to allocate %u bytes in the MRP arena (full/too small)\n",
               hdr->code_size);
        return MRP_RUN_ERR_NO_MEMORY;
    }

    // Byte-wise copy: the code may contain embedded 0x00 bytes.
    uint8_t* dst = (uint8_t*)exec_buf;
    for (uint32_t i = 0; i < hdr->code_size; i++) {
        dst[i] = code_src[i];
    }

    // ----- 5. Compute entry address -----
    uint32_t entry = (uint32_t)(uintptr_t)exec_buf + hdr->entry_offset;

    if (!mrp_quiet) {
        printf("mrp: running '%s' RING 3 (%u bytes code, entry+0x%x)\n",
               name, hdr->code_size, hdr->entry_offset);
    }

    /* ----- 6. HANDING CONTROL TO RING 3 (v10.7) ---------------------
     *
     * BEFORE (ring 0): `entry(&api)` - the program ran at CPL 0 with
     * full kernel access; `1/0` in a program panicked the whole OS.
     *
     * NOW: user3_launch installs the exit stub on the user trampoline
     * page, saves the kernel context (resume point + callee-saved
     * registers), then irets into CPL 3 with:
     *   CS=0x1B DS=0x23, ESP=USER_STACK_TOP, EFLAGS IF=1 IOPL=0
     *   the user stack holds [exit stub address][legacy arg = 0]
     *
     * This function does NOT return the normal way - only through the
     * exit()/fault path (kern_longjmp to the landing pad in usermode.cpp),
     * which restores this C frame intact. The MRP arena (0x500000-
     * 0x900000) is a USER region under paging - the program's code +
     * heap are fully isolated from the kernel. Program faults are killed by the exception handler, not a panic. */
    user_trampoline_init();
    uint32_t exit_status = user3_launch(entry, USER_STACK_TOP, 0 /*arg legacy*/);
    int normal = user3_exit_normal();
    // ----- KONTROL BALIK KE KERNEL (via exit syscall atau kill-path) -----

    if (!mrp_quiet) {
        if (normal) {
            printf("mrp: '%s' exited with status %u, cleaning up MRP heap\n",
                   name, exit_status);
        } else {
            printf("mrp: '%s' was terminated (fault), cleaning up MRP heap\n", name);
        }
    }

    // ----- 7. Cleanup MRP arena -----
    mrp_free_all();

    return MRP_RUN_OK;
}

// ============================================================================
//  mrp_run - public entry with the re-entrancy guard (audit V3 #1 fix).
//
//  Nested exec (a .mrp program calling exec() via syscall #2 while
//  still running) is REFUSED here with MRP_RUN_ERR_BUSY, NOT passed to
//  mrp_run_inner(). There is only one MRP arena and it currently holds
//  the caller's code: mrp_heap_init() in inner would reset that arena
//  and the caller's code would be overwritten -> wild execution -> panic.
//  The proper fix (per-call arenas / ring 3 + paging) is phase 3 of the roadmap.
// ============================================================================
extern "C" int mrp_run(struct fs_node* parent, const char* name) {
    if (mrp_exec_depth != 0) {
        printf("mrp: exec '%s' rejected (EBUSY) — another .mrp program is still running in the MRP arena\n", name);
        return MRP_RUN_ERR_BUSY;
    }
    mrp_exec_depth = 1;
    int r = mrp_run_inner(parent, name);
    mrp_exec_depth = 0;
    return r;
}
