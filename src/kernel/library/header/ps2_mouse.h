#ifndef PS2_MOUSE_H
#define PS2_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t buttons;
    int8_t dx;
    int8_t dy;
    uint8_t valid;
} mouse_packet_t;

void mouse_init(void);
mouse_packet_t mouse_get_packet(void);
uint32_t mouse_get_irq_count(void);
uint8_t mouse_get_raw_byte(void);

// Ring-buffer diagnostics (shell `ringstats`): pending packets, dropped
// packets (overflow policy: overwrite-oldest) and ring capacity.
void mouse_ring_stats(uint32_t* count, uint32_t* drops, uint32_t* cap);

// Absolute cursor state for userland games (SYS_MOUSE): drains all
// pending relative packets into an absolute position clamped to the
// screen, starting from the center. Buttons: bit0=left bit1=right
// bit2=middle. Any output pointer may be NULL.
void mouse_get_state(int32_t* out_x, int32_t* out_y, uint8_t* out_buttons);

// Relative deltas for full-screen games (SYS_MOUSEDELTA #33, v10.10):
// drains all pending packets and returns the RAW accumulated dx/dy
// since the previous call (no clamping) plus the live button mask.
void mouse_get_delta(int32_t* out_dx, int32_t* out_dy, uint8_t* out_buttons);

// Called from the IRQ handler in idt.cpp
void mouse_handle_byte(uint8_t data);

#ifdef __cplusplus
}
#endif

#endif