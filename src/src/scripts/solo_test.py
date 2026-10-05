#!/usr/bin/env python3
"""solo_test.py — compile the two deterministically-failing jobs
SEQUENTIALLY in the shell (no parallel pool). If they crash solo the
bug is in the file/mode path; if they pass solo it needs the 2-task
interleaving."""
import os, sys, time, socket, subprocess, tempfile

QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "dist", "equinox.iso")
SERIAL = "/tmp/solo_test_serial.log"


class Qemu:
    def __init__(self):
        self.sock = tempfile.mktemp(prefix="qmon-", suffix=".sock")
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = (
            os.path.expanduser("~/tools/root/usr/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/usr/lib"))
        cmd = [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "64",
               "-cdrom", ISO, "-display", "none", "-vga", "std",
               "-serial", f"file:{SERIAL}",
               "-monitor", "unix:" + self.sock + ",server,nowait",
               "-net", "none", "-no-reboot"]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, env=env)
        for _ in range(150):
            if os.path.exists(self.sock):
                break
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                self.mon.connect(self.sock)
                break
            except ConnectionRefusedError:
                time.sleep(0.2)
        self.mon.settimeout(2)
        self._drain(1.0)

    def _drain(self, t=0.15):
        out = b""
        self.mon.settimeout(t)
        try:
            while True:
                c = self.mon.recv(4096)
                if not c:
                    break
                out += c
        except socket.timeout:
            pass
        return out.decode(errors="replace")

    def cmd(self, line, t=0.3):
        self.mon.settimeout(t)
        self.mon.sendall((line + "\n").encode())
        time.sleep(0.02)
        return self._drain(t)

    KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus"}

    def type_line(self, text, wait=1.0):
        for ch in text:
            k = ("shift-" + ch.lower()) if "A" <= ch <= "Z" else self.KEYMAP.get(ch, ch)
            self.cmd(f"sendkey {k}")
            time.sleep(0.12)
        self.cmd("sendkey ret")
        time.sleep(wait)

    def quit(self):
        try:
            self.cmd("quit", 3)
        except Exception:
            pass
        try:
            self.proc.terminate(); self.proc.wait(5)
        except Exception:
            try: self.proc.kill()
            except Exception: pass


def serial():
    try:
        with open(SERIAL, errors="replace") as fh:
            return fh.read()
    except FileNotFoundError:
        return ""


def wait_for(pat, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pat in serial():
            return True
        time.sleep(0.4)
    return False


if os.path.exists(SERIAL):
    os.remove(SERIAL)
rig = Qemu()
assert wait_for("user $", 150), "boot failed"
print("[solo] booted", flush=True)

for cmdline in [
    "mtcc --lib -q -c /equinox/libc/heap.c",
    "mtcc --lib -q -c /equinox/libc/heap.c",
    "mtcc -q -c /equinox/tools/basename.c",
    "mtcc -q -c /equinox/tools/basename.c",
]:
    base = len(serial())
    rig.type_line(cmdline, wait=3)
    time.sleep(2)   # let it finish/quiet
    out = serial()[base:base + 3000]
    faulted = "program faulted" in out
    print(f"[solo] $ {cmdline}")
    print("   " + " | ".join(ln.strip() for ln in out.split("\n")
                             if ln.strip())[:220])
    print(f"   -> faulted={faulted}", flush=True)

rig.quit()
print("[solo] done")
