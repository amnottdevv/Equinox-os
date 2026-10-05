/* libc/gfx.c - per-task graphics window (set_clip/draw_line, FR-17).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: none. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_GFX_C
#define LIBC_GFX_C

/* ===================== graphics (FR-17) ===================== */
/* set_clip(x | w<<16, y | h<<16): confine this task's draws to a
   window - put_pixel / fill_rect / draw_line are clipped to it
   (kernel-side, per task). Same packed form as fill_rect. */
int set_clip(int xw, int yh) {
    return __gfx_clip2(xw, yh);
}
/* draw_line(x0 | y0<<16, x1 | y1<<16, color): Bresenham line,
   clipped to the task's window (if any) and console focus. */
int draw_line(int p0, int p1, int color) {
    return __gfx_line3(p0, p1, color);
}
#endif
