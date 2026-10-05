#!/usr/bin/env python3
"""shell_feat_test.py — Fase 2 v0.8: shell hidup di QEMU.

  F1 echo.mrp redirect `>`      : echo halo > /user/x1.txt
  F2 builtin cat baca hasilnya  : cat /user/x1.txt -> "halo"
  F3 append `>>`                : echo lagi >> /user/x1.txt
  F4 glob `*` pada argumen      : cd /equinox; echo *.ruf
  F5 mtcc -make eq.ruf (41 job) -> 0 gagal  (butuh cat/grep/wc .mrp)
  F6 pipe cat|grep              : cat /equinox/eq.ruf | grep src
  F7 pipe cat|wc                : cat /equinox/eqmini.ruf | wc -c
  F8 redirect `<`               : wc -c < /equinox/eq.ruf
  F9 negatif: builtin di-pipe   : Qfs | grep x -> pesan .mrp
  F10 regresi: info menampilkan Fitur build 0.4 Beta
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32                     # noqa: E402

b32.SERIAL = "/tmp/shellfeat_serial.log"

SERIAL = b32.SERIAL
ISO = b32.ISO
check, serial, wait_serial = b32.check, b32.serial, b32.wait_serial


def window(base, pat, timeout, rig, limit=6000):
    t0 = time.time()
    while time.time() - t0 < timeout:
        t = serial()[base:]
        if pat in t:
            return t
        if not rig.alive():
            return t
        time.sleep(0.3)
    return serial()[base:base + limit]


def report():
    print()
    print(f"=== shellfeat: {len(b32.PASS)} PASS, {len(b32.FAIL)} FAIL")
    for n in b32.PASS:
        print("  PASS:", n)
    if b32.FAIL:
        print("  failed:", ", ".join(b32.FAIL))
    return 0 if not b32.FAIL else 1


def main():
    if not os.path.exists(ISO):
        print(f"[feat] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[feat] boot ...", flush=True)
    rig = b32.Qemu(ISO)
    try:
        ok = wait_serial("user $", 150, rig, t0=time.time())
        check("F0 boot ke shell", ok)
        if not ok:
            return report()

        # ---- F1: echo.mrp + redirect > -------------------------------
        base = len(serial())
        rig.type_line("echo halo-dunia > /user/x1.txt", wait=2.0)
        t = window(base, "-> /user/x1.txt", 15, rig)
        check("F1 `echo halo > /user/x1.txt` (redirect)",
              "-> /user/x1.txt" in t)

        # ---- F2: cat builtin membaca file hasil redirect -------------
        base = len(serial())
        rig.type_line("cat x1.txt", wait=2.0)
        t = window(base, "halo-dunia", 10, rig)
        check("F2 cat -> isi 'halo-dunia'", "halo-dunia" in t)

        # ---- F3: append >> -------------------------------------------
        base = len(serial())
        rig.type_line("echo baris-kedua >> /user/x1.txt", wait=2.0)
        t = window(base, "(append)", 15, rig)
        check("F3 `echo >> ` (append)", "(append)" in t)
        base = len(serial())
        rig.type_line("cat x1.txt", wait=2.0)
        t = window(base, "baris-kedua", 10, rig)
        check("F3b append terbaca", "baris-kedua" in t)

        # ---- F4: glob *.ruf pada argumen echo ------------------------
        base = len(serial())
        rig.type_line("cd /equinox", wait=1.0)
        rig.type_line("echo *.ruf", wait=2.0)
        t = window(base, "eqmini.ruf", 15, rig)
        check("F4 glob `echo *.ruf`",
              "eq.ruf" in t and "eqfull.ruf" in t and "eqmini.ruf" in t)

        # ---- F5: build userland penuh (cat/grep/wc .mrp jadi) --------
        base = len(serial())
        rig.type_line("mtcc -make /equinox/eq.ruf", wait=1.0)
        t = window(base, "0 gagal", 900, rig)
        check("F5 mtcc -make eq.ruf -> 0 gagal", "0 gagal" in t,
              [l.strip() for l in t.splitlines() if "gagal" in l][-1:]
              if "gagal" in t else None)

        # ---- F6: pipe cat | grep -------------------------------------
        base = len(serial())
        rig.type_line("cat /equinox/eq.ruf | grep src", wait=2.0)
        t = window(base, "src /equinox", 30, rig)
        check("F6 pipe `cat eq.ruf | grep src`", "src /equinox" in t)

        # ---- F7: pipe cat | wc ---------------------------------------
        base = len(serial())
        rig.type_line("cat /equinox/eqmini.ruf | wc -c", wait=2.0)
        t = window(base, "(stdin)", 30, rig)
        check("F7 pipe `cat eqmini.ruf | wc -c`", "(stdin)" in t)

        # ---- F8: redirect < ------------------------------------------
        base = len(serial())
        rig.type_line("wc -c < /equinox/eq.ruf", wait=2.0)
        t = window(base, "(stdin)", 30, rig)
        check("F8 `wc -c < eq.ruf`", "(stdin)" in t)

        # ---- F9: negatif — builtin tidak bisa di-pipe ----------------
        base = len(serial())
        rig.type_line("Qfs | grep x", wait=2.0)
        t = window(base, "bukan program .mrp", 15, rig)
        check("F9 `Qfs | grep x` -> pesan .mrp", "bukan program .mrp" in t)

        # ---- F10: info build marker ----------------------------------
        base = len(serial())
        rig.type_line("info", wait=2.0)
        t = window(base, "Fitur build 0.4 Beta", 10, rig)
        check("F10 info -> Fitur build 0.4 Beta", "Fitur build 0.4 Beta" in t)
    finally:
        rig.quit()
    return report()


if __name__ == "__main__":
    sys.exit(main())
