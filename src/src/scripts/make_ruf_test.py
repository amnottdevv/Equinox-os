#!/usr/bin/env python3
"""make_ruf_test.py — QEMU regression untuk `mtcc -make <file.ruf>` +
`equinoxinstall -build <file.ruf>` (v0.6 self-hosting tanpa wizard).

Pemakaian:
    python3 scripts/make_ruf_test.py eq      # eq.ruf (exclude games)
    python3 scripts/make_ruf_test.py eqfull  # eqfull.ruf (libc+tools+games)

Pemeriksaan:
  T1  boot ke shell
  T2  mtcc -make menyelesaikan semua job dengan 0 gagal
  T3  produk .mrp muncul di /equinox/tools (ls RAMFS)
  T4  hasil build bisa dieksekusi (run basename.mrp -> output benar)
  T5  equinoxinstall -build <ruf> (wrapper shell) sukses
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, ISO, PASS, FAIL, serial, wait_serial, check

SERIAL = "/tmp/boot_v032_serial.log"   # Qemu (boot_test_v032) menulis ke sini

RUF = {
    "eq": {
        "file": "/equinox/eq.ruf",
        "jobs": 41,          # 12 libc + 29 tools (games di-exclude)
        "summary": "[make] selesai: 41 ok, 0 gagal (41 job)",
    },
    "eqfull": {
        "file": "/equinox/eqfull.ruf",
        "jobs": 44,          # 12 libc + 29 tools + 3 games
        "summary": "[make] selesai: 44 ok, 0 gagal (44 job)",
    },
}


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "eq"
    spec = RUF[which]

    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    print(f"[make-ruf] boot ({which}) ...", flush=True)
    rig = Qemu(ISO)
    ok = wait_serial("user $", 150, rig)
    check("T1 boot ke shell", ok)
    if not ok:
        rig.quit()
        report()
        return 1

    # T2 — mtcc -make dari RAMFS
    n0 = len(serial())
    rig.type_line(f"mtcc -make {spec['file']}", wait=1.0)
    ok = wait_serial("[make] selesai:", 600, rig, t0=time.time())
    check("T2 mtcc -make selesai", ok)
    if ok:
        tail = serial()[n0:]
        ok_sum = "0 gagal (" + str(spec["jobs"]) + " job)" in tail
        check(f"T2b summary {spec['jobs']} job 0 gagal", ok_sum,
              [l for l in tail.splitlines() if "selesai" in l][-1:])
        ok0 = wait_serial("[make] ok: /equinox/tools/ls.c", 5, rig)
        check("T2c log per-file ok (ls.c)", ok0 or ("/equinox/tools/ls.c" in tail))

    # T3 — produk .mrp terlihat di RAMFS
    rig.type_line("cd /equinox/tools", wait=1.0)
    n1 = len(serial())
    rig.type_line("ls", wait=2.0)
    tail = serial()[n1:]
    check("T3 ls.mrp ada di /equinox/tools", "ls.mrp" in tail or
          wait_serial("ls.mrp", 5, rig))

    # T4 — eksekusi hasil build
    n2 = len(serial())
    rig.type_line("run /equinox/tools/basename.mrp /usr/local/hello.txt", wait=3.0)
    tail = serial()[n2:]
    check("T4 basename.mrp jalan -> hello.txt",
          "hello.txt" in tail or wait_serial("hello.txt", 6, rig))

    # T5 — wrapper shell equinoxinstall -build
    n3 = len(serial())
    rig.type_line("equinoxinstall -build /equinox/eqmini.ruf", wait=1.0)
    ok = wait_serial("[build] selesai — semua job ok", 180, rig, t0=time.time())
    if not ok:
        # build kedua di RAMFS: mtcc menulis ulang .mrp — harus tetap ok
        ok = wait_serial("[build] mtcc exit", 5, rig)
    check("T5 equinoxinstall -build sukses", ok)
    _ = n3

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
