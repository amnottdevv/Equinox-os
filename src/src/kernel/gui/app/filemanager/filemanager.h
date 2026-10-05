/**
 * @file filemanager.h
 * @brief Modern LVGL-based file manager for Equinox OS.
 *
 * Redesigned with:
 *   - Sidebar with shortcuts (Home, Up, Refresh)
 *   - Toolbar icon-only (New File, New Folder, Delete, Rename, Refresh, Up)
 *   - File/folder list with distinct icons (LV_SYMBOL_DIRECTORY / LV_SYMBOL_FILE /
 *     LV_SYMBOL_IMAGE / LV_SYMBOL_SAVE / LV_SYMBOL_EDIT / etc.) by extension
 *   - Status bar with item count + path breadcrumb
 *   - Properties panel when an item is selected
 *   - "Are you sure?" delete dialog (lv_msgbox)
 *   - Inline rename textarea (lv_textarea + lv_keyboard)
 *
 * The public API stays the same as the old version (filemanager_open) so
 * kernel.cpp needs no changes.
 */
#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open the file manager. start_path may be NULL (root) or an absolute path.
 * Blocking until the user presses ESC or the close button.
 */
void filemanager_open(const char* start_path);

#ifdef __cplusplus
}
#endif

#endif /* FILEMANAGER_H */
