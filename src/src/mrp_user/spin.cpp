// spin.cpp — Phase C demo: a long-lived program (60 seconds) for
// testing `ps` + `kill <pid>`. Loops with yield + sleep, printing
// elapsed seconds — non-interactive, never touches the keyboard.
//
// Test in the Equinox shell:
//   spawn spin          (task runs ~60 seconds in the background)
//   ps                  (see the spin task: pid, state READY/SLEEP)
//   kill <pid>          (stop it early — reaped by the scheduler)
//   ps                  (the task is gone from the table)

#include "Morph.h"

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;

    print("[spin] pid ");
    printint(getpid());
    print(" started — spinning for 60 seconds (kill <pid> to stop)\n");

    for (int s = 1; s <= 60; s++) {
        sleep_ms(1000);
        print("[spin] ");
        printint((uint32_t)s);
        print(" s\n");
        task_yield();
    }

    print("[spin] done, exit(0)\n");
    exit(0);
}
