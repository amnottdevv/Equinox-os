#!/usr/bin/env python3
"""eggkg_debug3.py — matriks variasi redirect echo untuk memencil
pemicu kernel panic CR2=0x5E03EA. Setiap kasus instan (tanpa mtcc)."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, ISO, serial, wait_serial

def step(rig, label, cmd, wait=1.5):
    n = len(serial())
    rig.type_line(cmd, wait=wait)
    t = serial()[n:]
    crash = "KERNEL PANIC" in t or "panic" in t.lower()
    print(f"{label}: {'CRASH!' if crash else 'ok'}  | {cmd!r} -> "
          f"{[l for l in t.splitlines() if l.strip()][:2]}", flush=True)
    return crash

def main():
    rig = Qemu(ISO)
    ok = wait_serial("user $", 150, rig)
    print("boot:", ok, flush=True)
    if not ok: rig.quit(); return 1

    if step(rig, "V1 baseline abs", "echo halo-dunia > /user/x2.txt"): return 9
    if step(rig, "V2 tiga-arg abs",  "echo satu dua tiga > /user/x3.txt"): return 9
    if step(rig, "V3 relatif /user", "echo relatif > relx.txt"): return 9
    if step(rig, "V4 cdir+cd",       "cdir eggt"): return 9
    if step(rig, "V5 cd runtime",    "cd /user/eggt"): return 9
    if step(rig, "V6 rel di runtime","echo hi > rx.txt"): return 9
    if step(rig, "V7 abs ke runtime","echo hi > /user/eggt/rx2.txt"): return 9
    if step(rig, "V8 append rel",    "echo hi-lagi >> rx.txt"): return 9
    if step(rig, "V9 echo 3arg rel", "echo p q r > pq.txt"): return 9
    print("SEMUA OK", flush=True)
    rig.quit()
    return 0

if __name__ == "__main__":
    sys.exit(main())
