#!/usr/bin/env python3
"""Probe cepat: boot -> doom -> poll OCR 0.5s, dump semua layar, cari 'Error'."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_doom_test import Qemu, screen_text, ppm_stats, ISO

q = Qemu(ISO)
try:
    booted = False
    for _ in range(40):
        time.sleep(1)
        s = q.screen("bpoll")
        if "root::users" in s and "$" in s:
            booted = True
            break
        if "press any key to skip" in s:
            q.cmd("sendkey spc", timeout=5)
    print("booted:", booted)
    q.type_str("doom\n")
    outdir = "/tmp/doomprobe"
    os.makedirs(outdir, exist_ok=True)
    for i in range(90):
        time.sleep(0.7)
        d = q.dump("")
        s = screen_text(d)
        st = ppm_stats(d)
        txtfile = f"{outdir}/t{i:03d}.txt"
        open(txtfile, "w").write(s)
        interesting = [l for l in s.splitlines()
                       if any(k in l for k in ("Error", "error", "IWAD", "wad",
                                               "Init", "init", "mrp", "DOOM",
                                               "zone", "heap"))]
        print(f"--- t{i:03d} colors={st['colors']} ink={st['center_ink']:.2f} "
              f"side_black={st['side_black']}")
        for l in interesting[:6]:
            print("   |", l[:150])
        if "recursive call to I_Error" in s:
            print("=== RECURSIVE I_Error seen, stopping ===")
            break
        if st["side_black"] and st["center_ink"] > 0.05 and st["colors"] >= 40:
            print("=== TITLE SCREEN DETECTED ===")
            break
finally:
    q.kill()
