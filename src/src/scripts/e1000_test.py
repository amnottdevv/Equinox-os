#!/usr/bin/env python3
"""e1000_test.py — driver NIC Intel PRO/1000 (e1000, 8086:100E).

Mesin QEMU: ISO + `-device e1000,netdev=net0` TANPA ne2k_isa, jadi
bila driver e1000 tidak bekerja, `ifconfig` ikut mati (net: down).

  E1  boot ke shell
  E2  lspci menemukan 0x8086 / 0x100E (82540EM) + irq aktif
  E3  ifconfig: driver e1000 (bukan ne2000)
  E4  DHCP slirp -> inet 10.0.2.15
  E5  ping 10.0.2.2 -> 4/4, 0% packet loss (RX/TX lewat e1000)
  E6  mget http://10.0.2.2:8022/e1000.txt -> HTTP 200 + saved
  E7  ifconfig: RX/TX > 0 dan irq > 0 setelah trafik
  E8  tcpping 10.0.2.2 8022 -> OPEN (TCP handshake lewat e1000)

(pemilihan driver via <volume>/boot/system.ecf diuji oleh
 scripts/installer_wizard_test.py — di sini tak ada ne2000 sama sekali,
 jadi registry jatuh ke e1000 lewat jalur fallback normal.)

Usage: python3 e1000_test.py [iso]
"""
import functools
import http.server
import os
import re
import socketserver
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32                     # noqa: E402

# Mesin khusus: NIC e1000 saja (netdev id sama, ne2k_isa dibuang).
b32.SERIAL = "/tmp/e1000_serial.log"
b32.NET = ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"]

SERIAL = b32.SERIAL
ISO = sys.argv[1] if len(sys.argv) > 1 else b32.ISO
PORT = 8022
BODY = ("Equinox e1000 driver test\n"
        "TCP: guest 10.0.2.15 -> slirp 10.0.2.2 -> host python http.server\n"
        + "x" * 64 + "\n") * 4

check, since, serial, wait_serial = b32.check, b32.since, b32.serial, b32.wait_serial


def start_http():
    """Host HTTP server di :PORT — dijangkau guest lewat 10.0.2.2:PORT."""
    d = tempfile.mkdtemp(prefix="e1000-http-")
    with open(os.path.join(d, "e1000.txt"), "w") as fh:
        fh.write(BODY)

    class H(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass

    handler = functools.partial(H, directory=d)
    httpd = socketserver.TCPServer(("127.0.0.1", PORT), handler)
    httpd.allow_reuse_address = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def window(base, pat, timeout, rig, limit=6000):
    """Tunggu `pat` muncul di log serial SETELAH posisi `base`."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        t = serial()[base:]
        if pat in t:
            return t
        if not rig.alive():
            return t
        time.sleep(0.3)
    return serial()[base:base + limit]


def main():
    if not os.path.exists(ISO):
        print(f"[e1000] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    httpd = start_http()

    print("[e1000] boot (ISO + -device e1000) ...", flush=True)
    rig = b32.Qemu(ISO)
    ok = wait_serial("user $", 150, rig, t0=time.time())
    check("E1 boot to shell", ok)

    # ---- E2: perangkat PCI terdeteksi -------------------------------
    base = len(serial())
    rig.type_line("lspci", wait=1.0)
    # Tunggu hingga "BAR" muncul (menandakan device dengan BAR tercetak,
    # termasuk e1000 di 00:03.0) — ambil output penuh hingga prompt
    t = window(base, "BAR", 30, rig, 30000)
    # Pastikan prompt juga sudah kembali
    wait_serial("user $", 5, rig)
    tl = t.lower()
    check("E2 lspci menemukan 8086:100E",
          ("0x8086" in tl and "0x100e" in tl),
          "vendor/device tercetak" if ("0x100e" in tl) else t[:800])

    # ---- E3/E4: ifconfig --------------------------------------------
    base = len(serial())
    rig.type_line("ifconfig", wait=3.0)
    t = window(base, "RX ", 25, rig, 4000)
    check("E3 ifconfig pakai driver e1000", "driver e1000" in t,
          t.split("driver")[-1][:40] if "driver" in t else t[:200])
    check("E4 DHCP slirp -> inet 10.0.2.15", "inet 10.0.2.15" in t,
          [l.strip() for l in t.splitlines() if "inet" in l][:1])

    # ---- E5: ping gateway slirp --------------------------------------
    base = len(serial())
    rig.type_line("ping 10.0.2.2", wait=6.0)
    t = window(base, "packet loss", 40, rig, 4000)
    check("E5 ping 10.0.2.2 0% loss", "0% packet loss" in t,
          [l.strip() for l in t.splitlines() if "packet loss" in l][:1])

    # ---- E6: mget dari host (RST di atas e1000) ----------------------
    base = len(serial())
    rig.type_line(f"mget http://10.0.2.2:{PORT}/e1000.txt", wait=5.0)
    t = window(base, "saved (HTTP", 45, rig, 4000)
    check("E6 mget HTTP 200 + saved", "HTTP 200" in t and "saved" in t,
          [l.strip() for l in t.splitlines() if "saved" in l or "HTTP" in l][:2])

    # ---- E7: penghitung RX/TX/irq setelah trafik ---------------------
    base = len(serial())
    rig.type_line("ifconfig", wait=3.0)
    t = window(base, "RX ", 25, rig, 4000)
    m = re.search(r"RX (\d+)\s+TX (\d+)\s+drop \d+\s+irq (\d+)", t)
    rx, tx, irq = (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else (0, 0, 0)
    check("E7 RX/TX/irq setelah trafik", rx > 0 and tx > 0 and irq > 0,
          f"RX {rx} TX {tx} irq {irq}")

    # ---- E8: TCP handshake lewat kartu PCI ---------------------------
    base = len(serial())
    rig.type_line(f"tcpping 10.0.2.2 {PORT}", wait=5.0)
    t = window(base, "OPEN (handshake", 35, rig, 3000)
    check("E8 tcpping OPEN", "OPEN (handshake" in t,
          [l.strip() for l in t.splitlines() if "tcpping:" in l][:1])

    rig.quit()
    httpd.shutdown()
    print(f"\n[e1000] RESULT: {len(b32.PASS)} PASS, {len(b32.FAIL)} FAIL")
    if b32.FAIL:
        print("[e1000] failed:", ", ".join(b32.FAIL))
    return 1 if b32.FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
