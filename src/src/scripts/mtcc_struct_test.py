#!/usr/bin/env python3
"""mtcc_struct_test.py — in-OS regression untuk mtcc Stage 2 + Stage 3.

Menjalankan mtcc DI DALAM QEMU (bukan di host) supaya parser/codegen yang
sama diuji lewat jalur nyata: RAMFS -> mtcc -> .mrp -> mrp_run.

  S1  boot ke shell
  S2  mtcc /test/sinit.c        Stage 3: struct/union INITIALIZER
                                (global, local, nested, array-of-struct,
                                 union, char field, char* = "literal")
  S3  mtcc /test/struct.c       Stage 2: struct/union akses . dan ->
  S4  mtcc /test/swenum.c       Stage 2: enum + switch/case/default
  S5  mtcc /test/mf_main.c /test/mf_helper.c
                                Stage 3: MULTI-FILE link in-OS
                                (panggilan lintas file + extern global +
                                 #include "mf_shared.h" relatif + guard
                                 morph.h di dua file)
  S6  mtcc -c -o /mfprog.mrp <dua file>  lalu run /mfprog.mrp
                                Stage 3: tulis .mrp multi-file & eksekusi
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32                     # noqa: E402

b32.SERIAL = "/tmp/mtcc_struct_serial.log"
SERIAL = b32.SERIAL
ISO = b32.ISO
check, serial, wait_serial = b32.check, b32.serial, b32.wait_serial


def window(base, pat, timeout, rig, limit=6000):
    """Tunggu `pat` muncul di log serial SETELAH posisi `base`."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        t = serial()[base:]
        if pat in t:
            return t
        if not rig.alive():
            return t
        time.sleep(0.3)
    return serial()[base:base + limit]


def run_cmd(rig, cmd, want, timeout, wait=2.0):
    """Ketik `cmd`, tunggu `want` muncul; return potongan log + status."""
    base = len(serial())
    rig.type_line(cmd, wait=wait)
    t = window(base, want, timeout, rig)
    return t, want in t


def main():
    if not os.path.exists(ISO):
        print(f"[mtcc-test] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[mtcc-test] boot ...", flush=True)
    rig = b32.Qemu(ISO)
    ok = wait_serial("user $", 150, rig, t0=time.time())
    check("S1 boot ke shell", ok)
    if not ok:
        rig.quit()
        report()
        return 1

    # ---- S2: struct/union initializer -----------------------------------
    t, hit = run_cmd(rig, "mtcc /test/sinit.c",
                     "mix=65,7,66,8 m=67,9,68,10 hp=hi3", 90)
    check("S2 sinit.c (initializer struct/union)",
          hit and "g=10,20 p=5,6 l=2,3,7 loc=7,10,11 arr=1,3 u=99" in t,
          [l.strip() for l in t.splitlines() if "g=" in l or "mix=" in l][:2])

    # ---- S3: struct access . / -> --------------------------------------
    t, hit = run_cmd(rig, "mtcc /test/struct.c", "rc=9,111,222", 90)
    check("S3 struct.c (akses . dan ->)",
          hit and "p=(3,7)" in t and "sum=60" in t,
          [l.strip() for l in t.splitlines() if "p=" in l][:1])

    # ---- S4: enum + switch ---------------------------------------------
    t, hit = run_cmd(rig, "mtcc /test/swenum.c", "weekend=0", 90)
    check("S4 swenum.c (enum + switch)",
          hit and "RED=0 GREEN=5 BLUE=6" in t and "sw=222" in t,
          [l.strip() for l in t.splitlines() if "RED=" in l][:1])

    # ---- S5: multi-file link (compile & run sekaligus) ------------------
    t, hit = run_cmd(rig,
                     "mtcc /test/mf_main.c /test/mf_helper.c",
                     "dx=6 total=42 done", 120, wait=3.0)
    check("S5 multi-file link (2 .c + 1 .h in-OS)",
          hit and "undefined reference" not in t and
          "duplicate global" not in t and
          "cannot open include file" not in t,
          [l.strip() for l in t.splitlines()
           if "dx=" in l or "error" in l][:2])

    # ---- S6: tulis .mrp multi-file, lalu run ----------------------------
    t, hit = run_cmd(rig,
                     "mtcc -c -o /mfprog.mrp /test/mf_main.c /test/mf_helper.c",
                     "/mfprog.mrp", 120, wait=3.0)
    check("S6a mtcc -c -o (tulis .mrp multi-file)",
          hit and "error" not in t.lower(),
          [l.strip() for l in t.splitlines() if "mrp" in l.lower()][-2:])

    t, hit = run_cmd(rig, "run /mfprog.mrp", "dx=6 total=42 done", 60)
    check("S6b run /mfprog.mrp", hit,
          [l.strip() for l in t.splitlines() if "dx=" in l][:1])

    rig.quit()
    report()
    return 1 if b32.FAIL else 0


def report():
    print(f"\n[mtcc-test] RESULT: {len(b32.PASS)} PASS, {len(b32.FAIL)} FAIL")
    for name in b32.PASS:
        print("  PASS:", name)
    if b32.FAIL:
        print("  failed:", ", ".join(b32.FAIL))


if __name__ == "__main__":
    sys.exit(main())
