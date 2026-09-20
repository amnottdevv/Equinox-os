// ============================================================
//  pci.cpp — PCI configuration-space driver + bus enumeration
// ------------------------------------------------------------
//  v0.3 (FR-12). See header/pci.h for the design notes.
//
//  Access protocol (type-0 config cycles, 32-bit):
//      out 0xCF8 = 0x80000000 | bus<<16 | dev<<11 | fn<<8 | reg
//      in  0xCFC (+ (reg & 3) for byte/word reads)
//  A device is present when the vendor ID is not 0xFFFF.
// ============================================================
#include "header/pci.h"
#include "header/stdio.h"      /* printf for lspci          */
#include "header/libstring.h"  /* NULL via stddef chain     */
#include <stddef.h>

extern "C" {

// ------------------------------------------------------------
//  Raw config-space access
// ------------------------------------------------------------
static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)fn   << 8)
         | ((uint32_t)(reg & 0xFC));
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(0xCF8, pci_addr(bus, dev, fn, reg));
    return inl(0xCFC);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(0xCF8, pci_addr(bus, dev, fn, reg));
    return (uint16_t)(inl(0xCFC) >> ((reg & 2) * 8));
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                     uint32_t val) {
    outl(0xCF8, pci_addr(bus, dev, fn, reg));
    outl(0xCFC, val);
}

// ------------------------------------------------------------
//  Enumeration table
// ------------------------------------------------------------
static struct pci_dev g_devs[PCI_MAX_DEVS];
static int            g_count;

int pci_count(void) { return g_count; }

const struct pci_dev* pci_get(int i) {
    if (i < 0 || i >= g_count) return NULL;
    return &g_devs[i];
}

const struct pci_dev* pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < g_count; i++) {
        if (g_devs[i].vendor == vendor && g_devs[i].device == device)
            return &g_devs[i];
    }
    return NULL;
}

// One (bus, dev, fn) triple -> one table entry (if present).
static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint16_t vendor = pci_cfg_read16(bus, dev, fn, 0);
    if (vendor == 0xFFFF) return;
    if (g_count >= PCI_MAX_DEVS) return;

    struct pci_dev* d = &g_devs[g_count++];
    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor = vendor;
    d->device = pci_cfg_read16(bus, dev, fn, 2);

    uint32_t cmd = pci_cfg_read32(bus, dev, fn, 4);
    d->command   = (uint16_t)(cmd & 0xFFFF);
    d->rev       = (uint8_t)(cmd >> 16);
    d->prog_if   = (uint8_t)(cmd >> 24);

    uint32_t cls = pci_cfg_read32(bus, dev, fn, 8);
    d->subclass    = (uint8_t)(cls >> 16);
    d->class_code  = (uint8_t)(cls >> 24);
    d->header_type = (uint8_t)(pci_cfg_read32(bus, dev, fn, 12) >> 16);

    uint32_t irq = pci_cfg_read32(bus, dev, fn, 60);
    d->irq_line   = (uint8_t)irq;
    d->irq_pin    = (uint8_t)(irq >> 8);

    for (int b = 0; b < 6; b++) {
        uint32_t raw = pci_cfg_read32(bus, dev, fn, (uint8_t)(16 + b * 4));
        d->bar[b] = raw;
        d->bar_kind[b] = 0;
        if (raw == 0) continue;
        if (raw & 1) {
            d->bar_kind[b] = 1;                 /* I/O space BAR  */
        } else if ((raw & 0x6) == 0) {
            d->bar_kind[b] = 2;                 /* 32-bit MEM BAR */
        } else if ((raw & 0x6) == 4) {
            d->bar_kind[b] = 3;                 /* 64-bit MEM BAR */
        }
    }

    d->secondary_bus = 0;
    if (d->class_code == 0x06 && d->subclass == 0x04) {
        /* PCI-PCI bridge: register the secondary bus for the caller */
        d->secondary_bus = (uint8_t)(pci_cfg_read32(bus, dev, fn, 24) >> 8);
    }
}

static void pci_scan_bus(uint8_t bus, int depth) {
    if (depth > 2) return;          /* QEMU: 1 level is plenty */
    for (uint8_t dev = 0; dev < 32; dev++) {
        /* function 0 first: multi-function only if header bit 7 */
        uint16_t v0 = pci_cfg_read16(bus, dev, 0, 0);
        if (v0 == 0xFFFF) continue;
        uint8_t ht = (uint8_t)(pci_cfg_read32(bus, dev, 0, 12) >> 16);
        uint8_t max_fn = (ht & 0x80) ? 8 : 1;
        for (uint8_t fn = 0; fn < max_fn; fn++) {
            int before = g_count;
            pci_scan_function(bus, dev, fn);
            if (g_count > before &&
                g_devs[g_count - 1].secondary_bus != 0) {
                pci_scan_bus(g_devs[g_count - 1].secondary_bus, depth + 1);
            }
        }
    }
}

int pci_init(void) {
    g_count = 0;
    pci_scan_bus(0, 0);
    return g_count;
}

// ------------------------------------------------------------
//  lspci — human-readable dump
// ------------------------------------------------------------
static const char* pci_class_name(uint8_t cls, uint8_t sub) {
    switch (cls) {
        case 0x00: return (sub == 0x01) ? "old VGA device" : "uncategorized";
        case 0x01: return (sub == 0x01) ? "IDE controller"
                                        : "mass storage controller";
        case 0x02: return (sub == 0x00) ? "Ethernet controller"
                                        : "network controller";
        case 0x03: return "display controller (VGA)";
        case 0x04: return "multimedia device";
        case 0x05: return "memory controller";
        case 0x06: switch (sub) {
            case 0x00: return "host bridge";
            case 0x01: return "ISA bridge";
            case 0x03: return "other bridge";
            case 0x04: return "PCI-PCI bridge";
            default:   return "bridge device";
        }
        case 0x07: return "communication controller";
        case 0x08: return "system peripheral";
        case 0x09: return "input device";
        case 0x0A: return "docking station";
        case 0x0B: return "processor";
        case 0x0C: return "serial bus controller (USB/FireWire)";
        default:   return "unknown class";
    }
}

static const char* pci_irq_name(uint8_t pin) {
    switch (pin) {
        case 0:  return "-";
        case 1:  return "INTA";
        case 2:  return "INTB";
        case 3:  return "INTC";
        case 4:  return "INTD";
        default: return "?";
    }
}

void pci_lspci(void) {
    printf("PCI bus: %d device(s)\n", g_count);
    printf("%-6s %-10s %-10s %-9s %-28s %s\n",
           "BD:F", "VENDOR", "DEVICE", "CLASS", "DESCRIPTION", "IRQ");
    for (int i = 0; i < g_count; i++) {
        const struct pci_dev* d = &g_devs[i];
        printf("%02x:%02x.%x 0x%04x     0x%04x     %02x%02x/%02x  %-28s %s %d\n",
               d->bus, d->dev, d->fn, d->vendor, d->device,
               d->class_code, d->subclass, d->prog_if,
               pci_class_name(d->class_code, d->subclass),
               pci_irq_name(d->irq_pin), d->irq_line);
        for (int b = 0; b < 6; b++) {
            if (d->bar_kind[b] == 0) continue;
            printf("      BAR%d %s 0x%08x (%s)\n", b,
                   (d->bar_kind[b] == 1) ? "I/O " : "MEM ",
                   d->bar[b] & ~((d->bar_kind[b] == 1) ? 3u : 15u),
                   (d->bar_kind[b] == 3) ? "64-bit" : "32-bit");
        }
    }
}

} // extern "C"
