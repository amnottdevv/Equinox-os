#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
//  PANIC — kernel panic API (software path)
//
//  kernel_panic(reason, detail):
//    Displays the screen:
//      Kernel panic!: 0x<caller's memory address>
//      Why panic   : <reason>   (e.g. "can't load libc")
//      Detail      : <detail>
//      Error! The system will reboot in 30 seconds.
//    then counts down 30 -> 0 and reboots() automatically.
//
//  Safe to call from a normal context AS WELL AS from an exception
//  handler (the in_panic flag prevents recursive panics; if the
//  timer is not running yet / IRQs are off, the reboot is done
//  immediately without a countdown so it never hangs forever).
// ============================================================
__attribute__((noreturn))
void kernel_panic(const char* reason, const char* detail);

// ----- Internal (used by idt.cpp for the CPU exception path) -----

// Returns 1 if we are ALREADY in a panic (the caller must go
// straight to cli+hlt without printing anything); otherwise sets
// the flag and returns 0.
int panic_enter(void);

// Draws the full panic screen (addr = "memory location of the panic")
// then counts down 30 seconds + reboots. The in_panic flag MUST
// already be set via panic_enter() before calling this function.
__attribute__((noreturn))
void panic_screen(uint32_t addr, const char* reason, const char* detail);

#ifdef __cplusplus
}
#endif

#endif /* PANIC_H */
