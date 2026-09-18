#include "../header/tui.h"
#include "../header/stdio.h"
#include "../header/color.h"
#include "../header/libstring.h"
#include <stdint.h>
#include <stddef.h>

// ======================== WINDOW ========================
void tui_draw_window(tui_window_t* win) {
    set_color(win->fg_color, win->bg_color);
    set_cursor_position(win->y, win->x);
    put_char('+');
    for (int i = 0; i < win->width - 2; i++) put_char('-');
    put_char('+');

    int title_len = strlen(win->title);
    if (title_len > 0 && title_len < win->width - 4) {
        int start_x = win->x + (win->width - title_len) / 2;
        set_cursor_position(win->y, start_x);
        printf("%s", win->title);
    }

    for (int row = win->y + 1; row < win->y + win->height - 1; row++) {
        set_cursor_position(row, win->x);
        put_char('|');
        for (int col = 0; col < win->width - 2; col++) put_char(' ');
        put_char('|');
    }

    set_cursor_position(win->y + win->height - 1, win->x);
    put_char('+');
    for (int i = 0; i < win->width - 2; i++) put_char('-');
    put_char('+');

    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

// ======================== CLEAR AREA ========================
void tui_clear_area(int x, int y, int w, int h) {
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    for (int row = y; row < y + h; row++) {
        set_cursor_position(row, x);
        for (int col = 0; col < w; col++) put_char(' ');
    }
}

// ======================== LABEL ========================
void tui_draw_label(tui_label_t* label) {
    set_color(label->color, VGA_COLOR_BLACK);
    set_cursor_position(label->y, label->x);
    printf("%s", label->text);
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

// ======================== BUTTON ========================
void tui_draw_button(tui_button_t* btn) {
    if (btn->selected) {
        set_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
    } else {
        set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
    set_cursor_position(btn->y, btn->x);
    put_char('[');
    int len = strlen(btn->label);
    int padding = (btn->width - len - 2) / 2;
    for (int i = 0; i < padding; i++) put_char(' ');
    printf("%s", btn->label);
    for (int i = 0; i < padding; i++) put_char(' ');
    if ((btn->width - len - 2) % 2 != 0) put_char(' ');
    put_char(']');
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

// ======================== TOGGLE ========================
void tui_draw_toggle(tui_toggle_t* toggle) {
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    set_cursor_position(toggle->y, toggle->x);
    printf("%s", toggle->label);
    int label_len = strlen(toggle->label);
    set_cursor_position(toggle->y, toggle->x + label_len + 2);
    if (toggle->value) {
        set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        printf("[X]");
    } else {
        set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        printf("[ ]");
    }
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

// ======================== INPUT FIELD ========================
void tui_draw_input(tui_input_t* input) {
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    set_cursor_position(input->y, input->x);
    printf("%s:", input->label);
    int label_len = strlen(input->label);
    int field_x = input->x + label_len + 2;
    set_cursor_position(input->y, field_x);
    if (input->is_focused) {
        set_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
    } else {
        set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
    for (int i = 0; i < input->width; i++) put_char(' ');
    set_cursor_position(input->y, field_x);
    printf("%s", input->value);
    if (input->is_focused) {
        int cursor_x = field_x + input->cursor_pos;
        set_cursor_position(input->y, cursor_x);
        set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        char c = (input->value[input->cursor_pos] == '\0') ? ' ' : input->value[input->cursor_pos];
        put_char(c);
    }
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

void tui_input_handle(tui_input_t* input, int key) {
    if (!input->is_focused) return;
    if (key >= 32 && key <= 126) {
        int len = strlen(input->value);
        if (len < input->width - 1) {
            for (int i = len; i >= input->cursor_pos; i--) {
                input->value[i + 1] = input->value[i];
            }
            input->value[input->cursor_pos] = (char)key;
            input->cursor_pos++;
        }
    } else if (key == '\b') {
        if (input->cursor_pos > 0) {
            int len = strlen(input->value);
            for (int i = input->cursor_pos - 1; i < len; i++) {
                input->value[i] = input->value[i + 1];
            }
            input->cursor_pos--;
        }
    } else if (key == -3) {
        if (input->cursor_pos > 0) input->cursor_pos--;
    } else if (key == -4) {
        int len = strlen(input->value);
        if (input->cursor_pos < len) input->cursor_pos++;
    }
}

// ======================== DROPDOWN ========================
void tui_draw_dropdown(tui_dropdown_t* dd) {
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    set_cursor_position(dd->y, dd->x);
    printf("%s:", dd->label);
    int label_len = strlen(dd->label);
    int field_x = dd->x + label_len + 2;
    if (dd->selected_index >= 0 && dd->selected_index < dd->option_count) {
        set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        set_cursor_position(dd->y, field_x);
        for (int i = 0; i < dd->width; i++) put_char(' ');
        set_cursor_position(dd->y, field_x);
        printf("%s", dd->options[dd->selected_index]);
        put_char(' ');
        put_char('v');
    }
    if (dd->is_expanded) {
        int max_width = dd->width;
        for (int i = 0; i < dd->option_count; i++) {
            int len = strlen(dd->options[i]);
            if (len > max_width) max_width = len;
        }
        for (int i = 0; i < dd->option_count; i++) {
            int row = dd->y + 1 + i;
            set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            set_cursor_position(row, field_x);
            for (int c = 0; c < max_width + 2; c++) put_char(' ');
            set_cursor_position(row, field_x);
            if (i == dd->selected_index) {
                set_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
            } else {
                set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            }
            printf("%s", dd->options[i]);
        }
    }
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

void tui_dropdown_handle(tui_dropdown_t* dd, int key) {
    if (!dd->is_expanded) {
        if (key == '\n') dd->is_expanded = 1;
        return;
    }
    if (key == -1) {
        dd->selected_index--;
        if (dd->selected_index < 0) dd->selected_index = dd->option_count - 1;
    } else if (key == -2) {
        dd->selected_index++;
        if (dd->selected_index >= dd->option_count) dd->selected_index = 0;
    } else if (key == '\n') {
        dd->is_expanded = 0;
        if (dd->on_select) dd->on_select(dd->user_data);
    } else if (key == 27) {
        dd->is_expanded = 0;
    }
}

// ======================== PANEL ========================
void tui_draw_panel(tui_panel_t* panel) {
    set_color(VGA_COLOR_WHITE, panel->bg_color);
    for (int row = panel->y; row < panel->y + panel->height; row++) {
        set_cursor_position(row, panel->x);
        for (int col = 0; col < panel->width; col++) put_char(' ');
    }
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

// ======================== MENU ========================
void tui_draw_menu(int x, int y, const char* items[], int count, int selected) {
    set_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
    int total_width = 0;
    for (int i = 0; i < count; i++) {
        total_width += strlen(items[i]) + 4;
    }
    set_cursor_position(y, x);
    for (int i = 0; i < total_width; i++) put_char(' ');
    int current_x = x;
    for (int i = 0; i < count; i++) {
        if (i == selected) {
            set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        } else {
            set_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
        }
        set_cursor_position(y, current_x);
        put_char(' ');
        printf("%s", items[i]);
        put_char(' ');
        current_x += strlen(items[i]) + 4;
    }
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

// ======================== ALERT ========================
void tui_alert(const char* title, const char* message) {
    int msg_len = strlen(message);
    int title_len = strlen(title);
    int w = (msg_len > title_len) ? msg_len + 6 : title_len + 6;
    if (w < 30) w = 30;
    int h = 5;
    int x = (80 - w) / 2;
    int y = (25 - h) / 2;
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    for (int row = y; row < y + h; row++) {
        set_cursor_position(row, x);
        for (int col = 0; col < w; col++) {
            if (row == y || row == y + h - 1) {
                if (col == 0 || col == w - 1) put_char('+');
                else put_char('-');
            } else {
                if (col == 0 || col == w - 1) put_char('|');
                else put_char(' ');
            }
        }
    }
    set_cursor_position(y, x + (w - title_len) / 2);
    printf("%s", title);
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    set_cursor_position(y + 2, x + (w - msg_len) / 2);
    printf("%s", message);
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE);
    set_cursor_position(y + 3, x + (w - 14) / 2);
    printf("Press any key");
    getchar();
    set_color(VGA_COLOR_BLACK, VGA_COLOR_BLACK);
    for (int row = y; row < y + h; row++) {
        set_cursor_position(row, x);
        for (int col = 0; col < w; col++) put_char(' ');
    }
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}