#include "../header/settings.h"
#include "../header/stdio.h"
#include "../header/color.h"
#include "../header/tui.h"
#include "../header/timer.h"
#include "../header/malloc.h"
#include "../header/libstring.h"   // <-- tambahkan
#include "../header/itoa_atoi.h"   // for itoa? But settings uses get_heap_used
#include <stdint.h>
#include <stddef.h>

// ======================== GLOBAL SETTINGS ========================
static char settings_username[32] = "myos_user";
static char settings_hostname[32] = "myos_pc";
static int  settings_show_clock = 1;
static int  settings_auto_save = 0;
static int  settings_debug_mode = 0;
static int  color_scheme_index = 0;
static const char* color_schemes[] = {"Default", "Dark", "Light", "Green", "Amber"};

// ======================== COMMON ========================
static void draw_bottom_bar(const char* msg) {
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE);
    set_cursor_position(24, 0);
    for (int i = 0; i < 80; i++) put_char(' ');
    set_cursor_position(24, 0);
    printf("%s", msg);
}

// ======================== PAGE: USER ========================
static void page_user(void) {
    char temp_username[32];
    char temp_hostname[32];
    strcpy(temp_username, settings_username);
    strcpy(temp_hostname, settings_hostname);
    int focus = 0;
    while (1) {
        set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        clear_screen();
        set_cursor_position(1, 28);
        printf("=== USER SETTINGS ===");
        // Username
        tui_label_t label1;
        label1.x = 15; label1.y = 4;
        strcpy(label1.text, "Username:");
        label1.color = VGA_COLOR_WHITE;
        tui_draw_label(&label1);
        tui_input_t input_username;
        input_username.x = 30; input_username.y = 4; input_username.width = 25;
        input_username.label[0] = '\0';
        strcpy(input_username.value, temp_username);
        input_username.cursor_pos = strlen(temp_username);
        input_username.is_focused = (focus == 0);
        input_username.on_enter = NULL;
        input_username.user_data = NULL;
        tui_draw_input(&input_username);
        // Hostname
        tui_label_t label2;
        label2.x = 15; label2.y = 6;
        strcpy(label2.text, "Hostname:");
        label2.color = VGA_COLOR_WHITE;
        tui_draw_label(&label2);
        tui_input_t input_hostname;
        input_hostname.x = 30; input_hostname.y = 6; input_hostname.width = 25;
        input_hostname.label[0] = '\0';
        strcpy(input_hostname.value, temp_hostname);
        input_hostname.cursor_pos = strlen(temp_hostname);
        input_hostname.is_focused = (focus == 1);
        input_hostname.on_enter = NULL;
        input_hostname.user_data = NULL;
        tui_draw_input(&input_hostname);
        draw_bottom_bar("Arrow keys to switch  ENTER to save  ESC to cancel");
        int key = getkey();
        if (key == 27) break;
        else if (key == -1 || key == -2) {
            focus = (focus + 1) % 2;
            continue;
        } else if (key == '\n') {
            strcpy(settings_username, temp_username);
            strcpy(settings_hostname, temp_hostname);
            tui_alert("Success", "User settings saved");
            break;
        } else {
            if (focus == 0) {
                tui_input_handle(&input_username, key);
                /* FIX(audit V3 #2): input value[64] -> temp[32] wajib
                 * bounded. It used to be safe ONLY because width was hardcoded to 25
                 * (tui_input_handle limits len < width-1); if width
                 * dinaikkan -> stack overflow senyap. Guard eksplisit
                 * biar keamanan gak titip ke nilai width. */
                strncpy(temp_username, input_username.value, sizeof(temp_username) - 1);
                temp_username[sizeof(temp_username) - 1] = '\0';
            } else {
                tui_input_handle(&input_hostname, key);
                strncpy(temp_hostname, input_hostname.value, sizeof(temp_hostname) - 1);
                temp_hostname[sizeof(temp_hostname) - 1] = '\0';
            }
        }
    }
}

// ======================== PAGE: APPEARANCE ========================
static void page_appearance(void) {
    int temp_color = color_scheme_index;
    int temp_clock = settings_show_clock;
    int focus = 0;
    tui_dropdown_t dd;
    dd.x = 15; dd.y = 4; dd.width = 20;
    strcpy(dd.label, "Color Scheme");
    dd.options = (char**)color_schemes;
    dd.option_count = 5;
    dd.selected_index = temp_color;
    dd.is_expanded = 0;
    dd.on_select = NULL;
    dd.user_data = NULL;
    tui_toggle_t toggle_clock;
    toggle_clock.x = 15; toggle_clock.y = 6;
    strcpy(toggle_clock.label, "Show Clock");
    toggle_clock.value = temp_clock;
    toggle_clock.on_change = NULL;
    toggle_clock.user_data = NULL;
    while (1) {
        set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        clear_screen();
        set_cursor_position(1, 28);
        printf("=== APPEARANCE ===");
        tui_draw_dropdown(&dd);
        tui_draw_toggle(&toggle_clock);
        draw_bottom_bar("Arrows move  ENTER select/expand  S save  ESC cancel");
        int key = getkey();
        if (key == 27) {
            /* FIX(T1): ESC closes the dropdown first; the second ESC exits.
             * (The old `else if (key == 27 && dd.is_expanded)` branch was
             * never executed because key==27 already broke out above.) */
            if (dd.is_expanded) dd.is_expanded = 0;
            else break;
        }
        else if (key == -1 || key == -2) {
            if (dd.is_expanded) {
                tui_dropdown_handle(&dd, key);
                temp_color = dd.selected_index;
            } else {
                focus = (focus + 1) % 2;
            }
            continue;
        } else if (key == '\n') {
            /* FIX(T2): ENTER purely expands/collapses/selects & toggles.
             * Before: as soon as the dropdown collapsed or the clock toggled, the
             * "if (key == '\n' ...)" block below immediately saved+exited. */
            if (focus == 0) {
                if (dd.is_expanded) {
                    temp_color = dd.selected_index;
                    dd.is_expanded = 0;
                } else {
                    dd.is_expanded = 1;
                }
            } else {
                temp_clock = !temp_clock;
                toggle_clock.value = temp_clock;
            }
            continue;
        } else if (key == 's' || key == 'S') {
            /* explicit save — consistent with page_system() */
            color_scheme_index = temp_color;
            settings_show_clock = temp_clock;
            tui_alert("Success", "Appearance saved");
            break;
        } else {
            if (focus == 0 && dd.is_expanded) {
                tui_dropdown_handle(&dd, key);
                temp_color = dd.selected_index;
            }
        }
    }
}

// ======================== PAGE: SYSTEM ========================
static void page_system(void) {
    int temp_autosave = settings_auto_save;
    int temp_debug = settings_debug_mode;
    int focus = 0;
    tui_toggle_t toggle_autosave;
    toggle_autosave.x = 15; toggle_autosave.y = 4;
    strcpy(toggle_autosave.label, "Auto Save");
    toggle_autosave.value = temp_autosave;
    toggle_autosave.on_change = NULL;
    toggle_autosave.user_data = NULL;
    tui_toggle_t toggle_debug;
    toggle_debug.x = 15; toggle_debug.y = 6;
    strcpy(toggle_debug.label, "Debug Mode");
    toggle_debug.value = temp_debug;
    toggle_debug.on_change = NULL;
    toggle_debug.user_data = NULL;
    while (1) {
        set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        clear_screen();
        set_cursor_position(1, 28);
        printf("=== SYSTEM SETTINGS ===");
        tui_draw_toggle(&toggle_autosave);
        tui_draw_toggle(&toggle_debug);
        draw_bottom_bar("Arrow keys  ENTER to toggle  ESC to cancel");
        int key = getkey();
        if (key == 27) break;
        else if (key == -1 || key == -2) {
            focus = (focus + 1) % 2;
            continue;
        } else if (key == '\n') {
            if (focus == 0) {
                temp_autosave = !temp_autosave;
                toggle_autosave.value = temp_autosave;
            } else {
                temp_debug = !temp_debug;
                toggle_debug.value = temp_debug;
            }
        } else if (key == 's' || key == 'S') {
            settings_auto_save = temp_autosave;
            settings_debug_mode = temp_debug;
            tui_alert("Success", "System settings saved");
            break;
        }
    }
}

// ======================== MAIN MENU ========================
static void draw_main_menu(int selected) {
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    clear_screen();
    set_cursor_position(1, 30);
    printf("=== SETTINGS ===");
    const char* menu_items[] = {"User Settings", "Appearance", "System"};
    int count = 3;
    for (int i = 0; i < count; i++) {
        int x = 25, y = 4 + i * 2;
        if (i == selected) {
            set_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
        } else {
            set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        }
        set_cursor_position(y, x);
        if (i == selected) {
            printf("> ");
        } else {
            printf("  ");
        }
        printf("%s", menu_items[i]);
        int len = 0;
        while (menu_items[i][len]) len++;
        for (int j = len; j < 30; j++) put_char(' ');
    }
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE);
    draw_bottom_bar("Arrow keys to select  ENTER to open  ESC to close");
}

// ======================== PUBLIC ========================
void settings_open(void) {
    int selected = 0;
    draw_main_menu(selected);
    while (1) {
        int key = getkey();
        if (key == 27) break;
        else if (key == -1) {
            selected--;
            if (selected < 0) selected = 2;
            draw_main_menu(selected);
        } else if (key == -2) {
            selected++;
            if (selected > 2) selected = 0;
            draw_main_menu(selected);
        } else if (key == '\n') {
            if (selected == 0) page_user();
            else if (selected == 1) page_appearance();
            else if (selected == 2) page_system();
            draw_main_menu(selected);
        }
    }
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    clear_screen();
    printf("Settings closed.\n");
}