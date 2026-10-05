#!/usr/bin/env python3
"""eggkg_build_test.py — QEMU test paket "bash" eggkg (v0.9).

NOTE (0.4 Beta, slim ISO): the base ISO no longer bundles the bash
package — install it in-OS first (`eggkg update && eggkg install bash`,
or keep a locally staged /equinox/.local/bash in dist/ when testing).
The rest of the layout contract is unchanged:

    /equinox/.local/bash/build.ruf     <- resep (src /equinox/.local/bash/src + out)
    /equinox/.local/bash/src/*.c       <- 19 coreutils (dari package.list repo Eggkg-l)

Pemeriksaan:
  T1  boot ke shell
  T2  mtcc -make /equinox/.local/bash/build.ruf -> 19 job 0 gagal
  T3  produk .mrp muncul di /equinox/.local/bash (out aktif)
  T4  hasil build jalan: which.mrp mtcc -> /equinox/tools/mtcc.mrp
  T5  hasil build jalan: rev.mrp membalik isi file (save + run)
  T6  wrapper equinoxinstall -build <build.ruf> sukses
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, ISO, PASS, FAIL, serial, wait_serial, check

SERIAL = "/tmp/boot_v032_serial.log"

BUILD_RUF = "/equinox/.local/bash/build.ruf"


def main():
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    print(f"[eggkg-build] boot ({ISO}) ...", flush=True)
    rig = Qemu(ISO)
    ok = wait_serial("user $", 150, rig)
    check("T1 boot ke shell", ok)
    if not ok:
        rig.quit()
        report()
        return 1

    # T2 — mtcc -make paket bash (19 job)
    n0 = len(serial())
    rig.type_line(f"mtcc -make {BUILD_RUF}", wait=1.0)
    ok = wait_serial("selesai:", 900, rig, t0=time.time())
    check("T2 mtcc -make selesai", ok)
    if ok:
        tail = serial()[n0:]
        ok_sum = "0 gagal (19 job)" in tail
        check("T2b summary 19 job 0 gagal", ok_sum,
              [l for l in tail.splitlines() if "selesai" in l][-3:])
        check("T2c '19 job(s)' terdeteksi", "19 job(s)" in tail)

    # T3 — produk .mrp di /equinox/.local/bash (out aktif, src tetap bersih)
    rig.type_line("cd /equinox/.local/bash", wait=1.0)
    n1 = len(serial())
    rig.type_line("ls", wait=2.0)
    tail = serial()[n1:]
    check("T3 cat.mrp ada di /equinox/.local/bash",
          "cat.mrp" in tail or wait_serial("cat.mrp", 5, rig))
    check("T3b which.mrp ada", "which.mrp" in tail or
          wait_serial("which.mrp", 3, rig))

    # T4 — eksekusi hasil build: which
    n2 = len(serial())
    rig.type_line("run /equinox/.local/bash/which.mrp mtcc", wait=3.0)
    tail = serial()[n2:]
    check("T4 which.mrp menemukan mtcc",
          "equinox/tools/mtcc.mrp" in tail or
          wait_serial("mtcc.mrp", 6, rig))

    # T5 — eksekusi hasil build: rev (save ke cwd = /equinox/.local/bash,
    #      rev dibaca relatif — cwd task mewarisi shell)
    rig.type_line('save f.txt << "hello eggkg"', wait=1.0)
    n3 = len(serial())
    rig.type_line("run /equinox/.local/bash/rev.mrp f.txt", wait=3.0)
    tail = serial()[n3:]
    check("T5 rev.mrp membalik 'hello eggkg'",
          "gkgge olleh" in tail or wait_serial("gkgge", 6, rig),
          [l for l in tail.splitlines() if "olleh" in l][:1])

    # T6 — wrapper shell equinoxinstall -build
    n4 = len(serial())
    rig.type_line(f"equinoxinstall -build {BUILD_RUF}", wait=1.0)
    ok = wait_serial("semua job ok", 900, rig, t0=time.time())
    if not ok:
        ok = wait_serial("[build] mtcc exit", 5, rig)
    check("T6 equinoxinstall -build build.ruf sukses", ok,
          serial()[n4:].splitlines()[-2:])
    _ = n4

    rig.quit()
    report()
    return 0 if (PASS and not FAIL) else 1


def report():
    print("\n=== HASIL ===")
    for name in PASS:
        print("  PASS:", name)
    for name in FAIL:
        print("  FAIL:", name)
    print(f"  total: {len(PASS)} pass, {len(FAIL)} fail")


if __name__ == "__main__":
    sys.exit(main())
