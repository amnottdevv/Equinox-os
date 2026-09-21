// ============================================================
//  pci.h — PCI configuration-space driver + bus enumeration
// ------------------------------------------------------------
//  v0.3 (FR-12): the OS discovers its own hardware.
//  Config space via the classic 0xCF8/0xCF9 address ports +
//  0xCFC/0xCFC+2 data ports; type-0 header scan of bus 0 (and
//  secondary buses behind PCI-PCI bridges, depth 2).
//
//  Why: v0.2 hardcodes everything — ne2000 ISA @ 0x300 IRQ9,
//  ATA primary, VESA from multiboot. With enumeration the kernel
//  can SEE what QEMU actually presents (host bridge, PIIX3 ISA,
//  IDE, USB, VGA, e1000...) and drivers can claim devices by
//  (vendor, device) instead of magic addresses. `lspci` prints
//  the table from the shell.
// ============================================================
#ifndef EQUINOX_PCI_H
#define EQUINOX_PCI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCI_MAX_DEVS 32

struct pci_dev {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;         /* 0xFFFF vendor = absent            */
    uint16_t command;                /* IO/MEM/BUS-master enables         */
    uint8_t  rev, prog_if, subclass, class_code;
    uint8_t  header_type;
    uint8_t  irq_pin;                /* 0 = none, 1..4 = INTA..D          */
    uint8_t  irq_line;               /* routed PIC input (0xFF = none)    */
    uint32_t bar[6];                 /* raw BAR values (addr | flags)     */
    uint8_t  bar_kind[6];            /* 0 none, 1 I/O, 2 MEM32, 3 MEM64   */
    uint32_t secondary_bus;          /* PCI-PCI bridge only, else 0       */
};

// --- raw config-space access (any offset 0..255, dword-aligned) ---
uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                         uint32_t val);

// --- enumeration ---
int  pci_init(void);                        // scan; returns device count
int  pci_count(void);
const struct pci_dev* pci_get(int i);       // i < pci_count(), else NULL
const struct pci_dev* pci_find(uint16_t vendor, uint16_t device);

// --- shell command: dump the table to the active console ---
void pci_lspci(void);

#ifdef __cplusplus
}
#endif

#endif // EQUINOX_PCI_H
