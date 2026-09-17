// ============================================================
//  ne2000.h — NE2000 ISA driver (DP8390 core) for Equinox OS
// ------------------------------------------------------------
//  Pure PIO (remote DMA via the data port at 0x10) — NO ISA
//  system DMA controller, no MMIO, no PCI. Fits QEMU:
//  -device ne2k_isa,netdev=n0,iobase=0x300,irq=9
//
//  This driver is deliberately "polling-friendly": the ISR only
//  acks + sets flags; packet movement happens in ne2000_recv()
//  called from net_poll.
// ============================================================
#ifndef EQUINOX_NE2000_H
#define EQUINOX_NE2000_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NE_IOBASE   0x300
#define NE_IRQ      9
#define NE_TXPAGE   0x40          /* TX buffer page (3KB area) */
#define NE_RING_MIN 0x46          /* RX ring start page        */
#define NE_MAXFRAME 1536

// Reset + probe + ring init + MAC read + IRQ9 unmask.
// return 1 success, 0 no card.
int  ne2000_init(void);

// Send one Ethernet frame (auto-padded to 60 bytes).
// return 0 success, -1 timeout/error.
int  ne2000_send(const uint8_t* frame, int len);

// Fetch one packet from the ring (including packets that wrap
// around the end of the ring).
// return: frame length, 0 when empty, -1 on ring corruption or
//         overflow (the ring is recovered by the call itself).
int  ne2000_recv(uint8_t* frame, int maxlen);

// The 6-byte MAC (valid after a successful init).
const uint8_t* ne2000_mac(void);

// Debug/counters.
uint32_t ne2000_irq_count(void);

// Debug forensics (v10.11): dump the PROM + card RAM round-trip test.
void ne2000_dbg_probe(void);
uint32_t ne2000_rx_overflow(void);

#ifdef __cplusplus
}
#endif

#endif // EQUINOX_NE2000_H
