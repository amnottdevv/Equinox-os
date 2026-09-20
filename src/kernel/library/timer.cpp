#include "header/timer.h"
#include "header/stdio.h"
#include "header/task.h"
#include <stdint.h>
#include <stddef.h>

// Kursor blink 2 Hz — diktek tiap tick IRQ0 (implementasi di stdio.cpp;
// no-op before init_display / on the VGA text backend).
extern "C" void term_cursor_tick(void);

// v10.5: advance the PC-speaker note queue. Producer = task context
// (sys_sndbeep / shell `song`), consumer = THIS irq. The ring primitive
// guards its own ops, so no extra locking is needed here.
extern "C" void audio_tick(uint32_t ms);

// v10.11: drain NIC RX + the lwIP timers (ARP/TCP/ICMP). net_poll has
// cli-guard inside, so it is safe to call from this IRQ context.
extern "C" void net_timer_tick(void);

static volatile uint32_t tick_counter = 0;
static tick_callback_t  tick_cb      = NULL;
static uint32_t         timer_freq   = 100;

extern "C" void timer_set_tick_cb(tick_callback_t cb) {
    tick_cb = cb;
}

extern "C" void timer_init(uint32_t frequency) {
    if (frequency == 0) frequency = 100;
    timer_freq = frequency;
    uint32_t divisor = 1193180 / frequency;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

/* ============================================================
 *  Phase A — the new IRQ0 asm stub + the C dispatcher.
 *  ------------------------------------------------------------
 *  The old stub used GCC __attribute__((interrupt)) and could not
 *  context-switch (fixed popa+iret epilogue). The new stub mimics
 *  pola isr_128 (syscall.cpp):
 *
 *      pushal            ; the 8 GPRs onto the CURRENT task's kernel stack
 *      pushl %esp        ; argumen &regs
 *      call timer_dispatch_c   ; MAY context-switch inside it
 *      addl $4, %esp
 *      popal             ; register task yang di-RESUME
 *      iretl             ; frame interrupt task yang di-RESUME
 *
 *  When a CPL3 task is interrupted, the CPU uses TSS.ESP0 = the kernel
 *  stack task tsb (di-update scheduler tiap switch) → frame berada
 *  di stack task yang benar → suspend/resume konsisten.
 *
 *  The EOI is sent BEFORE task_irq_dispatch(): if the dispatch
 *  context-switches, the PIC does not hold IRQ0 in-service while
 *  another task runs (lower-priority IRQs — mouse/net — would
 *  starve if the EOI were delayed).
 * ============================================================ */
asm(
    ".global isr_32\n"
    ".text\n"
    "isr_32:\n"
    "    pushal\n"
    "    pushl %esp\n"                  /* arg: &regs[0] (cdecl di stack) */
    "    call timer_dispatch_c\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    iretl\n"
);

extern "C" void timer_dispatch_c(void* regs) {
    (void)regs;
    tick_counter++;
    term_cursor_tick();                 // cursor blink 2 Hz (console aktif)
    audio_tick(1000 / timer_freq);      // v10.5: queued-note playback (10ms)
    /* v0.3 FR-09: net_timer_tick() removed from IRQ0 — lwIP now runs
     * in the "net" kernel task (net_service). The timer IRQ stays
     * lean so heavy traffic never delays ticks/keyboard/mouse. */

    if (tick_cb) tick_cb(1000 / timer_freq);

    outb(0x20, 0x20);                   // EOI BEFORE scheduling

    /* Phase A: F1/F2 + quantum + context switch. */
    task_irq_dispatch();
}

extern "C" uint32_t get_tick(void) {
    return tick_counter;
}

extern "C" uint32_t timer_freq_hz(void) {
    return timer_freq;
}

extern "C" void sleep_ms(uint32_t ms) {
    /* Phase A: when the scheduler is active, use task_sleep (BLOCKED —
     * other tasks can run); boot-time (no tasks) keeps the busy-wait. */
    if (task_sched_active()) {
        task_sleep(ms);
        return;
    }
    uint32_t ms_per_tick = 1000 / timer_freq;
    if (ms_per_tick == 0) ms_per_tick = 1;
    uint32_t target = get_tick() + (ms / ms_per_tick);
    while (get_tick() < target) { }
}
