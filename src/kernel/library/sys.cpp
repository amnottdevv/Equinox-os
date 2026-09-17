#include "header/sys.h"
#include "header/fs_ram.h"
#include "header/libstring.h"
#include "header/malloc.h"
#include "header/stdio.h"
#include <stdint.h>
#include <stddef.h>

// ============================================================
//  Global State: Current Working Directory
// ============================================================
static struct fs_node* sys_cwd = NULL;

// ============================================================
//  Helper: parse path -> parent node + basename
// ============================================================
static int split_path(const char* path, struct fs_node** parent, char* basename, size_t basename_size) {
    if (!path || !path[0]) return -1;

    // Copy the path into a temporary buffer
    char path_copy[256];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    // Find the last slash
    char* last_slash = strrchr(path_copy, '/');
    struct fs_node* base = NULL;

    if (last_slash == path_copy) {
        // A "/"-only path = root. But "/foo" also has a slash at
        // index 0 — an old bug conflated the two and returned an empty
        // basename, so 1-level absolute mkdir/touch always failed.
        // Now only a path of exactly "/" is treated as root.
        if (path_copy[1] == '\0') {
            if (basename) basename[0] = '\0';
            if (parent) *parent = fs_get_root();
            return 0;
        }
        // path = "/foo" -> parent = root, basename = "foo"
        if (parent) *parent = fs_get_root();
        if (basename) {
            strncpy(basename, path_copy + 1, basename_size - 1);
            basename[basename_size - 1] = '\0';
        }
        return 0;
    }

    if (last_slash) {
        // There is a slash: split into parent and basename
        *last_slash = '\0';
        const char* parent_path = path_copy;
        const char* name = last_slash + 1;

        // Get the parent node from the path
        if (parent_path[0] == '/') {
            base = fs_get_node_from_path(fs_get_root(), parent_path);
        } else {
            // Relative to the CWD (avoid //path when cwd = root)
            char full[256];
            fs_get_path(sys_cwd, full, sizeof(full));
            size_t cur_len = strlen(full);
            if (cur_len == 0 || full[cur_len - 1] != '/') {
                /* FIX(X1): append '/' with bounds. If the cwd path is
                 * already 255 chars, the old strcat wrote '/' + NUL outside
                 * full[256] (stack overflow) AND the strncat bound below
                 * underflowed size_t -> unbounded append. */
                if (cur_len < sizeof(full) - 1) {
                    full[cur_len] = '/';
                    full[cur_len + 1] = '\0';
                }
            }
            strncat(full, parent_path, sizeof(full) - strlen(full) - 1);
            base = fs_get_node_from_path(fs_get_root(), full);
        }

        if (!base || !base->is_dir) {
            return -1; // parent invalid or not a directory
        }

        if (parent) *parent = base;
        if (basename) {
            strncpy(basename, name, basename_size - 1);
            basename[basename_size - 1] = '\0';
        }
        return 0;
    } else {
        // No slash: the path is a file/dir name inside the CWD
        if (parent) *parent = sys_cwd;
        if (basename) {
            strncpy(basename, path, basename_size - 1);
            basename[basename_size - 1] = '\0';
        }
        return 0;
    }
}

// ============================================================
//  Initialization
// ============================================================
void sys_init(void) {
    sys_cwd = fs_get_root();
    if (!sys_cwd) {
        printf("Sys: Failed to initialize CWD\n");
    }
}

// ============================================================
//  sys_create_file
// ============================================================
int sys_create_file(const char* path, const char* content) {
    if (!path || !path[0]) return -1;

    struct fs_node* parent = NULL;
    char basename[64] = {0};

    if (split_path(path, &parent, basename, sizeof(basename)) != 0) {
        return -1;
    }

    if (!parent || !basename[0]) return -1;

    // Check whether it already exists
    if (fs_find_child(parent, basename)) {
        return -2; // File/dir already exists
    }

    int ret = fs_create_file(parent, basename, content ? content : "");
    return ret;
}

// ============================================================
//  sys_create_dir
// ============================================================
int sys_create_dir(const char* path) {
    if (!path || !path[0]) return -1;

    struct fs_node* parent = NULL;
    char basename[64] = {0};

    if (split_path(path, &parent, basename, sizeof(basename)) != 0) {
        return -1;
    }

    if (!parent || !basename[0]) return -1;

    if (fs_find_child(parent, basename)) {
        return -2;
    }

    int ret = fs_create_dir(parent, basename);
    return ret;
}

// ============================================================
//  sys_delete
// ============================================================
int sys_delete(const char* path) {
    if (!path || !path[0]) return -1;

    struct fs_node* parent = NULL;
    char basename[64] = {0};

    if (split_path(path, &parent, basename, sizeof(basename)) != 0) {
        return -1;
    }

    if (!parent || !basename[0]) return -1;

    struct fs_node* target = fs_find_child(parent, basename);
    if (!target) {
        return -3; // Not found
    }

    int ret = fs_delete_node(parent, basename);
    return ret;
}

// ============================================================
//  sys_rename
// ============================================================
int sys_rename(const char* old_path, const char* new_path) {
    // Simple implementation: copy then delete.
    // For production this should be more efficient, but it suffices for now.
    if (!old_path || !new_path) return -1;

    // Read the old file's contents
    struct fs_node* old_parent = NULL;
    char old_name[64] = {0};
    if (split_path(old_path, &old_parent, old_name, sizeof(old_name)) != 0) return -1;
    struct fs_node* old_node = fs_find_child(old_parent, old_name);
    if (!old_node || old_node->is_dir) {
        // Folder rename not supported for now (can be added later)
        return -4;
    }

    // Create the new file with the same contents
    const char* content = old_node->content ? old_node->content : "";
    int ret = sys_create_file(new_path, content);
    if (ret != 0) return ret;

    // Delete the old file
    return sys_delete(old_path);
}

// ============================================================
//  sys_copy
// ============================================================
int sys_copy(const char* src_path, const char* dest_path) {
    if (!src_path || !dest_path) return -1;

    struct fs_node* src_parent = NULL;
    char src_name[64] = {0};
    if (split_path(src_path, &src_parent, src_name, sizeof(src_name)) != 0) return -1;
    struct fs_node* src_node = fs_find_child(src_parent, src_name);
    if (!src_node || src_node->is_dir) return -4;

    const char* content = src_node->content ? src_node->content : "";
    return sys_create_file(dest_path, content);
}

// ============================================================
//  sys_chdir
// ============================================================
int sys_chdir(const char* path) {
    if (!path || !path[0]) return -1;

    struct fs_node* target = NULL;
    if (path[0] == '/') {
        target = fs_get_node_from_path(fs_get_root(), path);
    } else {
        // Relative to the CWD. Avoid a double slash when the cwd is
        // the root (fs_get_path returns "/" -> "/"+"/"+path).
        char full[256];
        fs_get_path(sys_cwd, full, sizeof(full));
        size_t cur_len = strlen(full);
        if (cur_len == 0 || full[cur_len - 1] != '/') {
            /* FIX(X1): append '/' with bounds — prevents stack overflow
             * + strncat bound underflow when the cwd path is 255 chars. */
            if (cur_len < sizeof(full) - 1) {
                full[cur_len] = '/';
                full[cur_len + 1] = '\0';
            }
        }
        strncat(full, path, sizeof(full) - strlen(full) - 1);
        target = fs_get_node_from_path(fs_get_root(), full);
    }

    if (!target || !target->is_dir) return -1;

    sys_cwd = target;
    return 0;
}

// ============================================================
//  sys_getcwd
// ============================================================
char* sys_getcwd(char* buffer, size_t size) {
    if (!buffer || size == 0) return NULL;
    fs_get_path(sys_cwd, buffer, size);
    return buffer;
}

// ============================================================
//  sys_path_exists
// ============================================================
int sys_path_exists(const char* path) {
    if (!path || !path[0]) return 0;

    struct fs_node* parent = NULL;
    char basename[64] = {0};
    if (split_path(path, &parent, basename, sizeof(basename)) != 0) return 0;
    if (!parent || !basename[0]) return 0;

    return fs_find_child(parent, basename) != NULL;
}

// ============================================================
//  sys_is_dir / sys_is_file
// ============================================================
int sys_is_dir(const char* path) {
    if (!path || !path[0]) return 0;

    struct fs_node* parent = NULL;
    char basename[64] = {0};
    if (split_path(path, &parent, basename, sizeof(basename)) != 0) return 0;
    if (!parent || !basename[0]) return 0;

    struct fs_node* node = fs_find_child(parent, basename);
    if (!node) return 0;
    return node->is_dir ? 1 : 0;
}

int sys_is_file(const char* path) {
    if (!path || !path[0]) return 0;

    struct fs_node* parent = NULL;
    char basename[64] = {0};
    if (split_path(path, &parent, basename, sizeof(basename)) != 0) return 0;
    if (!parent || !basename[0]) return 0;

    struct fs_node* node = fs_find_child(parent, basename);
    if (!node) return 0;
    return node->is_dir ? 0 : 1;
}

// ============================================================
//  sys_ls
// ============================================================
void sys_ls(const char* path) {
    struct fs_node* dir = NULL;
    if (!path || path[0] == '\0') {
        dir = sys_cwd;
    } else if (path[0] == '/') {
        dir = fs_get_node_from_path(fs_get_root(), path);
    } else {
        char full[256];
        fs_get_path(sys_cwd, full, sizeof(full));
        size_t cur_len = strlen(full);
        if (cur_len == 0 || full[cur_len - 1] != '/') {
            /* FIX(X1): append '/' with bounds — prevents stack overflow
             * + strncat bound underflow when the cwd path is 255 chars. */
            if (cur_len < sizeof(full) - 1) {
                full[cur_len] = '/';
                full[cur_len + 1] = '\0';
            }
        }
        strncat(full, path, sizeof(full) - strlen(full) - 1);
        dir = fs_get_node_from_path(fs_get_root(), full);
    }

    if (!dir || !dir->is_dir) {
        printf("sys_ls: not a directory\n");
        return;
    }

    fs_ls(dir, 0);
}
