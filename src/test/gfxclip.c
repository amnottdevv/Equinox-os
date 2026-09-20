/* gfxclip.c — FR-17/18 demo: per-task draw window + Bresenham line.
 *
 * Exercises the new syscalls 53/54 through the mtcc prelude wrappers:
 *   set_clip(x | w<<16, y | h<<16)  — restrict this task's drawing
 *   fill_rect(...)                   — now clipped to the window
 *   draw_line(x0|y0<<16, x1|y1<<16, c) — Bresenham, clipped
 *
 * The whole-screen RED fill must land ONLY inside the 400x400 window
 * at (100,100); the GREEN screen diagonal must be clipped to the same
 * window. The outer QEMU harness screendumps during the sleep and
 * counts pixels:
 *   red   ~= 400*400 = 160000  (a broken clip gives ~1360*768)
 *   green ~= 370      (the diagonal segment inside the window)
 *   red in screen row 0 == 0    (nothing may leak above the window)
 *
 * Stays alive 15 s after drawing so the screendump can catch the
 * canvas before the console is restored (canvas save/restore, Phase A).
 *
 * printf / set_clip / draw_line come from the <morph.h> prelude;
 * fill_rect / sleep are direct mtcc builtins.
 */
#include <morph.h>

int main() {
    printf("GFXCLIP: window 400x400 at (100,100)\n");
    set_clip(100 | (400 << 16), 100 | (400 << 16));

    printf("GFXCLIP: full-screen fill_rect (must clip to window)\n");
    fill_rect(0 | (1360 << 16), 0 | (768 << 16), 0xFF0000);

    printf("GFXCLIP: screen diagonal draw_line (must clip)\n");
    draw_line(0 | (0 << 16), 1359 | (767 << 16), 0x00FF00);

    printf("GFXCLIP: holding canvas 15 s for the screendump\n");
    sleep(15000);

    set_clip(0, 0);
    printf("GFXCLIP: clip cleared, done\n");
    return 0;
}
