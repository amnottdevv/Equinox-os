#ifndef SYSCALL_H
#define SYSCALL_H

/*
 * ============================================================================
 *  syscall.h — Equinox OS syscall layer (int 0x80)
 * ----------------------------------------------------------------------------
 *  ABI contract (STABLE — do not change without a version bump & a note
 *  in the docs):
 *    - Syscall number    : EAX register
 *    - Arguments 1/2/3   : EBX / ECX / EDX registers
 *    - Return value      : EAX register (negative value = error, see errno)
 *    - Call instruction  : `int $0x80` (software interrupt)
 *
 *  Gate 0x80 in the IDT: TRAP gate DPL=3 (flags 0xEF), installed in
 *  idt_init().
 *    - DPL=3 -> callable from ring 3 (ACTIVE since v10.7: all .mrp
 *                programs run at CPL 3 via user3_launch) AND from
 *                ring 0 (legacy shell/kernel path).
 *    - Trap gate (not interrupt gate) -> IF is NOT cleared on entry to
 *      the handler, so blocking syscalls (readline/sleep/exec) can still
 *      be interrupted by timer/keyboard IRQs. Linux uses the same
 *      pattern. Note: if the caller invokes with IF=0 (cli section), IF
 *      stays 0 for the whole syscall -> blocking syscalls will hang.
 *      Do not call syscalls from a critical section.
 *
 *  The CPL 3 -> CPL 0 transition via int 0x80 automatically gets a kernel
 *  stack from the TSS (SS0:ESP0 = user_int_stack, see usermode.cpp) — the
 *  isr_128 stub and the TSS register slot layout do NOT change between
 *  ring 0 and ring 3; dispatch reads the CS slot (regs[9]) to detect the
 *  privilege level.
 *
 *  Current status: single-tasking, caller = ring 0 (shell) or ring 3
 *  (.mrp program).
 *    - EXIT from ring 3 branches to user3_terminate() (program dies,
 *      control returns to mrp_run/shell); EXIT from ring 0 merely
 *      returns the status to the caller.
 *    - Argument pointers from a ring 3 caller are validated against the
 *      user region (uaccess in syscall.cpp) — violation = SYS_EFAULT.
 *    - fd 0/1/2 = console (stdin/stdout/stderr), fd 3+ = RAMFS files
 *      (read-only, sequential; no file writes yet).
 *
 *  Since v10.7, .mrp programs run purely on top of this int 0x80 syscall
 *  interface (Morph.h / mtcc built-ins). The old mrp_api_t table survives
 *  only for historical reference — kernel function pointers cannot be
 *  called from CPL 3.
 * ============================================================================
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------
//  Syscall numbers (EAX). 0 is deliberately unused (easy to detect
//  as "forgot to set EAX"). Add new syscalls ONLY at the end of the
//  list + fill the table in syscall.cpp + update syscall_list().
// ---------------------------------------------------------------
#define SYS_EXIT      1   /* (status)                        -> status        */
#define SYS_EXEC      2   /* (path)   run .mrp from RAMFS    -> 0 / errno     */
#define SYS_GETPID    3   /* ()       caller's pid           -> 1 (single)    */
#define SYS_WRITE     4   /* (fd, buf, len) write to console -> n bytes       */
#define SYS_READ      5   /* (fd, buf, len) read RAMFS file  -> n / 0=EOF     */
#define SYS_OPEN      6   /* (path)   open RAMFS file (RO)   -> fd (3+)       */
#define SYS_CLOSE     7   /* (fd)    close fd                -> 0             */
#define SYS_GETKEY    8   /* ()      keyboard non-blocking   -> key / -1      */
#define SYS_READLINE  9   /* (buf, maxlen) read 1 line       -> length        */
#define SYS_PRINT     10  /* (str)   print string to console -> 0             */
#define SYS_PRINTINT  11  /* (num)   print unsigned number   -> 0             */
#define SYS_MALLOC    12  /* (size)  allocate MRP arena      -> ptr / 0       */
#define SYS_GETTICK   13  /* ()      timer ticks since boot  -> tick          */
#define SYS_SLEEP     14  /* (ms)    delay execution         -> 0             */
#define SYS_GETARGS   15  /* (buf, maxlen) args of last run  -> length     */
#define SYS_MKFILE    16  /* (path, buf, len) write RAMFS file -> 0 / errno  */
#define SYS_READFILE  17  /* (path, buf, maxlen) read whole file -> n/errno */
#define SYS_FILESIZE  18  /* (path) file size in bytes       -> n/errno    */
#define SYS_FILEEXISTS 19 /* (path) 1 = file exists, 0 = no  -> 0/1        */
#define SYS_FBINFO    20  /* (info*) VESA framebuffer info   -> 0 / EINVAL  */
#define SYS_PUTPIXEL  21  /* (x, y, color) draw 1 pixel      -> 0 / ENOTSUP */
#define SYS_FILLRECT  22  /* (x|w<<16, y|h<<16, color) rect  -> 0 / ENOTSUP */
#define SYS_POLLKEY   23  /* ()  keyboard NON-BLOCKING       -> 0 / key     */
#define SYS_MOUSE     24  /* (state*) absolute mouse + buttons -> 0 / EINVAL */
#define SYS_SPEAKER   25  /* (freq) freq>0 beep on, 0 off    -> 0           */
#define SYS_SNDBEEP   26  /* (freq, ms) queue timed beep     -> 0 / EBUSY   */
#define SYS_LSEEK     27  /* (fd, off, whence) fd position   -> pos / errno */
#define SYS_PRINTF    28  /* (fmt, int args[3]) kernel printf -> n char     */
#define SYS_RINGINFO  29  /* () caller CPL (0 kernel/3 user) -> 0 / 3       */
#define SYS_BLIT      30  /* (src, w|h<<16, flags) blit 8bpp -> 0 / errno   */
#define SYS_SETPAL    31  /* (pal*) set 256xRGB palette (768 B) -> 0 / errno */
#define SYS_KEYEVENT  32  /* () press/release event + RAW code -> 0 / packed */
#define SYS_MOUSEDELTA 33 /* (int32 dxdy[2]) PS/2 delta     -> buttons / err */
#define SYS_NETINFO    34 /* (u32 w[10]) network info       -> 0 / errno     */
#define SYS_NETPING    35 /* (const char* ip) 4x ICMP blk   -> 0..4 reply    */

/* Number of table entries (index 0 = NULL, 1..35 = syscalls above). */
#define SYS_COUNT     36

/* ---------------------------------------------------------------
 *  Syscall SYS_NETINFO #34 (v10.12 Fase C — ring 3 network access).
 * EBX = user pointer to uint32_t[10], filled by the kernel (net_get_info):
 *   w[0] = up       1 = stack + NIC alive
 *   w[1] = dhcp     1 = IP from DHCP (0 = static fallback)
 *   w[2] = ip       host order a.b.c.d (a = low byte)
 *   w[3] = netmask  host order
 *   w[4] = gateway  host order
 *   w[5] = mac_lo   MAC bytes 0..3 (little-endian packed)
 *   w[6] = mac_hi   MAC bytes 4..5
 *   w[7] = rx       packets received by the driver
 *   w[8] = tx       packets sent by the driver
 *   w[9] = drop     packets dropped (pool exhausted / error)
 * --------------------------------------------------------------- */

/* ---------------------------------------------------------------
 *  Syscall SYS_NETPING #35 (v10.12 Fase C).
 * EBX = pointer to dotted IP string. Blocks ~5 seconds (4x echo,
 * 1x/second + ARP warm-up). Return = number of replies (0..4);
 * without a NIC returns 0 (not an errno — 0 replies simply means
 * failure).
 * --------------------------------------------------------------- */

/* ---------------------------------------------------------------
 *  Syscall SYS_BLIT #30 flags (v10.9 — DOOM fast graphics path).
 *  EBX = user pointer to the 8bpp (palette index) buffer, ECX = w | h<<16,
 *  EDX = flags. The kernel converts + scales to the 32bpp LFB with
 *  nearest-neighbor — ONE syscall per frame (not per pixel).
 * --------------------------------------------------------------- */
#define SYS_BLIT_STRETCH    0x0u  /* default: stretch to the full screen */
#define SYS_BLIT_ASPECT43   0x1u  /* 4:3 letterbox (original DOOM 320x200)*/

/* ---------------------------------------------------------------
 *  Syscall SYS_KEYEVENT #32 (v10.9 — DOOM input path).
 * Return EAX: 0 = no event; otherwise packed:
 *   bit 16    = 1 PRESS event, 0 RELEASE event
 *   bit 0-15  = RAW key code (cast to int16_t for negative codes)
 * Codes: positive ASCII (unshifted), or negative: -1 Up, -2 Down,
 * -3 Left, -4 Right, -5..-9 Home/End/PgUp/PgDn/Del, -10 Ctrl,
 * -11 Shift, -12 Alt, -13..-22 F1..F10, -23 F11, -24 F12.
 * See getkey_event() in stdio.cpp for decoder details.
 * --------------------------------------------------------------- */

// ---------------------------------------------------------------
//  Errno (negative EAX return). Numbers are STABLE — do not reorder.
// ---------------------------------------------------------------
#define SYS_ENOSYS  (-1)  /* unknown syscall number / not implemented        */
#define SYS_EBADF   (-2)  /* invalid fd / not opened yet                     */
#define SYS_ENOENT  (-3)  /* file/path does not exist                        */
#define SYS_EISDIR  (-4)  /* path is a directory                             */
#define SYS_ENOMEM  (-5)  /* allocation failed / arena full                   */
#define SYS_EINVAL  (-6)  /* invalid argument (NULL, etc.)                    */
#define SYS_ENOTSUP (-7)  /* operation not supported yet (e.g. file writes)   */
#define SYS_EMFILE  (-8)  /* fd table full (too many open files)              */
#define SYS_EBUSY   (-9)  /* FIX(audit V3 #1): nested exec rejected - calling program still running in the MRP arena */
#define SYS_EFAULT  (-10) /* v10.7 ring 3: argument pointer outside the user region (uaccess) */
#define SYS_EIO     (-11) /* v0.2: disk I/O error (FAT32 read/write through) */

// ---------------------------------------------------------------
//  Whence values for SYS_LSEEK #27 (ABI values — do not change).
// ---------------------------------------------------------------
#define SYS_SEEK_SET 0
#define SYS_SEEK_CUR 1
#define SYS_SEEK_END 2

// ---------------------------------------------------------------
//  Standard file descriptors (POSIX-clone semantics).
// ---------------------------------------------------------------
#define SYS_FD_STDIN   0
#define SYS_FD_STDOUT  1
#define SYS_FD_STDERR  2

// ---------------------------------------------------------------
//  Shared ABI structs for syscalls #20 (fbinfo) and #24 (mouse).
//  Layout is STABLE: fields may only be APPENDED at the tail of the
//  struct, never changed/removed (userland programs compile this
//  layout via Morph.h — an exact mirror of this kernel definition).
// ---------------------------------------------------------------
typedef struct {
    uint32_t addr;     /* linear framebuffer address (0 = none)       */
    uint32_t width;    /* width in pixels                             */
    uint32_t height;   /* height in pixels                            */
    uint32_t bpp;      /* bits per pixel (32 expected)                */
    uint32_t pitch;    /* bytes per scanline                          */
    uint32_t avail;    /* 1 = graphics mode active, 0 = text/VGA only */
} morph_fbinfo_t;

typedef struct {
    int32_t  x;        /* absolute X position (clamped to screen)     */
    int32_t  y;        /* absolute Y position                         */
    uint32_t buttons;  /* bit0=left bit1=right bit2=middle            */
} morph_mouse_t;

// ---------------------------------------------------------------
//  Gate 0x80 entry stub (defined via asm in syscall.cpp).
//  Installed into the IDT by idt_init() — do not call it manually.
// ---------------------------------------------------------------
extern void isr_128(void);

// ---------------------------------------------------------------
//  .mrp program arguments: the shell stores the remainder of the
//  `run <file.mrp> <args...>` command line via syscall_set_args()
//  BEFORE mrp_run(); the program reads it via syscall SYS_GETARGS
//  (#15). Cleared again after the program finishes so it does not
//  leak into the next program.
// ---------------------------------------------------------------
void syscall_set_args(const char* args);

// ---------------------------------------------------------------
//  Process CWD: the shell updates this "process" working directory
//  every time the prompt is printed (and after `cd`). The
//  open/exec/mkfile syscalls resolve RELATIVE paths ("main.c",
//  "test/hello.c") against this cwd — that way global tools like
//  `mtcc main.c` work from any directory, not just from the root.
// ---------------------------------------------------------------
void syscall_set_cwd(struct fs_node* cwd);
struct fs_node* syscall_get_cwd(void);

// ---------------------------------------------------------------
//  Initialize the fd table (called once from kernel_main, after
//  fs_init — idempotent, safe to call repeatedly).
// ---------------------------------------------------------------
void syscall_init(void);

// Print the syscall list to the console (used by the `syscalls` shell command).
void syscall_list(void);

// ---------------------------------------------------------------
//  Syscall call wrappers — the raw `int $0x80` instruction.
//  Works in ring 0 now and in ring 3 later without changes.
//  (Requires a non-PIC toolchain — the Equinox OS makefile is non-PIC.)
// ---------------------------------------------------------------
static inline int syscall0(int num) {
    int ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(num)
                 : "memory", "cc");
    return ret;
}

static inline int syscall1(int num, uint32_t a1) {
    int ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(num), "b"(a1)
                 : "memory", "cc");
    return ret;
}

static inline int syscall2(int num, uint32_t a1, uint32_t a2) {
    int ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(num), "b"(a1), "c"(a2)
                 : "memory", "cc");
    return ret;
}

static inline int syscall3(int num, uint32_t a1, uint32_t a2, uint32_t a3) {
    int ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                 : "memory", "cc");
    return ret;
}

#ifdef __cplusplus
}
#endif

#endif /* SYSCALL_H */
