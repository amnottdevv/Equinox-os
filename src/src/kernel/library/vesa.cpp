#include "header/vesa.h"
#include "header/stdio.h"
#include "header/libstring.h"
#include <stdint.h>

// ============================================================
//  Static framebuffer state - the ONE place this lives now.
//  (Previously kernel.cpp kept its own separate copy of all of
//  this and its own vesa_draw_pixel()/vesa_fill_rect(), which
//  could silently drift out of sync with this file. Now
//  kernel.cpp just calls into these functions.)
// ============================================================
static uint32_t framebuffer_addr = 0;
static uint16_t screen_width     = 0;
static uint16_t screen_height    = 0;
static uint8_t  screen_bpp       = 0;
static uint16_t screen_pitch     = 0;   // bytes per scanline, from VBE - do NOT assume pitch == width * bytes_per_pixel, some modes pad rows.
static int      available        = 0;

// ============================================================
//  vesa_init_from_multiboot
//
//  Reads the VBE mode info block GRUB already filled in for us
//  (because start.asm's multiboot header requested a video mode),
//  via multiboot_info_t::vbe_mode_info. No BIOS calls here - by
//  this point we're in protected mode and int 0x10 would not
//  reach the BIOS anyway (see the comment in vesa.h).
// ============================================================
int vesa_init_from_multiboot(uint32_t mb_magic, multiboot_info_t* mbi) {
    available = 0;

    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        printf("VESA: bad multiboot magic (0x%x), start.asm/GRUB mismatch?\n", mb_magic);
        return -1;
    }
    if (!mbi) {
        printf("VESA: multiboot info pointer is NULL.\n");
        return -2;
    }
    if (!(mbi->flags & MULTIBOOT_INFO_VBE)) {
        printf("VESA: GRUB didn't provide VBE info (flags bit 11 unset).\n");
        printf("VESA: check the video-mode request fields in start.asm's multiboot header.\n");
        return -3;
    }

    vbe_mode_info_t* vbe = (vbe_mode_info_t*)mbi->vbe_mode_info;
    if (!vbe || vbe->framebuffer == 0) {
        printf("VESA: VBE mode info pointer/framebuffer invalid.\n");
        return -4;
    }
    if (vbe->bpp != 32 && vbe->bpp != 24 && vbe->bpp != 16) {
        printf("VESA: unsupported bpp (%d), only 16/24/32 are handled.\n", vbe->bpp);
        return -5;
    }

    framebuffer_addr = vbe->framebuffer;
    screen_width  = vbe->width;
    screen_height = vbe->height;
    screen_bpp    = vbe->bpp;
    screen_pitch  = vbe->pitch;
    available = 1;

    printf("VESA: mode 0x%x -> %dx%d @ %d bpp, pitch=%d, fb=0x%x\n",
           mbi->vbe_mode, screen_width, screen_height, screen_bpp,
           screen_pitch, framebuffer_addr);

    return 0;
}

// ============================================================
//  Getters
// ============================================================
int vesa_is_available(void)        { return available; }
uint32_t vesa_get_framebuffer(void){ return framebuffer_addr; }
uint16_t vesa_get_width(void)      { return screen_width; }
uint16_t vesa_get_height(void)     { return screen_height; }
uint8_t  vesa_get_bpp(void)        { return screen_bpp; }
uint16_t vesa_get_pitch(void)      { return screen_pitch; }

// ============================================================
//  FIX(V1) — KERNEL-SIDE MODE OVERRIDE via Bochs VBE DISPI
//  ------------------------------------------------------------
//  PROBLEM: start.asm + grub.cfg already request 1366x768x32,
//  but the VBE mode list reported by the firmware usually lacks
//  1366x768 (QEMU std-VBE e.g. only offers 640x400..1280x1024 /
//  1600x1200), so GRUB silently falls back to
//  1024x768 - a 4:3 ratio, not the requested 16:9.
//
//  SOLUTION: Bochs/VBE-compatible VGA cards (QEMU std, VirtualBox
//  VBoxVGA, some other emulators) expose DISPI registers at ports
//  0x1CE/0x1CF that can program an ARBITRARY resolution - the same
//  path SeaBIOS itself uses when setting a VBE mode. We detect the
//  card through the ID register (0xB0C0..0xB0CF), program
//  1366x768x32 directly, and VERIFY by reading the registers back.
//  If anything mismatches, the old mode is restored and the function
//  reports failure - the kernel keeps using the GRUB mode (a safe
//  fallback that never disables the display).
//
//  SAFETY on real hardware without DISPI: the ID register will
//  never read 0xB0Cx -> the function returns -2 immediately without
//  menyentuh mode apa pun.
// ============================================================
#define VBE_DISPI_INDEX_PORT  0x01CE
#define VBE_DISPI_DATA_PORT   0x01CF

#define VBE_DISPI_INDEX_ID        0x00
#define VBE_DISPI_INDEX_XRES      0x01
#define VBE_DISPI_INDEX_YRES      0x02
#define VBE_DISPI_INDEX_BPP       0x03
#define VBE_DISPI_INDEX_ENABLE    0x04

#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40

static void dispi_write(uint8_t index, uint16_t value) {
    /* FIX(V1d): BOTH DISPI ports are 16-BIT ports on the QEMU side
     * (vbe_portio_list_x86: {.size = 2}). An 8-bit outb to the INDEX
     * port (0x1CE) is silently DROPPED by QEMU's memory dispatcher,
     * so the next data access hits the register with a STALE index
     * (whatever the firmware last wrote) -> the ID read returns the
     * wrong register value -> the override looks "unavailable" (seen
     * in the boot log as "DISPI not available"). SeaBIOS itself
     * writes both registers with outw - we follow the same pattern. */
    outw(VBE_DISPI_INDEX_PORT, index);
    outw(VBE_DISPI_DATA_PORT, value);
}

static uint16_t dispi_read(uint8_t index) {
    outw(VBE_DISPI_INDEX_PORT, index);   // FIX(V1d): outw, bukan outb
    return inw(VBE_DISPI_DATA_PORT);
}

int vesa_force_mode(uint16_t width, uint16_t height, uint8_t bpp) {
    // Only makes sense when GRUB already handed over an active VESA
    // LFB (the framebuffer address and graphics-mode availability are
    // already known).

    // Target mode already active? Nothing to do.
    if (screen_width == width && screen_height == height &&
        screen_bpp  == bpp) {
        return 0;
    }

    // Deteksi Bochs VBE DISPI via register ID (0xB0C0..0xB0CF).
    uint16_t id = dispi_read(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0 || id > 0xB0CF) return -2;   // bukan kartu DISPI

    /* DISPI protocol: the ID register must be written ("unlocked")
     * before any other register accepts writes - the same order
     * SeaBIOS uses. We write back the ID we read (the highest level
     * the card supports); safe on QEMU/VBox. */
    dispi_write(VBE_DISPI_INDEX_ID, id);

    // Save the old mode (for recovery if verification fails).
    // NOTE: writing ENABLE=0 RESETS every DISPI register, so they
    // MUST be read BEFORE disabling.
    uint16_t old_xres = dispi_read(VBE_DISPI_INDEX_XRES);
    uint16_t old_yres = dispi_read(VBE_DISPI_INDEX_YRES);
    uint16_t old_bpp  = dispi_read(VBE_DISPI_INDEX_BPP);

    // Program the new mode (standard Bochs/VBE DISPI order):
    // disable -> XRES -> YRES -> BPP -> enable(+LFB).
    // BPP is written as the raw value (8/15/16/24/32) - the same
    // format SeaBIOS uses when it sets its VBE mode.
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    dispi_write(VBE_DISPI_INDEX_XRES,  width);
    dispi_write(VBE_DISPI_INDEX_YRES,  height);
    dispi_write(VBE_DISPI_INDEX_BPP,   bpp);
    dispi_write(VBE_DISPI_INDEX_ENABLE,
                VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /* --------------------------------------------------------------
     * FIX(V1c): READ BACK THE ACTIVE MODE - DO NOT ASSUME IT MATCHES
     * THE REQUEST EXACTLY.
     *
     * QEMU (vbe_fixup_regs in hw/display/vga.c) ROUNDS XRES DOWN to a
     * multiple of 8: r[XRES] &= ~7. A request for 1366 therefore
     * becomes 1360 (!), so the strict comparison used earlier
     * (readback != request) ALWAYS failed -> the old mode was
     * restored -> the override never activated.
     *
     * Current policy: BPP must match exactly; the actual resolution
     * is accepted as long as it is sane (width may round down by up
     * to 8 px, height must match exactly). 1360x768 from a 1366x768
     * request keeps the ~16:9 ratio and the same console column
     * count (1360/8 = 170).
     * -------------------------------------------------------------- */
    uint16_t act_xres = dispi_read(VBE_DISPI_INDEX_XRES);
    uint16_t act_yres = dispi_read(VBE_DISPI_INDEX_YRES);
    uint16_t act_bpp  = dispi_read(VBE_DISPI_INDEX_BPP);

    int ok = (act_bpp == bpp) &&
             (act_xres != 0) && (act_yres != 0) &&
             (act_xres <= width) && (act_xres + 7 >= width) &&
             (act_yres == height);

    if (!ok) {
        // Reject: restore the old mode (QEMU does not reset the
        // register values on ENABLE=0, so the saved values are still
        // valid).
        dispi_write(VBE_DISPI_INDEX_XRES,  old_xres);
        dispi_write(VBE_DISPI_INDEX_YRES,  old_yres);
        dispi_write(VBE_DISPI_INDEX_BPP,   old_bpp);
        dispi_write(VBE_DISPI_INDEX_ENABLE,
                    VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
        return -3;
    }

    // The LFB address does not change (the card's PCI BAR), but the
    // pitch must be recomputed from the ACTIVE mode: the VBE line
    // offset = virt_width * bpp/8 (QEMU sets virt_width = xres - see
    // vbe_fixup_regs). 32bpp is always 4-byte aligned, no extra
    // padding.
    screen_width  = act_xres;
    screen_height = act_yres;
    screen_bpp    = (uint8_t)act_bpp;
    screen_pitch  = (uint16_t)((uint32_t)act_xres * ((uint32_t)act_bpp / 8u));

    // Blank the new screen - leftover GRUB output at a different size
    // would look like garbage if not cleared. (init_display does this
    // too, but we make sure here as well so vesa_force_mode always
    // leaves a clean screen.)
    vesa_fill_rect(0, 0, screen_width, screen_height, 0x000000);

    return 0;
}

// ============================================================
//  Drawing functions
//
//  Uses screen_pitch (bytes/scanline reported by VBE) to find the
//  start of each row, instead of assuming pitch == width * bpp/8.
//  Some VBE modes pad each scanline for alignment, so indexing by
//  width alone (the old kernel.cpp behaviour) can quietly draw
//  diagonally-skewed garbage on modes where that assumption breaks.
// ============================================================
void vesa_draw_pixel(uint16_t x, uint16_t y, uint32_t color) {
    if (!available || !framebuffer_addr || x >= screen_width || y >= screen_height) return;

    uint8_t* row = (uint8_t*)framebuffer_addr + (uint32_t)y * screen_pitch;

    if (screen_bpp == 32) {
        ((uint32_t*)row)[x] = color;
    } else if (screen_bpp == 24) {
        uint8_t* px = row + (uint32_t)x * 3;
        px[0] = color & 0xFF;
        px[1] = (color >> 8) & 0xFF;
        px[2] = (color >> 16) & 0xFF;
    } else if (screen_bpp == 16) {
        uint16_t r = (color >> 19) & 0x1F;   // 5-bit
        uint16_t g = (color >> 10) & 0x3F;   // 6-bit
        uint16_t b = (color >> 3) & 0x1F;    // 5-bit
        ((uint16_t*)row)[x] = (r << 11) | (g << 5) | b;
    }
}

void vesa_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color) {
    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            vesa_draw_pixel(x + col, y + row, color);
        }
    }
}

// ============================================================
//  VBlank wait (optional) - legacy VGA CRTC status port. Works
//  under QEMU's VBE/std-vga on the modes we use; not guaranteed
//  on every real VBE-only card, but harmless if it just returns.
// ============================================================
void vesa_wait_vblank(void) {
    while ((inb(0x3DA) & 0x08) != 0);  // wait for retrace to end
    while ((inb(0x3DA) & 0x08) == 0);  // wait for retrace to start
}
