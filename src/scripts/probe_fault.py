#!/usr/bin/env python3
"""probe_fault.py — bedah runtime: run hello (fault eip=0), lalu baca
sisa exception frame di user_int_stack via QEMU monitor xp.

user_int_stack = 0x1b74a0, size 0x4000 → ESP0 = 0x1bb4a0
frame CPL3+#PF dari puncak: [ESP0-4]=SS [ESP0-8]=uESP [ESP0-12]=EFLAGS
[ESP0-16]=CS [ESP0-20]=EIP [ESP0-24]=errcode
"""
import sys, time
sys.path.insert(0, "scripts")
from qemu_v107_test import Qemu, ISO

ESP0 = 0x1BB4A0

q = Qemu(ISO)
try:
    time.sleep(16)
    q.type_str("\n")            # buang sisa apa pun (leading space race)
    time.sleep(2)
    q.type_str("hello\n")
    time.sleep(3)

    def xpw(addr, n):
        return q.cmd(f"xp /{n}wx {addr}", timeout=10)

    print("=== exception frame di user_int_stack (ESP0-0x28 .. ) ===")
    out = xpw(ESP0 - 0x28, 12)
    print(out)
    print("=== kode .mrp di 0x500010 (harusnya 55 89 e5 68 ...) ===")
    out = xpw(0x500010, 8)
    print(out)
    print("=== user stack top 0x911ff0 (stub retaddr + arg) ===")
    out = xpw(0x911FF0, 4)
    print(out)
    print("=== trampoline 0x900000 (harusnya b8 01 00 00 00 bb ...) ===")
    out = xpw(0x900000, 4)
    print(out)
    print("=== registers (CR0/CR3) ===")
    print(q.cmd("info registers", timeout=10))
finally:
    q.kill()
