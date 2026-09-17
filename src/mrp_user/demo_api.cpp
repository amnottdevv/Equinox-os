// demo_api.cpp — exercise every Morph.h syscall from inside a .mrp program.
// (v10.7: rewritten from the old mrp_api_t table demo — function pointers
// into the kernel cannot be called from CPL 3. It now demonstrates the
// pure int 0x80 path used by the games & mtcc.)
//
// Compile & pack:
//   python3 mrp_pack.py demo_api.cpp demo_api.mrp
//
// Run from the Equinox OS shell:
//   demo_api        (dispatch system path /equinox/tools)
//   run demo_api.mrp

#include "Morph.h"

static void line(const char* s)      { print(s); print("\n"); }
static void show(const char* label, int v) {
    print("  ");
    print(label);
    print(" = ");
    printint((uint32_t)v);
    print("\n");
}

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;

    line("=== demo_api (Morph.h int 0x80, ring 3) ===");

    // ----- Process / identity -----
    show("getpid()", getpid());
    show("gettick()", (int)gettick());

    // ----- File RAMFS: create -> size -> read -> overwrite -----
    char payload[32];
    payload[0] = 'v'; payload[1] = '1'; payload[2] = '0'; payload[3] = '.';
    payload[4] = '7'; payload[5] = '\0';

    int w = file_write("demo_api.txt", payload, 5);
    show("file_write(5 byte)", w);
    show("file_exists()", file_exists("demo_api.txt"));
    show("file_size()", file_size("demo_api.txt"));

    char back[32];
    int n = file_read_all("demo_api.txt", back, sizeof(back) - 1);
    if (n >= 0) back[n] = '\0';
    show("file_read_all()", n);
    print("  content = ");
    print(back);
    print("\n");

    // ----- Non-blocking input + time -----
    show("pollkey() idle (0)", pollkey());
    sleep_ms(50);
    show("gettick() +50ms", (int)gettick());

    // ----- Framebuffer -----
    morph_fbinfo_t fb;
    int fi = fb_info(&fb);
    show("fb_info()", fi);
    if (fb.avail) {
        print("  fb ");
        printint(fb.width);  print("x");
        printint(fb.height); print(" @");
        printint(fb.bpp);    print("bpp\n");
    }

    // ----- Speaker queue (non-blocking) -----
    show("snd_beep(660,80)", snd_beep(660, 80));

    line("=== done ===");
    exit(0);
}
