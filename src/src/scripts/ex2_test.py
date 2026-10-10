#!/usr/bin/env python3
"""ex2_test.py — extended 0.5: fork/sbrk/mmap/socket against the REAL kernel.

The host suite (scripts/tcc_host_test/run_tests.py) already runs the same
program `tools_user/ex2.c` through the x86 interpreter; this script is the
integration half: boot the ISO in QEMU, compile+run the test IN the OS
(`mtcc /equinox/tools/ex2.c`) and assert on the serial log:

  T1  boot to the shell
  T2  the program compiles and runs to `ex2 RESULT fails=0`
  T3  every [PASS] line is there, NO [FAIL] at all
  T4  COW isolation + wait() reaping (the fork contract)
  T5  sbrk/mmap contract
  T6  socket() opened (net() may SKIP: the harness has no HTTP server)
  T7  the kernel stayed alive — no page fault / faulted task

Requires dist/equinox/tools/mtcc.mrp in the ISO (build it first):
    make mtcc && make
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from boot_test_v032 import (Qemu, PASS, FAIL, serial, wait_serial, check,
                            since, ISO, SERIAL)

MTCC_CMD = "mtcc /equinox/tools/ex2.c"
RESULT = "ex2 RESULT fails=0"

# The guest reaches the host at 10.0.2.2 (QEMU user-net gateway); a tiny
# HTTP server there turns the socket check into a real round-trip. When
# slirp refuses the connect the test accepts the documented SKIP path.
HOST_HTTP_PORT = 8081

# [PASS] lines the kernel run must produce (order-independent).
EXPECT_PASS = [
    "[PASS] mmap returned a window",
    "[PASS] mmap window writable (page faulted in)",
    "[PASS] sbrk(0) reports the program break",
    "[PASS] sbrk(8192) returns the OLD break",
    "[PASS] break advanced by 8192",
    "[PASS] heap page inside the break is writable",
    "[PASS] wait() returned the forked child",
    "[PASS] child exit status is 0",
    "[PASS] parent page unchanged (COW isolation)",
    "[PASS] socket(AF_INET, SOCK_STREAM) opened",
]


def type_verified(rig, text, attempts=3, wait=2.0):
    """Type a line and confirm it actually landed in the guest.

    sendkey is fire-and-forget: a line that gets dropped (busy shell,
    keyboard ring overflow) just times out 3 minutes later with no
    clue. Flushing with Enter first clears any half-typed line.
    """
    for attempt in range(1, attempts + 1):
        if attempt > 1:
            rig.sendkey("ret", wait=1.5)      # drop a partial line
        base = len(serial())
        rig.type_line(text, wait=wait)
        if text in since(base, 4096):
            return True
        print(f"[ex2] keystrokes did not land (try {attempt}/{attempts}): "
              f"{text}", flush=True)
    return False


def start_host_http():
    """Serve /ex2.txt on 127.0.0.1:8081 so the guest's net() has a target."""
    import http.server
    import socketserver
    import tempfile
    import threading

    root = tempfile.mkdtemp(prefix="ex2_http-")
    with open(os.path.join(root, "ex2.txt"), "w") as f:
        f.write("equinox-ex2-ok\n")

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=root, **kw)

        def log_message(self, *a):     # keep the test output clean
            pass

    srv = socketserver.TCPServer(("127.0.0.1", HOST_HTTP_PORT), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    if not os.path.exists(ISO):
        print(f"[ex2] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    httpd = start_host_http()
    print("[ex2] boot ...", flush=True)
    rig = Qemu(ISO)
    try:
        ok = wait_serial("user $", 180, rig)
        check("T1 boot to shell", ok)
        if not ok:
            print(serial()[-2500:])
            return 1

        # the in-OS compiler ships as a prebuilt .mrp (make mtcc)
        base = len(serial())
        type_verified(rig, "ls /equinox/tools", wait=4)
        tl = since(base, 8000)
        check("T1b mtcc.mrp in the image", "mtcc.mrp" in tl,
              tl.strip().replace("\n", " | ")[:120])

        print(f"[ex2] {MTCC_CMD} ...", flush=True)
        base = len(serial())
        type_verified(rig, MTCC_CMD, wait=2)
        done = wait_serial(RESULT, 180, rig)
        t = since(base, 40000)
        check("T2 runs to completion", done, t.strip().replace("\n", " | ")[:160])

        check("T3 no [FAIL] line", "[FAIL]" not in t)
        for line in EXPECT_PASS:
            check(f"T3 {line[7:]}", line in t)

        # the fork contract, spelled out
        check("T4 parent page unchanged (COW)", "[PASS] parent page unchanged (COW isolation)" in t)
        check("T4 wait() reaped the child", "[PASS] wait() returned the forked child" in t)
        check("T4 child wrote its own copy", "child: wrote 87654321" in t)

        # the memory contract
        check("T5 sbrk moved the break", "[PASS] break advanced by 8192" in t)
        check("T5 mmap window faulted in", "[PASS] mmap window writable (page faulted in)" in t)

        # socket: connect may SKIP when no host HTTP server is present
        check("T6 socket fd opened", "[PASS] socket(AF_INET, SOCK_STREAM) opened" in t)
        roundtrip = "[PASS] socket round-trip from the HTTP server" in t
        skip = "[SKIP] net() connect refused" in t
        check("T6 net() round-trip or clean SKIP", roundtrip or skip,
              "round-trip" if roundtrip else ("SKIP" if skip else "neither"))
        check("T6 no [FAIL] in the socket block", "[FAIL] socket" not in t)

        # the kernel must still be healthy after the child exits.
        # (don't grep for "faulted" — the test's own PASS text contains it)
        crash = ("Page Fault" in t or "[user] program faulted" in t
                 or "KERNEL PANIC" in t or "[PANIC-RAW]" in t)
        check("T7 kernel survived fork/COW/socket", not crash)
        check("T7 shell alive", rig.alive())
        rig.type_line("ls /equinox/tools", wait=3)
        check("T7 shell answers after the test", "ex2.c" in since(base, 40000))
    finally:
        rig.quit()
        httpd.shutdown()

    print(f"\n[ex2] RESULT: {len(PASS)} PASS, {len(FAIL)} FAIL")
    if FAIL:
        print("[ex2] failed:", ", ".join(FAIL))
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
