#!/usr/bin/env python3
"""Bonus probe: doom -warp 1 -> E1M1 render + movement (arrow taps) proof."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_doom_test import (Qemu, screen_text, ppm_stats, frame_diff,
                            ppm_to_png, ISO, PROOF_DIR)

q = Qemu(ISO)
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
q.type_str("doom -warp 1\n")
level = None
for i in range(120):
    time.sleep(0.7)
    d = q.dump("")
    s = screen_text(d)
    st = ppm_stats(d)
    if "Error" in s:
        print("ERROR:", [l for l in s.splitlines() if "Error" in l][:2])
        break
    # level view: letterboxed + painted (any in-game screen)
    if st["side_black"] and st["center_ink"] > 0.5 and st["colors"] >= 40:
        level = d
        print(f"t{i:03d}: view up (colors={st['colors']} ink={st['center_ink']:.2f})")
        break

try:
    if not level:
        print("NO LEVEL VIEW")
    else:
        # let it settle, snapshot A
        time.sleep(3)
        a = q.dump("pre")
        # walk forward: 30 quick Up taps (each = short keydown->keyup burst)
        for _ in range(30):
            q.cmd("sendkey up", timeout=5)
            time.sleep(0.05)
        time.sleep(2)
        b = q.dump("post")
        diff = frame_diff(a, b)
        print(f"movement diff = {diff:.3%}")
        # turn right too
        for _ in range(20):
            q.cmd("sendkey right", timeout=5)
            time.sleep(0.05)
        time.sleep(2)
        c = q.dump("post2")
        diff2 = frame_diff(b, c)
        print(f"turn diff = {diff2:.3%}")
        os.makedirs(PROOF_DIR, exist_ok=True)
        out = os.path.join(PROOF_DIR, "doom_e1m1.png")
        ppm_to_png(c, out)
        print("proof:", out)
        print("GAMEPLAY:", "MOVING" if diff > 0.01 and diff2 > 0.01 else
              ("STATIC (check input)" if diff < 0.01 else "PARTIAL"))
finally:
    q.kill()
