#ifndef SERIAL_H
#define SERIAL_H

/*
 * serial.h — COM1 (0x3F8) polled UART for QEMU debugging.
 * Mirrors all console output to the serial line (QEMU: -serial file:...).
 * Polled TX only; no IRQ, safe from any context (IRQ handler,
 * exception handler, task switch).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the UART: 8N1 115200. Idempotent. */
void serial_init(void);

/* Send one byte (blocking-polled; QEMU is always ready). */
void serial_putc(char c);

/* Send a NUL-terminated string. */
void serial_puts(const char* s);

/* 1 = the serial port is up (after a successful serial_init). */
int  serial_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_H */
