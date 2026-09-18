#ifndef MORPHTUI_H
#define MORPHTUI_H

// ============================================================
//  MorphTUI - widget-based TUI library for Equinox OS
//
//  Design notes:
//   - No RTTI, no exceptions (freestanding kernel). Build with
//     -fno-rtti -fno-exceptions.
//   - Widgets are heap-allocated via `new` (see morphtui.cpp for
//     the malloc-backed operator new/delete). Never declare a
//     Widget/Window as a global/static object - global ctors are
//     not run by this kernel (no .init_array wiring yet).
//   - Only ONE Window is ever "active" (modal stack via
//     Window::push_modal / pop_modal), matching the old
//     clear_screen()-per-open pattern from filemanager.cpp.
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include "../header/color.h"

// ------------------------------------------------------------
// Rect: simple bounding box, row/col to match set_cursor_position(row,col)
// ------------------------------------------------------------
struct Rect {
    int row, col;
    int height, width;
};

// ------------------------------------------------------------
// Key codes not already covered by getkey()'s arrow-key negatives.
// getkey() returns: ASCII for normal keys, -1..-9 for special
// (Up,Down,Right,Left,Home,End,PgUp,PgDn,Del - see stdio.cpp),
// 27 for ESC, 8/127 for backspace/delete.
// ------------------------------------------------------------
enum MorphKey {
    KEY_ESC       = 27,
    KEY_ENTER     = '\n',
    KEY_BACKSPACE = 8,
    KEY_DELETE    = 127,
    KEY_UP        = -1,
    KEY_DOWN      = -2,
    KEY_LEFT      = -3,
    KEY_RIGHT     = -4,
    KEY_HOME      = -5,
    KEY_PGDN      = -6,
    KEY_END       = -7,
    KEY_PGUP      = -8,
};

// ------------------------------------------------------------
// Widget: abstract base for everything drawable/focusable.
// ------------------------------------------------------------
class Widget {
public:
    Widget(Rect bounds);
    virtual ~Widget();

    // Draw only if dirty; child classes implement draw_self().
    void draw();
    void mark_dirty();
    bool dirty() const { return dirty_; }

    // Returns true if the widget consumed the key.
    virtual bool handle_key(int key) { (void)key; return false; }

    virtual bool focusable() const { return false; }

    void set_focused(bool f);
    bool focused() const { return focused_; }

    Rect bounds;
    uint8_t fg_color;
    uint8_t bg_color;

protected:
    // Subclasses implement the actual character output here.
    virtual void draw_self() = 0;

    // Helpers available to all widgets - operate within `bounds`.
    void fill(char c, uint8_t fg, uint8_t bg);
    void print_at(int row, int col, const char* text, uint8_t fg, uint8_t bg);

private:
    bool dirty_;
    bool focused_;
};

// ------------------------------------------------------------
// Window: owns a set of widgets, runs the modal event loop.
// Draws its own border/title once, then only redraws widgets
// that report themselves dirty - no more full-screen clear on
// every keystroke.
// ------------------------------------------------------------
#define MORPHTUI_MAX_WIDGETS 16

class Window {
public:
    Window(Rect bounds, const char* title);
    ~Window();

    void add_widget(Widget* w);

    // Updates the title bar text in place (redraws just that
    // line, not the whole border) - lets a subclass reflect
    // state changes like "current directory" without a full
    // window repaint.
    void set_title(const char* title);

    // Focus cycling (Tab / Shift+Tab could be wired in later;
    // for now exposed so callers like ListBox-driven dialogs can
    // move focus programmatically).
    void focus_next();
    Widget* focused_widget();

    // Draws border+title (always) then all dirty children.
    void draw();

    // Runs until a widget's handle_key or the window itself
    // requests exit via request_close(). Returns the exit code
    // passed to request_close() (default -1 on ESC).
    int run();

    void request_close(int code);

    // Called by run() when the focused widget did not consume a
    // key (and it wasn't ESC). Override in a subclass to add
    // app-level shortcuts (e.g. filemanager's F/D/Ctrl+R) without
    // leaving the Widget model. Return true if handled - the
    // window will redraw dirty widgets afterward either way.
    virtual bool on_unhandled_key(int key) { (void)key; return false; }

    // Called once per key, after handle_key/on_unhandled_key,
    // regardless of whether the key was consumed. Useful for
    // syncing a status line to whatever widget state just changed
    // (e.g. ListBox selection) without duplicating key handling.
    virtual void on_after_key() {}

    uint8_t border_color, bg_color, title_color;

private:
    Rect bounds_;
    char title_[64];
    Widget* widgets_[MORPHTUI_MAX_WIDGETS];
    int widget_count_;
    int focus_index_;
    bool running_;
    int exit_code_;
};

// ------------------------------------------------------------
// Label - static text, redraws only when text/color changes.
// ------------------------------------------------------------
class Label : public Widget {
public:
    Label(Rect bounds, const char* text, uint8_t color);
    void set_text(const char* text);

protected:
    void draw_self() override;

private:
    char text_[128];
};

// ------------------------------------------------------------
// ListBox - the widget filemanager.cpp was missing. Handles its
// own scrolling; caller just feeds it item strings + an optional
// per-item tag (e.g. "is directory") via a render callback so it
// stays generic (not filesystem-specific).
// ------------------------------------------------------------
typedef void (*ListBoxItemRenderer)(int index, void* user_data,
                                     char* out_text, int out_text_size,
                                     uint8_t* out_fg);

class ListBox : public Widget {
public:
    ListBox(Rect bounds);

    void set_item_count(int count);
    void set_renderer(ListBoxItemRenderer renderer, void* user_data);

    int selected_index() const { return selected_; }
    void set_selected_index(int i);

    bool focusable() const override { return true; }
    bool handle_key(int key) override;

protected:
    void draw_self() override;

private:
    int item_count_;
    int selected_;
    int scroll_offset_;
    ListBoxItemRenderer renderer_;
    void* user_data_;

    void clamp_scroll();
};

// ------------------------------------------------------------
// Dialog - modal popup window. Common cases (alert/confirm/
// prompt) are exposed as free functions below so call sites
// don't need to hand-build a Window every time, matching the
// old tui_alert()-style ergonomics but going through the same
// Window/Widget machinery (no separate ad-hoc drawing code path).
// ------------------------------------------------------------
namespace dialog {
    // Blocking alert with a single [OK] dismiss (ESC/Enter).
    void alert(const char* title, const char* message);

    // Returns true if user confirmed (Y/Enter), false on N/ESC.
    bool confirm(const char* title, const char* message);

    // Returns true if user accepted (Enter) with buffer filled;
    // false if cancelled (ESC), buffer left untouched.
    bool prompt(const char* title, const char* label,
                char* buffer, int buffer_size);
}

#endif // MORPHTUI_H