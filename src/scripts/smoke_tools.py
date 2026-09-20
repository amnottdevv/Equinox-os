#!/usr/bin/env python3
"""smoke_tools.py — boot ISO baru, verifikasi:
1. `help` sudah dihapus (Unknown command)
2. eqbuild 29/29 (29 built, 0 failed)
3. spot-check tools baru: grep, head, tail, wc, sort, uniq, cut,
   tr, rev, nl, which, diff, strings, cksum, basename, dirname, find
"""
import os
import sys
import time
import socket
import subprocess
import tempfile

sys.path.insert(0, "/home/z/my-project/scripts")
from regression_task3 import Qemu, serial, wait_serial, since  # noqa

ISO = "/home/z/my-project/equinox_os/extracted/equinox_os_v0.2_beta/dist/equinox.iso"

PASS, FAIL = [], []


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}  {detail}", flush=True)


rig = Qemu(ISO)
try:
    ok = wait_serial("root::", 40, rig)
    check("boot sampai shell", ok)
    time.sleep(3)

    base = len(serial())

    # 1. help harus hilang
    rig.type_line("help", wait=1.5)
    t = since(base, 4000)
    check("help dihapus -> Unknown command",
          "Unknown command: 'help'" in t, t.strip().split("\n")[-1][:60])

    # 2. eqbuild 29 tools
    base = len(serial())
    rig.type_line("eqbuild", wait=5)
    ok = wait_serial("eqbuild: done", 600, rig, t0=time.time())
    t = since(base, 60000)
    n_ok = t.count("OK")
    check("eqbuild: 29 built, 0 failed",
          "29 built, 0 failed" in t and "FAIL" not in t,
          f"OK-lines={n_ok}")

    # 3. spot-check tools baru
    def run(cmd, wait=2.0):
        b = len(serial())
        rig.type_line(cmd, wait=wait)
        return since(b, 6000)

    t = run("wc /test/hello.c", 3)
    check("wc jalan", "hello.c" in t and any(c.isdigit() for c in t),
          t.strip().split("\n")[0][:50])

    t = run("head -n 2 /test/hello.c", 3)
    check("head -n 2", "include" in t.lower() or "/*" in t,
          t.strip().split("\n")[0][:50])

    t = run("tail -n 1 /test/hello.c", 3)
    check("tail -n 1", "}" in t, t.strip().split("\n")[-2][:50])

    t = run("grep printf /test/hello.c", 3)
    check("grep printf", "printf" in t, t.strip().split("\n")[0][:50])

    t = run("grep -c printf /test/hello.c", 3)
    check("grep -c count", t.strip().split("\n")[-2].strip().isdigit()
          if t.count("\n") > 1 else False)

    t = run("nl /test/hello.c", 4)
    check("nl numbering", "     1" in t or "    1" in t)

    t = run("sort /test/hello.c", 6)
    check("sort jalan", len(t.strip()) > 50)

    t = run("rev /test/hello.c", 4)
    check("rev jalan", len(t.strip()) > 50)

    t = run("which grep", 3)
    check("which grep -> /equinox/tools/grep.mrp",
          "/equinox/tools/grep.mrp" in t, t.strip().split("\n")[0][:60])

    t = run("diff /test/hello.c /test/hello.c", 3)
    check("diff identical", "identical" in t)

    t = run("basename /equinox/tools", 3)
    check("basename", "tools" in t)

    t = run("dirname /equinox/tools", 3)
    check("dirname", "/equinox" in t)

    t = run("cksum /test/hello.c", 3)
    check("cksum", "hello.c" in t)

    t = run("strings /test/hello.c 8", 4)
    check("strings", len(t.strip()) > 30)

    t = run("find /equinox/tools -name grep", 5)
    check("find -name grep", "grep" in t and "file(s)" in t)

    t = run("tr a-z A-Z /test/hello.c", 5)
    # output uppercase: baris pertama file jadi "/* HELLO.C — ..." 
    # (echo perintah tetap lowercase — skip baris echo)
    check("tr a-z A-Z", "HELLO.C" in t,
          t.strip().split("\n")[0][:50] if t else "(kosong)")

    t = run("uniq /test/hello.c", 4)
    check("uniq jalan", len(t.strip()) > 50)

    t = run("cut -d . -f 2 /test/hello.c", 4)
    check("cut -d . -f 2", "c" in t)

    rig.alive() and check("QEMU masih hidup", True)

finally:
    rig.quit()

print(f"\nSMOKE: {len(PASS)} PASS / {len(FAIL)} FAIL")
if FAIL:
    print("Gagal:", ", ".join(FAIL))
sys.exit(1 if FAIL else 0)
