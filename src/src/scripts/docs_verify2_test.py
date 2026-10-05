#!/usr/bin/env python3
"""docs_verify2_test.py — skenario PERSIS user untuk `mtcc -make`.

Round-1 menemukan: cwd awal shell = /user (bukan /). File .ruf buatan
user via `save` mendarat di /user. Round-1 salah menebak path (/...).
Round-2 menguji path yang BENAR — relatif dari cwd dan absolut:

  V1 boot ke shell (prompt /user)
  V2 `save custom_script.ruf << "src /equinox/libc"`  -> /user/custom_script.ruf
  V3 `mtcc -make custom_script.ruf`        (RELATIF, cwd /user) -> 0 gagal
  V4 `mtcc -make /user/custom_script.ruf`  (ABSOLUT benar)     -> 0 gagal
  V5 `mtcc -make /custom_script.ruf`       (ABSOLUT salah)     -> cannot read
     (dokumentasi: ini penyebab "ga bisa" — file bukan di /)
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32                     # noqa: E402

b32.SERIAL = "/tmp/docs_verify2_serial.log"

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
    print(f"=== docs_verify2: {len(b32.PASS)} PASS, {len(b32.FAIL)} FAIL")
    for name in b32.PASS:
        print("  PASS:", name)
    if b32.FAIL:
        print("  failed:", ", ".join(b32.FAIL))
    return 0 if not b32.FAIL else 1


def main():
    if not os.path.exists(ISO):
        print(f"[docs2] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[docs2] boot ...", flush=True)
    rig = b32.Qemu(ISO)
    try:
        ok = wait_serial("user $", 150, rig, t0=time.time())
        check("V1 boot ke shell", ok)
        if not ok:
            return report()

        # ---- V2: .ruf custom di cwd /user ----------------------------
        base = len(serial())
        rig.type_line('save custom_script.ruf << "src /equinox/libc"', wait=1.5)
        t = window(base, "save: wrote", 10, rig)
        check("V2 save custom_script.ruf di /user",
              "save: wrote 'custom_script.ruf'" in t)

        # ---- V3: RELATIF dari cwd /user (skenario user) --------------
        base = len(serial())
        rig.type_line("mtcc -make custom_script.ruf", wait=1.0)
        t = window(base, "0 gagal", 300, rig)
        err = [l.strip() for l in t.splitlines() if "cannot read" in l]
        check("V3 `mtcc -make custom_script.ruf` (relatif) -> 0 gagal",
              "0 gagal" in t, err[:1])

        # ---- V4: ABSOLUT dengan path benar ---------------------------
        base = len(serial())
        rig.type_line("mtcc -make /user/custom_script.ruf", wait=1.0)
        t = window(base, "0 gagal", 300, rig)
        check("V4 `mtcc -make /user/custom_script.ruf` -> 0 gagal",
              "0 gagal" in t)

        # ---- V5: ABSOLUT salah (penyebab kebingungan) ----------------
        base = len(serial())
        rig.type_line("mtcc -make /custom_script.ruf", wait=1.5)
        t = window(base, "cannot read", 10, rig)
        check("V5 path salah -> 'cannot read' (pesan jelas)",
              "cannot read '/custom_script.ruf'" in t)
    finally:
        rig.quit()
    return report()


if __name__ == "__main__":
    sys.exit(main())
