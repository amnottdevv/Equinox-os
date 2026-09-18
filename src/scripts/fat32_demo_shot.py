#!/usr/bin/env python3
"""fat32_demo_shot.py — the v0.2 delivery screenshot.

Boot ISO+disk, run: cd /mnt ; ls -l ; diskinfo ; cat test files,
then screendump -> PNG. Saves to dist/equinox_disk_demo.png.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from fat32_test import QemuDisk, ISO, DISK, wait_text  # noqa: E402

OUT = os.path.join(ROOT, "dist", "equinox_disk_demo.png")


def main():
    q = QemuDisk(ISO, DISK, net=False)
    try:
        # capture the boot log with the mount lines first
        boot = wait_text(q, "mounted at /mnt", timeout=240, tag="-boot")
        wait_text(q, "root::users /user $", timeout=120, tag="-sh")
        time.sleep(1)
        q.type_str("clear\n")
        time.sleep(0.6)
        # a compact demo script
        q.type_str("cd /mnt\n")
        time.sleep(0.8)
        q.type_str("ls -l\n")
        wait_text(q, "4196020", timeout=60, tag="-lsl")
        q.type_str("cat hello from equinox.txt\n")
        wait_text(q, "LONG name", timeout=60, tag="-cat")
        q.type_str("diskinfo\n")
        wait_text(q, "arena", timeout=60, tag="-di")
        time.sleep(1.0)
        path = q.dump("-demo")
        try:
            from PIL import Image
            Image.open(path).save(OUT)
        except Exception:
            os.replace(path, OUT)
        print("saved:", OUT)
    finally:
        q.kill()


if __name__ == "__main__":
    main()
