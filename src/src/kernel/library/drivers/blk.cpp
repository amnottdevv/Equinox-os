// ============================================================
//  blk.cpp — generic block device layer (0.4 Beta "SATA" phase)
// ------------------------------------------------------------
//  The slot table plus the bus-independent helpers that used to
//  live in ata.cpp (MBR parsing, size formatting).  Device
//  drivers fill in a blk_desc via blk_at(); this file never
//  talks to hardware itself, except through blk_init() which
//  brings the drivers up in order.
//
//  SLOTS ARE FIXED-INDEX (see blk.h): holes keep their index, so
//  a PATA disk is always ata0..ata3 and an AHCI disk always
//  lands on slot 4 or higher.
// ============================================================

#include "header/blk.h"
#include "header/ata.h"    // ata_init()  (ata.h itself pulls blk.h)
#include "header/ahci.h"   // ahci_init()
#include "header/stdio.h"  // snprintf
#include "header/libstring.h"  // memset

static struct blk_desc g_blk[BLK_MAX_DRIVES];

void blk_init(void) {
    // Order matters: the legacy disks keep slots 0..3, the SATA
    // driver then only ever claims slots 4 and above.  On a machine
    // without an AHCI controller ahci_init() returns immediately and
    // slots 4..7 stay zeroed — identical to a 0.4 Beta boot.
    ata_init();
    ahci_init();
}

struct blk_desc* blk_at(int slot) {
    if (slot < 0 || slot >= BLK_MAX_DRIVES) return NULL;
    return &g_blk[slot];
}

struct blk_desc* blk_get(int slot) {
    struct blk_desc* d = blk_at(slot);
    return (d && d->present) ? d : NULL;
}

int blk_count(void) { return BLK_MAX_DRIVES; }

int blk_is_disk(int slot) {
    struct blk_desc* d = blk_get(slot);
    if (!d) return 0;
    return (d->kind == BLK_PATA || d->kind == BLK_AHCI) ? 1 : 0;
}

// Thin dispatcher: bounds check + hand over to the driver.  The
// driver itself does the sched-lock (the ATA path has always taken
// it around a whole transfer, and the AHCI one follows suit), so no
// lock lives here — otherwise we would nest.
int blk_read(int slot, uint64_t lba, uint32_t count, void* buf) {
    struct blk_desc* d = blk_get(slot);
    if (!d || !d->read) return -1;
    if (!count || count > BLK_MAX_CHUNK || !buf) return -2;
    if (lba + count > d->total_sectors) return -3;   // beyond the disk
    return d->read(d->ctx, lba, count, buf);
}

int blk_write(int slot, uint64_t lba, uint32_t count, const void* buf) {
    struct blk_desc* d = blk_get(slot);
    if (!d || !d->write) return -1;
    if (!count || count > BLK_MAX_CHUNK || !buf) return -2;
    if (lba + count > d->total_sectors) return -3;
    return d->write(d->ctx, lba, count, buf);
}

void blk_format_size(uint64_t sectors, char* buf, size_t bufsize) {
    uint64_t bytes = sectors * BLK_SECTOR_SIZE;
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(buf, bufsize, "%u.%u GB",
                 (unsigned)(bytes / (1024ull * 1024 * 1024)),
                 (unsigned)((bytes % (1024ull * 1024 * 1024)) * 10 /
                            (1024ull * 1024 * 1024)));
    else
        snprintf(buf, bufsize, "%u.%u MB",
                 (unsigned)(bytes / (1024ull * 1024)),
                 (unsigned)((bytes % (1024ull * 1024)) * 10 / (1024ull * 1024)));
}

// ===================================================================
//  MBR parsing  (moved verbatim from ata.cpp, now bus-neutral)
// ===================================================================

int mbr_read_partitions(int slot, struct mbr_partition out[4]) {
    for (int i = 0; i < 4; i++) {
        out[i].present = 0;
        out[i].bootable = 0;
        out[i].type = 0;
        out[i].start_lba = 0;
        out[i].num_sectors = 0;
    }
    if (!blk_is_disk(slot)) return -1;

    uint8_t sec[BLK_SECTOR_SIZE];
    if (blk_read(slot, 0, 1, sec) != 0) return -2;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return -3;  // no MBR

    int found = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = &sec[446 + i * 16];
        uint8_t type = e[4];
        uint32_t start = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                         ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t nsec  = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                         ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (type == 0 || nsec == 0) continue;             // empty entry
        out[i].present    = 1;
        out[i].bootable   = e[0];
        out[i].type       = type;
        out[i].start_lba  = start;
        out[i].num_sectors = nsec;
        found++;
    }
    return found;
}
