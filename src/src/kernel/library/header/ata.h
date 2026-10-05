#ifndef ATA_H
#define ATA_H

// ============================================================
//  ata.h — ATA/IDE PIO driver (0.4 Beta "Disk" phase)
// ------------------------------------------------------------
//  Polling PIO driver for up to 4 ATA devices on the two legacy
//  IDE buses (primary 0x1F0, secondary 0x170, master + slave).
//  No IRQs are used: every transfer busy-polls the status
//  register, which is safe from task (shell) context — no
//  interrupt handler ever touches the ATA registers.
//
//  Capabilities:
//    - IDENTIFY DEVICE (0xEC) + IDENTIFY PACKET DEVICE (0xA1)
//    - LBA28 read/write (28-bit sector address, up to 137 GB)
//    - LBA48 read/write (48-bit sector address) when supported
//    - FLUSH CACHE (0xE7) after every write batch
//
//  0.4 Beta "SATA": this driver is now just ONE kind of device behind
//  the generic block layer (blk.h).  ata_init() registers slots
//  0..3 into that table; the FAT32 driver, mkfs, diskinfo and the
//  installer wizard all talk to blk.h instead of reaching in here.
//  MBR parsing and size formatting moved to blk.cpp as well.
//  blk.h is included below so every existing includer keeps seeing
//  struct mbr_partition / mbr_read_partitions / blk_format_size.
// ============================================================

#include <stdint.h>
#include <stddef.h>

#include "blk.h"     // the block layer this driver registers into

#ifdef __cplusplus
extern "C" {
#endif

#define ATA_MAX_DRIVES 4     // primary/secondary x master/slave
#define ATA_SECTOR_SIZE 512

// How a drive answered IDENTIFY
enum ata_kind {
    ATA_NONE = 0,     // nothing on this bus/socket (floating bus)
    ATA_PATA,         // parallel ATA disk (usable)
    ATA_ATAPI,        // packet device (CD/DVD) — listed, not mountable
    ATA_UNKNOWN       // answered with an unknown signature
};

struct ata_drive {
    uint8_t present;         // 1 = this slot answered IDENTIFY
    uint8_t kind;            // enum ata_kind
    uint8_t bus;             // 0 = primary, 1 = secondary
    uint8_t drive;           // 0 = master, 0x10 = slave (drive/head bit)
    uint8_t lba48;           // 1 = LBA48 supported
    char    model[41];       // IDENTIFY words 27-46, NUL-terminated
    uint32_t sectors28;      // sectors addressable via LBA28 (0 if none)
    uint64_t total_sectors;  // LBA48 count if supported, else LBA28
};

// Scan all 4 slots (IDENTIFY). Fills the internal table; results are
// also kept for ata_get_drive() / diskinfo. Never panics — a machine
// without disks simply reports zero present drives.  Also registers
// each present drive into the block layer at slot 0..3.
void ata_init(void);

// Drive slot 0..3 = [primary master, primary slave, secondary master,
// secondary slave]. Returns NULL when the slot is empty.
struct ata_drive* ata_get_drive(int slot);

// Read/write raw sectors from drive slot. count is capped at 128 per
// call by the driver (the FAT layer chunks bigger requests itself).
// Returns 0 on success, negative on error.  Prefer blk_read/blk_write,
// which work for SATA disks too.
int ata_read_sectors (int slot, uint64_t lba, uint32_t count, void* buf);
int ata_write_sectors(int slot, uint64_t lba, uint32_t count, const void* buf);

#ifdef __cplusplus
}
#endif

#endif // ATA_H
