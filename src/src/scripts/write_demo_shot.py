#!/usr/bin/env python3
"""write_demo_shot.py — Equinox OS v0.2 Beta write-flow demo screenshot.

Boot 1: save (create) -> save (overwrite) -> ls -l -> xxd -> diskinfo
Boot 2: same disk, cat -> the file survived the reboot.
Output: dist/equinox_write_demo.png (side-by-side of both screens).
"""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from fat32_test import QemuDisk, wait_text, ISO, DISK
import subprocess

OUT = os.path.join(os.path.dirname(HERE), "dist", "equinox_write_demo.png")


def shot1():
    q = QemuDisk(ISO, DISK, net=False)
    try:
        wait_text(q, "FAT32: 'EQDISK' mounted at /mnt", timeout=240,
                  tag="-s0")
        wait_text(q, "root::users /user $", timeout=120, tag="-s1")
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("cd /mnt\n"); time.sleep(0.8)
        q.type_str('save demo.txt << "Equinox OS v0.2 writes FAT32"\n')
        time.sleep(1.2)
        q.type_str('save demo.txt << "overwritten in place, smaller"\n')
        time.sleep(1.2)
        q.type_str("ls -l\n")
        time.sleep(1.2)
        q.type_str("cat demo.txt\n")
        time.sleep(1.2)
        q.type_str("diskinfo\n")
        txt = wait_text(q, "read-write", timeout=60, tag="-s2")
        return q.dump("-final1")
    finally:
        q.kill()


def shot2():
    q = QemuDisk(ISO, DISK, net=False)
    try:
        wait_text(q, "FAT32: 'EQDISK' mounted at /mnt", timeout=240,
                  tag="-b0")
        wait_text(q, "root::users /user $", timeout=120, tag="-b1")
        q.type_str("clear\n"); time.sleep(0.5)
        # NOTE: the shell's cat/ls look up children of the CWD — cd first
        q.type_str("cd /mnt\n"); time.sleep(0.8)
        q.type_str("ls\n")
        time.sleep(1.0)
        q.type_str("cat demo.txt\n")
        txt = wait_text(q, "overwritten in place", timeout=90, tag="-b2")
        assert "overwritten in place" in txt, "persistence demo failed"
        return q.dump("-final2")
    finally:
        q.kill()


def combine(p1, p2):
    from PIL import Image
    a, b = Image.open(p1), Image.open(p2)
    w = a.width + b.width + 16
    h = max(a.height, b.height)
    canvas = Image.new("RGB", (w, h), (10, 8, 18))
    canvas.paste(a, (0, 0))
    canvas.paste(b, (a.width + 16, 0))
    canvas.save(OUT)
    print(f"[demo] wrote {OUT}")


if __name__ == "__main__":
    p1 = shot1()
    p2 = shot2()
    combine(p1, p2)
