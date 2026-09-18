/*
 * ============================================================================
 *  Morph.h — Equinox OS userland API (single-include SDK)
 * ----------------------------------------------------------------------------
 *  One header for programs that run inside Equinox OS, on top of the stable
 *  int 0x80 syscall ABI (kernel/library/header/syscall.h).
 *
 *  TWO COMPILATION PATHS, ONE SET OF NAMES
 *  =======================================
 *  1. HOSTED BUILD (recommended for full-featured programs):
 *       i386-elf-g++ -m32 -ffreestanding ... your_program.cpp
 *     Just #include "Morph.h" — every function below is a static inline
 *     wrapper that emits the real `int $0x80` instruction. No libc, no
 *     glue code, no mrp_api_t pointer table needed. Pack the ELF with
 *     mrp_user/mrp_pack.py into a .mrp and `run` it from the shell.
 *
 *  2. IN-OS BUILD (mtcc, the self-hosted compiler):
 *       mtcc your_program.c
 *     mtcc has NO preprocessor, so do NOT #include this file there.
 *     Instead, mtcc recognizes the SAME function names below as compiler
 *     BUILT-INS (see the BUILTINS table in mtcc.c) and emits the same
 *     int 0x80 sequences directly. Source written against Morph.h
 *     therefore compiles BOTH ways — only the hosted build needs the
 *     header. (Functions marked [hosted-only] below wrap multiple
 *     syscalls and are built into the kernel as single syscalls
 *     instead: file_read_all -> SYS_READFILE, etc. — the names still
 *     exist as mtcc built-ins.)
 *
 *  FILE I/O MODEL (RAMFS)
 *  ======================
 *  - fd 0/1/2 = stdin/stdout/stderr (console)
 *  - fd 3..15  = regular files (open() returns them)
 *  - file_write(path, buf, len) CREATES the file or FULLY OVERWRITES it
 *    ("timpa"): there is no partial write / append mode yet.
 *  - The standard "edit a file" pattern:
 *      1. file_size(path)              -> allocate the buffer
 *      2. file_read_all(path, buf, n)  -> load the whole file
 *      3. modify buf in memory
 *      4. file_write(path, buf, n)     -> save (overwrite) the file
 *  - file_exists(path) is a cheap probe (no fd slot used).
 *
 *  RETURN VALUES
 *  =============
 *  File functions return a negative MORPH_E* code on error, a positive
 *  count/fd/value on success — the same convention as the kernel
 *  syscall layer. Check with `if (res < 0) ...`.
 *
 *  This file is kept in sync manually with:
 *    - kernel/library/header/syscall.h   (syscall numbers)
 *    - mtcc.c BUILTINS table             (in-OS built-in names)
 *  and has a mirrored copy at mrp_user/Morph.h for hosted builds.
 * ============================================================================
 */

#ifndef MORPH_H
#define MORPH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Syscall numbers (MUST match kernel/library/header/syscall.h) ---- */
#define MORPH_SYS_EXIT        1
#define MORPH_SYS_EXEC        2
#define MORPH_SYS_GETPID      3
#define MORPH_SYS_WRITE       4
#define MORPH_SYS_READ        5
#define MORPH_SYS_OPEN        6
#define MORPH_SYS_CLOSE       7
#define MORPH_SYS_GETKEY      8
#define MORPH_SYS_READLINE    9
#define MORPH_SYS_PRINT      10
#define MORPH_SYS_PRINTINT   11
#define MORPH_SYS_MALLOC     12
#define MORPH_SYS_GETTICK    13
#define MORPH_SYS_SLEEP      14
#define MORPH_SYS_GETARGS    15
#define MORPH_SYS_MKFILE     16
#define MORPH_SYS_READFILE   17
#define MORPH_SYS_FILESIZE   18
#define MORPH_SYS_FILEEXISTS 19
#define MORPH_SYS_FBINFO     20
#define MORPH_SYS_PUTPIXEL   21
#define MORPH_SYS_FILLRECT   22
#define MORPH_SYS_POLLKEY    23
#define MORPH_SYS_MOUSE      24
#define MORPH_SYS_SPEAKER    25
#define MORPH_SYS_SNDBEEP    26
#define MORPH_SYS_LSEEK      27
#define MORPH_SYS_PRINTF     28
#define MORPH_SYS_RINGINFO   29
#define MORPH_SYS_BLIT       30
#define MORPH_SYS_SETPAL     31
#define MORPH_SYS_KEYEVENT   32
#define MORPH_SYS_MOUSEDELTA 33
#define MORPH_SYS_NETINFO    34
#define MORPH_SYS_NETPING    35

/* ---- blit flags (syscall #30) ---- */
#define MORPH_BLIT_STRETCH   0x0u   /* full stretch to screen (default)  */
#define MORPH_BLIT_ASPECT43  0x1u   /* letterbox 4:3 (native DOOM ratio) */

/* ---- seek whence (lseek / fseek) ---- */
#define MORPH_SEEK_SET 0
#define MORPH_SEEK_CUR 1
#define MORPH_SEEK_END 2

/* ---- Error codes (negative returns, stable numbering) ---- */
#define MORPH_ENOSYS  (-1)   /* unknown syscall                   */
#define MORPH_EBADF   (-2)   /* bad file descriptor               */
#define MORPH_ENOENT  (-3)   /* no such file or directory         */
#define MORPH_EISDIR  (-4)   /* path is a directory               */
#define MORPH_ENOMEM  (-5)   /* allocation failed                 */
#define MORPH_EINVAL  (-6)   /* invalid argument                  */
#define MORPH_ENOTSUP (-7)   /* operation not supported           */
#define MORPH_EMFILE  (-8)   /* fd table full                     */
#define MORPH_EBUSY   (-9)   /* nested exec rejected              */
#define MORPH_EFAULT  (-10)  /* v10.7 ring 3: pointer outside user regions */

/* ---- Standard file descriptors ---- */
#define MORPH_STDIN    0
#define MORPH_STDOUT   1
#define MORPH_STDERR   2

/* ---- Shared structs (layout MUST match kernel syscall.h) ---- */
typedef struct {
    uint32_t addr;    /* linear framebuffer address (0 = none)    */
    uint32_t width;   /* pixels                                  */
    uint32_t height;  /* pixels                                  */
    uint32_t bpp;     /* bits per pixel (32 expected)            */
    uint32_t pitch;   /* bytes per scanline                      */
    uint32_t avail;   /* 1 = graphics mode active, 0 = text only */
} morph_fbinfo_t;

typedef struct {
    int32_t  x;       /* absolute X, clamped to the screen       */
    int32_t  y;       /* absolute Y                              */
    uint32_t buttons; /* bit0=left bit1=right bit2=middle        */
} morph_mouse_t;

/* ---- Special key codes (getkey / pollkey) ---- */
#define MORPH_KEY_UP    (-1)
#define MORPH_KEY_DOWN  (-2)
#define MORPH_KEY_LEFT  (-3)
#define MORPH_KEY_RIGHT (-4)
#define MORPH_KEY_HOME  (-5)
#define MORPH_KEY_END   (-6)
#define MORPH_KEY_PGUP  (-7)
#define MORPH_KEY_PGDN  (-8)
#define MORPH_KEY_DEL   (-9)

/* ---- Extended key codes (key_event() only, v10.9) ----
 * The press/release event decoder reports these in addition to the
 * codes above. getkey()/pollkey() keep their old behavior (modifiers
 * silent, no releases) so existing programs are unaffected.       */
#define MORPH_KEY_MODCTRL  (-10)   /* Ctrl press/release (either side) */
#define MORPH_KEY_MODSHIFT (-11)   /* Shift press/release              */
#define MORPH_KEY_MODALT   (-12)   /* Alt press/release                */
#define MORPH_KEY_F1       (-13)
#define MORPH_KEY_F2       (-14)
#define MORPH_KEY_F3       (-15)
#define MORPH_KEY_F4       (-16)
#define MORPH_KEY_F5       (-17)
#define MORPH_KEY_F6       (-18)
#define MORPH_KEY_F7       (-19)
#define MORPH_KEY_F8       (-20)
#define MORPH_KEY_F9       (-21)
#define MORPH_KEY_F10      (-22)
#define MORPH_KEY_F11      (-23)
#define MORPH_KEY_F12      (-24)

/* ============================================================
 *  Raw syscall interface (hosted builds only — mtcc users never
 *  call these; mtcc emits int 0x80 for its built-ins directly)
 * ============================================================ */
static inline int morph_syscall0(int num) {
    int ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(num)
                         : "memory", "cc");
    return ret;
}

static inline int morph_syscall1(int num, uint32_t a1) {
    int ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(num), "b"(a1)
                         : "memory", "cc");
    return ret;
}

static inline int morph_syscall2(int num, uint32_t a1, uint32_t a2) {
    int ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(num), "b"(a1), "c"(a2)
                         : "memory", "cc");
    return ret;
}

static inline int morph_syscall3(int num, uint32_t a1, uint32_t a2, uint32_t a3) {
    int ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                         : "memory", "cc");
    return ret;
}

/* ============================================================
 *  Console
 * ============================================================ */

/* Print a NUL-terminated string (no automatic newline). */
static inline void print(const char* s) {
    (void)morph_syscall1(MORPH_SYS_PRINT, (uint32_t)(uintptr_t)s);
}

/* Print an unsigned 32-bit integer in decimal (no newline). */
static inline void printint(uint32_t num) {
    (void)morph_syscall1(MORPH_SYS_PRINTINT, num);
}

/* Write raw bytes to the console (binary-safe, no NUL scanning). */
static inline int write(int fd, const void* buf, uint32_t len) {
    return morph_syscall3(MORPH_SYS_WRITE, fd,
                          (uint32_t)(uintptr_t)buf, len);
}

/* Non-blocking keyboard read: ASCII code, negative special-key code
 * (-1 Up, -2 Down, -3 Left, -4 Right, ...), or -1..-9 family. */
static inline int getkey(void) {
    return morph_syscall0(MORPH_SYS_GETKEY);
}

/* NON-BLOCKING key poll for game loops (SYS_POLLKEY). Returns:
 *      0  = no key waiting this frame
 *      >0 = ASCII code of the pressed key
 *      <0 = special key (MORPH_KEY_UP etc. — same codes as getkey)
 * Unlike getkey() this NEVER blocks, so animation keeps running.
 * 0 is the "empty" marker so arrows (-1..-9) stay distinguishable. */
static inline int pollkey(void) {
    return morph_syscall0(MORPH_SYS_POLLKEY);
}

/* Blocking line input; returns the line length. */
static inline int readline(char* buf, int maxlen) {
    return morph_syscall2(MORPH_SYS_READLINE,
                          (uint32_t)(uintptr_t)buf, (uint32_t)maxlen);
}

/* ============================================================
 *  Process / memory / misc
 * ============================================================ */

/* End the program, returning `status` to the caller (the shell). */
static inline void exit(int status) {
    (void)morph_syscall1(MORPH_SYS_EXIT, (uint32_t)status);
}

/* Run another .mrp from RAMFS. NOTE: exec from INSIDE a running
 * program is rejected with MORPH_EBUSY (the caller's arena is still
 * live). A kernel-side "run after exit" pattern is planned. */
static inline int exec(const char* path) {
    return morph_syscall1(MORPH_SYS_EXEC, (uint32_t)(uintptr_t)path);
}

static inline int getpid(void) {
    return morph_syscall0(MORPH_SYS_GETPID);
}

/* Allocate from the per-program MRP arena (freed as a whole when the
 * program exits — there is no per-allocation free yet). Returns 0 on
 * failure. */
static inline void* malloc(uint32_t size) {
    return (void*)(uintptr_t)morph_syscall1(MORPH_SYS_MALLOC, size);
}

/* Timer ticks since boot (100 Hz). */
static inline uint32_t gettick(void) {
    return (uint32_t)morph_syscall0(MORPH_SYS_GETTICK);
}

/* Sleep `ms` milliseconds. */
static inline void sleep_ms(uint32_t ms) {
    (void)morph_syscall1(MORPH_SYS_SLEEP, ms);
}

/* Arguments of the last `run <file.mrp> <args...>` command. */
static inline int getargs(char* buf, int maxlen) {
    return morph_syscall2(MORPH_SYS_GETARGS,
                          (uint32_t)(uintptr_t)buf, (uint32_t)maxlen);
}

/* ============================================================
 *  File I/O (RAMFS) — the Morph.h file story
 * ============================================================ */

/* Open a file for reading -> fd (3+), or negative MORPH_E* code.
 * (The RAMFS is currently read-only at fd level; use file_write()
 * to create/overwrite whole files.) */
static inline int file_open(const char* path) {
    return morph_syscall1(MORPH_SYS_OPEN, (uint32_t)(uintptr_t)path);
}

/* Read up to `len` bytes at the current fd position; returns the
 * byte count (0 = end of file) or a negative error. */
static inline int file_read(int fd, void* buf, uint32_t len) {
    return morph_syscall3(MORPH_SYS_READ, fd,
                          (uint32_t)(uintptr_t)buf, len);
}

/* Close an fd opened with file_open(). */
static inline int file_close(int fd) {
    return morph_syscall1(MORPH_SYS_CLOSE, (uint32_t)fd);
}

/* CREATE or FULLY OVERWRITE a file with `len` bytes from `buf`
 * (binary-safe, stops at nothing). This is THE save primitive —
 * "overwrite a file". Paths may be "name", "dir/name" or "/dir/name",
 * resolved against the process cwd. */
static inline int file_write(const char* path, const void* buf, uint32_t len) {
    return morph_syscall3(MORPH_SYS_MKFILE, (uint32_t)(uintptr_t)path,
                          (uint32_t)(uintptr_t)buf, len);
}

/* Read the WHOLE file into `buf` (up to maxlen bytes) in one call —
 * kernel-side open + read-all + close. Returns the byte count, or a
 * negative error. Compare with file_size() to detect truncation. */
static inline int file_read_all(const char* path, void* buf, uint32_t maxlen) {
    return morph_syscall3(MORPH_SYS_READFILE, (uint32_t)(uintptr_t)path,
                          (uint32_t)(uintptr_t)buf, maxlen);
}

/* Size of a file in bytes (needed to size the buffer before
 * file_read_all), or a negative error. */
static inline int file_size(const char* path) {
    return morph_syscall1(MORPH_SYS_FILESIZE, (uint32_t)(uintptr_t)path);
}

/* 1 if a regular file exists at `path`, 0 otherwise (directories do
 * not count). Cheap probe — consumes no fd slot. */
static inline int file_exists(const char* path) {
    return morph_syscall1(MORPH_SYS_FILEEXISTS, (uint32_t)(uintptr_t)path);
}

/* ============================================================
 *  Network API (syscalls 34-35) — v10.12 Phase C
 * ============================================================ */

/* Network status block filled by net_info(): 10 uint32 words.
 * ip/netmask/gateway are in HOST order (first octet = lowest byte):
 *   (n.ip >> 0) & 0xFF is the first dotted quad number. */
typedef struct MorphNetInfo {
    uint32_t up;        /* 1 = lwIP stack + NE2000 NIC running        */
    uint32_t dhcp;      /* 1 = address obtained via DHCP              */
    uint32_t ip;        /* host order a.b.c.d                         */
    uint32_t netmask;   /* host order                                 */
    uint32_t gateway;   /* host order                                 */
    uint32_t mac_lo;    /* MAC bytes 0..3 (little-endian packed)      */
    uint32_t mac_hi;    /* MAC bytes 4..5                             */
    uint32_t rx;        /* packets received by the driver             */
    uint32_t tx;        /* packets transmitted                        */
    uint32_t drop;      /* packets dropped (pool exhausted/error)     */
} MorphNetInfo;

/* Fill `n` with the current network status -> 0, or MORPH_EFAULT.
 * Safe no-op returning up=0 when no NIC is present. */
static inline int net_info(MorphNetInfo* n) {
    return morph_syscall1(MORPH_SYS_NETINFO, (uint32_t)(uintptr_t)n);
}

/* Send 4 blocking ICMP echoes to `ip` (dotted string, e.g.
 * "10.0.2.2"). Returns the reply count 0..4 (~5 s worst case). */
static inline int net_ping(const char* ip) {
    return morph_syscall1(MORPH_SYS_NETPING, (uint32_t)(uintptr_t)ip);
}

/* ============================================================
 *  Game API (syscalls 20-26) — VESA framebuffer, non-blocking
 *  keyboard, mouse and PC speaker, exposed to userland BEFORE
 *  ring 3 exists. Colors are raw 0x00RRGGBB values in the
 *  standard 32bpp mode.
 * ============================================================ */

/* Query the linear framebuffer. Always call this FIRST in a game:
 *     morph_fbinfo_t fb;
 *     fb_info(&fb);
 *     if (!fb.avail) { print("no graphics mode\n"); exit(1); }
 * Returns 0 (struct filled — check fb.avail) or MORPH_EINVAL.
 * Advanced: fb.addr + fb.pitch let you compute the address of any
 * pixel and write it directly (blitting) for bulk drawing. */
static inline int fb_info(morph_fbinfo_t* info) {
    return morph_syscall1(MORPH_SYS_FBINFO, (uint32_t)(uintptr_t)info);
}

/* Draw one pixel (out-of-range coords are clipped by the kernel). */
static inline int put_pixel(int x, int y, uint32_t color) {
    return morph_syscall3(MORPH_SYS_PUTPIXEL,
                          (uint32_t)x, (uint32_t)y, color);
}

/* Draw a solid rectangle. This wrapper PACKS x/w and y/h into the
 * two syscall argument registers (the ABI passes only 3 regs):
 * EBX = x | (w << 16), ECX = y | (h << 16), EDX = color.
 * mtcc users without Morph.h must pack inline:
 *     fill_rect(x | (w << 16), y | (h << 16), color);
 */
static inline int fill_rect(int x, int y, int w, int h, uint32_t color) {
    return morph_syscall3(MORPH_SYS_FILLRECT,
                          (uint32_t)x | ((uint32_t)w << 16),
                          (uint32_t)y | ((uint32_t)h << 16),
                          color);
}

/* Absolute mouse state: position clamped to the screen (starts at
 * the center before any movement) + button bits (1=left 2=right
 * 4=middle). Returns 0 or MORPH_EINVAL. */
static inline int mouse_state(morph_mouse_t* st) {
    return morph_syscall1(MORPH_SYS_MOUSE, (uint32_t)(uintptr_t)st);
}

/* Start a continuous PC-speaker tone at `freq` Hz — NON-blocking,
 * the caller silences it later with spk_silence(). */
static inline void spk_tone(uint32_t freq) {
    (void)morph_syscall1(MORPH_SYS_SPEAKER, freq);
}

/* Silence the PC speaker. */
static inline void spk_silence(void) {
    (void)morph_syscall1(MORPH_SYS_SPEAKER, 0);
}

/* Queue a TIMED PC-speaker note: freq Hz for ms milliseconds — the
 * kernel timer plays it out, the call returns immediately. A melody
 * is just a series of snd_beep() calls with NO sleep between them;
 * the program (and the shell under it) keeps running while the
 * music plays. freq=0 queues a rest (silence). The kernel queue
 * holds 64 pending notes; beyond that this returns MORPH_EBUSY and
 * the note is dropped. ms==0 returns MORPH_EINVAL. */
static inline int snd_beep(uint32_t freq, uint32_t ms) {
    return morph_syscall2(MORPH_SYS_SNDBEEP, freq, ms);
}

/* ============================================================
 *  FAST BLIT PATH (v10.9, syscalls 30-31) — the DOOM pipeline
 *  ============================================================
 * Per-pixel put_pixel() costs one int 0x80 per pixel — hopeless for
 * a 35 fps 320x200 game. Instead:
 *    1. set_palette(pal) once per palette change (768 bytes RGB),
 *    2. blit(buf, w, h, flags) once per frame — the kernel converts
 *       the 8bpp index buffer to 32bpp and scales it to the whole
 *       screen (nearest-neighbor, fixed-point, no per-pixel div).
 * flags: MORPH_BLIT_STRETCH fills the screen; MORPH_BLIT_ASPECT43
 * letterboxes to 4:3 (original DOOM proportions) centered.
 * The source buffer must live in the user arena (uaccess-checked). */
static inline int set_palette(const uint8_t pal[768]) {
    return morph_syscall1(MORPH_SYS_SETPAL, (uint32_t)(uintptr_t)pal);
}

static inline int blit(const void* src, int w, int h, uint32_t flags) {
    uint32_t dims = ((uint32_t)(uint16_t)h << 16) | (uint32_t)(uint16_t)w;
    return morph_syscall3(MORPH_SYS_BLIT,
                          (uint32_t)(uintptr_t)src, dims, flags);
}

/* ============================================================
 *  KEY EVENTS (v10.9, syscall 32) — press AND release + RAW codes
 *  ============================================================
 * pollkey() only reports key presses as shift-transformed ASCII and
 * keeps modifier keys silent — fine for menus, useless for games
 * that track HELD keys (DOOM: hold Up = keep walking). key_event()
 * returns one event per call:
 *    0                        -> nothing pending
 *    (1 << 16) | (code&FFFF)  -> PRESS   of `code`
 *    (0 << 16) | (code&FFFF)  -> RELEASE of `code`
 * `code` is the RAW (unshifted) key: positive ASCII, or negative
 * specials — arrows -1..-4 (see MORPH_KEY_*), modifiers -10..-12,
 * F1..F12 = -13..-24. Cast the low half to int16_t to sign-extend. */
static inline int key_event(void) {
    return morph_syscall0(MORPH_SYS_KEYEVENT);
}

/* Convenience: 1 if a press/release event was pending (code+pressed
 * stored through the pointers), 0 if the queue was empty. */
static inline int key_event_pop(int* pressed, int* code) {
    int v = morph_syscall0(MORPH_SYS_KEYEVENT);
    if (v == 0) return 0;
    *pressed = (v >> 16) & 1;
    *code = (int)(int16_t)(uint16_t)(v & 0xFFFFu);
    return 1;
}

/* ============================================================
 *  LIBC LAYER (v10.8) — hosted-build mirror of the mtcc prelude.
 *  Same NAMES as the in-OS prelude (#include <morph.h> in mtcc),
 *  so source compiles on BOTH paths. Differences are documented
 *  per function; the hosted side is full C++ (casts, structs,
 *  real varargs, generic qsort with function pointers).
 * ============================================================ */

/* ---- lseek / ringinfo (direct syscall wrappers) ---- */
static inline int lseek_(int fd, int off, int whence) {
    return morph_syscall3(MORPH_SYS_LSEEK, (uint32_t)fd,
                          (uint32_t)off, (uint32_t)whence);
}
/* NOTE on naming: < Morph.h > defines no `lseek` to keep freestanding
 * builds warning-free; use lseek_() here, or the prelude name lseek()
 * in mtcc. ring(): privilege level of the caller — 0 = kernel shell,
 * 3 = user program. A program proving it runs unprivileged:
 *     printf("ring %d\n", ring());   // prints 3 from a .mrp */
static inline int ring(void) {
    return morph_syscall0(MORPH_SYS_RINGINFO);
}

/* ---- memory ---- */
static inline void* memcpy_(void* d, const void* s, size_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
static inline void* memset_(void* d, int c, size_t n) {
    uint8_t* dd = (uint8_t*)d;
    for (size_t i = 0; i < n; i++) dd[i] = (uint8_t)c;
    return d;
}
static inline void* memmove_(void* d, const void* s, size_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    if (dd < ss) { for (size_t i = 0; i < n; i++) dd[i] = ss[i]; }
    else { for (size_t i = n; i > 0; i--) dd[i-1] = ss[i-1]; }
    return d;
}
static inline int memcmp_(const void* a, const void* b, size_t n) {
    const uint8_t* aa = (const uint8_t*)a; const uint8_t* bb = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++)
        if (aa[i] != bb[i]) return (int)aa[i] - (int)bb[i];
    return 0;
}

/* ---- string ---- */
static inline size_t strlen_(const char* s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static inline int strcmp_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
static inline int strncmp_(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
    }
    return 0;
}
static inline char* strcpy_(char* d, const char* s) {
    char* p = d; while ((*p++ = *s++)); return d;
}
static inline char* strncpy_(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}
static inline char* strcat_(char* d, const char* s) {
    char* p = d; while (*p) p++;
    while ((*p++ = *s++));
    return d;
}
static inline char* strncat_(char* d, const char* s, size_t n) {
    char* p = d; while (*p) p++;
    size_t i = 0;
    for (; i < n && s[i]; i++) p[i] = s[i];
    p[i] = '\0';
    return d;
}
static inline char* strchr_(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return 0;
    }
}
static inline char* strrchr_(const char* s, int c) {
    const char* last = 0;
    for (; *s; s++) if (*s == (char)c) last = s;
    return (char*)last;
}
static inline char* strstr_(const char* h, const char* n) {
    if (!n[0]) return (char*)h;
    for (; *h; h++) {
        const char* a = h; const char* b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char*)h;
    }
    return 0;
}

/* ---- conversion ---- */
static inline long strtol_(const char* s, char** endp, int base) {
    const char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        char c2 = p[2]; int d2 = -1;
        if (c2 >= '0' && c2 <= '9') d2 = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') d2 = c2 - 'a' + 10;
        else if (c2 >= 'A' && c2 <= 'F') d2 = c2 - 'A' + 10;
        if (d2 >= 0 && d2 < 16) { p += 2; base = 16; }
        else if (base == 0) base = 8;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }
    long v = 0; int any = 0;
    for (;;) {
        int d = -1; char c = *p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        if (d < 0 || d >= base) break;
        v = v * base + d; p++; any = 1;
    }
    if (endp) *endp = (char*)(any ? p : s);
    return neg ? -v : v;
}
static inline int atoi_(const char* s) { return (int)strtol_(s, 0, 10); }

/* ---- printf family (max 3 conversions — kernel ABI arg slots) ----
 * The kernel syscall MORPH_SYS_PRINTF renders %d %u %x %X %c %s %%
 * with width/zero-pad/left-align. The hosted build uses real varargs
 * but still forwards at most 3 conversion arguments. */
static inline int printf_(const char* fmt, int a = 0, int b = 0, int c = 0) {
    int args[3] = { a, b, c };
    return morph_syscall2(MORPH_SYS_PRINTF,
                          (uint32_t)(uintptr_t)fmt, (uint32_t)(uintptr_t)args);
}

/* snprintf_: bounded renderer, same spec set as the kernel printf.
 * Returns the FULL formatted length (C99), buffer always NUL-ended. */
static inline int snprintf_(char* buf, size_t size, const char* fmt,
                            int a = 0, int b = 0, int c = 0) {
    size_t pos = 0;
    auto putc_ = [&](char ch) {
        if (pos + 1 < size) buf[pos] = ch;
        pos++;
    };
    int args[3] = { a, b, c }; int argn = 0;
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { putc_(*p); continue; }
        p++;
        if (*p == '%') { putc_('%'); continue; }
        if (!*p) break;
        int left = 0, zero = 0, width = 0;
        if (*p == '-') { left = 1; p++; }
        if (*p == '0') { zero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        char spec = *p;
        int v = (argn < 3) ? args[argn] : 0;
        if (spec == 'd' || spec == 'u' || spec == 'x' || spec == 'X') {
            argn++;
            char nb[16]; int nl = 0;
            uint32_t base = (spec == 'x' || spec == 'X') ? 16 : 10;
            uint32_t uv; int negv = 0;
            if (spec == 'd' && v < 0) { negv = 1; uv = (uint32_t)(-(int32_t)v); }
            else uv = (uint32_t)v;
            if (!uv) nb[nl++] = '0';
            while (uv) {
                uint32_t d = uv % base;
                nb[nl++] = (char)(d < 10 ? '0' + d : (spec == 'X' ? 'A' : 'a') + d - 10);
                uv /= base;
            }
            if (negv) nb[nl++] = '-';
            char padc = (zero && !negv && !left) ? '0' : ' ';
            if (!left) for (int i = nl; i < width; i++) putc_(padc);
            for (int i = nl - 1; i >= 0; i--) putc_(nb[i]);
            if (left) for (int i = nl; i < width; i++) putc_(' ');
        } else if (spec == 's') {
            argn++;
            const char* s = (const char*)(uintptr_t)(uint32_t)v;
            if (!s) s = "(null)";
            size_t len = 0; while (s[len]) len++;
            if (!left) for (size_t i = len; i < (size_t)width; i++) putc_(' ');
            for (size_t i = 0; i < len; i++) putc_(s[i]);
            if (left) for (size_t i = len; i < (size_t)width; i++) putc_(' ');
        } else if (spec == 'c') {
            argn++;
            char ch = (char)v;
            if (!left) for (int i = 1; i < width; i++) putc_(' ');
            putc_(ch);
            if (left) for (int i = 1; i < width; i++) putc_(' ');
        }
    }
    if (size) buf[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
}
static inline int sprintf_(char* buf, const char* fmt,
                           int a = 0, int b = 0, int c = 0) {
    return snprintf_(buf, (size_t)-1 / 2, fmt, a, b, c);
}

/* ---- misc ---- */
static inline void abort_(void) {
    print("abort() called\n");
    exit(134);
}
static inline long time_(long* t) {          /* seconds since boot */
    long secs = (long)(morph_syscall0(MORPH_SYS_GETTICK) / 100u);
    if (t) *t = secs;
    return secs;
}
static inline const char* getenv_(const char* name) {
    (void)name;                              /* no environment block yet */
    return 0;
}

/* ---- generic qsort (hosted only — needs function pointers;
 * the mtcc prelude provides qsort_int / qsort_str instead) ---- */
static inline void qsort_(void* base, size_t n, size_t sz,
                          int (*cmp)(const void*, const void*)) {
    /* iterative quicksort, insertion sort for small partitions */
    if (!base || n < 2 || !sz || !cmp) return;
    uint8_t* a = (uint8_t*)base;
    uint8_t tmp[64];
    size_t stack_[32]; int sp = 0;
    stack_[sp++] = 0; stack_[sp++] = n - 1;
    while (sp) {
        size_t hi = stack_[--sp], lo = stack_[--sp];
        if (hi <= lo || hi - lo + 1 <= 8) {
            for (size_t i = lo + 1; i <= hi; i++) {
                uint8_t* src = a + i * sz;
                for (size_t k = 0; k < sz; k++) tmp[k] = src[k];
                size_t j = i;
                while (j > lo && cmp(a + (j - 1) * sz, tmp) > 0) {
                    uint8_t* dst = a + j * sz; uint8_t* mv = a + (j - 1) * sz;
                    for (size_t k = 0; k < sz; k++) dst[k] = mv[k];
                    j--;
                }
                uint8_t* dst = a + j * sz;
                for (size_t k = 0; k < sz; k++) dst[k] = tmp[k];
            }
            continue;
        }
        size_t mid = lo + (hi - lo) / 2;
        uint8_t* pl = a + lo * sz; uint8_t* pm = a + mid * sz;
        for (size_t k = 0; k < sz; k++) { uint8_t t = pl[k]; pl[k] = pm[k]; pm[k] = t; }
        size_t i = lo + 1, j = hi;
        for (;;) {
            while (i <= j && cmp(a + i * sz, pl) < 0) i++;
            while (j > lo && cmp(a + j * sz, pl) > 0) j--;
            if (i > j) break;
            uint8_t* pi = a + i * sz; uint8_t* pj = a + j * sz;
            for (size_t k = 0; k < sz; k++) { uint8_t t = pi[k]; pi[k] = pj[k]; pj[k] = t; }
            i++; if (!j) break; j--;
        }
        pl = a + lo * sz; uint8_t* pj = a + j * sz;
        for (size_t k = 0; k < sz; k++) { uint8_t t = pl[k]; pl[k] = pj[k]; pj[k] = t; }
        if (j > lo + 1) { stack_[sp++] = lo; stack_[sp++] = j - 1; }
        if (j + 1 < hi) { stack_[sp++] = j + 1; stack_[sp++] = hi; }
    }
}

/* ---- stdio FILE I/O (same model as the mtcc prelude) ----
 * "r": direct fd (fseek via lseek). "w"/"a": write buffer, fclose
 * flushes the whole file via file_write. Handles are 1..8 (0=NULL).
 * Modeled with a small static table — one open file per slot. */
#define MORPH_FOPEN_MAX 8
#define MORPH_FIO_PATH_MAX 64
struct morph_file_t {
    int   used;          /* slot state: 0 free, 1 read-fd, 2 write-buffer */
    int   fd;
    int   len;
    int   cap;
    char* buf;
    char  path[MORPH_FIO_PATH_MAX];
};
static morph_file_t morph_files[MORPH_FOPEN_MAX];

static inline int fopen_(const char* path, const char* mode) {
    for (int i = 0; i < MORPH_FOPEN_MAX; i++) {
        if (!morph_files[i].used) {
            morph_file_t* f = &morph_files[i];
            char m = mode ? mode[0] : 'r';
            if (m == 'w' || m == 'a') {
                f->buf = (char*)malloc(4096);
                if (!f->buf) return 0;
                f->cap = 4096; f->len = 0;
                strncpy_(f->path, path, MORPH_FIO_PATH_MAX - 1);
                f->path[MORPH_FIO_PATH_MAX - 1] = '\0';
                if (m == 'a') {
                    int n = file_size(path);
                    if (n > 0) {
                        if ((size_t)n + 16 > (size_t)f->cap) {
                            char* nb = (char*)malloc((uint32_t)n + 16);
                            if (!nb) return 0;
                            /* small buffer intentionally leaked — the MRP
                             * arena is released as a whole at program exit */
                            f->buf = nb; f->cap = n + 16;
                        }
                        n = file_read_all(path, f->buf, (uint32_t)n);
                        f->len = (n > 0) ? n : 0;
                    }
                }
                f->used = 2; f->fd = -1;
                return i + 1;
            }
            int fd = file_open(path);
            if (fd < 0) return 0;
            f->fd = fd; f->used = 1; f->len = 0;
            return i + 1;
        }
    }
    print("fopen: too many open files\n");
    return 0;
}
static inline int fread_(void* buf, size_t sz, size_t n, int h) {
    if (h <= 0 || h > MORPH_FOPEN_MAX) return 0;
    morph_file_t* f = &morph_files[h - 1];
    if (f->used != 1) return 0;
    size_t total = sz * n;
    if (!total) return 0;
    int got = file_read(f->fd, buf, (uint32_t)total);
    return (got > 0) ? got : 0;
}
static inline int fwrite_(const void* buf, size_t sz, size_t n, int h) {
    if (h <= 0 || h > MORPH_FOPEN_MAX) return 0;
    morph_file_t* f = &morph_files[h - 1];
    if (f->used != 2) return 0;
    size_t total = sz * n;
    if (!total) return 0;
    while ((size_t)f->len + total > (size_t)f->cap) {
        int ncap = f->cap * 2;
        char* nb = (char*)malloc((uint32_t)ncap);
        if (!nb) return 0;
        memcpy_(nb, f->buf, (size_t)f->len);
        /* NOTE: old buffer leaks by design (arena-freed at exit) */
        f->buf = nb; f->cap = ncap;
    }
    memcpy_(f->buf + f->len, buf, total);
    f->len += (int)total;
    return (int)total;
}
static inline int fseek_(int h, int off, int whence) {
    if (h <= 0 || h > MORPH_FOPEN_MAX) return -1;
    morph_file_t* f = &morph_files[h - 1];
    if (f->used == 1) {
        morph_syscall3(MORPH_SYS_LSEEK, (uint32_t)f->fd,
                       (uint32_t)off, (uint32_t)whence);
        return 0;
    }
    if (f->used == 2) {
        int npos = off;
        if (whence == MORPH_SEEK_CUR) npos = f->len + off;
        else if (whence == MORPH_SEEK_END) npos = f->len + off;
        if (npos < 0) npos = 0;
        if (npos > f->len) npos = f->len;
        f->len = npos;
        return 0;
    }
    return -1;
}
static inline int ftell_(int h) {
    if (h <= 0 || h > MORPH_FOPEN_MAX) return -1;
    morph_file_t* f = &morph_files[h - 1];
    if (f->used == 1)
        return morph_syscall3(MORPH_SYS_LSEEK, (uint32_t)f->fd, 0,
                              MORPH_SEEK_CUR);
    return f->len;
}
static inline int fclose_(int h) {
    if (h <= 0 || h > MORPH_FOPEN_MAX) return -1;
    morph_file_t* f = &morph_files[h - 1];
    int r = 0;
    if (f->used == 1) {
        r = file_close(f->fd);
    } else if (f->used == 2) {
        r = file_write(f->path, f->buf, (uint32_t)f->len);
    }
    f->used = 0; f->fd = 0; f->len = 0; f->cap = 0; f->buf = 0;
    f->path[0] = '\0';
    return r;
}

#ifdef __cplusplus
}
#endif

#endif /* MORPH_H */
