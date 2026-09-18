#!/usr/bin/env python3
"""net_debug.py v2 — proper waits, then ifconfig + ping step by step."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_net_test import ISO
from net_trace import QemuTraced as Qemu

def show(txt, tag):
    print(f"\n=== {tag} (non-empty) ===")
    for l in txt.split("\n"):
        if l.strip():
            print(repr(l[:110]))

q = Qemu(ISO)
try:
    # tunggu loading screen lalu 5 detik jeda
    t0 = time.time()
    while time.time() - t0 < 60:
        txt = q.screen("-wboot")
        if "Boot checks complete" in txt:
            break
        time.sleep(3)
    print("loading screen terlihat, tunggu 8s (skip wait + intro)…")
    time.sleep(8)
    txt = q.screen("-prompt")
    show(txt, "prompt state")

    q.type_str("ifconfig\n")
    time.sleep(3)
    show(q.screen("-ifconfig"), "after ifconfig")

    q.type_str("ping 10.0.2.2\n")
    for wait in (4, 6, 8, 10):
        time.sleep(wait)
        show(q.screen(f"-ping{wait}"), f"ping +{wait}s")
        txt = q.screen(f"-p{wait}b")
        if "ping statistics" in txt:
            break
finally:
    q.kill()
