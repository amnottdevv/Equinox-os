/*
 * ============================================================================
 *  syscall.cpp — Equinox OS syscall layer implementation (int 0x80)
 * ----------------------------------------------------------------------------
 *  This file contains:
 *    1. isr_128     — assembly stub: pushal -> call dispatch -> popal -> iretd.
 *                     An explicit stub (not __attribute__((interrupt))) so the
 *                     register layout on the stack is DETERMINISTIC: we know
 *                     exactly at which offset EAX/EBX/ECX/EDX sit, and can
 *                     overwrite the EAX slot with the return value before popal.
 *    2. syscall_dispatch — reads the number from the EAX slot, fetches the
 *                     arguments from the EBX/ECX/EDX slots, calls the table
 *                     handler, writes the return value back into the EAX slot.
 *    3. Syscall table (table-driven): adding a syscall = add 1 function + 1
 *                     table entry. Empty slot -> SYS_ENOSYS automatically.
 *    4. fd table for RAMFS files (fd 0-2 console, 3+ files, read-only).
 *
 *  Security (ring 0, no paging yet): argument pointers are only NULL-checked.
 *  A wild pointer from the caller can still fault the kernel — the same
 *  exposure as the rest of the kernel code. Full isolation comes with ring 3.
 *
 *  This file is compiled with -mgeneral-regs-only (FPU_SENSITIVE in the
 *  makefile) because it contains an interrupt entry — no x87/SSE
 *  instructions are allowed that could trigger a recursive #NM.
 * ============================================================================
 */

#include "header/syscall.h"
#include "header/stdio.h"       // printf, put_char, print_string, print_int, gets, getkey
#include "header/fs_ram.h"      // fs_get_root, fs_find_child, fs_get_node_from_path
#include "header/malloc.h"      // mrp_alloc (arena .mrp)
#include "header/timer.h"       // get_tick, sleep_ms
#include "header/mrp_loader.h"  // mrp_run (exec .mrp from RAMFS)
#include "header/vesa.h"        // framebuffer getters + draw (syscalls 20-22)
#include "header/ps2_mouse.h"   // mouse_get_state (syscall 24)
#include "header/libstring.h"  // memcpy/memcmp (blit fast path v10.10)
#include "header/libaudio.h"    // audio_tone_on/off (syscall 25)
#include "header/usermode.h"    // user3_terminate + region user (v10.7)
#include "net/net.h"             // v10.12 Phase C: net_get_info / net_ping_raw
#include <stdint.h>
#include <stddef.h>

// ============================================================
//  1. STUB ASSEMBLY — entry gate 0x80
// ------------------------------------------------------------
//  Stack layout after `pushal`, as seen by syscall_dispatch
//  (ESP at the moment of the `call`):
//     regs[0] = EDI   regs[4] = EBX    regs[8]  = EIP
//     regs[1] = ESI   regs[5] = EDX    regs[9]  = CS
//     regs[2] = EBP   regs[6] = ECX    regs[10] = EFLAGS
//     regs[3] = original ESP (this slot is skipped by `popal`)
//     regs[7] = EAX   <- syscall number in, return value out
//
//  `popal` reloads all registers from the slots; since dispatch
//  overwrites regs[7], the EAX value carried by `popal` is the RETURN VALUE.
//  `iretd` then pops the EIP/CS/EFLAGS the CPU pushed on `int $0x80`.
//
//  Build note: the `call syscall_dispatch` instruction uses a
//  non-PIC symbol — the Equinox OS makefile indeed builds non-PIC.
// ============================================================
asm(
    ".global isr_128\n"
    "isr_128:\n"
    "    pushal\n"                    // save all GPRs (standard GAS order)
    "    pushl %esp\n"                // cdecl ARGUMENT: &regs[0] on the STACK
                                      // (BUGFIX: the pointer used to be passed
                                      //  via EAX, while cdecl syscall_dispatch
                                      //  reads it from the stack -> regs =
                                      //  garbage -> every real int 0x80 failed
                                      //  with "unknown number". The host-only
                                      //  sctest never caught it.)
    "    call syscall_dispatch\n"     // C dispatch (overwrites the EAX slot)
    "    addl $4, %esp\n"             // clean up the argument (cdecl: caller)
    "    popal\n"                     // restore GPRs (EAX = return value)
    "    iretl\n"                     // return to the caller (AT&T: iretl)
);

// ============================================================
//  2. FILE DESCRIPTOR TABLE (RAMFS)
// ------------------------------------------------------------
//  fd 0/1/2 = console (stdin/stdout/stderr) — node == NULL.
//  fd 3..SYS_MAX_FDS-1 = read-only RAMFS file + read position.
//  A single global table (no per-process one yet) — good enough for
//  single-tasking; move to per-process when multitasking arrives.
// ============================================================
#define SYS_MAX_FDS 16

struct sys_fd_entry {
    struct fs_node* node;   // NULL = free slot / console
    uint32_t pos;           // next read position (sequential)
};

static struct sys_fd_entry fd_table[SYS_MAX_FDS];

// ============================================================
//  3a. PROCESS CWD — set by the shell before running a .mrp tool.
//  Relative paths in open()/exec()/mkfile() resolve against this
//  directory, so `mtcc main.c` works from whatever directory the
//  user is currently in (not hard-wired to root like before).
// ============================================================
static struct fs_node* proc_cwd = NULL;

void syscall_set_cwd(struct fs_node* cwd) {
    proc_cwd = (cwd && cwd->is_dir) ? cwd : fs_get_root();
}

struct fs_node* syscall_get_cwd(void) {
    return proc_cwd ? proc_cwd : fs_get_root();
}

// ============================================================
//  3. HELPER — resolve a path to a RAMFS node
// ------------------------------------------------------------
//  "/a/b"      = absolute (from root)
//  "a/b"       = relative to proc_cwd (the shell's cwd)
//  "." / ".."  = navigation components (need a parent pointer)
//  Path components are limited to 63 chars — same as fs_node::name[64].
// ============================================================
static struct fs_node* syscall_resolve(const char* path) {
    if (!path || !path[0]) return NULL;
    struct fs_node* root = fs_get_root();
    if (!root) return NULL;

    struct fs_node* cur = (path[0] == '/') ? root
                        : (proc_cwd ? proc_cwd : root);

    // Walk component by component.
    char part[64];
    uint32_t idx = 0;
    const char* p = path;
    if (*p == '/') p++;   // drop the absolute leading '/'

    while (1) {
        if (*p == '/' || *p == '\0') {
            part[idx] = '\0';
            if (idx > 0) {
                if (part[0] == '.' && part[1] == '\0') {
                    // "." — stay
                } else if (part[0] == '.' && part[1] == '.' && part[2] == '\0') {
                    if (cur->parent) cur = cur->parent;
                } else {
                    struct fs_node* child = fs_find_child(cur, part);
                    if (!child) return NULL;
                    cur = child;
                }
            }
            idx = 0;
            if (*p == '\0') break;
        } else {
            if (idx >= sizeof(part) - 1) return NULL;   // component > 63
            part[idx++] = *p;
        }
        p++;
    }
    return cur;
}

// ============================================================
//  3b. UACCESS — validate pointers from ring 3 programs (v10.7)
// ------------------------------------------------------------
//  User programs are now truly isolated (U/S pages), BUT syscalls
//  must stay defensive: a wild pointer from CPL 3 that slips into
//  the kernel executes at CPL 0 — supervisor can write supervisor
//  pages! Without this guard, `read(fd, 0x300000, 100)` from a
//  malicious program = kernel heap corruption.
//  Policy: a pointer MUST point into a user region (MRP arena /
//  trampoline / user stack). CPL 0 callers (legacy shell path) pass
//  unchecked — the kernel's internal ABI is trusted.
// ============================================================
static int g_syscall_from_user = 0;

static inline int syscall_from_user(void) {
    return g_syscall_from_user;
}

/* Is a single address inside one of the user regions? */
static inline int addr_in_user(uint32_t a) {
    if (a >= USER_ARENA_START && a < USER_GUARD_START) return 1;
    if (a >= USER_STACK_START && a < USER_STACK_END)   return 1;
    return 0;
}

/* Is the entire range [p, p+len) inside a user region (len 0 = ok).
 * v10.9: cap raised 4 MB -> 16 MB (the arena is now 33 MB; DOOM reads
 * large WAD lumps via readfile) + an explicit wrap guard so a
 * p+len that overflows 32 bits cannot pass as a valid range. */
static int user_range_ok(uint32_t p, uint32_t len) {
    if (len == 0) return addr_in_user(p) || p == 0;  /* len 0: a sane address suffices */
    if (p > 0xFFFFF000u || len > 0x1000000u) return 0; /* coarse anti-overflow (16 MB) */
    uint32_t end = p + len - 1;
    if (end < p) return 0;                            /* 32-bit wrap -> reject */
    int ok1 = (p >= USER_ARENA_START && end < USER_GUARD_START);
    int ok2 = (p >= USER_STACK_START && end < USER_STACK_END);
    return ok1 || ok2;
}

/* NUL-terminated string: starts in a user region + NUL found
 * before leaving the region (256 byte limit — enough for paths/text).
 * Dereferencing is safe because each address is checked BEFORE it is read. */
static int user_str_ok(const char* s) {
    if (!s) return 0;
    uint32_t p = (uint32_t)(uintptr_t)s;
    if (!addr_in_user(p)) return 0;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t a = p + i;
        if (!addr_in_user(a)) return 0;   /* ran to the end of the region */
        if (*(const volatile char*)a == '\0') return 1;
    }
    return 0;                              /* too long */
}

// ============================================================
//  4. SYSCALL HANDLERS
//  All handlers share one uniform signature:
//      uint32_t fn(uint32_t a1, uint32_t a2, uint32_t a3)
//  (a1=EBX, a2=ECX, a3=EDX) so they can all live in one table.
// ============================================================

// ---- 1: exit(status) ----
static uint32_t sys_exit(uint32_t status, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    /* Single-tasking: no process table yet — exit() returns the
     * status to the caller (mrp_run / sctest / shell). Once multitasking
     * is active, this is the point that makes the scheduler switch tasks. */
    return status;
}

// ---- 2: exec(path) — run a .mrp from RAMFS ----
static uint32_t sys_exec(uint32_t path_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    const char* path = (const char*)(uintptr_t)path_;
    if (syscall_from_user() && !user_str_ok(path)) return (uint32_t)SYS_EFAULT;
    struct fs_node* node = syscall_resolve(path);
    if (!node)        return (uint32_t)SYS_ENOENT;
    if (node->is_dir) return (uint32_t)SYS_EISDIR;
    if (!node->parent) return (uint32_t)SYS_EINVAL;   // path == "/" (root)

    /* mrp_run prints its own progress ("mrp: running ...").
     * mrp_run's return code is mapped to a syscall errno.
     * FIX(audit V3 #1): exec from INSIDE a still-running .mrp program
     * (nested) is rejected by mrp_run with MRP_RUN_ERR_BUSY -> SYS_EBUSY (-9).
     * Before: the MRP arena was reset -> the caller's code got overwritten
     * -> a deterministic #GP/#UD panic every time a nested exec was called. */
    int r = mrp_run(node->parent, node->name);
    switch (r) {
        case MRP_RUN_OK:            return 0;
        case MRP_RUN_ERR_NOT_FOUND: return (uint32_t)SYS_ENOENT;
        case MRP_RUN_ERR_IS_DIR:    return (uint32_t)SYS_EISDIR;
        case MRP_RUN_ERR_NO_MEMORY: return (uint32_t)SYS_ENOMEM;
        case MRP_RUN_ERR_BUSY:      return (uint32_t)SYS_EBUSY;   /* FIX(audit V3 #1) */
        default:                    return (uint32_t)SYS_EINVAL;
    }
}

// ---- 3: getpid() ----
static uint32_t sys_getpid(uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a1; (void)a2; (void)a3;
    /* Single-tasking: the only "process" is the kernel task.
     * Number 1 is consistent with init/PID 1 on UNIX-like systems. */
    return 1;
}

// ---- 4: write(fd, buf, len) ----
static uint32_t sys_write(uint32_t fd, uint32_t buf_, uint32_t len) {
    const char* buf = (const char*)(uintptr_t)buf_;
    if (!buf) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user() && !user_range_ok((uint32_t)(uintptr_t)buf, len))
        return (uint32_t)SYS_EFAULT;

    if (fd == SYS_FD_STDOUT || fd == SYS_FD_STDERR) {
        /* Console: print exactly len bytes (binary-safe). */
        uint32_t n = 0;
        for (; n < len; n++) put_char(buf[n]);
        return n;
    }
    if (fd == SYS_FD_STDIN) return (uint32_t)SYS_EBADF;   // write to stdin

    if (fd >= SYS_MAX_FDS || !fd_table[fd].node)
        return (uint32_t)SYS_EBADF;

    /* Writing to RAMFS files is not supported yet (needs an fs append API — v2). */
    return (uint32_t)SYS_ENOTSUP;
}

// ---- 5: read(fd, buf, len) — file RAMFS sequential ----
static uint32_t sys_read(uint32_t fd, uint32_t buf_, uint32_t len) {
    char* buf = (char*)(uintptr_t)buf_;
    if (!buf) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user() && !user_range_ok((uint32_t)(uintptr_t)buf, len))
        return (uint32_t)SYS_EFAULT;
    if (fd <= SYS_FD_STDERR) return (uint32_t)SYS_ENOTSUP;  // console: use READLINE/GETKEY

    if (fd >= SYS_MAX_FDS || !fd_table[fd].node)
        return (uint32_t)SYS_EBADF;

    struct fs_node* f = fd_table[fd].node;
    if (f->is_dir)  return (uint32_t)SYS_EISDIR;
    if (!f->content) return 0;              // empty file = EOF

    uint32_t pos = fd_table[fd].pos;
    if (pos >= f->size) return 0;           // EOF

    uint32_t n = f->size - pos;             // bytes remaining
    if (n > len) n = len;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = f->content[pos + i];
    }
    fd_table[fd].pos = pos + n;
    return n;
}

// ---- 6: open(path) — open a RAMFS file read-only ----
static uint32_t sys_open(uint32_t path_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    const char* path = (const char*)(uintptr_t)path_;
    if (syscall_from_user() && !user_str_ok(path)) return (uint32_t)SYS_EFAULT;
    struct fs_node* node = syscall_resolve(path);
    if (!node)        return (uint32_t)SYS_ENOENT;
    if (node->is_dir) return (uint32_t)SYS_EISDIR;

    /* Find a free fd slot (3..MAX-1). */
    for (uint32_t fd = 3; fd < SYS_MAX_FDS; fd++) {
        if (!fd_table[fd].node) {
            fd_table[fd].node = node;
            fd_table[fd].pos  = 0;
            return fd;
        }
    }
    return (uint32_t)SYS_EMFILE;   // table full
}

// ---- 7: close(fd) ----
static uint32_t sys_close(uint32_t fd, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    if (fd < 3 || fd >= SYS_MAX_FDS) return (uint32_t)SYS_EBADF;
    if (!fd_table[fd].node)          return (uint32_t)SYS_EBADF;
    fd_table[fd].node = nullptr;
    fd_table[fd].pos  = 0;
    return 0;
}

// ---- 8: getkey() — keyboard non-blocking ----
static uint32_t sys_getkey(uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a1; (void)a2; (void)a3;
    int k = getkey();               // -1 when the buffer is empty
    return (uint32_t)k;
}

// ---- 9: readline(buf, maxlen) ----
static uint32_t sys_readline(uint32_t buf_, uint32_t maxlen, uint32_t a3) {
    (void)a3;
    char* buf = (char*)(uintptr_t)buf_;
    if (!buf)                       return (uint32_t)SYS_EINVAL;
    if (maxlen == 0 || maxlen > 1024) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user() && !user_range_ok((uint32_t)(uintptr_t)buf, maxlen))
        return (uint32_t)SYS_EFAULT;

    gets(buf, (int)maxlen);
    uint32_t n = 0;
    while (buf[n] != '\0' && n < maxlen) n++;
    return n;
}

// ---- 10: print(str) — console fast-path ----
static uint32_t sys_print(uint32_t str_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    const char* s = (const char*)(uintptr_t)str_;
    if (!s) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user() && !user_str_ok(s)) return (uint32_t)SYS_EFAULT;
    print_string(s);
    return 0;
}

// ---- 11: printint(num) ----
static uint32_t sys_printint(uint32_t num, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    print_int(num);
    return 0;
}

// ---- 12: malloc(size) — from the MRP arena (not the kernel heap) ----
static uint32_t sys_malloc(uint32_t size, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    void* p = mrp_alloc(size);
    return (uint32_t)(uintptr_t)p;  // 0 = allocation failed (arena full)
}

// ---- 13: gettick() ----
static uint32_t sys_gettick(uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a1; (void)a2; (void)a3;
    return get_tick();
}

// ---- 14: sleep(ms) ----
static uint32_t sys_sleep(uint32_t ms, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    sleep_ms(ms);
    return 0;
}

// ---- 15: getargs(buf, maxlen) ----
// The last arguments stored by the shell via syscall_set_args()
// before mrp_run(). Used by programs such as tcc.mrp to know which
// file to compile (`run tcc.mrp hello.c`).
static char g_run_args[256];

void syscall_set_args(const char* args) {
    if (!args) args = "";
    uint32_t i = 0;
    for (; i < sizeof(g_run_args) - 1 && args[i]; i++) {
        g_run_args[i] = args[i];
    }
    g_run_args[i] = '\0';
}

static uint32_t sys_getargs(uint32_t buf_, uint32_t maxlen, uint32_t a3) {
    (void)a3;
    char* buf = (char*)(uintptr_t)buf_;
    if (!buf || maxlen == 0) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user() && !user_range_ok((uint32_t)(uintptr_t)buf, maxlen))
        return (uint32_t)SYS_EFAULT;
    uint32_t n = 0;
    while (g_run_args[n] != '\0' && n + 1 < maxlen) {
        buf[n] = g_run_args[n];
        n++;
    }
    buf[n] = '\0';
    return n;   // argument string length (without NUL)
}

// ---- 16: mkfile(path, buf, len) ----
// Byte-safe RAMFS file write (fs_write_binary): create if absent,
// fully OVERWRITE if present. This is the mtcc compiler output path
// (`run tcc.mrp -c hello.c` -> hello.mrp). The path may be "name",
// "/name", or "dir/name" (dir must be an existing directory).
static uint32_t sys_mkfile(uint32_t path_, uint32_t buf_, uint32_t len) {
    const char* path = (const char*)(uintptr_t)path_;
    const uint8_t* buf = (const uint8_t*)(uintptr_t)buf_;
    if (!path || !path[0] || !buf || len == 0) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user()
        && (!user_str_ok(path) || !user_range_ok((uint32_t)(uintptr_t)buf, len)))
        return (uint32_t)SYS_EFAULT;

    int is_abs = (path[0] == '/');

    // Find the last '/' -> split directory + file name.
    // Relative directories are resolved via syscall_resolve() (proc_cwd),
    // so `mtcc -c test/hello.c` writes to <cwd>/test/hello.mrp
    // and `mtcc -c hello.c` (after `cd test`) writes to test/hello.mrp.
    const char* last_slash = NULL;
    for (const char* q = path; *q; q++) {
        if (*q == '/') last_slash = q;
    }

    struct fs_node* parent;
    const char* name;
    if (!last_slash) {
        // plain name -> write in the cwd (a plain absolute "name" is
        // impossible, because "name" without '/' is always relative)
        parent = is_abs ? fs_get_root()
                        : (proc_cwd ? proc_cwd : fs_get_root());
        name = path;
    } else {
        uint32_t m = (uint32_t)(last_slash - path);
        if (m == 0) {
            parent = fs_get_root();          // "/name" — file in the root
        } else {
            char dirpart[256];
            if (m >= sizeof(dirpart)) return (uint32_t)SYS_EINVAL;
            for (uint32_t i = 0; i < m; i++) dirpart[i] = path[i];
            dirpart[m] = '\0';
            parent = syscall_resolve(dirpart);
        }
        if (!parent || !parent->is_dir) return (uint32_t)SYS_ENOENT;
        name = last_slash + 1;
    }
    if (!name[0]) return (uint32_t)SYS_EINVAL;

    int r = fs_write_binary(parent, name, buf, len);
    switch (r) {
        case 0:  return 0;                          // success
        case -3: return (uint32_t)SYS_ENOMEM;       // malloc failed
        case -6: return (uint32_t)SYS_EINVAL;       // name taken by a directory
        case -7: return (uint32_t)SYS_EINVAL;       // name > 63 chars
        default: return (uint32_t)SYS_EINVAL;
    }
}

// ---- 17: readfile(path, buf, maxlen) — read a WHOLE file in one call ----
// Server-side open + read-all + close. This is the read counterpart of
// mkfile(): programs (and the Morph.h / mtcc `file_read_all` builtin) get
// the whole file with a single int 0x80 instead of an open/read/close
// loop in userland. Returns the number of bytes copied (0 = empty file,
// short read = maxlen too small — compare with filesize() to detect).
static uint32_t sys_readfile(uint32_t path_, uint32_t buf_, uint32_t maxlen) {
    const char* path = (const char*)(uintptr_t)path_;
    char* buf = (char*)(uintptr_t)buf_;
    if (!path || !path[0] || !buf || maxlen == 0)
        return (uint32_t)SYS_EINVAL;
    if (syscall_from_user()
        && (!user_str_ok(path) || !user_range_ok((uint32_t)(uintptr_t)buf, maxlen)))
        return (uint32_t)SYS_EFAULT;

    struct fs_node* node = syscall_resolve(path);
    if (!node)        return (uint32_t)SYS_ENOENT;
    if (node->is_dir) return (uint32_t)SYS_EISDIR;
    if (!node->content || node->size == 0) return 0;   // empty file

    uint32_t n = (node->size < maxlen) ? node->size : maxlen;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = node->content[i];
    }
    return n;
}

// ---- 18: filesize(path) — size of a RAMFS file in bytes ----
// Needed BEFORE file_read_all() to size the destination buffer exactly
// (readfile signals truncation only by returning a short count).
static uint32_t sys_filesize(uint32_t path_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    const char* path = (const char*)(uintptr_t)path_;
    if (!path || !path[0]) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user() && !user_str_ok(path)) return (uint32_t)SYS_EFAULT;

    struct fs_node* node = syscall_resolve(path);
    if (!node)        return (uint32_t)SYS_ENOENT;
    if (node->is_dir) return (uint32_t)SYS_EISDIR;
    return node->size;
}

// ---- 19: fileexists(path) — 1 if a regular file exists, 0 otherwise ----
// Cheap resolve-only probe: no fd table slot is consumed (unlike the
// open()+close() trick), and directories do NOT count as files.
static uint32_t sys_fileexists(uint32_t path_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    const char* path = (const char*)(uintptr_t)path_;
    if (!path || !path[0]) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user() && !user_str_ok(path)) return (uint32_t)SYS_EFAULT;

    struct fs_node* node = syscall_resolve(path);
    return (node && !node->is_dir) ? 1u : 0u;
}

/* ---- GAME API (syscalls 20-25) ------------------------------------------
 *
 * Purpose: expose the VESA framebuffer, non-blocking keyboard, mouse
 * and PC speaker to userland programs (mtcc-built or hosted Morph.h)
 * so games can be written BEFORE ring 3 / paging exists. All handlers
 * are deliberately thin wrappers over existing kernel drivers — no new
 * driver state lives in this file (the mouse accumulator is in
 * ps2_mouse.cpp, the tone primitives in libaudio.cpp).
 *
 * Ring-0 note: for now userland draws by asking the kernel per pixel /
 * rect (syscalls below) OR by writing to fbinfo.addr directly — both are
 * equally trusted today. When ring 3 + paging arrive, per-pixel
 * syscalls remain valid (kernel-side copy) while direct writes will
 * need an mmap-style mapping. The ABI is designed to survive that. */

// ---- 20: fbinfo(info*) — query the linear framebuffer ----
// Fills a morph_fbinfo_t (see syscall.h) with address/width/height/
// bpp/pitch and an availability flag. Returns 0 for a valid pointer
// (even when graphics are unavailable — check info->avail in that case,
// all fields are zeroed), SYS_EINVAL for NULL. A game should call this
// FIRST and bail out gracefully on avail == 0 (VGA text mode).
static uint32_t sys_fbinfo(uint32_t info_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    morph_fbinfo_t* info = (morph_fbinfo_t*)(uintptr_t)info_;
    if (!info) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user()
        && !user_range_ok((uint32_t)(uintptr_t)info, sizeof(morph_fbinfo_t)))
        return (uint32_t)SYS_EFAULT;

    if (!vesa_is_available() || !vesa_get_framebuffer()) {
        info->addr = 0; info->width = 0; info->height = 0;
        info->bpp = 0;  info->pitch = 0; info->avail = 0;
        return 0;                       // struct valid, mode text: avail=0
    }
    info->addr   = vesa_get_framebuffer();
    info->width  = vesa_get_width();
    info->height = vesa_get_height();
    info->bpp    = vesa_get_bpp();
    info->pitch  = vesa_get_pitch();
    info->avail  = 1;
    return 0;
}

// ---- 21: putpixel(x, y, color) — single pixel ----
// Color is a raw 32-bit value written as-is into the framebuffer
// (0x00RRGGBB in the standard 32bpp mode). Out-of-range coordinates
// are clipped silently by vesa_draw_pixel.
static uint32_t sys_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!vesa_is_available()) return (uint32_t)SYS_ENOTSUP;
    vesa_draw_pixel((uint16_t)x, (uint16_t)y, color);
    return 0;
}

// ---- 22: fillrect(x|w<<16, y|h<<16, color) — solid rectangle ----
// The syscall ABI passes only 3 argument registers, but a rectangle
// needs 5 values — so x/w and y/h are each packed into one u32
// (low 16 bits = position, high 16 bits = size). Morph.h's fill_rect()
// wrapper does the packing; mtcc users pack inline:
//     fill_rect(x | (w << 16), y | (h << 16), color);
static uint32_t sys_fillrect(uint32_t xw, uint32_t yh, uint32_t color) {
    if (!vesa_is_available()) return (uint32_t)SYS_ENOTSUP;
    vesa_fill_rect((uint16_t)(xw & 0xFFFF), (uint16_t)(yh & 0xFFFF),
                   (uint16_t)(xw >> 16),    (uint16_t)(yh >> 16),
                   color);
    return 0;
}

// ---- 23: pollkey() — NON-BLOCKING keyboard for game loops ----
// getkey() (#8) busy-waits for the next keypress, which would freeze
// animation. pollkey() drains at most one key event per call:
//     0  = nothing waiting this frame
//     >0 = ASCII code of the pressed key
//     <0 = special key, same codes as getkey() (-1 Up ... -9 Del)
// 0 as the "empty" marker is what keeps arrows (-1..-9) distinguishable.
static uint32_t sys_pollkey(uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a1; (void)a2; (void)a3;
    return (uint32_t)getkey_poll();
}

// ---- 24: mouse(state*) — absolute cursor + buttons ----
// Fills a morph_mouse_t (see syscall.h). The kernel folds relative PS/2
// deltas into an absolute position clamped to the screen; before any
// movement the reported position is the screen center.
static uint32_t sys_mouse(uint32_t state_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    morph_mouse_t* st = (morph_mouse_t*)(uintptr_t)state_;
    if (!st) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user()
        && !user_range_ok((uint32_t)(uintptr_t)st, sizeof(morph_mouse_t)))
        return (uint32_t)SYS_EFAULT;

    int32_t x, y;
    uint8_t buttons;
    mouse_get_state(&x, &y, &buttons);
    st->x = x;
    st->y = y;
    st->buttons = buttons;
    return 0;
}

// ---- 25: speaker(freq) — non-blocking PC-speaker tone ----
// freq > 0 starts a continuous tone at that frequency; freq == 0
// silences it. The caller controls the duration (game sound effects:
// tone on, animate a few frames, tone off). NOTE (v10.5): this is the
// MANUAL override API — audio_tone_on/off flush the note queue first,
// so a lingering queued melody can never cut a manual tone short.
static uint32_t sys_speaker(uint32_t freq, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    if (freq == 0) audio_tone_off();
    else           audio_tone_on(freq);
    return 0;
}

// ---- 26: sndbeep(freq, ms) — queue a TIMED PC-speaker note ----
// v10.5 note-queue API (kernel ring buffer, drained by the 100 Hz
// timer IRQ): the call stores {freq, ms} and returns IMMEDIATELY —
// melodies and game jingles no longer need sleep loops, the shell
// keeps running while the music plays. freq=0 queues a rest. The
// queue holds 64 pending notes; pushing more returns SYS_EBUSY and
// the note is NOT queued. First note starts on the next timer tick
// (worst case 10 ms). ms==0 is rejected with EINVAL.
static uint32_t sys_sndbeep(uint32_t freq, uint32_t ms, uint32_t a3) {
    (void)a3;
    if (ms == 0) return (uint32_t)SYS_EINVAL;
    return (audio_queue_tone(freq, ms) == 0) ? 0 : (uint32_t)SYS_EBUSY;
}

// ---- 27: lseek(fd, offset, whence) — reposition a RAMFS file fd ----
// v10.8 libc: the foundation of user-side fseek()/ftell()/lseek(). fd_table
// already stores a per-fd read position — this syscall merely moves it.
// whence: SYS_SEEK_SET (0) = from the start, SYS_SEEK_CUR (1) = relative
// to the current position, SYS_SEEK_END (2) = from the end (offset <= 0).
// The resulting position is always clamped to [0, size] (seeking past EOF
// is allowed like on Linux — a read there simply returns EOF).
// Console fds (0-2) and empty fds = EBADF.
static uint32_t sys_lseek(uint32_t fd, uint32_t off, uint32_t whence) {
    if (fd >= SYS_MAX_FDS || !fd_table[fd].node)
        return (uint32_t)SYS_EBADF;

    struct fs_node* f = fd_table[fd].node;
    if (f->is_dir)  return (uint32_t)SYS_EISDIR;
    uint32_t size = f->size;

    // offset and whence arrive as uint32 ABI values — whence is validated
    // by value (0/1/2); offset is interpreted as SIGNED (int32_t) so that
    // negative SEEK_CUR and negative SEEK_END work naturally.
    int32_t so = (int32_t)off;
    if (whence != SYS_SEEK_SET && whence != SYS_SEEK_CUR &&
        whence != SYS_SEEK_END)
        return (uint32_t)SYS_EINVAL;

    int64_t target;
    if (whence == SYS_SEEK_SET)      target = (int64_t)so;
    else if (whence == SYS_SEEK_CUR) target = (int64_t)fd_table[fd].pos + (int64_t)so;
    else                             target = (int64_t)size + (int64_t)so;

    if (target < 0) target = 0;                       // lower clamp
    if (target > (int64_t)size) target = (int64_t)size; // upper clamp
    fd_table[fd].pos = (uint32_t)target;
    return fd_table[fd].pos;
}

// ---- 28: printf(fmt, int args[3]) — kernel printf for user programs ----
// v10.8 libc. User programs have no varargs (neither does mtcc),
// so the user-side wrapper collects up to 3 arguments into an int
// array and then calls this syscall; the kernel renders the format
// with its own printf engine (width, zero-pad, left-align, %d %u %x %X
// %s %c %% — equivalent to the kernel console printf).
//
// A conversion spec that EXCEEDS the available args is rendered as
// "(n/a)" — not a crash, easy to spot while debugging.
// %s from user stays safe: each pointer is first validated with
// user_str_ok (string outside the user regions -> "(badptr)"), because
// the renderer runs at CPL 0 and a wild pointer would fault the KERNEL.
static uint32_t sys_printf(uint32_t fmt_, uint32_t args_, uint32_t a3) {
    (void)a3;
    const char* fmt = (const char*)(uintptr_t)fmt_;
    int* args = (int*)(uintptr_t)args_;
    if (!fmt || !fmt[0]) return (uint32_t)SYS_EINVAL;

    // Copy fmt into a kernel buffer: the renderer must not hold a user
    // pointer while scanning (a fault mid-render = kernel panic).
    char kfmt[192];
    uint32_t n = 0;
    if (syscall_from_user() && !user_str_ok(fmt))
        return (uint32_t)SYS_EFAULT;
    while (fmt[n] && n < sizeof(kfmt) - 1) { kfmt[n] = fmt[n]; n++; }
    kfmt[n] = '\0';

    int kargs[3] = { 0, 0, 0 };
    if (args) {
        if (syscall_from_user() &&
            !user_range_ok((uint32_t)(uintptr_t)args, sizeof(kargs)))
            return (uint32_t)SYS_EFAULT;
        for (int i = 0; i < 3; i++) kargs[i] = args[i];
    }

    // Manual render (not the kernel's va_arg printf — the arguments are
    // already in an array). Supports: %[0][width]d/u/x/X/c/s and %%.
    uint32_t out = 0;
    int argn = 0;
    for (const char* p = kfmt; *p; p++) {
        if (*p != '%') { put_char(*p); out++; continue; }
        p++;
        if (*p == '%') { put_char('%'); out++; continue; }
        if (*p == '\0') break;

        int zero_pad = 0, width = 0, left_align = 0;
        if (*p == '-') { left_align = 1; p++; }
        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        char spec = *p;
        if (spec != 'd' && spec != 'u' && spec != 'x' && spec != 'X' &&
            spec != 'c' && spec != 's') {
            put_char('%'); put_char(spec ? spec : '?'); out += 2;
            continue;
        }
        if (argn >= 3) {                    // out of argument slots
            const char* na = "(n/a)";
            while (*na) { put_char(*na); na++; out++; }
            argn++;
            continue;
        }
        int v = kargs[argn++];

        if (spec == 's') {
            const char* s = (const char*)(uintptr_t)(uint32_t)v;
            if (!s) s = "(null)";
            else if (syscall_from_user() && !user_str_ok(s)) s = "(badptr)";
            uint32_t len = 0;
            while (s[len]) len++;
            if (!left_align) {
                for (uint32_t i = len; i < (uint32_t)width; i++) { put_char(' '); out++; }
            }
            for (uint32_t i = 0; i < len; i++) { put_char(s[i]); out++; }
            if (left_align) {
                for (uint32_t i = len; i < (uint32_t)width; i++) { put_char(' '); out++; }
            }
            continue;
        }

        if (spec == 'c') {
            char c = (char)v;
            if (!left_align) { for (uint32_t i = 1; i < (uint32_t)width; i++) { put_char(' '); out++; } }
            put_char(c); out++;
            if (left_align)  { for (uint32_t i = 1; i < (uint32_t)width; i++) { put_char(' '); out++; } }
            continue;
        }

        // %d / %u / %x / %X — render the digits (reversed) then pad
        {
            char nb[16];
            uint32_t nl = 0;
            uint32_t base = (spec == 'x' || spec == 'X') ? 16u : 10u;
            uint32_t uv;
            int negv = 0;
            if (spec == 'd' && v < 0) { negv = 1; uv = (uint32_t)(-(int32_t)v); }
            else uv = (uint32_t)v;
            if (uv == 0) nb[nl++] = '0';
            while (uv) {
                uint32_t d = uv % base;
                nb[nl++] = (char)(d < 10 ? '0' + d
                                    : (spec == 'X' ? 'A' : 'a') + (d - 10));
                uv /= base;
            }
            if (negv) nb[nl++] = '-';
            char padc = (zero_pad && !negv && !left_align) ? '0' : ' ';
            if (!left_align) {
                for (uint32_t i = nl; i < (uint32_t)width; i++) { put_char(padc); out++; }
            }
            for (int32_t i = (int32_t)nl - 1; i >= 0; i--) { put_char(nb[i]); out++; }
            if (left_align) {
                for (uint32_t i = nl; i < (uint32_t)width; i++) { put_char(' '); out++; }
            }
        }
    }
    return out;
}

// ---- 29: ringinfo() — CPL of the syscall caller ----
// v10.8: the shell has a `ring` command; a user program can now prove
// for itself that it runs in ring 3: printf("ring %d\n", ring()).
// The value comes from the g_syscall_from_user flag filled by dispatch
// from the CS slot of the stack frame — 3 = user (CPL 3), 0 = kernel/shell.
static uint32_t sys_ringinfo(uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a1; (void)a2; (void)a3;
    return syscall_from_user() ? 3u : 0u;
}

/* ---- BLIT PATH v10.9 (syscalls 30-31) -------------------------------------
 *
 * Problem: the per-pixel syscall (putpixel #21) = one int 0x80 per pixel —
 * 320x200 = 64000 syscalls per frame. Under QEMU TCG that is ~1-2 s/frame;
 * a 35 fps game is impossible. DOOM needs ONE syscall per frame.
 *
 * Solution: two syscalls:
 *   set_palette(pal)  — upload 256 x RGB (768 bytes) once per palette
 *                       change (DOOM V_SetPalette). The kernel prepares
 *                       a uint32 LUT ready to write to the LFB (0x00RRGGBB).
 *   blit(src, w|h<<16, flags) — convert + scale the user 8bpp buffer onto
 *                       the whole 32bpp screen, nearest-neighbor. Fixed-point
 *                       stepping per pixel (no div in the inner loop).
 *
 * Ring 3 security: src is validated with user_range_ok (SYS_EFAULT if it
 * points at kernel memory); the palette is copied into a kernel buffer
 * first. The LFB stays supervisor — user programs NEVER touch the
 * graphics hardware directly; the kernel does the writing (locked mode,
 * no severe tearing since it is one sequential pass).
 *
 * Flag: SYS_BLIT_ASPECT43 = centered 4:3 letterbox (DOOM's original ratio
 * on a 4:3 monitor — 320x200 pixels stretched 1.2x vertically),
 * otherwise a full 16:9 stretch. */

static uint32_t blit_pal[256];   /* 8bpp -> 0x00RRGGBB LUT (LFB-ready) */

/* v10.10 (Phase C) — kernel-side blit cache.
 * ----------------------------------------------------------------
 * v10.9 baseline profile (QEMU TCG, E1M1, holding W): ~5.7 fps, and
 * the only heavy work that CAN be removed from the render path
 * is SYS_BLIT itself:
 *
 *   old: (a) 4x vesa_fill_rect letterbox EVERY frame (~1 MB of wasted
 *              writes — the black bars never change),
 *         (b) an inner loop per DESTINATION pixel: fixed-point + LUT
 *              lookup for all 786,432 pixels (dw*dh) even though the
 *              same source row is scaled ~3.84x repeatedly in the
 *              vertical direction,
 *         (c) zero reuse across frames — a static screen (menu,
 *             title, pause) is still blitted in full.
 *
 *   new: (1) the scale tables sx[dx]/sy[dy] are built ONCE per mode
 *              (w,h,flags,framebuffer) — no per-pixel fixed-point,
 *         (2) the letterbox is blacked out ONCE per mode change,
 *         (3) row-diff: a 64 KB shadow stores the last source
 *             frame; identical rows are skipped entirely (menu/title
 *             become practically free),
 *         (4) a changed row is scaled ONCE into a temp buffer and
 *             then memcpy'd to every destination row that maps to
 *             that source row (a 4-byte aligned memcpy is far cheaper
 *             than per-pixel LUT+index).
 *
 * A palette change (SYS_SETPAL) invalidates the shadow -> one full
 * redraw (all colors change by definition).
 *
 * Fallback: a source buffer > BLIT_SHADOW_CAP or dimensions > 4096
 * take the direct scaled path without the cache (v10.9 behavior), so
 * the syscall ABI stays 100% compatible for other programs. */
#define BLIT_MAX_DIM   4096u
#define BLIT_SHADOW_CAP (512u * 1024u)   /* DOOM 320x200 = 64 KB */

static uint16_t blit_sx[BLIT_MAX_DIM];   /* src x per dst x */
static uint16_t blit_sy[BLIT_MAX_DIM];   /* src y per dst y */
static uint32_t blit_trow[BLIT_MAX_DIM]; /* one scaled row (temp) */
static uint8_t  blit_shadow[BLIT_SHADOW_CAP];

struct blit_mode_cache {
    uint32_t w, h, flags;                /* key + rebuild tables */
    uint32_t fb, fw, fh, pitch;
    uint32_t dx0, dy0, dw, dh;
    uint32_t row_start[BLIT_MAX_DIM];    /* per src row: first dst */
    uint32_t row_end[BLIT_MAX_DIM];      /* per src row: last dst+1 */
    int      valid;
};
static struct blit_mode_cache blit_mode;
static int blit_force_full = 1;          /* also set by sys_setpal */

// ---- 31: setpalette(pal*) — upload a 256-entry RGB palette ----
// pal = 768 bytes (r,g,b per entry, 0-255). Copied into the kernel
// LUT ONCE per palette change; the next blit uses it directly.
// A new palette = ALL colors change -> the shadow row-diff is
// invalidated so the next frame is a full redraw (v10.10).
static uint32_t sys_setpal(uint32_t pal_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    const uint8_t* pal = (const uint8_t*)(uintptr_t)pal_;
    if (!pal) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user() && !user_range_ok((uint32_t)(uintptr_t)pal, 768u))
        return (uint32_t)SYS_EFAULT;

    for (int i = 0; i < 256; i++) {
        uint32_t r = pal[i * 3 + 0];
        uint32_t g = pal[i * 3 + 1];
        uint32_t b = pal[i * 3 + 2];
        blit_pal[i] = (r << 16) | (g << 8) | b;   /* 0x00RRGGBB */
    }
    blit_force_full = 1;
    return 0;
}

// ---- 30: blit(src, w|h<<16, flags) — 8bpp -> LFB 32bpp scaled ----
// The v10.10 fast path (see the blit cache block comment above).
// The per-pixel fallback path (v10.9) remains available for large buffers.
static uint32_t sys_blit(uint32_t src_, uint32_t dims, uint32_t flags) {
    const uint8_t* src = (const uint8_t*)(uintptr_t)src_;
    uint32_t w = dims & 0xFFFFu;
    uint32_t h = dims >> 16;

    if (!src) return (uint32_t)SYS_EINVAL;
    if (w == 0 || h == 0 || w > BLIT_MAX_DIM || h > BLIT_MAX_DIM)
        return (uint32_t)SYS_EINVAL;
    uint32_t npix = w * h;
    if (npix > 0x400000u) return (uint32_t)SYS_EINVAL;   /* > 4 MP: reject */
    if (syscall_from_user() && !user_range_ok((uint32_t)(uintptr_t)src, npix))
        return (uint32_t)SYS_EFAULT;

    if (!vesa_is_available() || !vesa_get_framebuffer())
        return (uint32_t)SYS_ENOTSUP;
    if (vesa_get_bpp() != 32) return (uint32_t)SYS_ENOTSUP;

    uint32_t fb   = vesa_get_framebuffer();
    uint32_t fw   = vesa_get_width();
    uint32_t fh   = vesa_get_height();
    uint32_t pitch = vesa_get_pitch();

    /* Destination rectangle on screen */
    uint32_t dx0 = 0, dy0 = 0, dw = fw, dh = fh;
    if (flags & SYS_BLIT_ASPECT43) {
        /* Centered 4:3 letterbox: dw = min(fw, fh*4/3), dh = min(fh, fw*3/4) */
        dw = (fw < (fh * 4u / 3u)) ? fw : (fh * 4u / 3u);
        dh = (fh < (fw * 3u / 4u)) ? fh : (fw * 3u / 4u);
        dx0 = (fw - dw) / 2u;
        dy0 = (fh - dh) / 2u;
    }
    if (dw == 0 || dh == 0) return (uint32_t)SYS_EINVAL;
    if (dw > BLIT_MAX_DIM || dh > BLIT_MAX_DIM)
        return (uint32_t)SYS_EINVAL;

    /* ---- Rebuild the scale tables when the mode changes (or the first time).
     * Also black out the letterbox ONCE (not every frame). */
    if (!blit_mode.valid || blit_mode.w != w || blit_mode.h != h
        || blit_mode.flags != flags || blit_mode.fb != fb
        || blit_mode.fw != fw || blit_mode.fh != fh
        || blit_mode.pitch != pitch) {

        struct blit_mode_cache* m = &blit_mode;
        m->w = w; m->h = h; m->flags = flags;
        m->fb = fb; m->fw = fw; m->fh = fh; m->pitch = pitch;
        m->dx0 = dx0; m->dy0 = dy0; m->dw = dw; m->dh = dh;

        for (uint32_t dx = 0; dx < dw; dx++)
            blit_sx[dx] = (uint16_t)((dx * w) / dw);
        for (uint32_t dy = 0; dy < dh; dy++)
            blit_sy[dy] = (uint16_t)((dy * h) / dh);

        /* Per source row: the range of destination rows that map to it.
         * sy[] is monotonic non-decreasing -> one pass; a source row with
         * no destination row (downscale) gets an empty span. */
        for (uint32_t r = 0; r < h; r++) {
            m->row_start[r] = dh;        /* default: empty */
            m->row_end[r] = 0;
        }
        uint32_t last_r = 0xFFFFFFFFu;
        for (uint32_t dy = 0; dy < dh; dy++) {
            uint32_t r = blit_sy[dy];
            if (r != last_r) {
                if (last_r != 0xFFFFFFFFu) m->row_end[last_r] = dy;
                m->row_start[r] = dy;
                last_r = r;
            }
        }
        if (last_r != 0xFFFFFFFFu) m->row_end[last_r] = dh;

        /* Letterbox once per mode */
        if (dy0 > 0) vesa_fill_rect(0, 0, (uint16_t)fw, (uint16_t)dy0, 0);
        if (fh - dy0 - dh > 0)
            vesa_fill_rect(0, (uint16_t)(dy0 + dh), (uint16_t)fw,
                           (uint16_t)(fh - dy0 - dh), 0);
        if (dx0 > 0)
            vesa_fill_rect(0, (uint16_t)dy0, (uint16_t)dx0, (uint16_t)dh, 0);
        if (fw - dx0 - dw > 0)
            vesa_fill_rect((uint16_t)(dx0 + dw), (uint16_t)dy0,
                           (uint16_t)(fw - dx0 - dw), (uint16_t)dh, 0);

        blit_force_full = 1;            /* new mode: full redraw */
        m->valid = 1;
    }

    const struct blit_mode_cache* m = &blit_mode;

    /* ---- Fallback: buffer too large for the shadow -> direct scaling
     * (v10.9 behavior, no caching). */
    if (npix > BLIT_SHADOW_CAP) {
        for (uint32_t dy = 0; dy < dh; dy++) {
            const uint8_t* srow = src + (uint32_t)blit_sy[dy] * w;
            uint32_t* drow = (uint32_t*)(uintptr_t)(fb + (dy0 + dy) * pitch
                                                    + dx0 * 4u);
            for (uint32_t dx = 0; dx < dw; dx++) {
                drow[dx] = blit_pal[srow[blit_sx[dx]]];
            }
        }
        return 0;
    }

    /* ---- Fast path: row-diff + scaled-row temp + memcpy ---- */
    int full = blit_force_full;
    for (uint32_t r = 0; r < h; r++) {
        if (m->row_start[r] >= m->row_end[r]) continue;  /* not visible */

        const uint8_t* srow = src + r * w;
        uint8_t* shrow = blit_shadow + r * w;

        if (!full && memcmp(shrow, srow, w) == 0)
            continue;                          /* row unchanged */

        /* scale this row ONCE into temp */
        for (uint32_t dx = 0; dx < dw; dx++)
            blit_trow[dx] = blit_pal[srow[blit_sx[dx]]];

        /* copy to every destination row that maps to this source row */
        for (uint32_t dy = m->row_start[r]; dy < m->row_end[r]; dy++) {
            memcpy((void*)(uintptr_t)(fb + (dy0 + dy) * pitch + dx0 * 4u),
                   blit_trow, (size_t)dw * 4u);
        }

        memcpy(shrow, srow, w);                /* save for the next diff */
    }
    blit_force_full = 0;
    return 0;
}

// ---- 32: keyevent() — PRESS+RELEASE + RAW codes (the DOOM path) ----
// The getkey_event() decoder (stdio.cpp) reports press AND release events
// with physical key codes and no shift transformation — DOOM tracks held
// keys (hold an arrow = keep moving) and applies the shift
// transformation itself via modifier events (-10/-11/-12).
// Return: 0 = empty; otherwise bit16 = pressed, bit0-15 = code
// (cast to int16_t on the user side for negative special codes).
static uint32_t sys_keyevent(uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a1; (void)a2; (void)a3;
    int pressed = 0;
    int code = getkey_event(&pressed);
    if (code == 0) return 0;
    return ((pressed ? 1u : 0u) << 16) | ((uint32_t)code & 0xFFFFu);
}

// ---- 33: mousedelta(int32 dxdy[2]) — relative PS/2 delta + buttons ----
// The full-screen FPS game path (v10.10, Phase C — DOOM mouse-look).
// Unlike SYS_MOUSE #24 (absolute position clamped to the screen): here
// the raw delta since the last poll is NOT clamped — a view rotating
// past the screen edge must not lose counts.
//   EBX = user pointer to int32[2] (dx, dy) — overwritten with the accumulated result
//   return = live button mask (bit0 left, bit1 right, bit2 middle)
//            OR a negative errno if the pointer is invalid.
static uint32_t sys_mousedelta(uint32_t dxdy_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    int32_t* dxdy = (int32_t*)(uintptr_t)dxdy_;
    if (!dxdy) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user()
        && !user_range_ok((uint32_t)(uintptr_t)dxdy, 2 * sizeof(int32_t)))
        return (uint32_t)SYS_EFAULT;

    int32_t dx, dy;
    uint8_t buttons;
    mouse_get_delta(&dx, &dy, &buttons);
    dxdy[0] = dx;
    dxdy[1] = dy;
    return (uint32_t)buttons;
}

// ---- 34: netinfo(u32 w[10]) — network status for ring 3 programs ----
// The w[] layout exactly matches the syscall.h comment: 0=up 1=dhcp 2=ip 3=nm 4=gw
// (host order a.b.c.d) 5=mac_lo 6=mac_hi 7=rx 8=tx 9=drop.
static uint32_t sys_netinfo(uint32_t buf_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    uint32_t* w = (uint32_t*)(uintptr_t)buf_;
    if (!w) return (uint32_t)SYS_EINVAL;
    if (syscall_from_user()
        && !user_range_ok(buf_, 10 * sizeof(uint32_t)))
        return (uint32_t)SYS_EFAULT;
    net_get_info(w);
    return 0;
}

// ---- 35: netping(const char* ip) — 4x blocking ICMP echo ----
// Return: the number of replies (0..4). Without a NIC -> 0 (0 replies =
// failure, not an errno). Blocks ~5 s — the calling shell stalls briefly.
static uint32_t sys_netping(uint32_t ip_, uint32_t a2, uint32_t a3) {
    (void)a2; (void)a3;
    const char* ip = (const char*)(uintptr_t)ip_;
    if (!ip) return 0;
    if (syscall_from_user() && !user_str_ok(ip))
        return 0;
    if (!net_is_up()) return 0;
    return (uint32_t)net_ping_raw(ip, 4);
}

// ============================================================
//  5. DISPATCH TABLE — the single source of truth mapping number->handler.
//  Index 0 is intentionally NULL (number 0 = EAX not filled -> ENOSYS).
// ============================================================
typedef uint32_t (*sys_fn_t)(uint32_t a1, uint32_t a2, uint32_t a3);

static const sys_fn_t syscall_table[SYS_COUNT] = {
    NULL,             /*  0 — unused                       */
    sys_exit,         /*  1  exit(status)                  */
    sys_exec,         /*  2  exec(path)                    */
    sys_getpid,       /*  3  getpid()                      */
    sys_write,        /*  4  write(fd, buf, len)           */
    sys_read,         /*  5  read(fd, buf, len)            */
    sys_open,         /*  6  open(path)                    */
    sys_close,        /*  7  close(fd)                     */
    sys_getkey,       /*  8  getkey()                      */
    sys_readline,     /*  9  readline(buf, maxlen)         */
    sys_print,        /* 10  print(str)                    */
    sys_printint,     /* 11  printint(num)                 */
    sys_malloc,       /* 12  malloc(size)  [MRP arena]     */
    sys_gettick,      /* 13  gettick()                     */
    sys_sleep,        /* 14  sleep(ms)                     */
    sys_getargs,       /* 15  getargs(buf, maxlen)           */
    sys_mkfile,        /* 16  mkfile(path, buf, len)         */
    sys_readfile,      /* 17  readfile(path, buf, maxlen)    */
    sys_filesize,      /* 18  filesize(path)                 */
    sys_fileexists,    /* 19  fileexists(path)               */
    sys_fbinfo,        /* 20  fbinfo(info*)                  */
    sys_putpixel,      /* 21  putpixel(x, y, color)          */
    sys_fillrect,      /* 22  fillrect(x|w<<16, y|h<<16, c)  */
    sys_pollkey,       /* 23  pollkey() non-blocking         */
    sys_mouse,         /* 24  mouse(state*)                  */
    sys_speaker,       /* 25  speaker(freq) 0=off            */
    sys_sndbeep,       /* 26  sndbeep(freq, ms) queued       */
    sys_lseek,         /* 27  lseek(fd, off, whence)         */
    sys_printf,        /* 28  printf(fmt, int args[3])       */
    sys_ringinfo,      /* 29  ringinfo() -> CPL caller       */
    sys_blit,          /* 30  blit(src, w|h<<16, flags)      */
    sys_setpal,        /* 31  setpalette(pal*) 768 byte RGB  */
    sys_keyevent,      /* 32  keyevent() press/rel + RAW     */
    sys_mousedelta,    /* 33  mousedelta(int32 dxdy[2])      */
    sys_netinfo,       /* 34  netinfo(u32 w[10])             */
    sys_netping,       /* 35  netping(const char* ip)         */
};

static_assert(sizeof(syscall_table) / sizeof(syscall_table[0]) == SYS_COUNT,
              "syscall table must have exactly SYS_COUNT entries (see syscall.h)");

// ============================================================
//  6. DISPATCH — called by the assembly stub with a pointer to the register slots.
// ------------------------------------------------------------
//  v10.7: caller privilege detection from the CS slot (regs[9])
//  pushed by the CPU. A CPL 3 caller -> uaccess flag active + SYS_EXIT
//  does not return via iret, but diverts to user3_terminate
//  (longjmp into mrp_run) — the program ends, the shell continues.
// ============================================================
extern "C" void syscall_dispatch(uint32_t* regs) {
    if (!regs) return;    // impossible from the stub; defensive only

    uint32_t num = regs[7];               // EAX = syscall number
    g_syscall_from_user = ((regs[9] & 3u) == 3u);   // CS & 3 == 3 -> CPL 3

    if (num == 0 || num >= SYS_COUNT || syscall_table[num] == NULL) {
        /* Unknown number: print one line so it is visible while debugging
         * (e.g. an old .mrp program calling a number that does not exist yet). */
        printf("syscall: unknown number %u\n", num);
        regs[7] = (uint32_t)SYS_ENOSYS;
        g_syscall_from_user = 0;
        return;
    }

    uint32_t a1 = regs[4];                // EBX
    uint32_t a2 = regs[6];                // ECX
    uint32_t a3 = regs[5];                // EDX

    uint32_t ret = syscall_table[num](a1, a2, a3);
    regs[7] = ret;                        // return -> EAX slot

    /* SYS_EXIT from a ring 3 program = end of the program's life: divert
     * to the terminate path (noreturn). iret is not executed — the
     * interrupt stack is abandoned, TSS.ESP0 is ready for the next program. */
    if (g_syscall_from_user && num == SYS_EXIT) {
        g_syscall_from_user = 0;
        user3_terminate(1, ret, NULL);    // noreturn
    }
    g_syscall_from_user = 0;
}

// ============================================================
//  7. INIT + LISTING
// ============================================================
void syscall_init(void) {
    for (uint32_t fd = 0; fd < SYS_MAX_FDS; fd++) {
        fd_table[fd].node = nullptr;
        fd_table[fd].pos  = 0;
    }
}

void syscall_list(void) {
    printf("Equinox OS syscall interface (int 0x80):\n");
    printf("  [v10.7] caller ring 0 (shell) / ring 3 (program .mrp)\n");
    printf("  ABI: EAX=number, EBX/ECX/EDX=args, EAX=return (negative=error)\n");
    printf("  fd : 0=stdin 1=stdout 2=stderr, 3+ = RAMFS file (read-only)\n");
    printf("   1  exit(status)            end the program, status -> caller\n");
    printf("   2  exec(path)              run a .mrp from RAMFS (nested -> EBUSY)\n");
    printf("   3  getpid()                caller pid (single-task: 1)\n");
    printf("   4  write(fd, buf, len)     write to the console\n");
    printf("   5  read(fd, buf, len)      read a RAMFS file (0 = EOF)\n");
    printf("   6  open(path)              open a RAMFS file -> fd (3+)\n");
    printf("   7  close(fd)               close an fd\n");
    printf("   8  getkey()                non-blocking keyboard (-1 = empty)\n");
    printf("   9  readline(buf, maxlen)   read one line of input\n");
    printf("  10  print(str)              print a string to the console\n");
    printf("  11  printint(num)           print an unsigned number\n");
    printf("  12  malloc(size)            allocate from the MRP arena\n");
    printf("  13  gettick()               timer ticks since boot\n");
    printf("  14  sleep(ms)               delay execution\n");
    printf("  15  getargs(buf, maxlen)    the program's last `run` arguments\n");
    printf("  16  mkfile(path, buf, len)  write a RAMFS file (binary, overwrite)\n");
    printf("  17  readfile(path,buf,max)   read the whole file -> n bytes\n");
    printf("  18  filesize(path)           file size in bytes\n");
    printf("  19  fileexists(path)         1 = file exists, 0 = no\n");
    printf("  20  fbinfo(info*)            VESA framebuffer info (game API)\n");
    printf("  21  putpixel(x, y, color)    draw 1 pixel (0x00RRGGBB)\n");
    printf("  22  fillrect(x|w<<16,y|h<<16,c)  filled colored rectangle\n");
    printf("  23  pollkey()                non-blocking keyboard (0 = empty)\n");
    printf("  24  mouse(state*)            absolute mouse position + buttons\n");
    printf("  25  speaker(freq)            non-blocking PC-speaker tone (0=off)\n");
    printf("  26  sndbeep(freq, ms)        queue a timed tone (queue 64, EBUSY)\n");
    printf("  27  lseek(fd, off, whence)  reposition an fd (0=SET 1=CUR 2=END)\n");
    printf("  28  printf(fmt, args[3])     kernel printf for user (max 3 args)\n");
    printf("  29  ringinfo()               caller CPL: 0=kernel 3=user\n");
    printf("  30  blit(src, w|h<<16, fl)   blit 8bpp->LFB scaled (1 syscall/frame)\n");
    printf("  31  setpalette(pal*)         set a 256xRGB palette (768 B) for blit\n");
    printf("  32  keyevent()                press/release event + RAW code\n");
    printf("  33  mousedelta(int32[2])      raw PS/2 delta -> button mask\n");
    printf("  34  netinfo(u32 w[10])        network status (ip/mac/counter)\n");
    printf("  35  netping(ip)               4x blocking ICMP echo -> 0..4\n");
}
