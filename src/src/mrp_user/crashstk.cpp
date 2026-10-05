// crashstk.cpp — RING 3 TEST #4: stack overflow via unbounded recursion.
// The user stack is 64 KB (0x902000-0x911FFF); below it sits a supervisor
// GUARD PAGE (0x901000) — recursion hits the guard -> #PF -> the program
// is killed with a clean report, NOT silently trampling the trampoline/arena.

#include "Morph.h"

static int depth = 0;
static char pad[1024];   // enlarge per-frame stack consumption

static int recurse(void) {
    pad[0] = (char)(depth & 0xFF);
    depth++;
    return recurse() + pad[1];
}

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;

    print("crashstk: unbounded recursion (stack 64KB + guard)...\n");
    int r = recurse();
    printint((uint32_t)r);   // never reaches here
    exit(0);
}
