// ============================================================
//  e1000.h — Intel PRO/1000 (82540EM / 82545EM) PCI driver
// ------------------------------------------------------------
//  QEMU: -device e1000,netdev=net0  (8086:100E, BAR0 = MMIO)
//  Programming model: the classic descriptor-ring driver
//  (Linux e1000 / Intel datasheet 316589):
//    RX 32 legacy descriptors (16 B) + 32 x 2048 B buffers
//    TX  8 legacy descriptors, synchronous transmit (wait DD)
//    IRQ: INTx via PCI irq_line -> idt_register_irq(); the ISR
//         only acks the ICR (read-to-clear) and calls
//         net_rxr_drain_isr() — identical contract to ne2000.
//
//  Compiled by the makefile .c rule with g++ (see ne2000.c):
//  no designated initializers, exports use extern "C".
// ============================================================
#ifndef EQUINOX_E1000_H
#define EQUINOX_E1000_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define E1000_VENDOR   0x8086
#define E1000_DEV_82540 0x100E   /* QEMU -device e1000 (82540EM) */
#define E1000_DEV_82545 0x100F   /* 82545EM                       */

/* probe PCI, map BAR0, bring the ring up. 1 = active, 0 = absent. */
int e1000_probe(void);

/* nic_driver hooks (see nic.h) */
int           e1000_send(const uint8_t* frame, int len);
int           e1000_recv(uint8_t* frame, int maxlen);
const uint8_t* e1000_mac(void);
uint32_t      e1000_irq_count(void);
uint32_t      e1000_rx_overflow(void);

#ifdef __cplusplus
}
#endif

#endif // EQUINOX_E1000_H
