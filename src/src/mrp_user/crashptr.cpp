// crashptr.cpp — RING 3 TEST #2: NULL pointer dereference in a user program.
// Expected: #PF with CR2=0x00000000 (+ the "NULL pointer?" hint),
// program killed, shell alive.

#include "Morph.h"

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;

    print("crashptr: writing to address 0x0...\n");

    volatile int* p = (volatile int*)0;
    *p = 0xdeadbeef;                  // #PF write user not-present

    print("crashptr: never reaches here\n");
    exit(0);
}
