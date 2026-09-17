#include "../header/codeEditor.h"
#include "../header/stdio.h"
#include "../header/fs_ram.h"
#include "../header/malloc.h"
#include "../header/libstring.h"
#include "../header/color.h"
#include "../header/timer.h"
#include <stdint.h>

// ============================================================
//  Equinox OS Editor v2 — coding-grade, incremental rendering.
// ------------------------------------------------------------
//  Major changes vs v1 (the old codeEditor.cpp):
//
//  1. DYNAMIC SIZE — uses term_get_cols()/term_get_rows(),
//     so the editor fills the WHOLE screen: VGA 80x25, VESA 1024x768
//     (128x46 text rows), VESA 1366x768 (170x46). v1 hardcoded
//     80x24 — on a VESA screen it only showed in the top-left corner.
//
//  2. INCREMENTAL RENDER ("typing does not refresh the UI, just
//     like a terminal"). The screen is divided per row; each text row
//     has a shadow-hash (row content + block comment state + scroll
//     position). A keystroke only re-renders the rows that actually
//     changed (usually 1 row) + the status bar. Arrow movement does
//     not re-render text at all — only the cursor moves. There is
//     NO clear_screen() per keystroke -> no flicker.
//
//  3. A UI more fit for coding:
//     - title bar (blue): file name + modified marker
//     - line-number gutter (dark grey)
//     - C syntax highlighting: keywords (cyan), numbers (yellow),
//       strings (green), comments (grey) — active for .c/.h
//     - status bar (blue): L/C, size, line count, keys
//     - Tab = 4 spaces, PageUp/PageDown, Home/End, Delete,
//       cross-row Backspace (join), automatic vertical +
//       horizontal scrolling (long rows are not wrapped)
//
//  4. 16 KB buffer (v1: 4 KB — too small for serious sources).
// ============================================================

#define MAX_BUFFER_SIZE 16384
#define GUTTER_WIDTH    7      /* "9999 | " (4 digits, space, pipe, space) */
#define TAB_SPACES      4
#define ED_MAX_ROWS     64     /* shadow limit (46 text rows @ 768p) */

static struct {
    char* buffer;
    uint32_t size;
    uint32_t max_size;
    char fullpath[256];        // full file path
    char filename[64];         // file name only (for display)
    int cursor_row;            // logical row (0-based, buffer coordinates)
    int cursor_col;            // logical column
    int top_line;              // first visible logical row
    int left_col;              // first visible logical column
    int dirty;
    int line_count;
    int highlight;             // 1 = C syntax highlighting (.c/.h)
} editor;

// Block comment state per logical row (computed with one full walk
// per edit — O(size), cheap). Index = row number.
static uint8_t ed_comment_state[MAX_BUFFER_SIZE + 1];

// Screen layout (computed in editor_open)
static int scr_cols, scr_rows;
static int text_cols, text_rows;

// Render shadow — hash of the last drawn content (dirty row detection)
static uint32_t shadow_hash[ED_MAX_ROWS];
static uint32_t shadow_top    = 0x5A5A5A5Au;
static uint32_t shadow_left   = 0x5A5A5A5Au;
static uint32_t shadow_status = 0x5A5A5A5Au;
static uint32_t shadow_title  = 0x5A5A5A5Au;

// ============================================================
//  Buffer position helpers
// ============================================================
static uint32_t ed_line_start(int row) {
    if (row <= 0) return 0;
    uint32_t pos = 0;
    int r = 0;
    while (pos < editor.size && r < row) {
        if (editor.buffer[pos] == '\n') r++;
        pos++;
    }
    return pos;
}

static uint32_t ed_line_len(int row) {
    uint32_t pos = ed_line_start(row);
    uint32_t len = 0;
    while (pos + len < editor.size && editor.buffer[pos + len] != '\n') len++;
    return len;
}

static uint32_t ed_pos_of(int row, int col) {
    uint32_t pos = ed_line_start(row);
    uint32_t len = ed_line_len(row);
    if (col < 0) col = 0;
    if ((uint32_t)col > len) col = (int)len;
    return pos + (uint32_t)col;
}

// One walk: count the lines + block comment state per line.
// Called after load and after every edit (16 KB -> cheap).
// ed_comment_state[L] = 1 if line L STARTS inside /* ... */.
// An array of MAX_BUFFER_SIZE+1 covers the worst case (all '\n').
static void ed_recalc_states(void) {
    uint32_t i = 0;
    int line = 0;
    int in_block = 0;
    ed_comment_state[0] = 0;

    while (i < editor.size) {
        char c = editor.buffer[i];

        if (in_block) {
            if (c == '*' && i + 1 < editor.size &&
                editor.buffer[i + 1] == '/') {
                in_block = 0;
                i += 2;                  // consume "*/" in one go
                continue;
            }
        } else {
            if (c == '/' && i + 1 < editor.size &&
                editor.buffer[i + 1] == '*') {
                in_block = 1;
                i += 2;                  // consume "/*" in one go
                continue;
            }
            if (c == '/' && i + 1 < editor.size &&
                editor.buffer[i + 1] == '/') {
                i++;                     // skip to the end of the line
                while (i < editor.size && editor.buffer[i] != '\n') i++;
                continue;                // the '\n' is handled by the main loop
            }
            if (c == '"') {
                i++;
                while (i < editor.size && editor.buffer[i] != '"' &&
                       editor.buffer[i] != '\n') {
                    if (editor.buffer[i] == '\\' && i + 1 < editor.size &&
                        editor.buffer[i + 1] != '\n') {
                        i++;             // escape: skip the next char
                    }
                    i++;
                }
                if (i < editor.size && editor.buffer[i] == '"') {
                    i++;                 // consume the closing '"'
                }
                continue;                // '\n' / EOF handled by the main loop
            }
        }

        if (c == '\n') {
            line++;
            if (line <= MAX_BUFFER_SIZE) {
                ed_comment_state[line] = (uint8_t)in_block;
            }
        }
        i++;
    }
    editor.line_count = line + 1;
}

// Hash of the row content + comment state + scroll position — the key
// to "dirty row" detection for incremental rendering.
static uint32_t ed_hash_line(int ln) {
    uint32_t h = 1469598103u;   // FNV-1a
    uint32_t start = ed_line_start(ln);
    uint32_t len = ed_line_len(ln);
    for (uint32_t i = 0; i < len; i++) {
        h = (h ^ (uint8_t)editor.buffer[start + i]) * 16777619u;
    }
    h = (h ^ (uint32_t)ln) * 16777619u;
    h = (h ^ (uint32_t)editor.left_col) * 16777619u;
    h ^= ed_comment_state[ln] ? 0xB1B1B1B1u : 0x00C0FFEEu;
    return h;
}

/* ============================================================
 *  FIX(E1) — HASH SEPARATED FROM RENDER (architecture)
 *  ------------------------------------------------------------
 *  Bug report (Critical #1): ed_render_title()/ed_render_status()
 *  used to RETURN a hash while also DRAWING — and then
 *  ed_render_smart() called them AS hash functions:
 *
 *      if (ed_render_title() != shadow_title)      <- already rendered!
 *          shadow_title = ed_render_title();       <- renders AGAIN
 *
 *  Consequence: the title + status bar were drawn on EVERY keypress
 *  (twice when they changed), plus — because the full-width status
 *  bar is drawn on the LAST row — the put_char wrap triggered a
 *  screen scroll (see FIX R1 in stdio.cpp) so the entire editor
 *  content shifted one row per keystroke (text "appears briefly
 *  then vanishes", typing "behaves like pressing Enter").
 *
 *  New rule for this file (enforced by function structure):
 *      ed_hash_*()   PURE — never touches the screen
 *      ed_render_*() ONLY draws — never returns a hash
 *  ed_render_smart() computes the hashes first, compares, and only
 *  then renders what actually changed.
 * ============================================================ */
static uint32_t ed_hash_title(void) {
    uint32_t h = 1469598103u;
    for (const char* t = editor.filename; *t; t++) {
        h = (h ^ (uint8_t)*t) * 16777619u;
    }
    h = (h ^ (uint32_t)editor.dirty) * 16777619u;
    return h;
}

// Formats the status bar text — a PURE snprintf, never touches the screen.
// Shared by ed_hash_status() and ed_render_status() so the two are
// guaranteed consistent (a hash of exactly the text that gets drawn).
static void ed_format_status(char* buf, int bufsz) {
    snprintf(buf, (size_t)bufsz,
             " L%d C%d | %u bytes | %d lines | Tab PgUp/PgDn Home End | Ctrl+S save | Ctrl+Q quit",
             editor.cursor_row + 1, editor.cursor_col + 1,
             editor.size, editor.line_count);
}

static uint32_t ed_hash_status(void) {
    char buf[160];
    ed_format_status(buf, sizeof(buf));
    uint32_t h = 1469598103u;
    for (const char* p = buf; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

// ============================================================
//  C syntax highlighting — a mini per-row tokenizer
// ============================================================
static const char* const ed_keywords[] = {
    "int", "char", "void", "if", "else", "while", "for", "do",
    "return", "break", "continue", "static", "const", "unsigned",
    "long", "short", "struct", "union", "typedef", "sizeof",
    "switch", "case", "default", "goto", "enum", "float", "double",
    NULL
};

static int ed_is_keyword(const char* w, uint32_t len) {
    for (int i = 0; ed_keywords[i]; i++) {
        const char* k = ed_keywords[i];
        uint32_t kl = 0;
        while (k[kl]) kl++;
        if (kl == len) {
            uint32_t j = 0;
            while (j < kl && w[j] == k[j]) j++;
            if (j == kl) return 1;
        }
    }
    return 0;
}

static int ed_id_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int ed_id_char(char c) {
    return ed_id_start(c) || (c >= '0' && c <= '9');
}

// ============================================================
//  Rendering
// ============================================================

// Inline emit helper: color changed -> set_color; character to the screen.
#define ED_EMIT(ch, colr)                            \
    do {                                             \
        if ((colr) != cur) {                         \
            set_color((colr), VGA_COLOR_BLACK);      \
            cur = (colr);                            \
        }                                            \
        put_char(ch);                                \
        sc++;                                        \
    } while (0)

// Draws screen text row r (displaying logical row ln).
// FIX(E1): no longer returns a hash — ed_render_smart()
// computes the hash separately BEFORE rendering (hash-first,
// render-second) and stores the already computed result.
static void ed_render_text_row(int r, int ln) {
    int y = r + 1;                       // +1: the title bar is at row 0
    uint8_t cur = 0xFF;

    // --- Line-number gutter ---
    char gbuf[8];
    int glen = 0;
    if (ln < 10000) {
        int v = ln + 1;
        char tmp[6];
        int tl = 0;
        while (v > 0 && tl < 6) { tmp[tl++] = (char)('0' + v % 10); v /= 10; }
        for (int pad = 4 - tl; pad > 0; pad--) gbuf[glen++] = ' ';
        while (tl > 0) gbuf[glen++] = tmp[--tl];
    } else {
        gbuf[glen++] = '9'; gbuf[glen++] = '9';
        gbuf[glen++] = '9'; gbuf[glen++] = '9';
    }
    gbuf[glen++] = ' ';
    gbuf[glen++] = '|';
    gbuf[glen++] = ' ';
    gbuf[glen] = '\0';

    set_cursor_position(y, 0);
    set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    print_string(gbuf);
    cur = VGA_COLOR_DARK_GREY;

    // --- Row content (mini tokenizer + horizontal scrolling) ---
    uint32_t start = ed_line_start(ln);
    uint32_t end = start + ed_line_len(ln);
    int lc = 0;                          // LOGICAL column within the row
    int sc = 0;                          // SCREEN column (0..text_cols-1)
    uint32_t pos = start;

    enum { ST_CODE = 0, ST_STRING = 1, ST_LCOM = 2, ST_BLOCK = 3 };
    int state = ed_comment_state[ln] ? ST_BLOCK : ST_CODE;
    int pending_block_end = 0;
    int escaped = 0;
    uint8_t text_col = VGA_COLOR_LIGHT_GREY;

    while (pos < end && sc < text_cols) {
        char c = editor.buffer[pos];

        if (state == ST_BLOCK) {
            text_col = VGA_COLOR_DARK_GREY;
            if (pending_block_end) {
                pending_block_end = 0;
                state = ST_CODE;
            } else if (c == '*' && pos + 1 < end &&
                       editor.buffer[pos + 1] == '/') {
                pending_block_end = 1;
            }
            if (lc >= editor.left_col) ED_EMIT(c, text_col);
            lc++;  pos++;
        }
        else if (state == ST_LCOM) {
            text_col = VGA_COLOR_DARK_GREY;
            if (lc >= editor.left_col) ED_EMIT(c, text_col);
            lc++;  pos++;
        }
        else if (state == ST_STRING) {
            text_col = VGA_COLOR_LIGHT_GREEN;
            if (escaped) {
                escaped = 0;
            } else if (c == '\\') {
                escaped = 1;
            } else if (c == '"') {
                state = ST_CODE;
            }
            if (lc >= editor.left_col) ED_EMIT(c, text_col);
            lc++;  pos++;
        }
        else {   // ST_CODE
            if (editor.highlight && c == '/' && pos + 1 < end &&
                editor.buffer[pos + 1] == '/') {
                state = ST_LCOM;
                if (lc >= editor.left_col) ED_EMIT(c, VGA_COLOR_DARK_GREY);
                lc++;  pos++;
            }
            else if (editor.highlight && c == '/' && pos + 1 < end &&
                     editor.buffer[pos + 1] == '*') {
                state = ST_BLOCK;
                if (lc >= editor.left_col) ED_EMIT(c, VGA_COLOR_DARK_GREY);
                lc++;  pos++;
            }
            else if (editor.highlight && c == '"') {
                state = ST_STRING;
                if (lc >= editor.left_col) ED_EMIT(c, VGA_COLOR_LIGHT_GREEN);
                lc++;  pos++;
            }
            else if (editor.highlight && ed_id_start(c)) {
                uint32_t wend = pos;
                while (wend < end && ed_id_char(editor.buffer[wend])) wend++;
                uint8_t wcol = ed_is_keyword(editor.buffer + pos, wend - pos)
                                   ? VGA_COLOR_LIGHT_CYAN
                                   : VGA_COLOR_LIGHT_GREY;
                while (pos < wend && sc < text_cols) {
                    if (lc >= editor.left_col) ED_EMIT(editor.buffer[pos], wcol);
                    lc++;  pos++;
                }
            }
            else if (editor.highlight && c >= '0' && c <= '9') {
                uint32_t wend = pos;
                while (wend < end &&
                       ((editor.buffer[wend] >= '0' && editor.buffer[wend] <= '9') ||
                        editor.buffer[wend] == '.')) {
                    wend++;
                }
                while (pos < wend && sc < text_cols) {
                    if (lc >= editor.left_col) ED_EMIT(editor.buffer[pos], VGA_COLOR_LIGHT_BROWN);
                    lc++;  pos++;
                }
            }
            else {
                if (lc >= editor.left_col) ED_EMIT(c, VGA_COLOR_LIGHT_GREY);
                lc++;  pos++;
            }
        }
    }

    // Row remainder = spaces up to the right edge of the text area
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    while (sc < text_cols) { put_char(' '); sc++; }
}

// --- Title bar (row 0) — FIX(E1): just draw, no hash ---
static void ed_render_title(void) {
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    set_cursor_position(0, 0);
    for (int i = 0; i < scr_cols; i++) put_char(' ');

    set_cursor_position(0, 0);
    print_string(" ");
    print_string(editor.filename);
    if (editor.dirty) print_string(" *");
    print_string("  -  Equinox OS Editor");
}

// --- Status bar (last row) — FIX(E1): just draw, no hash ---
// NOTE: the full-width status row on the LAST row is only safe
// because the editor disables auto-scroll (term_set_scroll(0), see
// editor_open) — without that the put_char wrap shifts the whole screen.
static void ed_render_status(void) {
    char buf[160];
    ed_format_status(buf, sizeof(buf));

    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    set_cursor_position(scr_rows - 1, 0);
    for (int i = 0; i < scr_cols; i++) put_char(' ');

    set_cursor_position(scr_rows - 1, 0);
    print_string(buf);
}

// Full render — used on open, save, and scroll.
static void ed_render_all(void) {
    for (int r = 0; r < text_rows; r++) {
        ed_render_text_row(r, editor.top_line + r);
        shadow_hash[r] = ed_hash_line(editor.top_line + r);
    }
    uint32_t th = ed_hash_title();
    if (th != shadow_title) ed_render_title();
    shadow_title = th;

    uint32_t sh = ed_hash_status();
    if (sh != shadow_status) ed_render_status();
    shadow_status = sh;

    shadow_top    = (uint32_t)editor.top_line;
    shadow_left   = (uint32_t)editor.left_col;
}

/* Incremental render — called after EVERY keypress.
 * FIX(E1): the new "hash first, render second" pattern:
 *   1. compute row hashes -> compare -> render only what differs
 *   2. title/status hashes from PURE FUNCTIONS -> render only what
 *      differs (before: render-for-hash on every keypress + render again)
 * Combined with FIX R1 (scroll lock) and FIX R2 (quiet cursor)
 * this closes every "screen shifts / text vanishes" path. */
static void ed_render_smart(void) {
    if ((uint32_t)editor.top_line != shadow_top ||
        (uint32_t)editor.left_col != shadow_left) {
        ed_render_all();
        return;
    }
    for (int r = 0; r < text_rows; r++) {
        int ln = editor.top_line + r;
        uint32_t hash = ed_hash_line(ln);
        if (hash != shadow_hash[r]) {
            ed_render_text_row(r, ln);
            shadow_hash[r] = hash;      // hash already computed — use it
        }
    }
    uint32_t title_hash = ed_hash_title();
    if (title_hash != shadow_title) {
        ed_render_title();
        shadow_title = title_hash;
    }
    uint32_t status_hash = ed_hash_status();
    if (status_hash != shadow_status) {
        ed_render_status();
        shadow_status = status_hash;
    }
}

// Make sure the cursor is visible — shift the scroll if needed.
static void ed_ensure_visible(void) {
    if (editor.cursor_row < editor.top_line) {
        editor.top_line = editor.cursor_row;
    }
    if (editor.cursor_row >= editor.top_line + text_rows) {
        editor.top_line = editor.cursor_row - text_rows + 1;
    }
    if (editor.top_line < 0) editor.top_line = 0;

    if (editor.cursor_col < editor.left_col) {
        editor.left_col = editor.cursor_col;
    }
    if (editor.cursor_col >= editor.left_col + text_cols) {
        editor.left_col = editor.cursor_col - text_cols + 1;
    }
    if (editor.left_col < 0) editor.left_col = 0;
}

// Put the terminal cursor at the editor cursor position.
static void ed_place_cursor(void) {
    int y = editor.cursor_row - editor.top_line + 1;   // +1 title bar
    int x = editor.cursor_col - editor.left_col + GUTTER_WIDTH;
    if (y < 1) y = 1;
    if (y > text_rows) y = text_rows;
    if (x < GUTTER_WIDTH) x = GUTTER_WIDTH;
    if (x > GUTTER_WIDTH + text_cols - 1) x = GUTTER_WIDTH + text_cols - 1;
    set_cursor_position(y, x);
}

/* ============================================================
 *  FIX(E4) — ONE RENDER FRAME, ONE CURSOR PLACEMENT
 *  ------------------------------------------------------------
 *  Bug report (Critical #3 / #8): editor rendering used to call
 *  set_cursor_position() dozens of times per keypress (every put_char
 *  moves the terminal cursor), then ed_place_cursor() moved it again —
 *  safe ONLY as long as set_cursor_position purely sets the position.
 *  With FIX R2 the render pattern is now explicit:
 *
 *      1. cursor off (quiet)          — no underline
 *      2. render the changed area     — hash-first (FIX E1)
 *      3. place the final cursor      — position only
 *      4. cursor on                   — underline + blink at
 *                                        the final position, once
 *
 *  The blink ISR (timer) also stays quiet during phases 1-3, so there
 *  are no more line artifacts in the gutter/text rows and no racing
 *  with the render.
 * ============================================================ */
static void ed_render_frame(void) {
    term_set_cursor_visible(0);
    ed_render_smart();
    ed_place_cursor();
    term_set_cursor_visible(1);
}

// Restore the console to shell mode (scrolling + normal cursor).
// MUST be called on ALL exit paths of editor_open().
static void ed_restore_terminal(void) {
    term_set_scroll(1);
    term_set_cursor_visible(1);
}

// ============================================================
//  LOAD / SAVE
// ============================================================
static void editor_load(const char* fullpath) {
    struct fs_node* node = fs_get_node_from_path(fs_get_root(), fullpath);
    if (node && !node->is_dir) {
        editor.size = node->size;
        if (editor.size > editor.max_size) editor.size = editor.max_size;
        for (uint32_t i = 0; i < editor.size; i++) {
            editor.buffer[i] = node->content[i];
        }
        editor.buffer[editor.size] = '\0';
    } else {
        editor.size = 0;
        editor.buffer[0] = '\0';
    }
    editor.dirty = 0;
}

static void editor_save(void) {
    char parent_path[256] = {0};
    const char* last_slash = strrchr(editor.fullpath, '/');
    struct fs_node* parent = NULL;
    const char* filename = NULL;

    if (last_slash) {
        int parent_len = last_slash - editor.fullpath;
        if (parent_len == 0) parent_len = 1;               // "/name"
        strncpy(parent_path, editor.fullpath, parent_len);
        parent_path[parent_len] = '\0';
        parent = fs_get_node_from_path(fs_get_root(), parent_path);
        filename = last_slash + 1;
    } else {
        parent = fs_get_root();
        filename = editor.fullpath;
    }
    if (!parent) parent = fs_get_root();
    if (!filename || filename[0] == '\0') filename = "untitled";

    /* FIX v10.4 (editor round-trip): save through fs_write_binary()
     * instead of the old delete-then-fs_create_file() dance.
     *   - fs_write_binary is create-OR-overwrite, binary-safe (memcpy
     *     with an explicit length) and leaves no window where the
     *     file is already deleted but the create has not run yet.
     *   - editor.fullpath is now ALWAYS absolute (the shell resolves
     *     it through shell_resolve_path() before editor_open()), so
     *     both editor_load() and editor_save() hit the SAME node the
     *     shell's cat/mtcc see — the edit -> mtcc -> run loop keeps
     *     one single copy of the file. */
    int ret;
    if (editor.size > 0) {
        ret = fs_write_binary(parent, filename,
                              (const uint8_t*)editor.buffer, editor.size);
    } else {
        /* Empty buffer edge case: fs_write_binary would malloc(0)
        * for the content — keep the old create-empty path instead. */
        fs_delete_node(parent, filename);
        ret = fs_create_file(parent, filename, "");
    }

    /* FIX(E3): the save message is drawn with the cursor off (no
     * "marching" underline), and the status/title shadows are
     * invalidated immediately — the caller (ed_render_frame) redraws
     * the status bar + cursor ONCE after the message expires. */
    term_set_cursor_visible(0);
    set_color(ret == 0 ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED,
              VGA_COLOR_BLACK);
    set_cursor_position(scr_rows - 1, 0);
    if (ret != 0) {
        if (ret == -7) print_string("Save FAILED: name too long (max 63)   ");
        else if (ret == -6) print_string("Save FAILED: name used by a dir   ");
        else           print_string("Save FAILED (check filesystem)        ");
    } else {
        editor.dirty = 0;
        print_string("Saved.                                        ");
    }
    sleep_ms(400);
    shadow_status = 0x5A5A5A5Au;   // force the status bar to be redrawn
    shadow_title  = 0x5A5A5A5Au;   // (dirty flag gone -> the title changed)
}

// ============================================================
//  INPUT
// ============================================================
static void ed_insert_char(char c) {
    uint32_t pos = ed_pos_of(editor.cursor_row, editor.cursor_col);
    if (editor.size >= editor.max_size - 1) return;
    for (uint32_t i = editor.size; i > pos; i--) {
        editor.buffer[i] = editor.buffer[i - 1];
    }
    editor.buffer[pos] = c;
    editor.size++;
    editor.buffer[editor.size] = '\0';
    editor.dirty = 1;
}

static void ed_delete_at(uint32_t pos) {
    if (pos >= editor.size) return;
    for (uint32_t i = pos; i < editor.size; i++) {
        editor.buffer[i] = editor.buffer[i + 1];
    }
    editor.size--;
    editor.buffer[editor.size] = '\0';
    editor.dirty = 1;
}

static void editor_process_key(int key) {
    if (key >= 32 && key <= 126) {
        ed_insert_char((char)key);
        editor.cursor_col++;
    }
    else if (key == '\t') {                      // Tab -> 4 spaces
        for (int i = 0; i < TAB_SPACES; i++) ed_insert_char(' ');
        editor.cursor_col += TAB_SPACES;
    }
    else if (key == '\b') {                      // Backspace
        uint32_t pos = ed_pos_of(editor.cursor_row, editor.cursor_col);
        if (editor.cursor_col > 0) {
            ed_delete_at(pos - 1);
            editor.cursor_col--;
        } else if (editor.cursor_row > 0) {
            // Join with the previous row (delete the '\n')
            uint32_t prev_len = ed_line_len(editor.cursor_row - 1);
            ed_delete_at(pos - 1);
            editor.cursor_row--;
            editor.cursor_col = (int)prev_len;
        }
    }
    else if (key == '\n' || key == '\r') {       // Enter (LF + CR — FIX E2)
        ed_insert_char('\n');
        editor.cursor_row++;
        editor.cursor_col = 0;
    }
    else if (key == -3) {                        // Left
        if (editor.cursor_col > 0) editor.cursor_col--;
        else if (editor.cursor_row > 0) {
            editor.cursor_row--;
            editor.cursor_col = (int)ed_line_len(editor.cursor_row);
        }
    }
    else if (key == -4) {                        // Right
        uint32_t len = ed_line_len(editor.cursor_row);
        uint32_t pos = ed_pos_of(editor.cursor_row, editor.cursor_col);
        if ((uint32_t)editor.cursor_col < len) editor.cursor_col++;
        else if (pos < editor.size) {            // to the start of the next row
            editor.cursor_row++;
            editor.cursor_col = 0;
        }
    }
    else if (key == -1) {                        // Up
        if (editor.cursor_row > 0) {
            editor.cursor_row--;
            if ((uint32_t)editor.cursor_col > ed_line_len(editor.cursor_row))
                editor.cursor_col = (int)ed_line_len(editor.cursor_row);
        }
    }
    else if (key == -2) {                        // Down
        if (editor.cursor_row < editor.line_count - 1) {
            editor.cursor_row++;
            if ((uint32_t)editor.cursor_col > ed_line_len(editor.cursor_row))
                editor.cursor_col = (int)ed_line_len(editor.cursor_row);
        }
    }
    else if (key == -5) editor.cursor_col = 0;   // Home
    else if (key == -6) {                        // End
        editor.cursor_col = (int)ed_line_len(editor.cursor_row);
    }
    else if (key == -7) {                        // PageUp
        editor.cursor_row -= text_rows;
        if (editor.cursor_row < 0) editor.cursor_row = 0;
        if ((uint32_t)editor.cursor_col > ed_line_len(editor.cursor_row))
            editor.cursor_col = (int)ed_line_len(editor.cursor_row);
    }
    else if (key == -8) {                        // PageDown
        editor.cursor_row += text_rows;
        if (editor.cursor_row > editor.line_count - 1)
            editor.cursor_row = editor.line_count - 1;
        if ((uint32_t)editor.cursor_col > ed_line_len(editor.cursor_row))
            editor.cursor_col = (int)ed_line_len(editor.cursor_row);
    }
    else if (key == -9) {                        // Delete
        uint32_t pos = ed_pos_of(editor.cursor_row, editor.cursor_col);
        ed_delete_at(pos);
    }

    ed_recalc_states();
    ed_ensure_visible();
}

// ============================================================
//  PUBLIC API
// ============================================================
void editor_open(const char* fullpath) {
    /* FIX(R1): TUI mode — disable console auto-scroll while the
     * editor is alive. Without this, drawing the full-width status
     * row on the LAST row triggers wrap->scroll in put_char and the
     * WHOLE screen shifts one row per keystroke (the "text appears
     * briefly then vanishes" bug). ed_restore_terminal() restores it
     * on every exit path. */
    term_set_scroll(0);

    // ---- Layout: the whole screen, title + status bar ----
    scr_cols  = term_get_cols();
    scr_rows  = term_get_rows();
    text_cols = scr_cols - GUTTER_WIDTH;
    text_rows = scr_rows - 2;
    if (text_cols < 8) text_cols = 8;
    if (text_rows < 1) text_rows = 1;
    if (text_rows > ED_MAX_ROWS) text_rows = ED_MAX_ROWS;

    editor.max_size = MAX_BUFFER_SIZE;
    editor.buffer = (char*)malloc(editor.max_size + 1);
    if (!editor.buffer) {
        ed_restore_terminal();
        printf("Editor: out of memory\n");
        return;
    }
    editor.buffer[0] = '\0';
    editor.cursor_row = 0;
    editor.cursor_col = 0;
    editor.top_line = 0;
    editor.left_col = 0;
    editor.dirty = 0;
    editor.line_count = 1;
    editor.highlight = 0;

    if (strlen(fullpath) >= sizeof(editor.fullpath)) {
        ed_restore_terminal();
        printf("Editor: path too long (max 255)\n");
        free(editor.buffer);
        return;
    }
    strcpy(editor.fullpath, fullpath);
    const char* last_slash = strrchr(fullpath, '/');
    const char* fname = last_slash ? last_slash + 1 : fullpath;
    size_t flen = strlen(fname);
    if (flen >= sizeof(editor.filename)) flen = sizeof(editor.filename) - 1;
    for (size_t i = 0; i < flen; i++) editor.filename[i] = fname[i];
    editor.filename[flen] = '\0';

    // Syntax highlighting for C sources (.c / .h)
    size_t elen = strlen(editor.filename);
    if (elen >= 2) {
        const char* ext = editor.filename + elen - 2;
        if (ext[0] == '.' && (ext[1] == 'c' || ext[1] == 'h')) {
            editor.highlight = 1;
        }
    }

    editor_load(fullpath);
    ed_recalc_states();

    /* FIX(v10.4 render): full redraw on open. shadow_hash[] is static
     * and SURVIVES across editor sessions, but the shell repaints the
     * screen in between (prompt, command output, clear_screen). On a
     * second `edit` the incremental path would compare against hashes
     * from the PREVIOUS session, consider empty rows "unchanged", and
     * never paint them — stale shell text bleeds through the editor
     * text area (reproduced: edit a new file after running commands).
     * ed_render_all() draws every row unconditionally and refreshes
     * the shadow hashes, so the screen is rebuild from scratch here. */
    term_set_cursor_visible(0);
    ed_render_all();
    ed_place_cursor();
    term_set_cursor_visible(1);

    while (1) {
        int key = getkey();
        if (key == 17) {                          // Ctrl+Q
            if (editor.dirty) {
                term_set_cursor_visible(0);
                set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
                set_cursor_position(scr_rows - 1, 0);
                for (int i = 0; i < scr_cols; i++) put_char(' ');
                set_cursor_position(scr_rows - 1, 0);
                print_string(" Save changes? (y/n) ");
                char ans = getchar();
                if (ans == 'y' || ans == 'Y') {
                    editor_save();               // save...
                    break;                       // ...then quit (FIX E5)
                } else if (ans == 'n' || ans == 'N') {
                    break;                       // discard changes, quit
                }
                /* FIX(E5): before, any answer (including 'n') just did
                 * `continue` -> the prompt appeared AGAIN on the next Ctrl+Q
                 * and the editor could never be closed without saving.
                 * Now: y = save & quit, n = discard & quit, any other
                 * key = cancel (back to editing). */
                shadow_status = 0x5A5A5A5Au;     // restore the status bar
                ed_render_frame();
                continue;
            } else break;
        } else if (key == 19) {                   // Ctrl+S
            editor_save();
            ed_render_frame();
            continue;
        }
        editor_process_key(key);
        ed_render_frame();
    }

    free(editor.buffer);
    ed_restore_terminal();
    clear_screen();
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printf("Editor closed.\n");
}
