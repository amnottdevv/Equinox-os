#!/usr/bin/env python3
"""net2_httpd_debug.py — debug inbound hostfwd -> guest :80."""
import subprocess, sys, time, socket, tempfile, os
sys.path.insert(0, "/home/z/morphos/scripts")
from qemu_net2_test import Qemu, screen_text, PROMPT, wait_text

ISO = "/home/z/morphos/dist/morphos.iso"
PCAP = "/tmp/hostfwd.pcap"

q = Qemu(ISO)
try:
    txt = wait_text(q, PROMPT, timeout=120, tag="-dbg")
    print("boot ok:", PROMPT in txt)
    q.type_str("\n"); time.sleep(0.5)

    print("--- curl attempt 1 (verbose) ---")
    r = subprocess.run(["curl", "-4", "-sv", "--max-time", "12",
                        "http://127.0.0.1:8080/"],
                       capture_output=True, text=True)
    print("rc=", r.returncode)
    print(r.stderr[-1200:])
    print("body len:", len(r.stdout))

    time.sleep(1)
    print("--- curl attempt 2 ---")
    r = subprocess.run(["curl", "-4", "-s", "--max-time", "8",
                        "http://127.0.0.1:8080/test/netinfo.c"],
                       capture_output=True)
    print("rc=", r.returncode, "body len:", len(r.stdout))

    q.type_str("httpd\n"); time.sleep(1.5)
    txt = q.screen("-hstat2")
    for ln in txt.split("\n"):
        if "httpd" in ln.lower() or "hits" in ln.lower():
            print("SCREEN:", ln.rstrip())

    q.type_str("ifconfig\n"); time.sleep(1.5)
    txt = q.screen("-ifcfg")
    for ln in txt.split("\n"):
        if "RX" in ln or "inet" in ln:
            print("SCREEN:", ln.rstrip())
finally:
    q.kill()
