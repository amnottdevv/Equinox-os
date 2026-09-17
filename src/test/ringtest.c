/* ringtest.c — ring buffer subsystem regression (v10.5, syscall 26).
 * Runs both in the host interpreter (make test) and in Equinox OS
 * (mtcc test/ringtest.c). Exercises the note-queue story:
 *   snd_beep capacity (64 slots -> EBUSY) -> EINVAL on ms=0 ->
 *   manual speaker API (flush + tone + silence) -> pollkey idle.
 *
 * Uses ONLY mtcc-supported constructs (no preprocessor, no static,
 * no typedef, no casts): built-ins snd_beep / spk_tone / spk_silence /
 * pollkey — the exact Morph.h game API names.
 *
 * The 80 pushes are long notes (30 s) so nothing drains while the
 * loop runs; at most one timer tick can fire mid-loop and pop the
 * first note, so `queued` is 64..65 in Equinox OS and exactly 64 on the
 * host (which has no playback timer). spk_tone() at the end FLUSHES
 * the queue, so this test never leaves a half-hour of beeping behind.
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
    int ok = 0;
    int busy = 0;
    int i;
    int r;

    /* ---- 1: fill the queue past capacity: 64 accepted, rest EBUSY */
    for (i = 0; i < 80; i++) {
        r = snd_beep(440, 30000);
        if (r == 0) ok++;
        else busy++;
    }
    print("1 queued : "); prn(ok);
    print(" busy=");     prn(busy);
    print("\n");

    /* ---- 2: zero duration rejected with EINVAL (-6), even when full */
    r = snd_beep(440, 0);
    print("2 einval : "); prn(r); print("\n");                     /* -6 */

    /* ---- 3: manual speaker API flushes the queue and works (no hang) */
    spk_tone(500);
    spk_silence();
    print("3 manual : ok\n");

    /* ---- 4: pollkey unaffected by queue state: 0 = no key waiting */
    print("4 pollkey: "); prn(pollkey()); print("\n");             /* 0  */

    /* Compact summary: the QEMU harness asserts on this line (early
     * lines can scroll off during the in-OS compile). */
    print("SUM queued="); prn(ok);
    print(" busy=");      prn(busy);
    print(" einval=");
    prn(r);
    print("\n");
    print("RING DONE\n");
    return 0;
}
