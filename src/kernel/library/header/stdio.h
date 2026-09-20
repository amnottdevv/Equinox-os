#ifndef STDIO_H
#define STDIO_H

#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----- Output -----
void put_char(char c);
void print_string(const char* str);
void print_int(uint32_t num);
void clear_screen(void);
int printf(const char* format, ...);

// ----- Color & Cursor -----
void set_color(uint8_t fg, uint8_t bg);
void set_fg_rgb(uint32_t rgb);   // direct 24-bit fg (VESA); VGA snaps to white/light-magenta
void set_default_color(void);
void update_cursor(void);
void set_cursor_position(int row, int col);

// Active console size (VGA: 80x25, VESA: fb_width/8 x fb_height/16)
int term_get_cols(void);
int term_get_rows(void);

// ----- TUI mode (FIX R1 — screen shifted on every keypress bug) -----
// term_set_scroll(0) disables put_char auto-scroll: wrapping on the
// last row merely clamps the cursor, it does NOT shift the screen
// contents. Required for full-screen apps (editor/TUI) that draw
// screen-wide rows on the last row; it MUST be restored to 1 before
// returning to the shell. Default: 1 (normal console behavior).
void term_set_scroll(int enable);
int  term_get_scroll(void);

// ----- Quiet cursor (FIX R2 — cursor artifacts during bulk renders) -----
// term_set_cursor_visible(0) turns off ALL cursor drawing
// (put_char / set_cursor_position / blink ISR) during bulk renders;
// the app calls 1 back ONCE after the final cursor is placed.
// Erasing the old cursor uses the color remembered at draw time, so
// intervening set_color changes no longer leave colored-block
// residue in the text area. Default: 1 (cursor visible).
void term_set_cursor_visible(int visible);

// Cursor blink tick — called by the timer ISR (IRQ0, 100 Hz).
// Internal; do not call it manually from other kernel code.
void term_cursor_tick(void);

// ----- Display init (call once after VESA init) -----
// Detects VESA availability and switches the rendering backend.
// After this call, all put_char/printf/clear_screen go to the
// VESA framebuffer (if available) instead of VGA text buffer.
void init_display(void);

// ----- MULTI-CONSOLE (Phase A — multitasking) -----
// Every task owns a virtual console (its own cell mirror + cursor
// state). Only the ACTIVE console is rendered to the screen and
// receives keyboard input. F1 = new console (new shell),
// F2 = previous console.
int  console_create(void);          // new console id, -1 = table full
void console_free(int id);          // release the console slot
int  console_active_id(void);       // id of the visible/focused console
int  console_count(void);            // number of consoles in use (Phase C: switch)
int  console_next_used(int from, int dir);   // next used id (dir +/-1)
void console_activate(int id);      // switch the visible console (full re-render)

// Per-console PIXEL canvas (graphics program isolation). A console on
// which a user program has drawn (fillrect/putpixel/blit) keeps a
// full framebuffer snapshot while it is NOT the active console, and
// the snapshot is restored when focus returns — the frozen game
// reappears exactly as it was left, instead of leaking onto other
// terminals. The buffer lives in the user physical pool (identity
// mapped), so allocation can fail gracefully under memory pressure
// (the console then falls back to plain text re-rendering).
void console_canvas_mark(void);               // active console drew pixels
void console_canvas_invalidate(int console_id); // drop the canvas (game exited)

// The CURRENT output console = owned by the running task (printf
// writes to the running task's console, not necessarily the visible
// one). Called by the scheduler after each context switch.
void term_set_output(int id);

// Force output to the ACTIVE console (used by panic so it is visible).
void term_force_active_output(void);


// ----- Keyboard input (interrupt-driven buffer) -----
int keyboard_has_data(void);      // from idt.cpp
uint8_t keyboard_read_byte(void); // from idt.cpp (blocking, busy-wait)
int keyboard_read_byte_noblock(void); // from idt.cpp: -1 = empty buffer

// Ring-buffer diagnostics (v10.5, shell `ringstats`): pending
// scancodes, dropped scancodes (overflow = drop-newest) and capacity.
void keyboard_ring_stats(uint32_t* count, uint32_t* drops, uint32_t* cap);

// Scancode-to-ASCII lookup table (shared with LVGL input driver)
extern const char scancode_to_ascii[128];

char getchar(void);
void gets(char* buffer, int max);
int getkey(void);

// Non-blocking variant for game loops (SYS_POLLKEY): 0 = no key
// waiting, >0 = ASCII code, <0 = special key (same codes as getkey:
// -1 Up, -2 Down, -3 Left, -4 Right, -5..-9 Home/End/PgUp/PgDn/Del).
// 0 is the "empty" marker so it never collides with arrow codes.
int getkey_poll(void);

// v10.9 (DOOM): non-blocking PRESS+RELEASE event decoder.
// Returns 1 and fills *pressed (1 = key down, 0 = key up) + the RAW
// key code (unshifted ASCII, or negative specials -1..-9 arrows et al,
// -10 Ctrl, -11 Shift, -12 Alt, -13..-22 F1..F10, -23 F11, -24 F12).
// Returns 0 when no event is pending. Modifier/F-key events are NEW
// information only visible through this API — getkey/getkey_poll/
// getchar keep their old press-only, shift-transformed behavior.
int getkey_event(int* pressed);

// ----- Command history (shell line recall, up/down arrows) -----
// Ring buffer of the last commands typed through gets_history().
// history_add() skips empty lines and consecutive duplicates.
void history_add(const char* cmd);
int  history_count(void);

// Arrow-aware line editor for the shell prompt:
//   Up/Down     = walk through history (Up = older, Down = newer)
//   typing      = detach from history browsing back to the live line
//   Enter       = finish (line is NOT added to history here — the
//                 caller decides what deserves to be remembered)
// Other special keys (Left/Right/Home/End/...) are ignored for now.
// Fills `buffer` exactly like gets(): NUL-terminated, max `max` chars.
void gets_history(char* buffer, int max);

// ----- Port I/O -----
uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t val);
uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t val);
/* v0.3 (FR-12): 32-bit port I/O — PCI config space (0xCF8/0xCFC) */
uint32_t inl(uint16_t port);
void outl(uint16_t port, uint32_t val);

// ----- System -----
void reboot(void);

#ifdef __cplusplus
}
#endif

#endif