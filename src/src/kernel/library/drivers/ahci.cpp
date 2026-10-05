// ============================================================
//  ahci.cpp — AHCI SATA controller driver (0.4 Beta)
// ------------------------------------------------------------
//  Polling, task-file, no IRQ — deliberately the same shape as
//  ata.cpp: every transfer runs in task (shell) context under
//  task_sched_lock(), so no interrupt handler ever touches the
//  HBA registers and no locking beyond that is needed.
//
//  DMA memory: three static buffers per port (command list,
//  FIS receive area, command table + PRDT) live in the kernel BSS
//  inside the identity-mapped low RAM, so bus address == pointer.
//  The DATA buffer is the caller's, identity-mapped too — same
//  assumption e1000.c already makes for its rings.
//
//  Sequence per port (matches Linux ahci_port_start / QEMU's
//  ahci_cond_start_engines, and the mmxsrup reference driver):
//    1. clear ST+FRE, wait for CR/FR to drop  (SeaBIOS may have
//       left the port running against buffers that no longer exist)
//    2. PxSSTS.DET == 3 ?  else the port is empty
//    3. PxSIG != 0xEB140101 (ATAPI) — CD over AHCI is deferred
//    4. program PxCLB/PxFB, clear PxIS + PxSERR, set
//       ST|FRE|SPIN_UP|POD in one write
//    5. IDENTIFY (0xEC) over a 512-byte PRD
//    6. blk_at(4+n) and fill in the descriptor
//
//  A command is one slot (slot 0): write the command header +
//  command table, wait for PxTFD to be idle, set PxCI bit 0, then
//  poll PxCI clearing — with PxIS.TFES as an early bail-out,
//  because ahci_clear_cmd_issue() deliberately LEAVES PxCI set when
//  the drive reported an error.  On any timeout/error the port is
//  restarted (dropping ST resets PxCI in hardware), so a failed
//  command can never wedge the controller.
// ============================================================

#include "header/ahci.h"
#include "header/blk.h"
#include "header/pci.h"
#include "header/stdio.h"      // printf / snprintf
#include "header/timer.h"      // get_tick() — wall-clock timeouts
#include "header/task.h"       // task_sched_lock/unlock
#include "header/libstring.h"  // memset / memcpy

// ---- host registers (byte offsets from ABAR) -----------------------
#define AHCI_HC_CAP   0x00
#define AHCI_HC_GHC   0x04
#define AHCI_HC_IS    0x08
#define AHCI_HC_PI    0x0C
#define AHCI_HC_VS    0x10

#define AHCI_GHC_AE   (1u << 31)   // AHCI enable / legacy mode off

// ---- port registers ------------------------------------------------
#define AHCI_PORT_BASE      0x100
#define AHCI_PORT_SIZE      0x80

#define AHCI_P_CLB          0x00   // command list base (low)
#define AHCI_P_CLBU         0x04   // command list base (high)
#define AHCI_P_FB           0x08   // FIS receive base (low)
#define AHCI_P_FBU          0x0C   // FIS receive base (high)
#define AHCI_P_IS           0x10   // interrupt status (RW1C)
#define AHCI_P_IE           0x14   // interrupt enable (we leave it 0)
#define AHCI_P_CMD          0x18   // port command / status
#define AHCI_P_TFD          0x20   // task file data (status/error)
#define AHCI_P_SIG          0x24   // signature
#define AHCI_P_SSTS         0x28   // SATA status (SCR0)
#define AHCI_P_SCTL         0x2C   // SATA control (SCR2)
#define AHCI_P_SERR         0x30   // SATA error   (SCR1, RW1C)
#define AHCI_P_SACT         0x34   // SATA active (NCQ, unused)
#define AHCI_P_CI           0x38   // command issue

#define AHCI_CMD_START      (1u << 0)
#define AHCI_CMD_SPIN_UP    (1u << 1)
#define AHCI_CMD_POWER_ON   (1u << 2)
#define AHCI_CMD_FIS_RX     (1u << 4)
#define AHCI_CMD_FIS_ON     (1u << 14)  // read-only: engine running
#define AHCI_CMD_LIST_ON    (1u << 15)  // read-only: engine running

// PxTFD bits (the low byte is the ATA status register)
#define AHCI_TFD_BSY        0x80
#define AHCI_TFD_DRDY       0x40
#define AHCI_TFD_DF         0x20
#define AHCI_TFD_DRQ        0x08
#define AHCI_TFD_ERR        0x01

// PxIS bit used as the "this command is never going to clear PxCI" flag
#define AHCI_IRQ_TFES       (1u << 30)

// PxSSTS bits 3:0 = DET (3 = device present + PHY up)
#define AHCI_SSTS_DET_MASK  0x0Fu
#define AHCI_SSTS_DET_UP    0x03u

// PxSIG of a packet device (ATAPI), reported by QEMU after port reset
#define AHCI_SIG_ATAPI      0xEB140101u

// CFIS commands (Register Host-to-Device FIS, type 0x27)
#define ATA_IDENTIFY        0xEC
#define ATA_READ_DMA        0xC8    // LBA28
#define ATA_WRITE_DMA       0xCA    // LBA28
#define ATA_READ_DMA_EXT    0x25    // LBA48
#define ATA_WRITE_DMA_EXT   0x35    // LBA48
#define ATA_FLUSH_CACHE     0xE7
#define ATA_FLUSH_CACHE_EXT 0xEA

// Command table layout (AHCI spec 3.3.1)
#define AHCI_CFIS_SZ        0x40    // 0x00..0x3F  CFIS
#define AHCI_ACMD_SZ        0x10    // 0x40..0x4F  ATAPI command
#define AHCI_CMD_TBL_HDR    0x80    // 0x80        first PRD entry

// 128 sectors = 64 KB per command; PRDs are split on 4 KB boundaries
// so no entry ever crosses one. Worst case (buffer unaligned) needs
// 17 entries — 24 leaves headroom.
#define AHCI_MAX_PRD        24

#define AHCI_MAX_PORTS      8       // contexts we keep (ich9 has 6)
#define AHCI_POOL           4       // ports that may hold DMA buffers

// ---- on-disk/in-memory structures (packed, little-endian) ----------
struct ahci_cmd_hdr {                 // 32 bytes, DW0..DW7
    uint32_t opts;                    // [15:0] flags (CFL in 4:0), [31:16] PRDTL
    uint32_t status;                  // bytes transferred, written by the HBA
    uint32_t tbl_lo;                  // command table address (low)
    uint32_t tbl_hi;                  // command table address (high)
    uint32_t reserved[4];
} __attribute__((packed));

struct ahci_prd {                     // 16 bytes
    uint64_t addr;                    // data base address
    uint32_t reserved;
    uint32_t dbc;                     // [21:0] byte count MINUS 1
} __attribute__((packed));

typedef char ahci_cmd_hdr_sz[(sizeof(struct ahci_cmd_hdr) == 32) ? 1 : -1];
typedef char ahci_prd_sz[(sizeof(struct ahci_prd) == 16) ? 1 : -1];

// Per-port DMA pool. Three separate allocations because each has its
// own alignment requirement (1024 / 256 / 128).
struct ahci_port_mem {
    uint8_t clb[1024]                    __attribute__((aligned(1024)));
    uint8_t fb[256]                      __attribute__((aligned(256)));
    uint8_t ctab[AHCI_CMD_TBL_HDR +
                 AHCI_MAX_PRD * sizeof(struct ahci_prd)]
                                         __attribute__((aligned(128)));
};
typedef char ahci_pool_align[(sizeof(struct ahci_port_mem) % 1024 == 0)
                             ? 1 : -1];

static struct ahci_port_mem g_mem[AHCI_POOL];
static uint8_t g_mem_used[AHCI_POOL];

struct ahci_port {
    volatile uint8_t* base;            // ABAR + 0x100 + n*0x80
    int    pool;                       // index into g_mem[], -1 = none
    int    no;                         // port number
    uint8_t lba48;                     // drive supports 48-bit LBA
    uint64_t total_sectors;
};

static volatile uint8_t* g_abar;
static struct ahci_port g_port[AHCI_MAX_PORTS];
static int g_port_cnt;                 // registered port contexts
static int g_disk_cnt;                 // disks handed to the blk layer

// ---- MMIO accessors ------------------------------------------------
static uint32_t mm_r32(const volatile uint8_t* p) {
    return *(const volatile uint32_t*)p;
}
static void mm_w32(volatile uint8_t* p, uint32_t v) {
    *(volatile uint32_t*)p = v;
}
static uint32_t port_r32(volatile uint8_t* base, uint32_t off) {
    return *(const volatile uint32_t*)(base + off);
}
static void port_w32(volatile uint8_t* base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}

// ===================================================================
//  Port lifecycle
// ===================================================================

// Stop both DMA engines and wait for CR/FR to drop (AHCI 10.4.2).
static int ahci_port_stop(volatile uint8_t* base) {
    uint32_t cmd = port_r32(base, AHCI_P_CMD);
    port_w32(base, AHCI_P_CMD, cmd & ~(AHCI_CMD_START | AHCI_CMD_FIS_RX));
    uint32_t start = get_tick();
    uint32_t guard = 0;
    while (((get_tick() - start) <= 1000u) && guard++ < 400000000u) {
        if ((port_r32(base, AHCI_P_CMD) &
             (AHCI_CMD_LIST_ON | AHCI_CMD_FIS_ON)) == 0)
            return 0;
    }
    return -1;   // engine stuck — the caller still reprograms it
}

// Point the port at our buffers and start it.
static int ahci_port_start(volatile uint8_t* base, struct ahci_port_mem* m) {
    port_w32(base, AHCI_P_CLB,  (uint32_t)(uintptr_t)m->clb);
    port_w32(base, AHCI_P_CLBU, 0);
    port_w32(base, AHCI_P_FB,   (uint32_t)(uintptr_t)m->fb);
    port_w32(base, AHCI_P_FBU,  0);
    port_w32(base, AHCI_P_IS,   0xFFFFFFFFu);   // RW1C: drop stale status
    port_w32(base, AHCI_P_SERR, 0xFFFFFFFFu);
    // One write starts both engines (QEMU maps CLB/FB right here and
    // clears them again if the addresses are unmappable).
    port_w32(base, AHCI_P_CMD,
             AHCI_CMD_START | AHCI_CMD_FIS_RX |
             AHCI_CMD_SPIN_UP | AHCI_CMD_POWER_ON);

    uint32_t start = get_tick();
    uint32_t guard = 0;
    while (((get_tick() - start) <= 1000u) && guard++ < 400000000u) {
        if (port_r32(base, AHCI_P_CMD) & AHCI_CMD_START) return 0;
    }
    return -1;
}

// Drop PxCI + re-run the start sequence. Clearing ST resets PxCI in
// hardware (and in QEMU), which is the only supported way to get rid
// of a PxCI bit that an errored command left behind.
static int ahci_port_restart(volatile uint8_t* base, int pool) {
    (void)ahci_port_stop(base);
    if (pool < 0) return -1;
    return ahci_port_start(base, &g_mem[pool]);
}

static int ahci_wait_idle(volatile uint8_t* base, uint32_t timeout_ms) {
    uint32_t start = get_tick();
    uint32_t guard = 0;
    while (((get_tick() - start) <= timeout_ms) && guard++ < 400000000u) {
        uint32_t tfd = port_r32(base, AHCI_P_TFD);
        if (!(tfd & AHCI_TFD_BSY) && !(tfd & AHCI_TFD_DRQ)) return 0;
    }
    return -1;
}

static int ahci_pool_alloc(void) {
    for (int i = 0; i < AHCI_POOL; i++) {
        if (!g_mem_used[i]) { g_mem_used[i] = 1; return i; }
    }
    return -1;
}
static void ahci_pool_free(int pool) {
    if (pool >= 0 && pool < AHCI_POOL) g_mem_used[pool] = 0;
}

// ===================================================================
//  Command execution (one slot: command table at clb[0] + ctab)
// ===================================================================

// `cfis` must be a 20-byte Register H2D FIS. `buf` may be NULL for a
// non-data command (bytes = 0, nprdt = 0).
static int ahci_issue(volatile uint8_t* base, int pool,
                      const uint8_t* cfis, int is_write,
                      void* buf, uint32_t bytes, uint32_t timeout_ms) {
    if (pool < 0) return -1;
    struct ahci_port_mem* m = &g_mem[pool];

    if (ahci_wait_idle(base, 2000) != 0) return -1;
    if (port_r32(base, AHCI_P_CI) & 1u) return -2;   // stale bit
    port_w32(base, AHCI_P_IS, 0xFFFFFFFFu);

    // ---- command table: CFIS then the PRDT ----
    uint8_t* ct = m->ctab;
    memset(ct, 0, AHCI_CMD_TBL_HDR);
    memcpy(ct, cfis, 20);

    int nprdt = 0;
    if (bytes) {
        uint8_t* q = (uint8_t*)buf;
        uint32_t rem = bytes;
        while (rem) {
            if (nprdt >= AHCI_MAX_PRD) return -3;
            uint64_t a = (uint64_t)(uintptr_t)q;
            uint32_t chunk = 4096u - (uint32_t)(a & 4095u);
            if (chunk > rem) chunk = rem;
            struct ahci_prd* pr =
                (struct ahci_prd*)(ct + AHCI_CMD_TBL_HDR +
                                   nprdt * sizeof(struct ahci_prd));
            pr->addr     = a;
            pr->reserved = 0;
            pr->dbc      = (chunk - 1) & 0x3FFFFFu;   // zero-based
            q += chunk;
            rem -= chunk;
            nprdt++;
        }
    }

    // ---- command header slot 0 ----
    struct ahci_cmd_hdr* hdr = (struct ahci_cmd_hdr*)m->clb;
    hdr->status = 0;
    hdr->opts   = 5u                                   // CFL = 5 dwords
                | (is_write ? (1u << 6) : 0u)          // WRITE
                | ((uint32_t)nprdt << 16);             // PRDTL
    hdr->tbl_lo = (uint32_t)(uintptr_t)ct;
    hdr->tbl_hi = 0;

    asm volatile("mfence" ::: "memory");   // command data before PxCI

    port_w32(base, AHCI_P_CI, 1u);

    // ---- poll completion ----
    int rc = 0;
    uint32_t start = get_tick();
    uint32_t guard = 0;
    for (;;) {
        if ((port_r32(base, AHCI_P_CI) & 1u) == 0) break;   // done
        // An errored command never clears PxCI (QEMU's
        // ahci_clear_cmd_issue), so TFES is our real completion flag.
        if (port_r32(base, AHCI_P_IS) & AHCI_IRQ_TFES) { rc = -4; break; }
        if (((get_tick() - start) > timeout_ms) ||
            guard++ > 400000000u) { rc = -5; break; }
    }
    if (rc == 0) {
        uint32_t tfd = port_r32(base, AHCI_P_TFD);
        if (tfd & (AHCI_TFD_ERR | AHCI_TFD_DF)) rc = -6;
    }
    if (rc != 0) ahci_port_restart(base, pool);
    return rc;
}

// ===================================================================
//  IDENTIFY
// ===================================================================

// Copy IDENTIFY words 27..46 into a C string (byte-swapped words).
static void ahci_model_from_words(const uint16_t* w, char* out, size_t cap) {
    size_t o = 0;
    for (int i = 27; i <= 46 && o + 1 < (size_t)cap; i++) {
        char c1 = (char)(w[i] >> 8);
        char c2 = (char)(w[i] & 0xFF);
        if (c1) out[o++] = c1;
        if (c1 && !c2) break;              // NUL inside a word = end
        if (o + 1 < (size_t)cap && c2) out[o++] = c2;
    }
    out[o] = '\0';
    size_t end = o;
    while (end > 0 && out[end - 1] == ' ') end--;
    size_t start = 0;
    while (start < end && out[start] == ' ') start++;
    for (size_t i = start; i < end; i++) out[i - start] = out[i];
    out[end - start] = '\0';
}

static int ahci_identify(volatile uint8_t* base, int pool, uint16_t* id) {
    uint8_t cfis[20];
    memset(cfis, 0, sizeof(cfis));
    cfis[0] = 0x27;                 // Register H2D FIS
    cfis[1] = 0x80;                 // update the command register
    cfis[2] = ATA_IDENTIFY;
    cfis[7] = 0xE0;                 // classic drive/head value
    if (ahci_issue(base, pool, cfis, 0, id, 512, 3000) != 0) return -1;
    if (id[0] == 0x0000 || id[0] == 0xFFFF) return -2;  // garbage
    return 0;
}

// ===================================================================
//  Read / write  (LBA48 when the drive says it can, else LBA28 —
//  exactly the same choice ata.cpp makes)
// ===================================================================

static int ahci_rw(struct ahci_port* p, int is_write,
                   uint64_t lba, uint32_t count, void* buf) {
    uint8_t cfis[20];
    memset(cfis, 0, sizeof(cfis));
    cfis[0] = 0x27;
    cfis[1] = 0x80;

    int use48 = (p->lba48 && lba > 0x0FFFFFFFull);
    if (use48) {
        cfis[2]  = is_write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT;
        cfis[7]  = 0x40;                    // LBA mode, drive 0
        cfis[4]  = (uint8_t)(lba);
        cfis[5]  = (uint8_t)(lba >> 8);
        cfis[6]  = (uint8_t)(lba >> 16);
        cfis[8]  = (uint8_t)(lba >> 24);
        cfis[9]  = (uint8_t)(lba >> 32);
        cfis[10] = (uint8_t)(lba >> 40);
        cfis[12] = (uint8_t)(count);
        cfis[13] = (uint8_t)(count >> 8);
    } else {
        if (lba > 0x0FFFFFFFull || count > 255) return -2;
        cfis[2]  = is_write ? ATA_WRITE_DMA : ATA_READ_DMA;
        cfis[7]  = (uint8_t)(0xE0 | ((lba >> 24) & 0x0F));
        cfis[4]  = (uint8_t)(lba);
        cfis[5]  = (uint8_t)(lba >> 8);
        cfis[6]  = (uint8_t)(lba >> 16);
        cfis[12] = (uint8_t)(count);
    }

    int r = ahci_issue(p->base, p->pool, cfis, is_write, buf,
                       count * (uint32_t)BLK_SECTOR_SIZE,
                       is_write ? 8000u : 5000u);
    if (r != 0 || !is_write) return r;

    // FLUSH CACHE so the data is durable in the backing file even if
    // QEMU is killed — ata_write_sectors() does the same thing.
    memset(cfis, 0, sizeof(cfis));
    cfis[0] = 0x27;
    cfis[1] = 0x80;
    cfis[2] = p->lba48 ? ATA_FLUSH_CACHE_EXT : ATA_FLUSH_CACHE;
    return ahci_issue(p->base, p->pool, cfis, 0, NULL, 0, 8000);
}

// ---- blk layer trampolines ----------------------------------------
static int ahci_blk_read(void* ctx, uint64_t lba, uint32_t count, void* buf) {
    struct ahci_port* p = (struct ahci_port*)ctx;
    int r;
    task_sched_lock();          // no preemption mid-storage-operation
    r = ahci_rw(p, 0, lba, count, buf);
    task_sched_unlock();
    return r;
}

static int ahci_blk_write(void* ctx, uint64_t lba, uint32_t count,
                          const void* buf) {
    struct ahci_port* p = (struct ahci_port*)ctx;
    int r;
    task_sched_lock();
    r = ahci_rw(p, 1, lba, count, (void*)buf);
    task_sched_unlock();
    return r;
}

// ===================================================================
//  Bring-up
// ===================================================================

static const struct pci_dev* ahci_find_controller(void) {
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev* d = pci_get(i);
        if (!d) continue;
        // class 01 = mass storage, subclass 06 = SATA (AHCI).
        // NOTE: pci_dev.prog_if is read from the wrong dword by
        // pci.cpp (command/status, not class), so it is unusable for
        // matching — subclass 06 alone is the AHCI marker.
        if (d->class_code == 0x01 && d->subclass == 0x06) return d;
    }
    return NULL;
}

int ahci_init(void) {
    const struct pci_dev* hba = ahci_find_controller();
    if (!hba) return 0;                 // no AHCI: silent, like 0.4 Beta

    if (hba->bar_kind[5] < 2 || !(hba->bar[5] & 0xFFFFFFF0u)) {
        printf("ahci: %04x:%04x has no ABAR (BAR5) — controller ignored\n",
               hba->vendor, hba->device);
        return 0;
    }
    g_abar = (volatile uint8_t*)(uintptr_t)(hba->bar[5] & 0xFFFFFFF0u);

    // PCI command: I/O + MEM + bus master (QEMU leaves master off).
    uint32_t pcicmd = pci_cfg_read32(hba->bus, hba->dev, hba->fn, 0x04);
    pci_cfg_write32(hba->bus, hba->dev, hba->fn, 0x04, pcicmd | 0x0007u);

    // GHC.AHCI_ENABLE: without it the ports are in legacy IDE mode.
    if (!(mm_r32(g_abar + AHCI_HC_GHC) & AHCI_GHC_AE)) {
        for (int t = 0; t < 5; t++) {
            mm_w32(g_abar + AHCI_HC_GHC,
                   mm_r32(g_abar + AHCI_HC_GHC) | AHCI_GHC_AE);
            if (mm_r32(g_abar + AHCI_HC_GHC) & AHCI_GHC_AE) break;
        }
    }
    if (!(mm_r32(g_abar + AHCI_HC_GHC) & AHCI_GHC_AE)) {
        printf("ahci: GHC.AE could not be set — controller ignored\n");
        return 0;
    }

    uint32_t cap = mm_r32(g_abar + AHCI_HC_CAP);
    uint32_t pi  = mm_r32(g_abar + AHCI_HC_PI);
    uint32_t vs  = mm_r32(g_abar + AHCI_HC_VS);
    int nports = (int)((cap & 0x1Fu) + 1u);
    if (nports > AHCI_MAX_PORTS) nports = AHCI_MAX_PORTS;

    printf("ahci: %04x:%04x ABAR=0x%08x VS=%u.%u PI=0x%02x ports=%d\n",
           hba->vendor, hba->device, (uint32_t)(uintptr_t)g_abar,
           (unsigned)((vs >> 16) & 0xFFu), (unsigned)(vs & 0xFFu),
           (unsigned)pi, nports);

    int found = 0;
    for (int pn = 0; pn < nports; pn++) {
        if (!(pi & (1u << pn))) continue;
        volatile uint8_t* pb =
            g_abar + AHCI_PORT_BASE + pn * AHCI_PORT_SIZE;

        // 1. park the port — SeaBIOS usually leaves it running
        //    against DMA buffers that have since been reclaimed.
        (void)ahci_port_stop(pb);

        // 2. device present?
        if ((port_r32(pb, AHCI_P_SSTS) & AHCI_SSTS_DET_MASK) !=
            AHCI_SSTS_DET_UP)
            continue;

        // 3. packet device?  (CD over AHCI is deferred, see ahci.h)
        uint32_t sig = port_r32(pb, AHCI_P_SIG);
        if (sig == AHCI_SIG_ATAPI) {
            printf("SATA: ahci port %d ATAPI device (skipped)\n", pn);
            continue;
        }

        // 4. claim DMA memory + start the port
        int pool = ahci_pool_alloc();
        if (pool < 0) { printf("ahci: DMA pool exhausted\n"); break; }
        if (ahci_port_start(pb, &g_mem[pool]) != 0) {
            printf("SATA: ahci port %d will not start\n", pn);
            ahci_pool_free(pool);
            continue;
        }

        // 5. IDENTIFY
        uint16_t id[256];
        memset((uint8_t*)id, 0, sizeof(id));
        if (ahci_identify(pb, pool, id) != 0) {
            printf("SATA: ahci port %d IDENTIFY failed\n", pn);
            (void)ahci_port_stop(pb);
            ahci_pool_free(pool);
            continue;
        }

        struct ahci_port* p = &g_port[g_port_cnt];
        p->base = pb;
        p->pool = pool;
        p->no   = pn;

        uint32_t sectors28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
        uint16_t cmdsets    = id[83];
        // word 83 validity = bits 14:15 == 01, bit 10 = LBA48
        p->lba48 = ((cmdsets & 0xC000u) == 0x4000u &&
                    (cmdsets & 0x0400u)) ? 1 : 0;
        if (p->lba48) {
            p->total_sectors = (uint64_t)id[100] |
                               ((uint64_t)id[101] << 16) |
                               ((uint64_t)id[102] << 32) |
                               ((uint64_t)id[103] << 48);
        } else {
            p->total_sectors = sectors28;
        }
        if (p->total_sectors == 0) {
            printf("SATA: ahci port %d reports no capacity\n", pn);
            (void)ahci_port_stop(pb);
            ahci_pool_free(pool);
            continue;
        }

        // 6. hand it to the block layer (slots 4..7)
        int slot = BLK_PATA_SLOTS + g_disk_cnt;
        struct blk_desc* b = blk_at(slot);
        if (!b || slot >= BLK_MAX_DRIVES) {
            printf("ahci: disk on port %d has no free blk slot\n", pn);
            (void)ahci_port_stop(pb);
            ahci_pool_free(pool);
            break;
        }
        memset((uint8_t*)b, 0, sizeof(*b));
        b->present = 1;
        b->kind = BLK_AHCI;
        b->lba48 = p->lba48;
        b->total_sectors = p->total_sectors;
        snprintf(b->name, sizeof(b->name), "ahci%d", g_disk_cnt);
        snprintf(b->bus,  sizeof(b->bus),  "ahci port %d", pn);
        ahci_model_from_words(id, b->model, sizeof(b->model));
        b->ctx   = p;
        b->read  = ahci_blk_read;
        b->write = ahci_blk_write;

        g_port_cnt++;
        g_disk_cnt++;
        found++;
    }
    return found;
}
