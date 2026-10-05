#!/usr/bin/env python3
"""repro_install.py — run equinoxinstall N times in QEMU and report
which jobs fail each round (deterministic vs. racy failure)."""
import os, sys, time, socket, subprocess, tempfile

QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "dist", "equinox.iso")
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 2


class Qemu:
    def __init__(self, serial):
        self.sock = tempfile.mktemp(prefix="qmon-", suffix=".sock")
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = (
            os.path.expanduser("~/tools/root/usr/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/usr/lib"))
        cmd = [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "64",
               "-cdrom", ISO, "-display", "none", "-vga", "std",
               "-serial", f"file:{serial}",
               "-monitor", "unix:" + self.sock + ",server,nowait",
               "-net", "none", "-no-reboot"]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, env=env)
        for _ in range(150):
            if os.path.exists(self.sock):
                break
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU exited at boot")
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        last = None
        for _ in range(50):
            try:
                self.mon.connect(self.sock)
                last = None
                break
            except ConnectionRefusedError as e:
                last = e
                time.sleep(0.2)
        if last is not None:
            raise RuntimeError(f"monitor connect failed: {last}")
        self.mon.settimeout(2)
        self._drain(1.0)

    def _drain(self, t=0.15):
        import socket as s
        out = b""
        self.mon.settimeout(t)
        try:
            while True:
                c = self.mon.recv(4096)
                if not c:
                    break
                out += c
        except s.timeout:
            pass
        return out.decode(errors="replace")

    def cmd(self, line, t=0.3):
        self.mon.settimeout(t)
        self.mon.sendall((line + "\n").encode())
        time.sleep(0.02)
        return self._drain(t)

    KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus",
              "_": "shift-minus", ">": "shift-dot"}

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


def run_round(i):
    ser = f"/tmp/repro_install_{i}.log"
    if os.path.exists(ser):
        os.remove(ser)
    rig = Qemu(ser)
    t0 = time.time()
    # wait for shell
    ok = False
    while time.time() - t0 < 150:
        with open(ser, errors="replace") as fh:
            if "user $" in fh.read():
                ok = True
                break
        time.sleep(0.5)
    if not ok:
        print(f"[round {i}] boot FAILED")
        rig.quit()
        return None
    rig.type_line("equinoxinstall", wait=2)
    t0 = time.time()
    while time.time() - t0 < 600:
        with open(ser, errors="replace") as fh:
            if "equinoxinstall done" in fh.read():
                break
        if rig.proc.poll() is not None:
            print(f"[round {i}] QEMU died mid-install")
            break
        time.sleep(0.5)
    rig.quit()
    time.sleep(1)
    with open(ser, errors="replace") as fh:
        log = fh.read()
    fails = [ln for ln in log.split("\n") if "compile failed" in ln or "fallback" in ln]
    faults = [ln for ln in log.split("\n") if "program faulted" in ln or "CR2" in ln]
    summary = next((ln for ln in log.split("\n") if "summary" in ln), "?")
    print(f"[round {i}] {summary}")
    for f in fails:
        print("   FAIL:", f.strip())
    for f in faults[:8]:
        print("   FAULT:", f.strip())
    return fails


for i in range(1, ROUNDS + 1):
    run_round(i)
print("done")
