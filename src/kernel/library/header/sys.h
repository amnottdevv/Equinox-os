#ifndef SYS_H
#define SYS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the system (must be called after fs_init)
void sys_init(void);

// File & Directory operations
int sys_create_file(const char* path, const char* content);
int sys_create_dir(const char* path);
int sys_delete(const char* path);
int sys_rename(const char* old_path, const char* new_path);
int sys_copy(const char* src_path, const char* dest_path);

// Directory navigation
int sys_chdir(const char* path);
char* sys_getcwd(char* buffer, size_t size);

// Information
int sys_path_exists(const char* path);
int sys_is_dir(const char* path);
int sys_is_file(const char* path);
void sys_ls(const char* path);

#ifdef __cplusplus
}
#endif

#endif
