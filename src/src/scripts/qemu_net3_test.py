#!/usr/bin/env python3
"""
qemu_net3_test.py — Equinox OS v10.13 "DNS + Windows-friendly" in-OS regression.

Konteks: user jalanin v10.12 ISO di QEMU WINDOWS — ethernet OK, tapi
ping 8.8.8.8 gagal (slirp Windows memblok ICMP) dan wget 0 byte.
Sandbox ini kebetulan ping_group_range="1 0" (ICMP juga diblok) —
persis replika kondisi Windows milik user. Suite ini memvalidasi
solusi v10.13 dalam kondisi tersebut.

Topology: QEMU user-net (slirp) + ne2k_isa + hostfwd 8080->80 +
host python http.server :8022 (untuk wget/tcpping lokal).

Tests:
  D1  boot ke shell prompt
  D2  ifconfig -> DHCP 10.0.2.15
  D3  ping 10.0.2.2 -> 100% loss + HINT BARU muncul (arah tcpping)
      (di env ini ICMP diblok = replika Windows)
  D4  tcpping 10.0.2.2 8022 -> TERBUKA + RTT (pengganti ping)
  D5  dns example.com -> resolve IP via 10.0.2.3 (UDP hidup)
  D6  tcpping example.com 80 -> TERBUKA (internet beneran, TCP)
  D7  wget captive.apple.com 80 /hotspot-detect.html -> HTTP 200
      (example.com = DNS roulette: sebagian edge Cloudflare 403)
  D7b wget example.com 80 / -> 200 tersimpan ATAU 403 dilaporkan rapi
  D8  cat hotspot-detect.html -> konten "Success"
  D9  wget 10.0.2.2 8022 /test.txt -> byte-exact (regresi v10.12)
  D10 wget localhost 8022 -> hint "localhost = Equinox OS sendiri" muncul
  D11 httpd via hostfwd curl -> halaman status (regresi)

Usage: python3 qemu_net3_test.py [iso]
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from qemu_net2_test import (  # noqa: E402
    Qemu, http_get, start_wget_server, wait_text,
    PROMPT, WGET_PORT, WGET_CONTENT,
)
import tempfile  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, ".."))
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "equinox.iso")

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  — {detail}" if detail and not cond else ""))


def main():
    if not os.path.exists(ISO):
        print("ISO tidak ada:", ISO)
        sys.exit(2)

    srvdir = tempfile.mkdtemp(prefix="morph-httpd3-")
    with open(os.path.join(srvdir, "test.txt"), "wb") as f:
        f.write(WGET_CONTENT)
    srv = start_wget_server(srvdir)

    print("Boot QEMU (user-net + ne2k_isa + hostfwd :8080->:80)…")
    q = Qemu(ISO)
    try:
        # ---- D1: boot ----
        txt = wait_text(q, PROMPT, timeout=120, tag="-boot")
        check("D1 boot: shell prompt hidup", PROMPT in txt)
        q.type_str("\n")
        time.sleep(0.8)

        # ---- D2: ifconfig (DHCP) ----
        q.type_str("ifconfig\n")
        time.sleep(2.0)
        txt = q.screen("-ifc")
        check("D2 ifconfig: inet 10.0.2.15 (dhcp)", "10.0.2.15" in txt, txt[-400:])

        # ---- D3: ping gateway -> di env ICMP-blok: hint tcpping ----
        q.type_str("ping 10.0.2.2\n")
        txt = wait_text(q, "ping statistics", timeout=60, tag="-png")
        time.sleep(2.0)
        txt = q.screen("-png2")
        if "0% packet loss" in txt:
            check("D3 ping: 4/4 reply (ICMP diizinkan di env ini)", True)
            check("D3b ping-hint: tidak diperlukan (ICMP jalan)", True)
        else:
            # replika Windows: 100% loss -> hint harus muncul
            check("D3 ping: timeout (ICMP diblok slirp — replika Windows)",
                  "100% packet loss" in txt, txt[-500:])
            check("D3b ping-hint: pesan tcpping muncul", "tcpping" in txt, txt[-500:])

        # ---- D4: tcpping lokal (host server :8022) ----
        q.type_str(f"tcpping 10.0.2.2 {WGET_PORT}\n")
        txt = wait_text(q, "TERBUKA", timeout=30, tag="-tp1")
        check("D4 tcpping 10.0.2.2:8022 TERBUKA + RTT",
              "TERBUKA" in txt and "ms" in txt, txt[-300:])

        # ---- D5: DNS resolve (UDP via 10.0.2.3) ----
        q.type_str("dns example.com\n")
        txt = wait_text(q, "example.com ->", timeout=40, tag="-dns")
        line = ""
        for l in txt.split("\n"):
            if "example.com ->" in l:
                line = l.strip()
                break
        import re as _re
        m = _re.search(r"example\.com -> (\d+\.\d+\.\d+\.\d+)", line)
        check("D5 dns: example.com terresolve ke IP", m is not None, line)

        # ---- D6: tcpping internet (TCP via NAT slirp) ----
        q.type_str("tcpping example.com 80\n")
        txt = wait_text(q, "TERBUKA", timeout=30, tag="-tp2")
        check("D6 tcpping example.com:80 TERBUKA (internet)",
              "TERBUKA" in txt, txt[-300:])

        # ---- D7: wget internet beneran (captive.apple.com: stabil
        #      plain-HTTP 200 di semua edge — pelajaran example.com:
        #      DNS roulette bisa dapat edge Cloudflare yang 403) ----
        q.type_str("wget captive.apple.com 80 /hotspot-detect.html\n")
        txt = wait_text(q, "tersimpan", timeout=60, tag="-wgi")
        check("D7 wget internet: tersimpan HTTP 200",
              "tersimpan" in txt and "HTTP 200" in txt, txt[-400:])

        # ---- D7b: example.com — edge roulette: 200 tersimpan ATAU 403
        #      dilaporkan rapi. Dua-duanya bukti stack TCP sehat. ----
        q.type_str("wget example.com 80 /\n")
        txt = wait_text(q, "HTTP status", timeout=60, tag="-wgx")
        d7b = ("tersimpan" in txt and "HTTP 200" in txt) or \
              ("HTTP status 403" in txt and "menolak" in txt)
        check("D7b wget example.com: 200 tersimpan / 403 rapi", d7b, txt[-400:])

        # ---- D8: konten file hasil download internet ----
        q.type_str("cat hotspot-detect.html\n")
        time.sleep(2.5)
        txt = q.screen("-cat")
        check("D8 cat hotspot-detect.html: konten Success",
              "Success" in txt, txt[-400:])

        # ---- D9: regresi wget host :8022 ----
        q.type_str(f"wget 10.0.2.2 {WGET_PORT} /test.txt\n")
        txt = wait_text(q, "tersimpan", timeout=45, tag="-wgl")
        check("D9 wget lokal: byte-exact",
              "tersimpan" in txt and f"{len(WGET_CONTENT)} byte" in txt, txt[-400:])

        # ---- D10: wget localhost -> hint ----
        q.type_str(f"wget localhost {WGET_PORT} /test.txt\n")
        txt = wait_text(q, "catatan", timeout=45, tag="-wlh")
        check("D10 wget localhost: hint 'localhost = Equinox OS sendiri'",
              "catatan" in txt and "10.0.2.2" in txt, txt[-500:])
        # hasil apapun (tersimpan via host-loopback ATAU gagal rapi) —
        # yang penting tidak crash; tunggu selesai lalu cek prompt hidup
        txt = wait_text(q, PROMPT, timeout=30, tag="-wlh2")
        check("D10b setelah localhost: shell masih hidup", PROMPT in txt[-200:])

        # ---- D11: httpd regresi (hostfwd curl) ----
        page = http_get("http://127.0.0.1:8080/")
        check("D11 httpd: index dari host (TCP dua arah)",
              b"Equinox OS" in page and b"TCP/IP alive" in page, repr(page[:120]))

    finally:
        q.kill()
        srv.shutdown()

    ok = sum(1 for _, c in results if c)
    print(f"\n== HASIL: {ok}/{len(results)} PASS ==")
    if ok != len(results):
        sys.exit(1)


if __name__ == "__main__":
    main()
