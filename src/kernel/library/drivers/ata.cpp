// ============================================================
//  ata.cpp — ATA/IDE PIO driver (v0.2 "Disk" phase)
// ------------------------------------------------------------
//  Polling PIO implementation over the two legacy IDE buses.
//  Everything here runs in task (shell) context only; no IRQ
//  handler touches these registers, so no locking is needed
//  (the kernel is cooperative and single-CPU).
//
//  Port map (primary; secondary = same layout at 0x170/0x376):
//    0x1F0 data (16-bit)      0x1F4 LBA mid
//    0x1F1 error/features     0x1F5 LBA hi
//    0x1F2 sector count       0x1F6 drive/head
//    0x1F3 LBA lo             0x1F7 status (read) / command (write)
//    0x3F6 alternate status (reading it does NOT clear IRQ)
// ============================================================

#include "header/ata.h"
#include "header/stdio.h"      // inb/outb, printf
#include "header/libstring.h"  // memset

// ---- 16-bit port I/O (inb/outb live in stdio.cpp) ---------------
static inline uint16_t port_inw(uint16_t port) {
    uint16_t v;
    asm volatile("inw %w1, %w0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void port_outw(uint16_t port, uint16_t val) {
    asm volatile("outw %w0, %w1" : : "a"(val), "Nd"(port));
}

// ---- register layout of the two buses ----------------------------
struct ata_bus_ports {
    uint16_t data;
    uint16_t err;      // read: error / write: features
    uint16_t count;
    uint16_t lba_lo;
    uint16_t lba_mid;
    uint16_t lba_hi;
    uint16_t drive;    // drive/head select
    uint16_t status;   // read: status / write: command
    uint16_t ctrl;     // alternate status / device control
};
static const struct ata_bus_ports buses[2] = {
    { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 },
    { 0x170, 0x171, 0x172, 0x173, 0x174, 0x175, 0x176, 0x177, 0x376 },
};

// ---- status register bits ----------------------------------------
#define ATA_STAT_ERR  0x01
#define ATA_STAT_DRQ  0x08
#define ATA_STAT_DF   0x20
#define ATA_STAT_DRDY 0x40
#define ATA_STAT_BSY  0x80

// ---- commands -----------------------------------------------------
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_READ_PIO28      0x20
#define ATA_CMD_WRITE_PIO28     0x30
#define ATA_CMD_READ_PIO48      0x24
#define ATA_CMD_WRITE_PIO48     0x34
#define ATA_CMD_FLUSH_CACHE     0xE7

// One command may move at most this many sectors (keeps the
// sector-count register within its 8/16-bit field for LBA28/48 and
// bounds the polling loop; the FAT layer chunks anything larger).
#define ATA_MAX_CHUNK 128

// ---- drive table --------------------------------------------------
static struct ata_drive drives[ATA_MAX_DRIVES];
static int ata_scan_done = 0;

// ===================================================================
//  Low-level helpers
// ===================================================================

// 400 ns busy-wait: four reads of the alternate status register.
// QEMU answers instantly, real hardware needs the settling time.
static void ata_400ns(const struct ata_bus_ports* io) {
    for (int i = 0; i < 4; i++) (void)inb(io->ctrl);
}

// Wait until BSY clears. Returns the final status, or 0 on timeout
// (a missing device on a floating bus reads 0xFF; a timeout also
// yields 0 so callers treat it as "no data").
static uint8_t ata_wait_not_busy(const struct ata_bus_ports* io,
                                 uint32_t timeout_loops) {
    while (timeout_loops--) {
        uint8_t st = inb(io->status);
        if (st == 0xFF || st == 0x7F) return 0;   // floating bus
        if (!(st & ATA_STAT_BSY)) return st;
        ata_400ns(io);
    }
    return 0;
}

// After a data command: wait for BSY clear AND DRQ set (or error).
static int ata_wait_drq(const struct ata_bus_ports* io) {
    for (uint32_t i = 0; i < 2000000u; i++) {
        uint8_t st = inb(io->status);
        if (st == 0xFF || st == 0x7F) return -1;
        if (st & ATA_STAT_ERR) return -2;
        if (st & ATA_STAT_DF)  return -3;
        if (!(st & ATA_STAT_BSY) && (st & ATA_STAT_DRQ)) return 0;
        ata_400ns(io);
    }
    return -4;   // timeout
}

// Select master/slave and give the drive time to react.
static void ata_select(const struct ata_bus_ports* io, uint8_t drive_bit) {
    outb(io->drive, 0xA0 | drive_bit);   // 0xA0 = legacy CHS bit, LBA off for now
    ata_400ns(io);
}

// ===================================================================
//  IDENTIFY
// ===================================================================

// Run IDENTIFY (0xEC or 0xA1) into `idwords` (256 words).
// Returns: 1 = answered with data, 0 = no device, -1 = device present
// but the command aborted (unknown signature).
static int ata_identify(uint8_t bus, uint8_t drive_bit, uint8_t packet,
                        uint16_t* idwords) {
    const struct ata_bus_ports* io = &buses[bus];

    ata_select(io, drive_bit);

    // Pre-command sanity: on a floating bus the status reads 0xFF
    // (QEMU) — nothing is attached at all.
    uint8_t st = inb(io->status);
    if (st == 0xFF || st == 0x7F) return 0;

    // Wait for the drive to be ready (BSY clear). A slave socket with
    // no device behind a present master usually reads status 0 here.
    st = ata_wait_not_busy(io, 100000);
    if (st == 0) return 0;

    outb(io->count, 0);
    outb(io->lba_lo, 0);
    outb(io->lba_mid, 0);
    outb(io->lba_hi, 0);
    outb(io->status, packet ? ATA_CMD_IDENTIFY_PACKET : ATA_CMD_IDENTIFY);

    st = inb(io->status);
    if (st == 0) return 0;                 // no second drive on the bus
    if (st & ATA_STAT_ERR) {
        // ATAPI drives abort plain IDENTIFY but answer 0xA1; some old
        // disks vice versa. The caller retries with the other command.
        return -1;
    }
    if (ata_wait_drq(io) != 0) return 0;

    for (int i = 0; i < 256; i++) idwords[i] = port_inw(io->data);

    // Post-transfer: a second drive that does not exist can still
    // present a transient DRQ; validate via the ATA signature bytes.
    uint8_t cl = inb(io->lba_mid), ch = inb(io->lba_hi);
    if (!packet && cl == 0x14 && ch == 0xEB) {
        // ATAPI signature — this slot is a packet device that answered
        // the wrong command; caller will redo it with 0xA1.
        return -2;
    }
    return 1;
}

// Copy IDENTIFY words 27-46 into a C string (each word is byte-swapped:
// the high byte is the first character).
static void ata_model_from_words(const uint16_t* w, char* out, size_t cap) {
    size_t o = 0;
    for (int i = 27; i <= 46 && o + 1 < cap; i++) {
        char c1 = (char)(w[i] >> 8);
        char c2 = (char)(w[i] & 0xFF);
        if (c1) out[o++] = c1;
        if (c1 && !c2) break;              // NUL inside a word = end
        if (o + 1 < cap && c2) out[o++] = c2;
    }
    // trim leading/trailing spaces
    size_t end = o;
    while (end > 0 && out[end - 1] == ' ') end--;
    size_t start = 0;
    while (start < end && out[start] == ' ') start++;
    for (size_t i = start; i < end; i++) out[i - start] = out[i];
    out[end - start] = '\0';
}

void ata_init(void) {
    memset((uint8_t*)drives, 0, sizeof(drives));

    int slot = 0;
    for (uint8_t bus = 0; bus < 2; bus++) {
        for (uint8_t slave = 0; slave < 2; slave++) {
            struct ata_drive* d = &drives[slot];
            d->bus   = bus;
            d->drive = slave ? 0x10 : 0x00;

            uint16_t id[256];
            int r = ata_identify(bus, d->drive, 0, id);
            if (r == -2 || (r == -1 && slave == 0)) {
                // ATAPI signature or aborted plain IDENTIFY: retry as packet
                r = ata_identify(bus, d->drive, 1, id);
                if (r == 1) {
                    d->present = 1;
                    d->kind    = ATA_ATAPI;
                    ata_model_from_words(id, d->model, sizeof(d->model));
                    d->total_sectors = 0;   // packet: size via other means
                    slot++;
                    continue;
                }
            }
            if (r != 1) { slot++; continue; }   // empty socket

            d->present = 1;
            d->kind    = ATA_PATA;
            ata_model_from_words(id, d->model, sizeof(d->model));

            // LBA28 sector count: words 60-61
            d->sectors28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);

            // LBA48 supported? word 83 bit 10 (only if word 82/83 valid)
            uint16_t cmdsets = id[83];
            d->lba48 = ((cmdsets & 0x0400) && !(id[53] & 1 ? 0 : 0) &&
                        (cmdsets & 0xC000) == 0x4000) ? 1 : 0;

            if (d->lba48) {
                // words 100-103: 64-bit total sectors
                d->total_sectors = (uint64_t)id[100] |
                                   ((uint64_t)id[101] << 16) |
                                   ((uint64_t)id[102] << 32) |
                                   ((uint64_t)id[103] << 48);
            } else {
                d->total_sectors = d->sectors28;
            }
            slot++;
        }
    }
    ata_scan_done = 1;
}

struct ata_drive* ata_get_drive(int slot) {
    if (slot < 0 || slot >= ATA_MAX_DRIVES) return NULL;
    struct ata_drive* d = &drives[slot];
    return d->present ? d : NULL;
}

void ata_format_size(uint64_t sectors, char* buf, size_t bufsize) {
    uint64_t bytes = sectors * ATA_SECTOR_SIZE;
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
//  Sector transfer core
// ===================================================================

// Set up the sector count + LBA registers and issue the command.
static int ata_cmd_lba(const struct ata_bus_ports* io, uint8_t drive_bit,
                       uint8_t lba48, uint8_t write, uint64_t lba,
                       uint32_t count) {
    ata_select(io, drive_bit);
    if (ata_wait_not_busy(io, 100000) == 0) return -1;
    if (!(inb(io->status) & ATA_STAT_DRDY)) {
        // Some drives need one more settling read; real failure = no disk
        ata_400ns(io);
        if (!(inb(io->status) & ATA_STAT_DRDY)) return -1;
    }

    if (lba48) {
        // LBA48: high bytes first, then low bytes (the drive latches
        // the "previous write" as the high half of each field).
        outb(io->drive, 0x40 | drive_bit);             // LBA mode
        outb(io->count, (uint8_t)(count >> 8));
        outb(io->lba_lo, (uint8_t)(lba >> 24));
        outb(io->lba_mid, (uint8_t)(lba >> 32));
        outb(io->lba_hi, (uint8_t)(lba >> 40));
        outb(io->count, (uint8_t)(count & 0xFF));
        outb(io->lba_lo, (uint8_t)(lba & 0xFF));
        outb(io->lba_mid, (uint8_t)(lba >> 8));
        outb(io->lba_hi, (uint8_t)(lba >> 16));
        outb(io->status, write ? ATA_CMD_WRITE_PIO48 : ATA_CMD_READ_PIO48);
    } else {
        if (lba > 0x0FFFFFFFull || count > 256) return -2;  // out of range
        outb(io->drive, 0xE0 | drive_bit |
                        (uint8_t)((lba >> 24) & 0x0F));    // LBA mode
        outb(io->count, (uint8_t)(count & 0xFF));
        outb(io->lba_lo, (uint8_t)(lba & 0xFF));
        outb(io->lba_mid, (uint8_t)(lba >> 8));
        outb(io->lba_hi, (uint8_t)(lba >> 16));
        outb(io->status, write ? ATA_CMD_WRITE_PIO28 : ATA_CMD_READ_PIO28);
    }
    return 0;
}

int ata_read_sectors(int slot, uint64_t lba, uint32_t count, void* buf) {
    struct ata_drive* d = ata_get_drive(slot);
    if (!d || d->kind != ATA_PATA) return -1;
    if (!count || count > ATA_MAX_CHUNK || !buf) return -2;
    if (lba + count > d->total_sectors) return -3;      // beyond the disk

    const struct ata_bus_ports* io = &buses[d->bus];
    int use48 = (d->lba48 && lba > 0x0FFFFFFFull);

    if (ata_cmd_lba(io, d->drive, (uint8_t)use48, 0, lba, count) != 0)
        return -4;

    uint16_t* out = (uint16_t*)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq(io) != 0) return -5;
        for (int w = 0; w < 256; w++) out[w] = port_inw(io->data);
        out += 256;
    }
    // drain trailing status (clears DRQ)
    (void)inb(io->status);
    return 0;
}

int ata_write_sectors(int slot, uint64_t lba, uint32_t count, const void* buf) {
    struct ata_drive* d = ata_get_drive(slot);
    if (!d || d->kind != ATA_PATA) return -1;
    if (!count || count > ATA_MAX_CHUNK || !buf) return -2;
    if (lba + count > d->total_sectors) return -3;

    const struct ata_bus_ports* io = &buses[d->bus];
    int use48 = (d->lba48 && lba > 0x0FFFFFFFull);

    if (ata_cmd_lba(io, d->drive, (uint8_t)use48, 1, lba, count) != 0)
        return -4;

    const uint16_t* src = (const uint16_t*)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq(io) != 0) return -5;
        for (int w = 0; w < 256; w++) port_outw(io->data, src[w]);
        src += 256;
    }
    // wait for the buffer to drain, then FLUSH CACHE so the data is
    // durable in the backing file even if QEMU is killed.
    if (ata_wait_not_busy(io, 100000) == 0) return -6;
    outb(io->status, ATA_CMD_FLUSH_CACHE);
    (void)ata_wait_not_busy(io, 100000);
    return 0;
}

// ===================================================================
//  MBR parsing
// ===================================================================

int mbr_read_partitions(int slot, struct mbr_partition out[4]) {
    for (int i = 0; i < 4; i++) {
        out[i].present = 0;
        out[i].bootable = 0;
        out[i].type = 0;
        out[i].start_lba = 0;
        out[i].num_sectors = 0;
    }
    struct ata_drive* d = ata_get_drive(slot);
    if (!d || d->kind != ATA_PATA) return -1;

    uint8_t sec[ATA_SECTOR_SIZE];
    if (ata_read_sectors(slot, 0, 1, sec) != 0) return -2;
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

// Silence the "defined but not used" warning when the debug printer
// below is compiled out of test builds.
void ata_debug_dump(void);
void ata_debug_dump(void) {
    if (!ata_scan_done) return;
    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        struct ata_drive* d = &drives[i];
        printf("ata%d: %s bus=%u drive=%u\n", i,
               d->present ? "present" : "empty", d->bus, d->drive >> 4);
    }
}
