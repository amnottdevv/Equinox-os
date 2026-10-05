#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IDT entry (8 bytes)
struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t always0;
    uint8_t flags;
    uint16_t base_high;
} __attribute__((packed));

// IDT pointer (6 bytes) for lidt
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

// Initialize the IDT and PIC, then enable interrupts
void idt_init();

// 0.4 Beta: install a HARDWARE IRQ handler at runtime (vector 32+irq).
// The gate uses the same selector/flags as idt_init() and takes
// effect immediately — IDTR points at the same table, so a driver
// probed AFTER boot (e1000 on IRQ11...) can claim its line without
// rebuilding the IDT. irq 0..15; out-of-range calls are ignored.
// The handler is the usual extern "C" __attribute__((interrupt))
// stub and MUST send the EOI itself (see e1000_isr/ne2000_isr).
void idt_register_irq(int irq, void (*handler)(void*));

// This function is called from the IRQ handler (accessible from C++)
extern "C" void irq_handler(uint32_t irq);

#endif