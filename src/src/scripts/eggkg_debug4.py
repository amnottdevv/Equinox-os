#!/usr/bin/env python3
"""eggkg_debug4.py — ukur heap kernel nyata sebelum/sesudah install."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, ISO, serial, wait_serial

def grab(rig, label, cmd, pat, tmo=200):
    n = len(serial())
    rig.type_line(cmd, wait=2.0)
    ok = wait_serial(pat, tmo, rig, t0=time.time())
    t = serial()[n:]
    heap = [l for l in t.splitlines() if "Heap" in l]
    print(f"{label}: ok={ok} {heap}", flush=True)
    return t

def main():
    rig = Qemu(ISO)
    ok = wait_serial("user $", 150, rig)
    print("boot:", ok, flush=True)
    if not ok: rig.quit(); return 1

    grab(rig, "boot", "info", "Heap")
    grab(rig, "update", "eggkg update /equinox/repo/package.list", "paket di index")
    grab(rig, "install", "eggkg install bash -y", "terpasang")
    grab(rig, "post-install", "info", "Heap")
    grab(rig, "update-lagi", "eggkg update", "paket di index")
    grab(rig, "post-update2", "info", "Heap")
    rig.quit()
    return 0

if __name__ == "__main__":
    sys.exit(main())
