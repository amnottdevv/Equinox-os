// ============================================================
//  fs_fat32_write.cpp — FAT32 write path (v0.2, Phase B)
// ------------------------------------------------------------
//  All operations are WRITE-THROUGH: every call leaves the FAT,
//  the directory entries and the FSInfo sector consistent on
//  disk before returning. The RAMFS mirror is updated to match.
//
//    fat32_create_file  — new empty file (LFN + mangled 8.3)
//    fat32_write_file   — overwrite/extend/truncate + dirent update
//    fat32_create_dir   — new subdir (cluster + "." / "..")
//    fat32_delete       — unlink (0xE5 run + free chain)
// ============================================================

#include "fs_fat32_internal.h"
#include "header/fs_fat32.h"
#include "header/fs_ram.h"   /* v0.3 FR-08: fs_content_release / cont_owner */
#include "header/task.h"   // Phase A: sched-lock
#include "header/stdio.h"
#include "header/malloc.h"
#include "header/libstring.h"

// ===================================================================
//  NAME HELPERS
// ===================================================================

// Standard LFN checksum over the 11-byte short name.
uint8_t fat_lfn_checksum(const uint8_t sn[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sn[i]);
    return sum;
}

// Is `c` legal in the 8.3 uppercase charset?
static int sfn_char_ok(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return (c && strchr("#$%&'()-@^_`{}~!", c)) ? 1 : 0;
}

// Does the name need an LFN? (not representable as plain 8.3)
static int name_is_plain_sfn(const char* name) {
    int base = 0, ext = 0, dot = 0, i = 0;
    if (!name[0]) return 0;
    for (; name[i]; i++) {
        char c = name[i];
        if (c == '.') {
            if (dot) return 0;          // second dot -> LFN
            dot = 1;
            continue;
        }
        if (c >= 'a' && c <= 'z') return 0;      // lowercase -> LFN
        if (!sfn_char_ok(c)) return 0;           // special char -> LFN
        if (!dot) { base++; if (base > 8) return 0; }
        else      { ext++;  if (ext  > 3) return 0; }
    }
    if (base == 0) return 0;            // e.g. ".gitignore" -> LFN
    if (i > 12) return 0;
    return 1;
}

// Uppercase a byte for the 8.3 name.
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

// Build the 8.3 short name for `name`. Collisions (~1, ~2 ...) are
// resolved against the existing directory by the caller.
static void sfn_build(const char* name, int tail, uint8_t out[11]) {
    // split at the LAST dot
    const char* dot = NULL;
    for (const char* p = name; *p; p++)
        if (*p == '.') dot = p;
    const char* base = name;
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    const char* ext  = dot ? dot + 1 : "";
    size_t ext_len   = dot ? strlen(dot + 1) : 0;

    char b[9]; size_t bl = 0;
    // leave room for ~N when a numeric tail is needed
    size_t base_room = tail ? 6u : 8u;
    for (size_t i = 0; i < base_len && bl < base_room; i++)
        b[bl++] = sfn_char_ok(up(base[i])) ? up(base[i]) : '_';
    if (bl == 0) b[bl++] = '_';
    if (tail) {
        // pad to 6 then append ~N (classic numeric-tail scheme)
        while (bl < 6 && bl < sizeof(b)) b[bl++] = '_';
        b[bl++] = '~';
        b[bl++] = (char)('0' + (tail > 9 ? 9 : tail));
    }

    char e[4]; size_t el = 0;
    for (size_t i = 0; i < ext_len && el < 3; i++)
        e[el++] = sfn_char_ok(up(ext[i])) ? up(ext[i]) : '_';

    for (size_t i = 0; i < 8; i++) out[i] = (uint8_t)(i < bl ? b[i] : ' ');
    for (size_t i = 0; i < 3; i++) out[8 + i] = (uint8_t)(i < el ? e[i] : ' ');
}

void fat_sfn_from_name(const char* name, uint8_t out[11], int* needs_lfn) {
    *needs_lfn = !name_is_plain_sfn(name);
    sfn_build(name, 0, out);
}

// v0.2 write hardening: compare a candidate 8.3 short name against
// an EXISTING directory entry's real on-disk short name. The mirror
// node caches the raw 11 bytes at populate/create time, so this is an
// exact 11-byte comparison — no more guessing from the visible long
// name (which missed collisions like "My Document.txt" [MYDOC~1 TXT]
// vs a new "mydoc~1.txt").
static int sfn_equal(const uint8_t a[11], const uint8_t b[11]) {
    for (int i = 0; i < 11; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

// Does any child of `dir` already use this exact on-disk 8.3 name?
static int sfn_taken(struct fs_node* dir, const uint8_t sfn[11]) {
    for (struct fs_node* c = dir->children; c; c = c->next)
        if (c->backing == 1 && sfn_equal(c->sfn, sfn)) return 1;
    return 0;
}

// ===================================================================
//  DIRECTORY SLOT MANAGEMENT
// ===================================================================
struct dir_slot {
    uint64_t lba;        // absolute LBA of the sector holding the slot
    uint16_t index;      // entry index inside that sector (0-15)
};

// Walk the directory chain of `dir` looking for a free run of
// `need` consecutive dirent slots (0x00 / 0xE5). If none exists and
// the directory can grow, a new cluster is appended (zeroed) and its
// first slots are used. Returns 0 on success with the run start, and
// fills run_lba[need] with each slot's position when `run_lba` != NULL.
struct grow_ctx {
    struct fs_node* dir;
    uint32_t last_cluster;    // chain tail (for appending)
    int      chain_len;
};

static int dir_find_free_run(struct fs_node* dir, int need,
                             struct dir_slot* slots, struct grow_ctx* g) {
    // chain walk state
    uint32_t cluster = dir->first_cluster;
    uint32_t prev = 0;
    int run = 0;

    // grow_ctx defaults
    g->dir = dir;
    g->last_cluster = 0;
    g->chain_len = 0;

    if (cluster < 2) return -1;    // a dir always has >= 1 cluster

    while (cluster >= 2 && cluster < 2 + g_fat.cluster_count &&
           g->chain_len < 100000) {
        g->chain_len++;
        g->last_cluster = cluster;
        uint64_t clus_lba = fat_cluster_lba(cluster);
        for (uint8_t s = 0; s < g_fat.sec_per_clus; s++) {
            uint8_t* sec = fat_cache_get(clus_lba + s);
            if (!sec) return -2;
            for (int i = 0; i < FAT_DIRENTS_PER_SECTOR; i++) {
                uint8_t first = sec[i * 32];
                if (first == 0x00 || first == 0xE5) {
                    if (run < need) {
                        slots[run].lba   = clus_lba + s;
                        slots[run].index = (uint16_t)i;
                    }
                    run++;
                    if (run == need) return 0;   // found the whole run
                } else {
                    run = 0;
                }
            }
        }
        prev = cluster;
        cluster = fat_entry(cluster);
        if (cluster >= FAT32_EOC_MIN || cluster == FAT32_FREE) break;
    }
    (void)prev;

    // No run inside the existing clusters: grow the directory by one
    // cluster (directories CAN grow in FAT32, root included).
    uint32_t nc = fat_alloc_cluster();
    if (nc == 0) return -3;
    if (fat_entry_set(g->last_cluster, nc) != 0) return -4;
    g->chain_len++;

    // zero the new cluster (flush EACH sector: fat_cache_zero swaps
    // the cache line, only the last one would survive a single put)
    uint64_t nlba = fat_cluster_lba(nc);
    for (uint8_t s = 0; s < g_fat.sec_per_clus; s++) {
        fat_cache_zero(nlba + s);
        fat_cache_put();
    }

    // v0.2 write hardening (CRITICAL): the run CONTINUES into the
    // fresh cluster. The trailing free slots of the old chain
    // (slots[0..run-1], recorded by the scan above) stay part of it;
    // only the missing (need - run) slots come from the new cluster.
    //
    // The old code ignored the trailing run and used only new-cluster
    // slots. When the trailing free slot was 0x00 (never used), it
    // stayed 0x00 IN THE MIDDLE of the chain — the FAT spec's
    // end-of-directory marker — so every reader (Equinox after a
    // reboot, Windows, mtools, fsck) stopped there and everything
    // created later was invisible. Found by the reboot-persistence
    // stress test: 140 files created in one session, only the ones
    // before the first directory growth visible after reboot.
    for (int i = run; i < need; i++) {
        slots[i].lba   = nlba;
        slots[i].index = (uint16_t)(i - run);
    }
    return 0;
}

// ===================================================================
//  DIRENT WRITE HELPERS
// ===================================================================

// Fill a raw 32-byte dirent.
static void dirent_fill(uint8_t* e, const uint8_t sfn[11], uint8_t attr,
                        uint32_t first_clus, uint32_t size,
                        struct dos_datetime dt) {
    memset(e, 0, 32);
    for (int i = 0; i < 11; i++) e[i] = sfn[i];
    e[11] = attr;
    e[12] = 0;                          // NTRes
    e[13] = 0;                          // create-time 10 ms
    e[14] = (uint8_t)dt.time;  e[15] = (uint8_t)(dt.time >> 8);   // crtTime
    e[16] = (uint8_t)dt.date;  e[17] = (uint8_t)(dt.date >> 8);   // crtDate
    e[18] = (uint8_t)dt.date;  e[19] = (uint8_t)(dt.date >> 8);   // lastAcc
    e[20] = (uint8_t)(first_clus >> 16); e[21] = (uint8_t)(first_clus >> 24);
    e[22] = (uint8_t)dt.time;  e[23] = (uint8_t)(dt.time >> 8);   // wrtTime
    e[24] = (uint8_t)dt.date;  e[25] = (uint8_t)(dt.date >> 8);   // wrtDate
    e[26] = (uint8_t)first_clus; e[27] = (uint8_t)(first_clus >> 8);
    e[28] = (uint8_t)size; e[29] = (uint8_t)(size >> 8);
    e[30] = (uint8_t)(size >> 16); e[31] = (uint8_t)(size >> 24);
}

// Build one LFN entry (13 UTF-16 chars starting at name[pos]).
// v0.2 write hardening: after the name's NUL terminator the FAT spec
// requires 0x0000 (the terminator itself) followed by 0xFFFF padding
// for the rest of the entry. The old code kept READING name[] past
// the NUL — stack/heap garbage ended up on disk in the padding and
// strict tools (chkdsk, fsck.vfat) flagged the volume.
static void lfn_entry_fill(uint8_t* e, const char* name, size_t pos,
                           uint8_t seq, uint8_t csum) {
    memset(e, 0xFF, 32);                // spec padding everywhere
    e[0]  = seq;
    e[11] = FAT_ATTR_LFN;
    e[12] = 0;                          // type (always 0)
    e[13] = csum;
    static const int offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24,
                                  28, 30 };
    int terminated = 0;
    for (int i = 0; i < 13; i++) {
        uint16_t u;
        if (terminated) {
            u = 0xFFFF;                 // padding after the terminator
        } else {
            char c = name[pos + i];     // read stops AT the NUL, never past
            if (c == '\0') {
                u = 0x0000;             // the one and only terminator
                terminated = 1;
            } else if ((uint8_t)c < 0x80) {
                u = (uint8_t)c;         // ASCII: high byte 0
            } else {
                u = 0x003F;             // '?'
            }
        }
        e[offs[i]]     = (uint8_t)(u & 0xFF);
        e[offs[i] + 1] = (uint8_t)(u >> 8);
    }
}

// Write the whole dirent run (LFN entries in reverse + the 8.3 entry)
// at the given slots. slots[0..need-1] are used in ascending order:
// physically [LFN n][LFN n-1]...[LFN 1][8.3].
static int dirent_write_run(struct dir_slot* slots, int need,
                            const char* name, const uint8_t sfn[11],
                            uint8_t attr, uint32_t first_clus,
                            uint32_t size, struct dos_datetime dt) {
    int lfn_count = need - 1;
    uint8_t csum = fat_lfn_checksum(sfn);

    // LFN entries: logical chunk 1 is the LAST slot before the 8.3
    for (int c = 1; c <= lfn_count; c++) {
        struct dir_slot* sl = &slots[lfn_count - c];   // reverse order
        uint8_t* sec = fat_cache_get(sl->lba);
        if (!sec) return -1;
        uint8_t seq = (uint8_t)(c | (c == lfn_count ? 0x40 : 0));
        lfn_entry_fill(sec + sl->index * 32, name,
                       (size_t)(c - 1) * 13u, seq, csum);
        g_fat.cache_dirty = 1;
    }
    // 8.3 entry in the final slot
    {
        struct dir_slot* sl = &slots[need - 1];
        uint8_t* sec = fat_cache_get(sl->lba);
        if (!sec) return -2;
        dirent_fill(sec + sl->index * 32, sfn, attr, first_clus, size, dt);
        g_fat.cache_dirty = 1;
    }
    fat_cache_put();
    return 0;
}

// Update an existing 8.3 dirent (size + first cluster + timestamps).
static int dirent_update(struct fs_node* node) {
    uint8_t* sec = fat_cache_get(node->dirent_sector);
    if (!sec) return -1;
    uint8_t* e = sec + node->dirent_index * 32;
    struct dos_datetime dt = fat_rtc_now();
    e[20] = (uint8_t)(node->first_cluster >> 16);
    e[21] = (uint8_t)(node->first_cluster >> 24);
    e[22] = (uint8_t)dt.time;  e[23] = (uint8_t)(dt.time >> 8);
    e[24] = (uint8_t)dt.date;  e[25] = (uint8_t)(dt.date >> 8);
    e[26] = (uint8_t)node->first_cluster;
    e[27] = (uint8_t)(node->first_cluster >> 8);
    e[28] = (uint8_t)node->size; e[29] = (uint8_t)(node->size >> 8);
    e[30] = (uint8_t)(node->size >> 16); e[31] = (uint8_t)(node->size >> 24);
    g_fat.cache_dirty = 1;
    fat_cache_put();
    return 0;
}

// ===================================================================
//  PUBLIC WRITE OPERATIONS
// ===================================================================

int fat32_create_file(struct fs_node* dir, const char* name) {
    task_sched_lock();   /* Phase A: no preemption mid-storage-operation */
    if (!dir || !dir->is_dir || dir->backing != 1) { task_sched_unlock(); return -1; }
    if (!name || !name[0]) { task_sched_unlock(); return -2; }
    if (strlen(name) > FAT_NAME_MAX) { task_sched_unlock(); return -3; }
    if (!g_fat.writable) { task_sched_unlock(); return -4; }
    fat32_populate_dir(dir);
    if (fs_find_child(dir, name)) { task_sched_unlock(); return -5; }

    // short name + LFN decision
    uint8_t sfn[11];
    int needs_lfn = !name_is_plain_sfn(name);
    sfn_build(name, 0, sfn);

    // v0.2 write hardening: collision checks run against the REAL
    // on-disk 8.3 names (node->sfn), never the visible long names.
    //   plain 8.3 name  -> an exact short-name match is a duplicate
    //   LFN name        -> re-mangle with the classic ~1..~9 numeric
    //                       tails until the short name is free
    if (!needs_lfn) {
        if (sfn_taken(dir, sfn)) { task_sched_unlock(); return -5; }
    } else {
        if (sfn_taken(dir, sfn)) {
            int found = 0;
            for (int t = 1; t <= 9; t++) {
                uint8_t try11[11];
                sfn_build(name, t, try11);
                if (!sfn_taken(dir, try11)) {
                    for (int i = 0; i < 11; i++) sfn[i] = try11[i];
                    found = 1;
                    break;
                }
            }
            // honest failure instead of a duplicate short name on disk
            if (!found) {
                printf("FAT32: no free 8.3 alias for '%s' (~1..~9 taken)\n",
                       name);
                task_sched_unlock();
                return -10;
            }
        }
    }

    int lfn_n = needs_lfn ? (int)((strlen(name) + 12) / 13) : 0;
    int need  = 1 + lfn_n;
    struct dir_slot slots[24];
    if (need > (int)(sizeof(slots) / sizeof(slots[0]))) return -6;  // >63 ch
    struct grow_ctx g;
    if (dir_find_free_run(dir, need, slots, &g) != 0) {
        printf("FAT32: directory full / grow failed\n");
        task_sched_unlock();
        return -7;
    }

    struct dos_datetime dt = fat_rtc_now();
    if (dirent_write_run(slots, need, name, sfn, FAT_ATTR_ARCHIVE,
                         0, 0, dt) != 0) { task_sched_unlock(); return -8; }

    // RAM mirror node: position = the 8.3 slot
    struct fs_node* n = fat_new_node(dir, name, 0, 0, 0,
                                     slots[need - 1].lba,
                                     slots[need - 1].index,
                                     (uint16_t)lfn_n, sfn);
    if (!n) { task_sched_unlock(); return -9; }
    fat_fsinfo_sync();                  // dir may have grown (1 cluster)
    task_sched_unlock();
    return 0;
    task_sched_unlock();
}

int fat32_create_dir(struct fs_node* dir, const char* name) {
    task_sched_lock();   /* Phase A: no preemption mid-storage-operation */
    if (!dir || !dir->is_dir || dir->backing != 1) { task_sched_unlock(); return -1; }
    if (!name || !name[0]) { task_sched_unlock(); return -2; }
    if (strlen(name) > FAT_NAME_MAX) { task_sched_unlock(); return -3; }
    if (!g_fat.writable) { task_sched_unlock(); return -4; }
    fat32_populate_dir(dir);
    if (fs_find_child(dir, name)) { task_sched_unlock(); return -5; }

    uint8_t sfn[11];
    int needs_lfn = !name_is_plain_sfn(name);
    sfn_build(name, 0, sfn);

    // v0.2 write hardening: same real-8.3 collision policy as files
    if (!needs_lfn) {
        if (sfn_taken(dir, sfn)) { task_sched_unlock(); return -5; }
    } else if (sfn_taken(dir, sfn)) {
        int found = 0;
        for (int t = 1; t <= 9; t++) {
            uint8_t try11[11];
            sfn_build(name, t, try11);
            if (!sfn_taken(dir, try11)) {
                for (int i = 0; i < 11; i++) sfn[i] = try11[i];
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("FAT32: no free 8.3 alias for '%s' (~1..~9 taken)\n",
                   name);
            task_sched_unlock();
            return -10;
        }
    }

    int lfn_n = needs_lfn ? (int)((strlen(name) + 12) / 13) : 0;
    int need  = 1 + lfn_n;
    struct dir_slot slots[24];
    if (need > (int)(sizeof(slots) / sizeof(slots[0]))) { task_sched_unlock(); return -6; }
    struct grow_ctx g;
    if (dir_find_free_run(dir, need, slots, &g) != 0) { task_sched_unlock(); return -7; }

    // allocate + zero the directory cluster (flush each sector)
    uint32_t nc = fat_alloc_cluster();
    if (nc == 0) { task_sched_unlock(); return -8; }
    uint64_t nlba = fat_cluster_lba(nc);
    for (uint8_t s = 0; s < g_fat.sec_per_clus; s++) {
        fat_cache_zero(nlba + s);
        fat_cache_put();
    }

    // "." and ".." entries inside the new cluster (first sector)
    struct dos_datetime dt = fat_rtc_now();
    {
        uint8_t* sec = fat_cache_get(nlba);
        if (!sec) { task_sched_unlock(); return -9; }
        uint8_t dot[11]   = { '.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ' };
        uint8_t dotdot[11]= { '.','.',' ',' ',' ',' ',' ',' ',' ',' ',' ' };
        dirent_fill(sec + 0, dot, FAT_ATTR_DIR, nc, 0, dt);
        // ".." points at the parent cluster (0 when the parent is root)
        dirent_fill(sec + 32, dotdot, FAT_ATTR_DIR,
                    (dir->first_cluster == g_fat.root_clus)
                        ? 0 : dir->first_cluster, 0, dt);
        g_fat.cache_dirty = 1;
        fat_cache_put();
    }

    if (dirent_write_run(slots, need, name, sfn, FAT_ATTR_DIR, nc, 0,
                         dt) != 0) return -10;

    struct fs_node* n = fat_new_node(dir, name, 1, nc, 0,
                                     slots[need - 1].lba,
                                     slots[need - 1].index,
                                     (uint16_t)lfn_n, sfn);
    if (!n) { task_sched_unlock(); return -11; }
    fat_fsinfo_sync();                  // new dir cluster + maybe grow
    task_sched_unlock();
    return 0;
    task_sched_unlock();
}

int fat32_write_file(struct fs_node* file, const uint8_t* data,
                     uint32_t len) {
    task_sched_lock();   /* Phase A: no preemption mid-storage-operation */
    if (!file || file->backing != 1 || file->is_dir) { task_sched_unlock(); return -1; }
    if (!g_fat.writable) { task_sched_unlock(); return -2; }
    if (len && !data) { task_sched_unlock(); return -3; }

    uint32_t cb       = g_fat.cluster_bytes;
    uint32_t need_cl  = (len + cb - 1) / cb;
    uint32_t old_size = file->size;

    // ---- adjust the cluster chain --------------------------------
    // v0.2 write hardening: every failure path below leaves the
    // volume in a state the dirent still describes correctly:
    //   new file   -> chain freed again on failure
    //   extend     -> the ADDED chain unlinked + freed on failure
    //   shrink     -> the tail is freed only AFTER the dirent was
    //                 updated (an early failure just leaves a few
    //                 allocated-but-unused clusters: spec-legal)
    //   truncate 0 -> dirent updated first, chain freed after
    uint32_t first = file->first_cluster;
    uint32_t added = 0;                  // first cluster of an extension
    uint32_t shrink_keep = 0, shrink_tail = 0;   // deferred shrink
    if (len == 0) {
        // dirent first (size 0, cluster 0), then free the old chain
        file->size = 0;
        file->first_cluster = 0;
        if (dirent_update(file) != 0) {
            file->size = old_size;               // restore the mirror
            file->first_cluster = first;
            task_sched_unlock();
            return -8;
        }
        if (first >= 2) fat_free_chain(first);
        goto mirror_refresh;
    } else if (first < 2) {
        first = fat_alloc_chain(need_cl);
        if (first == 0) {
            printf("FAT32: volume full\n");
            task_sched_unlock();
            return -4;
        }
        added = first;                          // whole chain is new
    } else {
        // walk + extend / shrink (c ends at the chain tail cluster)
        uint32_t have = 1, c = first;
        while (have < need_cl) {
            uint32_t next = fat_entry(c);
            if (next >= FAT32_EOC_MIN || next == FAT32_FREE) break;
            c = next; have++;
        }
        if (have < need_cl) {
            // extend: allocate the missing clusters (single call ->
            // one contiguous chain, linked to the tail)
            uint32_t add = fat_alloc_chain(need_cl - have);
            if (add == 0) {
                printf("FAT32: volume full\n");
                task_sched_unlock();
                return -4;
            }
            fat_entry_set(c, add);
            added = add;                        // remember for rollback
        } else if (have > need_cl) {
            // shrink: locate the cut point, DEFER the free until the
            // dirent is consistent with the smaller size
            uint32_t keep = first;
            for (uint32_t i = 1; i < need_cl; i++) keep = fat_entry(keep);
            shrink_keep = keep;
            shrink_tail = fat_entry(keep);
        }
    }

    // ---- write the data -------------------------------------------
    // v0.2 write hardening: SECTOR-granular writes through a 512-byte
    // bounce buffer. The old code bounced a whole cluster through a
    // 16 KB stack array: it ate a quarter of the 64 KB kernel stack
    // in one frame and silently refused to write any volume with
    // clusters larger than 16 KB (32/64 KB clusters are the Windows
    // default for big volumes). The tail beyond EOF inside the last
    // sector is zero-filled.
    if (len) {
        uint32_t offset = 0;
        uint32_t c = first;
        uint32_t guard = 0;
        while (offset < len && c >= 2 && c < 2 + g_fat.cluster_count &&
               guard++ < 1000000u) {
            uint64_t clba = fat_cluster_lba(c);
            uint32_t in_off = 0;               // offset inside the cluster
            while (in_off < cb && offset < len) {
                uint8_t bounce[FAT_SECTOR];
                memset(bounce, 0, sizeof(bounce));
                uint32_t chunk = len - offset;
                if (chunk > FAT_SECTOR) chunk = FAT_SECTOR;
                for (uint32_t i = 0; i < chunk; i++)
                    bounce[i] = data[offset + i];
                if (fat_wr_sectors(clba + in_off / FAT_SECTOR, 1,
                                   bounce) != 0)
                    goto write_fail;
                offset += chunk;
                in_off  += FAT_SECTOR;
            }
            c = fat_entry(c);
        }
        if (offset < len) goto write_fail;      // chain broken

        // ---- dirent + deferred shrink -------------------------------
        file->first_cluster = first;
        file->size = len;
        if (dirent_update(file) != 0) {
            file->size = old_size;               // mirror back to disk state
            file->first_cluster =
                (old_size == 0) ? 0 : file->first_cluster;
            goto write_fail;
        }
        if (shrink_tail) {
            fat_entry_set(shrink_keep, FAT32_EOC_MIN);
            fat_free_chain(shrink_tail);
        }
    }

mirror_refresh:
    // ---- RAM mirror -----------------------------------------------
    // refresh the RAM cache: point at a COPY of the new data (the
    // caller's buffer is often stack memory). Arena first (cont_owner 2,
    // v0.3: reclaimable via fat_arena_free), heap as fallback for small
    // files; when neither is available the content stays NULL and
    // re-reads lazily.
    //
    // v0.3 FR-01 (UAF fix): `data` may BE the old file->content (the
    // flush-from-cache path used by SYS_CLOSE). Fill the NEW cache
    // first and release the old buffer only afterwards.
    {
        char*    old_content = file->content;
        uint8_t  old_owner   = file->cont_owner;
        uint8_t  old_ref     = file->is_ref;
        file->content = NULL;
        file->is_ref  = 0;
        file->cont_owner = 0;
        if (len > 0) {
            uint8_t* cache = fat_arena_alloc_public(len);
            if (cache) {
                for (uint32_t i = 0; i < len; i++) cache[i] = data[i];
                file->content = (char*)cache;
                file->is_ref  = 1;
                file->cont_owner = 2;
            } else if (len <= 262144) {
                file->content = (char*)malloc(len);
                if (file->content)
                    for (uint32_t i = 0; i < len; i++)
                        file->content[i] = (char)data[i];
            }
        }
        /* release the OLD cache (owner-aware) AFTER the copy */
        if (old_content) {
            if (!old_ref && old_owner == 0) {
                free(old_content);
            } else if (old_ref && old_owner == 2) {
                fat_arena_free(old_content);
            }
        }
    }
    fat_fsinfo_sync();                  // one FSInfo write per operation
    task_sched_unlock();
    return 0;

write_fail:
    // roll the FAT back to what the dirent still describes
    if (added) {
        if (added != first) {
            // extension: unlink the added chain from the old tail,
            // then free it. The original clusters are untouched.
            uint32_t c = first;
            uint32_t guard = 0;
            while (guard++ < 1000000u) {
                uint32_t next = fat_entry(c);
                if (next >= FAT32_EOC_MIN || next == FAT32_FREE) break;
                if (next == added) { fat_entry_set(c, FAT32_EOC_MIN); break; }
                c = next;
            }
        }
        fat_free_chain(added);
        if (added == first) first = 0;   // brand-new chain, all gone
    }
    // drop any stale cache so the next read reflects the dirent truth
    // (v0.3 FR-08: owner-aware — arena caches are reclaimed too)
    if (file->content) {
        uint8_t own = file->cont_owner;
        uint8_t ref = file->is_ref;
        char*   c   = file->content;
        file->content = NULL;
        file->is_ref  = 0;
        file->cont_owner = 0;
        if (!ref && own == 0) free(c);
        else if (ref && own == 2) fat_arena_free(c);
    }
    file->size = old_size;
    file->first_cluster = (old_size == 0) ? 0 : first;
    // NOTE: when added == first (new file) the dirent still says
    // size 0 / cluster 0, so first_cluster must go back to 0 — the
    // assignment above handles old_size == 0; for old_size > 0 with
    // added == first the chain was fully freed but the dirent still
    // references it — impossible in practice (that path requires
    // first < 2, i.e. old_size == 0).
    printf("FAT32: write failed, chain rolled back\n");
    fat_fsinfo_sync();
    task_sched_unlock();
    return -6;
    task_sched_unlock();
}

int fat32_delete(struct fs_node* node) {
    task_sched_lock();   /* Phase A: no preemption mid-storage-operation */
    if (!node || node->backing != 1 || !node->parent) { task_sched_unlock(); return -1; }
    if (!g_fat.writable) { task_sched_unlock(); return -2; }
    if (node->is_dir) {
        fat32_populate_dir(node);
        // v0.2 write hardening: -4 = "directory not empty", the SAME
        // code the shell (rm/rmdir) and fs_delete_node already use —
        // the old -3 printed a second, differently-worded message on
        // top of the shell's "rm: failed".
        if (node->children) { task_sched_unlock(); return -4; }
    }

    // mark the dirent run 0xE5: LFN entries (same or earlier sectors,
    // contiguous backwards) then the 8.3 entry
    uint32_t total = 1 + node->lfn_count;
    // slots walk backwards from the 8.3 entry
    uint64_t lba  = node->dirent_sector;
    int      idx  = node->dirent_index;
    for (uint32_t k = 0; k < total; k++) {
        uint8_t* sec = fat_cache_get(lba);
        if (!sec) { task_sched_unlock(); return -4; }
        uint8_t* e = sec + idx * 32;
        // sanity: the 8.3 slot must not already be free
        if (k == 0 && (e[0] == 0xE5 || e[0] == 0x00)) { task_sched_unlock(); return -5; }
        e[0] = 0xE5;
        g_fat.cache_dirty = 1;
        // step back one entry, crossing sector boundaries backwards
        if (idx == 0) {
            if (k + 1 >= total) break;
            // v0.2 write hardening: LFN runs DO cross cluster
            // boundaries now (dir_find_free_run continues the run
            // into a grown cluster), so the backwards walk must follow
            // the directory CHAIN: find the cluster preceding the one
            // holding `lba` and continue at its last slot.
            uint32_t cur_clus =
                2 + (uint32_t)((lba - g_fat.part_lba - g_fat.data_start) /
                               g_fat.sec_per_clus);
            uint32_t pc = node->parent->first_cluster;
            uint32_t prev_clus = 0, guard2 = 0;
            while (pc >= 2 && pc < 2 + g_fat.cluster_count &&
                   guard2++ < 1000000u) {
                if (pc == cur_clus) break;
                prev_clus = pc;
                pc = fat_entry(pc);
            }
            if (prev_clus == 0) break;      // first cluster: nothing before
            lba = fat_cluster_lba(prev_clus) + g_fat.sec_per_clus - 1;
            idx = FAT_DIRENTS_PER_SECTOR - 1;
        } else {
            idx--;
        }
    }
    fat_cache_put();

    // free the data chain
    if (node->first_cluster >= 2)
        fat_free_chain(node->first_cluster);

    // unlink the RAM mirror node (v0.3 FR-08: ALL caches are released
    // owner-aware — the arena is a free-list now, so arena caches are
    // reclaimed instead of abandoned)
    struct fs_node* parent = node->parent;
    struct fs_node* prev = NULL;
    struct fs_node* c = parent->children;
    while (c) {
        if (c == node) {
            if (prev) prev->next = c->next;
            else parent->children = c->next;
            break;
        }
        prev = c;
        c = c->next;
    }
    fs_content_release(node);
    free(node);
    fat_fsinfo_sync();                  // data chain was freed
    task_sched_unlock();
    return 0;
    task_sched_unlock();
}

// fat_arena_alloc_public() lives in fs_fat32.cpp (arena owner).
