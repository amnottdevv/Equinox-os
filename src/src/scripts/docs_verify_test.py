#!/usr/bin/env python3
"""docs_verify_test.py — verifikasi klaim docs pada ISO build v0.7.

Latar: user melapor "ga ada -x, dan equinoxinstall -compile ataupun
mtcc -make custom_script.ruf". Bukti strings: kernel ISO lama (tarball
asli) TIDAK memuat "-make"/"-build"/"ruf" (0 hit) — user kemungkinan
boot ISO lama. Test ini membuktikan ISO v0.7 memang punya semuanya,
termasuk .ruf buatan sendiri (dibuat via `save`) dan path relatif.

  D0 banner boot memuat "build v0.7"           (penanda baru)
  D1 `set -x` tanpa file -> "set: set -x butuh nama file"  (flag ADA)
  D2 skrip .es header-only via `set -x vt.es` -> "es: selesai"
  D3 `save mybuild.ruf << "src /equinox/libc"` -> "save: wrote"
  D4 `mtcc -make /mybuild.ruf`   (absolut) -> "0 gagal"
  D5 `cd /` + `mtcc -make mybuild.ruf` (RELATIF) -> "0 gagal"
  D6 `mtcc -make /nope.ruf` -> "cannot read"   (-make dikenali)
  D7 `info` -> "Fitur build 0.4 Beta"
  D8 `equinoxinstall -compile /equinox` -> "summary —"     (terakhir)
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32                     # noqa: E402

b32.SERIAL = "/tmp/docs_verify_serial.log"

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
    print(f"=== docs_verify: {len(b32.PASS)} PASS, {len(b32.FAIL)} FAIL")
    for name in b32.PASS:
        print("  PASS:", name)
    if b32.FAIL:
        print("  failed:", ", ".join(b32.FAIL))
    return 0 if not b32.FAIL else 1


def main():
    if not os.path.exists(ISO):
        print(f"[docs] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[docs] boot ...", flush=True)
    rig = b32.Qemu(ISO)
    try:
        ok = wait_serial("build v0.7", 150, rig, t0=time.time())
        check("D0 banner boot 'build v0.7'", ok)
        ok = wait_serial("user $", 60, rig)
        check("D0b boot ke shell", ok)
        if not ok:
            return report()

        # ---- D1: flag -x ADA pada set --------------------------------
        base = len(serial())
        rig.type_line("set -x", wait=1.5)
        t = window(base, "set: set -x butuh nama file", 10, rig)
        check("D1 `set -x` dikenali (minta nama file)", "butuh nama file" in t)

        # ---- D2: skrip .es via set -x --------------------------------
        base = len(serial())
        rig.type_line('save vt.es << "[Eqshell]"', wait=1.5)
        t = window(base, "save: wrote", 10, rig)
        check("D2a save vt.es", "save: wrote 'vt.es'" in t)
        base = len(serial())
        rig.type_line("set -x vt.es", wait=2.0)
        t = window(base, "es: selesai", 15, rig)
        check("D2b `set -x vt.es` -> es: selesai", "es: selesai" in t)

        # ---- D3: .ruf buatan sendiri (satu direktif src) -------------
        base = len(serial())
        rig.type_line('save mybuild.ruf << "src /equinox/libc"', wait=1.5)
        t = window(base, "save: wrote", 10, rig)
        check("D3 save mybuild.ruf (custom .ruf)", "save: wrote 'mybuild.ruf'" in t)

        # ---- D4: mtcc -make ABSOLUT ----------------------------------
        base = len(serial())
        rig.type_line("mtcc -make /mybuild.ruf", wait=1.0)
        t = window(base, "0 gagal", 300, rig)
        check("D4 `mtcc -make /mybuild.ruf` -> 0 gagal", "0 gagal" in t,
              [l.strip() for l in t.splitlines() if "gagal" in l][-1:])

        # ---- D5: mtcc -make RELATIF (dari cwd /) ---------------------
        base = len(serial())
        rig.type_line("cd /", wait=1.0)
        rig.type_line("mtcc -make mybuild.ruf", wait=1.0)
        t = window(base, "0 gagal", 300, rig)
        check("D5 `mtcc -make mybuild.ruf` (relatif) -> 0 gagal",
              "0 gagal" in t)

        # ---- D6: -make dikenali utk file hilang ----------------------
        base = len(serial())
        rig.type_line("mtcc -make /nope.ruf", wait=1.5)
        t = window(base, "cannot read", 10, rig)
        check("D6 `mtcc -make /nope.ruf` -> cannot read", "cannot read" in t)

        # ---- D7: info menampilkan level build ------------------------
        base = len(serial())
        rig.type_line("info", wait=2.0)
        t = window(base, "Fitur build 0.4 Beta", 10, rig)
        check("D7 `info` -> 'Fitur build 0.4 Beta'", "Fitur build 0.4 Beta" in t)

        # ---- D8: equinoxinstall -compile /equinox (paling berat) -----
        base = len(serial())
        rig.type_line("equinoxinstall -compile /equinox", wait=1.0)
        t = window(base, "summary —", 600, rig)
        line = [l.strip() for l in t.splitlines() if "summary" in l]
        check("D8 `equinoxinstall -compile /equinox` -> summary",
              "summary —" in t, line[-1:] if line else None)
    finally:
        rig.quit()
    return report()


if __name__ == "__main__":
    sys.exit(main())
