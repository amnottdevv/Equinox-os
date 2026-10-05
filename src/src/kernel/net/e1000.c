// ============================================================
//  e1000.c — Intel PRO/1000 (82540EM/82545EM) PCI driver
// ------------------------------------------------------------
//  Model (identical to ne2000.c — see net.h):
//    * probe()  maps BAR0 (MMIO, identity-mapped like the VESA
//               framebuffer), enables the PCI command bits and
//               brings RX/TX rings up.
//    * the ISR  acks ICR (read-to-clear) + net_rxr_drain_isr()
//               + EOI — lwIP is never touched from IRQ context.
//    * recv()   hands one completed RX descriptor back to the
//               caller and returns its descriptor to the NIC
//               (RDT = the index just consumed, as Linux does).
//    * send()   waits for the descriptor DD flag before returning
//               so the frame buffer can be released by the stack.
//
//  QEMU: -device e1000,netdev=net0   (8086:100E, INTx, IRQ 11)
//
//  Compiled with g++ (makefile .c rule) — extern "C" exports.
// ============================================================
#include "e1000.h"
#include "net.h"
#include "nic.h"

#include <stdint.h>
#include <stddef.h>

#include "library/header/stdio.h"     /* printf, inb/outb          */
#include "library/header/libstring.h" /* memcpy/memset             */
#include "library/header/pci.h"
#include "library/header/idt.h"       /* idt_register_irq          */
#include "library/header/timer.h"     /* get_tick (ms timeouts)    */

// ---- register map (byte offset from BAR0) ---------------------
#define E1000_CTRL      0x00000
#define E1000_STATUS    0x00008
#define E1000_EERD      0x00014
#define E1000_ICR       0x000C0   /* RW1C — a read ACKs          */
#define E1000_IMS       0x000D0
#define E1000_IMC       0x000D8
#define E1000_RCTL      0x00100
#define E1000_TCTL      0x00400
#define E1000_TIPG      0x00410
#define E1000_RDBAL     0x02800
#define E1000_RDBAH     0x02804
#define E1000_RDLEN     0x02808
#define E1000_RDH       0x02810
#define E1000_RDT       0x02818
#define E1000_TDBAL     0x03800
#define E1000_TDBAH     0x03804
#define E1000_TDLEN     0x03808
#define E1000_TDH       0x03810
#define E1000_TDT       0x03818
#define E1000_MTA       0x05200   /* 128 dwords                  */
#define E1000_RAL       0x05400
#define E1000_RAH       0x05404

#define E1000_CTRL_SLU      (1u << 6)
#define E1000_CTRL_ASDE     (1u << 5)
#define E1000_CTRL_RST      (1u << 26)
#define E1000_STATUS_LINK   (1u << 2)

#define E1000_RCTL_EN      (1u << 1)
#define E1000_RCTL_UPE     (1u << 3)   /* accept all unicast      */
#define E1000_RCTL_MPE     (1u << 4)   /* accept all multicast    */
#define E1000_RCTL_BAM     (1u << 15)  /* accept broadcast        */
#define E1000_RCTL_SECRC   (1u << 26)  /* strip the FCS           */

#define E1000_TCTL_EN      (1u << 1)
#define E1000_TCTL_PSP     (1u << 3)

/* ICR/IMS bits we care about */
#define E1000_IC_TXDW   (1u << 0)
#define E1000_IC_LSC    (1u << 2)
#define E1000_IC_RXDMT0 (1u << 4)
#define E1000_IC_RXO    (1u << 5)
#define E1000_IC_RXT0   (1u << 7)
#define E1000_IC_TXQE   (1u << 1)

/* TX descriptor command bits */
#define E1000_TXD_CMD_EOP  0x01
#define E1000_TXD_CMD_IFCS 0x02
#define E1000_TXD_CMD_RS   0x08
#define E1000_TXD_DD       0x01

#define E1000_RXD_DD       0x01

// ---- rings ----------------------------------------------------
#define E1000_RXN    32          /* 32 * 16 B = 512 B (128-aligned) */
#define E1000_TXN    8           /*  8 * 16 B = 128 B               */
#define E1000_RXBUF  2048        /* RCTL.BSIZE = 00 -> 2048 B       */
#define E1000_MAXF   1518

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

/* TX descriptor = TEPAT 16 B (stride harian NIC, TDLEN = n*16):
 * [0..7] addr  [8..9] length  [10] cso  [11] cmd  [12] status
 * [13] css  [14..15] special.  Versi lama 17 B + cmd/status salah
 * posisi -> TDLEN 136 (harus kelipatan 128) dan NIC membaca
 * descriptor sebagai sampah: TDT tak pernah diproses, bit DD tak
 * pernah muncul, kirim mandek. */
struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));
typedef char e1000_tx_desc_must_be_16_bytes
        [(sizeof(struct e1000_tx_desc) == 16) ? 1 : -1];

/* DMA memory: BSS (not stored in the ELF), 16-byte aligned. */
static struct e1000_rx_desc rx_desc[E1000_RXN] __attribute__((aligned(16)));
static struct e1000_tx_desc tx_desc[E1000_TXN] __attribute__((aligned(16)));
static uint8_t rx_buf[E1000_RXN][E1000_RXBUF] __attribute__((aligned(16)));
/* Bounce buffer PER DESCRIPTOR: kirim jadi asinkron (lihat e1000_send)
 * — data paket harus tetap hidup sampai NIC selesai membacanya. */
static uint8_t tx_buf[E1000_TXN][E1000_MAXF] __attribute__((aligned(16)));
/* Batas putaran tunggu (bukan get_tick!): nic_send() sering dipanggil
 * dengan interupsi mati (net_lock() = penjaga cli di net_init/lwIP),
 * sehingga timeout berbasis tick tidak akan pernah jatuh -> deadlock
 * boot yang diamati sebagai "hang setelah nic: e1000-pci ...". */
#define E1000_TX_SPIN  1000000u

static volatile uint8_t* e_mmio;
static int      e_present;
static int      e_irq      = -1;
static uint8_t  e_mac[6];
static uint32_t e_irqs, e_ovf, e_txto;
static uint32_t e_rxh;          /* shadow: next descriptor to consume */
static uint32_t e_txt;          /* shadow: next descriptor to fill    */

// ---- MMIO -----------------------------------------------------
static inline uint32_t er32(uint32_t off) {
    return *(volatile uint32_t*)(e_mmio + off);
}
static inline void ew32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(e_mmio + off) = v;
}
/* compiler/CPU ordering: descriptor written by us -> read by the NIC */
static inline void e_wmb(void) { asm volatile("mfence" ::: "memory"); }
static inline void e_rmb(void) { asm volatile("mfence" ::: "memory"); }

// ---- EEPROM ---------------------------------------------------
/* EERD: START=bit0, DONE=bit1, ADDR=bits15..2, DATA=bits31..16. */
static uint16_t e1000_eeprom_word(int word) {
    ew32(E1000_EERD, ((uint32_t)word << 2) | 1u);
    uint32_t t0 = get_tick();
    do {
        uint32_t v = er32(E1000_EERD);
        if (v & 0x2) return (uint16_t)(v >> 16);   /* DONE = bit1, bukan bit2 */
    } while (get_tick() - t0 < 10);
    return 0;
}

// ---- PIC unmask (same dance ne2000 does, parameterized) -------
static void e1000_unmask_pic(int irq) {
    if (irq < 0 || irq > 15) return;
    if (irq >= 8) {
        uint8_t sm = inb(0xA1);
        sm &= (uint8_t)~(1 << (irq - 8));
        outb(0xA1, sm);
        uint8_t mm = inb(0x21);
        mm &= (uint8_t)~(1 << 2);      /* master: cascade IRQ2  */
        outb(0x21, mm);
    } else {
        uint8_t mm = inb(0x21);
        mm &= (uint8_t)~(1 << irq);
        outb(0x21, mm);
    }
}

// ---- ISR ------------------------------------------------------
/* Level-triggered INTx: reading ICR is the acknowledge. EOI only
 * after the device is quiet, or the line re-fires forever. */
extern "C" __attribute__((interrupt)) void e1000_isr(void* frame) {
    (void)frame;
    if (e_mmio) {
        uint32_t icr = er32(E1000_ICR);     /* read clears (RW1C) */
        (void)icr;
        e_irqs++;
        net_rxr_drain_isr();                /* frames -> RX ring   */
    }
    if (e_irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

// ---- bring-up -------------------------------------------------
static int e1000_start(void) {
    /* interrupts off while the rings are wired up */
    ew32(E1000_IMC, 0xFFFFFFFFu);
    (void)er32(E1000_ICR);

    /* link auto-detect + auto-negotiation complete */
    uint32_t ctrl = er32(E1000_CTRL);
    ctrl &= ~E1000_CTRL_RST;
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ew32(E1000_CTRL, ctrl);

    /* stop both engines */
    ew32(E1000_RCTL, 0);
    ew32(E1000_TCTL, 0);

    /* ---- RX ring ---- */
    memset(rx_desc, 0, sizeof(rx_desc));
    for (int i = 0; i < E1000_RXN; i++)
        rx_desc[i].addr = (uint32_t)(uintptr_t)rx_buf[i];
    e_wmb();
    ew32(E1000_RDBAL, (uint32_t)(uintptr_t)rx_desc);
    ew32(E1000_RDBAH, 0);
    ew32(E1000_RDLEN, sizeof(rx_desc));
    ew32(E1000_RDH, 0);
    e_rxh = 0;
    /* HW owns [RDH, RDT): stops when RDH == RDT -> one descriptor
     * short of the ring, which is exactly what Linux programs. */
    ew32(E1000_RDT, E1000_RXN - 1);
    ew32(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
                     E1000_RCTL_UPE | E1000_RCTL_MPE |
                     E1000_RCTL_SECRC);

    /* ---- TX ring ---- */
    memset(tx_desc, 0, sizeof(tx_desc));
    e_wmb();
    ew32(E1000_TDBAL, (uint32_t)(uintptr_t)tx_desc);
    ew32(E1000_TDBAH, 0);
    ew32(E1000_TDLEN, sizeof(tx_desc));
    ew32(E1000_TDH, 0);
    e_txt = 0;
    ew32(E1000_TDT, 0);
    /* EN | PSP, collision timing 15, collision distance 64 */
    ew32(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                     (15u << 4) | (64u << 16));
    ew32(E1000_TIPG, 10u | (8u << 10) | (6u << 20));

    /* ---- unicast filter: our MAC (RAH.AV set) ---- */
    uint32_t lo = (uint32_t)e_mac[0]        | ((uint32_t)e_mac[1] << 8) |
                  ((uint32_t)e_mac[2] << 16)| ((uint32_t)e_mac[3] << 24);
    uint32_t hi = (uint32_t)e_mac[4]        | ((uint32_t)e_mac[5] << 8);
    ew32(E1000_RAL, lo);
    ew32(E1000_RAH, hi | 0x80000000u);
    for (int i = 0; i < 128; i++)          /* multicast hash: accept all */
        ew32(E1000_MTA + (uint32_t)i * 4, 0xFFFFFFFFu);

    /* ---- interrupts + PIC line ---- */
    ew32(E1000_IMS, E1000_IC_RXT0 | E1000_IC_RXDMT0 | E1000_IC_RXO |
                    E1000_IC_LSC  | E1000_IC_TXDW);
    e1000_unmask_pic(e_irq);
    idt_register_irq(e_irq, e1000_isr);

    return 1;
}

int e1000_probe(void) {
    e_present = 0;
    e_mmio    = 0;

    const struct pci_dev* d = pci_find(E1000_VENDOR, E1000_DEV_82540);
    if (!d) d = pci_find(E1000_VENDOR, E1000_DEV_82545);
    if (!d) return 0;

    if (d->bar_kind[0] != 2) {
        printf("e1000: BAR0 is not a 32-bit MEM BAR — cannot map\n");
        return 0;
    }
    e_mmio = (volatile uint8_t*)(uintptr_t)(d->bar[0] & 0xFFFFFFF0u);
    e_irq  = d->irq_line;

    /* PCI command: I/O + MEM + bus master (QEMU leaves master off) */
    uint32_t cmd = pci_cfg_read32(d->bus, d->dev, d->fn, 0x04);
    pci_cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd | 0x0007u);

    uint32_t status = er32(E1000_STATUS);
    if (status == 0xFFFFFFFFu || status == 0) {
        printf("e1000: MMIO at 0x%08x does not answer\n",
               (uint32_t)(uintptr_t)e_mmio);
        e_mmio = 0;
        return 0;
    }

    /* MAC: EEPROM words 0..2 first (the datasheet layout), then the
     * RAL/RAH the firmware left behind, then QEMU's default. */
    uint16_t w0 = e1000_eeprom_word(0);
    uint16_t w1 = e1000_eeprom_word(1);
    uint16_t w2 = e1000_eeprom_word(2);
    e_mac[0] = (uint8_t)w0;        e_mac[1] = (uint8_t)(w0 >> 8);
    e_mac[2] = (uint8_t)w1;        e_mac[3] = (uint8_t)(w1 >> 8);
    e_mac[4] = (uint8_t)w2;        e_mac[5] = (uint8_t)(w2 >> 8);
    if (e_mac[0] & 0x01 || e_mac[0] == 0 || e_mac[5] == 0) {
        uint32_t ral = er32(E1000_RAL), rah = er32(E1000_RAH);
        e_mac[0] = (uint8_t)ral;        e_mac[1] = (uint8_t)(ral >> 8);
        e_mac[2] = (uint8_t)(ral >> 16); e_mac[3] = (uint8_t)(ral >> 24);
        e_mac[4] = (uint8_t)rah;        e_mac[5] = (uint8_t)(rah >> 8);
    }
    if ((e_mac[0] & 0x01) || e_mac[0] == 0 || e_mac[5] == 0) {
        static const uint8_t dflt[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        memcpy(e_mac, dflt, 6);
    }

    if (!e1000_start()) return 0;

    e_present = 1;
    e_irqs = 0;
    e_ovf  = 0;
    printf("nic: e1000-pci at 0x%08x irq %d mac %02x:%02x:%02x:%02x:%02x:%02x "
           "link %s\n",
           (uint32_t)(uintptr_t)e_mmio, e_irq,
           e_mac[0], e_mac[1], e_mac[2], e_mac[3], e_mac[4], e_mac[5],
           (er32(E1000_STATUS) & E1000_STATUS_LINK) ? "up" : "down");
    return 1;
}

// ---- nic_driver hooks -----------------------------------------
int e1000_send(const uint8_t* frame, int len) {
    if (!e_present || !frame) return -1;
    if (len > E1000_MAXF || len <= 0) return -1;

    uint32_t tail = e_txt;
    uint32_t next = (tail + 1u) % E1000_TXN;
    if (next == er32(E1000_TDH)) {                /* ring full     */
        uint32_t i;
        for (i = 0; i < E1000_TX_SPIN && next == er32(E1000_TDH); i++)
            asm volatile("pause" ::: "memory");
        if (next == er32(E1000_TDH)) { e_txto++; return -1; }
    }

    /* cadangan milik driver — aman sampai TDH melewati descriptor ini
     * (pemeriksaan ring-full di atas menjamin itu). */
    uint8_t* buf = tx_buf[tail];
    memcpy(buf, frame, (size_t)len);
    int tlen = len;
    if (tlen < 60) {                               /* Ethernet pad  */
        memset(buf + tlen, 0, (size_t)(60 - tlen));
        tlen = 60;
    }

    struct e1000_tx_desc* d = &tx_desc[tail];
    d->addr    = (uint32_t)(uintptr_t)buf;
    d->length  = (uint16_t)tlen;
    d->cso     = 0;
    d->css     = 0;
    d->special = 0;
    d->status  = 0;
    d->cmd     = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    e_wmb();
    e_txt = next;
    ew32(E1000_TDT, next);
    /* ASINKRON: tanpa menunggu bit DD. Menunggu DD di sini pernah
     * membekukan boot — pemanggilannya sering terjadi di bawah cli
     * (net_lock), di mana get_tick() tak pernah maju. */
    return 0;
}

int e1000_recv(uint8_t* frame, int maxlen) {
    if (!e_present || !frame) return 0;

    struct e1000_rx_desc* d = &rx_desc[e_rxh];
    if (!(d->status & E1000_RXD_DD)) return 0;       /* nothing new   */
    e_rmb();

    uint16_t len = d->length;
    if (len == 0 || len > E1000_RXBUF) {             /* corrupt       */
        e_ovf++;
    } else {
        int n = (int)len > maxlen ? maxlen : (int)len;
        memcpy(frame, rx_buf[e_rxh], (size_t)n);
        d->status = 0;                                /* hand back     */
        e_wmb();
        e_rxh = (e_rxh + 1u) % E1000_RXN;
        ew32(E1000_RDT, (e_rxh + E1000_RXN - 1u) % E1000_RXN);
        return n;
    }

    /* error path: still recycle the descriptor */
    d->status = 0;
    e_wmb();
    e_rxh = (e_rxh + 1u) % E1000_RXN;
    ew32(E1000_RDT, (e_rxh + E1000_RXN - 1u) % E1000_RXN);
    return 0;
}

const uint8_t* e1000_mac(void) { return e_mac; }
uint32_t e1000_irq_count(void)  { return e_irqs; }
uint32_t e1000_rx_overflow(void) { return e_ovf; }
