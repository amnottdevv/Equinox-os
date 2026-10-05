/*
 * ============================================================================
 *  Libgame — lightweight game framework for Equinox OS .mrp programs
 * ----------------------------------------------------------------------------
 *  Header-only library layered ON TOP of Morph.h (the raw syscall SDK).
 *  It exists so every game (snake, breakout, pong, tetris, ...) shares the
 *  same battle-tested helpers instead of re-implementing them (and re-fixing
 *  the same bugs) per game:
 *
 *    lg_init()        framebuffer probe + capability check (32bpp required)
 *    lg_rect()        filled rectangle (kernel SYS_FILLRECT)
 *    lg_circle_fill() filled circle via integer sqrt, no FPU needed
 *    lg_char()/lg_text()/lg_text_uint()
 *                     5x7 bitmap font with integer scaling — in-game HUD
 *                     (scores, lives) WITHOUT any new syscall. The kernel
 *                     console font is not reachable from userland yet.
 *    lg_key()         non-blocking keyboard poll (SYS_POLLKEY)
 *    lg_mouse()       absolute mouse position + buttons (SYS_MOUSE)
 *    lg_sfx()         non-blocking timed beep into the kernel audio ring
 *                     (SYS_SNDBEEP) — melodies are just a series of calls
 *    lg_every()       fixed-timestep helper ("run physics every N ticks")
 *    lg_srand()/lg_rand()/lg_rand_range()
 *                     xorshift32 RNG seeded from the timer
 *    lg_overlap()     AABB collision test
 *    lg_finish()      silence speaker + blank screen before exit text
 *
 *  HOSTED BUILDS ONLY. mtcc (the in-OS compiler) has no preprocessor and no
 *  #include, so in-OS C programs keep using the Morph.h names directly as
 *  compiler built-ins; Libgame targets .mrp games built with the hosted
 *  toolchain via mrp_pack.py (which adds this directory to -I).
 *
 *  Entry-point pattern for a Libgame game (see the games folder):
 *      #include "libgame.h"
 *      static void game_main(void);
 *      extern "C" __attribute__((section(".start")))
 *      void _start(void* legacy_api) { (void)legacy_api; game_main(); }
 *      static void game_main(void) { ... }
 *
 *  All state is `static` (single translation unit, zero heap): the MRP arena
 *  has no per-allocation free, so games avoid malloc entirely.
 * ============================================================================
 */

#ifndef LIBGAME_H
#define LIBGAME_H

#include "Morph.h"

/* ============================================================
 *  Framebuffer context
 * ============================================================ */

/* Probed once by lg_init(); the whole game reads geometry from here. */
static morph_fbinfo_t lg_fb;

/* Probe the framebuffer and verify the game-capable mode (32bpp linear).
 * Returns 0 on success, -1 if there is no usable graphics mode — callers
 * should print a message and exit(1), exactly like the snake reference. */
static inline int lg_init(void) {
    if (fb_info(&lg_fb) != 0) return -1;
    if (!lg_fb.avail || lg_fb.bpp != 32) return -1;
    return 0;
}

/* Screen geometry shortcuts. */
#define LG_W ((int)lg_fb.width)
#define LG_H ((int)lg_fb.height)

/* ============================================================
 *  Palette helpers (0x00RRGGBB)
 * ============================================================ */

#define LG_RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

#define LG_BLACK   LG_RGB(0x00, 0x00, 0x00)
#define LG_WHITE   LG_RGB(0xff, 0xff, 0xff)
#define LG_GREY    LG_RGB(0x90, 0x90, 0x90)
#define LG_RED     LG_RGB(0xff, 0x44, 0x44)
#define LG_ORANGE  LG_RGB(0xff, 0xa0, 0x20)
#define LG_YELLOW  LG_RGB(0xff, 0xe0, 0x40)
#define LG_GREEN   LG_RGB(0x22, 0xcc, 0x44)
#define LG_CYAN    LG_RGB(0x40, 0xc8, 0xdc)
#define LG_BLUE    LG_RGB(0x60, 0x80, 0xff)
#define LG_MAGENTA LG_RGB(0xe0, 0x50, 0xe0)

/* ============================================================
 *  Drawing primitives
 * ============================================================ */

/* Single pixel (kernel clips out-of-range coordinates). */
static inline void lg_pixel(int x, int y, uint32_t color) {
    (void)put_pixel(x, y, color);
}

/* Filled rectangle (kernel clips; w/h are packed into 16-bit halves of the
 * syscall ABI, so keep 0 <= x,y,w,h < 65536 — true for every sane game). */
static inline void lg_rect(int x, int y, int w, int h, uint32_t color) {
    (void)fill_rect(x, y, w, h, color);
}

/* Clear the whole screen to one color. */
static inline void lg_clear(uint32_t color) {
    lg_rect(0, 0, LG_W, LG_H, color);
}

/* floor(sqrt(n)) for 0 <= n <= 0x3fffffff — classic digit-by-digit
 * integer square root. No FPU, no doubles, exact for our small radii. */
static inline int lg_isqrt(int n) {
    if (n <= 0) return 0;
    int r = 0;
    int bit = 1 << 30;                 /* highest power of four <= 2^30 */
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else               { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

/* Filled circle, scanline style: one lg_rect per row. Radius is clamped
 * to 512 so r*r can never overflow int arithmetic. A ball erase+redraw
 * at 30 Hz costs ~130 fill_rects/s — negligible next to the kernel's
 * fill_rect cost (a row memset). */
static inline void lg_circle_fill(int cx, int cy, int r, uint32_t color) {
    if (r < 0) return;
    if (r > 512) r = 512;
    for (int dy = -r; dy <= r; dy++) {
        int hw = lg_isqrt(r * r - dy * dy);
        lg_rect(cx - hw, cy + dy, hw * 2 + 1, 1, color);
    }
}

/* ============================================================
 *  5x7 bitmap font (in-game text without a text syscall)
 * ------------------------------------------------------------
 *  Each glyph is 5 column-bytes, bit 0 = top row, bit 6 = bottom row.
 *  Supported set (enough for HUDs): space ! - . / : ? 0-9 A-Z.
 *  Lowercase maps to uppercase; anything unknown renders as space.
 *
 *  Geometry: glyph box 5x7, advance 6*scale (1px inter-glyph gap,
 *  scaled). scale=1 -> 5x7 pixels, scale=2 -> 10x14, etc.
 * ============================================================ */

static const char LG_CHARS[] =
    " !-./:?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static const uint8_t LG_GLYPHS[sizeof(LG_CHARS) - 1][5] = {
    /* ' ' */ { 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* '!' */ { 0x00, 0x00, 0x5F, 0x00, 0x00 },
    /* '-' */ { 0x08, 0x08, 0x08, 0x08, 0x08 },
    /* '.' */ { 0x00, 0x60, 0x60, 0x00, 0x00 },
    /* '/' */ { 0x20, 0x10, 0x08, 0x04, 0x02 },
    /* ':' */ { 0x00, 0x36, 0x36, 0x00, 0x00 },
    /* '?' */ { 0x02, 0x01, 0x51, 0x09, 0x06 },
    /* '0' */ { 0x3E, 0x51, 0x49, 0x45, 0x3E },
    /* '1' */ { 0x00, 0x42, 0x7F, 0x40, 0x00 },
    /* '2' */ { 0x42, 0x61, 0x51, 0x49, 0x46 },
    /* '3' */ { 0x21, 0x41, 0x45, 0x4B, 0x31 },
    /* '4' */ { 0x18, 0x14, 0x12, 0x7F, 0x10 },
    /* '5' */ { 0x27, 0x45, 0x45, 0x45, 0x39 },
    /* '6' */ { 0x3C, 0x4A, 0x49, 0x49, 0x30 },
    /* '7' */ { 0x01, 0x71, 0x09, 0x05, 0x03 },
    /* '8' */ { 0x36, 0x49, 0x49, 0x49, 0x36 },
    /* '9' */ { 0x06, 0x49, 0x49, 0x29, 0x1E },
    /* 'A' */ { 0x7E, 0x11, 0x11, 0x11, 0x7E },
    /* 'B' */ { 0x7F, 0x49, 0x49, 0x49, 0x36 },
    /* 'C' */ { 0x3E, 0x41, 0x41, 0x41, 0x22 },
    /* 'D' */ { 0x7F, 0x41, 0x41, 0x22, 0x1C },
    /* 'E' */ { 0x7F, 0x49, 0x49, 0x49, 0x41 },
    /* 'F' */ { 0x7F, 0x09, 0x09, 0x09, 0x01 },
    /* 'G' */ { 0x3E, 0x41, 0x49, 0x49, 0x7A },
    /* 'H' */ { 0x7F, 0x08, 0x08, 0x08, 0x7F },
    /* 'I' */ { 0x00, 0x41, 0x7F, 0x41, 0x00 },
    /* 'J' */ { 0x20, 0x40, 0x41, 0x3F, 0x01 },
    /* 'K' */ { 0x7F, 0x08, 0x14, 0x22, 0x41 },
    /* 'L' */ { 0x7F, 0x40, 0x40, 0x40, 0x40 },
    /* 'M' */ { 0x7F, 0x02, 0x0C, 0x02, 0x7F },
    /* 'N' */ { 0x7F, 0x04, 0x08, 0x10, 0x7F },
    /* 'O' */ { 0x3E, 0x41, 0x41, 0x41, 0x3E },
    /* 'P' */ { 0x7F, 0x09, 0x09, 0x09, 0x06 },
    /* 'Q' */ { 0x3E, 0x41, 0x51, 0x21, 0x5E },
    /* 'R' */ { 0x7F, 0x09, 0x16, 0x22, 0x41 },
    /* 'S' */ { 0x46, 0x49, 0x49, 0x49, 0x31 },
    /* 'T' */ { 0x01, 0x01, 0x7F, 0x01, 0x01 },
    /* 'U' */ { 0x3F, 0x40, 0x40, 0x40, 0x3F },
    /* 'V' */ { 0x1F, 0x20, 0x40, 0x20, 0x1F },
    /* 'W' */ { 0x3F, 0x40, 0x38, 0x40, 0x3F },
    /* 'X' */ { 0x63, 0x14, 0x08, 0x14, 0x63 },
    /* 'Y' */ { 0x07, 0x08, 0x70, 0x08, 0x07 },
    /* 'Z' */ { 0x61, 0x51, 0x49, 0x45, 0x43 },
};

/* Glyph table index for `c` (uppercase-folded, space fallback). */
static inline int lg_glyph_index(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (int i = 0; LG_CHARS[i]; i++) {
        if (LG_CHARS[i] == c) return i;
    }
    return 0;                                   /* unknown -> space */
}

/* Draw one character at (x, y) with the given integer scale. */
static inline void lg_char(int x, int y, char c, int scale, uint32_t color) {
    if (scale < 1) scale = 1;
    const uint8_t* g = LG_GLYPHS[lg_glyph_index(c)];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (uint8_t)(1u << row)) {
                if (scale == 1) {
                    lg_pixel(x + col, y + row, color);
                } else {
                    lg_rect(x + col * scale, y + row * scale,
                            scale, scale, color);
                }
            }
        }
    }
}

/* Draw a NUL-terminated string. Returns the x just past the last glyph
 * (handy for chaining lg_text_uint right after a label). */
static inline int lg_text(int x, int y, const char* s, int scale,
                          uint32_t color) {
    if (scale < 1) scale = 1;
    int cx = x;
    for (; *s; s++) {
        lg_char(cx, y, *s, scale, color);
        cx += 6 * scale;
    }
    return cx;
}

/* Draw an unsigned 32-bit value in decimal. Returns the x just past the
 * last digit. */
static inline int lg_text_uint(int x, int y, uint32_t v, int scale,
                               uint32_t color) {
    char buf[11];                               /* 4294967295 + NUL */
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v);
    return lg_text(x, y, &buf[i], scale, color);
}

/* Centered helper: returns the x where a string of `len` glyphs would
 * start to be horizontally centered on the screen. */
static inline int lg_center_x(const char* s, int scale) {
    int n = 0;
    while (s[n]) n++;
    return (LG_W - n * 6 * scale) / 2;
}

/* ============================================================
 *  Input
 * ============================================================ */

/* Non-blocking key poll: 0 = none this frame, >0 ASCII, <0 special
 * (MORPH_KEY_UP etc.). Drain in a `while ((k = lg_key()) != 0)` loop. */
static inline int lg_key(void) {
    return pollkey();
}

/* Absolute mouse state (position clamped to screen, button bits). */
static inline int lg_mouse(morph_mouse_t* st) {
    return mouse_state(st);
}

/* 1 while ANY mouse button is held (convenient "fire/launch" check). */
static inline int lg_button(void) {
    morph_mouse_t st;
    if (mouse_state(&st) != 0) return 0;
    return st.buttons != 0;
}

/* ============================================================
 *  Audio — thin layer over the kernel note ring (SYS_SNDBEEP)
 * ============================================================ */

/* Queue a timed note; returns snd_beep()'s value (0 ok, -9 queue full,
 * -6 bad args). Melodies = a burst of lg_sfx() calls, no sleeping. */
static inline int lg_sfx(uint32_t freq, uint32_t ms) {
    return snd_beep(freq, ms);
}

/* Small convenience scale for pitch-per-row brick tables etc.
 * C4 E4 G4 A4 C5 E5 G5 C6 (Hz, equal-tempered, rounded). */
#define LG_N_C4   262u
#define LG_N_E4   330u
#define LG_N_G4   392u
#define LG_N_A4   440u
#define LG_N_C5   523u
#define LG_N_E5   659u
#define LG_N_G5   784u
#define LG_N_C6   1047u

/* ============================================================
 *  Time — fixed-timestep helper
 * ============================================================
 *  The kernel timer ticks at 100 Hz, so period is in 10 ms units:
 *  lg_every(&last, 3) fires ~33 times per second (snake uses 12
 *  for its ~8 steps/s logic tick). Unsigned wraparound of the
 *  tick counter is handled by the subtraction comparison.
 */
static inline uint32_t lg_ticks(void) {
    return gettick();
}

static inline int lg_every(uint32_t* last, uint32_t period) {
    uint32_t now = gettick();
    if (now - *last >= period) {
        *last = now;
        return 1;
    }
    return 0;
}

static inline void lg_sleep(uint32_t ms) {
    sleep_ms(ms);
}

/* ============================================================
 *  RNG — xorshift32 (deterministic, seedable from the timer)
 * ============================================================ */

static uint32_t lg_rng_state = 0x1234abcdu;     /* never zero */

static inline void lg_srand(uint32_t seed) {
    lg_rng_state = seed | 1u;                   /* avoid the fixed point */
}

static inline uint32_t lg_rand(void) {
    lg_rng_state ^= lg_rng_state << 13;
    lg_rng_state ^= lg_rng_state >> 17;
    lg_rng_state ^= lg_rng_state << 5;
    return lg_rng_state;
}

/* Uniform in [lo, hi] inclusive (hi >= lo, hi - lo < 2^31). */
static inline uint32_t lg_rand_range(uint32_t lo, uint32_t hi) {
    return lo + lg_rand() % (hi - lo + 1u);
}

/* ============================================================
 *  Collision — axis-aligned bounding boxes
 * ============================================================ */

static inline int lg_overlap(int x1, int y1, int w1, int h1,
                             int x2, int y2, int w2, int h2) {
    return x1 < x2 + w2 && x2 < x1 + w1 &&
           y1 < y2 + h2 && y2 < y1 + h1;
}

/* ============================================================
 *  Teardown
 * ============================================================ */

/* Silence the speaker and blank the canvas so the exit text printed
 * right after (kernel console font) is readable on a black screen.
 * Games must call this before print()/exit(). */
static inline void lg_finish(void) {
    spk_silence();
    lg_clear(0x00000000u);
}

#endif /* LIBGAME_H */
