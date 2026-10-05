// ============================================================
//  nic.c — NIC driver registry (0.4 Beta, FR-13)
// ------------------------------------------------------------
//  See nic.h. Current driver: ne2000 ISA (kernel/net/ne2000.c).
//  PCI NIC recognition (reported, not yet driven):
//      8086:100E  e1000       8086:100F  e1000 (82545EM)
//      1022:2000  pcnet32     8086:1229  eepro100
// ============================================================
#include "nic.h"
#include "ne2000.h"
#include "e1000.h"
#include "net.h"                 /* printf via the kernel console  */
#include "library/header/stdio.h"
#include <stddef.h>

#include "library/header/pci.h"
#include "library/header/ecf.h"     /* 0.4 Beta: baca /boot/system.ecf   */

/* ---- registry (compile-time; the first probe that wins is used) ----
 * (kernel .c files are compiled as C++ — no designated
 * initializers, positional fields in declaration order).
 * Order = fallback order; the CONFIGURED driver is probed first
 * (see net_nic_init). */
static struct nic_driver ne2000_driver = {
    "ne2000-isa",
    ne2000_init,
    ne2000_send,
    ne2000_recv,
    ne2000_mac,
    ne2000_irq_count,
    ne2000_rx_overflow,
};

static struct nic_driver e1000_driver = {
    "e1000-pci",
    e1000_probe,
    e1000_send,
    e1000_recv,
    e1000_mac,
    e1000_irq_count,
    e1000_rx_overflow,
};

static struct nic_driver* const g_drivers[] = {
    &ne2000_driver,
    &e1000_driver,
};
#define NIC_DRIVER_COUNT (sizeof(g_drivers) / sizeof(g_drivers[0]))

static const struct nic_driver* g_active;

const struct nic_driver* nic_active(void) { return g_active; }

const char* nic_name(void) {
    return g_active ? g_active->name : "none";
}

/* ---- recognized-but-unsupported PCI NICs (FR-13, honest reporting) ----
 * e1000 lives in the registry now, so it is gone from this list. */
static const struct { uint16_t vendor, device; const char* name; }
    g_pci_nics[] = {
    { 0x1022, 0x2000, "AMD PCnet32" },
    { 0x8086, 0x1229, "Intel EEPro100" },
};

/* ---- 0.4 Beta: the driver choice equinoxinstall wrote -------------
 * <mount>/boot/system.ecf (format .ecf, key `net.driver`) holds one
 * of: ne2000 | e1000 | none. It is read at probe time — boot order
 * is fs_init -> volume mount -> modules -> pci_init -> net_init, so
 * the file is there. The OLD boot/nic.cfg format is gone: .ecf is
 * the single source of truth (no fallback). */
static char g_pref[16];
static int  g_pref_loaded;

static int nic_word(const char* a, const char* b) {  /* "a == b" */
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int nic_starts(const char* full, const char* pre) {
    while (*pre) { if (*full != *pre) return 0; full++; pre++; }
    return 1;
}

static void nic_load_pref(void) {
    if (g_pref_loaded) return;
    g_pref_loaded = 1;
    g_pref[0] = 0;
    const char* v = ecf_get(ecf_store(), "net.driver");
    if (v && v[0]) {
        uint32_t i = 0;
        while (v[i] && i < sizeof(g_pref) - 1) { g_pref[i] = v[i]; i++; }
        g_pref[i] = 0;
    }
}

int net_nic_init(void) {
    g_active = NULL;
    nic_load_pref();

    if (nic_word(g_pref, "none")) {
        printf("nic: disabled by system.ecf (none)\n");
        return 0;
    }

    /* 1. the configured driver, if it names one of ours ... */
    if (g_pref[0]) {
        for (uint32_t i = 0; i < NIC_DRIVER_COUNT && !g_active; i++)
            if (nic_starts(g_drivers[i]->name, g_pref) &&
                g_drivers[i]->probe && g_drivers[i]->probe())
                g_active = g_drivers[i];
    }
    /* 2. ... then every other driver in registration order */
    for (uint32_t i = 0; i < NIC_DRIVER_COUNT && !g_active; i++) {
        if (g_pref[0] && nic_starts(g_drivers[i]->name, g_pref)) continue;
        if (g_drivers[i]->probe && g_drivers[i]->probe())
            g_active = g_drivers[i];
    }

    if (g_active && g_pref[0] && !nic_starts(g_active->name, g_pref)) {
        printf("nic: '%s' from system.ecf not available — fell back to %s\n",
               g_pref, g_active->name);
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
