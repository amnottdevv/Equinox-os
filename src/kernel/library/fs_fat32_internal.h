#ifndef FS_FAT32_INTERNAL_H
#define FS_FAT32_INTERNAL_H

// ============================================================
//  fs_fat32_internal.h — shared internals of the FAT32 driver
//  (used by fs_fat32.cpp = mount/read, fs_fat32_write.cpp = write)
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include "header/fs_ram.h"
#include "header/ata.h"

#define FAT_SECTOR 512

// ---- on-disk structures -------------------------------------------

// Directory entry attribute bits
#define FAT_ATTR_RO      0x01
#define FAT_ATTR_HIDDEN  0x02
#define FAT_ATTR_SYSTEM  0x04
#define FAT_ATTR_VOLUME  0x08
#define FAT_ATTR_DIR     0x10
#define FAT_ATTR_ARCHIVE 0x20
#define FAT_ATTR_LFN     0x0F   // (RO|HIDDEN|SYSTEM|VOLUME)

#define FAT_DIRENTS_PER_SECTOR (FAT_SECTOR / 32)

// Cluster chain terminators / specials
#define FAT32_FREE      0x00000000u
#define FAT32_EOC_MIN   0x0FFFFFF8u   // >= this = end of chain
#define FAT32_BAD       0x0FFFFFF7u
#define FAT32_MASK      0x0FFFFFFFu   // low 28 bits are the cluster value

// ---- the mount -----------------------------------------------------

struct fat32_mount {
    int      ata_slot;        // ATA drive slot
    uint64_t part_lba;        // absolute LBA of the partition start
    uint16_t bytes_per_sec;   // 512 (only 512 supported)
    uint8_t  sec_per_clus;    // power of two
    uint16_t reserved_secs;   // reserved sectors before FAT1
    uint8_t  num_fats;        // almost always 2
    uint32_t fat_secs;        // sectors per FAT copy
    uint32_t total_secs;      // total sectors (32-bit field)
    uint32_t root_clus;       // first cluster of the root dir (usually 2)
    uint32_t data_start;      // first data sector (partition-relative)
    uint32_t cluster_count;   // number of data clusters
    uint32_t free_clusters;   // maintained on alloc/free (from FSInfo or counted)
    uint32_t next_free;       // allocation hint
    uint32_t fsinfo_sector;   // partition-relative FSInfo sector (usually 1)
    uint32_t cluster_bytes;   // sec_per_clus * 512
    char     label[12];       // volume label ("" when absent)
    uint8_t  writable;        // 1 = write-through enabled
    uint8_t  mounted;         // 1 = mount valid
    struct fs_node* mount_pt; // the RAMFS directory node (/mnt)
    // single-sector cache (FAT + dirent read-modify-write locality)
    uint8_t  cache[FAT_SECTOR];
    uint64_t cache_lba;
    uint8_t  cache_valid;
    uint8_t  cache_dirty;
    uint32_t hits, misses;    // stats for diskinfo
};

extern struct fat32_mount g_fat;   // the one mount of v0.2

// ---- sector layer (fs_fat32.cpp) -----------------------------------

// Absolute-LBA sector IO through the ATA driver, single-sector
// cached variant for FAT/dirent read-modify-write patterns.
int  fat_rd_sectors(uint64_t lba, uint32_t count, void* buf);
int  fat_wr_sectors(uint64_t lba, uint32_t count, const void* buf);

// Cached sector access: returns a pointer to a stable 512-byte
// buffer holding `lba` (reads it when not cached).
//   fat_cache_get(lba)          — for read + modify (mark dirty explicitly)
//   fat_cache_put()             — flush a dirty cache, keep the line
//   fat_cache_zero(lba)         — mark cache as all-zero (dirty) without IO
uint8_t* fat_cache_get(uint64_t lba);
void     fat_cache_put(void);
void     fat_cache_zero(uint64_t lba);

// ---- FAT table ops (fs_fat32.cpp) ----------------------------------

// Value of FAT[cluster] (0xFFFFFFFF on IO error).
uint32_t fat_entry(uint32_t cluster);

// Write FAT[cluster] = value (masked to 28 bits) into EVERY FAT copy.
// Uses the sector cache; caller must fat_cache_put() before the next
// uncached access of a different sector (helpers below do that).
int fat_entry_set(uint32_t cluster, uint32_t value);

// Find a free cluster (linear scan from g_fat.next_free).
// Returns the cluster number or 0 when the volume is full.
uint32_t fat_alloc_cluster(void);

// Allocate `n` clusters as a chain, returns the first (0 on full).
uint32_t fat_alloc_chain(uint32_t n);

// Free the whole chain starting at `cluster` (marks entries FREE).
int fat_free_chain(uint32_t cluster);

// Number of clusters in the chain starting at `cluster`.
uint32_t fat_chain_length(uint32_t cluster);

// ---- cluster <-> LBA ------------------------------------------------
static inline uint64_t fat_cluster_lba(uint32_t cluster) {
    return g_fat.part_lba + g_fat.data_start +
           (uint64_t)(cluster - 2) * g_fat.sec_per_clus;
}

// ---- FSInfo ----------------------------------------------------------
// v0.2 write hardening: the in-memory free/next counters are updated
// on every alloc/free, but the FSInfo SECTOR is written only once per
// public operation (fat_fsinfo_sync) — writing it per cluster turned
// a 4 MB file write into thousands of extra sector writes (the cache
// thrashed FAT <-> FSInfo on every single cluster).
void fat_fsinfo_sync(void);

// ---- RTC (CMOS) — DOS date/time for dirent timestamps ---------------
struct dos_datetime { uint16_t date, time; };
struct dos_datetime fat_rtc_now(void);

// ---- LFN helpers (fs_fat32_write.cpp, also used when parsing) -------
uint8_t fat_lfn_checksum(const uint8_t short_name[11]);
void    fat_sfn_from_name(const char* name, uint8_t out[11], int* needs_lfn);

// Create + link a FAT-backed RAMFS node into `parent` (fs_fat32.cpp).
// `sfn` = the on-disk 8.3 short name of this dirent (11 raw bytes,
// "NAME    EXT") so later creates can collision-check against REAL
// short names, not just the long names shown in the mirror.
struct fs_node* fat_new_node(struct fs_node* parent, const char* name,
                             int is_dir, uint32_t first_clus, uint32_t size,
                             uint64_t dirent_lba, uint16_t dirent_idx,
                             uint16_t lfn_count, const uint8_t sfn[11]);

// Arena bump-alloc (owner: fs_fat32.cpp; 16-byte aligned, never freed).
uint8_t* fat_arena_alloc_public(uint32_t bytes);

// Node name limit (matches fs_node::name[64])
#define FAT_NAME_MAX 63

#endif // FS_FAT32_INTERNAL_H
