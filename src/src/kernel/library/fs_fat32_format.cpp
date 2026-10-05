// ============================================================
//  fs_fat32_format.cpp — in-OS FAT32 formatter (0.4 Beta installer)
// ------------------------------------------------------------
//  fat32_mkfs(slot, label) — 0.4 Beta WHOLE-DISK FAT32 (TERBUKTI):
//  satu-satunya layout yang GRUB-core-minimal mampu di-mount reliably
//  (scripts/bootproof.sh: boot kernel + shell dari disk, berulang).
//
//    LBA 0        VBR/BPB (reserved 2048) + stub int 18h @0x5A
//                 (BIOS jatuh anggun ke CD sebelum install-boot)
//    LBA 1..291   [install-boot] GRUB core.img
//    LBA 2047     FSInfo (di-update FAT layer — di luar jangkauan core)
//    LBA 2048     FAT #1, FAT #2, lalu data area (root = cluster 2)
//
//  Deteksi driver: mbr_read_partitions -> 0 entri -> kandidat LBA 0
//  ("floppy-style" whole-disk, jalur fat32_do_mount yang teruji).
//  Qfs -install-boot menulis MBR = boot.img + BPB volume + PT-area
//  di-nol-kan + kernel_sector=1.
//  DESTRUCTIVE — caller wajib konfirmasi. Tolak volume terpasang.
// ============================================================

#include "fs_fat32_internal.h"
#include "header/fs_fat32.h"
#include "header/blk.h"
#include "header/stdio.h"
#include "header/libstring.h"

#define MKFS_RESERVED  2048u   /* 0.4 Beta: whole-disk, rumah core.img */
#define MKFS_FATS      2u
#define MKFS_FSINFO    2047u   /* ujung reserved — jauh dari core */

static void mkfs_w16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void mkfs_w32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* One sector, write-through the block layer. */
static int mkfs_wr1(int slot, uint64_t lba, const uint8_t* sec) {
    return blk_write(slot, lba, 1, sec) == 0 ? 0 : -1;
}

/* Zero `nsec` sectors at `lba`, in 8-sector chunks — one FLUSH CACHE
 * per chunk instead of one per sector (a FAT copy can be thousands of
 * sectors, and every flush is an fsync of the backing file). */
static int mkfs_zeroes(int slot, uint64_t lba, uint64_t nsec) {
    static uint8_t z[8 * FAT_SECTOR];      /* BSS: starts zeroed, never written */
    while (nsec) {
        uint32_t n = nsec > 8 ? 8 : (uint32_t)nsec;
        if (blk_write(slot, lba, n, z) != 0) return -1;
        lba += n;
        nsec -= n;
    }
    return 0;
}

int fat32_mkfs(int slot, const char* label) {
    if (!blk_is_disk(slot)) return -1;

    /* never format the volume the system is running on */
    struct fat32_mount* m = fat32_get_mount();
    if (m && m->blk_slot == slot) return -2;

    uint64_t total = blk_get(slot)->total_sectors;
    if (total <= (uint64_t)MKFS_RESERVED + 8192) return -3;
    uint64_t part_secs = total;      /* 0.4 Beta: volume = seluruh disk */

    /* Geometry: 512-byte clusters (keeps small volumes a real FAT32
     * for mtools, >= 65525 clusters), doubling only when the cluster
     * count would exceed the 28-bit FAT32 limit. The FAT size and the
     * cluster count depend on each other — iterate to a fixed point.
     *
     * 0.4 Beta (B — optimasi salin): volume BESAR juga dinaikkan
     * cluster-nya (maksimal 131072 cluster, minimal 65536 = tetap
     * FAT32 valid). Tiap cluster yang dialokasikan butuh 2 penulihan
     * entri FAT (FAT#1 + FAT#2 lewat cache sektor-tunggal yang
     * bergantian -> masing-masing menyimpan + FLUSH CACHE): pada
     * 512 B, file 4 MB = 8192 cluster = ~16 ribu flush hanya untuk
     * alokasi; pada 4 KB tinggal 1024 cluster. Data sektor tetap
     * ditulis batch 16 sektor (fs_fat32_write.cpp). Volume <= 128 MB
     * tidak berubah (tetap 512 B) — tidak ada tes yang bergeser. */
#define MKFS_CL_MAX 131072u     /* target: cluster di bawah ini    */
#define MKFS_CL_MIN 65536u      /* syarat FAT32 (>= 65525 cluster) */
    uint8_t  spc = 1;
    uint32_t fat_secs = 1;
    uint64_t clusters = 0;
    for (int it = 0; it < 32; it++) {
        if (part_secs <= (uint64_t)MKFS_RESERVED +
                         (uint64_t)MKFS_FATS * fat_secs + spc)
            return -3;
        uint64_t data = part_secs - MKFS_RESERVED -
                        (uint64_t)MKFS_FATS * fat_secs;
        clusters = data / spc;
        if (clusters > 0x0FFFFFF5ull) {
            if (spc >= 128) return -4;           /* volume too big for FAT32 */
            spc = (uint8_t)(spc << 1);
            continue;
        }
        /* 0.4 Beta (B): naikkan cluster bila volume terlalu banyak
         * cluster-nya — TANPA melanggar batas bawah FAT32. */
        if (clusters > MKFS_CL_MAX && spc < 128 &&
            (clusters >> 1) >= MKFS_CL_MIN) {
            spc = (uint8_t)(spc << 1);
            continue;
        }
        uint32_t need = (uint32_t)(((clusters + 2) * 4 + (FAT_SECTOR - 1)) /
                                   FAT_SECTOR);
        if (need == fat_secs) break;
        fat_secs = need;
        if (it == 31) return -5;
    }

    uint64_t data_start = (uint64_t)MKFS_RESERVED +
                          (uint64_t)MKFS_FATS * fat_secs;
    uint64_t fat1_lba   = MKFS_RESERVED;
    uint64_t fat2_lba   = fat1_lba + fat_secs;
    uint64_t root_lba   = data_start;
    uint32_t root_clus  = 2;
    uint32_t free_c     = clusters > 0 ? (uint32_t)clusters - 1 : 0;

    printf("mkfs: volume utuh 0..%u (%u MB), reserved=%u, spc=%u, FAT=%u sec, %u cluster\n",
           (unsigned)total, (unsigned)((total / 2) / 1024),
           (unsigned)MKFS_RESERVED, (unsigned)spc,
           (unsigned)fat_secs, (unsigned)clusters);

    /* ---- 1. FAT copies: seed sector 0, zero the remainder ---- */
    uint8_t fat0[FAT_SECTOR];
    memset(fat0, 0, FAT_SECTOR);
    mkfs_w32(fat0 + 0, 0x0FFFFFF8u);   /* entry 0: media descriptor  */
    mkfs_w32(fat0 + 4, 0xFFFFFFFFu);   /* entry 1: end-of-chain      */
    mkfs_w32(fat0 + 8, 0x0FFFFFFFu);   /* entry 2: ROOT directory EOC */

    for (int f = 0; f < (int)MKFS_FATS; f++) {
        uint64_t lba = f ? fat2_lba : fat1_lba;
        if (mkfs_zeroes(slot, lba + 1, (uint64_t)fat_secs - 1) != 0) {
            printf("mkfs: gagal menulis FAT#%d\n", f + 1);
            return -6;
        }
        if (mkfs_wr1(slot, lba, fat0) != 0) return -6;
    }

    /* ---- 2. root directory cluster (empty) ---- */
    if (mkfs_zeroes(slot, root_lba, spc) != 0) return -6;

    /* ---- 3. FSInfo ---- */
    uint8_t fi[FAT_SECTOR];
    memset(fi, 0, FAT_SECTOR);
    fi[0] = 'R'; fi[1] = 'R'; fi[2] = 'a'; fi[3] = 'A';
    fi[484] = 'r'; fi[485] = 'r'; fi[486] = 'A'; fi[487] = 'a';
    mkfs_w32(fi + 488, free_c);
    mkfs_w32(fi + 492, 3);                 /* next-free hint */
    fi[510] = 0x55; fi[511] = 0xAA;
    if (mkfs_wr1(slot, MKFS_FSINFO, fi) != 0) return -6;

    /* ---- 4. VBR (BPB) + backup ----
     * Field layout matches mtools exactly: the driver checks
     * bpb[66] == 0x29, reads the label from bpb[71] and the fstype
     * from bpb[82] (NOT the classic 54/59/70 positions). */
    uint8_t vbr[FAT_SECTOR];
    memset(vbr, 0, FAT_SECTOR);
    vbr[0] = 0xEB; vbr[1] = 0x58; vbr[2] = 0x90;
    memcpy(vbr + 3, "MTOO4049", 8);                 /* OEM id   */
    mkfs_w16(vbr + 11, (uint16_t)FAT_SECTOR);       /* 512      */
    vbr[13] = spc;
    mkfs_w16(vbr + 14, (uint16_t)MKFS_RESERVED);
    vbr[16] = (uint8_t)MKFS_FATS;
    mkfs_w16(vbr + 17, 0);                          /* root ents*/
    mkfs_w16(vbr + 19, 0);                          /* tot16    */
    vbr[21] = 0xF8;                                 /* media    */
    mkfs_w16(vbr + 22, 0);                          /* fat16 sz */
    mkfs_w16(vbr + 24, 63);                         /* sec/track*/
    mkfs_w16(vbr + 26, 16);                         /* heads    */
    mkfs_w32(vbr + 28, 0);                          /* hidden   */
    mkfs_w32(vbr + 32, (uint32_t)part_secs);
    mkfs_w32(vbr + 36, fat_secs);
    mkfs_w16(vbr + 40, 0);                          /* ext flags*/
    mkfs_w16(vbr + 42, 0);                          /* fs ver  */
    mkfs_w32(vbr + 44, root_clus);
    mkfs_w16(vbr + 48, (uint16_t)MKFS_FSINFO);      /* FSInfo   */
    mkfs_w16(vbr + 50, 6);                          /* backup   */
    vbr[52] = 0x00;                                 /* drive no */
    vbr[53] = 0x00;
    /* 54..89 = reserved (BPB area untuk salinan boot.img) */
    vbr[66] = 0x29;                                 /* boot sig */
    mkfs_w32(vbr + 67, 0x4E585101u);                /* serial   */
    for (int i = 0; i < 11; i++) vbr[71 + i] = ' ';
    if (label)
        for (int i = 0; label[i] && i < 11; i++) vbr[71 + i] = (uint8_t)label[i];
    memcpy(vbr + 82, "FAT32   ", 8);
    vbr[510] = 0x55; vbr[511] = 0xAA;

    /* ---- 5. VBR TERAKHIR + stub int 18h @0x5A (target jmp EB 58):
     *         BIOS yang mencoba boot di sini jatuh anggun ke CD. ---- */
    vbr[0x5A] = 0xCD; vbr[0x5B] = 0x18;
    if (mkfs_wr1(slot, 0, vbr) != 0) {
        printf("mkfs: gagal menulis VBR\n");
        return -7;
    }
    if (mkfs_wr1(slot, 6, vbr) != 0) return -7;

    printf("mkfs: selesai — label '%s', %u cluster (%u MB data)\n",
           label ? label : "(tanpa label)", (unsigned)clusters,
           (unsigned)((clusters * spc) / 2 / 1024));
    return 0;
}
