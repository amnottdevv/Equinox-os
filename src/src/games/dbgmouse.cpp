/* dbgmouse.cpp — SYS_MOUSE diagnostic (.mrp).
 * Waits 4 s (test harness moves the mouse during this window), then
 * prints the absolute mouse position + buttons to the console and
 * exits WITHOUT touching the framebuffer, so the shell OCR can read
 * the numbers right after.
 */
#include "libgame.h"

static void game_main(void);

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;
    game_main();
}

static void game_main(void) {
    uint32_t t0 = gettick();
    while (gettick() - t0 < 400u) {     /* 4 s window for mouse moves */
        sleep_ms(50u);
    }
    morph_mouse_t ms;
    int r = mouse_state(&ms);
    print("dbgmouse: ret=");
    printint((uint32_t)r);
    print(" x=");
    printint((uint32_t)ms.x);
    print(" y=");
    printint((uint32_t)ms.y);
    print(" btn=");
    printint(ms.buttons);
    print("\n");
    exit(0);
}
