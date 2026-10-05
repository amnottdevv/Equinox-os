/**
 * @file stubs.cpp
 * Implementations of standard C library functions used by LVGL that Equinox OS
 * does not have yet. Bare-metal freestanding environment = we must provide
 * them ourselves.
 */

#include "header/malloc.h"
#include "header/libstring.h"
#include "header/stdlib.h"   // defines div_t, ldiv_t, abs, labs, rand, etc
#include <stdint.h>
#include <stddef.h>

// ============ string.h ============

extern "C" void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (uint8_t)c) return (void*)(p + i);
    }
    return nullptr;
}

// ============ stdlib.h ============

extern "C" int abs(int n) {
    return (n < 0) ? -n : n;
}

extern "C" long labs(long n) {
    return (n < 0) ? -n : n;
}

extern "C" div_t div(int numer, int denom) {
    div_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

extern "C" ldiv_t ldiv(long numer, long denom) {
    ldiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

// Simple LCG PRNG — LVGL rarely calls rand() directly,
// but having it avoids linker errors if any code references it.
static uint32_t lcg_state = 12345;

extern "C" int rand(void) {
    lcg_state = lcg_state * 1103515245 + 12345;
    return (int)((lcg_state >> 16) & 0x7FFF);
}

extern "C" void srand(unsigned int seed) {
    lcg_state = seed;
}

extern "C" void abort(void) {
    // Infinite loop — bare metal, no exit()
    for (;;) asm volatile("cli; hlt");
}

extern "C" void exit(int status) {
    (void)status;
    abort();
}

extern "C" int atexit(void (*func)(void)) {
    (void)func;
    return -1; // not supported
}
