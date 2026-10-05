// crashkmem.cpp — RING 3 TEST #3 (MOST IMPORTANT): write to KERNEL MEMORY.
// 0x300000 = KERNEL_HEAP (supervisor page). Ring 0 version: this write
// went through & silently corrupted the kernel heap. With U/S paging v10.7:
// #PF write user/present — program killed, kernel intact, shell alive.
// This is proof of real memory protection, not just a privilege switch.

#include "Morph.h"

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;

    print("crashkmem: attempting to write to kernel heap 0x300000...\n");

    volatile unsigned int* kernel_mem = (volatile unsigned int*)0x300000;
    *kernel_mem = 0xCAFEBABE;         // MUST #PF: user -> supervisor

    print("crashkmem: GOT THROUGH?! paging protection failed!\n");
    exit(0);
}
