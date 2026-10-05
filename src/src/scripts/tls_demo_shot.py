#!/usr/bin/env python3
"""tls_demo_shot.py — capture the TLS feature demo screenshot.
Boots the ISO, runs both flagship mget https:// commands, saves
equinox_tls_demo.png into /home/z/my-project/download/.
"""
import os
import sys
import time
import socket
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_net2_test import Qemu, PROMPT, ocr_screen  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, ".."))
ISO = os.path.join(ROOT, "dist", "equinox.iso")
OUT = "/home/z/my-project/download/equinox_tls_demo.png"

q = Qemu(ISO)
try:
    print("booting...", flush=True)
    t0 = time.time()
    while time.time() - t0 < 300:
        txt = q.screen("-b")
        if PROMPT in txt:
            break
        time.sleep(4)
    print("shell up; waiting 2s", flush=True)
    time.sleep(2)

    print("typing command 1 (verified chain)...", flush=True)
    q.type_str("mget https://raw.githubusercontent.com/torvalds/"
               "linux/master/README\n")
    t0 = time.time()
    while time.time() - t0 < 240:
        txt = q.screen("-w1")
        if "README saved" in txt:
            break
        time.sleep(4)
    time.sleep(2)

    print("typing command 2 (github fallback)...", flush=True)
    q.type_str("mget https://github.com/octocat/Hello-World\n")
    t0 = time.time()
    while time.time() - t0 < 300:
        txt = q.screen("-w2")
        if "Hello-World saved" in txt:
            break
        time.sleep(4)
    time.sleep(3)

    p = q.dump("-demo")
    subprocess.run(["python3", "-c",
                    "from PIL import Image; "
                    f"Image.open('{p}').save('{OUT}')"], check=True)
    print("saved:", OUT, flush=True)

    txt = q.screen("-final")
    lines = [l.rstrip() for l in txt.split("\n") if l.rstrip()]
    for l in lines[-16:]:
        print(l[:165], flush=True)
finally:
    q.kill()
print("done", flush=True)
