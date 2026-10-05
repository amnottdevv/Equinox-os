#include "morphtui.h"
#include "../header/stdio.h"
#include "../header/libstring.h"
#include "../header/malloc.h"

// ============================================================
// C++ runtime stubs needed because this kernel links -nostdlib
// with no libsupc++. Anything with a pure virtual function
// (Widget::draw_self()) makes the compiler emit a reference to
// __cxa_pure_virtual in the vtable as a safety-net entry - it's
// only actually called if a pure virtual gets invoked before a
// derived override exists (e.g. from within a base constructor),
// which should never happen here, so we just halt loudly instead
// of jumping into undefined memory.
// ============================================================
extern "C" void __cxa_pure_virtual() {
    asm volatile("cli");
    asm volatile("hlt");
    while (1) {}
}

// ============================================================
// Minimal malloc-backed operator new/delete.
// No exceptions in this build (-fno-exceptions), so these never
// throw - a failed allocation returns NULL and the caller is
// responsible for checking (kernel code, not hosted C++, so we
// don't get the luxury of bad_alloc anyway).
// ============================================================
void* operator new(size_t size) {
    void* p = malloc(size);
    if (!p) {
        // malloc.cpp's allocator is a bump allocator with a no-op
        // free() right now, so this WILL eventually trigger with
        // heavy dialog/widget use. Fail loud instead of silently
        // constructing an object at address 0.
        printf("MorphTUI: out of memory, halting.\n");
        asm volatile("cli");
        asm volatile("hlt");
        while (1) {}
    }
    return p;
}
void* operator new[](size_t size) {
    return operator new(size);
}
void operator delete(void* ptr) {
    free(ptr);
}
void operator delete[](void* ptr) {
    free(ptr);
}
void operator delete(void* ptr, size_t) {
    free(ptr);
}
void operator delete[](void* ptr, size_t) {
    free(ptr);
}

// ============================================================
// Widget
// ============================================================
Widget::Widget(Rect b)
    : bounds(b), fg_color(VGA_COLOR_WHITE), bg_color(VGA_COLOR_BLUE),
      dirty_(true), focused_(false) {}

Widget::~Widget() {}

void Widget::mark_dirty() { dirty_ = true; }

void Widget::set_focused(bool f) {
    if (focused_ != f) {
        focused_ = f;
        mark_dirty();
    }
}

void Widget::draw() {
    if (!dirty_) return;
    draw_self();
    dirty_ = false;
}

void Widget::fill(char c, uint8_t fg, uint8_t bg) {
    set_color(fg, bg);
    for (int r = 0; r < bounds.height; r++) {
        set_cursor_position(bounds.row + r, bounds.col);
        for (int cidx = 0; cidx < bounds.width; cidx++) put_char(c);
    }
}

void Widget::print_at(int row, int col, const char* text, uint8_t fg, uint8_t bg) {
    set_color(fg, bg);
    set_cursor_position(row, col);
    printf("%s", text);
}

// ============================================================
// Window
// ============================================================
Window::Window(Rect b, const char* title)
    : border_color(VGA_COLOR_LIGHT_GREY), bg_color(VGA_COLOR_BLUE),
      title_color(VGA_COLOR_WHITE),
      bounds_(b), widget_count_(0), focus_index_(-1),
      running_(false), exit_code_(-1) {
    strncpy(title_, title ? title : "", 63);
    title_[63] = '\0';
}

Window::~Window() {
    for (int i = 0; i < widget_count_; i++) delete widgets_[i];
}

void Window::add_widget(Widget* w) {
    if (widget_count_ >= MORPHTUI_MAX_WIDGETS) return;
    widgets_[widget_count_++] = w;
    if (focus_index_ < 0 && w->focusable()) {
        focus_index_ = widget_count_ - 1;
        w->set_focused(true);
    }
}

void Window::set_title(const char* title) {
    // Clear the whole title line span first (old title may have
    // been longer than the new one) then draw the new text.
    set_color(border_color, bg_color);
    set_cursor_position(bounds_.row, bounds_.col + 1);
    for (int i = 0; i < bounds_.width - 2; i++) put_char('-');

    strncpy(title_, title ? title : "", 63);
    title_[63] = '\0';
    if (title_[0]) {
        set_color(title_color, bg_color);
        set_cursor_position(bounds_.row, bounds_.col + 2);
        printf(" %s ", title_);
    }
}

void Window::focus_next() {
    if (widget_count_ == 0) return;
    int start = focus_index_;
    int i = focus_index_;
    for (int tries = 0; tries < widget_count_; tries++) {
        i = (i + 1) % widget_count_;
        if (widgets_[i]->focusable()) {
            if (start >= 0) widgets_[start]->set_focused(false);
            widgets_[i]->set_focused(true);
            focus_index_ = i;
            return;
        }
    }
}

Widget* Window::focused_widget() {
    if (focus_index_ < 0 || focus_index_ >= widget_count_) return NULL;
    return widgets_[focus_index_];
}

void Window::draw() {
    // Border + title drawn once per open (not every keystroke -
    // this whole block only runs from run() on first entry, see
    // below; widgets still redraw themselves only when dirty).
    set_color(border_color, bg_color);
    for (int r = 0; r < bounds_.height; r++) {
        set_cursor_position(bounds_.row + r, bounds_.col);
        put_char('|');
        set_cursor_position(bounds_.row + r, bounds_.col + bounds_.width - 1);
        put_char('|');
    }
    set_cursor_position(bounds_.row, bounds_.col);
    put_char('+');
    for (int i = 0; i < bounds_.width - 2; i++) put_char('-');
    put_char('+');
    set_cursor_position(bounds_.row + bounds_.height - 1, bounds_.col);
    put_char('+');
    for (int i = 0; i < bounds_.width - 2; i++) put_char('-');
    put_char('+');

    if (title_[0]) {
        set_color(title_color, bg_color);
        set_cursor_position(bounds_.row, bounds_.col + 2);
        printf(" %s ", title_);
    }

    for (int i = 0; i < widget_count_; i++) widgets_[i]->draw();
}

void Window::request_close(int code) {
    running_ = false;
    exit_code_ = code;
}

int Window::run() {
    running_ = true;
    exit_code_ = -1;

    // Fill interior once so widgets don't each need to clear
    // their own background on first paint.
    set_color(VGA_COLOR_WHITE, bg_color);
    for (int r = 1; r < bounds_.height - 1; r++) {
        set_cursor_position(bounds_.row + r, bounds_.col + 1);
        for (int c = 0; c < bounds_.width - 2; c++) put_char(' ');
    }

    draw();

    while (running_) {
        int key = getkey();

        if (key == KEY_ESC) {
            request_close(-1);
            break;
        }

        bool consumed = false;
        Widget* fw = focused_widget();
        if (fw) consumed = fw->handle_key(key);

        if (!consumed) {
            on_unhandled_key(key);
        }
        on_after_key();

        // Redraw only what marked itself dirty.
        for (int i = 0; i < widget_count_; i++) widgets_[i]->draw();
    }

    return exit_code_;
}

// ============================================================
// Label
// ============================================================
Label::Label(Rect b, const char* text, uint8_t color) : Widget(b) {
    fg_color = color;
    strncpy(text_, text ? text : "", 127);
    text_[127] = '\0';
}

void Label::set_text(const char* text) {
    strncpy(text_, text ? text : "", 127);
    text_[127] = '\0';
    mark_dirty();
}

void Label::draw_self() {
    print_at(bounds.row, bounds.col, text_, fg_color, bg_color);
}

// ============================================================
// ListBox
// ============================================================
ListBox::ListBox(Rect b)
    : Widget(b), item_count_(0), selected_(0), scroll_offset_(0),
      renderer_(NULL), user_data_(NULL) {}

void ListBox::set_item_count(int count) {
    item_count_ = count;
    if (selected_ >= item_count_) selected_ = item_count_ - 1;
    if (selected_ < 0) selected_ = 0;
    clamp_scroll();
    mark_dirty();
}

void ListBox::set_renderer(ListBoxItemRenderer renderer, void* user_data) {
    renderer_ = renderer;
    user_data_ = user_data;
    mark_dirty();
}

void ListBox::set_selected_index(int i) {
    if (i < 0) i = 0;
    if (i >= item_count_) i = item_count_ - 1;
    if (i != selected_) {
        selected_ = i;
        clamp_scroll();
        mark_dirty();
    }
}

void ListBox::clamp_scroll() {
    int visible = bounds.height;
    if (selected_ < scroll_offset_) scroll_offset_ = selected_;
    if (selected_ >= scroll_offset_ + visible) scroll_offset_ = selected_ - visible + 1;
    if (scroll_offset_ < 0) scroll_offset_ = 0;
}

bool ListBox::handle_key(int key) {
    if (item_count_ == 0) return false;
    if (key == KEY_UP) {
        set_selected_index(selected_ - 1);
        return true;
    } else if (key == KEY_DOWN) {
        set_selected_index(selected_ + 1);
        return true;
    } else if (key == KEY_PGUP) {
        set_selected_index(selected_ - bounds.height);
        return true;
    } else if (key == KEY_PGDN) {
        set_selected_index(selected_ + bounds.height);
        return true;
    } else if (key == KEY_HOME) {
        set_selected_index(0);
        return true;
    } else if (key == KEY_END) {
        set_selected_index(item_count_ - 1);
        return true;
    }
    return false;
}

void ListBox::draw_self() {
    char text[128];
    uint8_t fg;

    for (int row = 0; row < bounds.height; row++) {
        int idx = scroll_offset_ + row;
        set_cursor_position(bounds.row + row, bounds.col);

        if (idx < item_count_ && renderer_) {
            text[0] = '\0';
            fg = fg_color;
            renderer_(idx, user_data_, text, sizeof(text), &fg);

            uint8_t row_bg = bg_color;
            if (idx == selected_) {
                fg = VGA_COLOR_BLACK;
                row_bg = VGA_COLOR_LIGHT_GREY;
            }
            set_color(fg, row_bg);

            int len = strlen(text);
            printf("%s", text);
            for (int c = len; c < bounds.width; c++) put_char(' ');
        } else {
            set_color(fg_color, bg_color);
            for (int c = 0; c < bounds.width; c++) put_char(' ');
        }
    }
}

// ============================================================
// dialog:: helpers
// ============================================================
namespace dialog {

void alert(const char* title, const char* message) {
    Rect r = { 8, 15, 7, 50 };
    Window win(r, title);
    Rect lbl_bounds = { r.row + 2, r.col + 2, 1, r.width - 4 };
    win.add_widget(new Label(lbl_bounds, message, VGA_COLOR_WHITE));
    Rect hint_bounds = { r.row + r.height - 2, r.col + 2, 1, r.width - 4 };
    win.add_widget(new Label(hint_bounds, "Press ENTER or ESC to continue", VGA_COLOR_LIGHT_GREY));
    win.run();
}

bool confirm(const char* title, const char* message) {
    Rect r = { 8, 15, 7, 50 };
    Window win(r, title);
    Rect lbl_bounds = { r.row + 2, r.col + 2, 1, r.width - 4 };
    win.add_widget(new Label(lbl_bounds, message, VGA_COLOR_WHITE));
    Rect hint_bounds = { r.row + r.height - 2, r.col + 2, 1, r.width - 4 };
    win.add_widget(new Label(hint_bounds, "Y: Yes   N/ESC: No", VGA_COLOR_LIGHT_GREY));

    win.draw();
    while (1) {
        int key = getkey();
        if (key == 'y' || key == 'Y') return true;
        if (key == 'n' || key == 'N' || key == KEY_ESC) return false;
    }
}

bool prompt(const char* title, const char* label, char* buffer, int buffer_size) {
    Rect r = { 8, 15, 8, 50 };
    Window win(r, title);
    Rect lbl_bounds = { r.row + 2, r.col + 2, 1, r.width - 4 };
    win.add_widget(new Label(lbl_bounds, label, VGA_COLOR_WHITE));

    // Minimal inline text entry (a full InputBox widget with
    // cursor/editing is a natural follow-up; kept out of this
    // pass to stay focused on ListBox, which is what
    // filemanager.cpp actually needs first).
    win.draw();
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    set_cursor_position(r.row + 4, r.col + 2);
    printf("> ");

    char tmp[128];
    int i = 0;
    tmp[0] = '\0';
    while (1) {
        int key = getkey();
        if (key == KEY_ESC) return false;
        if (key == KEY_ENTER) {
            strncpy(buffer, tmp, buffer_size - 1);
            buffer[buffer_size - 1] = '\0';
            return true;
        }
        if ((key == KEY_BACKSPACE || key == KEY_DELETE) && i > 0) {
            i--;
            tmp[i] = '\0';
            set_cursor_position(r.row + 4, r.col + 4 + i);
            put_char(' ');
            set_cursor_position(r.row + 4, r.col + 4 + i);
        } else if (key >= 32 && key <= 126 && i < buffer_size - 1 && i < (int)sizeof(tmp) - 1) {
            tmp[i] = (char)key;
            tmp[i + 1] = '\0';
            set_cursor_position(r.row + 4, r.col + 4 + i);
            put_char((char)key);
            i++;
        }
    }
}

} // namespace dialog