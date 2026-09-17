#ifndef FS_RAM_H
#define FS_RAM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Filesystem node (file or directory)
struct fs_node {
    char name[64];
    uint8_t is_dir;          // 1 if directory, 0 if file
    uint8_t is_ref;          // v10.9: 1 = zero-copy content (GRUB module
                             // staging) - pointer does NOT belong to the
                             // kernel heap, never free() it
    uint32_t size;           // file size (0 for dirs)
    char* content;           // file contents (malloc'd), NULL for dirs
    struct fs_node* parent;  // parent directory
    struct fs_node* children; // linked list of children (for dirs)
    struct fs_node* next;    // next sibling
};

// Initialize the filesystem (creates the root)
void fs_init(void);

// Get the root node
struct fs_node* fs_get_root(void);

// Find a child with the given name under parent
struct fs_node* fs_find_child(struct fs_node* parent, const char* name);

// Create a (empty) file under parent
int fs_create_file(struct fs_node* parent, const char* name, const char* content);

// Write BINARY data (may contain 0x00 bytes) to a file that ALREADY EXISTS.
// fs_create_file() uses strcpy/strlen so the file gets truncated at the
// first NUL byte -- not safe for machine code / .mrp. This function uses
// an explicit length (memcpy), not strlen, so it is safe for binary data.
// If the file does not exist yet, it is created (empty) first, then written.
// Returns 0 on success, <0 on failure (see fs_ram.cpp for error codes).
int fs_write_binary(struct fs_node* parent, const char* name,
                     const uint8_t* data, uint32_t len);

// v10.9 "WAD path": create a file that POINTS at data already in
// memory (GRUB module staging at 0x2800000+, supervisor pages)
// WITHOUT copying it into the kernel heap — for large files (4.2 MB
// doom1.wad) that could never fit in the 2 MB KERNEL_HEAP. The
// resulting node reads like a regular file (read/readfile/lseek use
// the content directly); is_ref=1 keeps free()/overwrite safe.
// Returns 0 on success, <0 on failure (same error codes as fs_write_binary).
int fs_reference_binary(struct fs_node* parent, const char* name,
                        const uint8_t* data, uint32_t len);

// Create a directory under parent
int fs_create_dir(struct fs_node* parent, const char* name);

// Delete a node (file or empty directory)
int fs_delete_node(struct fs_node* parent, const char* name);

// Get the absolute path of a node
char* fs_get_path(struct fs_node* node, char* buffer, size_t bufsize);

// List directory contents (prints to the screen)
void fs_ls(struct fs_node* dir, int show_details);

// Display a (recursive) tree
void fs_tree(struct fs_node* dir, int depth);

// Change directory by path (absolute or relative)
// returns 0 on success, -1 on failure
int fs_change_dir(struct fs_node** cwd, const char* path);

// Get a node from an absolute path
struct fs_node* fs_get_node_from_path(struct fs_node* root, const char* path);

#ifdef __cplusplus
}
#endif

#endif