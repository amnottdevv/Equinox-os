#ifndef EQUINOX_BLK_H
#define EQUINOX_BLK_H

// ============================================================
//  blk.h — generic block device layer (0.4 Beta "SATA" phase)
// ------------------------------------------------------------
//  One table of block devices in fixed slots, with a common
//  read/write entry point.  Device drivers only differ in HOW a
//  sector is moved; everything above (FAT32, mkfs, MBR parsing,
//  diskinfo, the installer wizard) only cares WHICH slot.
//
//  Slot layout is FIXED-INDEX, never compacting — a hole keeps its
//  index, so names, disk numbers and g_fat.blk_slot values stay
//  byte-identical to the 0.4 Beta era:
//
//    0..3  legacy PATA  (ata0..ata3, hdA..hdD)
//    4..7  AHCI / SATA  (ahci0..ahci3, hde..hdh)
//
//  Consequences of the fixed layout:
//    - a machine WITHOUT an AHCI controller behaves exactly like
//      0.4 Beta (ahci_init() is a no-op, slots 4..7 stay empty),
//    - adding a SATA disk never renumbers the IDE disks,
//    - hd%c naming in the installer wizard keeps working for the
//      legacy disks and just gains hde.. for SATA ones.
//
//  blk_init() = ata_init() then ahci_init().  Drivers register
//  themselves through blk_at(slot) + fill in the blk_desc.
// ============================================================

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLK_MAX_DRIVES  8        // 0..3 legacy PATA, 4..7 AHCI
#define BLK_PATA_SLOTS  4        // first AHCI slot
#define BLK_SECTOR_SIZE 512
#define BLK_NAME_LEN    16
#define BLK_BUS_LEN     24        // "secondary master" is 16 chars + NUL
#define BLK_MODEL_LEN   41

// One command may move at most this many sectors (was ATA_MAX_CHUNK).
#define BLK_MAX_CHUNK   128

// How a device answered IDENTIFY / was attached
enum blk_kind {
    BLK_NONE  = 0,   // empty slot
    BLK_PATA,        // legacy parallel-ATA disk (usable)
    BLK_ATAPI,       // packet device (CD/DVD) — listed, not mountable
    BLK_AHCI         // SATA disk behind an AHCI port (usable)
};

// Primary MBR entry (moved here from ata.h so the FAT layer gets it
// from the block layer, not from the PATA driver).
struct mbr_partition {
    uint8_t  present;      // 1 = entry in use
    uint8_t  bootable;     // 0x80 = active partition
    uint8_t  type;         // MBR type byte (0x0B/0x0C = FAT32)
    uint64_t start_lba;    // first sector of the partition
    uint64_t num_sectors;  // partition length in sectors
};

// One block device.  `ctx` + the two function pointers are the whole
// driver interface; the rest is display/metadata shared by every bus.
struct blk_desc {
    uint8_t  present;                 // 1 = slot holds a device
    uint8_t  kind;                    // enum blk_kind
    uint8_t  lba48;                   // 1 = 48-bit addressing available
    uint64_t total_sectors;           // 0 for ATAPI (size via other means)
    char     name[BLK_NAME_LEN];      // "ata0", "ahci0"
    char     bus[BLK_BUS_LEN];        // "primary master", "ahci port 0"
    char     model[BLK_MODEL_LEN];    // IDENTIFY model string
    void*    ctx;                     // driver-private (slot / port)
    int (*read) (void* ctx, uint64_t lba, uint32_t count, void* buf);
    int (*write)(void* ctx, uint64_t lba, uint32_t count, const void* buf);
};

// Bring up every block driver: ata_init() then ahci_init().
// Safe to call once; a machine with no controller just finds nothing.
void blk_init(void);

// RAW slot pointer — used by drivers while registering, and by code
// that must look at an empty slot too.  NULL when out of range.
struct blk_desc* blk_at(int slot);

// Device pointer, NULL when the slot is empty.  Use this to READ.
struct blk_desc* blk_get(int slot);

// Number of slots (BLK_MAX_DRIVES).  Iterate `for (i = 0; i < blk_count(); i++)`.
int blk_count(void);

// 1 when the slot holds a mountable disk (PATA or AHCI).
// ATAPI/empty slots answer 0 — this replaces `kind != ATA_PATA`.
int blk_is_disk(int slot);

// Read/write raw sectors.  count <= BLK_MAX_CHUNK per call; the FAT
// layer chunks bigger requests itself.  Returns 0 or negative.
int blk_read (int slot, uint64_t lba, uint32_t count, void* buf);
int blk_write(int slot, uint64_t lba, uint32_t count, const void* buf);

// Human-readable size string ("63.5 MB", "2.1 GB") into buf.
void blk_format_size(uint64_t sectors, char* buf, size_t bufsize);

// Parse the MBR partition table of drive `slot` into out[4].
// Returns the number of valid entries found, -1 when the slot is not
// a disk, -2 on a read error and -3 when sector 0 has no 0x55AA
// signature (a "floppy-style" whole-disk volume) — the FAT layer then
// mounts the whole disk at LBA 0.
int mbr_read_partitions(int slot, struct mbr_partition out[4]);

#ifdef __cplusplus
}
#endif

#endif // EQUINOX_BLK_H
