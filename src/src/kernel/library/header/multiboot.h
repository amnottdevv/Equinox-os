#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

// Magic value GRUB puts in EAX when jumping to a Multiboot1 kernel.
// start.asm forwards this straight through as kernel_main's 1st arg.
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

// Multiboot1 info structure handed to us by GRUB (pointer arrives in
// EBX from GRUB, forwarded by start.asm as kernel_main's 2nd arg).
// Only the fields Equinox OS currently reads are included; layout must
// stay byte-for-byte identical to the real spec since we read it
// directly out of memory GRUB wrote.
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t bootloader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
} __attribute__((packed)) multiboot_info_t;

// flags bit 11 = VBE info block(s) are valid/present
#define MULTIBOOT_INFO_VBE (1 << 11)

// flags bit 3 = mods_count / mods_addr valid (boot modules loaded by GRUB)
// Used to load .mrp files into RAMFS at boot (see workflow_mrp.md
// Section A "Blockers that MUST be resolved first").
#define MULTIBOOT_INFO_MODS (1 << 3)

// ============================================================================
//  Multiboot module structure (array of these at mb_info->mods_addr).
//  GRUB places one entry per `module` directive in grub.cfg.
//  Layout follows the Multiboot1 spec (spec says "unsigned long" = 32-bit on i386).
// ============================================================================
struct multiboot_mod_entry {
    uint32_t mod_start;   // physical address where module starts in memory
    uint32_t mod_end;     // physical address where module ends (exclusive)
    uint32_t cmdline;     // string passed with the module (e.g. "/boot/hello.mrp")
    uint32_t pad;         // reserved
} __attribute__((packed));

#endif
