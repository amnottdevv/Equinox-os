#include "header/stdio.h"
#include "header/idt.h"
#include "header/timer.h"
#include "header/ps2_mouse.h"
#include "header/color.h"
#include "header/libstring.h"   // snprintf (bounded) for panic details
#include "header/panic.h"       // panic_enter / panic_screen
#include "header/syscall.h"     // isr_128 — entry gate syscall int 0x80
#include "header/usermode.h"    // user3_report_fault — kill program ring 3 (v10.7)
#include "header/task.h"        // Phase A: per-console keyboard + F1/F2

// Phase A: the new IRQ0 stub (asm in timer.cpp) — it can context-switch.
extern "C" void isr_32(void);
#include <stdint.h>

// ============================================================
//  KEYBOARD PER-CONSOLE (Phase A — multitasking)
//  IRQ1 pushes scancodes into the FOCUSED console's ring (the
//  active console). F1/F2 (scancodes 0x3B/0x3C) are intercepted
//  and turned into global requests (g_req_new_console /
//  g_req_prev_console) processed by the timer. Readers (getkey
//  etc.) drain the ring of the CURRENT task's console — each
//  console's shell reads its own queue.
// ============================================================
#include "header/ringbuf.h"
#define KBD_CONSOLES 8
static RingBuffer<uint8_t, 128> con_kbd[KBD_CONSOLES];

/* The console drained by the current task (fallback: the active console). */
static inline int kbd_console_current(void) {
    struct Task* t = task_current();
    int c = t ? t->console : -1;
    if (c < 0 || c >= KBD_CONSOLES) c = console_active_id();
    if (c < 0 || c >= KBD_CONSOLES) c = 0;
    return c;
}

extern "C" int keyboard_has_data(void) {
    return con_kbd[kbd_console_current()].count() > 0;
}

/* Blocking pop. hlt waits for the next interrupt (100 Hz timer or
 * key) — CPU-friendly and the scheduler still gets its tick. Pop vs
 * IRQ1 atomicity is guaranteed by the ring primitive (internal cli/sti). */
extern "C" uint8_t keyboard_read_byte(void) {
    uint8_t data;
    int con = kbd_console_current();
    while (!con_kbd[con].pop(&data)) {
        asm volatile("hlt");
    }
    return data;
}

/* Non-blocking pop for game loops (SYS_POLLKEY). */
extern "C" int keyboard_read_byte_noblock(void) {
    uint8_t data;
    if (!con_kbd[kbd_console_current()].pop(&data)) return -1;
    return (int)data;
}

/* Shell `ringstats` diagnostics (the current task's console ring). */
extern "C" void keyboard_ring_stats(uint32_t* count, uint32_t* drops,
                                    uint32_t* cap) {
    RingBuffer<uint8_t, 128>* r = &con_kbd[kbd_console_current()];
    if (count) *count = r->count();
    if (drops) *drops = r->drops();
    if (cap)   *cap   = r->capacity();
}

// ============================================================
//  HANDLER EKSTERNAL
// ============================================================
// Phase A: the timer now uses the asm stub isr_32 (timer.cpp),
// not the GCC-interrupt timer_handler — the old declaration is unused.

// v10.11: NE2000 ISA NIC — IRQ9 (vector 41). Driver: kernel/net/ne2000.c
extern "C" __attribute__((interrupt)) void ne2000_isr(void* frame);

// ============================================================
//  CPU EXCEPTION TABLE
//  Names for all 32 exception vectors (0-31)
// ============================================================
static const char* exception_names[32] = {
    "Division By Zero",          //  0  #DE
    "Debug",                     //  1  #DB
    "Non Maskable Interrupt",    //  2  #NMI
    "Breakpoint",                //  3  #BP
    "Overflow",                  //  4  #OF
    "Bound Range Exceeded",      //  5  #BR
    "Invalid Opcode",            //  6  #UD
    "Device Not Available",      //  7  #NM (FPU)
    "Double Fault",              //  8  #DF
    "Coprocessor Segment",       //  9
    "Invalid TSS",              // 10  #TS
    "Segment Not Present",      // 11  #NP
    "Stack-Segment Fault",      // 12  #SS
    "General Protection Fault",  // 13  #GP
    "Page Fault",               // 14  #PF
    "Reserved",                  // 15
    "x87 FPU Error",            // 16  #MF
    "Alignment Check",          // 17  #AC
    "Machine Check",            // 18  #MC
    "SIMD Floating-Point",      // 19  #XM
    "Virtualization",           // 20  #VE
    "Control Protection",       // 21  #CP
    "Reserved",                  // 22
    "Reserved",                  // 23
    "Reserved",                  // 24
    "Reserved",                  // 25
    "Reserved",                  // 26
    "Reserved",                  // 27
    "Hypervisor Injection",      // 28  #HV
    "VMM Communication",         // 29  #VC
    "Reserved",                  // 30
    "Reserved",                  // 31
};

// ============================================================
//  PANIC — Kernel Exception Handler (v10.7: SADAR PRIVILEGE)
//
//  The golden rule now:
//    fault from CPL 0 (a KERNEL bug)  -> panic screen + 30 s reboot
//    fault from CPL 3 (a PROGRAM bug) -> program killed, shell lives
//
//  Detection: the CS the CPU pushed in the frame. CS&3 == 3 means the
//  faulting instruction ran in ring 3 = .mrp program code. At this
//  point the kernel does NOT call panic_enter (nothing on the kernel
//  side is broken); it just builds a report and longjmps back to
//  mrp_run() through user3_report_fault().
// ============================================================

// Layout of the interrupt frame the CPU pushes in protected mode
// (no privilege change). With __attribute__((interrupt)), the frame
// parameter already points at EIP - including for exceptions with an
// error code (GCC adjusts the error-code offset automatically).
struct interrupt_frame_t {
    uint32_t eip;       // the faulting instruction = "panic location"
    uint32_t cs;
    uint32_t eflags;
    uint32_t esp;       // only filled on a privilege change (unused)
    uint32_t ss;
};

__attribute__((noreturn))
static void exception_panic(uint8_t vec, uint32_t err_code, void* frame) {
    // ========================================================
    //  v10.7: a fault from RING 3 -> kill the program, DON'T panic.
    //  This is the requested "handler that doesn't instantly panic":
    //  `1/0` in a user program now ends with a clean report
    //  and the shell stays alive for the next command.
    // ========================================================
    if (frame) {
        uint32_t cs = ((const interrupt_frame_t*)frame)->cs;
        if ((cs & 3u) == 3u) {
            uint32_t eip = ((const interrupt_frame_t*)frame)->eip;
            user3_report_fault(vec, err_code, eip);   // noreturn
        }
    }

    // Prevent recursive panic (e.g. printf itself faulting):
    // once already in a panic, go straight to HLT without printing again.
    if (panic_enter()) {
        asm volatile("cli");
        asm volatile("hlt");
        while (1) {}
    }

    // "panic location" = the EIP of the faulting instruction.
    uint32_t eip = 0;
    if (frame) {
        eip = ((const interrupt_frame_t*)frame)->eip;
    }

    /* Phase A: a panic always renders on the ACTIVE console (not the
     * faulting task's console — that may be a background console). */
    term_force_active_output();

    const char* name = (vec < 32) ? exception_names[vec] : "Unknown exception";

    // Per-exception detail, built on the stack with bounded snprintf
    // (libstring FIX) - cannot overflow.
    char detail[96];
    if (vec == 14) {  // #PF: Page Fault
        uint32_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        snprintf(detail, sizeof(detail),
                 "CR2=0x%08x err=0x%08x %s/%s/%s",
                 cr2, err_code,
                 (err_code & 2) ? "write" : "read",
                 (err_code & 4) ? "user" : "supervisor",
                 (err_code & 1) ? "present" : "not-present");
    } else if (vec == 8) {  // #DF: Double Fault
        snprintf(detail, sizeof(detail),
                 "err=0x%08x (fault while handling another exception)", err_code);
    } else if (vec == 7) {  // #NM: FPU not initialized
        snprintf(detail, sizeof(detail),
                 "err=0x%08x (FPU not initialized - fninit?)", err_code);
    } else {
        snprintf(detail, sizeof(detail), "error code 0x%08x", err_code);
    }

    panic_screen(eip, name, detail);
    while (1) {}  // panic_screen noreturn; safety net compiler
}

// ============================================================
//  CPU EXCEPTION HANDLERS (Vectors 0-31)
//
//  Exception TANPA error code: vectors 0-7, 9, 15-31
//    → signature: void handler(void* frame)
//
//  Exception WITH error code: vectors 8, 10-14
//    → signature: void handler(void* frame, uint32_t err_code)
//
//  GCC __attribute__((interrupt)) handles the prologue/epilogue
//  automatically (pusha, popa, iret). We only need to distinguish
//  the signature based on whether the CPU pushes an error code.
//
//  NOTE: This file is compiled with -mgeneral-regs-only in the
//  Makefile (FPU_SENSITIVE), so handlers are safe from x87/SSE
//  instructions that could trigger recursive #NM.
// ============================================================

// --- Macro: exception WITHOUT error code ---
#define DEFINE_ISR_NOERR(n)                                             \
    extern "C" __attribute__((interrupt)) void isr_##n(void* frame) {  \
        exception_panic(n, 0, frame);                                   \
    }

// --- Macro: exception WITH error code ---
#define DEFINE_ISR_ERR(n)                                                    \
    extern "C" __attribute__((interrupt)) void isr_##n(void* frame,         \
                                                     uint32_t err_code) { \
        exception_panic(n, err_code, frame);                                 \
    }

// ----- Generate all 32 exception handlers -----
DEFINE_ISR_NOERR(0)    // #DE  Division By Zero
DEFINE_ISR_NOERR(1)    // #DB  Debug
DEFINE_ISR_NOERR(2)    //      Non Maskable Interrupt
DEFINE_ISR_NOERR(3)    // #BP  Breakpoint
DEFINE_ISR_NOERR(4)    // #OF  Overflow
DEFINE_ISR_NOERR(5)    // #BR  Bound Range Exceeded
DEFINE_ISR_NOERR(6)    // #UD  Invalid Opcode
DEFINE_ISR_NOERR(7)    // #NM  Device Not Available (FPU)
DEFINE_ISR_ERR(8)      // #DF  Double Fault           [error code]
DEFINE_ISR_NOERR(9)    //      Coprocessor Segment Overrun
DEFINE_ISR_ERR(10)     // #TS  Invalid TSS            [error code]
DEFINE_ISR_ERR(11)     // #NP  Segment Not Present    [error code]
DEFINE_ISR_ERR(12)     // #SS  Stack-Segment Fault     [error code]
DEFINE_ISR_ERR(13)     // #GP  General Protection Fault [error code]
// v0.3 (FR-06): #PF gets a custom body — BEFORE the kill/panic
// path, the demand-paging hook gets a chance to satisfy the fault
// (a reserved, non-present user page: allocate + zero + map + resume
// the faulting instruction). task_demand_fault() returns 0 for every
// "real" fault (guard page, unmapped hole, supervisor bug) which then
// falls through to the existing ring-3-kill / kernel-panic path.
extern "C" __attribute__((interrupt)) void isr_14(void* frame,
                                                  uint32_t err_code) {
    uint32_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    if (task_demand_fault(cr2, err_code)) return;   /* handled: iret */
    exception_panic(14, err_code, frame);
}
DEFINE_ISR_NOERR(15)   //      Reserved
DEFINE_ISR_NOERR(16)   // #MF  x87 FPU Error
DEFINE_ISR_ERR(17)     // #AC  Alignment Check  [error code] FIX: CPU pushes an error code for #AC
DEFINE_ISR_NOERR(18)   // #MC  Machine Check
DEFINE_ISR_NOERR(19)   // #XM  SIMD Floating-Point
DEFINE_ISR_NOERR(20)   // #VE  Virtualization Exception
DEFINE_ISR_NOERR(21)   // #CP  Control Protection
DEFINE_ISR_NOERR(22)   //      Reserved
DEFINE_ISR_NOERR(23)   //      Reserved
DEFINE_ISR_NOERR(24)   //      Reserved
DEFINE_ISR_NOERR(25)   //      Reserved
DEFINE_ISR_NOERR(26)   //      Reserved
DEFINE_ISR_NOERR(27)   //      Reserved
DEFINE_ISR_NOERR(28)   // #HV  Hypervisor Injection
DEFINE_ISR_NOERR(29)   // #VC  VMM Communication
DEFINE_ISR_NOERR(30)   //      Reserved
DEFINE_ISR_NOERR(31)   //      Reserved

// ============================================================
//  KEYBOARD IRQ1 HANDLER (interrupt-driven, per-console + F1/F2)
// ============================================================
extern "C" __attribute__((interrupt)) void irq1_handler(void* frame) {
    (void)frame;

    /* FIX(I2): check the 8042 status first. Bit0 = output-buffer data,
     * bit5 = the byte belongs to AUX (the mouse). */
    uint8_t st = inb(0x64);
    if (st & 0x01) {                 // data present
        uint8_t data = inb(0x60);
        if (st & 0x20) {
            mouse_handle_byte(data);
        } else if (data == 0x3B) {
            /* F1 (make) — request a NEW shell/console. */
            g_req_new_console = 1;
        } else if (data == 0xBB) {
            /* F1 release — ignored. */
        } else if (data == 0x3C) {
            /* F2 (make) — focus the PREVIOUS console. */
            g_req_prev_console = 1;
        } else if (data == 0xBC) {
            /* F2 release — ignored. */
        } else {
            /* regular scancode -> push into the FOCUSED (active) console ring. */
            int foc = console_active_id();
            if (foc < 0 || foc >= KBD_CONSOLES) foc = 0;
            con_kbd[foc].push(data);
        }
    }

    // Send EOI to the master PIC
    outb(0x20, 0x20);
}

// ============================================================
//  MOUSE IRQ12 HANDLER
// ============================================================
extern "C" __attribute__((interrupt)) void mouse_handler(void* frame) {
    (void)frame;
    /* FIX(I3): check the 8042 status - data must be present AND come
     * from AUX. If a keyboard byte strays in via IRQ12, push it into
     * the keyboard buffer; don't poison the mouse packet state machine. */
    uint8_t st = inb(0x64);
    if (st & 0x01) {
        uint8_t data = inb(0x60);
        if (st & 0x20) {
            mouse_handle_byte(data);
        } else {
            // keyboard byte strayed in via IRQ12 -> focused console ring
            int foc = console_active_id();
            if (foc < 0 || foc >= KBD_CONSOLES) foc = 0;
            con_kbd[foc].push(data);
        }
    }
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

// ============================================================
//  SPURIOUS IRQ HANDLER
//  Handles spurious PIC interrupts without an EOI.
//  Sends the EOI to the master only if it is genuinely spurious.
// ============================================================
static void spurious_irq7_handler(void) {
    // Read the master PIC's ISR register
    outb(0x20, 0x0B);
    uint8_t isr = inb(0x20);
    // If bit 7 (IRQ7) is NOT active in the ISR, it is spurious
    if (!(isr & 0x80)) {
        // Spurious: do NOT send an EOI to the slave (it could cause a
        // cascade storm). End it at the master only.
        return;
    }
    // Not spurious - send a normal EOI
    outb(0x20, 0x20);
}

static void spurious_irq15_handler(void) {
    // Read the slave PIC's ISR register
    outb(0xA0, 0x0B);
    uint8_t isr_slave = inb(0xA0);
    if (!(isr_slave & 0x80)) {
        // Spurious from the slave - EOI the master only
        outb(0x20, 0x20);
        return;
    }
    // Not spurious - EOI both slave and master
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

extern "C" __attribute__((interrupt)) void spurious_irq7(void* frame) {
    (void)frame;
    spurious_irq7_handler();
}

extern "C" __attribute__((interrupt)) void spurious_irq15(void* frame) {
    (void)frame;
    spurious_irq15_handler();
}

// ============================================================
//  IDT SETUP
// ============================================================
#define IDT_SIZE 256
static idt_entry idt[IDT_SIZE];
static idt_ptr idtp;

#define PIC1_OFFSET 0x20
#define PIC2_OFFSET 0x28

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

static void pic_remap() {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, PIC1_OFFSET);
    outb(0xA1, PIC2_OFFSET);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    // ENABLE IRQ0 (timer), IRQ1 (keyboard), dan IRQ2 (cascade)
    outb(0x21, 0xF8);   // 11111000
    outb(0xA1, 0xFF);   // disable every slave line first (the mouse stays dead until init)
}

void idt_init() {
    idtp.limit = sizeof(idt_entry) * IDT_SIZE - 1;
    idtp.base = (uint32_t)idt;

    // Zero every entry first
    for (int i = 0; i < IDT_SIZE; i++)
        idt_set_gate(i, 0, 0, 0);

    uint16_t sel = 0x08;
    uint8_t flags = 0x8E;   // Present=1, Interrupt Gate=0xE, DPL=0

    // ============================================================
    //  CPU EXCEPTION HANDLERS (vectors 0-31)
    //  These were PREVIOUSLY MISSING - the main cause of the bootloop!
    //  Without them, a CPU exception -> jump to 0x00000000
    //  -> Triple Fault -> CPU RESET -> infinite bootloop.
    // ============================================================
    idt_set_gate(0,  (uint32_t)isr_0,  sel, flags);   // #DE Division By Zero
    idt_set_gate(1,  (uint32_t)isr_1,  sel, flags);   // #DB Debug
    idt_set_gate(2,  (uint32_t)isr_2,  sel, flags);   //    NMI
    idt_set_gate(3,  (uint32_t)isr_3,  sel, flags);   // #BP Breakpoint
    idt_set_gate(4,  (uint32_t)isr_4,  sel, flags);   // #OF Overflow
    idt_set_gate(5,  (uint32_t)isr_5,  sel, flags);   // #BR Bound Range
    idt_set_gate(6,  (uint32_t)isr_6,  sel, flags);   // #UD Invalid Opcode
    idt_set_gate(7,  (uint32_t)isr_7,  sel, flags);   // #NM Device Not Available (FPU)
    idt_set_gate(8,  (uint32_t)isr_8,  sel, flags);   // #DF Double Fault
    idt_set_gate(9,  (uint32_t)isr_9,  sel, flags);   //    Coprocessor Segment
    idt_set_gate(10, (uint32_t)isr_10, sel, flags);   // #TS Invalid TSS
    idt_set_gate(11, (uint32_t)isr_11, sel, flags);   // #NP Segment Not Present
    idt_set_gate(12, (uint32_t)isr_12, sel, flags);   // #SS Stack-Segment Fault
    idt_set_gate(13, (uint32_t)isr_13, sel, flags);   // #GP General Protection Fault
    idt_set_gate(14, (uint32_t)isr_14, sel, flags);   // #PF Page Fault
    idt_set_gate(15, (uint32_t)isr_15, sel, flags);   //    Reserved
    idt_set_gate(16, (uint32_t)isr_16, sel, flags);   // #MF x87 FPU Error
    idt_set_gate(17, (uint32_t)isr_17, sel, flags);   // #AC Alignment Check
    idt_set_gate(18, (uint32_t)isr_18, sel, flags);   // #MC Machine Check
    idt_set_gate(19, (uint32_t)isr_19, sel, flags);   // #XM SIMD FP
    idt_set_gate(20, (uint32_t)isr_20, sel, flags);   // #VE Virtualization
    idt_set_gate(21, (uint32_t)isr_21, sel, flags);   // #CP Control Protection
    idt_set_gate(22, (uint32_t)isr_22, sel, flags);   //    Reserved
    idt_set_gate(23, (uint32_t)isr_23, sel, flags);   //    Reserved
    idt_set_gate(24, (uint32_t)isr_24, sel, flags);   //    Reserved
    idt_set_gate(25, (uint32_t)isr_25, sel, flags);   //    Reserved
    idt_set_gate(26, (uint32_t)isr_26, sel, flags);   //    Reserved
    idt_set_gate(27, (uint32_t)isr_27, sel, flags);   //    Reserved
    idt_set_gate(28, (uint32_t)isr_28, sel, flags);   // #HV Hypervisor
    idt_set_gate(29, (uint32_t)isr_29, sel, flags);   // #VC VMM Comm
    idt_set_gate(30, (uint32_t)isr_30, sel, flags);   //    Reserved
    idt_set_gate(31, (uint32_t)isr_31, sel, flags);   //    Reserved

    // ============================================================
    //  HARDWARE IRQ HANDLERS (vectors 32-47)
    //  Phase A: vector 32 = isr_32 (the new asm stub in timer.cpp —
    //  pushal + call timer_dispatch_c; it can context-switch).
    // ============================================================
    idt_set_gate(32, (uint32_t)isr_32,          sel, flags);   // IRQ0  Timer
    idt_set_gate(33, (uint32_t)irq1_handler,     sel, flags);   // IRQ1  Keyboard
    idt_set_gate(39, (uint32_t)spurious_irq7,   sel, flags);   // IRQ7  Spurious
    idt_set_gate(41, (uint32_t)ne2000_isr,      sel, flags);   // IRQ9  NE2000 (v10.11)
    idt_set_gate(44, (uint32_t)mouse_handler,    sel, flags);   // IRQ12 Mouse
    idt_set_gate(47, (uint32_t)spurious_irq15,  sel, flags);   // IRQ15 Spurious

    // ============================================================
    //  SYSCALL GATE (vector 0x80) - the `int $0x80` entry door
    //  Trap gate DPL=3 (flags 0xEF):
    //    - DPL=3  -> callable from ring 3 (user mode)
    //                AND ring 0 (today: shell/kernel/.mrp).
    //    - Trap gate (type 0xF, not interrupt gate 0xE) -> IF is
    //      NOT cleared on entry, so blocking syscalls
    //      (readline/sleep/exec) can still be interrupted by
    //      timer/keyboard IRQs - the same pattern Linux uses.
    //  The stub + dispatcher live in syscall.cpp (isr_128).
    // ============================================================
    idt_set_gate(0x80, (uint32_t)isr_128, sel, 0xEF);

    // ============================================================
    //  REMAP PIC → LOAD IDT
    //  NOTE: sti is NOT called here!
    //  kernel_main decides when interrupts get enabled.
    // ============================================================
    pic_remap();

    asm volatile("lidt %0" : : "m"(idtp));
    // no sti here - kernel_main calls sti itself
}
