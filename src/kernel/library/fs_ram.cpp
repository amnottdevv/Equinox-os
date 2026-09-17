#include "header/fs_ram.h"
#include "header/stdio.h"
#include "header/malloc.h"
#include "header/itoa_atoi.h"   // strlen, strcpy, strcat, strcmp
#include <stdint.h>
#include <stddef.h>
#include "header/libstring.h"

// FIX: node-name length limit -- MUST match sizeof(fs_node::name).
// Used by every create function so there is no unbounded strcpy() into name[64].
#define FS_NAME_MAX 64

// ====================================================================
//                            ROOT NODE
// ====================================================================
static struct fs_node* root_node = NULL;

// ====================================================================
//                        INISIALISASI
// ====================================================================
void fs_init(void) {
    root_node = (struct fs_node*)malloc(sizeof(struct fs_node));
    if (!root_node) {
        printf("FS: Failed to allocate root node\n");
        return;
    }
    root_node->name[0] = '\0';
    root_node->is_dir = 1;
    root_node->is_ref = 0;
    root_node->size = 0;
    root_node->content = NULL;
    root_node->parent = NULL;
    root_node->children = NULL;
    root_node->next = NULL;
}

struct fs_node* fs_get_root(void) {
    return root_node;
}

// ====================================================================
//                        FUNGSI BANTUAN
// ====================================================================

// find a child by exact name (case-sensitive)
struct fs_node* fs_find_child(struct fs_node* parent, const char* name) {
    if (!parent || !parent->is_dir) return NULL;
    struct fs_node* child = parent->children;
    while (child) {
        if (strcmp(child->name, name) == 0) {
            return child;
        }
        child = child->next;
    }
    return NULL;
}

/* my_strstr() removed - libstring.cpp already provides strstr()
 * usable directly. The static version below was never called and only
 * raised a -Wunused-function warning. */

// ====================================================================
//                        BUAT FILE / DIREKTORI
// ====================================================================

int fs_create_file(struct fs_node* parent, const char* name, const char* content) {
    if (!parent || !parent->is_dir) return -1;
    if (!name || !name[0]) return -8;             // FIX: NULL/empty name
    if (strlen(name) >= FS_NAME_MAX) return -7;   // FIX: name > 63 chars = heap overflow
    if (fs_find_child(parent, name)) return -2; // already exists

    struct fs_node* new_node = (struct fs_node*)malloc(sizeof(struct fs_node));
    if (!new_node) return -3; // out of memory

    // FIX: bounded copy + always NUL-terminated (length already validated < 64)
    size_t nlen = strlen(name);
    for (size_t i = 0; i < nlen; i++) new_node->name[i] = name[i];
    new_node->name[nlen] = '\0';
    new_node->is_dir = 0;
    new_node->is_ref = 0;
    new_node->parent = parent;
    new_node->children = NULL;

    if (content) {
        size_t len = strlen(content);
        new_node->content = (char*)malloc(len + 1);
        if (!new_node->content) {
            free(new_node);
            return -3;
        }
        strcpy(new_node->content, content);
        new_node->size = len;
    } else {
        new_node->content = NULL;
        new_node->size = 0;
    }

    // sisipkan di depan (linked list)
    new_node->next = parent->children;
    parent->children = new_node;

    return 0;
}

// ====================================================================
//  WRITE BINARY DATA (used by the .mrp loader; may contain 0x00 bytes)
// ====================================================================
int fs_write_binary(struct fs_node* parent, const char* name,
                     const uint8_t* data, uint32_t len) {
    if (!parent || !parent->is_dir) return -1;
    if (!data) return -5;

    struct fs_node* node = fs_find_child(parent, name);

    if (!node) {
        // File does not exist yet -> create it (empty) first, then fill it in
        // manually below (NOT via the strlen-based fs_create_file(content)).
        int ret = fs_create_file(parent, name, nullptr);
        if (ret != 0) return ret;
        node = fs_find_child(parent, name);
        if (!node) return -3;
    } else if (node->is_dir) {
        return -6; // that name is already taken by a directory
    }

    // Discard the old contents (if any) before swapping in the new buffer.
    // v10.9: an is_ref node (zero-copy module staging) is NOT freed --
    // its pointer does not belong to the kernel heap. An overwritten
    // staging file turns into a normal heap copy (is_ref reset to 0).
    if (node->content && !node->is_ref) {
        free(node->content);
    }
    node->content = nullptr;
    node->is_ref = 0;

    node->content = (char*)malloc(len);
    if (!node->content) {
        node->size = 0;
        return -3; // out of memory
    }

    // Manual memcpy (byte-by-byte) -- NOT strcpy, so embedded 0x00 bytes
    // do not truncate the file. This is what makes the function safe
    // for .mrp compiler machine code.
    const uint8_t* src = data;
    uint8_t* dst = (uint8_t*)node->content;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

    node->size = len;
    return 0;
}

// ====================================================================
//  v10.9 "WAD PATH": ZERO-COPY REFERENCES (GRUB module staging)
// --------------------------------------------------------------------
//  Like fs_write_binary, BUT content points directly at data that
//  already lives in memory - start.asm places every GRUB module in
//  MODULE_STAGE 0x2800000+ (supervisor pages, outside KERNEL_HEAP).
//  No kernel-heap copy is made: a 4.2 MB file (doom1.wad) lives in
//  the RAMFS without wasting the 2 MB heap.
//
//  The node gets is_ref=1:
//    - free()/fs_delete_node skip the content (not heap-owned),
//    - the next fs_write_binary replaces it with a normal heap copy
//      (is_ref reset) - overwriting a staging file yields a normal
//      file, the sensible behavior for edit/save.
// ====================================================================
int fs_reference_binary(struct fs_node* parent, const char* name,
                        const uint8_t* data, uint32_t len) {
    if (!parent || !parent->is_dir) return -1;
    if (!data || len == 0) return -5;

    struct fs_node* node = fs_find_child(parent, name);
    if (!node) {
        int ret = fs_create_file(parent, name, nullptr);
        if (ret != 0) return ret;
        node = fs_find_child(parent, name);
        if (!node) return -3;
    } else if (node->is_dir) {
        return -6; // that name is already taken by a directory
    }

    // Detach the old contents WITHOUT free when it is also a zero-copy ref.
    if (node->content && !node->is_ref) {
        free(node->content);
    }
    node->content = (char*)(uintptr_t)data; // milik staging — jangan free
    node->size    = len;
    node->is_ref  = 1;
    return 0;
}

int fs_create_dir(struct fs_node* parent, const char* name) {
    if (!parent || !parent->is_dir) return -1;
    if (!name || !name[0]) return -8;             // FIX: NULL/empty name
    if (strlen(name) >= FS_NAME_MAX) return -7;   // FIX: name > 63 chars = heap overflow
    if (fs_find_child(parent, name)) return -2;

    struct fs_node* new_dir = (struct fs_node*)malloc(sizeof(struct fs_node));
    if (!new_dir) return -3;

    // FIX: bounded copy + always NUL-terminated (length already validated < 64)
    size_t nlen = strlen(name);
    for (size_t i = 0; i < nlen; i++) new_dir->name[i] = name[i];
    new_dir->name[nlen] = '\0';
    new_dir->is_dir = 1;
    new_dir->is_ref = 0;
    new_dir->size = 0;
    new_dir->content = NULL;
    new_dir->parent = parent;
    new_dir->children = NULL;
    new_dir->next = parent->children;
    parent->children = new_dir;

    return 0;
}

// ====================================================================
//                        HAPUS NODE
// ====================================================================

int fs_delete_node(struct fs_node* parent, const char* name) {
    if (!parent || !parent->is_dir) return -1;
    struct fs_node* prev = NULL;
    struct fs_node* curr = parent->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            // if a directory, it must be empty
            if (curr->is_dir && curr->children) {
                return -4; // directory not empty
            }
            // unlink
            if (prev) prev->next = curr->next;
            else parent->children = curr->next;
            // bebaskan konten jika file (v10.9: kecuali zero-copy ref
            // module staging — pointer itu bukan milik heap kernel)
            if (curr->content && !curr->is_ref) free(curr->content);
            free(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    return -2; // not found
}

// ====================================================================
//                        PATH
// ====================================================================

char* fs_get_path(struct fs_node* node, char* buffer, size_t bufsize) {
    /* Old bug: the bufsize parameter was never used - every
     * strcpy/strcat wrote into the buffer unchecked -> overflow when
     * the path outgrew the caller's buffer. Now uses
     * strncpy/strncat bounded by bufsize. */
    if (!buffer || bufsize == 0) return buffer;
    if (!node) {
        buffer[0] = '\0';
        return buffer;
    }
    if (node->parent == NULL) {
        if (bufsize < 2) { buffer[0] = '\0'; return buffer; }
        buffer[0] = '/';
        buffer[1] = '\0';
        return buffer;
    }
    char parent_path[256];
    fs_get_path(node->parent, parent_path, sizeof(parent_path));
    if (strcmp(parent_path, "/") == 0) {
        strncpy(buffer, "/", bufsize - 1);
        buffer[bufsize - 1] = '\0';
        strncat(buffer, node->name, bufsize - strlen(buffer) - 1);
    } else {
        strncpy(buffer, parent_path, bufsize - 1);
        buffer[bufsize - 1] = '\0';
        strncat(buffer, "/", bufsize - strlen(buffer) - 1);
        strncat(buffer, node->name, bufsize - strlen(buffer) - 1);
    }
    return buffer;
}

// ====================================================================
//                        LIST / TREE
// ====================================================================

void fs_ls(struct fs_node* dir, int show_details) {
    if (!dir || !dir->is_dir) {
        printf("Not a directory\n");
        return;
    }
    struct fs_node* child = dir->children;
    if (!child) {
        printf("(empty)\n");
        return;
    }
    while (child) {
        if (child->is_dir) {
            printf("[DIR]  %s\n", child->name);
        } else {
            if (show_details) {
                printf("[FILE] %s  (%d bytes)\n", child->name, child->size);
            } else {
                printf("%s  ", child->name);
            }
        }
        child = child->next;
    }
    if (!show_details) printf("\n");
}

void fs_tree(struct fs_node* dir, int depth) {
    if (!dir) return;
    for (int i = 0; i < depth; i++) printf("  ");
    if (depth == 0) {
        printf("/\n");
    } else {
        printf("|-- %s\n", dir->name);
    }
    if (dir->is_dir) {
        struct fs_node* child = dir->children;
        while (child) {
            fs_tree(child, depth + 1);
            child = child->next;
        }
    }
}

// ====================================================================
//                    NAVIGASI (CD)
// ====================================================================

// fetch a node from an absolute path
struct fs_node* fs_get_node_from_path(struct fs_node* root, const char* path) {
    if (!root) return NULL;
    if (path[0] != '/') return NULL; // must be absolute
    if (strcmp(path, "/") == 0) return root;

    const char* p = path + 1;
    struct fs_node* current = root;
    char part[64];
    int idx = 0;
    while (*p) {
        if (*p == '/') {
            part[idx] = '\0';
            struct fs_node* child = fs_find_child(current, part);
            if (!child) return NULL;
            current = child;
            idx = 0;
        } else {
            // FIX: a path component > 63 chars can never match a node name
            // (max 63). Previously unbounded -> stack smash via cd /AAAA...(64+)
            if (idx >= (int)sizeof(part) - 1) return NULL;
            part[idx++] = *p;
        }
        p++;
    }
    if (idx > 0) {
        part[idx] = '\0';
        struct fs_node* child = fs_find_child(current, part);
        if (!child) return NULL;
        current = child;
    }
    return current;
}

int fs_change_dir(struct fs_node** cwd, const char* path) {
    if (!*cwd) return -1;
    if (path[0] == '/') {
        // absolut
        struct fs_node* node = fs_get_node_from_path(root_node, path);
        if (node && node->is_dir) {
            *cwd = node;
            return 0;
        }
        return -1;
    } else {
        // relatif
        char path_copy[256];
        // FIX: check the length before strcpy (shell input may exceed 255 chars)
        if (strlen(path) >= sizeof(path_copy)) return -1;
        strcpy(path_copy, path);
        struct fs_node* current = *cwd;
        int i = 0;
        while (path_copy[i]) {
            // cari '/' atau akhir
            int start = i;
            while (path_copy[i] && path_copy[i] != '/') i++;
            char old = path_copy[i];
            path_copy[i] = '\0';
            const char* token = path_copy + start;
            if (strcmp(token, "..") == 0) {
                if (current->parent) current = current->parent;
                else current = root_node;
            } else if (strcmp(token, ".") != 0 && strlen(token) > 0) {
                struct fs_node* child = fs_find_child(current, token);
                if (!child || !child->is_dir) return -1;
                current = child;
            }
            if (old == '/') i++; // lanjut
        }
        *cwd = current;
        return 0;
    }
}