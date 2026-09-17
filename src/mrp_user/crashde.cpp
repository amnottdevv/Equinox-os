// crashde.cpp — RING 3 TEST #1: division by zero in a user program.
// Expected: the "[user] program faulted: Division By Zero" report, then
// the shell stays ALIVE and accepts the next command. Old version (ring 0):
// kernel panic + 30-second reboot.

#include "Morph.h"

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;

    print("crashde: ready to divide by zero...\n");
    print("crashde: 42 / 0 = ");

    volatile int zero = 0;
    volatile int fortytwo = 42;
    int boom = fortytwo / zero;       // #DE from CPL 3
    printint((uint32_t)boom);          // never reaches here
    print("\n");

    exit(0);
}
