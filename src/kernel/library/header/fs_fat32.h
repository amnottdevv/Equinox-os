#ifndef FS_FAT32_H
#define FS_FAT32_H

// ============================================================
//  fs_fat32.h — FAT32 filesystem driver (v0.2 "Disk" phase)
// ------------------------------------------------------------
//  Phase A (read-only): mount a FAT32 partition, lazily mirror
//  its directory tree into the RAMFS under /mnt, read whole
//  files through the ATA PIO layer (cluster-chain walk with
//  contiguous-run coalescing), LFN (long file name) support.
//
//  Phase B (read/write): write-through operations — create
//  files, overwrite/extend/truncate, create directories,
//  delete — with full FAT + FSInfo maintenance, LFN entry
//  generation (with 8.3 name mangling and collision suffixes)
//  and CMOS-RTC timestamps.
//
//  Integration model: fs_ram.cpp stores a FAT32 "backing store"
//  in fs_node (backing == 1). Directory listings populate lazily
//  (on the first fs_find_child/fs_ls); file contents load lazily
//  via fat32_read_whole() into the disk arena (a bump allocator
//  placed after the GRUB module staging area). Every mutating
//  fs_ram call on a FAT-backed directory is written through to
//  the disk immediately — there is no dirty cache to flush.
// ============================================================

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fs_node;

// ---- boot-time bring-up -------------------------------------------

// Scan ATA buses, probe the MBR of every hard disk and mount the
// first FAT32 partition found at /mnt. Called once from kernel_main
// after the RAMFS exists; prints its own boot-log lines. Safe when
// no disk is attached (prints "ATA: no disks found" and returns).
void fat32_boot_init(uint32_t mem_upper_bytes);

// Hook so the kernel's boot_log() captures the disk bring-up lines
// (must be called before fat32_boot_init).
void fat32_set_boot_log(void (*fn)(const char*));

// Shell command `mount`: (re-)mount the first FAT32 partition.
// Returns 0 on success, <0 on failure (message already printed).
int fat32_mount_cmd(void);

// Shell command `umount`.
int fat32_unmount_cmd(void);

// Shell command `diskinfo`: drives, partitions, filesystem layout,
// free space, arena usage.
void fat32_diskinfo(void);

// ---- VFS bridge (called from fs_ram.cpp) --------------------------

// Lazily read a FAT-backed directory and mirror it into the RAMFS
// (children linked list). Idempotent — a populated flag guards the
// second call. No-op for RAMFS nodes.
void fat32_populate_dir(struct fs_node* dir);

// Lazily load the whole content of a FAT-backed file into memory
// (arena first, small files may fall back to the kernel heap) and
// point node->content at it. Returns 0 on success (also when the
// node needs no IO: RAMFS node, empty file, already cached).
int fat32_read_whole(struct fs_node* file);

// ---- Phase B: write path (all write-through) -----------------------

// Create a new empty file `name` inside FAT-backed directory `dir`
// (8.3 + LFN entries, RTC timestamps). Returns 0 / <0 on error.
int fat32_create_file(struct fs_node* dir, const char* name);

// Overwrite `file` with `len` bytes from `data` (extend or shrink
// the cluster chain, update the dirent size + timestamps, refresh
// the RAM cache). Returns 0 / <0.
int fat32_write_file(struct fs_node* file, const uint8_t* data,
                     uint32_t len);

// Create a subdirectory (allocates one cluster, writes "." and "..").
int fat32_create_dir(struct fs_node* dir, const char* name);

// Delete a file or an EMPTY directory (marks its dirent run 0xE5,
// frees the cluster chain). Returns 0 / <0.
int fat32_delete(struct fs_node* node);

// ---- status helpers ------------------------------------------------

struct fat32_mount* fat32_get_mount(void);
int                 fat32_mount_is_writable(void);

// Arena stats for diskinfo: bytes used / total.
uint32_t fat32_arena_used(void);
uint32_t fat32_arena_total(void);

#ifdef __cplusplus
}
#endif

#endif // FS_FAT32_H
