// ============================================================
//  ne2000.c — NE2000 ISA driver for Equinox OS (v10.11)
// ------------------------------------------------------------
//  Programming-model reference: NS DP8390 datasheet + the classic
//  NE2000 driver patterns (Linux drivers/net/ethernet/8390).
//  All packet transfers use REMOTE DMA through the data port
//  (byte-wide, DCR=0x48) — deliberately NOT using the ISA DMA
//  channel, keeping the driver portable and simple.
//
//  The RX ring is probed 8KB vs 16KB at init (write-read pattern
//  on the last page) — safe for QEMU and physical cards.
//
//  This file is compiled by the Equinox OS makefile .c rule with the
//  C++ compiler (g++), so: no C99 features missing from C++
//  (designated initializers, implicit void* casts); exports use
//  extern "C".
// ============================================================
#include "net.h"
#include "ne2000.h"

#include <stdint.h>
#include <stddef.h>

// Console + port I/O + kernel timer (all extern "C" in headers)
#include "library/header/stdio.h"
#include "library/header/libstring.h"

// ---------------- Register map (offset from NE_IOBASE) -----
#define NE_CR      0x00   /* command register, all pages     */
#define NE_PSTART  0x01   /* p0 w: ring start page            */
#define NE_PSTOP   0x02   /* p0 w: ring stop page             */
#define NE_BNRY    0x03   /* p0 r/w: boundary (last read)     */
#define NE_TPSR    0x04   /* p0 w: TX start page              */
#define NE_TBCR0   0x05   /* p0 w: TX byte count lo           */
#define NE_TBCR1   0x06   /* p0 w: TX byte count hi           */
#define NE_ISR     0x07   /* p0 r/w: interrupt status (ack via write-back) */
#define NE_CURR    0x07   /* p1 r/w: current write page       */
#define NE_PAR0    0x01   /* p1 r/w: MAC byte 0..5 (PAR0..PAR5) */
#define NE_MAR0    0x08   /* p1 w: multicast hash, 8 bytes    */
#define NE_RSAR0   0x08   /* p0 w: remote DMA start addr lo   */
#define NE_RSAR1   0x09   /* p0 w: remote DMA start addr hi   */
#define NE_RBCR0   0x0A   /* p0 w: remote DMA byte count lo   */
#define NE_RBCR1   0x0B   /* p0 w: remote DMA byte count hi   */
#define NE_RCR     0x0C   /* p0 w: RX config                  */
#define NE_TCR     0x0D   /* p0 w: TX config                  */
#define NE_DCR     0x0E   /* p0 w: data config                */
#define NE_IMR     0x0F   /* p0 w: interrupt mask             */
#define NE_DATA    0x10   /* remote DMA data port (word I/O)  */
#define NE_RESET   0x1F   /* reset port (read then write)    */

// CR bits
#define CR_STP     0x01   /* stop the chip                    */
#define CR_STA     0x02   /* start the chip                   */
#define CR_TXP     0x04   /* transmit packet                  */
#define CR_RD0     0x08   /* RDMA1:RDMA0 = 01 -> remote READ  */
#define CR_RD1     0x10   /* RDMA1:RDMA0 = 10 -> remote WRITE */
#define CR_PS0     0x20   /* page 0 select                    */
#define CR_PS1     0x40   /* page 1 select                    */

// ISR / IMR bits
#define ISR_PRX     0x01  /* packet received ok               */
#define ISR_PTX     0x02  /* packet transmitted ok            */
#define ISR_RXE     0x04  /* receive error                    */
#define ISR_TXE     0x08  /* transmit error                   */
#define ISR_OVW     0x10  /* receive buffer overflow           */
#define ISR_RDC     0x40  /* remote DMA complete              */
#define ISR_RST     0x80  /* reset status                     */

// ---------------- state ------------------------------------
static uint16_t ne_io      = NE_IOBASE;
static uint8_t  ne_mac[6]  = {0, 0, 0, 0, 0, 0};
static uint8_t  ne_pstart  = NE_RING_MIN;
static uint8_t  ne_pstop   = 0x80;     /* probed: 0x80 (16KB) or 0x60 (8KB) */
static int      ne_present = 0;
static volatile int      ne_ovw    = 0;   /* ring overflow -> recovered in recv */
static volatile uint32_t ne_irqs   = 0;
static volatile uint32_t ne_ovwcnt = 0;

// Scratch TX buffer, 16-byte aligned so word I/O stays safe.
static uint8_t ne_txbuf[NE_MAXFRAME] __attribute__((aligned(16)));

// ---------------- primitive I/O ----------------------------
static inline void ne_out(uint8_t reg, uint8_t val) {
    outb(ne_io + reg, val);
}
static inline uint8_t ne_in(uint8_t reg) {
    return inb(ne_io + reg);
}

// Bounded busy-wait — DO NOT use get_tick(): this function can
// be called from a context with IF=0 (the IRQ0 poll), where
// tick_counter cannot advance.
static void ne_spin(int n) {
    for (volatile int i = 0; i < n; i++) { /* pause */ }
}

// ---------------- remote DMA -------------------------------
// Read 'len' bytes from card memory 'addr' (may be odd) into
// 'dst' using a remote READ + the data port. (Byte-wide mode
// since v10.11 final, so odd alignment is a non-issue.)
static void ne_read_mem(uint8_t* dst, uint32_t addr, int len) {
    if (len <= 0) return;

    int      odd    = addr & 1;
    uint32_t start  = addr & ~1u;
    int      total  = len + odd;
    if (total & 1) total++;                     /* round up    */

    (void)odd; (void)start; (void)total;
    ne_out(NE_RSAR0, (uint8_t)(addr & 0xFF));
    ne_out(NE_RSAR1, (uint8_t)(addr >> 8));
    ne_out(NE_RBCR0, (uint8_t)(len & 0xFF));
    ne_out(NE_RBCR1, (uint8_t)(len >> 8));
    ne_out(NE_CR, CR_STA | CR_PS0 | CR_RD0);    /* remote read   */

    /* BYTE-wide (v10.11 final): DCR=0x48. In word mode QEMU's
     * ne2k duplicates bytes on an inw from the data port (proven
     * via pcap: the MAC read back as 52:52:54:54:00:00). Byte
     * mode: 1 inb = 1 byte, no odd-alignment trouble. Classic
     * 8390 pattern: read the data port DIRECTLY (the card holds
     * IOCHRDY), then ack RDC. */
    for (int i = 0; i < len; i++) {
        dst[i] = inb(ne_io + NE_DATA);
    }

    int guard = 0;
    while (!(ne_in(NE_ISR) & ISR_RDC)) {
        if (++guard > 10000) break;
    }
    ne_out(NE_ISR, ISR_RDC);                    /* ack RDC       */
}

// Write 'len' bytes to card memory 'addr' (always even for our
// callers: TX always starts on a page boundary).
static void ne_write_mem(const uint8_t* src, uint32_t addr, int len) {
    if (len <= 0) return;

    ne_out(NE_RSAR0, (uint8_t)(addr & 0xFF));
    ne_out(NE_RSAR1, (uint8_t)(addr >> 8));
    ne_out(NE_RBCR0, (uint8_t)(len & 0xFF));
    ne_out(NE_RBCR1, (uint8_t)(len >> 8));
    ne_out(NE_CR, CR_STA | CR_PS0 | CR_RD1);    /* remote write  */

    /* BYTE-wide (v10.11 final) — see the note in ne_read_mem. */
    for (int i = 0; i < len; i++) {
        outb(ne_io + NE_DATA, src[i]);
    }

    int guard = 0;
    while (!(ne_in(NE_ISR) & ISR_RDC)) {
        if (++guard > 10000) break;
    }
    ne_out(NE_ISR, ISR_RDC);
}

// ---------------- ring management --------------------------
// Reset the ring pointers (used by init + overflow recovery).
static void ne_ring_reset(void) {
    ne_out(NE_CR, CR_STP | CR_PS0);
    ne_out(NE_PSTART, ne_pstart);
    ne_out(NE_PSTOP,  ne_pstop);
    ne_out(NE_BNRY,   ne_pstart);      /* BNRY = CURR-1: the ring is empty
                                          when CURR = PSTART+1 (see the
                                          DP8390 datasheet: next = BNRY+1, wrapping) */

    ne_out(NE_CR, CR_STP | CR_PS1);
    ne_out(NE_CURR, ne_pstart + 1);    /* CURR = next write page */

    ne_out(NE_CR, CR_STP | CR_PS0);
    ne_out(NE_ISR, 0xFF);              /* clear all interrupts    */
}

// Ring-overflow recovery (pattern from the Linux 8390 driver):
// stop, reset the pointers, start. Overwritten packets are lost.
static void ne_recover_overflow(void) {
    ne_ovwcnt++;
    ne_out(NE_CR, CR_STP | CR_PS0);
    ne_spin(2000);

    /* abort any remote DMA still in flight */
    ne_out(NE_RBCR0, 0);
    ne_out(NE_RBCR1, 0);

    ne_ring_reset();
    ne_out(NE_CR, CR_STA | CR_PS0);
    ne_out(NE_TCR, 0x00);              /* normal TX              */
    ne_out(NE_RCR, 0x04);              /* accept bcast + unicast */
    ne_out(NE_IMR, ISR_PRX | ISR_OVW);
    ne_ovw = 0;
}

// ---------------- public API -------------------------------
const uint8_t* ne2000_mac(void)   { return ne_mac; }
uint32_t ne2000_irq_count(void)   { return ne_irqs; }
uint32_t ne2000_rx_overflow(void) { return ne_ovwcnt; }

int ne2000_init(void) {
    ne_present = 0;

    /* 1. Soft reset (read then write the reset port), wait for it
     *    to stabilize */
    uint8_t r = ne_in(NE_RESET);
    ne_out(NE_RESET, r);
    ne_spin(100000);

    /* 2. Probe: request STOP + page0, read CR back. Without a
     *    card, the ISA bus floats (0x00/0xFF) -> the STA/STP bits
     *    won't match what we wrote. */
    ne_out(NE_CR, CR_STP | CR_PS0);
    uint8_t cr = ne_in(NE_CR) & 0x03;
    if (cr != CR_STP) {
        return 0;                     /* no NE2000 here          */
    }

    /* 3. Basic configuration (still in STOP) */
    ne_out(NE_DCR,   0x48);           /* BYTE-wide DMA (QEMU ne2k:
                                      inw on the data port duplicates
                                      bytes in word mode — see the
                                      v10.11 net.pcap forensics) */
    ne_out(NE_RBCR0, 0);
    ne_out(NE_RBCR1, 0);
    ne_out(NE_RCR,   0x20);           /* monitor mode for now    */
    ne_out(NE_TCR,   0x02);           /* internal loopback init  */

    /* 4. Probe the RAM size: write a pattern at page 0x7F.
     *    16KB card: pattern reads back -> PSTOP 0x80.
     *    8KB  card: fails             -> PSTOP 0x60.             */
    uint8_t probe[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t back[4]  = {0, 0, 0, 0};
    ne_pstart = NE_RING_MIN;
    ne_pstop  = 0x60;                 /* assume 8KB for now     */
    ne_ring_reset();
    ne_out(NE_CR, CR_STA | CR_PS0);
    ne_write_mem(probe, (0x7F << 8), 4);
    ne_read_mem(back, (0x7F << 8), 4);
    if (back[0] == 0xDE && back[1] == 0xAD &&
        back[2] == 0xBE && back[3] == 0xEF) {
        ne_pstop = 0x80;              /* 16KB RAM                */
    }

    /* 5. Read the MAC from the card PROM. QEMU's ne2k presents
     *    the PROM in word-mode layout: EVERY BYTE DOUBLED (netdbg
     *    forensics: 52 52 54 54 00 00 12 12 34 34 56 56). The
     *    standard classic-driver fix: read 2n bytes, keep the
     *    even ones. */
    static uint8_t prom[16];
    ne_read_mem(prom, 0x0000, 16);
    for (int i = 0; i < 6; i++) {
        ne_mac[i] = prom[2 * i];
    }
    if ((ne_mac[0] & 0x80) || (ne_mac[0] == 0xFF && ne_mac[1] == 0xFF)) {
        /* MAC clearly invalid -> probably not an ne2000     */
        return 0;
    }

    /* 6. Set ring final + page1 registers (PAR = MAC, MAR = all) */
    ne_ring_reset();

    ne_out(NE_CR, CR_STP | CR_PS1);
    for (int i = 0; i < 6; i++) {
        ne_out(NE_PAR0 + (uint8_t)i, ne_mac[i]);
    }
    for (int i = 0; i < 8; i++) {
        ne_out(NE_MAR0 + (uint8_t)i, 0xFF);
    }

    /* 7. Start + normal operating mode */
    ne_out(NE_CR, CR_STA | CR_PS0);
    ne_out(NE_TCR, 0x00);             /* TX normal + CRC on      */
    ne_out(NE_RCR, 0x04);             /* accept broadcast+uni    */
    ne_out(NE_TPSR, NE_TXPAGE);
    ne_out(NE_ISR, 0xFF);
    ne_out(NE_IMR, ISR_PRX | ISR_OVW);

    /* 8. Unmask IRQ9 (slave PIC bit 1) + make sure the master's
     *    IRQ2 cascade is open (mouse_init already opened it, but
     *    don't depend on init order). */
    uint8_t smask = inb(0xA1);
    smask &= (uint8_t)~(1 << 1);
    outb(0xA1, smask);
    uint8_t mmask = inb(0x21);
    mmask &= (uint8_t)~(1 << 2);
    outb(0x21, mmask);

    ne_present = 1;
    ne_ovw = 0;
    return 1;
}

int ne2000_send(const uint8_t* frame, int len) {
    if (!ne_present) return -1;
    if (len > NE_MAXFRAME) return -1;
    if (len < 60) {                   /* pad Ethernet minimum     */
        memcpy(ne_txbuf, frame, (size_t)len);
        memset(ne_txbuf + len, 0, (size_t)(60 - len));
        len = 60;
        frame = ne_txbuf;
    }

    /* Copy into the aligned scratch when the caller does not
     * guarantee word alignment (lwIP pbufs may be odd). */
    if (((uintptr_t)frame & 1) && frame != ne_txbuf) {
        memcpy(ne_txbuf, frame, (size_t)len);
        frame = ne_txbuf;
    }

    ne_write_mem(frame, (uint32_t)NE_TXPAGE << 8, len);

    ne_out(NE_TPSR,  NE_TXPAGE);
    ne_out(NE_TBCR0, (uint8_t)(len & 0xFF));
    ne_out(NE_TBCR1, (uint8_t)(len >> 8));
    ne_out(NE_CR, CR_STA | CR_PS0 | CR_TXP);   /* GO!             */

    /* TXP self-clears when done (QEMU + real HW). */
    int guard = 0;
    while (ne_in(NE_CR) & CR_TXP) {
        if (++guard > 800000) {
            ne_out(NE_ISR, ISR_PTX | ISR_TXE);
            return -1;
        }
    }
    ne_out(NE_ISR, ISR_PTX | ISR_TXE);
    return 0;
}

int ne2000_recv(uint8_t* frame, int maxlen) {
    if (!ne_present) return 0;

    /* Overflow recovery is deferred until here (safe context) */
    if (ne_ovw) {
        ne_recover_overflow();
        return -1;                    /* this frame is dropped   */
    }

    /* BNRY (page0) + CURR (page1) */
    ne_out(NE_CR, CR_STA | CR_PS0);
    uint8_t bnry = ne_in(NE_BNRY);
    ne_out(NE_CR, CR_STA | CR_PS1);
    uint8_t curr = ne_in(NE_CURR);
    ne_out(NE_CR, CR_STA | CR_PS0);

    uint8_t next = (uint8_t)(bnry + 1);
    if (next >= ne_pstop) next = ne_pstart;
    if (next == curr) return 0;      /* ring empty              */

    /* 4-byte packet header: status, next-page, count (LE, incl.
     * the 4-byte header + data + the 4-byte CRC). */
    uint8_t hdr[4];
    ne_read_mem(hdr, (uint32_t)next << 8, 4);

    uint8_t status = hdr[0];
    uint8_t next2  = hdr[1];
    int     count  = hdr[2] | (hdr[3] << 8);

    /* Validate the header — if it looks wrong, the ring is
     * corrupt: reset it.
     * QEMU NOTE: count = 4-byte header + frame length (WITHOUT
     * the CRC — unlike a real DP8390, which includes it). A
     * minimal ethernet frame is 60 bytes -> count is at least 64. */
    if (next2 < ne_pstart || next2 >= ne_pstop ||
        count < 64 || count > NE_MAXFRAME + 4) {
        ne_ring_reset();
        ne_out(NE_CR, CR_STA | CR_PS0);
        return -1;
    }
    if (!(status & 0x01)) {          /* PRX=0: CRC/runt error    */
        /* still advance BNRY so the ring keeps moving */
        ne_out(NE_BNRY, (uint8_t)(next2 == ne_pstart ? ne_pstop - 1
                                                     : next2 - 1));
        return -1;
    }

    int flen = count - 4;            /* drop the 4-byte header (QEMU excludes the CRC) */
    if (flen > maxlen) flen = maxlen;

    /* The data may wrap at the end of the ring -> two reads.
     * IMPORTANT: the on-card packet layout is [4-byte header]
     * [data] — data starts at (page<<8)+4, NOT page<<8 (a v10.11
     * draft bug: frames came out shifted by 4, garbling the
     * ethertype). */
    uint32_t addr  = ((uint32_t)next << 8) + 4;
    int      avail = ((int)ne_pstop << 8) - (int)addr;
    if (avail > flen) avail = flen;
    ne_read_mem(frame, addr, avail);
    if (flen > avail) {
        ne_read_mem(frame + avail, (uint32_t)ne_pstart << 8,
                    flen - avail);
    }

    /* Advance boundary */
    ne_out(NE_BNRY, (uint8_t)(next2 == ne_pstart ? ne_pstop - 1
                                                 : next2 - 1));
    return flen;
}

// ---------------- debug forensik ---------------------------
extern "C" void ne2000_dbg_probe(void) {
    if (!ne_present) {
        printf("netdbg: no card\n");
        return;
    }
    static uint8_t buf[32];

    uint8_t save_cr = ne_in(NE_CR);
    ne_out(NE_CR, CR_STA | 0x20);           /* page0 + nodma */

    /* 1. PROM 16 byte dari address 0 */
    ne_read_mem(buf, 0x0000, 16);
    printf("PROM[0..15]:");
    for (int i = 0; i < 16; i++) printf(" %02x", buf[i]);
    printf("\n");

    /* 2. round-trip: tulis pattern ke 0x5000, baca balik */
    uint8_t pat[8] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    uint8_t bak[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    ne_write_mem(pat, 0x5000, 8);
    ne_read_mem(bak, 0x5000, 8);
    printf("RAM[0x5000] w:");
    for (int i = 0; i < 8; i++) printf(" %02x", pat[i]);
    printf(" r:");
    for (int i = 0; i < 8; i++) printf(" %02x", bak[i]);
    printf("\n");

    /* 3. baca dari offset ganjil */
    ne_read_mem(bak, 0x0001, 6);
    printf("PROM[1..6]  :");
    for (int i = 0; i < 6; i++) printf(" %02x", bak[i]);
    printf("\n");

    /* 4. register echo test */
    ne_out(NE_RBCR0, 0xAA);
    ne_out(NE_RBCR1, 0xBB);
    printf("RBCR w=AA:BB r=%02x:%02x\n", ne_in(NE_RBCR0), ne_in(NE_RBCR1));

    /* 5. register config live readback */
    ne_out(NE_CR, CR_STA | 0x20);           /* page0 */
    printf("RCR=%02x TCR=%02x DCR=%02x IMR=%02x BNRY=%02x ISR=%02x CR=%02x\n",
           ne_in(0x0C), ne_in(0x0D), ne_in(0x0E), ne_in(0x0F),
           ne_in(NE_BNRY), ne_in(NE_ISR), ne_in(NE_CR));
    ne_out(NE_CR, CR_STA | 0x40);           /* page1 */
    printf("CURR=%02x PAR=%02x:%02x:%02x:%02x:%02x:%02x\n",
           ne_in(NE_CURR),
           ne_in(0x01), ne_in(0x02), ne_in(0x03), ne_in(0x04), ne_in(0x05),
           ne_in(0x06));
    ne_out(NE_CR, save_cr);
}

// ---------------- ISR (IRQ9, vector 41) --------------------
// Didaftar idt_set_gate(41, ...) di idt.cpp. Hanya: ack IRQ di
// card, catat flag overflow, poll (net_poll punya cli-guard
// sendiri), EOI ke PIC.
extern "C" __attribute__((interrupt)) void ne2000_isr(void* frame) {
    (void)frame;
    ne_irqs++;

    uint8_t isr = ne_in(NE_ISR);
    if (isr & ISR_OVW) ne_ovw = 1;
    ne_out(NE_ISR, isr);            /* write-back = ack          */

    net_poll();                     /* drain as soon as possible */

    outb(0xA0, 0x20);               /* EOI slave + master        */
    outb(0x20, 0x20);
}
