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

// This function is called from the IRQ handler (accessible from C++)
extern "C" void irq_handler(uint32_t irq);

#endif