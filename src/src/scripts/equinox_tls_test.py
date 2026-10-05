#!/usr/bin/env python3
"""
equinox_tls_test.py — Equinox OS v0.1 Beta TLS smoke test (mget https://).

Validates the BearSSL 0.6 integration end-to-end under QEMU user-net
(slirp), from the built ISO:

  T1  boot reaches the shell prompt
  T2  `mget` (no args) -> usage block now documents https://
  T3  `mget https://example.com/` -> TLS handshake + HTTP 200 + saved
      (whatever verify mode example.com's chain lands in)
  T4  `mget https://raw.githubusercontent.com/torvalds/linux/master/README`
      -> strict chain verification against ISRG Root X1 + HTTP 200 +
      saved (README)
  T5  `mget https://github.com/octocat/Hello-World`
      -> github.com's P-384 Sectigo chain cannot be verified ->
      automatic fallback (warning printed) + HTTP 200 + saved

Usage: python3 equinox_tls_test.py [iso]
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from qemu_net2_test import Qemu, wait_text, PROMPT  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, ".."))
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "equinox.iso")

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {name}"
          + (f"  -- {detail}" if detail and not cond else ""))


def run_cmd(q, cmd, expect, timeout=180, tag="", clear=True):
    """Clear the screen, type a command + Enter, wait for `expect`.

    The screen is cleared first so `expect` can only match THIS
    command's output (previous tests' text stays visible otherwise
    and the needle would match stale output instantly)."""
    if clear:
        q.type_str("clear\n")
        time.sleep(2.5)
    q.type_str(cmd + "\n")
    time.sleep(0.5)
    return wait_text(q, expect, timeout=timeout, tag=tag)


def main():
    if not os.path.exists(ISO):
        print("ISO not found:", ISO)
        sys.exit(2)

    print("Booting QEMU (user-net + ne2k_isa, outbound TLS via slirp)...")
    q = Qemu(ISO)
    try:
        # ---- T1: boot to shell ----
        txt = wait_text(q, PROMPT, timeout=300, tag="-boot")
        check("T1 boot reaches shell", PROMPT in txt, txt[-200:])

        # ---- T2: usage block documents https ----
        q.type_str("mget\n")
        time.sleep(0.5)
        txt = wait_text(q, "usage", timeout=30, tag="-usage")
        ok = ("https://host" in txt and "BearSSL" in txt)
        check("T2 mget usage documents https", ok, txt[-400:])

        # ---- T3: https://example.com (any verify outcome) ----
        txt = run_cmd(q, "mget https://example.com/",
                      "saved (HTTP", timeout=240, tag="-ex", clear=True)
        ok = ("HTTP status 200" in txt and "saved (HTTP" in txt
              and "TLS:" in txt)
        check("T3 mget https://example.com -> 200 + saved", ok, txt[-400:])

        # ---- T4: raw.githubusercontent.com, strict-verified chain ----
        txt = run_cmd(
            q,
            "mget https://raw.githubusercontent.com/torvalds/linux/master/README",
            "saved (HTTP", timeout=240, tag="-raw")
        ok = ("chain verified" in txt and "HTTP status 200" in txt
              and "README saved" in txt)
        check("T4 raw.githubusercontent verified + README saved", ok,
              txt[-500:])

        # ---- T5: github.com P-384 chain -> fallback warning + 200 ----
        txt = run_cmd(q, "mget https://github.com/octocat/Hello-World",
                      "saved (HTTP", timeout=300, tag="-gh")
        ok = ("NOT VERIFIED" in txt and "HTTP status 200" in txt
              and "Hello-World saved" in txt)
        check("T5 github.com fallback warning + 200 + saved", ok,
              txt[-500:])

    finally:
        q.kill()

    print()
    npass = sum(1 for _, c in results if c)
    for name, c in results:
        print(f"  {'ok ' if c else 'FAIL'} {name}")
    print(f"\n{npass}/{len(results)} PASS")
    sys.exit(0 if npass == len(results) else 1)


if __name__ == "__main__":
    main()
