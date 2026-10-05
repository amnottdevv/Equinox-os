#ifndef FS_FAT32_H
#define FS_FAT32_H

// ============================================================
//  fs_fat32.h — FAT32 filesystem driver (0.4 Beta "Disk" phase)
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
// 1 = the mounted volume is a base (mounted as /, not /mnt).
extern "C" int fat32_base_active(void);

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

// ---- 0.4 Beta (D): progres penulisan -------------------------------
//  Hook opsional, dipanggil dari dalam loop data fat32_write_file()
//  setiap ~128 KB dengan (done, total). Dipakai equinoxinstall agar
//  salin file besar (doom1.wad 4 MB = puluhan ribu sektor + FLUSH
//  CACHE) menampilkan progres hidup, bukan diam terlihat seperti
//  hang. NULL = nonaktif (default).
typedef void (*fat32_write_progress_t)(uint32_t done, uint32_t total);
void fat32_set_write_progress(fat32_write_progress_t cb);

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

// ---- 0.4 Beta: slot selection, formatting, volume label ---------------
//  (used by the equinoxinstall wizard: pick a target disk, prepare it
//   as an install base, then copy the system onto it.)

struct fat32_slot_info {
    int      present;       // 1 = PATA drive present in this slot
    int      has_fat32;     // 1 = a FAT32 volume was found on it
    int      active;        // 1 = this slot is the currently mounted volume
    int      is_base;       // 1 = mounted AND promoted to root (/)
    uint64_t part_lba;      // partition start (0 = whole-disk volume)
    uint64_t part_sectors;  // partition length in sectors
    char     label[12];     // volume label from the BPB ("" = none)
    char     model[41];     // drive model string
    char     size[24];      // drive size, human readable
    char     part_size[24]; // partition size, human readable
};

// Read-only inspection of drive slot 0..3 (no mount side effects).
// Returns 0 when the slot holds a drive, -1 when it is empty.
int fat32_slot_scan(int slot, struct fat32_slot_info* out);

// Mount ONE drive slot at /mnt. Refuses while a base volume is the
// root filesystem (returns -2). 0 = success (message printed).
int fat32_mount_slot(int slot);

// Rewrite the label of the MOUNTED volume (BPB offset 71 + the root
// label entry). Use "EQUINOXBASE" to make it a boot base. 0 = ok.
int fat32_set_label(const char* label);

// DESTRUCTIVE: format drive `slot` as an equinox install base — MBR
// (type 0x0C partition at LBA 2048 + int 18h stub) + FAT32 volume
// labelled `label`. Refuses when the slot is the mounted volume (-2)
// or the drive is too small (-3). 0 = success.
int fat32_mkfs(int slot, const char* label);

// 0.4 Beta FR-08: return a fat_arena_alloc_public() payload to the
// free-list pool (called by fs_content_release in fs_ram.cpp).
// Out-of-range / wild pointers are ignored, never fatal.
void fat_arena_free(void* ptr);

// 0.4 Beta FR-01: arena allocation for syscall write buffers (growing
// FAT-backed files). Returns NULL when the arena has no fit.
uint8_t* fat_arena_alloc_public(uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif // FS_FAT32_H
