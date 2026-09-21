#ifndef MRP_LOADER_H
#define MRP_LOADER_H

/*
 * ============================================================================
 *  MRP Loader v2 — Ring 0 (no user mode / paging yet)
 * ----------------------------------------------------------------------------
 *  Brief history:
 *    v1: mrp_api_t was defined TWICE (here & in mrp_user/mrp_api.h).
 *        Old comments said "MUST be byte-identical" -- but there was no
 *        mechanism to check it, so they would silently drift whenever one
 *        side was changed.
 *    v2 (this): mrp_api_t has a SINGLE source of truth in mrp_user/mrp_api.h.
 *        This file just #includes "mrp_api.h" (a shim to the userland header).
 *        Loader-specific declarations (mrp_run, mrp_entry_fn, mrp_run_result)
 *        stay here because they are an internal kernel contract, not a user
 *        API.
 * ============================================================================
 */

#include <stdint.h>
#include "fs_ram.h"
#include "mrp_api.h"   // <- single source of truth for struct mrp_api_t
#include "vector.h"    // <- Vector (passed to vec_* via the api)

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Old design note about the function-pointer table (still relevant):
 *
 *  Because .cpp compilation output is a FLAT BINARY (not ELF), there is no
 *  symbol table to resolve kernel function names from inside the program.
 *  The solution: the kernel PASSES a struct of function pointers (the
 *  "syscall table") to the program via the first argument of its entry
 *  point. The program can only touch the outside world through these
 *  pointers.
 *
 *  This is DELIBERATELY made similar to a real syscall pattern (like
 *  int 0x80 on Linux) so that when we upgrade to Ring 3 later, existing
 *  .mrp programs will not need to be rewritten from scratch -- only the
 *  calling mechanism changes (function-pointer call -> software
 *  interrupt), not the API.
 */

// Entry point type for .mrp programs: void _start(mrp_api_t* api)
// The program MUST return normally (not reboot/hlt) so that the loader can
// take control back (it is invoked with `call`, not `jmp`).
typedef void (*mrp_entry_fn)(struct mrp_api_t*);

enum mrp_run_result {
    MRP_RUN_OK              = 0,
    MRP_RUN_ERR_NOT_FOUND   = -1,  // file does not exist in the fs
    MRP_RUN_ERR_IS_DIR      = -2,  // the given path is a directory
    MRP_RUN_ERR_INVALID     = -3,  // is_valid_mrp() failed -- see the printf log
    MRP_RUN_ERR_NO_MEMORY   = -4,  // MRP heap arena full, code does not fit
    MRP_RUN_ERR_BUSY        = -5,  // FIX(audit V3 #1): nested exec rejected - the calling program's code is still live in the MRP arena
};

// Load & run a .mrp file from RAMFS. Blocks until the program finishes.
// parent = directory where the file resides, name = file name (e.g. "hello.mrp").
// Returns one of the mrp_run_result codes (0 = ran & finished normally).
//
// If `name` ends with "/" or is not a regular file, returns
// MRP_RUN_ERR_IS_DIR / MRP_RUN_ERR_NOT_FOUND depending on the case.
int mrp_run(struct fs_node* parent, const char* name);

/* Phase A (multitasking): variant with an explicit arena size hint —
 * different programs need different heap slack (mtcc ~8 MB, doom
 * ~24 MB, small programs are fine with 2 MB). The hint is rounded
 * up to 4 KB pages. */
int mrp_run_hint(struct fs_node* parent, const char* name, uint32_t arena_heap_hint);

/* Phase A.1 (DOOM regression fix): pick the arena hint from the
 * program NAME. Pre-multitasking, the .mrp arena was one static 33 MB
 * region, so DOOM silently had ~24+ MB of heap; the per-task loader
 * cut that to a flat 2 MB, which cannot even hold the shareware WAD
 * (4.2 MB) — `doom` failed with an arena allocation error. Every run
 * path (shell `run` / `./` / `doom` / `spawn` / SYS_EXEC) now routes
 * through this helper so the policy lives in ONE place. */
uint32_t mrp_arena_hint_for(const char* name);

/* Default hint (heap bytes in addition to the code image). */
#define MRP_ARENA_DEFAULT  (2u * 1024u * 1024u)
#define MRP_ARENA_MTCC     (8u * 1024u * 1024u)
#define MRP_ARENA_DOOM     (24u * 1024u * 1024u)

// Quiet mode: suppresses loader info messages ("mrp: running ..."/"finished").
// Used by the global tool dispatch in the shell (`mtcc main.c` from any
// directory) so tool output stays clean -- only the program's output.
// ERROR messages are always printed regardless of the mode. `run`/`./` do
// not use quiet.
void mrp_set_quiet(int quiet);

// FIX(audit V3 #1): returns != 0 while mrp_run() is active (a .mrp program
// is running in the MRP arena). Kernel-internal - not a program API.
int mrp_exec_active(void);

/*
 *  Internal helpers (used by mrp_loader.cpp, exposed here so they can be
 *  unit-tested from a separate test file if desired). Not meant to be called
 *  directly from kernel.cpp or from a .mrp program.
 *
 *  mrp_build_api() builds the struct mrp_api_t containing all kernel-side
 *  function pointers. It is split into its own function (not inlined in
 *  mrp_run) so that:
 *    1. Testable -- tests can call mrp_build_api() & inspect its fields
 *       without having to trigger a full load cycle.
 *    2. Single point of update -- when a new kernel function needs to be
 *       exposed, just edit it here, not in the body of mrp_run().
 */
struct mrp_api_t mrp_build_api(void);

#ifdef __cplusplus
}
#endif

#endif
