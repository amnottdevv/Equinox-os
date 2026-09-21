/**
 * @file lv_port_indev.c
 * LVGL input port for Equinox OS — PS/2 mouse + keyboard.
 *
 * Mouse: accumulates dx/dy from IRQ12 packets, clamps to screen bounds.
 * Keyboard: non-blocking read from the IRQ1 scancode buffer.
 */

#include "lvgl.h"
#include "../header/stdio.h"     /* keyboard_has_data, keyboard_read_byte, scancode_to_ascii */
#include "../header/ps2_mouse.h"
#include "../header/vesa.h"
#include "lv_port_indev.h"
#include <stdint.h>

/* ----------------------------------------------------------------
 *  MOUSE
 * -------------------------------------------------------------- */
static int mouse_x = 0;
static int mouse_y = 0;
/* BUG FIX M1: PS/2 only sends a packet on movement/button-change, not on
 * every poll. If we blindly report RELEASED whenever pkt.valid==0, a
 * click-and-hold-without-moving gets reported as released the very next
 * read_cb call (right after the press packet is consumed) and LVGL never
 * sees the button held down — clicks/drags get lost intermittently.
 * Fix: remember the last known button state and always report *that*
 * unless a fresh packet says otherwise. */
static lv_indev_state_t mouse_state = LV_INDEV_STATE_RELEASED;

static void mouse_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    mouse_packet_t pkt = mouse_get_packet();

    if (pkt.valid) {
        mouse_x += pkt.dx;
        mouse_y += pkt.dy;

        int max_x = (int)vesa_get_width()  - 1;
        int max_y = (int)vesa_get_height() - 1;
        if (mouse_x < 0)   mouse_x = 0;
        if (mouse_y < 0)   mouse_y = 0;
        if (mouse_x > max_x) mouse_x = max_x;
        if (mouse_y > max_y) mouse_y = max_y;

        mouse_state = (pkt.buttons & 0x01)
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
    }

    /* BUG FIX M2: LVGL requires read_cb to set data->point on *every*
     * call, not just when a new packet arrived — otherwise it reads
     * stale/garbage coordinates from the data struct. Set it unconditionally,
     * and always report the persisted button state from above. */
    data->point.x = mouse_x;
    data->point.y = mouse_y;
    data->state   = mouse_state;
}

/* ----------------------------------------------------------------
 *  KEYBOARD  (LV_INDEV_TYPE_KEYPAD)
 *
 * LVGL's keypad model uses navigation keys (UP/DOWN/ENTER/ESC)
 * and regular ASCII for text input.  We translate scancodes here.
 * -------------------------------------------------------------- */
static uint32_t last_key = 0;
static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;

/* Map PS/2 scancodes (key-down only, 0x80 bit stripped) to LVGL keys */
static uint32_t scancode_to_lv_key(uint8_t sc)
{
    switch (sc) {
        case 0x48: return LV_KEY_UP;
        case 0x50: return LV_KEY_DOWN;
        case 0x4B: return LV_KEY_LEFT;
        case 0x4D: return LV_KEY_RIGHT;
        case 0x1C: return LV_KEY_ENTER;
        case 0x01: return LV_KEY_ESC;
        case 0x47: return LV_KEY_HOME;
        case 0x4F: return LV_KEY_END;
        case 0x49: return LV_KEY_NEXT;    /* PgUp */
        case 0x51: return LV_KEY_PREV;    /* PgDn */
        case 0x0E: return LV_KEY_BACKSPACE;
        case 0x3A: return LV_KEY_DEL;     /* caps lock as DEL (no DEL scancode mapped) */
        default:   return 0;               /* not a special key */
    }
}

static void keyboard_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;

    if (keyboard_has_data()) {
        uint8_t sc = keyboard_read_byte();

        if (sc & 0x80) {
            /* Key release — send RELEASED for the last pressed key */
            data->key   = last_key;
            data->state = LV_INDEV_STATE_RELEASED;
            last_state = LV_INDEV_STATE_RELEASED;
        } else {
            /* Key press */
            uint32_t lk = scancode_to_lv_key(sc);

            /* Printable ASCII — use the shared scancode_to_ascii table */
            if (lk == 0) {
                char ch = scancode_to_ascii[sc];
                if (ch >= 32 && ch <= 126)
                    lk = (uint32_t)ch;
            }

            last_key   = lk;
            data->key   = lk;
            data->state = LV_INDEV_STATE_PRESSED;
            last_state = LV_INDEV_STATE_PRESSED;
        }
    } else {
        /* No data — keep reporting last state so LVGL sees the release */
        data->key   = last_key;
        data->state = last_state;
    }
}

/* ----------------------------------------------------------------
 *  PUBLIC
 * -------------------------------------------------------------- */
void lv_port_indev_init(void)
{
    /* Mouse */
    static lv_indev_drv_t mouse_drv;
    lv_indev_drv_init(&mouse_drv);
    mouse_drv.type    = LV_INDEV_TYPE_POINTER;
    mouse_drv.read_cb = mouse_read_cb;
    lv_indev_t *mouse_indev = lv_indev_drv_register(&mouse_drv);

    /* Keyboard */
    static lv_indev_drv_t kb_drv;
    lv_indev_drv_init(&kb_drv);
    kb_drv.type    = LV_INDEV_TYPE_KEYPAD;
    kb_drv.read_cb = keyboard_read_cb;
    lv_indev_drv_register(&kb_drv);

    /* Start cursor in the center of the screen */
    mouse_x = (int)vesa_get_width()  / 2;
    mouse_y = (int)vesa_get_height() / 2;

    /* BUG FIX (no visible cursor): nothing was ever drawn at mouse_x/
     * mouse_y, so there was no on-screen object whose position was
     * guaranteed to match what read_cb reports for hit-testing. What
     * looked like "the cursor" was actually QEMU's own host-side arrow,
     * which is completely independent of this relative PS/2 tracker —
     * it drifts further from the real (invisible) click position the
     * more the mouse moves, which is why clicks landed further off the
     * longer/farther you moved it. A small dot on lv_layer_sys(),
     * driven by lv_indev_set_cursor(), is positioned by LVGL from the
     * exact same data->point this driver reports, so what you see is
     * always what gets clicked. */
    lv_obj_t *cursor = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(cursor, 8, 8);
    lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cursor, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cursor, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(cursor, 1, 0);
    lv_obj_set_style_shadow_width(cursor, 0, 0);
    lv_obj_clear_flag(cursor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_indev_set_cursor(mouse_indev, cursor);
}
