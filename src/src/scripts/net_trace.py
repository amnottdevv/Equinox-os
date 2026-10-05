#!/usr/bin/env python3
"""net_trace.py — QEMU + trace ne2000* + ketik ping, lalu analisis trace."""
import os, sys, time, subprocess, socket, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_net_test import Qemu

ISO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "dist", "equinox.iso"))
TRACE = "/tmp/qtrace2.log"

class QemuTraced(Qemu):
    def __init__(self, iso):
        self.sock_path = tempfile.mktemp(prefix="qmon-", suffix=".sock")
        self.dumpdir = tempfile.mkdtemp(prefix="qdump-")
        self.dump_n = 0
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = os.path.expanduser(
            "~/tools/root/usr/lib/x86_64-linux-gnu") + ":" + os.path.expanduser(
            "~/tools/root/lib/x86_64-linux-gnu") + ":" + os.path.expanduser(
            "~/tools/root/usr/lib")
        self.proc = subprocess.Popen(
            [os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386"),
             "-L", os.path.expanduser("~/tools/root/usr/share/qemu"),
             "-L", os.path.expanduser("~/tools/root/usr/share/seabios"),
             "-m", "64", "-cdrom", iso,
             "-display", "none", "-vga", "std",
             "-netdev", "user,id=net0",
             "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9",
             "-d", "trace:ne2000*",
             "-D", TRACE,
             "-monitor", "unix:" + self.sock_path + ",server,nowait",
             "-no-reboot"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        deadline = time.time() + 10
        while not os.path.exists(self.sock_path):
            if time.time() > deadline:
                raise RuntimeError("monitor socket tidak muncul")
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.mon.connect(self.sock_path)
        self.mon.settimeout(120)
        self._drain(2.0)

q = QemuTraced(ISO)
try:
    t0 = time.time()
    while time.time() - t0 < 60:
        if "Boot checks complete" in q.screen("-w"):
            break
        time.sleep(3)
    time.sleep(8)
    q.type_str("ping 10.0.2.2\n")
    time.sleep(15)
finally:
    q.kill()

print("=== analisis trace ===")
lines = open(TRACE).read().split("\n")
print("total lines:", len(lines))
# tanda init final: PSTOP=0x80 write
for i, l in enumerate(lines):
    if "write addr=0x02 val=0x80" in l:
        print("init-final di line", i)
        break
# CURR reads (page1, addr 0x07): nilai > 0x47 = paket masuk!
currs = []
for l in lines:
    if "io read addr=0x07" in l:
        try:
            v = int(l.split("val=0x")[1], 16)
            currs.append(v)
        except Exception:
            pass
import collections
print("CURR read distribution:", dict(collections.Counter(currs)))
# data port reads besar (baca paket): addr=0x10 reads
dr = sum(1 for l in lines if "io read addr=0x10" in l)
print("data port reads:", dr)
# writes ISR (ack pattern)
iw = [l for l in lines if "write addr=0x07" in l]
print("ISR writes (uniq vals):", sorted(set(l.split('val=')[-1] for l in iw))[:10])
