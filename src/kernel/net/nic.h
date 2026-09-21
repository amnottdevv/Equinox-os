// ============================================================
//  nic.h — NIC driver abstraction layer (v0.3, FR-13)
// ------------------------------------------------------------
//  One struct, one registry: the network stack (net_lwip.c)
//  talks to "the active NIC" through nic_send/nic_recv/nic_mac
//  and never names a driver directly. Adding e1000/pcnet later
//  = implement one struct + register it — no stack changes.
//
//  probe() is called in registration order at boot; the first
//  driver that returns 1 becomes active. ISA probing (ne2000)
//  and PCI discovery (e1000...) coexist: PCI NICs we RECOGNIZE
//  but have no driver for are reported honestly instead of
//  silently ignored.
// ============================================================
#ifndef EQUINOX_NIC_H
#define EQUINOX_NIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nic_driver {
    const char*   name;          /* "ne2000-isa", "e1000-pci", ...      */
    int  (*probe)(void);         /* detect + init; 1 = present          */
    int  (*send)(const uint8_t* frame, int len);   /* 0 = OK             */
    int  (*recv)(uint8_t* frame, int maxlen);      /* len, 0, -1         */
    const uint8_t* (*mac)(void); /* 6 bytes, valid after probe          */
    uint32_t (*irq_count)(void);     /* optional: stats / netdebug       */
    uint32_t (*rx_overflow)(void);   /* optional: stats / netdebug       */
};

/* Probe every registered driver (in order). Returns 1 when a NIC
 * is active. Also prints recognized-but-unsupported PCI NICs. */
int net_nic_init(void);

/* The winning driver (NULL before net_nic_init / on failure). */
const struct nic_driver* nic_active(void);
const char* nic_name(void);       /* active driver name or "none" */

/* Thin wrappers — safe to call from the stack with no active NIC
 * (send/recv return -1, mac returns a zero MAC). */
int           nic_send(const uint8_t* frame, int len);
int           nic_recv(uint8_t* frame, int maxlen);
const uint8_t* nic_mac(void);
uint32_t      nic_irq_count(void);
uint32_t      nic_rx_overflow(void);

#ifdef __cplusplus
}
#endif

#endif // EQUINOX_NIC_H
