// bgcount.cpp — Phase B demo: a NON-INTERACTIVE background program.
// Used via `spawn bgcount` (a new task runs in parallel with the shell
// and never consumes console input — unlike hello, which asks for a name).
//
// Test in the Equinox shell:
//   spawn bgcount          (the shell returns instantly; the program counts in the background)
//   mtcc /test/multitask.c (spawn + yield demo from a C program)

#include "Morph.h"

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;

    print("[bgcount] pid ");
    printint(getpid());
    print(" started...\n");

    for (int i = 1; i <= 8; i++) {
        sleep_ms(250);
        print("[bgcount] count ");
        printint((uint32_t)i);
        print("\n");
        task_yield();
    }

    print("[bgcount] done, exit(0)\n");
    exit(0);
}
