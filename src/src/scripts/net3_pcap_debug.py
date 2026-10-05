#!/usr/bin/env python3
"""net3_pcap_debug.py — capture & analyze wget example.com dari guest.

Boot QEMU (slirp+ne2k, filter-dump pcap), ketik `wget example.com 80 /`,
tunggu selesai, quit, lalu parse pcap: timeline TCP (seq/ack/flags/len).
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, "/home/z/morphos/scripts")
from qemu_net2_test import PROMPT, wait_text  # noqa: E402
from qemu_net3_test import Qemu as Qemu3  # reuse? (net3 punya kelas sendiri? tidak)

ISO = "/home/z/morphos/dist/equinox.iso"
PCAP = "/tmp/wget_example.pcap"
if os.path.exists(PCAP):
    os.unlink(PCAP)

# ---- boot manual dengan filter-dump ----
sock_path = tempfile.mktemp(prefix="qmon-", suffix=".sock")
proc = subprocess.Popen(
    [os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386"),
     "-L", os.path.expanduser("~/tools/root/usr/share/qemu"),
     "-L", os.path.expanduser("~/tools/root/usr/share/seabios"),
     "-m", "64", "-cdrom", ISO, "-display", "none", "-vga", "std",
     "-netdev", "user,id=net0",
     "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9",
     "-object", f"filter-dump,id=f1,netdev=net0,file={PCAP}",
     "-monitor", "unix:" + sock_path + ",server,nowait", "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

deadline = time.time() + 10
while not os.path.exists(sock_path):
    time.sleep(0.1)
mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
mon.connect(sock_path)
mon.settimeout(90)


def cmd(c, t=30):
    mon.sendall(c.encode() + b"\n")
    buf = b""
    try:
        while True:
            ch = mon.recv(65536)
            if not ch:
                break
            buf += ch
            if b"(qemu)" in buf:
                break
    except socket.timeout:
        pass
    return buf.decode("utf-8", "replace")


# mini harness (dari qemu_net2_test) — perlu instance dengan dump()
sys.path.insert(0, "/home/z/morphos/scripts")
import qemu_net2_test as q2  # noqa: E402
q2.Qemu.proc = None  # unused marker


class MiniQ(q2.Qemu):
    """bajak kelas Qemu supaya pakai proc/monitor yang sudah jalan."""

    def __init__(self, proc, mon, sock_path):  # noqa: D107
        self.proc = proc
        self.mon = mon
        self.sock_path = sock_path
        self.dumpdir = tempfile.mkdtemp(prefix="qdump-")
        self.dump_n = 0


q = MiniQ(proc, mon, sock_path)
cmd("")
print("tunggu boot…")
txt = wait_text(q, PROMPT, timeout=120, tag="-boot")
print("boot:", "OK" if PROMPT in txt else "GAGAL")

q.type_str("\n"); time.sleep(0.5)
q.type_str("wget example.com 80 /\n")
print("wget diketik, tunggu hasil…")
txt = wait_text(q, "tersimpan", timeout=40, tag="-wget")
for line in txt.split("\n"):
    if "wget" in line or "tersimpan" in line or "terputus" in line:
        print("GUEST>", line.strip())
# tunggu prompt kembali
txt = wait_text(q, PROMPT, timeout=20, tag="-done")
time.sleep(1)
try:
    cmd("quit", 3)
except Exception:
    pass
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()

# ---------------- parse pcap ----------------
data = open(PCAP, "rb").read()
print(f"\npcap: {len(data)} byte, {PCAP}")


def parse_pcap(buf):
    pkts = []
    off = 24
    while off + 16 <= len(buf):
        ts_s, ts_u, incl, orig = struct.unpack("<IIII", buf[off:off + 16])
        off += 16
        if off + incl > len(buf):
            break
        pkts.append((ts_s + ts_u / 1e6, buf[off:off + incl]))
        off += incl
    return pkts


pkts = parse_pcap(data)
print(f"{len(pkts)} packet")


def parse_tcp(pkt):
    if len(pkt) < 34:
        return None
    et = struct.unpack(">H", pkt[12:14])[0]
    if et != 0x0800:
        return None
    ihl = (pkt[14] & 0x0F) * 4
    proto = pkt[14 + 9]
    if proto != 6:
        return None
    src = ".".join(str(b) for b in pkt[14 + 12:14 + 16])
    dst = ".".join(str(b) for b in pkt[14 + 16:14 + 20])
    t = 14 + ihl
    sport, dport, seq, ack = struct.unpack(">HHII", pkt[t:t + 12])
    doff = (pkt[t + 12] >> 4) * 4
    flags = pkt[t + 13]
    payload = len(pkt) - (t + doff)
    fn = "".join(n for b, n in [(0x02, "S"), (0x10, "A"), (0x01, "F"),
                                (0x04, "R"), (0x08, "P")] if flags & b)
    return (src, sport, dst, dport, seq, ack, fn, payload)


t0 = None
for ts, pkt in pkts:
    r = parse_tcp(pkt)
    if not r:
        continue
    src, sp, dst, dp, seq, ack, fn, pl = r
    if t0 is None:
        t0 = ts
    # hanya flow port 80
    if sp != 80 and dp != 80:
        continue
    if sp == 80:
        arrow = "<=="
    else:
        arrow = "==>"
    print(f"[{(ts - t0) * 1000:7.1f}ms] {arrow} {src}:{sp}->{dst}:{dp} "
          f"{fn:3s} seq={seq} ack={ack} len={pl}")
