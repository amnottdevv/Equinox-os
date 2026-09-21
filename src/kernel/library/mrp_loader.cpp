#include "header/mrp_loader.h"
#include "header/syscall.h"   /* v0.3 FR-01: syscall_fd_flush_all on program exit */
#include "header/mrp_api.h"      // (shim -> mrp_user/mrp_api.h, mrp_api_t)
#include "header/mrp_format.h"
#include "header/elf.h"         // v0.3 (FR-07): ELF32 executables
#include "header/fs_ram.h"
#include "header/stdio.h"
#include "header/malloc.h"
#include "header/timer.h"
#include "header/usermode.h"     // user3_launch / USER_STACK_TOP (v10.7)
#include "header/task.h"        // Phase A: per-task arena + page dir
#include "header/paging.h"      // Phase A: paging_kernel_dir
#include "header/libstring.h"   // strstr (mrp_arena_hint_for)
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
//  Phase A — the re-entrancy guard is now PER-TASK (Task::user_running).
//  Programs in task A and task B may run CONCURRENTLY (separate physical
//  arenas via per-task page directories). Nested exec within the SAME
//  task is still rejected: its physical chunk still holds the caller's
//  code. mrp_exec_active() globally reports the CURRENT task.
// ============================================================================

static int mrp_quiet = 0;

extern "C" void mrp_set_quiet(int quiet) {
    mrp_quiet = quiet ? 1 : 0;
}

extern "C" int mrp_exec_active(void) {
    struct Task* t = task_current();
    return (t && t->user_running) ? 1 : 0;
}

/* Phase A.1 (DOOM regression fix): arena hint policy in ONE place.
 * See the header comment for the rationale. */
extern "C" uint32_t mrp_arena_hint_for(const char* name) {
    if (name) {
        if (strstr(name, "doom")) return MRP_ARENA_DOOM;
        if (strstr(name, "mtcc")) return MRP_ARENA_MTCC;
    }
    return MRP_ARENA_DEFAULT;
}

/* The old mrp_run() body. The per-task guard is checked by the wrapper.
 * v0.3: BOTH formats (.mrp and ELF32 — FR-07) run through the
 * per-task DEMAND window (FR-06): the VMA range is RESERVED with
 * non-present PTEs; pages appear zero-filled on the first touch
 * (CPL 3 program access or this loader's own VMA writes — the
 * current task is the right task here, so the #PF demand path is
 * valid). No upfront physical chunk: a 24 MB DOOM arena only costs
 * the pages DOOM actually touches. */
static int mrp_run_inner(struct fs_node* parent, const char* name,
                         uint32_t arena_heap_hint) {
    struct Task* t = task_current();

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

    int    is_elf = 0;
    uint32_t entry = 0;

    if (elf_is_elf(file_data, total_len)) {
        // ----- 2a. ELF32 (FR-07): validate + load through the demand window
        is_elf = 1;
        if (task_user_map_demand(t, ELF_HEAP_VMA, ELF_HEAP_BYTES) != 0)
            return MRP_RUN_ERR_NO_MEMORY;
        task_load_cr3(t->page_dir);          /* the VMA window goes live */
        if (elf_load(t, file_data, total_len) != 0) {
            task_user_unmap(t);
            t->page_dir = paging_kernel_dir();
            task_load_cr3(t->page_dir);
            return MRP_RUN_ERR_INVALID;
        }
        uint32_t elf_entry = 0, elf_lo = 0, elf_span = 0;
        elf_check(file_data, total_len, &elf_entry, &elf_lo, &elf_span);
        entry = elf_entry;
        if (!mrp_quiet) {
            printf("elf: running '%s' RING 3 (image %u KB demand @0x%08x, entry 0x%08x)\n",
                   name, elf_span / 1024u, elf_lo, elf_entry);
        }
    } else {
        // ----- 2b. .mrp: validate header -----
        enum mrp_validate_reason reason;
        if (!is_valid_mrp(file_data, total_len, &reason)) {
            printf("mrp: '%s' is not a valid .mrp — %s\n", name, mrp_reason_str(reason));
            return MRP_RUN_ERR_INVALID;
        }

        const struct mrp_header* hdr = (const struct mrp_header*)file_data;
        const uint8_t* code_src = file_data + MRP_HEADER_SIZE;

        // ----- 3. Reserve the demand window (no physical chunk) -----
        uint32_t need = (hdr->code_size + arena_heap_hint + 0xFFFu) & ~0xFFFu;
        if (task_user_map_demand(t, USER_ARENA_START, need) != 0)
            return MRP_RUN_ERR_NO_MEMORY;
        task_load_cr3(t->page_dir);          /* the VMA window goes live */

        // ----- 4. This task's arena allocator + alloc & copy code -----
        // mrp_heap_init / mrp_alloc / the memcpy below write through
        // the VMA window: non-present demand pages fault in through
        // the CPL 0 #PF path (the CURRENT task — safe by design).
        mrp_heap_init();
        void* exec_buf = mrp_alloc(hdr->code_size);
        if (!exec_buf) {
            printf("mrp: failed to allocate %u bytes in the MRP arena\n",
                   hdr->code_size);
            task_user_unmap(t);
            t->page_dir = paging_kernel_dir();
            task_load_cr3(t->page_dir);
            return MRP_RUN_ERR_NO_MEMORY;
        }

        uint8_t* dst = (uint8_t*)exec_buf;
        for (uint32_t i = 0; i < hdr->code_size; i++) {
            dst[i] = code_src[i];
        }

        entry = (uint32_t)(uintptr_t)exec_buf + hdr->entry_offset;

        if (!mrp_quiet) {
            printf("mrp: running '%s' RING 3 (%u bytes code, entry+0x%x, arena %u KB demand @0x%x)\n",
                   name, hdr->code_size, hdr->entry_offset,
                   (uint32_t)(t->mrp_arena.max - t->mrp_arena.start) / 1024u,
                   (uint32_t)t->mrp_arena.start);
        }
    }

    user_trampoline_init();
    t->user_running = 1;
    uint32_t exit_status = user3_launch(entry, USER_STACK_TOP, 0 /*arg legacy*/);
    t->user_running = 0;
    int normal = user3_exit_normal();

    if (!mrp_quiet) {
        if (normal) {
            printf("%s: '%s' exited with status %u, cleaning up\n",
                   is_elf ? "elf" : "mrp", name, exit_status);
        } else {
            printf("%s: '%s' was terminated (fault), cleaning up\n",
                   is_elf ? "elf" : "mrp", name);
        }
    }

    // ----- 7. Cleanup: release the VMA window — task_user_unmap walks
    // the page tables and returns every FAULTED-IN page to the pool.
    // Also drop the console's pixel canvas (if the program drew one).
    // -----
    if (t->console >= 0) console_canvas_invalidate(t->console);
    /* v0.3 FR-01: the program is gone — flush + close every fd it left
     * open (a program that forgot close() still gets its writes on
     * disk; the FAT content caches are released per FR-08). */
    syscall_fd_flush_all();
    task_sched_force_unlock();   /* v0.3: leak safety net (nettask) */
    task_user_unmap(t);
    t->page_dir = paging_kernel_dir();
    task_load_cr3(t->page_dir);

    return MRP_RUN_OK;
}

// ============================================================================
//  mrp_run / mrp_run_hint — public entry points with the per-task guard.
//  Nested exec within the SAME task is rejected (its arena still holds
//  the caller's code). Programs in OTHER tasks keep running — that is
//  the Phase A "nested program" feature: doom on console 1 while
//  hello.mrp runs on console 2.
// ============================================================================
extern "C" int mrp_run(struct fs_node* parent, const char* name) {
    return mrp_run_hint(parent, name, MRP_ARENA_DEFAULT);
}

extern "C" int mrp_run_hint(struct fs_node* parent, const char* name,
                            uint32_t arena_heap_hint) {
    struct Task* t = task_current();
    if (t && t->user_running) {
        printf("mrp: exec '%s' rejected (EBUSY) — this task is still running another program\n", name);
        return MRP_RUN_ERR_BUSY;
    }
    return mrp_run_inner(parent, name, arena_heap_hint);
}
