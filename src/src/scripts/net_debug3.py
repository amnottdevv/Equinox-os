#!/usr/bin/env python3
"""net_debug3 — ping dulu, ifconfig SETELAHNYA (fresh counters)."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_net_test import ISO
from net_trace import QemuTraced as Qemu

q = Qemu(ISO)
try:
    t0 = time.time()
    while time.time() - t0 < 60:
        if "Boot checks complete" in q.screen("-w"): break
        time.sleep(3)
    time.sleep(8)
    q.type_str("ping 10.0.2.2\n")
    time.sleep(20)                      # biarkan ping selesai + paket diproses
    q.type_str("ifconfig\n")
    time.sleep(3)
    txt = q.screen("-final")
    lines = [l for l in txt.split("\n") if l.strip()]
    for l in lines[-25:]:
        print(repr(l[:130]))
finally:
    q.kill()
