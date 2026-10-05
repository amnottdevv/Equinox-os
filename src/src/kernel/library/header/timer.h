#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void timer_init(uint32_t frequency);
void timer_handler(void* frame);
uint32_t get_tick(void);
void sleep_ms(uint32_t ms);

/* Phase A: the new dispatch, called by the asm stub isr_32 (it can context-switch). */
void task_irq_dispatch(void);

/* Returns the timer frequency (Hz). */
uint32_t timer_freq_hz(void);

typedef void (*tick_callback_t)(uint32_t ms);
void timer_set_tick_cb(tick_callback_t cb);

#ifdef __cplusplus
}
#endif

#endif
