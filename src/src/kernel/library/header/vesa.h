#ifndef VESA_H
#define VESA_H

#include <stdint.h>
#include "multiboot.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
//  VESA/VBE Mode Info Block
//
//  NOTE: We never call BIOS int 0x10 ourselves to get this. By
//  the time our code runs we're already in 32-bit protected mode
//  (start.asm switched us there), and int 0x10 in protected mode
//  does NOT reach the BIOS - it hits whatever is sitting in our
//  own IDT vector 16 instead, which either panics or, if the IDT
//  isn't loaded yet, triple-faults the CPU straight back to GRUB
//  (this was the actual cause of the earlier bootloop).
//
//  Instead, GRUB does the real-mode VBE call *for us* (because we
//  requested a video mode in the multiboot header in start.asm)
//  and copies this exact struct into memory, then hands us a
//  pointer to it via multiboot_info_t::vbe_mode_info. See
//  vesa_init_from_multiboot() below.
// ============================================================
typedef struct {
    uint16_t attributes;
    uint8_t  window_a;
    uint8_t  window_b;
    uint16_t granularity;
    uint16_t window_size;
    uint16_t segment_a;
    uint16_t segment_b;
    uint32_t win_func_ptr;
    uint16_t pitch;              // bytes per scanline
    uint16_t width;
    uint16_t height;
    uint8_t  w_char;
    uint8_t  y_char;
    uint8_t  planes;
    uint8_t  bpp;
    uint8_t  banks;
    uint8_t  memory_model;
    uint8_t  bank_size;
    uint8_t  image_pages;
    uint8_t  reserved0;
    uint8_t  red_mask;
    uint8_t  red_position;
    uint8_t  green_mask;
    uint8_t  green_position;
    uint8_t  blue_mask;
    uint8_t  blue_position;
    uint8_t  rsvd_mask;
    uint8_t  rsvd_position;
    uint8_t  direct_color_attributes;
    uint32_t framebuffer;        // physical address of linear framebuffer
    uint32_t offscreen_mem_off;
    uint16_t offscreen_mem_size;
    uint8_t  reserved1[206];
} __attribute__((packed)) vbe_mode_info_t;

// ============================================================
//  Public API - single source of truth for VESA state.
//  kernel.cpp just calls vesa_init_from_multiboot() once at boot
//  and then everything (shell, future GUI/LVGL) reads through
//  these getters instead of keeping its own copy.
// ============================================================

// Parse the VBE mode info GRUB already set up for us. Returns 0 on
// success, negative on failure (no VBE info, bad pointer, etc).
// Safe to call even if mb_magic isn't the multiboot magic - it
// will just report unavailable rather than dereferencing garbage.
int vesa_init_from_multiboot(uint32_t mb_magic, multiboot_info_t* mbi);

/* FIX(V1): override the active mode via Bochs VBE DISPI (ports 0x1CE/0x1CF).
 * Used by kernel_main to enforce 1366x768x32 (16:9) on emulators that
 * do not have a 1366x768 mode in their VBE list (GRUB silently falls
 * back to 1024x768 4:3). Returns:
 *    0  = target mode already active OR successfully programmed
 *   -1  = VESA unavailable (touches nothing)
 *   -2  = card has no DISPI (usually real hardware) — the GRUB
 *         mode is kept as-is
 *   -3  = read-back verification failed — the old mode is restored
 * Safe to call on any hardware: without DISPI, only the ID register
 * is read and the function exits. */
int vesa_force_mode(uint16_t width, uint16_t height, uint8_t bpp);

int vesa_is_available(void);
uint32_t vesa_get_framebuffer(void);
uint16_t vesa_get_width(void);
uint16_t vesa_get_height(void);
uint8_t  vesa_get_bpp(void);
uint16_t vesa_get_pitch(void);

void vesa_draw_pixel(uint16_t x, uint16_t y, uint32_t color);
void vesa_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color);

// Wait for vertical retrace (VBlank) - optional, avoids tearing.
// Only meaningful for the legacy VGA CRTC port; on most VBE LFB
// modes in QEMU this still works but isn't guaranteed on real HW.
void vesa_wait_vblank(void);

#ifdef __cplusplus
}
#endif

#endif
