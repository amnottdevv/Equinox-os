#include "../header/ps2_mouse.h"
#include "../header/stdio.h"
#include "../header/vesa.h"     // screen bounds for the absolute tracker
#include "../header/ringbuf.h"  // v10.5: mouse packet ring
#include <stdint.h>

#define MOUSE_PORT_DATA   0x60
#define MOUSE_PORT_CMD    0x64
#define MOUSE_PORT_STATUS 0x64

/* FIX: save/restore IF (EFLAGS bit 9) for critical sections — not an
 * unconditional cli/sti. If this function is called from an IRQ context
 * (IF already 0), the old sti() would actually TURN ON interrupts in
 * the middle of the handler -> a race with other handlers. */
static inline uint32_t irq_save(void) {
    uint32_t flags;
    asm volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint32_t flags) {
    asm volatile("pushl %0\n\tpopfl" :: "r"(flags) : "memory");
}

/* v10.5 — PACKET RING (replaces the single-slot state).
 * ---------------------------------------------------------------
 * The old design kept ONE decoded packet (mouse_state.valid): when a
 * second packet arrived before the consumer polled, the first was
 * SILENTLY OVERWRITTEN. The PS/2 device reports at 100 Hz while a
 * game loop polls at ~60 Hz — up to 40% of motion deltas could be
 * lost, making the in-game cursor feel heavy and jumpy.
 *
 * 128 slots = 1.28 s of buffered motion at 100 Hz. Overflow policy
 * is overwrite-OLDEST: if a consumer ever stalls that long, fresher
 * motion is more valuable than stale deltas. Losses are counted in
 * the ring's drop counter (see `ringstats`). */
static RingBuffer<mouse_packet_t, 128> mouse_ring;

/* Packet-assembly state machine (IRQ context): 3 raw bytes -> packet. */
static volatile struct {
    uint8_t cycle;
    uint8_t packet[3];
} mouse_state = { .cycle = 0, .packet = {0, 0, 0} };

static volatile uint32_t mouse_irq_counter = 0;
static volatile uint8_t  mouse_last_byte = 0;

static void mouse_wait_data(void) {
    uint32_t timeout = 100000;
    while (((inb(MOUSE_PORT_STATUS) & 1) == 0) && timeout-- > 0);
}

static void mouse_wait_write(void) {
    uint32_t timeout = 100000;
    while (((inb(MOUSE_PORT_STATUS) & 2) != 0) && timeout-- > 0);
}

static void mouse_send_command(uint8_t cmd) {
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0xD4);
    mouse_wait_write();
    outb(MOUSE_PORT_DATA, cmd);
}

static uint8_t mouse_read_ack(void) {
    mouse_wait_data();
    return inb(MOUSE_PORT_DATA);
}

void mouse_handle_byte(uint8_t data) {
    mouse_last_byte = data;
    mouse_irq_counter++;

    if (mouse_state.cycle == 0) {
        if (data & 0x08) {
            mouse_state.packet[0] = data;
            mouse_state.cycle = 1;
        }
    } else if (mouse_state.cycle == 1) {
        mouse_state.packet[1] = data;
        mouse_state.cycle = 2;
    } else if (mouse_state.cycle == 2) {
        mouse_state.packet[2] = data;
        mouse_state.cycle = 0;

        uint8_t b1 = mouse_state.packet[0];
        uint8_t b2 = mouse_state.packet[1];
        uint8_t b3 = mouse_state.packet[2];

        mouse_packet_t pkt;
        pkt.buttons = b1 & 0x07;
        pkt.dx      = (int8_t)b2;
        pkt.dy      = (int8_t)(-(int8_t)b3);
        pkt.valid   = 1;

        // overwrite-oldest on overflow: freshest motion wins
        mouse_ring.push_overwrite(pkt);
    }
}

void mouse_init(void) {
    uint32_t irqf = irq_save();

    /* Step 1: enable the AUX port on the 8042 */
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0xA8);

    /* Step 2 (BUG FIX M4): set 8042 controller config bit 1
     * (Mouse Interrupt Enable). Without this, IRQ12 is not generated
     * by the 8042 even though the PIC is already unmasked. */
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0x20);
    mouse_wait_data();
    uint8_t config = inb(MOUSE_PORT_DATA);
    config |= 0x02;     /* bit 1 = Mouse Interrupt Enable */
    config &= ~0x20;    /* bit 5 = 0 (enable mouse clock) */
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0x60);
    mouse_wait_write();
    outb(MOUSE_PORT_DATA, config);

    /* Step 3 (BUG FIX M3): correct order — DISABLE → set rate → ENABLE */
    mouse_send_command(0xF5);   /* Disable data reporting */
    mouse_read_ack();

    mouse_send_command(0xF3);   /* Set sample rate */
    mouse_read_ack();
    mouse_send_command(100);
    mouse_read_ack();

    mouse_send_command(0xE8);   /* Set resolution 4 counts/mm */
    mouse_read_ack();
    mouse_send_command(0x02);
    mouse_read_ack();

    mouse_send_command(0xF4);   /* Enable data reporting */
    uint8_t ack = mouse_read_ack();
    if (ack != 0xFA) {
        printf("Mouse init: ACK failed (0x%x)\n", ack);
    } else {
        printf("Mouse init: ACK OK\n");
    }

    /* Step 4: unmask IRQ12 on the slave PIC + IRQ2 on the master PIC */
    uint8_t slave_mask = inb(0xA1);
    slave_mask &= ~(1 << 4);
    outb(0xA1, slave_mask);

    uint8_t master_mask = inb(0x21);
    master_mask &= ~(1 << 2);
    outb(0x21, master_mask);

    mouse_state.cycle = 0;
    mouse_ring.reset();     // v10.5: clear stale pre-init packets
    mouse_irq_counter = 0;
    mouse_last_byte = 0;

    printf("Mouse IRQ12 enabled (8042 cfg=0x%x)\n", config);
    irq_restore(irqf);
}

mouse_packet_t mouse_get_packet(void) {
    mouse_packet_t pkt;
    /* v10.5: pop from the ring — packets are NO LONGER lost when >1
     * packet arrives between two polls (the old single-slot behavior).
     * Atomicity against IRQ12 is guaranteed by the ring primitive's
     * internal guard. */
    if (mouse_ring.pop(&pkt)) {
        return pkt;                       // valid=1 as pushed
    }
    pkt.buttons = 0;
    pkt.dx = 0;
    pkt.dy = 0;
    pkt.valid = 0;
    return pkt;
}

void mouse_ring_stats(uint32_t* count, uint32_t* drops, uint32_t* cap) {
    if (count) *count = mouse_ring.count();
    if (drops) *drops = mouse_ring.drops();
    if (cap)   *cap   = mouse_ring.capacity();
}

uint32_t mouse_get_irq_count(void) {
    return mouse_irq_counter;
}

uint8_t mouse_get_raw_byte(void) {
    return mouse_last_byte;
}

/* Absolute-position tracker for userland games (SYS_MOUSE).
 * ---------------------------------------------------------------
 * The PS/2 protocol only gives RELATIVE deltas per packet, but a
 * game wants "where is the cursor right now". This consumer-driven
 * accumulator drains every pending packet from the ring each call
 * (mouse_get_packet consumes one) and folds dx/dy into an absolute
 * position, clamped to the visible screen.
 *
 * Coexistence with the LVGL port: lv_port_indev.c ALSO consumes
 * packets through mouse_get_packet(). The two consumers never run
 * at the same time (a game owns the screen OR the GUI does), so
 * whoever polls owns the deltas. Since v10.5 pending packets queue
 * up in the ring instead of a single slot, both consumers now see
 * EVERY packet — the GUI cursor gets the same no-loss guarantee.
 * When nobody polls, packets accumulate (up to 128) and the oldest
 * are evicted first.
 *
 * The position is lazily initialized to the screen center on the
 * FIRST movement so a game can draw a cursor before any input. */
void mouse_get_state(int32_t* out_x, int32_t* out_y, uint8_t* out_buttons) {
    static int32_t abs_x = -1;
    static int32_t abs_y = -1;
    static uint8_t last_buttons = 0;

    int32_t max_x = 319;
    int32_t max_y = 239;
    if (vesa_is_available() && vesa_get_width() > 0 && vesa_get_height() > 0) {
        max_x = (int32_t)vesa_get_width()  - 1;
        max_y = (int32_t)vesa_get_height() - 1;
    }

    // Drain all pending packets into the accumulator.
    for (;;) {
        mouse_packet_t pkt = mouse_get_packet();
        if (!pkt.valid) break;
        if (abs_x < 0) {                   // first movement: start centered
            abs_x = (max_x + 1) / 2;
            abs_y = (max_y + 1) / 2;
        }
        abs_x += pkt.dx;
        abs_y += pkt.dy;
        last_buttons = pkt.buttons;
    }

    if (abs_x < 0) {                       // never moved yet: report center
        abs_x = (max_x + 1) / 2;
        abs_y = (max_y + 1) / 2;
    }
    if (abs_x < 0)   abs_x = 0;
    if (abs_y < 0)   abs_y = 0;
    if (abs_x > max_x) abs_x = max_x;
    if (abs_y > max_y) abs_y = max_y;

    if (out_x) *out_x = abs_x;
    if (out_y) *out_y = abs_y;
    if (out_buttons) *out_buttons = last_buttons;
}

/* Relative-delta reader for games that own the whole screen (v10.10,
 * SYS_MOUSEDELTA #33 — the DOOM mouse-look path).
 * ---------------------------------------------------------------
 * Unlike mouse_get_state(), this does NOT fold motion into a clamped
 * absolute position: it drains every pending packet and returns the
 * RAW accumulated dx/dy since the previous call plus the live button
 * mask. A first-person view turning past the screen edge must never
 * lose counts, which the clamped tracker would.
 *
 * Coexistence: same packet ring as mouse_get_state() — a program uses
 * ONE of the two APIs per session (screen-owning game vs GUI cursor),
 * so whoever polls owns the deltas. */
void mouse_get_delta(int32_t* out_dx, int32_t* out_dy,
                     uint8_t* out_buttons) {
    int32_t dx = 0, dy = 0;
    uint8_t btn = 0;

    for (;;) {
        mouse_packet_t pkt = mouse_get_packet();
        if (!pkt.valid) break;
        dx += pkt.dx;
        dy += pkt.dy;
        btn = pkt.buttons;
    }

    if (out_dx) *out_dx = dx;
    if (out_dy) *out_dy = dy;
    if (out_buttons) *out_buttons = btn;
}
