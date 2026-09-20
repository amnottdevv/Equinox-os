#include "header/stdio.h"
#include "header/itoa_atoi.h"
#include "header/libstring.h"   /* memcpy — canvas snapshot/restore (Phase A.1) */
#include "header/vesa.h"
#include "header/font8x16.h"
#include "header/color.h"
#include "header/serial.h"
#include "header/task.h"        /* Phase A.1: per-console canvas lives in the
                                       user physical pool (task_user_phys_alloc) */
#include "header/paging.h"      /* v0.3: kernel dir for the canvas copy */
#include <stdint.h>
#include <stdarg.h>

/* v0.3: IRQ save/restore for the canvas copy critical section
 * (CR3 is switched — IRQ0 must not preempt in between). */
static inline uint32_t con_irq_save(void) {
    uint32_t flags;
    asm volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void con_irq_restore(uint32_t flags) {
    asm volatile("pushl %0\n\tpopfl" :: "r"(flags) : "memory");
}

// ============================================================
//  DISPLAY BACKEND SELECTION
//  After init_display() is called, all text output goes through
//  the active backend.  If VESA is available we render glyphs
//  into the linear framebuffer; otherwise we fall back to the
//  legacy VGA text buffer at 0xB8000.
// ============================================================

static int use_vesa  = 0;   // 0 = VGA text mode, 1 = VESA framebuffer
static int term_cols = 80;
static int term_rows = 25;

/* ============================================================
 *  MULTI-CONSOLE (Phase A — multitasking)
 *  ------------------------------------------------------------
 *  All console state that used to be module-static is now a field
 *  of a PER-CONSOLE struct TermCon. The old identifiers below are
 *  macros pointing into g_out, so the legacy backend code
 *  (put_char_vesa etc.) compiles unchanged.
 *
 *    g_out : OUTPUT console — owned by the running task (a
 *            background task's printf updates its own cells).
 *    g_act : ACTIVE console — the only one rendered to the screen
 *            + receiving keyboard focus. F2 = console_activate.
 *
 *  Render rule: the cell mirror is ALWAYS updated (per-console text
 *  mirror); pixel/VGA writes happen only when g_out == g_act.
 *
 *  Phase A.1 (graphics isolation): each console additionally owns a
 *  PIXEL canvas snapshot. When focus leaves a console on which a
 *  user program drew pixels (fillrect / putpixel / blit), the
 *  framebuffer is saved into that console's canvas; when focus
 *  returns, it is restored. Together with the scheduler's draw gate
 *  (task_console_draw_gate) this keeps a frozen game on its own
 *  terminal instead of painting over the shell the user is typing
 *  into (the "snake on terminal 2" bug).
 * ============================================================ */
#define N_CONSOLES    8
#define TERM_MAX_COLS 160
#define TERM_MAX_ROWS 64
#define TERM_MAX_CELLS (TERM_MAX_COLS * TERM_MAX_ROWS)

struct TermCell {
    uint8_t ch;
    uint8_t attr;    /* VGA 16-color fg|bg<<4 (approximation for re-render) */
};

struct TermCon {
    int      used;
    int      row, col;
    uint8_t  attr;            /* current_color */
    uint32_t fg_rgb, bg_rgb;
    int      scroll_enabled;
    int      curvis;
    int      prev_row, prev_col;
    int      onscreen;
    uint32_t cell_fg, cell_bg;
    struct TermCell cells[TERM_MAX_CELLS];
    /* --- Phase A.1: pixel canvas snapshot (VESA only) --- */
    int      has_canvas;      /* pixels were drawn while active */
    uint8_t* canvas;          /* framebuffer copy (user phys pool, VMA == phys) */
};

static struct TermCon tcons[N_CONSOLES];
static struct TermCon* g_out = &tcons[0];
static struct TermCon* g_act = &tcons[0];

/* Legacy identifiers -> g_out fields (the old backend code compiles unchanged). */
#define cursor_row          (g_out->row)
#define cursor_col          (g_out->col)
#define current_color       (g_out->attr)
#define term_fg_rgb         (g_out->fg_rgb)
#define term_bg_rgb         (g_out->bg_rgb)
#define term_scroll_enabled (g_out->scroll_enabled)
#define cursor_visible      (g_out->curvis)
#define prev_cursor_row     (g_out->prev_row)
#define prev_cursor_col     (g_out->prev_col)
#define cursor_onscreen     (g_out->onscreen)
#define cursor_cell_fg      (g_out->cell_fg)
#define cursor_cell_bg      (g_out->cell_bg)

/* Store one cell into the console mirror (always; rendering is separate). */
static void con_cell_put(struct TermCon* c, int row, int col, uint8_t ch) {
    if (row < 0 || row >= term_rows || row >= TERM_MAX_ROWS) return;
    if (col < 0 || col >= term_cols || col >= TERM_MAX_COLS) return;
    c->cells[row * TERM_MAX_COLS + col].ch   = ch;
    c->cells[row * TERM_MAX_COLS + col].attr = current_color;
}

/* Default state of a fresh console (used by create + init_display). */
static void con_defaults(struct TermCon* c) {
    c->used = 1;
    c->row = 0; c->col = 0;
    c->attr = 0x0F;
    c->fg_rgb = 0xFFFFFF;
    c->bg_rgb = 0x000000;
    c->scroll_enabled = 1;
    c->curvis = 1;
    c->prev_row = -1; c->prev_col = -1;
    c->onscreen = 0;
    c->cell_fg = 0xFFFFFF; c->cell_bg = 0x000000;
    c->has_canvas = 0;
    c->canvas = NULL;
    for (int i = 0; i < TERM_MAX_CELLS; i++) {
        c->cells[i].ch = ' ';
        c->cells[i].attr = 0x0F;
    }
}

/* ============================================================
 *  FIX(R1) — TUI SCROLL LOCK
 *  ------------------------------------------------------------
 *  put_char() used to do an EAGER SCROLL: as soon as the last glyph
 *  of the rightmost column of the bottom row was drawn, the wrap
 *  immediately raised cursor_row to term_rows and the WHOLE SCREEN
 *  scrolled up one row. For the shell that is exactly the desired
 *  behavior, but for a full-screen TUI app (the code editor!) that
 *  draws a full-width status bar on the LAST ROW, this is a disaster:
 *  every status bar render = the screen shifts one row. That is the
 *  root of the "typed text appears briefly then vanishes / turns
 *  black" + "typing behaves like pressing Enter" bugs (rows shifted
 *  on every keystroke).
 *
 *  term_set_scroll(0) disables auto-scroll (TUI mode): wrapping on
 *  the last row merely clamps the cursor to (term_rows-1, 0),
 *  it does NOT shift the screen contents. Full-screen apps must use
 *  this; the shell keeps the default (scrolling active).
 * ============================================================ */

/* ============================================================
 *  FIX(R2) — QUIET CURSOR + ERASE WITH THE CORRECT COLOR
 *  ------------------------------------------------------------
 *  Old problems:
 *   1. update_cursor_vesa() erased the old cursor underline using
 *      the CURRENT term_bg_rgb. If set_color() had changed since
 *      that underline was drawn (e.g. the editor renders a blue
 *      title bar THEN goes back to drawing text), the erase used
 *      the wrong color -> colored block residue in the text area.
 *   2. Every put_char() drew a "marching" cursor underline across
 *      the screen during bulk renders -> artifacts.
 *   3. The timer ISR (blink) could draw at a half-finished
 *      position while the main render was still running.
 *
 *  Solution:
 *   - the underline cell's fg/bg colors are REMEMBERED when drawn
 *     (cursor_cell_fg/bg) and reused when erasing/blinking.
 *   - term_set_cursor_visible(0) turns off ALL cursor drawing
 *     (including from the blink ISR) during bulk renders; the app
 *     turns it back on ONCE at the final position.
 * ============================================================ */

// ============================================================
//  VGA 16-color → RGB palette (matching real VGA colors)
// ============================================================
static const uint32_t vga_palette[16] = {
    0x000000, // 0  black
    0x0000AA, // 1  blue
    0x00AA00, // 2  green
    0x00AAAA, // 3  cyan
    0xAA0000, // 4  red
    0xAA00AA, // 5  magenta
    0xAA5500, // 6  brown
    0xAAAAAA, // 7  light grey
    0x555555, // 8  dark grey
    0x5555FF, // 9  light blue
    0x55FF55, // 10 light green
    0x55FFFF, // 11 light cyan
    0xFF5555, // 12 light red
    0xFF55FF, // 13 light magenta
    0xFFFF55, // 14 yellow (light brown)
    0xFFFFFF, // 15 white
};

// fg/bg 32-bit RGB is now per-console (TermCon fields).

// ============================================================
//  VGA TEXT MODE BACKEND
//  (the cell mirror is ALWAYS updated; vga_buffer belongs to the active console only)
// ============================================================
static volatile uint16_t* vga_buffer = (uint16_t*)0xB8000;

/* Shift the cell mirror up by one row (any console). */
static void scroll_cells(struct TermCon* c) {
    for (int y = 0; y + 1 < term_rows && y + 1 < TERM_MAX_ROWS; y++) {
        for (int x = 0; x < term_cols && x < TERM_MAX_COLS; x++) {
            c->cells[y * TERM_MAX_COLS + x] = c->cells[(y + 1) * TERM_MAX_COLS + x];
        }
    }
    for (int x = 0; x < term_cols && x < TERM_MAX_COLS; x++) {
        c->cells[(term_rows - 1) * TERM_MAX_COLS + x].ch   = ' ';
        c->cells[(term_rows - 1) * TERM_MAX_COLS + x].attr = current_color;
    }
}

static void scroll_screen_vga(void) {
    if (cursor_row >= 25) {
        scroll_cells(g_out);
        if (g_out == g_act) {
            for (int y = 0; y < 24; y++)
                for (int x = 0; x < 80; x++)
                    vga_buffer[y * 80 + x] = vga_buffer[(y + 1) * 80 + x];
            for (int x = 0; x < 80; x++)
                vga_buffer[24 * 80 + x] = (current_color << 8) | ' ';
        }
        cursor_row = 24;
    }
}

static void vga_hide_crtc_cursor(void) {
    /* CRTC index 0x0A (cursor start) bit 5 = 1 -> hardware cursor OFF.
     * Used by term_set_cursor_visible(0) — once, not per char. */
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

static void vga_show_crtc_cursor(void) {
    /* Underline on the last 2 scanlines of the 16-scanline cell: start=14, end=15.
     * Both are written on show so start<=end always holds
     * (if start>end some VGAs suppress the cursor). */
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
}

static void update_cursor_vga(void) {
    if (!cursor_visible) return;          // hidden: do not touch the CRTC
    if (g_out != g_act) return;           // only the visible console sets the CRTC
    uint16_t pos = cursor_row * 80 + cursor_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void clear_screen_vga(void) {
    /* the cell mirror is always cleared (console g_out) */
    for (int i = 0; i < TERM_MAX_CELLS; i++) {
        g_out->cells[i].ch = ' ';
        g_out->cells[i].attr = current_color;
    }
    if (g_out == g_act) {
        for (int y = 0; y < 25; y++)
            for (int x = 0; x < 80; x++)
                vga_buffer[y * 80 + x] = (current_color << 8) | ' ';
    }
    cursor_row = 0;
    cursor_col = 0;
    update_cursor_vga();
}

/* Wrap after writing a glyph: full column -> next row.
 * FIX(R1): scroll only if term_scroll_enabled; otherwise the
 * cursor is clamped to the start of the last row (full-screen TUI mode). */
static void vga_wrap_after_char(void) {
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= 25) {
        if (term_scroll_enabled) scroll_screen_vga();
        else                     cursor_row = 24;   // TUI: do not scroll
    }
}

static void put_char_vga(char c) {
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
        if (cursor_row >= 25) {
            if (term_scroll_enabled) scroll_screen_vga();
            else                     cursor_row = 24;
        }
        if (cursor_visible) update_cursor_vga();
        return;
    }
    if (c == '\r') {                      // FIX(S6): CR = column 0, same row
        cursor_col = 0;
        if (cursor_visible) update_cursor_vga();
        return;
    }
    if (c == '\t') {                      // FIX(S6): TAB only moves the cursor
        cursor_col = (cursor_col + 8) & ~7;   // tab stops every 8 columns
        if (cursor_col >= 80) vga_wrap_after_char();
        if (cursor_visible) update_cursor_vga();
        return;
    }
    if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            con_cell_put(g_out, cursor_row, cursor_col, ' ');
            if (g_out == g_act)
                vga_buffer[cursor_row * 80 + cursor_col] = (current_color << 8) | ' ';
            if (cursor_visible) update_cursor_vga();
        }
        return;
    }
    con_cell_put(g_out, cursor_row, cursor_col, (uint8_t)c);
    if (g_out == g_act)
        vga_buffer[cursor_row * 80 + cursor_col] = (current_color << 8) | c;
    cursor_col++;
    if (cursor_col >= 80) {
        vga_wrap_after_char();
    }
    if (cursor_visible) update_cursor_vga();
}

// ============================================================
//  VESA FRAMEBUFFER BACKEND
//  Renders 8x16 bitmap glyphs into the linear framebuffer.
//  Terminal size = fb_width/8 columns x fb_height/16 rows.
//  For 1024x768 that's 128x48 — much more room than VGA 80x25.
// ============================================================

// The previous cursor position is now per-console (TermCon fields).

/*
 * Render one glyph at (row, col) using the 8x16 font.
 * Draws BOTH foreground and background pixels so overwriting
 * works correctly (no ghosting from previous characters).
 */
static void draw_glyph(int row, int col, uint8_t ch) {
    /* FIX(S1): draw via the bpp-aware vesa_draw_pixel() (16/24/32).
     * Previously only 32 & 24 bpp branches existed -> in 16bpp mode the
     * glyph was never drawn at all (INVISIBLE TEXT). vesa_draw_pixel
     * already handles the per-mode pixel layout + clipping. */
    uint16_t px = (uint16_t)(col * 8);   // pixel x
    uint16_t py = (uint16_t)(row * 16);  // pixel y

    for (int gy = 0; gy < 16; gy++) {
        uint8_t line = font8x16[ch][gy];
        for (int gx = 0; gx < 8; gx++) {
            // bit 7 = leftmost pixel, bit 0 = rightmost
            uint32_t color = (line & (1 << (7 - gx))) ? term_fg_rgb : term_bg_rgb;
            vesa_draw_pixel((uint16_t)(px + gx), (uint16_t)(py + gy), color);
        }
    }
}

/*
 * Draw the cursor as a solid underline (bottom 2 scanlines of cell).
 * Erases the old cursor by re-drawing the character that was under it.
 */
/* FIX(S2): the cursor is drawn per-pixel via vesa_draw_pixel() (bpp-aware).
 * Before: a hardcoded uint32 write per pixel -> in 16bpp it wrote 2x the
 * bytes per pixel (overflow into the neighboring cell / possibly past the
 * end of the framebuffer row), and in 24bpp the color was wrong. */
static void vesa_cursor_cell(int row, int col, uint32_t color) {
    uint16_t px = (uint16_t)(col * 8);
    uint16_t py = (uint16_t)(row * 16 + 14);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 8; x++)
            vesa_draw_pixel((uint16_t)(px + x), (uint16_t)(py + y), color);
}

// ============================================================
//  CURSOR BLINK — powered by the timer IRQ (100 Hz).
//  A 250 ms phase per state = a comfortable 2 Hz blink
//  (the right "refresh rate" for a shell: a live cursor, and no
//  screen flicker because only 8x2 pixels are redrawn).
//  The VGA text backend does not need this — its hardware cursor
//  blinks on its own.
// ============================================================
#define CURSOR_BLINK_TICKS 25   /* 100 Hz / 25 = 4 toggles/second = 2 Hz */
static volatile uint8_t  cursor_blink_on   = 1;
static volatile uint32_t cursor_blink_acc  = 0;

/* FIX(R2): erase the OLD cursor underline with the bg color
 * REMEMBERED when that underline was drawn — not the current
 * term_bg_rgb (which may have changed via set_color in between). */
static void hide_cursor_vesa(void) {
    if (cursor_onscreen && prev_cursor_row >= 0 && prev_cursor_col >= 0) {
        vesa_cursor_cell(prev_cursor_row, prev_cursor_col, cursor_cell_bg);
    }
    cursor_onscreen  = 0;
    prev_cursor_row  = -1;
    prev_cursor_col  = -1;
}

/* Called from the timer ISR (see timer.cpp). Redraws ONLY the
 * 8x2 pixel cursor area — cheap enough to call on every tick,
 * and it does not touch the rest of the screen (no shell flicker).
 * FIX(R2): honor cursor_visible (a bulk render is in progress)
 * and use the remembered cell colors, not the current global colors. */
void term_cursor_tick(void) {
    if (!use_vesa || !cursor_visible || !cursor_onscreen) return;
    if (g_out != g_act) return;           /* blink only the visible console */
    if (g_act->has_canvas) return;        /* v0.3: no cursor on canvas */
    if (++cursor_blink_acc < CURSOR_BLINK_TICKS) return;
    cursor_blink_acc = 0;
    cursor_blink_on ^= 1;
    if (cursor_blink_on) vesa_cursor_cell(cursor_row, cursor_col, cursor_cell_fg);
    else                 vesa_cursor_cell(cursor_row, cursor_col, cursor_cell_bg);
}

static void update_cursor_vesa(void) {
    if (!cursor_visible) return;          // quiet mode: draw nothing
    if (g_out != g_act) return;           // only the visible console
    if (g_out->has_canvas) return;        // v0.3: canvas mode — the
                                          // graphics program owns the
                                          // screen; the cursor underline
                                          // must not cut into its pixels

    // --- Erase old cursor (bg color at draw time — FIX R2) ---
    if (cursor_onscreen && prev_cursor_row >= 0 && prev_cursor_col >= 0) {
        vesa_cursor_cell(prev_cursor_row, prev_cursor_col, cursor_cell_bg);
    }

    // --- Draw new cursor (underline on the last 2 scanlines) ---
    cursor_cell_fg = term_fg_rgb;
    cursor_cell_bg = term_bg_rgb;
    vesa_cursor_cell(cursor_row, cursor_col, term_fg_rgb);
    cursor_onscreen = 1;

    prev_cursor_row = cursor_row;
    prev_cursor_col = cursor_col;

    /* Reset the blink phase: the cursor is immediately VISIBLE after
     * moving/typing — instant feedback like a real terminal. */
    cursor_blink_acc = 0;
    cursor_blink_on  = 1;
}

static void clear_screen_vesa(void) {
    /* the cell mirror is always cleared */
    for (int i = 0; i < TERM_MAX_CELLS; i++) {
        g_out->cells[i].ch = ' ';
        g_out->cells[i].attr = current_color;
    }
    if (g_out == g_act) {
        uint16_t width  = vesa_get_width();
        uint16_t height = vesa_get_height();
        for (uint16_t y = 0; y < height; y++)
            for (uint16_t x = 0; x < width; x++)
                vesa_draw_pixel(x, y, term_bg_rgb);
    }
    cursor_row = 0;
    cursor_col = 0;
    prev_cursor_row = -1;
    prev_cursor_col = -1;
    cursor_onscreen  = 0;    // FIX(R2): no underline active
}

static void scroll_screen_vesa(void) {
    if (cursor_row >= term_rows) {
        scroll_cells(g_out);              /* the cell mirror always shifts */
        if (g_out == g_act && !g_out->has_canvas) {
            /* v0.3: in canvas mode the pixel shift is SKIPPED —
             * the mirror-only scroll keeps the program's pixels. */
            uint32_t fb_addr = vesa_get_framebuffer();
            uint16_t pitch   = vesa_get_pitch();
            uint16_t fb_h    = vesa_get_height();

            int scroll_px = 16;  // one text row in pixels

            // Move framebuffer up by 16 pixel rows (dword-at-a-time)
            uint32_t* dst = (uint32_t*)fb_addr;
            uint32_t* src = (uint32_t*)((uint8_t*)fb_addr + (uint32_t)scroll_px * pitch);
            uint32_t total_dwords = ((uint32_t)(fb_h - scroll_px) * pitch) / 4;
            for (uint32_t i = 0; i < total_dwords; i++)
                dst[i] = src[i];

            for (uint16_t y = (uint16_t)(fb_h - scroll_px); y < fb_h; y++)
                for (uint16_t x = 0; x < vesa_get_width(); x++)
                    vesa_draw_pixel(x, y, term_bg_rgb);
        }
        cursor_row = term_rows - 1;
    }
}

/* Wrap after a glyph: full column -> next row.
 * FIX(R1): scroll only if term_scroll_enabled (shell mode);
 * in TUI mode (editor etc.) the cursor is clamped — the screen does NOT shift.
 * This is the main fix for the "text appears briefly then vanishes" bug:
 * ed_render_status() draws a full-width status bar on the LAST
 * row; before, its wrap triggered scroll_screen_vesa(),
 * so the entire editor content moved up one row on EVERY KEYSTROKE. */
static void vesa_wrap_after_char(void) {
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= term_rows) {
        if (term_scroll_enabled) scroll_screen_vesa();
        else                     cursor_row = term_rows - 1;  // TUI: clamp
    }
}

static void put_char_vesa(char c) {
    /* Phase A.1 consistency rule: a text write on an UNFOCUSED console
     * would update the cell mirror only — the console's pixel canvas
     * snapshot (if any) would go stale. Drop the canvas so the next
     * activation does a clean full text re-render instead of showing
     * an outdated picture. (Frozen games never hit this: the draw gate
     * suspends them before they can print.) */
    if (g_out != g_act && g_out->has_canvas) {
        console_canvas_invalidate((int)(g_out - tcons));
    }
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
        if (cursor_row >= term_rows) {
            if (term_scroll_enabled) scroll_screen_vesa();
            else                     cursor_row = term_rows - 1;
        }
        if (cursor_visible) update_cursor_vesa();
        return;
    }
    if (c == '\r') {                      // FIX(S6): CR = column 0, same row
        cursor_col = 0;
        if (cursor_visible) update_cursor_vesa();
        return;
    }
    if (c == '\t') {                      // FIX(S6): TAB only moves the cursor
        cursor_col = (cursor_col + 8) & ~7;   // tab stops every 8 columns
        if (cursor_col >= term_cols) vesa_wrap_after_char();
        if (cursor_visible) update_cursor_vesa();
        return;
    }
    if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            // Overwrite with space (clears the cell)
            con_cell_put(g_out, cursor_row, cursor_col, ' ');
            if (g_out == g_act && !g_out->has_canvas)
                draw_glyph(cursor_row, cursor_col, ' ');
            if (cursor_visible) update_cursor_vesa();
        }
        return;
    }

    con_cell_put(g_out, cursor_row, cursor_col, (uint8_t)c);
    if (g_out == g_act && !g_out->has_canvas)   /* v0.3: mirror-only */
        draw_glyph(cursor_row, cursor_col, (uint8_t)c);  /* while canvas  */
    cursor_col++;
    if (cursor_col >= term_cols) {
        vesa_wrap_after_char();
    }
    if (cursor_visible) update_cursor_vesa();
}

/* ============================================================
 *  RGB -> nearest VGA 16-color (for the cell attr mirror, so the
 *  re-render after a console switch keeps a similar color).
 * ============================================================ */
static uint8_t rgb_nearest16(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    uint32_t best = 0, bestd = 0xFFFFFFFF;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t pr = vga_palette[i] >> 16, pg = (vga_palette[i] >> 8) & 0xFF,
                 pb = vga_palette[i] & 0xFF;
        uint32_t dr = pr > r ? pr - r : r - pr;
        uint32_t dg = pg > g ? pg - g : g - pg;
        uint32_t db = pb > b ? pb - b : b - pb;
        uint32_t d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return (uint8_t)best;
}

/* ============================================================
 *  CONSOLE API (Phase A — used by task.cpp / idt.cpp)
 * ============================================================ */
int console_create(void) {
    for (int i = 0; i < N_CONSOLES; i++) {
        if (!tcons[i].used) {
            con_defaults(&tcons[i]);
            return i;
        }
    }
    return -1;
}

void console_free(int id) {
    if (id < 0 || id >= N_CONSOLES) return;
    if (&tcons[id] == g_act) return;      /* the active console is never freed */
    console_canvas_invalidate(id);
    tcons[id].used = 0;
}

int console_active_id(void) {
    return (int)(g_act - tcons);
}

/* Phase C: number of consoles in use (bounds-check for `switch n`). */
int console_count(void) {
    int n = 0;
    for (int i = 0; i < N_CONSOLES; i++)
        if (tcons[i].used) n++;
    return n;
}

int console_next_used(int from, int dir) {
    if (from < 0) from = 0;
    if (from >= N_CONSOLES) from = N_CONSOLES - 1;
    for (int k = 1; k <= N_CONSOLES; k++) {
        int idx = (from + dir * k + N_CONSOLES * 8) % N_CONSOLES;
        if (tcons[idx].used) return idx;
    }
    return -1;
}

/* ---------- Phase A.1: per-console pixel canvas ---------- */

static uint32_t con_canvas_bytes(void) {
    return (uint32_t)vesa_get_pitch() * (uint32_t)vesa_get_height();
}

static void con_render_full(struct TermCon* c);   /* v0.3 */

/* Mark the ACTIVE console as having pixels on screen (called by the
 * graphics syscalls after the focus gate passes).
 * v0.3 (FR-17/18): the FIRST draw takes a SNAPSHOT of the text
 * screen (restored/re-rendered when the program exits) and CLEARS the
 * framebuffer — graphics programs start on a clean black canvas
 * instead of painting over the boot log. Text output while the
 * canvas is live updates the cell MIRROR only (serial still mirrors):
 * glyphs and the cursor must not trash the program's pixels. */
void console_canvas_mark(void) {
    if (!use_vesa || !g_act) return;
    struct TermCon* c = g_act;
    if (c->has_canvas) return;              /* already in canvas mode */
    if (!c->canvas)
        c->canvas = (uint8_t*)(uintptr_t)
                    task_user_phys_alloc(con_canvas_bytes());
    if (c->canvas) {
        /* snapshot + clear under the KERNEL directory (irq off, CR3
         * saved/restored) — same discipline as the console-switch
         * save path: the identity VMA is guaranteed there. */
        uint32_t f = con_irq_save();
        uint32_t saved_cr3 = task_read_cr3();
        task_load_cr3(paging_kernel_dir());
        memcpy(c->canvas,
               (const void*)(uintptr_t)vesa_get_framebuffer(),
               (size_t)con_canvas_bytes());
        memset((void*)(uintptr_t)vesa_get_framebuffer(), 0,
               (size_t)con_canvas_bytes());
        task_load_cr3((uint32_t*)(uintptr_t)saved_cr3);
        con_irq_restore(f);
    }
    /* even without a snapshot buffer: the program owns the screen —
     * text goes mirror-only until the program exits. */
    c->has_canvas = 1;
    c->onscreen = 0;                        /* force re-render at exit */
    cursor_onscreen = 0;                    /* stale underline is gone  */
    prev_cursor_row = -1;
    prev_cursor_col = -1;
}

/* Drop the canvas of a console (a graphics program exited / the
 * console is being freed) and return the buffer to the physical
 * pool — the memory-cleanup half of the isolation fix.
 * v0.3 (FR-17/18): when the ACTIVE console leaves canvas mode
 * (program exit), the text screen is RE-RENDERED from the cell mirror
 * — which the program's own printf output kept up to date — instead
 * of restoring a stale snapshot. */
void console_canvas_invalidate(int console_id) {
    if (console_id < 0 || console_id >= N_CONSOLES) return;
    struct TermCon* c = &tcons[console_id];
    int was_live = (c == g_act && c->has_canvas && use_vesa);
    if (c->canvas) {
        task_user_phys_free((uint32_t)(uintptr_t)c->canvas,
                            con_canvas_bytes());
        c->canvas = NULL;
    }
    c->has_canvas = 0;
    if (was_live)
        con_render_full(c);          /* text back on screen, clean */
}

/* Full re-render of a console onto the screen (called on activate).
 * Borrows g_out so the render helpers target the requested console. */
static void con_render_full(struct TermCon* c) {
    struct TermCon* save_out = g_out;
    g_out = c;
    if (use_vesa) {
        /* v10.11 perf fix: one hardware-friendly fill_rect instead of
         * ~1M single-pixel writes (a full-screen clear used to cost
         * tens of milliseconds on every F1/F2 switch). */
        vesa_fill_rect(0, 0, vesa_get_width(), vesa_get_height(), 0x000000);
        uint32_t save_fg = term_fg_rgb;
        for (int r = 0; r < term_rows && r < TERM_MAX_ROWS; r++) {
            for (int q = 0; q < term_cols && q < TERM_MAX_COLS; q++) {
                struct TermCell* cell = &c->cells[r * TERM_MAX_COLS + q];
                if (cell->ch == ' ' || cell->ch == 0) continue;
                term_fg_rgb = vga_palette[cell->attr & 0x0F];
                draw_glyph(r, q, cell->ch);
            }
        }
        term_fg_rgb = save_fg;
        c->onscreen = 0;
        c->prev_row = -1;
        c->prev_col = -1;
        if (c->curvis) update_cursor_vesa();
    } else {
        for (int y = 0; y < 25; y++)
            for (int x = 0; x < 80; x++) {
                struct TermCell* cell = &c->cells[y * TERM_MAX_COLS + x];
                vga_buffer[y * 80 + x] = ((uint16_t)cell->attr << 8) | cell->ch;
            }
        update_cursor_vga();
    }
    g_out = save_out;
}

void console_activate(int id) {
    if (id < 0 || id >= N_CONSOLES) return;
    if (!tcons[id].used) return;
    if (g_act == &tcons[id]) return;

    struct TermCon* old = g_act;
    g_act = &tcons[id];

    /* Phase A.1 — graphics console isolation on switch-away:
     * save the outgoing console's pixels (if it has a canvas) into
     * its snapshot buffer so another terminal's text does not
     * overwrite a frozen game's screen. Allocation from the user
     * physical pool can fail under memory pressure — then the
     * console degrades to plain text re-rendering.
     * v0.3 FIX (demand paging): the canvas buffer is a RAW
     * physical chunk accessed through its VMA==phys identity — but
     * with demand-paged task windows that VMA may be remapped in the
     * CURRENT task's directory (the switch can preempt any task).
     * The copy now runs under the KERNEL directory (irq off, CR3
     * saved/restored) so the identity mapping is guaranteed. */
    if (old->has_canvas && use_vesa) {
        if (!old->canvas)
            old->canvas = (uint8_t*)(uintptr_t)
                          task_user_phys_alloc(con_canvas_bytes());
        if (old->canvas) {
            uint32_t f = con_irq_save();
            uint32_t saved_cr3 = task_read_cr3();
            task_load_cr3(paging_kernel_dir());
            memcpy(old->canvas,
                   (const void*)(uintptr_t)vesa_get_framebuffer(),
                   (size_t)con_canvas_bytes());
            task_load_cr3((uint32_t*)(uintptr_t)saved_cr3);
            con_irq_restore(f);
        } else {
            old->has_canvas = 0;   /* no memory for a snapshot: text fallback */
        }
    }

    /* Phase A.1 — switch-in: restore the incoming console's frozen
     * pixels, or do a full text re-render when it has none.
     * v0.3: kernel-directory copy (see the save side above). */
    if (tcons[id].has_canvas && tcons[id].canvas && use_vesa) {
        uint32_t f = con_irq_save();
        uint32_t saved_cr3 = task_read_cr3();
        task_load_cr3(paging_kernel_dir());
        memcpy((void*)(uintptr_t)vesa_get_framebuffer(),
               (const void*)tcons[id].canvas,
               (size_t)con_canvas_bytes());
        task_load_cr3((uint32_t*)(uintptr_t)saved_cr3);
        con_irq_restore(f);
        /* cursor tracking restarts cleanly for the focused console */
        struct TermCon* save_out = g_out;
        g_out = &tcons[id];
        tcons[id].onscreen = 0;
        tcons[id].prev_row = -1;
        tcons[id].prev_col = -1;
        if (tcons[id].curvis) update_cursor_vesa();
        g_out = save_out;
    } else {
        con_render_full(g_act);
    }

    /* SIGTTOU-style resume: wake programs parked on this console by
     * the scheduler's graphics draw gate (they continue mid-syscall
     * and finish the draw they were suspended on). */
    task_wake_console(id);
}

void term_set_output(int id) {
    if (id < 0 || id >= N_CONSOLES || !tcons[id].used) {
        g_out = g_act;
        return;
    }
    g_out = &tcons[id];
}

void term_force_active_output(void) {
    g_out = g_act;
}

// ============================================================
//  PUBLIC API — dispatches to the active backend
// ============================================================

void init_display(void) {
    if (vesa_is_available()) {
        use_vesa  = 1;
        term_cols = (int)(vesa_get_width()  / 8);
        term_rows = (int)(vesa_get_height() / 16);
        // Clamp to sane minimums
        if (term_cols < 40) term_cols = 40;
        if (term_rows < 20) term_rows = 20;
        if (term_cols > TERM_MAX_COLS) term_cols = TERM_MAX_COLS;
        if (term_rows > TERM_MAX_ROWS) term_rows = TERM_MAX_ROWS;
        // Fill framebuffer with black
        vesa_fill_rect(0, 0, vesa_get_width(), vesa_get_height(), 0x000000);
    } else {
        use_vesa  = 0;
        term_cols = 80;
        term_rows = 25;
    }
    /* Console 0 = console boot. */
    con_defaults(&tcons[0]);
    g_out = g_act = &tcons[0];
}

// Active console size — used by panic.cpp to draw a correct
// full-width header in both VGA text (80) and VESA (e.g. 128).
int term_get_cols(void) { return term_cols; }
int term_get_rows(void) { return term_rows; }

// ===================== SCROLL LOCK (FIX R1) =================
// 1 = normal auto-scroll (shell). 0 = full-screen TUI mode:
// wrapping on the last row does NOT shift the screen (cursor clamped).
// Usage: the editor / a TUI calls term_set_scroll(0) on open
// and MUST call term_set_scroll(1) before returning to the shell.
void term_set_scroll(int enable) { term_scroll_enabled = enable ? 1 : 0; }
int  term_get_scroll(void)       { return term_scroll_enabled; }

// ===================== CURSOR VISIBLE (FIX R2) ==============
// 0 = turn off all cursor drawing (TUI bulk render: put_char /
// set_cursor_position / the blink ISR draw no underline).
// 1 = turn it back on + draw the underline at the current cursor
// position. A TUI app calls 0 before a bulk render, then 1
// ONCE after the final cursor position is in place.
void term_set_cursor_visible(int visible) {
    cursor_visible = visible ? 1 : 0;
    if (use_vesa) {
        if (!cursor_visible) hide_cursor_vesa();
        else                 update_cursor_vesa();
    } else {
        if (!cursor_visible) vga_hide_crtc_cursor();
        else {
            vga_show_crtc_cursor();
            update_cursor_vga();
        }
    }
}

void put_char(char c) {
    serial_putc(c);          /* debug mirror to COM1 (QEMU -serial) */
    if (use_vesa) put_char_vesa(c);
    else          put_char_vga(c);
}

void print_string(const char* str) {
    while (*str) put_char(*str++);
}

void print_int(uint32_t num) {
    char buffer[12];
    int i = 0;
    if (num == 0) { put_char('0'); return; }
    while (num > 0) {
        buffer[i++] = (num % 10) + '0';
        num /= 10;
    }
    while (i > 0)
        put_char(buffer[--i]);
}

void clear_screen(void) {
    if (use_vesa) clear_screen_vesa();
    else          clear_screen_vga();
}

// ======================== COLORS ===========================
void set_color(uint8_t fg, uint8_t bg) {
    current_color = (bg << 4) | (fg & 0x0F);
    if (use_vesa) {
        term_fg_rgb = vga_palette[fg & 0x0F];
        term_bg_rgb = vga_palette[bg & 0x0F];
    }
}

/* set_fg_rgb() — direct 24-bit foreground color (0xRRGGBB).
 * VESA console: exact RGB (used by the Equinox boot banner for its
 * purple/white gradient, which the 16-color VGA palette cannot
 * express). VGA text fallback: snapped to the nearest palette entry
 * by brightness (bright -> white, dark -> light magenta), so the
 * banner still reads as purple+white in text mode. */
void set_fg_rgb(uint32_t rgb) {
    if (use_vesa) {
        term_fg_rgb = rgb & 0x00FFFFFFu;
        /* snap the 16-color fg for the cell attr mirror (consistent re-render) */
        current_color = (current_color & 0xF0) | rgb_nearest16(term_fg_rgb);
        return;
    }
    uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    uint32_t lum = (uint32_t)r * 299u + (uint32_t)g * 587u + (uint32_t)b * 114u;
    uint8_t attr = (lum >= 140000u) ? (uint8_t)VGA_COLOR_WHITE
                                    : (uint8_t)VGA_COLOR_LIGHT_MAGENTA;
    current_color = (current_color & 0xF0) | (attr & 0x0F);
}

void set_default_color(void) {
    current_color = 0x0F;
    if (use_vesa) {
        term_fg_rgb = 0xFFFFFF;
        term_bg_rgb = 0x000000;
    }
}

// ======================== CURSOR ============================
void update_cursor(void) {
    if (use_vesa) update_cursor_vesa();
    else          update_cursor_vga();
}

void set_cursor_position(int row, int col) {
    if (row < 0) row = 0;
    if (row >= term_rows) row = term_rows - 1;
    if (col < 0) col = 0;
    if (col >= term_cols) col = term_cols - 1;
    cursor_row = row;
    cursor_col = col;
    update_cursor();
}

// ======================== PRINTF ============================
static void print_padded(const char* str, int width, int left_align, int zero_pad, int* count) {
    /* FIX(S7): %s with a NULL pointer used to deref NULL in a
     * manual strlen -> page fault -> panic. Print "(null)"
     * like most libcs do. */
    if (!str) str = "(null)";
    int len = 0;
    while (str[len]) len++;
    int pad = width - len;
    if (pad < 0) pad = 0;

    if (!left_align) {
        for (int i = 0; i < pad; i++) {
            put_char(zero_pad ? '0' : ' ');
            (*count)++;
        }
    }
    print_string(str);
    *count += len;
    if (left_align) {
        for (int i = 0; i < pad; i++) {
            put_char(' ');
            (*count)++;
        }
    }
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int count = 0;

    for (const char* p = format; *p; p++) {
        if (*p == '%') {
            p++;
            int width = 0;
            int left_align = 0;
            int zero_pad = 0;

            if (*p == '-') { left_align = 1; p++; }
            if (*p == '0') { zero_pad = 1; p++; }

            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }

            /* FIX(S5): consume length modifiers (z/l/h/hh). On i386,
             * size_t/long/int are all 4 bytes -> just skip them so the
             * arguments do not shift. Without this "%zu" printed a
             * literal "%z"+"u" AND the size_t was never consumed -> all
             * following arguments were misread. */
            if (*p == 'z' || *p == 'l' || *p == 'h') {
                p++;
                if (*p == 'h') p++;   // "hh"
            }

            switch (*p) {
                case 'd': {
                    int val = va_arg(args, int);
                    /* Old bug: 'val = -val' overflowed for INT_MIN.
                     * Use utoa with a well-defined unsigned cast. */
                    char buf[32];
                    if (val < 0) {
                        put_char('-'); count++;
                        utoa((uint32_t)(-(uint32_t)val), buf, 10);
                    } else {
                        utoa((uint32_t)val, buf, 10);
                    }
                    print_padded(buf, width, left_align, zero_pad, &count);
                    break;
                }
                case 'u': {
                    uint32_t val = va_arg(args, uint32_t);
                    char buf[32];
                    utoa(val, buf, 10);
                    print_padded(buf, width, left_align, zero_pad, &count);
                    break;
                }
                case 'x': {
                    uint32_t val = va_arg(args, uint32_t);
                    char buf[32];
                    utoa(val, buf, 16);   /* fix: was itoa((int)val,...) */
                    print_padded(buf, width, left_align, zero_pad, &count);
                    break;
                }
                case 'X': {
                    uint32_t val = va_arg(args, uint32_t);
                    char buf[32];
                    utoa(val, buf, 16);
                    for (char* q = buf; *q; q++) {
                        if (*q >= 'a' && *q <= 'f') *q = *q - 'a' + 'A';
                    }
                    print_padded(buf, width, left_align, zero_pad, &count);
                    break;
                }
                case 's': {
                    const char* s = va_arg(args, const char*);
                    print_padded(s, width, left_align, zero_pad, &count);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    put_char(c);
                    count++;
                    break;
                }
                case '%': {
                    put_char('%');
                    count++;
                    break;
                }
                case 'f': {
                    /* FIX(S5): support %f — fixed-point, 6 decimal digits.
                     * The FPU was already fninit'ed in kernel_main, so
                     * double arithmetic is safe to use here.
                     * Before: fell through to default -> printed a
                     * literal "%f" and the double argument was not consumed ->
                     * all following arguments shifted. */
                    double val = va_arg(args, double);
                    if (val < 0) { put_char('-'); count++; val = -val; }
                    if (val > 4294967295.0) val = 4294967295.0; // clamp 32-bit
                    uint32_t ipart = (uint32_t)val;
                    double frac = val - (double)ipart;
                    uint32_t fpart = (uint32_t)(frac * 1000000.0 + 0.5);
                    if (fpart >= 1000000u) { ipart++; fpart -= 1000000u; }
                    char ibuf[12];
                    utoa(ipart, ibuf, 10);
                    print_padded(ibuf, width, left_align, zero_pad, &count);
                    put_char('.'); count++;
                    char fbuf[8];
                    uint32_t scale = 100000u;
                    for (int d = 0; d < 6; d++) {
                        fbuf[d] = (char)('0' + (int)((fpart / scale) % 10u));
                        scale /= 10u;
                    }
                    fbuf[6] = '\0';
                    print_string(fbuf); count += 6;
                    break;
                }
                default:
                    put_char('%');
                    put_char(*p);
                    count += 2;
            }
        } else {
            put_char(*p);
            count++;
        }
    }
    va_end(args);
    return count;
}

// ======================== PORT I/O ==========================
uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* FIX(V1b): 16-bit port — needed for the Bochs VBE DISPI DATA
 * register (0x1CF). Before, vesa_force_mode() wrote via 8-bit outb:
 * a 16-bit value such as XRES=1366 (0x0556) got truncated to the
 * low byte (0x56) -> read-back verification failed -> the GRUB
 * mode was restored and the 1366x768 override never took effect. */
uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* v0.3 (FR-12): 32-bit port I/O — PCI config cycles. */
uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outl(uint16_t port, uint32_t val) {
    asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

// ======================== KEYBOARD I/O ======================
/* Scancode set 1 (as translated by the 8042) -> ASCII table, index =
 * make scancode. Rewritten with explicit index numbering and an
 * EXACT count of 128 entries (old version: 118 initializers, the
 * other 10 implicitly zero-filled — semantically correct, but without
 * index markers it was error-prone to count when edited later).
 *   0x00-0x0E : 0, ESC, 1..0, '-', '=', Backspace
 *   0x0F-0x1C : Tab, q..p, '[', ']', Enter('\n')
 *   0x1D-0x29 : Ctrl(L)=0, a..l, ';', '\'', '`'
 *   0x2A-0x35 : Shift(L)=0, '\', z../
 *   0x36-0x39 : Shift(R)=0, keypad '*', Alt(L)=0, Space
 *   0x3A..    : CapsLock etc. = 0 (handled specially in the decoder) */
const char scancode_to_ascii[128] = {
    /*  0 */ 0,   27,  '1', '2', '3', '4', '5', '6',
    /*  8 */ '7', '8', '9', '0', '-', '=', '\b',
    /* 15 */ '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u',
    /* 23 */ 'i', 'o', 'p', '[', ']', '\n',
    /* 29 */ 0,   'a', 's', 'd', 'f', 'g', 'h', 'j',
    /* 37 */ 'k', 'l', ';', '\'', '`',
    /* 42 */ 0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    /* 50 */ 'm', ',', '.', '/',
    /* 54 */ 0,   '*',  0,   ' ', 0,   0,   0,   0,
    /* 62 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 70 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 78 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 86 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 94 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /*102 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /*110 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /*118 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /*126 */ 0,   0
};

static int shift_pressed = 0;
static int caps_lock = 0;
static int ctrl_pressed = 0;

/* FIX(S9): extended sequences (0xE0 xx) are now consumed IN PAIRS
 * by ALL decoders (before only getkey did; getchar() relied on
 * 0xE0 accidentally passing through the break-code branch). Extended
 * releases are cleanly ignored; keypad Enter -> '\n', keypad '/' -> '/'. */
char getchar(void) {
    while (1) {
        uint8_t scancode = keyboard_read_byte();

        if (scancode == 0xE0) {              // extended: consume its pair
            uint8_t code2 = keyboard_read_byte();
            if (!(code2 & 0x80)) {
                if (code2 == 0x1C) return '\n';   // keypad Enter
                if (code2 == 0x35) return '/';    // keypad '/'
            }
            continue;                        // release / arrows: ignore
        }

        if (scancode & 0x80) {
            uint8_t key = scancode & 0x7F;
            if (key == 0x2A || key == 0x36) shift_pressed = 0;
            if (key == 0x1D || key == 0x9D) ctrl_pressed = 0;
            continue;
        }

        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; continue; }
        if (scancode == 0x1D || scancode == 0x9D) { ctrl_pressed = 1; continue; }
        if (scancode == 0x3A) { caps_lock = !caps_lock; continue; }

        char ascii = scancode_to_ascii[scancode];
        if (ascii == 0) continue;

        if (ctrl_pressed && ascii >= 'a' && ascii <= 'z') {
            return ascii - 'a' + 1;
        }

        if (ascii >= 'a' && ascii <= 'z') {
            if (shift_pressed || caps_lock)
                ascii = ascii - 'a' + 'A';
        }
        if (shift_pressed) {
            switch (ascii) {
                case '1': ascii = '!'; break; case '2': ascii = '@'; break;
                case '3': ascii = '#'; break; case '4': ascii = '$'; break;
                case '5': ascii = '%'; break; case '6': ascii = '^'; break;
                case '7': ascii = '&'; break; case '8': ascii = '*'; break;
                case '9': ascii = '('; break; case '0': ascii = ')'; break;
                case '-': ascii = '_'; break; case '=': ascii = '+'; break;
                case '[': ascii = '{'; break; case ']': ascii = '}'; break;
                case '\\': ascii = '|'; break; case ';': ascii = ':'; break;
                case '\'': ascii = '"'; break; case ',': ascii = '<'; break;
                case '.': ascii = '>'; break; case '/': ascii = '?'; break;
                case '`': ascii = '~'; break;
            }
        }

        return ascii;
    }
}

void gets(char* buffer, int max) {
    int i = 0;
    while (1) {
        char c = getchar();
        if (c == '\n') {
            buffer[i] = '\0';
            put_char('\n');
            return;
        } else if (c == '\b' && i > 0) {
            i--;
            put_char('\b');
        } else if (c >= 32 && c <= 126 && i < max - 1) {
            buffer[i++] = c;
            put_char(c);
        }
    }
}

// ======================== COMMAND HISTORY ===================
//  Ring buffer of recent shell commands + an arrow-aware line editor
//  (gets_history). getkey() reports special keys as negative codes
//  (-1 = Up, -2 = Down, see the decoder above), which gets() cannot
//  see — that is why the shell prompt must use gets_history().
//
//  Redraw model: put_char('\b') moves the cursor back AND clears the
//  cell in both backends (VGA and VESA), so "erase i chars" is simply
//  i backspaces, followed by printing the replacement text.
// ============================================================
#define HIST_MAX  16     // number of remembered commands
#define HIST_LEN  128    // max stored command length (incl. NUL)

static char hist_ring[HIST_MAX][HIST_LEN];
static int  hist_count = 0;    // entries stored so far (<= HIST_MAX)
static int  hist_head  = -1;   // ring index of the newest entry (-1 = empty)

// Tiny local compare: avoids depending on libstring linkage here.
static int hist_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void history_add(const char* cmd) {
    if (!cmd || !cmd[0]) return;                       // skip empty lines
    if (hist_head >= 0 && hist_streq(hist_ring[hist_head], cmd))
        return;                                        // skip consecutive duplicate

    hist_head = (hist_head + 1) % HIST_MAX;
    int i = 0;
    while (cmd[i] && i < HIST_LEN - 1) {
        hist_ring[hist_head][i] = cmd[i];
        i++;
    }
    hist_ring[hist_head][i] = '\0';
    if (hist_count < HIST_MAX) hist_count++;
}

int history_count(void) {
    return hist_count;
}

// k = 0 -> newest entry, k = 1 -> one older, ...
static const char* history_entry(int k) {
    if (k < 0 || k >= hist_count) return 0;
    int idx = (hist_head - k + HIST_MAX) % HIST_MAX;
    return hist_ring[idx];
}

// Replace the visible line (and `buf`/`i`) with `text`.
// `max` is the caller's buffer capacity.
static void hist_replace_line(char* buf, int* i, int max, const char* text) {
    while (*i > 0) {                                   // erase visible chars
        put_char('\b');
        (*i)--;
    }
    int j = 0;
    while (text[j] && j < max - 1) {                   // type replacement
        buf[j] = text[j];
        put_char(text[j]);
        j++;
    }
    *i = j;
}

void gets_history(char* buffer, int max) {
    int i   = 0;      // current line length / cursor column
    int nav = -1;     // -1 = editing the live line, 0.. = browsing history
    char live[HIST_LEN];   // draft of the live line, saved on first Up
    live[0] = '\0';

    while (1) {
        int k = getkey();

        if (k == '\n' || k == '\r') {                  // Enter: finish line
            buffer[i] = '\0';
            put_char('\n');
            return;
        }

        if (k == -1) {                                 // Up: older entry
            if (nav + 1 >= hist_count) continue;       // already at oldest
            if (nav == -1) {                           // save the live draft
                int j = 0;
                while (j < i && j < HIST_LEN - 1) { live[j] = buffer[j]; j++; }
                live[j] = '\0';
            }
            nav++;
            const char* e = history_entry(nav);
            if (e) hist_replace_line(buffer, &i, max, e);
            continue;
        }

        if (k == -2) {                                 // Down: newer entry
            if (nav == -1) continue;                   // already live
            nav--;
            const char* e = (nav == -1) ? live : history_entry(nav);
            hist_replace_line(buffer, &i, max, e ? e : "");
            continue;
        }

        if (k == '\b') {                               // Backspace
            if (i > 0) {
                i--;
                put_char('\b');
            }
            continue;
        }

        if (k >= 32 && k <= 126 && i < max - 1) {      // Printable char
            buffer[i++] = (char)k;
            put_char((char)k);
            // Editing while browsing: detach back to the live line so the
            // next Up starts over from the newest entry (doskey behavior).
            if (nav != -1) {
                int j = 0;
                while (j < i && j < HIST_LEN - 1) { live[j] = buffer[j]; j++; }
                live[j] = '\0';
                nav = -1;
            }
            continue;
        }

        // Other special keys (Left/Right/Home/End/PgUp/PgDn/Del = -3..-9)
        // and control combos: silently ignored.
    }
}

// ======================== GETKEY =============================
// The scancode -> key code translation is split into two stages so it
// can be shared by getkey() (blocking) and getkey_poll()
// (non-blocking, for game loops via SYS_POLLKEY):
//   translate_ext(code2)  : second byte of an 0xE0 sequence (arrows etc.)
//   translate_base(code)  : regular scancode (shift/ctrl/caps aware)
// Both STICK to the global shift/ctrl/caps state (by design: keyboard
// state is shared) and return KEY_IGNORED for events that produce no
// key (releases, bare shifts, etc.).
#define KEY_IGNORED (-100)

static int translate_ext(uint8_t code2) {
    switch (code2) {
        case 0x48: return -1;        // up
        case 0x50: return -2;        // down
        case 0x4B: return -3;        // left
        case 0x4D: return -4;        // right
        case 0x47: return -5;        // home
        case 0x4F: return -6;        // end
        case 0x49: return -7;        // page up
        case 0x51: return -8;        // page down
        case 0x53: return -9;        // delete
        case 0x1C: return '\n';     // keypad Enter (FIX S9)
        case 0x35: return '/';      // keypad '/'  (FIX S9)
        default:   return KEY_IGNORED;   // win key etc.: ignore
    }
}

static int translate_base(uint8_t code) {
    if (code & 0x80) {
        uint8_t key = code & 0x7F;
        if (key == 0x2A || key == 0x36) shift_pressed = 0;
        if (key == 0x1D || key == 0x9D) ctrl_pressed = 0;
        return KEY_IGNORED;
    }
    if (code == 0x2A || code == 0x36) { shift_pressed = 1; return KEY_IGNORED; }
    if (code == 0x1D || code == 0x9D) { ctrl_pressed = 1; return KEY_IGNORED; }
    if (code == 0x3A) { caps_lock = !caps_lock; return KEY_IGNORED; }

    char ascii = scancode_to_ascii[code];
    if (ascii == 0) return KEY_IGNORED;

    if (ctrl_pressed && ascii >= 'a' && ascii <= 'z') {
        return ascii - 'a' + 1;
    }

    if (ascii >= 'a' && ascii <= 'z') {
        if (shift_pressed || caps_lock)
            ascii = ascii - 'a' + 'A';
    }
    if (shift_pressed) {
        switch (ascii) {
            case '1': ascii = '!'; break; case '2': ascii = '@'; break;
            case '3': ascii = '#'; break; case '4': ascii = '$'; break;
            case '5': ascii = '%'; break; case '6': ascii = '^'; break;
            case '7': ascii = '&'; break; case '8': ascii = '*'; break;
            case '9': ascii = '('; break; case '0': ascii = ')'; break;
            case '-': ascii = '_'; break; case '=': ascii = '+'; break;
            case '[': ascii = '{'; break; case ']': ascii = '}'; break;
            case '\\': ascii = '|'; break; case ';': ascii = ':'; break;
            case '\'': ascii = '"'; break; case ',': ascii = '<'; break;
            case '.': ascii = '>'; break; case '/': ascii = '?'; break;
            case '`': ascii = '~'; break;
        }
    }
    return (int)(uint8_t)ascii;
}

int getkey(void) {
    while (1) {
        // keyboard_read_byte() already waits (busy-wait) until data
        // arrives — a keyboard_has_data() pre-check is no longer needed.
        uint8_t code = keyboard_read_byte();

        if (code == 0xE0) {
            // Extended sequence: wait for + consume both bytes.
            uint8_t code2 = keyboard_read_byte();
            if (code2 & 0x80) continue;      // extended release: ignore
            int k = translate_ext(code2);
            if (k != KEY_IGNORED) return k;
        } else {
            int k = translate_base(code);
            if (k != KEY_IGNORED) return k;
        }
    }
}

/* getkey_poll() — the NON-BLOCKING variant for game loops (SYS_POLLKEY).
 * ----------------------------------------------------------------
 * Return: 0  = no key waiting (nothing pressed this frame)
 *         >0 = ASCII code of the newly pressed key
 *         <0 = special key code (same as getkey: -1 Up, -2 Down,
 *              -3 Left, -4 Right, -5..-9 Home/End/PgUp/PgDn/Del)
 * 0 was chosen as the "empty" marker so it does NOT collide with the
 * arrow codes -1..-9 (if -1 meant "empty", the Up arrow could not be
 * distinguished from an empty buffer).
 *
 * An extended sequence (0xE0 + second byte) can be split across polls
 * (the second byte has not reached the buffer yet): the pending_e0 state
 * remembers the 0xE0 until its second byte arrives on the next poll. */
int getkey_poll(void) {
    static int pending_e0 = 0;

    while (1) {
        int c = pending_e0 ? 0xE0 : keyboard_read_byte_noblock();
        pending_e0 = 0;
        if (c < 0) return 0;               // empty buffer
        uint8_t code = (uint8_t)c;

        if (code == 0xE0) {
            int c2 = keyboard_read_byte_noblock();
            if (c2 < 0) {                  // second byte not arrived yet
                pending_e0 = 1;
                return 0;
            }
            if (c2 & 0x80) continue;       // extended release: ignore
            int k = translate_ext((uint8_t)c2);
            if (k != KEY_IGNORED) return k;
        } else {
            int k = translate_base(code);
            if (k != KEY_IGNORED) return k;
        }
    }
}

// ======================== KEY EVENTS (v10.9 — DOOM) =========
// getkey_event() — a NON-BLOCKING decoder complementing getkey_poll()
// for games that track HELD keys (DOOM: hold an arrow = keep
// moving). Differences vs the old decoder:
//
//   1. Reports both PRESS *and* RELEASE events (the old decoder
//      silently dropped every break-code).
//   2. The reported code is the RAW code WITHOUT the shift/caps/ctrl
//      transformation — a game (e.g. DOOM) wants the physical key
//      code and then applies the shift transformation itself via the
//      modifier events that are now also reported.
//   3. Modifier keys + F1-F12 now produce events (previously silent):
//        -10 Ctrl (left/right)     -11 Shift (left/right)
//        -12 Alt  (left/right)     -13..-22 F1..F10
//        -23 F11                   -24 F12
//      The old decoder's special codes stay the same: -1 Up, -2 Down,
//      -3 Left, -4 Right, -5 Home, -6 End, -7 PgUp, -8 PgDn, -9 Del.
//
// API: returns 1 event (sets *pressed 1/0, returns the code) or 0 when
// there is no event. CapsLock/NumLock/ScrollLock stay silent (internal
// toggle only) so the game is not flooded with useless events.
// The global shift/ctrl state IS still updated so the old decoders
// (getchar/getkey) never see stale state after the program ends.
// ============================================================
int getkey_event(int* pressed) {
    static int pending_e0 = 0;

    while (1) {
        int c = pending_e0 ? 0xE0 : keyboard_read_byte_noblock();
        pending_e0 = 0;
        if (c < 0) return 0;               // empty buffer
        uint8_t code = (uint8_t)c;

        if (code == 0xE0) {
            // Extended (E0 xx): arrows, keypad, right Ctrl/Alt.
            int c2 = keyboard_read_byte_noblock();
            if (c2 < 0) { pending_e0 = 1; return 0; }
            uint8_t k = (uint8_t)c2;
            int rel = (k & 0x80) != 0;
            k &= 0x7F;
            switch (k) {
                case 0x48: *pressed = !rel; return -1;    // Up
                case 0x50: *pressed = !rel; return -2;    // Down
                case 0x4B: *pressed = !rel; return -3;    // Left
                case 0x4D: *pressed = !rel; return -4;    // Right
                case 0x47: *pressed = !rel; return -5;    // Home
                case 0x4F: *pressed = !rel; return -6;    // End
                case 0x49: *pressed = !rel; return -7;    // PgUp
                case 0x51: *pressed = !rel; return -8;    // PgDn
                case 0x53: *pressed = !rel; return -9;    // Del
                case 0x1C: *pressed = !rel; return '\n';  // keypad Enter
                case 0x35: *pressed = !rel; return '/';   // keypad '/'
                case 0x1D: ctrl_pressed = !rel;
                           *pressed = !rel; return -10;   // Right Ctrl
                case 0x38: *pressed = !rel; return -12;   // Right Alt
                default:   continue;                      // Win etc.
            }
        } else {
            int rel = (code & 0x80) != 0;
            uint8_t k = code & 0x7F;

            if (k == 0x57) { *pressed = !rel; return -23; }          // F11
            if (k == 0x58) { *pressed = !rel; return -24; }          // F12
            if (k >= 0x3B && k <= 0x44) {                            // F1..F10
                *pressed = !rel;
                return -13 - (k - 0x3B);
            }
            if (k == 0x2A || k == 0x36) {                            // Shift
                shift_pressed = !rel;
                *pressed = !rel; return -11;
            }
            if (k == 0x1D) {                                         // Ctrl
                ctrl_pressed = !rel;
                *pressed = !rel; return -10;
            }
            if (k == 0x38) { *pressed = !rel; return -12; }          // Alt
            if (k == 0x3A) {                                         // CapsLock
                if (!rel) caps_lock = !caps_lock;
                continue;
            }
            if (k == 0x45 || k == 0x46) continue;                    // Num/ScrLock

            char ascii = scancode_to_ascii[k];      // RAW (no shift)
            if (ascii == 0) continue;
            *pressed = !rel;
            return (int)(uint8_t)ascii;
        }
    }
}

// ======================== REBOOT ============================
void reboot(void) {
    /* outb(0x64, 0xFE) = pulse the reset line via the keyboard controller.
     * That alone is enough to reboot on emulated and bare-metal PCs.
     * Old bug: after the outb, an asm block wrote 0 to CR3 (wiping the
     * page directory!) then jumped to 0x1234 (a bogus address -- that is
     * the BIOS warm-reset magic value, NOT a jump target). The result was
     * a triple fault before the keyboard controller could reset. Now we
     * just hlt after the outb; if the CPU somehow survives, hlt loop. */
    asm volatile ("cli");
    outb(0x64, 0xFE);
    for (;;) {
        asm volatile ("hlt");
    }
}
