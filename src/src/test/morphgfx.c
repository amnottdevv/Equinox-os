/* morphgfx.c — Morph.h game API regression (syscalls 20-25).
 * Runs both in the host interpreter (make test) and in Equinox OS
 * (mtcc test/morphgfx.c). Exercises the whole game story:
 *   fb_info (struct fill via array decay) -> put_pixel -> fill_rect
 *   (packed x|w<<16 ABI) -> clipping safety -> pollkey (no key
 *   waiting) -> mouse_state -> speaker on/off.
 *
 * Uses ONLY mtcc-supported constructs (no preprocessor, no static,
 * no typedef, no casts): built-ins fb_info / put_pixel / fill_rect /
 * pollkey / mouse_state / spk_tone / spk_silence — the exact Morph.h
 * game API names.
 *
 * Direct pixel readback (fb.addr deref) is verified by the hosted
 * Morph.h test instead: mtcc's C subset has no int->pointer casts.
 * Visual verification in QEMU is done by the outer screendump harness.
 */

/* print a signed int (printint() itself is unsigned-only) */
void prn(int v) {
    if (v < 0) {
        print("-");
        v = -v;
    }
    printint(v);
}

int main() {
    int fb[6];      /* {addr, width, height, bpp, pitch, avail} */
    int m[3];       /* mouse_state: {x, y, buttons} */
    int r;

    /* ---- 1-2: fb_info fills the struct, graphics available ---- */
    r = fb_info(fb);
    print("1 fbinfo  : "); prn(r); print("\n");                    /* 0  */
    print("2 avail   : "); prn(fb[5]); print("\n");                /* 1  */
    if (fb[5] == 0) {
        print("no graphics mode - aborting\n");
        return 1;
    }
    print("3 dims    : ");
    prn(fb[1]); print("x"); prn(fb[2]); print(" bpp="); prn(fb[3]);
    print(" pitch="); prn(fb[4]); print("\n");

    /* ---- 4: single pixel through the kernel (put_pixel) ---- */
    r = put_pixel(1, 2, 255);                                       /* blue */
    print("4 putpix  : "); prn(r); print("\n");                     /* 0  */

    /* ---- 5: fill_rect packed ABI: 40x30 block at (4,8), red ---- */
    r = fill_rect(4 | (40 << 16), 8 | (30 << 16), 16711680);
    print("5 fillrect: "); prn(r); print("\n");                     /* 0  */

    /* ---- 6: clipping safety: huge rect must not crash, returns 0 ---- */
    r = fill_rect(0 | (65535 << 16), 0 | (65535 << 16), 0);
    print("6 clip    : "); prn(r); print("\n");                     /* 0  */

    /* ---- 7: pollkey - no key waiting in a test run ---- */
    print("7 pollkey : "); prn(pollkey()); print("\n");             /* 0  */

    /* ---- 8: mouse_state: return 0 + position report ---- */
    r = mouse_state(m);
    print("8 mouse   : "); prn(r); print(" x=");
    prn(m[0]); print(" y="); prn(m[1]); print(" b=");
    prn(m[2]); print("\n");                                         /* 0 ... */

    /* ---- 9-10: speaker on/off round-trip (no crash, no hang) ---- */
    spk_tone(440);
    print("9 tone    : 440Hz\n");
    spk_silence();
    print("10 quiet  : ok\n");

    /* Compact one-line summary: the full 10-line report above can
     * scroll off the 46-row screen in QEMU capture (compile progress
     * eats the scrollback), but the LAST two lines always stay
     * visible — the QEMU harness asserts on this line instead of
     * hunting for the early lines. */
    print("SUM dims=");
    prn(fb[1]); print("x"); prn(fb[2]); print("x"); prn(fb[3]);
    print(" avail="); prn(fb[5]);
    print("\n");

    print("GFX DONE\n");
    return 0;
}
