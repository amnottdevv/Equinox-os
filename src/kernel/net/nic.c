// ============================================================
//  nic.c — NIC driver registry (v0.3, FR-13)
// ------------------------------------------------------------
//  See nic.h. Current driver: ne2000 ISA (kernel/net/ne2000.c).
//  PCI NIC recognition (reported, not yet driven):
//      8086:100E  e1000       8086:100F  e1000 (82545EM)
//      1022:2000  pcnet32     8086:1229  eepro100
// ============================================================
#include "nic.h"
#include "ne2000.h"
#include "net.h"                 /* printf via the kernel console  */
#include "library/header/stdio.h"
#include <stddef.h>

#include "library/header/pci.h"

/* ---- registry (compile-time; the first probe that wins is used) ----
 * (kernel .c files are compiled as C++ — no designated
 * initializers, positional fields in declaration order). */
static struct nic_driver ne2000_driver = {
    "ne2000-isa",
    ne2000_init,
    ne2000_send,
    ne2000_recv,
    ne2000_mac,
    ne2000_irq_count,
    ne2000_rx_overflow,
};

static struct nic_driver* const g_drivers[] = {
    &ne2000_driver,
};
#define NIC_DRIVER_COUNT (sizeof(g_drivers) / sizeof(g_drivers[0]))

static const struct nic_driver* g_active;

const struct nic_driver* nic_active(void) { return g_active; }

const char* nic_name(void) {
    return g_active ? g_active->name : "none";
}

/* ---- recognized-but-unsupported PCI NICs (FR-13, honest reporting) ---- */
static const struct { uint16_t vendor, device; const char* name; }
    g_pci_nics[] = {
    { 0x8086, 0x100E, "Intel e1000" },
    { 0x8086, 0x100F, "Intel e1000 (82545EM)" },
    { 0x1022, 0x2000, "AMD PCnet32" },
    { 0x8086, 0x1229, "Intel EEPro100" },
};

int net_nic_init(void) {
    g_active = NULL;
    for (uint32_t i = 0; i < NIC_DRIVER_COUNT; i++) {
        if (g_drivers[i]->probe && g_drivers[i]->probe()) {
            g_active = g_drivers[i];
            break;
        }
    }

    /* PCI side: report recognized NICs even without a driver — the
     * bus is enumerated in kernel_main before net_init(). */
    for (uint32_t i = 0; i < sizeof(g_pci_nics) / sizeof(g_pci_nics[0]); i++) {
        const struct pci_dev* d =
            pci_find(g_pci_nics[i].vendor, g_pci_nics[i].device);
        if (d) {
            printf("nic: %s seen on PCI %02x:%02x.%x (no driver yet - "
                   "using %s)\n",
                   g_pci_nics[i].name, d->bus, d->dev, d->fn,
                   g_active ? g_active->name : "nothing");
        }
    }
    return g_active ? 1 : 0;
}

/* ---- thin wrappers (NULL-safe) ---- */
static const uint8_t g_zero_mac[6] = {0, 0, 0, 0, 0, 0};

int nic_send(const uint8_t* frame, int len) {
    if (!g_active || !g_active->send) return -1;
    return g_active->send(frame, len);
}

int nic_recv(uint8_t* frame, int maxlen) {
    if (!g_active || !g_active->recv) return -1;
    return g_active->recv(frame, maxlen);
}

const uint8_t* nic_mac(void) {
    if (!g_active || !g_active->mac) return g_zero_mac;
    return g_active->mac();
}

uint32_t nic_irq_count(void) {
    if (!g_active || !g_active->irq_count) return 0;
    return g_active->irq_count();
}

uint32_t nic_rx_overflow(void) {
    if (!g_active || !g_active->rx_overflow) return 0;
    return g_active->rx_overflow();
}
