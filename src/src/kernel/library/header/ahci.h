#ifndef EQUINOX_AHCI_H
#define EQUINOX_AHCI_H

// ============================================================
//  ahci.h — AHCI SATA controller driver (0.4 Beta)
// ------------------------------------------------------------
//  Polling, task-file, no IRQ — the same shape as ata.cpp:
//  everything runs in task (shell) context, so no lock is held by
//  an interrupt handler.  DMA uses statically allocated, 4 KB
//  aligned command-list / FIS-receive / command-table buffers that
//  live in the identity-mapped low RAM, so bus address == address.
//
//  Scope (0.4 Beta):
//    - find the controller by PCI class 01:06 (SATA / AHCI)
//    - enable GHC.AE, run each port that actually has a device
//    - IDENTIFY DEVICE (0xEC) for model + capacity + LBA48
//    - READ/WRITE (0x25/0x35 LBA48 DMA, 0xC8/0xCA LBA28 DMA)
//    - FLUSH CACHE (0xEA / 0xE7) after every write batch
//    - register every disk into the block layer at slot 4 and up
//
//  Deferred: NCQ, interrupt mode, ATAPI-over-AHCI, port multipliers.
// ============================================================

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the controller found on the PCI bus and register every
// disk it owns into the block layer (slots 4..7).  Returns the
// number of disks registered; 0 = no AHCI controller present, which
// is the normal answer on a machine that only has legacy IDE.
int ahci_init(void);

#ifdef __cplusplus
}
#endif

#endif // EQUINOX_AHCI_H
