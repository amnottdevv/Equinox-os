/**
 * @file lv_port_disp.c
 * LVGL display port for Equinox OS — writes into the VESA linear framebuffer.
 *
 * BUG FIX (was: garbled/low-res-looking graphics + random crashes/lag):
 * This used to assume the VBE framebuffer is always 32bpp XRGB8888 and
 * blindly wrote lv_color_t (32-bit) words straight into it. But
 * vesa_init_from_multiboot() (see vesa.cpp) accepts 16/24/32bpp — GRUB
 * can and does negotiate a mode other than the requested 1024x768x32 if
 * that exact mode isn't in the card's VBE mode list (falls back to
 * whatever's closest, which on QEMU's default display is often a lower
 * bpp or resolution). When the real framebuffer was 24bpp (3 bytes/px)
 * or 16bpp (2 bytes/px), writing 4-byte words per pixel:
 *   - misaligned every pixel after the first, producing the smeared/
 *     "looks like a scaled-up 480p image" garbage you saw, and
 *   - walked off the end of each scanline (and eventually off the end
 *     of the mapped framebuffer region near the bottom of the screen),
 *     which is the random crash/lag/hang.
 * Fix: branch on the actual negotiated bpp (like vesa_draw_pixel()
 * already correctly does) and convert each pixel instead of assuming
 * the format matches.
 */

#include "lvgl.h"
#include "../header/vesa.h"
#include "lv_port_disp.h"
#include <stdint.h>

/* Draw buffer — LVGL renders into this, then we flush it to the FB.
 * Bumped from 240x10 (2400px, ~328 flush calls to cover a 1024x768
 * screen) to 1024x48 (49152px). A full-screen redraw — e.g. the dimmed
 * backdrop a msgbox draws behind itself, like the file-properties
 * dialog — now takes ~16 flushes instead of ~328, which is the other
 * half of the "properties dialog / clicks feel laggy" symptom (the
 * bpp corruption above was the visual-quality half). 49152 * 4 bytes
 * = 192 KB static — comfortably inside the 18 MB kernel budget. */
static lv_color_t buf1[1024 * 48];

static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_p)
{
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);

    uint8_t *fb_base = (uint8_t *)vesa_get_framebuffer();
    uint16_t pitch    = vesa_get_pitch();
    uint8_t  bpp      = vesa_get_bpp();

    for (uint32_t y = 0; y < h; y++) {
        uint8_t    *row = fb_base + (uint32_t)(area->y1 + y) * pitch;
        lv_color_t *src = color_p + y * w;

        if (bpp == 32) {
            uint32_t *dst = (uint32_t *)row + area->x1;
            for (uint32_t x = 0; x < w; x++)
                dst[x] = src[x].full;
        } else if (bpp == 24) {
            uint8_t *dst = row + (uint32_t)area->x1 * 3;
            for (uint32_t x = 0; x < w; x++) {
                dst[x * 3 + 0] = src[x].ch.blue;
                dst[x * 3 + 1] = src[x].ch.green;
                dst[x * 3 + 2] = src[x].ch.red;
            }
        } else { /* 16bpp, RGB565 */
            uint16_t *dst = (uint16_t *)row + area->x1;
            for (uint32_t x = 0; x < w; x++) {
                uint16_t r = src[x].ch.red   >> 3;   /* 8->5 bits */
                uint16_t g = src[x].ch.green >> 2;   /* 8->6 bits */
                uint16_t b = src[x].ch.blue  >> 3;   /* 8->5 bits */
                dst[x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
    }

    /* Tell LVGL we're done — critical! Without this, LVGL hangs
     * waiting for the flush to complete. */
    lv_disp_flush_ready(drv);
}

/* disp_gpu_blend_cb() removed -- LVGL 8.3 has no disp_drv.gpu_blend_cb
 * field (that was a removed LVGL 7.x API).
 * LVGL 8.3 has built-in software alpha blending via lv_draw_sw_ctx_t,
 * so no custom callback is needed. LVGL handles blending automatically
 * once we set LV_COLOR_DEPTH=32 (already done in lv_conf.h).
 *
 * Note: lv_color32_t in LVGL 8.3 has a 'ch.red' member, NOT
 * 'color.red'. If custom blending is needed later, use:
 *   dest[i].ch.red = (src[i].ch.red * opa + dest[i].ch.red * (255-opa)) / 255;
 *   dest[i].ch.green = ...;
 *   dest[i].ch.blue = ...;
 */

void lv_port_disp_init(void)
{
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t       disp_drv;

    /* FIX(V1): Do not register the LVGL display when VESA is unavailable.
     * Real case: booting via `qemu -kernel` -> QEMU's built-in multiboot
     * loader does NOT set a VBE mode (unlike GRUB via ISO, which honors
     * the video-mode request in the multiboot header of start.asm), so
     * vesa_init_from_multiboot() fails and the console falls back to VGA
     * text 80x25. Without this guard: hor_res/ver_res = 0 (0x0 resolution
     * -> undefined LVGL behavior) and disp_flush_cb writes to
     * framebuffer = 0 -> writes to low physical addresses. Both are
     * crash/hang paths for `gui` / `fm`. */
    if (!vesa_is_available()) {
        return;
    }

    /* Draw buffer */
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, sizeof(buf1) / sizeof(lv_color_t));

    /* Driver */
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = vesa_get_width();
    disp_drv.ver_res  = vesa_get_height();
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &draw_buf;

    /* GPU blend callback removed -- see the comment above.
     * LVGL 8.3 uses its built-in lv_draw_sw_ctx for software blending. */

    lv_disp_drv_register(&disp_drv);
}
