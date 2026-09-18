// hello.cpp — simplest possible .mrp program example: print text, read
// input, print again. (v10.7: ported from the old mrp_api_t table to
// pure-syscall Morph.h — function pointers into the kernel cannot be
// called from CPL 3.)
//
// Compile & pack with:
//   python3 mrp_pack.py hello.cpp hello.mrp
//
// Run from the Equinox OS shell:
//   hello          (dispatch system path /equinox/tools)
//   ./hello.mrp    (explicit)
//   run hello.mrp  (old way, still works)

#include "Morph.h"

// Note: this loader does not yet support global constructors (complex
// static init), so avoid global objects with non-trivial constructors.
// Plain static buffers (like the one below) are safe.
static char name_buf[64];

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;   // pure syscalls now — the legacy argument is ignored

    print("Halo dari program .mrp!\n");
    print("Siapa nama kamu? ");

    readline(name_buf, (int)sizeof(name_buf));

    print("Halo, ");
    print(name_buf);
    print("! Sekarang balik ke shell...\n");

    exit(0);
}
