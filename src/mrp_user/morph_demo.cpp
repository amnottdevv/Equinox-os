/* morph_demo.cpp — hosted Morph.h demo program (.mrp).
 * ----------------------------------------------------------------------------
 * Built with the hosted toolchain (i386-elf-g++ / g++ -m32 + mrp_pack.py),
 * NOT with mtcc — this is the path where the REAL Morph.h header is used.
 * The same file also serves as documentation for the "edit a file" pattern:
 *   file_size -> file_read_all -> modify -> file_write (overwrite)
 *
 * In-OS usage after packing:
 *   run morph_demo.mrp
 */
#include "Morph.h"

/* Entry point: .start section at offset 0 (mrp_pack.py requirement).
 * We ignore the legacy mrp_api_t pointer — Morph.h programs talk to the
 * kernel directly through int 0x80. */
extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;

    /* 1. create (or overwrite) a data file */
    file_write("morph_demo.txt", "Morph.h v1", 10);

    /* 2. check it now exists and report its size */
    print("exists=");
    printint((uint32_t)file_exists("morph_demo.txt"));
    print(" size=");
    printint((uint32_t)file_size("morph_demo.txt"));
    print("\n");

    /* 3. the classic edit pattern: load -> modify -> save */
    char buf[64];
    int n = file_read_all("morph_demo.txt", buf, sizeof(buf) - 1);
    if (n >= 0) {
        buf[n] = '!';
        buf[n + 1] = '\0';
        file_write("morph_demo.txt", buf, (uint32_t)(n + 1));  /* timpa */
    }

    /* 4. verify the overwrite took effect */
    n = file_read_all("morph_demo.txt", buf, sizeof(buf));
    buf[(n >= 0 && n < 64) ? n : 0] = '\0';
    print("content=");
    print(buf);
    print("\n");

    exit(0);
}
