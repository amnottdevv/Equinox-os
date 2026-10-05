#include "header/serial.h"
#include "header/stdio.h"      /* inb/outb */
#include <stdint.h>

/* ============================================================
 *  serial.cpp — polled COM1 UART for QEMU debugging
 *  ------------------------------------------------------------
 *  Register map (COM1, base 0x3F8):
 *    0x3F8  THR  (DLAB=0)  data out
 *    0x3F8  DLL  (DLAB=1)  divisor low
 *    0x3F9  DLM  (DLAB=1)  divisor high
 *    0x3FA  FCR           FIFO control
 *    0x3FB  LCR           line control (8N1 + DLAB)
 *    0x3FD  LSR           line status (bit5 = THR empty)
 *  Konfigurasi: 115200 baud 8N1, FIFO on (14-byte).
 *
 *  Polling murni: serial_putc menunggu LSR bit5. Di QEMU THR
 *  selalu siap hampir seketika (slirp/chardev buffer), jadi
 *  never blocks for real. A 65536-iteration timeout guard
 *  mencegah hang permanen di hardware tanpa UART.
 *
 *  Every function is IRQ-context safe: there is no mutable state
 *  besides the ready flag, and I/O ports need no lock.
 * ============================================================ */

#define COM1_BASE 0x3F8

static int serial_ready = 0;

void serial_init(void) {
    outb(COM1_BASE + 1, 0x00);    /* disable interrupts */
    outb(COM1_BASE + 3, 0x80);    /* DLAB = 1 */
    outb(COM1_BASE + 0, 0x01);    /* divisor low: 115200 */
    outb(COM1_BASE + 1, 0x00);    /* divisor high */
    outb(COM1_BASE + 3, 0x03);    /* 8N1, DLAB = 0 */
    outb(COM1_BASE + 2, 0xC7);    /* FIFO on, clear, 14-byte */
    outb(COM1_BASE + 4, 0x0B);    /* DTR+RTS+OUT2 */

    /* Loopback self-test: on failure there is no UART —
     * serial_ready stays 0 and the whole mirror becomes a no-op. */
    outb(COM1_BASE + 4, 0x1E);    /* loopback mode */
    outb(COM1_BASE + 0, 0xAE);    /* byte tes */
    uint8_t back = inb(COM1_BASE + 0);
    outb(COM1_BASE + 4, 0x0F);    /* normal mode */
    serial_ready = (back == 0xAE);
}

int serial_is_ready(void) {
    return serial_ready;
}

void serial_putc(char c) {
    if (!serial_ready) return;
    if (c == '\n') {
        /* CRLF: serial terminal senang \r\n. */
        uint32_t spin = 0;
        while (((inb(COM1_BASE + 5) & 0x20) == 0) && (spin++ < 65536u)) { }
        outb(COM1_BASE + 0, '\r');
    }
    uint32_t spin = 0;
    while (((inb(COM1_BASE + 5) & 0x20) == 0) && (spin++ < 65536u)) { }
    outb(COM1_BASE + 0, (uint8_t)c);
}

void serial_puts(const char* s) {
    if (!s) return;
    while (*s) serial_putc(*s++);
}
