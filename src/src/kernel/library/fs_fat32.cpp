// ============================================================
//  fs_fat32.cpp — FAT32 mount + read path (0.4 Beta, Phase A)
// ------------------------------------------------------------
//  Everything mount-related lives here: BPB parsing, FSInfo,
//  the arena (file-content cache allocator), the single-sector
//  cache, FAT table access, MBR partition probing, the lazy
//  directory mirror (fat32_populate_dir) and whole-file reads
//  (fat32_read_whole). The write path is in fs_fat32_write.cpp.
// ============================================================

#include "fs_fat32_internal.h"
#include "header/fs_fat32.h"
#include "header/task.h"   // Phase A: sched-lock
#include "header/blk.h"      // 0.4 Beta: generic block layer (PATA + AHCI)
#include "header/stdio.h"
#include "header/malloc.h"
#include "header/libstring.h"   // memset/memcpy
#include "header/ecf.h"         // 0.4 Beta: set -b -> base.path (dibaca saat boot)

// ===================================================================
//  THE mount (0.4 Beta: exactly one FAT32 volume at a time)
// ===================================================================
struct fat32_mount g_fat;

// Boot-log hook: kernel.cpp registers its boot_log() here so the
// disk bring-up lines are stored + replayed on the VESA console.
static void (*fat_boot_log)(const char*) = NULL;

// ===================================================================
//  ARENA — file-content cache memory
// ------------------------------------------------------------
//  FAT-backed file contents are cached OUTSIDE the 2 MB kernel
//  heap: an allocator placed right after the GRUB module staging
//  area (0x2800000 + 12 MB = 0x3400000). Identity-mapped by
//  paging_init (0-64 MB), supervisor pages.
//
//  0.4 Beta FR-08: the arena is now a FREE-LIST (address-ordered blocks
//  with split + coalesce, same design as malloc.cpp) instead of a
//  bump pointer. A cache is released with fat_arena_free() when the
//  last fd referencing the file closes (fs_content_release), so
//  reading many files in sequence no longer exhausts the arena.
//  Arena-cached nodes carry is_ref = 1 AND cont_owner = 2 so the
//  generic release path knows to route the pointer back here.
//
//  The usable size is clamped against the multiboot upper-memory
//  figure so a machine with less than 64 MB degrades gracefully
//  (small files then use the kernel heap instead).
// ===================================================================
#define FAT_ARENA_BASE 0x3400000u
#define FAT_ARENA_SIZE 0x800000u        // 8 MB -> ends at 60 MB

struct fat_block {
    uint32_t magic;       /* 0x46415400 "FAT" + NUL — corruption canary */
    uint32_t size;        /* payload size (without this 16-byte header)  */
    uint8_t  free_flag;
    uint8_t  pad[3];      /* keep the header 16 bytes, 16-aligned        */
    struct fat_block* next;   /* next block, address-ordered            */
};
#define FAT_BLOCK_MAGIC  0x46415400u
#define FAT_MIN_SPLIT    64u

static struct fat_block* arena_head = NULL;   /* NULL = not initialized */
static uint32_t arena_limit = 0;
static uint32_t arena_bump   = 0;             /* high-water mark (stats) */

static void fat_arena_init(uint32_t mem_upper_bytes) {
    uint32_t room = 0;
    if (mem_upper_bytes > FAT_ARENA_BASE)
        room = mem_upper_bytes - FAT_ARENA_BASE;
    if (room > FAT_ARENA_SIZE) room = FAT_ARENA_SIZE;
    arena_limit = room;
    arena_bump  = 0;
    arena_head  = NULL;                 /* lazily laid out on first alloc */
}

/* Internal: first-fit allocation over the block list. 16-byte aligned
 * payload, splits large free blocks, returns NULL when exhausted. */
static uint8_t* fat_arena_alloc(uint32_t bytes) {
    if (bytes == 0 || arena_limit < sizeof(struct fat_block)) return NULL;
    uint32_t need = (bytes + 15u) & ~15u;

    if (!arena_head) {
        /* first allocation: lay out one big free block */
        struct fat_block* first = (struct fat_block*)FAT_ARENA_BASE;
        first->magic = FAT_BLOCK_MAGIC;
        first->size  = arena_limit - sizeof(struct fat_block);
        first->free_flag = 1;
        first->next  = NULL;
        arena_head = first;
    }

    for (struct fat_block* b = arena_head; b; b = b->next) {
        if (!b->free_flag || b->size < need) continue;
        /* split when the remainder is worth a header */
        if (b->size - need >= sizeof(struct fat_block) + FAT_MIN_SPLIT) {
            struct fat_block* nb =
                (struct fat_block*)((uint8_t*)b + sizeof(*b) + need);
            nb->magic = FAT_BLOCK_MAGIC;
            nb->size  = b->size - need - sizeof(struct fat_block);
            nb->free_flag = 1;
            nb->next  = b->next;
            b->size   = need;
            b->next   = nb;
        }
        b->free_flag = 0;
        uint32_t off = (uint32_t)(uintptr_t)b + sizeof(*b) - FAT_ARENA_BASE;
        if (off + sizeof(*b) + b->size > arena_bump)
            arena_bump = off + sizeof(*b) + b->size;
        return (uint8_t*)b + sizeof(struct fat_block);
    }
    return NULL;      /* no fit: arena fragmented or full */
}

/* 0.4 Beta FR-08: return a fat_arena_alloc() payload to the pool.
 * Coalesces with the physical neighbors. Out-of-range pointers are
 * ignored (the caller may hand us kernel-heap/staging pointers by
 * mistake — never crash on those). */
void fat_arena_free(void* ptr) {
    if (!ptr) return;
    uint32_t p = (uint32_t)(uintptr_t)ptr;
    if (p < FAT_ARENA_BASE + sizeof(struct fat_block) ||
        p >= FAT_ARENA_BASE + arena_limit)
        return;
    struct fat_block* b = (struct fat_block*)(p - sizeof(struct fat_block));
    if (b->magic != FAT_BLOCK_MAGIC || b->free_flag) return;  /* wild/double free */
    b->free_flag = 1;
    /* coalesce forward */
    while (b->next && b->next->free_flag) {
        struct fat_block* nx = b->next;
        b->size += sizeof(*b) + nx->size;
        b->next  = nx->next;
    }
    /* coalesce backward: find the predecessor by walking (the list is
     * address-ordered, so the predecessor of a middle block is unique) */
    struct fat_block* prev = NULL;
    for (struct fat_block* c = arena_head; c && c != b; c = c->next) prev = c;
    if (prev && prev->free_flag) {
        prev->size += sizeof(*b) + b->size;
        prev->next  = b->next;
        /* coalesce the merged block forward as well */
        while (prev->next && prev->next->free_flag) {
            struct fat_block* nx = prev->next;
            prev->size += sizeof(*prev) + nx->size;
            prev->next  = nx->next;
        }
    }
}

uint32_t fat32_arena_used(void) {
    uint32_t used = 0;
    for (struct fat_block* b = arena_head; b; b = b->next)
        if (!b->free_flag) used += sizeof(*b) + b->size;
    return used;
}
uint32_t fat32_arena_total(void) { return arena_limit; }

// Arena access for the write path (fs_fat32_write.cpp).
uint8_t* fat_arena_alloc_public(uint32_t bytes) { return fat_arena_alloc(bytes); }

struct fat32_mount* fat32_get_mount(void) { return g_fat.mounted ? &g_fat : NULL; }
int fat32_mount_is_writable(void) { return g_fat.mounted && g_fat.writable; }

// ===================================================================
//  RTC — CMOS date/time for dirent timestamps (Phase B)
// ===================================================================
static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}
static int bcd2(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

struct dos_datetime fat_rtc_now(void) {
    struct dos_datetime dt = { 0x2A21, 0 };   // fallback: 2000-01-01 00:00
    for (int t = 0; t < 30 && (cmos(0x0A) & 0x80); t++) { }  // skip update
    uint8_t rb   = cmos(0x0B);
    int bcd_mode = !(rb & 0x04);
    int hour24   = !(rb & 0x02);

    int sec  = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    int day  = cmos(0x07), mon = cmos(0x08), yr = cmos(0x09);
    if (bcd_mode) {
        sec = bcd2(sec); min = bcd2(min); hour = bcd2(hour);
        day = bcd2(day); mon = bcd2(mon); yr = bcd2(yr);
    }
    if (!hour24 && (hour & 0x80)) hour = ((hour & 0x7F) + 12) % 24;
    int year = 2000 + yr;
    if (year < 1980 || year > 2107) { year = 2000; mon = 1; day = 1; }
    if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
        sec > 59 || min > 59 || hour > 23) { mon = 1; day = 1; }
    dt.date = (uint16_t)(((year - 1980) << 9) | (mon << 5) | day);
    dt.time = (uint16_t)((hour << 11) | (min << 5) | (sec >> 1));
    return dt;
}

// ===================================================================
//  SECTOR LAYER
// ===================================================================

// Chunked multi-sector transfer through the block layer (works for a
// PATA disk in slot 0..3 and an AHCI disk in slot 4..7 alike).
static int fat_io(uint64_t lba, uint32_t count, void* buf, int write) {
    uint8_t* p = (uint8_t*)buf;
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        int r = write ? blk_write(g_fat.blk_slot, lba, n, p)
                      : blk_read (g_fat.blk_slot, lba, n, p);
        if (r != 0) return r;
        p   += n * FAT_SECTOR;
        lba += n;
        count -= n;
    }
    return 0;
}

int fat_rd_sectors(uint64_t lba, uint32_t count, void* buf) {
    return fat_io(lba, count, buf, 0);
}
int fat_wr_sectors(uint64_t lba, uint32_t count, const void* buf) {
    return fat_io(lba, count, (void*)buf, 1);
}

// ---- single-sector cache ------------------------------------------

static void fat_cache_flush(void) {
    if (g_fat.cache_valid && g_fat.cache_dirty) {
        if (fat_wr_sectors(g_fat.cache_lba, 1, g_fat.cache) != 0) {
            printf("FAT32: cache flush failed at LBA %u\n",
                   (unsigned)g_fat.cache_lba);
        }
        g_fat.cache_dirty = 0;
    }
}

uint8_t* fat_cache_get(uint64_t lba) {
    if (g_fat.cache_valid && g_fat.cache_lba == lba) {
        g_fat.hits++;
        return g_fat.cache;
    }
    fat_cache_flush();                          // write out the old line
    if (fat_rd_sectors(lba, 1, g_fat.cache) != 0) {
        g_fat.cache_valid = 0;
        return NULL;
    }
    g_fat.cache_valid = 1;
    g_fat.cache_lba   = lba;
    g_fat.cache_dirty = 0;
    g_fat.misses++;
    return g_fat.cache;
}

void fat_cache_put(void) { fat_cache_flush(); }

// The caller is about to overwrite the whole cached sector.
void fat_cache_zero(uint64_t lba) {
    g_fat.cache_valid = 1;
    g_fat.cache_lba   = lba;
    g_fat.cache_dirty = 1;
    memset(g_fat.cache, 0, FAT_SECTOR);
}

// ===================================================================
//  FAT TABLE
// ===================================================================

uint32_t fat_entry(uint32_t cluster) {
    uint64_t lba = g_fat.part_lba + g_fat.reserved_secs +
                   (uint64_t)(cluster * 4u) / FAT_SECTOR;
    uint32_t off = (cluster * 4u) % FAT_SECTOR;
    uint8_t* sec = fat_cache_get(lba);
    if (!sec) return 0xFFFFFFFFu;
    uint32_t v = (uint32_t)sec[off] | ((uint32_t)sec[off+1] << 8) |
                 ((uint32_t)sec[off+2] << 16) | ((uint32_t)sec[off+3] << 24);
    return v & FAT32_MASK;
}

int fat_entry_set(uint32_t cluster, uint32_t value) {
    // A 28-bit value never touches the reserved top nibble, so the
    // read-modify-write below preserves FAT flags on real disks.
    uint64_t lba = g_fat.part_lba + g_fat.reserved_secs +
                   (uint64_t)(cluster * 4u) / FAT_SECTOR;
    uint32_t off = (cluster * 4u) % FAT_SECTOR;
    for (uint8_t f = 0; f < g_fat.num_fats; f++) {
        uint8_t* sec = fat_cache_get(lba + (uint64_t)f * g_fat.fat_secs);
        if (!sec) return -1;
        uint32_t old = (uint32_t)sec[off] | ((uint32_t)sec[off+1] << 8) |
                       ((uint32_t)sec[off+2] << 16) | ((uint32_t)sec[off+3] << 24);
        uint32_t nw = (old & ~FAT32_MASK) | (value & FAT32_MASK);
        sec[off]   = (uint8_t)(nw);
        sec[off+1] = (uint8_t)(nw >> 8);
        sec[off+2] = (uint8_t)(nw >> 16);
        sec[off+3] = (uint8_t)(nw >> 24);
        g_fat.cache_dirty = 1;
    }
    fat_cache_put();
    return 0;
}

uint32_t fat_alloc_cluster(void) {
    uint32_t start = g_fat.next_free ? g_fat.next_free : 2;
    for (uint32_t i = 0; i < g_fat.cluster_count; i++) {
        uint32_t c = start + i;
        if (c >= 2 + g_fat.cluster_count) c -= g_fat.cluster_count;  // wrap
        if (c < 2) c = 2;
        if (fat_entry(c) == FAT32_FREE) {
            fat_entry_set(c, FAT32_EOC_MIN);      // mark end-of-chain
            g_fat.free_clusters--;
            g_fat.next_free = c + 1;
            // FSInfo sector synced once per public op (fat_fsinfo_sync)
            return c;
        }
    }
    return 0;   // volume full
}

uint32_t fat_alloc_chain(uint32_t n) {
    if (n == 0) return 0;
    uint32_t first = 0, prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = fat_alloc_cluster();
        if (c == 0) {
            if (first) fat_free_chain(first);     // roll back
            return 0;
        }
        if (prev) fat_entry_set(prev, c); else first = c;
        prev = c;
    }
    return first;
}

int fat_free_chain(uint32_t cluster) {
    uint32_t guard = 0;
    while (cluster >= 2 && cluster < 2 + g_fat.cluster_count &&
           fat_entry(cluster) != FAT32_FREE && guard++ < 1000000u) {
        uint32_t next = fat_entry(cluster);
        if (next >= FAT32_EOC_MIN) next = 0;
        fat_entry_set(cluster, FAT32_FREE);
        g_fat.free_clusters++;
        if (next == 0) break;
        cluster = next;
    }
    // FSInfo sector synced once per public op (fat_fsinfo_sync)
    return 0;
}

uint32_t fat_chain_length(uint32_t cluster) {
    uint32_t n = 0, guard = 0;
    while (cluster >= 2 && cluster < 2 + g_fat.cluster_count &&
           guard++ < 1000000u) {
        n++;
        cluster = fat_entry(cluster);
        if (cluster >= FAT32_EOC_MIN || cluster == FAT32_FREE) break;
    }
    return n;
}

// ===================================================================
//  FSInfo
// ===================================================================
// 0.4 Beta write hardening: alloc/free only maintain the IN-MEMORY
// counters; the FSInfo sector is synced once per public operation
// (and on unmount). Per-cluster sector writes made large writes
// O(clusters) extra IO and wore the cache line out.
void fat_fsinfo_sync(void) {
    if (!g_fat.mounted) return;
    uint8_t* sec = fat_cache_get(g_fat.part_lba + g_fat.fsinfo_sector);
    if (!sec) return;
    // keep the four signatures, refresh the two counters
    sec[488] = (uint8_t)g_fat.free_clusters;
    sec[489] = (uint8_t)(g_fat.free_clusters >> 8);
    sec[490] = (uint8_t)(g_fat.free_clusters >> 16);
    sec[491] = (uint8_t)(g_fat.free_clusters >> 24);
    sec[492] = (uint8_t)g_fat.next_free;
    sec[493] = (uint8_t)(g_fat.next_free >> 8);
    sec[494] = (uint8_t)(g_fat.next_free >> 16);
    sec[495] = (uint8_t)(g_fat.next_free >> 24);
    g_fat.cache_dirty = 1;
    fat_cache_put();
}

// ===================================================================
//  MOUNT
// ===================================================================

// Validity probe for a FAT32 BPB at absolute sector `lba`.
// Read sector `lba` of drive `slot` and validate it as a FAT32 BPB.
// 0.4 Beta: slot diberikan EKSPLISIT — pemanggilan terjadi SEBELUM g_fat
// terisi (fat32_do_mount menulis g_fat setelah probe lolos), jadi
// jalur cache fat_rd_sectors() (yang memakai g_fat.blk_slot, masih 0
// dari BSS) akan membaca DISK YANG SALAH pada mesin multi-disk.
static int fat32_probe(int slot, uint64_t lba, uint8_t* bpb) {
    if (blk_read(slot, lba, 1, bpb) != 0) return -1;
    uint16_t bps      = (uint16_t)bpb[11] | ((uint16_t)bpb[12] << 8);
    uint16_t root_ent = (uint16_t)bpb[17] | ((uint16_t)bpb[18] << 8);
    uint16_t tot16    = (uint16_t)bpb[19] | ((uint16_t)bpb[20] << 8);
    uint16_t fat16    = (uint16_t)bpb[22] | ((uint16_t)bpb[23] << 8);
    uint32_t fat32v   = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
                        ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    if (bps != 512) return -2;                              // 512-byte only
    if (root_ent != 0 || tot16 != 0 || fat16 != 0) return -3; // FAT12/16 shape
    if (fat32v == 0) return -4;
    // BPB signature (0x29 at offset 66) or the "FAT32     " fstype string
    if (bpb[66] != 0x29 && !(bpb[82]=='F'&&bpb[83]=='A'&&bpb[84]=='T'&&
                              bpb[85]=='3'&&bpb[86]=='2')) return -5;
    return 0;
}

// Count free clusters by scanning the FAT (when FSInfo is unusable).
static uint32_t fat_count_free(void) {
    uint32_t free_c = 0;
    uint32_t limit = g_fat.cluster_count + 2;
    uint32_t total_entries = (g_fat.fat_secs * FAT_SECTOR) / 4;
    if (limit > total_entries) limit = total_entries;
    for (uint32_t c = 2; c < limit; c++) {
        if (fat_entry(c) == FAT32_FREE) free_c++;
    }
    return free_c;
}

// Build the mount from the BPB. `mount_pt` = the RAMFS dir node that
// will mirror the FAT root (usually /mnt). Returns 0 on success.
static int fat32_do_mount(int slot, uint64_t lba, struct fs_node* mount_pt) {
    uint8_t bpb[FAT_SECTOR];
    int pr = fat32_probe(slot, lba, bpb);
    if (pr != 0) return pr;

    memset((uint8_t*)&g_fat, 0, sizeof(g_fat));
    g_fat.blk_slot      = slot;
    g_fat.part_lba      = lba;
    g_fat.sec_per_clus  = bpb[13];
    g_fat.reserved_secs = (uint16_t)bpb[14] | ((uint16_t)bpb[15] << 8);
    g_fat.num_fats      = bpb[16];
    g_fat.fat_secs      = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
                          ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    g_fat.total_secs    = (uint32_t)bpb[32] | ((uint32_t)bpb[33] << 8) |
                          ((uint32_t)bpb[34] << 16) | ((uint32_t)bpb[35] << 24);
    g_fat.root_clus     = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                          ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);
    uint16_t fsinfo     = (uint16_t)bpb[48] | ((uint16_t)bpb[49] << 8);

    if (g_fat.sec_per_clus == 0 ||
        (g_fat.sec_per_clus & (g_fat.sec_per_clus - 1)) != 0 ||
        g_fat.num_fats == 0 || g_fat.fat_secs == 0 ||
        g_fat.total_secs == 0 || g_fat.root_clus < 2) return -10;

    g_fat.data_start    = g_fat.reserved_secs + g_fat.num_fats * g_fat.fat_secs;
    uint32_t data_secs  = g_fat.total_secs - g_fat.data_start;
    g_fat.cluster_count = data_secs / g_fat.sec_per_clus;
    if (g_fat.cluster_count < 2) return -11;
    g_fat.cluster_bytes = (uint32_t)g_fat.sec_per_clus * 512;
    g_fat.writable      = 1;
    g_fat.mounted       = 1;
    g_fat.mount_pt      = mount_pt;
    g_fat.fsinfo_sector = fsinfo ? fsinfo : 1;

    // Turn the RAMFS mount-point node itself into the FAT root: from
    // here on, fs_find_child/fs_ls lazily mirror the FAT root dir.
    mount_pt->backing       = 1;
    mount_pt->populated     = 0;
    mount_pt->first_cluster = g_fat.root_clus;
    mount_pt->size          = 0;
    mount_pt->mnt           = &g_fat;
    mount_pt->lfn_count     = 0;
    mount_pt->dirent_sector = 0;
    mount_pt->dirent_index  = 0;

    // volume label from the BPB; blank when absent
    for (int i = 0; i < 11; i++) g_fat.label[i] = bpb[71 + i];
    g_fat.label[11] = '\0';
    for (int i = 10; i >= 0 && (g_fat.label[i] == ' ' || g_fat.label[i] == 0);
         i--) g_fat.label[i] = 0;

    // FSInfo free-count hint (validated; otherwise counted by scan)
    uint8_t fi[FAT_SECTOR];
    g_fat.free_clusters = 0xFFFFFFFFu;
    g_fat.next_free = 0;
    if (fsinfo && fat_rd_sectors(lba + fsinfo, 1, fi) == 0 &&
        fi[0]=='R'&&fi[1]=='R'&&fi[2]=='a'&&fi[3]=='A' &&
        fi[484]=='r'&&fi[485]=='r'&&fi[486]=='A'&&fi[487]=='a') {
        uint32_t fc = (uint32_t)fi[488] | ((uint32_t)fi[489] << 8) |
                      ((uint32_t)fi[490] << 16) | ((uint32_t)fi[491] << 24);
        uint32_t nf = (uint32_t)fi[492] | ((uint32_t)fi[493] << 8) |
                      ((uint32_t)fi[494] << 16) | ((uint32_t)fi[495] << 24);
        if (fc <= g_fat.cluster_count) g_fat.free_clusters = fc;
        if (nf >= 2 && nf < 2 + g_fat.cluster_count) g_fat.next_free = nf;
    }
    if (g_fat.free_clusters == 0xFFFFFFFFu)
        g_fat.free_clusters = fat_count_free();
    if (g_fat.next_free == 0) g_fat.next_free = 2;

    return 0;
}

// ===================================================================
//  DIRECTORY PARSING (lazy RAMFS mirror — Phase A core)
// ===================================================================

// Assemble the 8.3 on-disk name ("NAME    EXT") into "NAME.EXT".
static void sfn_to_name(const uint8_t* de, char* out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < 8 && o + 1 < cap; i++) {
        if (de[i] == ' ') break;
        out[o++] = de[i];
    }
    if (de[8] != ' ' && o + 1 < cap) {
        out[o++] = '.';
        for (int i = 8; i < 11 && o + 1 < cap; i++) {
            if (de[i] == ' ') break;
            out[o++] = de[i];
        }
    }
    out[o] = '\0';
    // 0x05 first byte means 0xE5 (kanji lead byte quirk)
    if (out[0] == 0x05) out[0] = (char)0xE5;
}

// Append one UCS-2 code unit to the LFN buffer (ASCII projection).
static void lfn_put_char(char* buf, size_t* len, size_t cap, uint16_t u) {
    if (u == 0 || u == 0xFFFF || *len + 1 >= cap) return;
    buf[(*len)++] = (u < 0x80) ? (char)(uint8_t)u : '?';
}

// Read one LFN entry (13 UTF-16 chars) into buf.
static void lfn_read_entry(const uint8_t* e, char* buf, size_t* len,
                           size_t cap) {
    static const int offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24,
                                  28, 30 };
    for (int i = 0; i < 13; i++) {
        uint16_t u = (uint16_t)e[offs[i]] | ((uint16_t)e[offs[i] + 1] << 8);
        if (u == 0x0000) {           // terminator: rest of the name is padding
            break;
        }
        lfn_put_char(buf, len, cap, u);
    }
}

// Create + link a FAT-backed RAMFS node (shared with the write path).
struct fs_node* fat_new_node(struct fs_node* parent, const char* name,
                                    int is_dir, uint32_t first_clus,
                                    uint32_t size, uint64_t dirent_lba,
                                    uint16_t dirent_idx, uint16_t lfn_count,
                                    const uint8_t sfn[11]) {
    struct fs_node* n = (struct fs_node*)malloc(sizeof(struct fs_node));
    if (!n) return NULL;
    size_t l = strlen(name);
    if (l > FAT_NAME_MAX) l = FAT_NAME_MAX;
    for (size_t i = 0; i < l; i++) n->name[i] = name[i];
    n->name[l] = '\0';
    n->is_dir  = (uint8_t)is_dir;
    n->is_ref  = 0;
    n->size    = size;
    n->content = NULL;
    n->parent  = parent;
    n->children = NULL;
    n->next     = parent->children;
    parent->children = n;
    // FAT32 backing store
    n->backing       = 1;
    n->populated     = 0;
    n->first_cluster = first_clus;
    n->dirent_sector = (uint32_t)dirent_lba;
    n->dirent_index  = dirent_idx;
    n->lfn_count     = lfn_count;
    n->mnt           = &g_fat;
    // on-disk 8.3 short name of this dirent (collision checks)
    if (sfn) for (int i = 0; i < 11; i++) n->sfn[i] = sfn[i];
    else     for (int i = 0; i < 11; i++) n->sfn[i] = ' ';
    return n;
}

void fat32_populate_dir(struct fs_node* dir) {
    if (!dir || !dir->is_dir || dir->backing != 1 || dir->populated) return;
    dir->populated = 1;                    // set first: never re-enter

    uint32_t cluster = dir->first_cluster;
    uint32_t guard   = 0;
    char     lfn_buf[FAT_NAME_MAX + 1];
    size_t   lfn_len = 0;
    uint16_t lfn_entries = 0;
    uint8_t  lfn_csum = 0;          // checksum from the 0x40-marked entry
    int      lfn_csum_ok = 0;

    while (cluster >= 2 && cluster < 2 + g_fat.cluster_count &&
           guard++ < 100000u) {
        uint64_t clus_lba = fat_cluster_lba(cluster);
        for (uint8_t s = 0; s < g_fat.sec_per_clus; s++) {
            uint8_t sec[FAT_SECTOR];
            if (fat_rd_sectors(clus_lba + s, 1, sec) != 0) return;
            for (int i = 0; i < FAT_DIRENTS_PER_SECTOR; i++) {
                uint8_t* e = sec + i * 32;
                if (e[0] == 0x00) return;               // end of directory
                if (e[0] == 0xE5) {                     // deleted: reset LFN
                    lfn_len = 0; lfn_entries = 0; lfn_csum_ok = 0;
                    continue;
                }
                uint8_t attr = e[11];
                if (attr == FAT_ATTR_LFN) {             // long-name entry
                    uint8_t seq = e[0] & 0x1F;
                    if (e[0] & 0x40) {                  // logical first chunk
                        lfn_len = 0;
                        lfn_entries = seq;
                        lfn_csum = e[13];
                        lfn_csum_ok = 1;
                        memset((uint8_t*)lfn_buf, 0, sizeof(lfn_buf));
                    }
                    // The chunks arrive physically in reverse order
                    // (0x43, 0x42, 0x41) — buffer them per-chunk and
                    // stitch below, in ascending order, at the 8.3 hit.
                    if (seq >= 1 && seq <= 20) {
                        char part[14]; size_t plen = 0;
                        lfn_read_entry(e, part, &plen, sizeof(part));
                        // chunks: keep a tiny per-seq store
                        // (0.4 Beta keeps it simple: most names fit in
                        // 1-2 chunks; longer names fall back to 8.3)
                        if (lfn_len + plen < sizeof(lfn_buf)) {
                            // prepend logic handled by ordered stitch:
                            // store in seq order using insertion
                            // (chunks come 3,2,1 -> we insert each at
                            // its fixed offset 13*(seq-1))
                            size_t at = 13u * (seq - 1u);
                            if (at + plen < sizeof(lfn_buf)) {
                                for (size_t k = 0; k < plen; k++)
                                    lfn_buf[at + k] = part[k];
                                if (at + plen > lfn_len) lfn_len = at + plen;
                            }
                        }
                    }
                    continue;
                }
                if (attr & FAT_ATTR_VOLUME) {           // volume label
                    lfn_len = 0; lfn_entries = 0; lfn_csum_ok = 0;
                    continue;
                }
                // ---- 8.3 entry: finalize the node ----
                char name[FAT_NAME_MAX + 1];
                sfn_to_name(e, name, sizeof(name));
                int is_lfn_ok = 0;
                if (lfn_entries && lfn_len && lfn_csum_ok &&
                    lfn_csum == fat_lfn_checksum(e)) {
                    // checksum matches: the stitched long name is valid
                    lfn_buf[lfn_len] = '\0';
                    if (lfn_buf[0]) {
                        strncpy(name, lfn_buf, FAT_NAME_MAX);
                        name[FAT_NAME_MAX] = '\0';
                        is_lfn_ok = 1;
                    }
                }
                // skip "." and ".." hard links
                if (name[0] == '.' && (name[1] == '\0' ||
                                       (name[1] == '.' && name[2] == '\0'))) {
                    lfn_len = 0; lfn_entries = 0;
                    continue;
                }
                // FAT32 0.4 Beta: files > 63 chars keep their 8.3 name
                // (fs_node::name cap); noted in RELEASE notes.
                uint32_t fclus = ((uint32_t)e[20] << 16) | (uint32_t)e[21] |
                                 ((uint32_t)e[26]) | ((uint32_t)e[27] << 8);
                uint32_t fsize = (uint32_t)e[28] | ((uint32_t)e[29] << 8) |
                                 ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
                int is_dir_e = (attr & FAT_ATTR_DIR) ? 1 : 0;
                fat_new_node(dir, name, is_dir_e, fclus,
                             is_dir_e ? 0 : fsize,
                             clus_lba + s, (uint16_t)i,
                             is_lfn_ok ? lfn_entries : 0, e);
                lfn_len = 0; lfn_entries = 0; lfn_csum_ok = 0;
            }
        }
        cluster = fat_entry(cluster);
        if (cluster >= FAT32_EOC_MIN || cluster == FAT32_FREE) break;
    }
}

// ===================================================================
//  WHOLE-FILE READ (lazy content cache)
// ===================================================================

int fat32_read_whole(struct fs_node* file) {
    task_sched_lock();   /* Phase A: no preemption mid-storage-operation */
    if (!file || file->backing != 1 || file->is_dir) { task_sched_unlock(); return -1; }
    /* 0.4 Beta LEAK FIX: these early returns used to LEAK the sched lock
     * (schedule() bails while locked -> wake_scan never runs -> every
     * sleeping task, e.g. the nettask, starves -> the network dies).
     * Found via SCHEDLOCK=2 after "cp" into /mnt. */
    if (file->content) { task_sched_unlock(); return 0; }   // already cached
    if (file->size == 0) { task_sched_unlock(); return 0; } // empty: NULL fine
    if (file->first_cluster < 2) { task_sched_unlock(); return -2; }

    // 0.4 Beta write hardening: this read bypasses the sector cache — a
    // dirty line left by a FAILED earlier write would hand us stale
    // disk bytes. Flushing first is free and always safe.
    fat_cache_put();

    // Round the buffer up to a whole cluster so multi-sector reads
    // never overrun it (consumers only look at the first `size` bytes).
    uint32_t clus_bytes = g_fat.cluster_bytes;
    uint32_t nclust = (file->size + clus_bytes - 1) / clus_bytes;
    uint32_t bufsz  = nclust * clus_bytes;

    uint8_t* buf = fat_arena_alloc(bufsz);
    int heap = 0;
    if (!buf) {
        if (bufsz > 262144) {
            printf("FAT32: %u KB too large for cache (arena full)\n",
                   (unsigned)(bufsz / 1024));
            task_sched_unlock();
            return -3;
        }
        buf = (uint8_t*)malloc(bufsz);
        if (!buf) { task_sched_unlock(); return -3; }
        heap = 1;
    }
    memset(buf, 0, bufsz);

    // Walk the chain, coalescing CONTIGUOUS clusters into one ATA
    // transfer (up to 128 sectors): FAT files are usually mostly
    // contiguous, so this collapses thousands of 512-byte PIOs into
    // a handful of multi-sector reads.
    uint32_t cluster = file->first_cluster;
    uint32_t offset  = 0;
    uint32_t guard   = 0;
    while (cluster >= 2 && cluster < 2 + g_fat.cluster_count &&
           offset < file->size && guard++ < 1000000u) {
        // find the contiguous run starting at `cluster`
        uint32_t run = 1;
        while (run < nclust - offset / clus_bytes) {
            uint32_t next = fat_entry(cluster + run - 1);
            if (next != cluster + run) break;
            run++;
        }
        uint32_t sec_off = 0;
        uint32_t max_sec = (file->size - offset + 511) / 512;
        while (sec_off < run * g_fat.sec_per_clus) {
            uint32_t nsec = run * g_fat.sec_per_clus - sec_off;
            if (nsec > 128) nsec = 128;
            if (nsec > max_sec) nsec = max_sec;
            if (nsec == 0) break;
            uint64_t lba = fat_cluster_lba(cluster) + sec_off;
            if (fat_rd_sectors(lba, nsec, buf + offset + sec_off * 512) != 0) {
                if (heap) free(buf);
                task_sched_unlock();
                return -4;
            }
            sec_off += nsec;
        }
        offset += run * clus_bytes;
        cluster = fat_entry(cluster + run - 1);
        if (cluster >= FAT32_EOC_MIN || cluster == FAT32_FREE) break;
    }

    file->content = (char*)buf;
    file->is_ref  = heap ? 0 : 1;    // arena memory: skip kernel free()
    file->cont_owner = heap ? 0 : 2; /* 0.4 Beta FR-08: 2 = FAT arena (fat_arena_free) */
    task_sched_unlock();
    return 0;
    task_sched_unlock();
}

// ===================================================================
//  MOUNT COMMANDS + BOOT
// ===================================================================

static void fat_mount_log(const char* msg) {
    if (fat_boot_log) fat_boot_log(msg);
    else printf("%s\n", msg);
}

// Scan every ATA hard disk; mount the first FAT32 partition found
// (MBR types 0x0B / 0x0C, or any entry whose boot sector parses as
// FAT32; also handles "floppy-style" whole-disk FAT32 images with
// no partition table). mount_pt = RAMFS node of /mnt.
static int fat32_try_all(struct fs_node* mount_pt) {
    for (int slot = 0; slot < blk_count(); slot++) {
        if (!blk_is_disk(slot)) continue;

        struct mbr_partition parts[4];
        int n = mbr_read_partitions(slot, parts);
        if (n > 0) {
            for (int i = 0; i < 4; i++) {
                if (!parts[i].present) continue;
                if (fat32_do_mount(slot, parts[i].start_lba, mount_pt) == 0)
                    return 0;
            }
        } else {
            // no MBR signature: try a partitionless FAT32 volume
            if (fat32_do_mount(slot, 0, mount_pt) == 0) return 0;
        }
    }
    return -1;
}

int fat32_mount_cmd(void) {
    if (g_fat.mounted) {
        /* 0.4 Beta: a base volume lives at / , not /mnt */
        if (g_fat.base) printf("mount: already mounted as / (label '%s')\n",
                               g_fat.label);
        else            printf("mount: already mounted at /mnt (label '%s')\n",
                               g_fat.label);
        return 0;
    }
    struct fs_node* root = fs_get_root();
    struct fs_node* mnt  = fs_find_child(root, "mnt");
    if (!mnt) {
        if (fs_create_dir(root, "mnt") != 0) {
            printf("mount: cannot create /mnt\n");
            return -1;
        }
        mnt = fs_find_child(root, "mnt");
    }
    if (!mnt || !mnt->is_dir) {
        printf("mount: /mnt is not a directory\n");
        return -1;
    }
    if (fat32_try_all(mnt) != 0) {
        printf("mount: no FAT32 partition found on any ATA disk\n");
        return -1;
    }
    char szb[24];
    uint64_t freeb = (uint64_t)g_fat.free_clusters * g_fat.cluster_bytes;
    snprintf(szb, sizeof(szb), "%u.%u MB",
             (unsigned)(freeb / (1024u * 1024)),
             (unsigned)((freeb % (1024u * 1024)) * 10 / (1024u * 1024)));
    printf("FAT32: '%s' mounted at /mnt (%s free)\n",
           g_fat.label[0] ? g_fat.label : "(no label)", szb);
    return 0;
}

// ===================================================================
//  0.4 Beta — SLOT SELECTION + VOLUME LABEL (equinoxinstall wizard)
// ===================================================================

/* Read-only inspection of drive slot 0..3: drive present, FAT32
 * volume found, label, partition geometry, and whether this slot is
 * the active mount. Never mutates g_fat, so it is safe while another
 * volume is mounted. Returns 0 = drive present, -1 = empty slot. */
int fat32_slot_scan(int slot, struct fat32_slot_info* out) {
    if (!out) return -1;
    memset((uint8_t*)out, 0, sizeof(*out));
    struct blk_desc* d = blk_get(slot);
    if (!d || !blk_is_disk(slot)) return -1;
    out->present = 1;
    snprintf(out->model, sizeof(out->model), "%s", d->model);
    blk_format_size(d->total_sectors, out->size, sizeof(out->size));

    /* candidate volume starts: MBR partition entries, or LBA 0 for a
     * "floppy-style" whole-disk volume with no partition table. */
    struct mbr_partition parts[4];
    uint64_t cand[5];
    int nc = 0;
    int n = mbr_read_partitions(slot, parts);
    if (n > 0) {
        for (int i = 0; i < 4 && nc < 5; i++)
            if (parts[i].present) cand[nc++] = parts[i].start_lba;
    } else {
        cand[nc++] = 0;
    }

    for (int i = 0; i < nc; i++) {
        uint8_t bpb[FAT_SECTOR];
        if (fat32_probe(slot, cand[i], bpb) != 0) continue;
        out->has_fat32 = 1;
        out->part_lba  = cand[i];
        if (n > 0) {
            for (int k = 0; k < 4; k++)
                if (parts[k].present && parts[k].start_lba == cand[i]) {
                    out->part_sectors = parts[k].num_sectors;
                    break;
                }
        } else {
            out->part_sectors = d->total_sectors;
        }
        for (int k = 0; k < 11; k++) out->label[k] = bpb[71 + k];
        out->label[11] = '\0';
        for (int k = 10; k >= 0 && (out->label[k] == ' ' || out->label[k] == 0);
             k--) out->label[k] = 0;
        break;
    }
    if (out->part_sectors)
        blk_format_size(out->part_sectors, out->part_size, sizeof(out->part_size));

    if (g_fat.mounted && g_fat.blk_slot == slot) {
        out->active = 1;
        out->is_base = g_fat.base;
        if (g_fat.label[0])
            snprintf(out->label, sizeof(out->label), "%s", g_fat.label);
    }
    return 0;
}

/* Mount ONE drive slot at /mnt (per-slot partition scan). Refuses when
 * a base volume is mounted as / (that volume IS the running system);
 * a different plain volume at /mnt is unmounted first. 0 = success. */
int fat32_mount_slot(int slot) {
    if (g_fat.mounted) {
        if (g_fat.base) {
            printf("mount: base volume terpasang sebagai / — lepas dulu (umount)\n");
            return -2;
        }
        if (g_fat.blk_slot == slot) {
            printf("mount: slot %d sudah terpasang di /mnt\n", slot);
            return 0;
        }
        fat32_unmount_cmd();
        if (g_fat.mounted) return -3;            /* unmount refused */
    }

    struct fs_node* root = fs_get_root();
    struct fs_node* mnt  = fs_find_child(root, "mnt");
    if (!mnt) {
        if (fs_create_dir(root, "mnt") != 0) return -4;
        mnt = fs_find_child(root, "mnt");
    }
    if (!mnt || !mnt->is_dir) return -4;

    struct mbr_partition parts[4];
    int n = mbr_read_partitions(slot, parts);
    int ok = 0;
    if (n > 0) {
        for (int i = 0; i < 4 && !ok; i++)
            if (parts[i].present &&
                fat32_do_mount(slot, parts[i].start_lba, mnt) == 0) ok = 1;
    } else {
        ok = (fat32_do_mount(slot, 0, mnt) == 0);
    }
    if (!ok) {
        printf("mount: slot %d tidak berisi partisi FAT32\n", slot);
        return -5;
    }
    printf("FAT32: '%s' mounted at /mnt (slot %d)\n",
           g_fat.label[0] ? g_fat.label : "(no label)", slot);
    return 0;
}

/* Rewrite the volume label of the MOUNTED volume: BPB field at offset
 * 71 (11 bytes, space padded) plus the root-directory label entry when
 * one exists. The boot-time base check reads the BPB, so relabelling a
 * plain disk EQUINOXBASE turns it into a boot base on the next boot. */
int fat32_set_label(const char* label) {
    if (!g_fat.mounted) { printf("label: nothing mounted\n"); return -1; }
    if (!label || !label[0] || strlen(label) > 11) {
        printf("label: butuh 1..11 karakter\n");
        return -2;
    }
    char padded[11];
    int i;
    for (i = 0; i < 11; i++) padded[i] = ' ';
    for (i = 0; label[i] && i < 11; i++) padded[i] = label[i];

    uint8_t vbr[FAT_SECTOR];
    if (fat_rd_sectors(g_fat.part_lba, 1, vbr) != 0) return -3;
    for (i = 0; i < 11; i++) vbr[71 + i] = (uint8_t)padded[i];
    if (fat_wr_sectors(g_fat.part_lba, 1, vbr) != 0) return -4;

    /* matching volume-label entry in the root directory (if present) */
    uint8_t sec[FAT_SECTOR];
    uint64_t rlba = fat_cluster_lba(g_fat.root_clus);
    if (fat_rd_sectors(rlba, 1, sec) == 0) {
        for (int e = 0; e < FAT_DIRENTS_PER_SECTOR; e++) {
            uint8_t* de = sec + e * 32;
            if (de[0] == 0x00) break;
            if (de[0] == 0xE5) continue;
            if (de[11] == FAT_ATTR_VOLUME) {
                for (i = 0; i < 11; i++) de[i] = (uint8_t)padded[i];
                fat_wr_sectors(rlba, 1, sec);
                break;
            }
        }
    }

    for (i = 0; i < 11; i++) g_fat.label[i] = padded[i];
    g_fat.label[11] = '\0';
    for (i = 10; i >= 0 && (g_fat.label[i] == ' ' || g_fat.label[i] == 0);
         i--) g_fat.label[i] = 0;
    if (g_fat.cache_valid && g_fat.cache_lba == g_fat.part_lba)
        g_fat.cache_valid = 0;
    return 0;
}

static void fat_free_subtree(struct fs_node* n) {
    struct fs_node* c = n->children;
    while (c) {
        struct fs_node* nx = c->next;
        fat_free_subtree(c);
        c = nx;
    }
    if (n->content && !n->is_ref) free(n->content);
    free(n);
}

int fat32_unmount_cmd(void) {
    if (!g_fat.mounted) {
        printf("umount: nothing mounted\n");
        return -1;
    }
    /* 0.4 Beta: the base volume IS the root filesystem. Releasing it would
     * free every directory the shell is standing in (cwd, /equinox,
     * /test, ...) and leave the root node pointing at a dead mount. */
    if (g_fat.base) {
        printf("umount: refusing — EQUINOXBASE is the ROOT filesystem (/)\n");
        return -1;
    }
    fat_fsinfo_sync();                    // persist the free counters
    fat_cache_put();                      // flush a dirty sector line

    // Recursively drop the mirrored RAMFS tree under /mnt (heap caches
    // freed; arena caches (is_ref=1) abandoned by design).
    struct fs_node* mnt = g_fat.mount_pt;
    struct fs_node* c = mnt->children;
    while (c) {
        struct fs_node* nx = c->next;
        fat_free_subtree(c);
        c = nx;
    }
    int was_base = g_fat.base;
    mnt->children  = NULL;
    mnt->populated = 0;
    mnt->backing   = 0;                   // /mnt is a plain dir again
    mnt->mnt       = NULL;
    g_fat.mounted = 0;
    g_fat.base    = 0;                    // 0.4 Beta: base released too
    printf(was_base ? "umount: / released\n"
                    : "umount: /mnt released\n");
    return 0;
}

/* 0.4 Beta — does the mounted volume carry an equinox BASE? The BPB
 * label is 11 bytes, space-padded (fat32_do_mount() right-trims it),
 * so match `want` and accept NUL or trailing spaces after it. */
static int fat32_label_is(const char* want) {
    const char* s = g_fat.label;
    int i;
    for (i = 0; want[i]; i++) {
        if (s[i] != want[i]) return 0;
    }
    return s[i] == '\0' || s[i] == ' ';
}

/* 0.4 Beta — promote the freshly mounted volume from /mnt to ROOT.
 *
 * `root` and `mnt` are distinct RAMFS nodes: the volume was mounted on
 * /mnt (fat32_do_mount already turned THAT node into the FAT root), so
 * we move the FAT identity over to the root node and hand /mnt back to
 * RAMFS. Order matters — kernel.cpp mounts the volume BEFORE the GRUB
 * modules are installed, so root->children is still empty and no RAMFS
 * node can shadow a directory that exists on the disk. */
static void fat32_promote_to_root(struct fs_node* root, struct fs_node* mnt) {
    if (mnt && mnt != root) {
        /* anything mirrored under /mnt goes away with the mount */
        struct fs_node* c = mnt->children;
        while (c) {
            struct fs_node* nx = c->next;
            fat_free_subtree(c);
            c = nx;
        }
        mnt->children      = NULL;
        mnt->backing       = 0;
        mnt->populated     = 0;
        mnt->first_cluster = 0;
        mnt->size          = 0;
        mnt->mnt           = NULL;
        mnt->lfn_count     = 0;
        mnt->dirent_sector = 0;
        mnt->dirent_index  = 0;
    }

    root->backing       = 1;
    root->populated     = 0;   /* listing comes from the disk, lazily */
    root->first_cluster = g_fat.root_clus;
    root->size          = 0;
    root->mnt           = &g_fat;
    root->lfn_count     = 0;
    root->dirent_sector = 0;
    root->dirent_index  = 0;

    g_fat.mount_pt = root;
    g_fat.base     = 1;
}

void fat32_boot_init(uint32_t mem_upper_bytes) {
    fat_arena_init(mem_upper_bytes);
    blk_init();          // 0.4 Beta: ata_init() (slots 0..3) + ahci_init() (4..7)

    int ndisks = 0;
    for (int i = 0; i < blk_count(); i++)
        if (blk_is_disk(i)) ndisks++;

    if (ndisks == 0) {
        fat_mount_log("ATA: no hard disks found");
        return;
    }

    // One line per disk, exactly like 0.4 Beta printed the PATA ones —
    // an AHCI disk is announced as "SATA: ahci port N ...".
    for (int i = 0; i < blk_count(); i++) {
        struct blk_desc* d = blk_get(i);
        if (!d || !blk_is_disk(i)) continue;
        char szb[24];
        blk_format_size(d->total_sectors, szb, sizeof(szb));
        char line[96];
        const char* tag = (d->kind == BLK_AHCI) ? "SATA" : "ATA";
        snprintf(line, sizeof(line), "%s: %s \"%s\" (%s)",
                 tag, d->bus, d->model, szb);
        fat_mount_log(line);
    }

    struct fs_node* root = fs_get_root();
    struct fs_node* mnt  = fs_find_child(root, "mnt");
    if (!mnt) {
        fs_create_dir(root, "mnt");
        mnt = fs_find_child(root, "mnt");
    }
    if (!mnt) return;

    if (fat32_try_all(mnt) == 0) {
        char szb[24];
        uint64_t freeb = (uint64_t)g_fat.free_clusters * g_fat.cluster_bytes;
        snprintf(szb, sizeof(szb), "%u.%u MB",
                 (unsigned)(freeb / (1024u * 1024)),
                 (unsigned)((freeb % (1024u * 1024)) * 10 / (1024u * 1024)));
        char line[96];

        /* 0.4 Beta: label EQUINOXBASE = an equinox INSTALL BASE. Promote
         * the volume from /mnt to the ROOT filesystem so /user,
         * /equinox and /test resolve on the disk (write-through,
         * persistent); a plain disk keeps the old /mnt mount. */
        if (fat32_label_is("EQUINOXBASE")) {
            fat32_promote_to_root(root, mnt);
            snprintf(line, sizeof(line),
                     "FAT32: '%s' mounted as ROOT (/) — install base (%s free)",
                     g_fat.label, szb);
        } else {
            /* 0.4 Beta — `set -b PATH` merekam pivot base di system.ecf.
             * Diterapkan DI SINI, saat boot (bukan saat runtime: promosi
             * root butuh pohon masih kosong). Volume berlabel EQUINOXBASE
             * sudah masuk cabang atas, jadi jalur lama tak berubah. */
            int promoted = 0;
            const char* bp = ecf_file_get(ecf_active_path(0), "base.path");
            if (bp && strcmp(bp, "/mnt") == 0) {
                fat32_promote_to_root(root, mnt);
                promoted = 1;
            }
            if (promoted)
                snprintf(line, sizeof(line),
                         "FAT32: '%s' mounted as ROOT (/) — base.path (%s free)",
                         g_fat.label, szb);
            else
                snprintf(line, sizeof(line),
                         "FAT32: '%s' mounted at /mnt (%s free)",
                         g_fat.label[0] ? g_fat.label : "(no label)", szb);
        }
        fat_mount_log(line);
    } else {
        fat_mount_log("FAT32: no FAT32 partition found (disk not mounted)");
    }
}

/* 0.4 Beta: 1 when the volume became the ROOT filesystem (label
 * EQUINOXBASE). Used by the shell / equinoxinstall to report where
 * builds land. */
extern "C" int fat32_base_active(void) {
    return (g_fat.mounted && g_fat.base) ? 1 : 0;
}

// Hook for kernel.cpp: capture boot_log() so mount lines replay on VESA.
void fat32_set_boot_log(void (*fn)(const char*)) {
    fat_boot_log = fn;
}

// ===================================================================
//  DISKINFO
// ===================================================================
void fat32_diskinfo(void) {
    printf("== ATA drives ==\n");
    int any = 0;
    for (int i = 0; i < blk_count(); i++) {
        struct blk_desc* d = blk_get(i);
        if (!d) continue;
        any = 1;
        char szb[24];
        blk_format_size(d->total_sectors, szb, sizeof(szb));
        const char* kind;
        if (d->kind == BLK_ATAPI)     kind = "ATAPI (packet)";
        else if (d->kind == BLK_AHCI) kind = d->lba48 ? "SATA LBA48"
                                                     : "SATA LBA28";
        else                          kind = d->lba48 ? "PATA LBA48"
                                                     : "PATA LBA28";
        printf("  %s: %s  %s  \"%s\"  %s\n",
               d->name, d->bus, kind, d->model, szb);
    }
    if (!any) printf("  (none)\n");

    if (!g_fat.mounted) {
        printf("== FAT32: not mounted ==\n");
        return;
    }
    printf(g_fat.base ? "== FAT32 volume at / (install base) ==\n"
                      : "== FAT32 volume at /mnt ==\n");
    struct blk_desc* vd = blk_get(g_fat.blk_slot);
    printf("  label      : %s\n", g_fat.label[0] ? g_fat.label : "(none)");
    printf("  partitions : %s @ LBA %u\n",
           vd ? vd->name : "?", (unsigned)g_fat.part_lba);
    printf("  cluster    : %u sectors (%u KB)\n",
           g_fat.sec_per_clus, g_fat.cluster_bytes / 1024);
    printf("  FAT        : %u sectors x %u copies\n",
           g_fat.fat_secs, g_fat.num_fats);
    printf("  data       : %u clusters, %u free (%u%% free)\n",
           g_fat.cluster_count, g_fat.free_clusters,
           g_fat.cluster_count
               ? (unsigned)(g_fat.free_clusters * 100 / g_fat.cluster_count)
               : 0);
    printf("  mode       : %s\n",
           g_fat.writable ? "read-write (write-through)" : "read-only");
    printf("  cache      : %u hits / %u misses\n", g_fat.hits, g_fat.misses);
    printf("  arena      : %u KB used / %u KB\n",
           fat32_arena_used() / 1024, fat32_arena_total() / 1024);
}
