#include "header/timer.h"
#include "header/stdio.h"
#include <stdint.h>
#include <stddef.h>

// Kursor blink 2 Hz — diktek tiap tick IRQ0 (implementasi di stdio.cpp;
// no-op before init_display / on the VGA text backend).
extern "C" void term_cursor_tick(void);

// v10.5: advance the PC-speaker note queue. Producer = task context
// (sys_sndbeep / shell `song`), consumer = THIS irq. The ring primitive
// guards its own ops, so no extra locking is needed here.
extern "C" void audio_tick(uint32_t ms);

// v10.11: drain NIC RX + timer lwIP (ARP/TCP/ICMP). net_poll punya
// cli-guard internal, jadi aman dipanggil dari context IRQ ini.
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

extern "C" __attribute__((interrupt)) void timer_handler(void* frame) {
    (void)frame;
    tick_counter++;
    term_cursor_tick();          // cursor blink 2 Hz (vesa console)
    audio_tick(1000 / timer_freq);  // v10.5: queued-note playback (10ms)
    net_timer_tick();            // v10.11: lwIP RX + timeouts
    if (tick_cb) tick_cb(1000 / timer_freq);
    outb(0x20, 0x20);
}

extern "C" uint32_t get_tick(void) {
    return tick_counter;
}

extern "C" uint32_t timer_freq_hz(void) {
    return timer_freq;
}

extern "C" void sleep_ms(uint32_t ms) {
    uint32_t ms_per_tick = 1000 / timer_freq;
    if (ms_per_tick == 0) ms_per_tick = 1;
    uint32_t target = get_tick() + (ms / ms_per_tick);
    while (get_tick() < target) { }
}
