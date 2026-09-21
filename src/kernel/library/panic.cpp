/*
 * ============================================================================
 *  panic.cpp — Kernel panic screen + auto-reboot
 * ----------------------------------------------------------------------------
 *  Two entry paths:
 *    1. kernel_panic(reason, detail)  — software panic from kernel code
 *       (example: "can't load libc" when the libc module fails to enter
 *       RAMFS). "memory location of the panic" = the caller's address
 *       (__builtin_return_address).
 *    2. idt.cpp exception_panic()     — CPU exception (#PF, #GP, etc).
 *       "memory location of the panic" = EIP of the faulting instruction
 *       (from the interrupt frame).
 *
 *  Screen (works in VGA text 80x25 AS WELL AS the VESA framebuffer —
 *  the row width comes from term_get_cols()):
 *
 *    [ full-width red header: KERNEL PANIC ]
 *
 *    Kernel panic!: 0x00106482  (memory location of the panic)
 *    Why panic    : can't load libc
 *    Detail       : fs_write_binary('libc.mrp') failed, err=-7
 *
 *    Error! The system will reboot in 30 seconds.
 *    Reboot in 30 seconds...     <- in-place countdown
 *
 *  Robustness:
 *    - in_panic flag: a second exception during a panic -> straight to
 *      cli+hlt (no more printing, no recursion).
 *    - Timer liveness check before the countdown: if the tick does not
 *      advance (panic happened before timer_init, or IRQs are off),
 *      print a short message then reboot IMMEDIATELY so we never end up
 *      in a countdown that never finishes.
 *    - All output is only printf/put_char/set_cursor_position —
 *      integer-only, safe for interrupt-sensitive files.
 * ============================================================================
 */

#include "header/panic.h"
#include "header/stdio.h"
#include "header/timer.h"
#include "header/color.h"
#include <stdint.h>

// Anti-recursive-panic flag. The single storage location (idt.cpp uses
// it via panic_enter()).
static volatile int in_panic = 0;

// ----------------------------------------------------------------------------
//  panic_enter — guard against recursive panics.
//  Returns 1 = already in a panic (the caller must cli+hlt without printing).
//  Returns 0 = safe to continue; the flag is already set.
// ----------------------------------------------------------------------------
extern "C" int panic_enter(void) {
    if (in_panic) return 1;
    in_panic = 1;
    return 0;
}

// ----------------------------------------------------------------------------
//  panic_screen — draws the panic screen + 30-second countdown + reboot().
//  noreturn.
// ----------------------------------------------------------------------------
extern "C" __attribute__((noreturn))
void panic_screen(uint32_t addr, const char* reason, const char* detail) {
    if (!in_panic) in_panic = 1;   // defensive: called without panic_enter()

    // Stop interrupts while drawing (IRQs could interleave output).
    asm volatile("cli");

    clear_screen();
    set_cursor_position(0, 0);

    // ---- Full-width red header ----
    int cols = term_get_cols();
    set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    for (int i = 0; i < cols; i++) put_char(' ');
    set_cursor_position(0, 2);
    printf(" KERNEL PANIC ");

    // ---- Main rows ----
    set_cursor_position(2, 2);
    set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    printf("Kernel panic!: ");
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printf("0x%08x", addr);
    set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    printf("  (memory location of the panic)");

    set_cursor_position(4, 2);
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printf("Why panic    : ");
    set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    printf("%s", reason ? reason : "(not specified)");

    set_cursor_position(5, 2);
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printf("Detail       : ");
    set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    printf("%s", detail ? detail : "-");

    // ---- Error row + countdown ----
    set_cursor_position(7, 2);
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printf("Error! The system will reboot in 30 seconds.");

    set_cursor_position(9, 2);
    set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    // ---- Turn interrupts back on so PIT ticks -> sleep_ms works ----
    // (If another exception fires during the countdown, exception_panic
    //  sees in_panic and goes straight to cli+hlt — no recursion.)
    asm volatile("sti");

    // ---- Check the timer is alive: the tick must advance in a spin window ----
    uint32_t t0 = get_tick();
    volatile uint32_t spin = 0;
    while (spin < 60000000u && get_tick() == t0) spin++;

    if (get_tick() == t0) {
        // Timer dead (very early panic / IRQs not running).
        // Do not count down forever — reboot now.
        set_cursor_position(9, 2);
        printf("Timer not running - rebooting now.");
        reboot();
    }

    // ---- Countdown 30 -> 1, updated in-place ----
    for (uint32_t s = 30; s >= 1; s--) {
        set_cursor_position(9, 2);
        printf("Reboot in %2u seconds...  ", s);
        sleep_ms(1000);
    }

    set_cursor_position(9, 2);
    printf("Rebooting now...          ");
    sleep_ms(300);
    reboot();

    // reboot() does not return; this is just a compiler safety net.
    while (1) {
        asm volatile("hlt");
    }
}

// ----------------------------------------------------------------------------
//  kernel_panic — software panic API.
//  "memory location of the panic" = the caller instruction's address
//  (return address).
// ----------------------------------------------------------------------------
extern "C" __attribute__((noreturn))
void kernel_panic(const char* reason, const char* detail) {
    if (panic_enter()) {
        // Already in a panic — do not print anything.
        asm volatile("cli");
        while (1) {
            asm volatile("hlt");
        }
    }

    uint32_t here = (uint32_t)(uintptr_t)__builtin_return_address(0);
    panic_screen(here, reason, detail);

    while (1) {
        asm volatile("hlt");
    }
}
