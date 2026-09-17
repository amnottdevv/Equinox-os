#ifndef TUI_H
#define TUI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x, y;
    int width, height;
    char title[64];
    uint8_t border_color;
    uint8_t bg_color;
    uint8_t fg_color;
} tui_window_t;

typedef struct {
    int x, y;
    int width;
    char label[32];
    int selected;
    void (*on_click)(void* data);
    void* user_data;
} tui_button_t;

typedef struct {
    int x, y;
    char label[32];
    int value;
    void (*on_change)(void* data);
    void* user_data;
} tui_toggle_t;

typedef struct {
    int x, y;
    int width;
    char label[32];
    char value[64];
    int cursor_pos;
    int is_focused;
    void (*on_enter)(void* data);
    void* user_data;
} tui_input_t;

typedef struct {
    int x, y;
    int width;
    char label[32];
    char** options;
    int option_count;
    int selected_index;
    int is_expanded;
    void (*on_select)(void* data);
    void* user_data;
} tui_dropdown_t;

typedef struct {
    int x, y;
    char text[64];
    uint8_t color;
} tui_label_t;

typedef struct {
    int x, y;
    int width, height;
    uint8_t bg_color;
} tui_panel_t;

// Core functions
void tui_draw_window(tui_window_t* win);
void tui_clear_area(int x, int y, int w, int h);
void tui_draw_label(tui_label_t* label);
void tui_draw_button(tui_button_t* btn);
void tui_draw_toggle(tui_toggle_t* toggle);
void tui_draw_input(tui_input_t* input);
void tui_draw_dropdown(tui_dropdown_t* dd);
void tui_draw_panel(tui_panel_t* panel);
void tui_draw_menu(int x, int y, const char* items[], int count, int selected);

// Interaction
void tui_input_handle(tui_input_t* input, int key);
void tui_dropdown_handle(tui_dropdown_t* dd, int key);

// Alert
void tui_alert(const char* title, const char* message);

#ifdef __cplusplus
}
#endif

#endif