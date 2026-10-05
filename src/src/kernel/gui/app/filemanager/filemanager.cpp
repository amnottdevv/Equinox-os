/**
 * @file filemanager.cpp
 * @brief Paus OS File Manager — fullscreen, lightweight, no AI slop.
 *
 * ============================================================
 *  DESIGN NOTES
 * ============================================================
 *  - Fullscreen (uses 100% of screen, no lv_win border/titlebar).
 *  - Single color palette: bg / panel / list / accent / text / dim.
 *    No gradients, no shadows, no glow. Flat.
 *  - Layout is a grid: top header (40px), then body row split into
 *    sidebar (220px) + main column; bottom status bar (28px).
 *
 * ============================================================
 *  KEYBOARD
 * ============================================================
 *  Up/Down     navigate list
 *  Enter       open folder / show file info
 *  Backspace   go to parent
 *  Delete      delete selected (with confirm)
 *  F2          rename (auto-appended "_renamed")
 *  ESC         exit file manager
 *
 * ============================================================
 *  MOUSE / TOUCH
 * ============================================================
 *  Single click   select item
 *  Double click   open folder / show file info
 */

#ifdef HAS_LVGL

#include "filemanager.h"
#include "../../../library/header/fs_ram.h"
#include "../../../library/header/libstring.h"
#include "../../../library/header/malloc.h"
#include "../../../library/header/timer.h"
#include "../../../library/header/stdio.h"
#include "lvgl.h"
#include <stdint.h>
#include <stddef.h>

#ifndef LV_KEY_F2
#define LV_KEY_F2 0x101
#endif

// ============================================================
//  Section 1: Constants
// ============================================================
#define FM_SIDEBAR_W   220
#define FM_HEADER_H    40
#define FM_STATUS_H    28
#define FM_TOOLBAR_H   36

// ============================================================
//  Section 2: Color Palette — 6 flat colors, no gradient/glow
// ============================================================
namespace fm {
    static const uint32_t BG            = 0x0E1014;
    static const uint32_t PANEL         = 0x181B22;
    static const uint32_t LIST_BG       = 0x121419;
    static const uint32_t ITEM          = 0x1E2128;
    static const uint32_t ITEM_SELECTED = 0x2A3038;
    static const uint32_t BORDER        = 0x2A2D35;
    static const uint32_t TEXT          = 0xDDE2E8;
    static const uint32_t TEXT_DIM      = 0x707682;
    static const uint32_t ACCENT        = 0x56B6E0;
    static const uint32_t FOLDER        = 0xE0B056;
}

// ============================================================
//  Section 3: Static State
// ============================================================
static struct fs_node*   s_cwd           = nullptr;
static lv_obj_t*         s_root          = nullptr;
static lv_obj_t*         s_file_list     = nullptr;
static lv_obj_t*         s_status_label  = nullptr;
static lv_obj_t*         s_path_label    = nullptr;
static lv_obj_t*         s_info_label    = nullptr;
static lv_obj_t*         s_mbox          = nullptr;

static int               s_item_count    = 0;
static int               s_dir_count     = 0;
static int               s_file_count    = 0;
static int               s_sel_index     = -1;
static volatile int      s_exit_flag     = 0;

static struct fs_node**  s_file_arr      = nullptr;
static int               s_file_arr_cap  = 0;

static int               s_prev_sel      = -1; // for keyboard-navigation focus

// ============================================================
//  Section 4: Forward Declarations
// ============================================================
static void rebuild_list(void);
static void update_status(void);
static void update_path_label(void);
static void update_info_panel(void);
static void go_to_parent(void);
static void open_selected(void);
static void delete_selected(void);
static void create_file_action(void);
static void create_dir_action(void);
static void rename_selected_action(void);
static void show_properties(void);
static void show_messagebox(const char* title, const char* msg, const char* btn_text);
static struct fs_node* selected_node(void);

// ============================================================
//  Section 5: Helpers — extension detection, icon mapping
// ============================================================

static int ends_with(const char* str, const char* suffix) {
    size_t ls = strlen(str);
    size_t lf = strlen(suffix);
    if (lf == 0 || lf > ls) return 0;
    return strcmp(str + (ls - lf), suffix) == 0;
}

static const char* get_extension(const char* name) {
    const char* dot = nullptr;
    for (const char* p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    return dot;
}

static const char* file_icon(const struct fs_node* node) {
    if (!node) return LV_SYMBOL_FILE;
    if (node->is_dir) return LV_SYMBOL_DIRECTORY;
    const char* ext = get_extension(node->name);
    if (!ext) return LV_SYMBOL_FILE;
    if (ends_with(ext, ".bmp") || ends_with(ext, ".png") ||
        ends_with(ext, ".jpg") || ends_with(ext, ".jpeg") ||
        ends_with(ext, ".gif"))  return LV_SYMBOL_IMAGE;
    if (ends_with(ext, ".sav") || ends_with(ext, ".dat") ||
        ends_with(ext, ".bin") || ends_with(ext, ".exe")) return LV_SYMBOL_SAVE;
    if (ends_with(ext, ".aud") || ends_with(ext, ".wav") ||
        ends_with(ext, ".mp3") || ends_with(ext, ".mid")) return LV_SYMBOL_AUDIO;
    if (ends_with(ext, ".cfg") || ends_with(ext, ".ini") ||
        ends_with(ext, ".conf")) return LV_SYMBOL_SETTINGS;
    return LV_SYMBOL_FILE;
}

static uint32_t file_icon_color(const struct fs_node* node) {
    if (node && node->is_dir) return fm::FOLDER;
    return fm::TEXT_DIM;
}

// ============================================================
//  Section 6: Helpers — sorted array, size formatting
// ============================================================

static void format_size(uint32_t bytes, char* buf, size_t bufsz) {
    if (bytes < 1024) {
        snprintf(buf, bufsz, "%u B", bytes);
    } else if (bytes < 1024 * 1024) {
        uint32_t kb = bytes / 1024;
        uint32_t kb_frac = (bytes % 1024) * 10 / 1024;
        snprintf(buf, bufsz, "%u.%u KB", kb, kb_frac);
    } else {
        uint32_t mb = bytes / (1024 * 1024);
        uint32_t mb_frac = (bytes % (1024 * 1024)) * 10 / (1024 * 1024);
        snprintf(buf, bufsz, "%u.%u MB", mb, mb_frac);
    }
}

static void free_file_arr(void) {
    if (s_file_arr) {
        free(s_file_arr);
        s_file_arr = nullptr;
    }
    s_file_arr_cap = 0;
    s_item_count = 0;
}

static int node_cmp(const struct fs_node* a, const struct fs_node* b) {
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void build_file_arr(void) {
    free_file_arr();
    int n = 0;
    for (struct fs_node* c = s_cwd->children; c; c = c->next) n++;
    s_file_arr_cap = n;
    if (n == 0) return;
    s_file_arr = (struct fs_node**)malloc((size_t)n * sizeof(struct fs_node*));
    if (!s_file_arr) return;
    int i = 0;
    for (struct fs_node* c = s_cwd->children; c; c = c->next) {
        int j = i;
        while (j > 0 && node_cmp(c, s_file_arr[j - 1]) < 0) {
            s_file_arr[j] = s_file_arr[j - 1];
            j--;
        }
        s_file_arr[j] = c;
        i++;
    }
    s_item_count = i;
}

// ============================================================
//  Section 7: Helpers — action primitives
// ============================================================

static struct fs_node* selected_node(void) {
    if (s_sel_index < 0 || s_sel_index >= s_item_count) return nullptr;
    return s_file_arr[s_sel_index];
}

static void go_to_parent(void) {
    if (!s_cwd || !s_cwd->parent) return;
    s_cwd = s_cwd->parent;
    s_sel_index = -1;
    s_prev_sel = -1;
    rebuild_list();
    update_path_label();
    update_status();
}

static void open_selected(void) {
    struct fs_node* node = selected_node();
    if (!node) return;
    if (node->is_dir) {
        s_cwd = node;
        s_sel_index = -1;
        s_prev_sel = -1;
        rebuild_list();
        update_path_label();
        update_status();
    } else {
        show_properties();
    }
}

static void make_unique_name(const char* base, const char* ext,
                              char* out, size_t out_sz) {
    snprintf(out, out_sz, "%s%s", base, ext ? ext : "");
    if (!fs_find_child(s_cwd, out)) return;
    for (int i = 1; i < 1000; i++) {
        snprintf(out, out_sz, "%s_%d%s", base, i, ext ? ext : "");
        if (!fs_find_child(s_cwd, out)) return;
    }
}

static void create_file_action(void) {
    char name[80];
    make_unique_name("untitled", ".txt", name, sizeof(name));
    int ret = fs_create_file(s_cwd, name, "");
    if (ret == -2) {
        show_messagebox(LV_SYMBOL_WARNING " Error",
                        "Name collision (should not happen).", "OK");
    } else if (ret != 0) {
        show_messagebox(LV_SYMBOL_WARNING " Error",
                        "Failed to create file (out of memory?).", "OK");
    }
    s_sel_index = -1;
    s_prev_sel = -1;
    rebuild_list();
    update_status();
    for (int i = 0; i < s_item_count; i++) {
        if (s_file_arr[i] && strcmp(s_file_arr[i]->name, name) == 0) {
            s_sel_index = i;
            break;
        }
    }
    update_status();
}

static void create_dir_action(void) {
    char name[80];
    make_unique_name("new_folder", nullptr, name, sizeof(name));
    int ret = fs_create_dir(s_cwd, name);
    if (ret == -2) {
        show_messagebox(LV_SYMBOL_WARNING " Error",
                        "Name collision (should not happen).", "OK");
    } else if (ret != 0) {
        show_messagebox(LV_SYMBOL_WARNING " Error",
                        "Failed to create folder.", "OK");
    }
    s_sel_index = -1;
    s_prev_sel = -1;
    rebuild_list();
    update_status();
    for (int i = 0; i < s_item_count; i++) {
        if (s_file_arr[i] && strcmp(s_file_arr[i]->name, name) == 0) {
            s_sel_index = i;
            break;
        }
    }
    update_status();
}

static void rename_selected_action(void) {
    struct fs_node* node = selected_node();
    if (!node) {
        show_messagebox(LV_SYMBOL_WARNING " No Selection",
                        "Please select an item first.", "OK");
        return;
    }
    if (node->is_dir && node->children) {
        show_messagebox(LV_SYMBOL_WARNING " Cannot Rename",
                        "Cannot rename non-empty folder.", "OK");
        return;
    }

    // --- Extract the base name and extension correctly ---
    char base_only[80];
    const char* dot = get_extension(node->name);
    const char* ext = nullptr;
    if (!node->is_dir && dot) {
        // File with an extension: take the name before the last dot
        size_t len = dot - node->name;
        if (len >= sizeof(base_only)) len = sizeof(base_only) - 1;
        strncpy(base_only, node->name, len);
        base_only[len] = '\0';
        ext = dot;  // includes the dot, e.g. ".txt"
    } else {
        // Folder or extension-less file: the whole name is the base name
        strncpy(base_only, node->name, sizeof(base_only) - 1);
        base_only[sizeof(base_only)-1] = '\0';
        ext = nullptr;
    }

    // Build the new name with the "_renamed" suffix
    char base_name[80];
    snprintf(base_name, sizeof(base_name), "%s_renamed", base_only);
    char new_name[80];
    make_unique_name(base_name, ext, new_name, sizeof(new_name));

    // Create the new node; copy the content if it is a file
    const char* content = node->is_dir ? nullptr : node->content;
    int ret = node->is_dir
        ? fs_create_dir(s_cwd, new_name)
        : fs_create_file(s_cwd, new_name, content);
    if (ret != 0) {
        show_messagebox(LV_SYMBOL_WARNING " Rename",
                        "Rename failed (out of memory?).", "OK");
        return;
    }

    // Delete the old node; roll back on failure
    int del_ret = fs_delete_node(s_cwd, node->name);
    if (del_ret != 0) {
        // Rollback: remove the newly created node
        if (node->is_dir)
            fs_delete_node(s_cwd, new_name);
        else
            fs_delete_node(s_cwd, new_name);
        show_messagebox(LV_SYMBOL_WARNING " Rename Error",
                        "Could not remove original item. Rename aborted.", "OK");
        return;
    }

    s_sel_index = -1;
    s_prev_sel = -1;
    rebuild_list();
    update_status();
    // Find and select the freshly renamed item
    for (int i = 0; i < s_item_count; i++) {
        if (s_file_arr[i] && strcmp(s_file_arr[i]->name, new_name) == 0) {
            s_sel_index = i;
            break;
        }
    }
    update_status();
}

static void show_properties(void) {
    struct fs_node* node = selected_node();
    if (!node) return;
    char msg[256];
    if (node->is_dir) {
        int n = 0;
        for (struct fs_node* c = node->children; c; c = c->next) n++;
        snprintf(msg, sizeof(msg),
                 LV_SYMBOL_DIRECTORY "  %s\n\n"
                 "Type:     Folder\n"
                 "Contains: %d item(s)\n"
                 "Path:     parent=%s",
                 node->name, n,
                 node->parent ? "yes" : "(root)");
    } else {
        char size_buf[32];
        format_size(node->size, size_buf, sizeof(size_buf));
        snprintf(msg, sizeof(msg),
                 "%s  %s\n\n"
                 "Type:   File\n"
                 "Size:   %s (%u bytes)\n"
                 "Path:   parent=%s",
                 file_icon(node), node->name, size_buf, node->size,
                 node->parent ? "yes" : "(root)");
    }
    show_messagebox(LV_SYMBOL_LIST " Properties", msg, "OK");
}

static void show_messagebox(const char* title, const char* msg, const char* btn_text) {
    if (s_mbox) { lv_msgbox_close(s_mbox); s_mbox = nullptr; }
    static const char* btns[2];
    btns[0] = btn_text;
    btns[1] = "";
    s_mbox = lv_msgbox_create(NULL, title, msg, btns, false);
    lv_obj_center(s_mbox);
    lv_obj_add_event_cb(s_mbox, [](lv_event_t* e) {
        (void)e;
        if (s_mbox) { lv_msgbox_close(s_mbox); s_mbox = nullptr; }
    }, LV_EVENT_VALUE_CHANGED, nullptr);
}

// ============================================================
//  Section 8: UI refresh functions
// ============================================================

static void update_path_label(void) {
    if (!s_path_label || !s_cwd) return;
    char path[256];
    fs_get_path(s_cwd, path, sizeof(path));
    char buf[300];
    snprintf(buf, sizeof(buf), LV_SYMBOL_HOME "  %s", path);
    lv_label_set_text(s_path_label, buf);
}

static void update_status(void) {
    if (!s_status_label) return;
    char buf[160];
    if (s_sel_index >= 0 && s_sel_index < s_item_count) {
        struct fs_node* node = s_file_arr[s_sel_index];
        if (node->is_dir) {
            int children = 0;
            for (struct fs_node* c = node->children; c; c = c->next) children++;
            snprintf(buf, sizeof(buf),
                     LV_SYMBOL_DIRECTORY "  %s   |   %d items inside   |   #%d/%d",
                     node->name, children, s_sel_index + 1, s_item_count);
        } else {
            char size_buf[32];
            format_size(node->size, size_buf, sizeof(size_buf));
            snprintf(buf, sizeof(buf),
                     "%s  %s   |   %s   |   #%d/%d",
                     file_icon(node), node->name, size_buf,
                     s_sel_index + 1, s_item_count);
        }
    } else {
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_LIST "  %d items   (%d folders, %d files)",
                 s_item_count, s_dir_count, s_file_count);
    }
    lv_label_set_text(s_status_label, buf);
}

static void update_info_panel(void) {
    if (!s_info_label) return;
    char buf[320];
    uint32_t heap_used  = get_heap_used();
    uint32_t heap_total = get_heap_total();
    uint32_t heap_pct   = heap_total ? (heap_used * 100) / heap_total : 0;
    snprintf(buf, sizeof(buf),
             "#56B6E0 " LV_SYMBOL_HOME " Location#\n"
             "  Paus OS RAM FS\n\n"
             "#56B6E0 " LV_SYMBOL_LIST " Statistics#\n"
             "  Items:   %d\n"
             "  Folders: %d\n"
             "  Files:   %d\n\n"
             "#56B6E0 " LV_SYMBOL_SAVE " Memory#\n"
             "  Used: %u%%\n"
             "  %u / %u B",
             s_item_count, s_dir_count, s_file_count,
             heap_pct, heap_used, heap_total);
    lv_label_set_text(s_info_label, buf);
    lv_label_set_recolor(s_info_label, true);
}

// ============================================================
//  Section 9: Event callbacks
// ============================================================

// Klik tunggal → seleksi
static void list_item_select_cb(lv_event_t* e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    s_sel_index = (int)idx;
    update_status();
}

// Short click (in this LVGL version = double-click / quick tap) -> open folder/properties
static void list_item_open_cb(lv_event_t* e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    s_sel_index = (int)idx;
    update_status();
    open_selected();
}

static void sidebar_home_cb(lv_event_t* e) {
    (void)e;
    s_cwd = fs_get_root();
    s_sel_index = -1;
    s_prev_sel = -1;
    rebuild_list();
    update_path_label();
    update_status();
}

static void sidebar_up_cb(lv_event_t* e) {
    (void)e;
    go_to_parent();
}

static void sidebar_refresh_cb(lv_event_t* e) {
    (void)e;
    s_sel_index = -1;
    s_prev_sel = -1;
    rebuild_list();
    update_status();
}

static void win_close_cb(lv_event_t* e) {
    (void)e;
    s_exit_flag = 1;
}

static void win_key_cb(lv_event_t* e) {
    uint32_t key = *(uint32_t*)lv_event_get_param(e);
    if (key == LV_KEY_ESC) {
        s_exit_flag = 1;
        return;
    }

    // Block keyboard input while a dialog is open
    if (s_mbox) return;
    if (!s_file_list) return;

    if (key == LV_KEY_UP) {
        if (s_sel_index > 0) s_sel_index--;
        else if (s_item_count > 0) s_sel_index = 0;
    } else if (key == LV_KEY_DOWN) {
        if (s_sel_index < s_item_count - 1) s_sel_index++;
        else if (s_item_count > 0) s_sel_index = 0;
    } else if (key == LV_KEY_ENTER) {
        open_selected();
        return;
    } else if (key == LV_KEY_BACKSPACE) {
        go_to_parent();
        return;
    } else if (key == LV_KEY_DEL) {
        delete_selected();
        return;
    } else if (key == LV_KEY_F2) {
        rename_selected_action();
        return;
    } else {
        return;
    }

    // Update the visual focus for keyboard navigation
    if (s_sel_index >= 0 && s_sel_index < s_item_count && s_sel_index != s_prev_sel) {
        lv_obj_t* btn = lv_obj_get_child(s_file_list, s_sel_index);
        if (btn) {
            if (s_prev_sel >= 0 && s_prev_sel < s_item_count) {
                lv_obj_t* old_btn = lv_obj_get_child(s_file_list, s_prev_sel);
                if (old_btn) {
                    lv_obj_clear_state(old_btn, LV_STATE_FOCUSED);
                }
            }
            lv_obj_scroll_to_view(btn, LV_ANIM_ON);
            lv_obj_add_state(btn, LV_STATE_FOCUSED);
            s_prev_sel = s_sel_index;
        }
    } else {
        s_prev_sel = -1;
    }
    update_status();
}

static void toolbar_event_cb(lv_event_t* e) {
    lv_obj_t* toolbar = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(toolbar);
    if (id == 0)      create_file_action();
    else if (id == 1) create_dir_action();
    else if (id == 2) delete_selected();
    else if (id == 3) rename_selected_action();
    else if (id == 4) { s_sel_index = -1; s_prev_sel = -1; rebuild_list(); update_status(); }
    else if (id == 5) go_to_parent();
}

static void delete_confirm_cb(lv_event_t* e) {
    (void)e;
    lv_obj_t* this_mbox = s_mbox;
    // Grab the button text before closing the dialog (avoid use-after-free)
    const char* btn_text = this_mbox ? lv_msgbox_get_active_btn_text(this_mbox) : nullptr;
    bool do_delete = btn_text && strcmp(btn_text, "Delete") == 0;

    if (this_mbox) {
        lv_msgbox_close(this_mbox);
        if (s_mbox == this_mbox) s_mbox = nullptr;
    }

    if (do_delete) {
        struct fs_node* node = selected_node();
        if (node) {
            int ret = fs_delete_node(s_cwd, node->name);
            if (ret == -4) {
                show_messagebox(LV_SYMBOL_WARNING " Error",
                                "Directory not empty.\nCannot delete non-empty folder.",
                                "OK");
            } else if (ret != 0) {
                show_messagebox(LV_SYMBOL_WARNING " Error",
                                "Delete failed (unknown error).",
                                "OK");
            }
        }
    }
    s_sel_index = -1;
    s_prev_sel = -1;
    rebuild_list();
    update_status();
}

static void delete_selected(void) {
    struct fs_node* node = selected_node();
    if (!node) {
        show_messagebox(LV_SYMBOL_WARNING " No Selection",
                        "Please select an item first.",
                        "OK");
        return;
    }
    char msg[160];
    snprintf(msg, sizeof(msg),
             LV_SYMBOL_TRASH "  Delete '%s'?\n\nThis action cannot be undone.",
             node->name);
    if (s_mbox) { lv_msgbox_close(s_mbox); s_mbox = nullptr; }
    static const char* btns[] = {"Cancel", "Delete", ""};
    s_mbox = lv_msgbox_create(NULL, LV_SYMBOL_WARNING " Confirm Delete",
                              msg, btns, false);
    lv_obj_add_event_cb(s_mbox, delete_confirm_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_center(s_mbox);
}

// ============================================================
//  Section 10: Build UI regions
// ============================================================

static void build_header(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), FM_HEADER_H);
    lv_obj_set_style_bg_color(header, lv_color_hex(fm::PANEL), 0);
    lv_obj_set_style_border_color(header, lv_color_hex(fm::BORDER), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_style_pad_left(header, 16, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, LV_SYMBOL_LIST "  Paus OS Files");
    lv_obj_set_style_text_color(title, lv_color_hex(fm::TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* close_btn = lv_btn_create(header);
    lv_obj_set_size(close_btn, 32, 28);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(fm::ITEM), 0);
    lv_obj_set_style_radius(close_btn, 4, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t* close_icon = lv_label_create(close_btn);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close_btn, win_close_cb, LV_EVENT_CLICKED, nullptr);
}

static void build_sidebar(lv_obj_t* parent) {
    lv_obj_t* sidebar = lv_obj_create(parent);
    lv_obj_set_size(sidebar, FM_SIDEBAR_W, LV_PCT(100));
    lv_obj_set_style_bg_color(sidebar, lv_color_hex(fm::PANEL), 0);
    lv_obj_set_style_border_color(sidebar, lv_color_hex(fm::BORDER), 0);
    lv_obj_set_style_border_side(sidebar, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(sidebar, 1, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);
    lv_obj_set_style_pad_all(sidebar, 12, 0);
    lv_obj_clear_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* nav_title = lv_label_create(sidebar);
    lv_label_set_text(nav_title, LV_SYMBOL_HOME "  NAVIGATION");
    lv_obj_set_style_text_color(nav_title, lv_color_hex(fm::ACCENT), 0);
    lv_obj_set_style_text_font(nav_title, &lv_font_montserrat_14, 0);
    lv_obj_align(nav_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* nav_list = lv_list_create(sidebar);
    lv_obj_set_size(nav_list, LV_PCT(100), 110);
    lv_obj_align(nav_list, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(nav_list, lv_color_hex(fm::LIST_BG), 0);
    lv_obj_set_style_border_width(nav_list, 0, 0);
    lv_obj_set_style_radius(nav_list, 4, 0);
    lv_obj_set_style_pad_all(nav_list, 0, 0);

    static const char* nav_label_strs[] = {"Home", "Go Up", "Refresh"};
    static const char* nav_icons[] = {LV_SYMBOL_HOME, LV_SYMBOL_UP, LV_SYMBOL_REFRESH};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* btn = lv_list_add_btn(nav_list, nav_icons[i], nav_label_strs[i]);
        lv_obj_set_style_bg_color(btn, lv_color_hex(fm::ITEM), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(fm::ITEM_SELECTED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(fm::ITEM_SELECTED), LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(fm::TEXT), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 3, 0);
        lv_obj_set_style_pad_top(btn, 6, 0);
        lv_obj_set_style_pad_bottom(btn, 6, 0);
        lv_obj_t* icon_lbl = lv_obj_get_child(btn, 0);
        if (icon_lbl) {
            lv_obj_set_style_text_color(icon_lbl, lv_color_hex(fm::ACCENT), 0);
        }
        if (i == 0) lv_obj_add_event_cb(btn, sidebar_home_cb, LV_EVENT_CLICKED, nullptr);
        else if (i == 1) lv_obj_add_event_cb(btn, sidebar_up_cb, LV_EVENT_CLICKED, nullptr);
        else lv_obj_add_event_cb(btn, sidebar_refresh_cb, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* info_title = lv_label_create(sidebar);
    lv_label_set_text(info_title, LV_SYMBOL_LIST "  INFO");
    lv_obj_set_style_text_color(info_title, lv_color_hex(fm::ACCENT), 0);
    lv_obj_set_style_text_font(info_title, &lv_font_montserrat_14, 0);
    lv_obj_align(info_title, LV_ALIGN_TOP_LEFT, 0, 150);

    s_info_label = lv_label_create(sidebar);
    lv_obj_set_width(s_info_label, FM_SIDEBAR_W - 24);
    lv_obj_align(s_info_label, LV_ALIGN_TOP_LEFT, 0, 174);
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(fm::TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_12, 0);
    lv_label_set_recolor(s_info_label, true);
}

static void build_main_column(lv_obj_t* parent) {
    lv_obj_t* main_col = lv_obj_create(parent);
    lv_obj_set_size(main_col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(main_col, 1);
    lv_obj_set_style_bg_color(main_col, lv_color_hex(fm::LIST_BG), 0);
    lv_obj_set_style_border_width(main_col, 0, 0);
    lv_obj_set_style_radius(main_col, 0, 0);
    lv_obj_set_style_pad_all(main_col, 8, 0);
    lv_obj_set_flex_flow(main_col, LV_FLEX_FLOW_COLUMN);

    /* Path bar */
    lv_obj_t* path_bar = lv_obj_create(main_col);
    lv_obj_set_size(path_bar, LV_PCT(100), 32);
    lv_obj_set_style_bg_color(path_bar, lv_color_hex(fm::PANEL), 0);
    lv_obj_set_style_radius(path_bar, 4, 0);
    lv_obj_set_style_border_color(path_bar, lv_color_hex(fm::BORDER), 0);
    lv_obj_set_style_border_width(path_bar, 1, 0);
    lv_obj_set_style_pad_left(path_bar, 10, 0);
    lv_obj_set_style_pad_right(path_bar, 10, 0);
    lv_obj_clear_flag(path_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_path_label = lv_label_create(path_bar);
    lv_obj_align(s_path_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(s_path_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_path_label, lv_color_hex(fm::ACCENT), 0);

    lv_obj_t* refresh_btn = lv_btn_create(path_bar);
    lv_obj_set_size(refresh_btn, 28, 24);
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_hex(fm::ITEM), 0);
    lv_obj_set_style_radius(refresh_btn, 3, 0);
    lv_obj_set_style_border_width(refresh_btn, 0, 0);
    lv_obj_set_style_shadow_width(refresh_btn, 0, 0);
    lv_obj_t* refresh_icon = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_icon, LV_SYMBOL_REFRESH);
    lv_obj_center(refresh_icon);
    lv_obj_set_style_text_color(refresh_icon, lv_color_hex(fm::ACCENT), 0);
    lv_obj_add_event_cb(refresh_btn, sidebar_refresh_cb, LV_EVENT_CLICKED, nullptr);

    /* Toolbar */
    lv_obj_t* toolbar = lv_btnmatrix_create(main_col);
    lv_obj_set_size(toolbar, LV_PCT(100), FM_TOOLBAR_H);
    static const char* toolbar_map[] = {
        LV_SYMBOL_PLUS "\nNew",
        LV_SYMBOL_DIRECTORY "\nFolder",
        LV_SYMBOL_TRASH "\nDelete",
        LV_SYMBOL_EDIT "\nRename",
        LV_SYMBOL_REFRESH "\nRefresh",
        LV_SYMBOL_UP "\nUp",
        ""
    };
    lv_btnmatrix_set_map(toolbar, toolbar_map);
    lv_btnmatrix_set_btn_ctrl_all(toolbar, LV_BTNMATRIX_CTRL_RECOLOR);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(fm::PANEL), 0);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(fm::ITEM_SELECTED),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(fm::ITEM_SELECTED),
                              LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(toolbar, lv_color_hex(fm::TEXT), 0);
    lv_obj_set_style_text_color(toolbar, lv_color_hex(0xFFFFFF),
                                LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_font(toolbar, &lv_font_montserrat_12, LV_PART_ITEMS);
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_set_style_radius(toolbar, 4, 0);
    lv_obj_set_style_pad_all(toolbar, 2, 0);
    lv_obj_set_style_pad_gap(toolbar, 2, 0);
    lv_obj_add_event_cb(toolbar, toolbar_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    /* File list */
    s_file_list = lv_list_create(main_col);
    lv_obj_set_size(s_file_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_file_list, 1);
    lv_obj_set_style_bg_color(s_file_list, lv_color_hex(fm::LIST_BG), 0);
    lv_obj_set_style_border_color(s_file_list, lv_color_hex(fm::BORDER), 0);
    lv_obj_set_style_border_width(s_file_list, 1, 0);
    lv_obj_set_style_radius(s_file_list, 4, 0);
    lv_obj_set_style_pad_all(s_file_list, 4, 0);

    /* Status bar */
    lv_obj_t* status_bar = lv_obj_create(main_col);
    lv_obj_set_size(status_bar, LV_PCT(100), FM_STATUS_H);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(fm::PANEL), 0);
    lv_obj_set_style_radius(status_bar, 4, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 10, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(status_bar);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(fm::TEXT_DIM), 0);
}

static void rebuild_list(void) {
    if (!s_file_list || !s_cwd) return;
    lv_obj_clean(s_file_list);
    free_file_arr();
    build_file_arr();

    s_dir_count = 0;
    s_file_count = 0;

    if (s_item_count == 0) {
        lv_obj_t* placeholder = lv_label_create(s_file_list);
        lv_label_set_text(placeholder,
                          LV_SYMBOL_DIRECTORY "  (empty folder)\n\n"
                          "Use the toolbar or press the + button to create\n"
                          "a new file or folder here.");
        lv_obj_set_style_text_color(placeholder,
            lv_color_hex(fm::TEXT_DIM), 0);
        lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(placeholder);
        update_info_panel();
        return;
    }

    for (int i = 0; i < s_item_count; i++) {
        struct fs_node* node = s_file_arr[i];
        if (node->is_dir) s_dir_count++;
        else s_file_count++;

        lv_obj_t* btn = lv_list_add_btn(s_file_list,
                                        file_icon(node), node->name);
        lv_obj_set_style_bg_color(btn, lv_color_hex(fm::ITEM), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(fm::ITEM_SELECTED),
                                  LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(fm::ITEM_SELECTED),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(fm::TEXT), 0);
        lv_obj_set_style_pad_top(btn, 6, 0);
        lv_obj_set_style_pad_bottom(btn, 6, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 3, 0);

        lv_obj_t* icon_label = lv_obj_get_child(btn, 0);
        if (icon_label) {
            lv_obj_set_style_text_color(icon_label,
                lv_color_hex(file_icon_color(node)), 0);
            lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_16, 0);
        }

        if (!node->is_dir) {
            char size_buf[32];
            format_size(node->size, size_buf, sizeof(size_buf));
            char full_label[128];
            snprintf(full_label, sizeof(full_label),
                     "%s  #707682 %s#", node->name, size_buf);
            lv_obj_t* text_label = lv_obj_get_child(btn, 1);
            if (text_label) {
                lv_label_set_text(text_label, full_label);
                lv_label_set_recolor(text_label, true);
            }
        }

        // Klik tunggal → seleksi
        lv_obj_add_event_cb(btn, list_item_select_cb, LV_EVENT_CLICKED,
                            (void*)(uintptr_t)i);
                // Short click (double-click in this version) -> open folder / properties
        lv_obj_add_event_cb(btn, list_item_open_cb, LV_EVENT_SHORT_CLICKED,
                            (void*)(uintptr_t)i);
    }

    update_info_panel();
}

static void build_ui(const char* start_path) {
    s_cwd = fs_get_root();
    if (!s_cwd) {
        printf("File Manager: FATAL - Filesystem root is NULL!\n");
        return;
    }

    if (start_path && start_path[0]) {
        struct fs_node* n = fs_get_node_from_path(s_cwd, start_path);
        s_cwd = n ? n : s_cwd;
    }

    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(fm::BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);

    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(fm::BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0,0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);

    build_header(s_root);

    lv_obj_t* body = lv_obj_create(s_root);
    lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_color(body, lv_color_hex(fm::BG), 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    build_sidebar(body);
    build_main_column(body);

    lv_obj_add_event_cb(s_root, win_key_cb, LV_EVENT_KEY, nullptr);

    update_path_label();
    rebuild_list();
    update_status();
    update_info_panel();
}

// ============================================================
//  Section 11: Cleanup
// ============================================================

static void cleanup(void) {
    free_file_arr();
    if (s_mbox) {
        lv_msgbox_close(s_mbox);
        s_mbox = nullptr;
    }
    s_cwd = nullptr;
    s_root = nullptr;
    s_file_list = nullptr;
    s_status_label = nullptr;
    s_path_label = nullptr;
    s_info_label = nullptr;
    s_sel_index = -1;
    s_prev_sel = -1;
    s_item_count = 0;
    s_dir_count = 0;
    s_file_count = 0;
}

// ============================================================
//  Section 12: Public entry
// ============================================================

extern "C" void filemanager_open(const char* start_path) {
    s_exit_flag = 0;
    s_sel_index = -1;
    s_prev_sel = -1;
    build_ui(start_path);
    if (!s_root) {
        printf("File Manager: UI build failed, exiting.\n");
        return;
    }
    while (!s_exit_flag) {
        lv_timer_handler();
        sleep_ms(5);
    }
    lv_obj_clean(lv_scr_act());
    cleanup();
}

#else /* !HAS_LVGL */

#include "../../../library/header/filemanager.h"
#include "../../../library/header/stdio.h"

extern "C" void filemanager_open(const char* start_path) {
    (void)start_path;
    printf("File Manager requires LVGL. Recompile with LVGL source in kernel/gui/lvgl/\n");
}

#endif /* HAS_LVGL */
