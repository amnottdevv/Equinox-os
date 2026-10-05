#!/usr/bin/env python3
"""net2_probe.py — boot Equinox OS + screendump bertahap (debug crash v10.12)."""
import sys, time
sys.path.insert(0, "/home/z/morphos/scripts")
from qemu_net2_test import Qemu, screen_text

ISO = "/home/z/morphos/dist/morphos.iso"
OUT = "/tmp/net2_probe.txt"
q = Qemu(ISO)
buf = []
try:
    t0 = time.time()
    time.sleep(6)
    txt = screen_text(q.dump("-p6"))
    buf.append(f"=== t=6s (alive={q.proc.poll() is None}) ===\n" + txt)
    time.sleep(6)
    txt = screen_text(q.dump("-p12"))
    buf.append(f"=== t=12s (alive={q.proc.poll() is None}) ===\n" + txt)
    time.sleep(8)
    txt = screen_text(q.dump("-p20"))
    buf.append(f"=== t=20s (alive={q.proc.poll() is None}) ===\n" + txt)
    time.sleep(10)
    txt = screen_text(q.dump("-p30"))
    buf.append(f"=== t=30s (alive={q.proc.poll() is None}) ===\n" + txt)
finally:
    q.kill()

with open(OUT, "w") as f:
    f.write("\n".join(buf))
print(f"written {OUT}")

# tampilkan hanya baris non-kosong per snapshot
for snap in "\n".join(buf).split("=== t="):
    if not snap.strip():
        continue
    lines = [ln.rstrip() for ln in snap.split("\n") if ln.strip()]
    print("=== t=" + "\n".join(lines[:34]))
    print("~~~~")
