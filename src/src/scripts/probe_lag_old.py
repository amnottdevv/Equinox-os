#!/usr/bin/env python3
"""probe_lag_old.py — bukti kuantitatif LAG di ISO v0.4.1 (desktop v0.2).

Metode: boot ISO lama -> desktop -> gerak kursor jauh -> tunggu 0.6s ->
screendump -> cari piksel putih kursor di posisi TUJUAN. Pada renderer
full-frame-per-gerakan (ThorVG + flip 4MB), kursor tertinggal jauh;
desktop v0.3 (layer-cache) langsung sampai. Juga ukur waktu sampai
kursor terlihat di posisi via polling dump.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, read_ppm, check, PASS, FAIL, serial, wait_serial

ISO_OLD = "/tmp/my-project/download/equinox_os_v0.4.1_desktop.iso"


def count_white(img, x0, x1, y0, y1):
    w, h, data = img
    cnt = 0
    for y in range(y0, y1):
        base = y * w
        for x in range(x0, x1):
            i = (base + x) * 3
            if data[i] > 215 and data[i + 1] > 215 and data[i + 2] > 215:
                cnt += 1
    return cnt


def mouse_move(q, dx, dy, step=120):
    while dx != 0 or dy != 0:
        sx = max(-step, min(step, dx))
        sy = max(-step, min(step, dy))
        q.cmd(f"mouse_move {sx} {sy}", timeout=0.25)
        dx -= sx; dy -= sy
        time.sleep(0.06)


def main():
    q = Qemu(ISO_OLD)
    try:
        ok = wait_serial("user $", 40, rig=q)
        check("boot lama ke shell", ok)
        q.type_line("desktop", wait=5.0)
        check("desktop v0.2 lama aktif", wait_serial("desktop aktif", 30, rig=q))
        time.sleep(2.0)

        # gerak kursor 500px ke kiri-atas (dari center 680,384 -> 180,300)
        t0 = time.time()
        mouse_move(q, -500, -84)
        # segera dump (tanpa jeda): kursor seharusnya TERTINGGAL
        p = q.dump("oldlag1")
        img = read_ppm(p)
        # posisi tujuan (180..200, 300..316) vs titik tengah jalur (~430,342)
        at_target = count_white(img, 168, 216, 292, 320)
        at_mid = count_white(img, 418, 466, 334, 362)
        check("LAG: kursor TIDAK sampai target segera (bukti berat)",
              at_target < 10, f"white@target={at_target}")
        print(f"  (white di titik tengah jalur: {at_mid} — kursor masih di jalur)")

        # tunggu 2.5s — akhirnya sampai?
        time.sleep(2.5)
        p = q.dump("oldlag2")
        img = read_ppm(p)
        at_target = count_white(img, 168, 216, 292, 320)
        check("kursor sampai target setelah 2.5 s", at_target >= 10,
              f"white@target={at_target}")

        # ukur: berapa lama sampai frame BARU muncul setelah gerakan?
        # kirim gerakan kecil lalu polling dump 250ms — hitung delay
        t0 = time.time()
        mouse_move(q, 300, 0)
        seen = None
        for i in range(28):  # sampai 7 detik
            time.sleep(0.25)
            p = q.dump(f"oldpoll{i}")
            img = read_ppm(p)
            # area tujuan (980..1100, 300..316)
            if count_white(img, 968, 1116, 292, 320) >= 10:
                seen = time.time() - t0
                break
        check("delay kursor sampai posisi (ukur)", seen is not None,
              f"{seen:.2f} s" if seen else "> 7 s")
    finally:
        q.quit()
    print()
    print(f"PASS {len(PASS)} / FAIL {len(FAIL)}")


if __name__ == "__main__":
    main()
