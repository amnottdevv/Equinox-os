#!/usr/bin/env python3
"""
qemu_mget_test.py — Equinox OS v0.1 Beta `mget` smoke test.

Focused validation of the new URL-based HTTP client (per v0.1 scope;
the full v10.13 regression lives in qemu_net3_test.py):

  M1  boot reaches the shell prompt
  M2  `mget http://10.0.2.2:8022/data.json` -> HTTP 200, saved as
      data.json, content-type reported, `cat` shows the JSON tokens
  M3  `mget http://10.0.2.2/data.json -port 8033` (second server on a
      non-standard port) -> -port override works
  M4  `mget https://10.0.2.2:8022/data.json` (TLS client vs a plain
      HTTP server) -> clean TLS error, shell stays alive
  M5  `mget` with no args -> usage block
  M6  `help` mentions `mget <url>` and no longer mentions `wget`
  M7  `info` command prints "Equinox OS v0.2 Beta"

Usage: python3 qemu_mget_test.py [iso]
"""

import functools
import http.server
import os
import socketserver
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from qemu_net2_test import (  # noqa: E402
    Qemu, wait_text, PROMPT, WGET_PORT,
)

ROOT = os.path.abspath(os.path.join(HERE, ".."))
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "equinox.iso")

PORT2 = 8033
JSON_BODY = '{"ok":true,"n":42,"app":"equinox-mget"}'

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {name}"
          + (f"  -- {detail}" if detail and not cond else ""))


class DocServer(socketserver.TCPServer):
    allow_reuse_address = True


def serve_dir(docroot, port):
    # serve each docroot explicitly (independent of the process CWD)
    handler = functools.partial(http.server.SimpleHTTPRequestHandler,
                                directory=docroot)
    srv = DocServer(("127.0.0.1", port), handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


def main():
    if not os.path.exists(ISO):
        print("ISO not found:", ISO)
        sys.exit(2)

    d1 = tempfile.mkdtemp(prefix="mget-a-")
    with open(os.path.join(d1, "data.json"), "w") as f:
        f.write(JSON_BODY)
    d2 = tempfile.mkdtemp(prefix="mget-b-")
    with open(os.path.join(d2, "data.json"), "w") as f:
        f.write(JSON_BODY)

    cwd = os.getcwd()
    os.chdir(d1)
    s1 = serve_dir(d1, WGET_PORT)
    os.chdir(d2)
    s2 = serve_dir(d2, PORT2)
    os.chdir(cwd)

    print("Booting QEMU (user-net + ne2k_isa + hostfwd 8080->80)...")
    q = Qemu(ISO)
    try:
        # ---- M1: boot ----
        txt = wait_text(q, PROMPT, timeout=120, tag="-boot")
        check("M1 boot: shell prompt alive", PROMPT in txt)
        q.type_str("\n")
        time.sleep(0.8)

        # ---- M7: version via the `info` command (OCR of the ASCII
        #      emblem is unreliable, `info` prints plain text) ----
        q.type_str("info\n")
        time.sleep(1.5)
        scr = q.screen("-ver")
        check("M7 info shows Equinox OS v0.2 Beta",
              ("v0.2 Beta" in scr) and ("Equinox OS" in scr), scr[-300:])
        q.type_str("clear\n")
        time.sleep(0.8)

        # ---- M2: mget JSON via URL with :port ----
        q.type_str(f"mget http://10.0.2.2:{WGET_PORT}/data.json\n")
        time.sleep(6.0)
        txt = q.screen("-m2")
        ok2 = ("HTTP status 200" in txt and "data.json saved" in txt)
        check("M2 mget: HTTP 200 + data.json saved", ok2, txt[-600:])
        check("M2 mget: content-type reported", "application/json" in txt, txt[-600:])

        q.type_str("cat data.json\n")
        time.sleep(2.0)
        txt = q.screen("-cat")
        check("M2 cat: JSON body present",
              ("ok" in txt and "42" in txt and "equinox-mget" in txt), txt[-400:])

        # ---- M3: -port override against a second server ----
        q.type_str(f"mget http://10.0.2.2/data.json -port {PORT2}\n")
        time.sleep(6.0)
        txt = q.screen("-m3")
        ok3 = ("HTTP status 200" in txt and "data.json saved" in txt)
        check(f"M3 mget -port {PORT2}: override works", ok3, txt[-600:])

        # ---- M4: https vs plain-HTTP server -> clean TLS error ----
        # (v0.1 TLS: https is now attempted for real; against a server
        # that is not TLS the handshake must fail cleanly with a "TLS:"
        # diagnostic and the shell must stay alive for M5/M6)
        q.type_str("mget https://10.0.2.2:8022/data.json\n")
        time.sleep(25)
        txt = q.screen("-m4")
        check("M4 mget https to plain server: clean TLS error",
              "TLS:" in txt, txt[-400:])

        # ---- M5: usage block ----
        q.type_str("mget\n")
        time.sleep(2.0)
        txt = q.screen("-m5")
        check("M5 mget usage block",
              ("usage: mget <url>" in txt and "-port" in txt), txt[-500:])

        # ---- M6: help mentions mget, not wget ----
        q.type_str("help\n")
        time.sleep(3.0)
        txt = q.screen("-m6")
        check("M6 help: mget listed", "mget <url>" in txt, txt[-800:])
        check("M6 help: wget gone", "wget" not in txt, txt[-800:])
    finally:
        q.kill()
        s1.shutdown()
        s2.shutdown()

    passed = sum(1 for _, ok in results if ok)
    print(f"\n== mget smoke: {passed}/{len(results)} PASS ==")
    if passed != len(results):
        sys.exit(1)


if __name__ == "__main__":
    main()
