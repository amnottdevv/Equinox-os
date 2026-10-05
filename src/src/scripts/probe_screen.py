#!/usr/bin/env python3
"""probe_screen.py — boot Equinox OS, eksekusi beberapa perintah, cetak OCR
layar lengkap untuk diagnosis harness (bukan test pass/fail)."""
import sys, time
sys.path.insert(0, "scripts")
from qemu_v107_test import Qemu, ISO, screen_text

q = Qemu(ISO)
try:
    time.sleep(14)
    q.type_str(" ")
    time.sleep(3)
    txt = q.screen("-p1")
    print("========== DUMP 1 (boot + prompt) ==========")
    print(txt)
    q.type_str("memmap\n"); time.sleep(2)
    txt = q.screen("-p2")
    print("========== DUMP 2 (memmap) ==========")
    print(txt)
    q.type_str("hello\n"); time.sleep(2)
    txt = q.screen("-p3")
    print("========== DUMP 3 (hello + readline) ==========")
    print(txt)
    q.type_str("morph\n"); time.sleep(2)
    txt = q.screen("-p4")
    print("========== DUMP 4 (setelah input nama) ==========")
    print(txt)
    q.type_str("run crashde.mrp\n"); time.sleep(3)
    txt = q.screen("-p5")
    print("========== DUMP 5 (crashde) ==========")
    print(txt)
finally:
    q.kill()
