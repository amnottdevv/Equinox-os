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

// Relative deltas for full-screen games (SYS_MOUSEDELTA #33, 0.4 Beta):
// drains all pending packets and returns the RAW accumulated dx/dy
// since the previous call (no clamping) plus the live button mask.
void mouse_get_delta(int32_t* out_dx, int32_t* out_dy, uint8_t* out_buttons);

// Called from the IRQ handler in idt.cpp
void mouse_handle_byte(uint8_t data);

// 0.4 Beta — LOSSLESS click/release edge counters (monotonic, never reset by
// readers). mouse_get_state()'s button mask reflects only the LAST drained
// packet, so a press+release between two consumer polls (~16-20 ms) is
// invisible ("taskbar clicks feel blocked"). These counters increment in
// IRQ context on every 0->1 (press) / 1->0 (release) transition of the
// left button, so edges can never be lost regardless of poll frequency.
// Consumers keep a private snapshot and detect changes.
uint32_t mouse_press_edge_count(void);
uint32_t mouse_release_edge_count(void);
// 0.4 Beta — the same lossless edge counters for the RIGHT button, used by the
// file manager's context menu (right-click). Same contract: monotonic IRQ-side
// counters; consumers keep a private snapshot and detect changes.
uint32_t mouse_rpress_edge_count(void);
uint32_t mouse_rrelease_edge_count(void);

// 0.4 Beta fase 2 — MOUSE WHEEL (roda) / dua jari touchpad PS/2.
// mouse_get_wheel(): wheel delta accumulated since the previous call
// (POSITIVE = scroll up / ke arah riwayat), then cleared — call it
// once per frame from task context. 0 = no wheel input pending.
int  mouse_get_wheel(void);
// Diagnostics (serial/boot log): 1 = the device negotiated the 4-byte
// wheel packet format; mouse_wheel_id() = the Get Device ID reply (3/4).
int  mouse_wheel_enabled(void);
int  mouse_wheel_id(void);

#ifdef __cplusplus
}
#endif

#endif