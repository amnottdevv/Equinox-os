#!/usr/bin/env python3
"""net2_pcap_debug.py — capture guest-side frames during hostfwd curl."""
import subprocess, sys, time, socket, tempfile, os
sys.path.insert(0, "/home/z/morphos/scripts")
from qemu_net2_test import Qemu, screen_text, PROMPT, wait_text

ISO = "/home/z/morphos/dist/morphos.iso"
PCAP = "/tmp/hostfwd.pcap"
if os.path.exists(PCAP):
    os.unlink(PCAP)

# boot manual dengan filter-dump
sock_path = tempfile.mktemp(prefix="qmon-", suffix=".sock")
dumpdir = tempfile.mkdtemp(prefix="qdump-")
proc = subprocess.Popen(
    [os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386"),
     "-L", os.path.expanduser("~/tools/root/usr/share/qemu"),
     "-L", os.path.expanduser("~/tools/root/usr/share/seabios"),
     "-m", "64", "-cdrom", ISO, "-display", "none", "-vga", "std",
     "-netdev", "user,id=net0,hostfwd=tcp::8080-:80",
     "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9",
     "-object", f"filter-dump,id=f1,netdev=net0,file={PCAP}",
     "-monitor", "unix:" + sock_path + ",server,nowait", "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

deadline = time.time() + 10
while not os.path.exists(sock_path):
    time.sleep(0.1)
mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
mon.connect(sock_path)
mon.settimeout(60)

def cmd(c, t=30):
    mon.sendall(c.encode() + b"\n")
    buf = b""
    try:
        while True:
            ch = mon.recv(65536)
            if not ch: break
            buf += ch
            if b"(qemu)" in buf: break
    except socket.timeout:
        pass
    return buf.decode("utf-8", "replace")

cmd(""); time.sleep(2)

# tunggu boot (poll screendump via monitor)
booted = False
for i in range(40):
    path = os.path.join(dumpdir, "b.ppm")
    cmd(f"screendump {path}", 20)
    if os.path.exists(path) and os.path.getsize(path) > 100:
        txt = screen_text(path)
        if PROMPT in txt:
            booted = True
            break
    time.sleep(3)
print("booted:", booted)
time.sleep(2)

r = subprocess.run(["curl", "-4", "-sv", "--max-time", "10", "http://127.0.0.1:8080/"],
                   capture_output=True, text=True)
print("curl rc:", r.returncode, "| last:", r.stderr.splitlines()[-1] if r.stderr else "")
time.sleep(2)

cmd("quit", 3)
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()

# ---- parse pcap ----
data = open(PCAP, "rb").read()
print(f"pcap: {len(data)} bytes")
off = 24  # pcap header
pkts = []
n = 0
while off + 16 <= len(data):
    import struct
    ts, tus, caplen, wirelen = struct.unpack("<IIII", data[off:off+16])
    off += 16
    pkt = data[off:off+caplen]
    off += caplen
    n += 1
    if len(pkt) < 34:
        continue
    eth_type = (pkt[12] << 8) | pkt[13]
    if eth_type != 0x0800:
        continue
    ip = pkt[14:]
    ihl = (ip[0] & 0xF) * 4
    proto = ip[9]
    if proto != 6:
        continue
    src = ".".join(str(b) for b in ip[12:16])
    dst = ".".join(str(b) for b in ip[16:20])
    tcp = ip[ihl:]
    sport = (tcp[0] << 8) | tcp[1]
    dport = (tcp[2] << 8) | tcp[3]
    seq = int.from_bytes(tcp[4:8], "big")
    doff = ((tcp[12] >> 4) & 0xF) * 4
    flags = tcp[13]
    payload = len(tcp) - doff
    fname = "".join(s for f, s in [(0x02,"SYN"),(0x10,"ACK"),(0x01,"FIN"),(0x04,"RST"),(0x08,"PSH")] if flags & f)
    pkts.append((n, ts, f"{src}:{sport} -> {dst}:{dport}", fname, f"seq={seq}", f"len={payload}"))

for p in pkts:
    print(p)
