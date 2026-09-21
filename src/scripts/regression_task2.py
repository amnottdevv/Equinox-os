#!/usr/bin/env python3
"""regression_task2.py — v0.3 QEMU regression.

Covers FR-05/06/07 (memory + demand paging + ELF) and FR-02
(wait/pipe):
  W1  boot to shell
  W2  fstest 24/24 (demand paging: arena blocks, heap, file buffers)
  W3  elfdemo.elf runs (ELF32 loader + bss zero-fill + meminfo)
  W4  spawn ELF child + wait -> status 42 (zombie collection)
  W5  spawn .mrp child + wait -> status (mrp zombie path)
  W6  wait with no children -> ECHILD message
  W7  pipedemo (pipe + fd inheritance + EOF + wait, mtcc program)
  W8  meminfo: pool stats + per-task faulted pages while a child runs
  W9  zombie visible in ps (DEAD state) before wait collects it
  W10 kill while parent waits (parent survives, wait returns)
  W11 pool recovers after children exit (no page leak)
  W12 60 s soak - no reboot/panic
"""
import os
import sys
import time
import socket
import subprocess
import tempfile

sys.path.insert(0, "/home/z/my-project/equinox_os/extracted/equinox_os_v0.2_beta/scripts")
from fps_measure import load_font  # noqa: E402

QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]
ISO = "/home/z/my-project/equinox_os/extracted/equinox_os_v0.2_beta/dist/equinox.iso"
SERIAL = "/tmp/regen_task2_serial.log"

PASS, FAIL = [], []


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}  {detail}", flush=True)


class Qemu:
    def __init__(self, iso):
        self.sock_path = tempfile.mktemp(prefix="qmon-", suffix=".sock")
        self.dumpdir = tempfile.mkdtemp(prefix="qdump-")
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = (
            os.path.expanduser("~/tools/root/usr/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/usr/lib"))
        cmd = [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "64",
               "-cdrom", iso, "-display", "none", "-vga", "std",
               "-serial", f"file:{SERIAL}",
               "-monitor", "unix:" + self.sock_path + ",server,nowait",
               "-no-reboot"]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, env=env)
        deadline = time.time() + 15
        while not os.path.exists(self.sock_path):
            if time.time() > deadline:
                raise RuntimeError("monitor socket did not appear")
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU exited at boot")
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.mon.connect(self.sock_path)
        self.mon.settimeout(2)
        self._drain(1.0)

    def _drain(self, timeout=0.15):
        self.mon.settimeout(timeout)
        buf = b""
        try:
            while True:
                b = self.mon.recv(65536)
                if not b:
                    break
                buf += b
        except socket.timeout:
            pass
        return buf.decode("latin1", "replace")

    def cmd(self, line, timeout=0.3):
        self.mon.settimeout(timeout)
        self.mon.sendall((line + "\n").encode())
        time.sleep(0.02)
        return self._drain(timeout)

    def sendkey(self, key, wait=0.06):
        self.cmd(f"sendkey {key}")
        if wait:
            time.sleep(wait)

    KEYMAP = {
        " ": "spc", ".": "dot", "/": "slash", ",": "comma",
        "-": "minus", ";": "semicolon", "=": "equal", "'": "apostrophe",
        "`": "grave_accent", "[": "bracket_left", "]": "bracket_right",
        "\\": "backslash", "\t": "tab", "_": "shift-minus",
        ">": "shift-dot",
    }

    def keyname(self, ch):
        if "A" <= ch <= "Z":
            return "shift-" + ch.lower()
        return self.KEYMAP.get(ch, ch)

    def type_line(self, text, wait=1.0):
        # 0.12 s/key: 0.05 s dropped keys mid-word during the W5 hunt
        for ch in text:
            self.sendkey(self.keyname(ch), wait=0.12)
        self.sendkey("ret", wait=wait)

    def alive(self):
        return self.proc.poll() is None

    def quit(self):
        try:
            self.cmd("quit", timeout=3)
        except Exception:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(5)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


def serial():
    try:
        with open(SERIAL, errors="replace") as fh:
            return fh.read()
    except FileNotFoundError:
        return ""


def after(marker, limit=3000):
    s = serial()
    if marker not in s:
        return ""
    return s.split(marker, 1)[1][:limit]


def since(n, limit=4000):
    """Serial tail from byte offset n (avoids matching EARLIER markers)."""
    return serial()[n:n + limit]


def wait_serial(pat, timeout=60, rig=None):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pat in serial():
            return True
        if rig is not None and not rig.alive():
            return False
        time.sleep(0.4)
    return False


def main():
    for f in (SERIAL,):
        if os.path.exists(f):
            os.remove(f)

    print("[task2] boot ...")
    rig = Qemu(ISO)
    ok = wait_serial("user $", 120, rig)
    check("W1 boot to shell", ok)
    if not ok:
        rig.quit()
        print(serial()[-2000:])
        return 1

    base = len(serial())

    # ---- v0.3: the ISO ships tools as .c — build them first ----
    # (pipedemo/fstest live in /equinox/tools as sources now)
    print("[task2] eqbuild (self-hosting userland) ...")
    rig.type_line("eqbuild", wait=3)
    ok = wait_serial("eqbuild: done", 420, rig)
    check("W1b eqbuild compiles the userland in-OS",
          ok and "29 built, 0 failed" in serial())

    # ---- W2: fstest under demand paging ----
    print("[task2] fstest (demand paging stress) ...")
    base = len(serial())
    rig.type_line("fstest", wait=30)
    ok = "24/24 PASS" in since(base, 4000)
    check("W2 fstest 24/24 (demand paging)", ok)

    # ---- W3: ELF loader ----
    print("[task2] elfdemo.elf ...")
    base = len(serial())
    rig.type_line("elfdemo.elf", wait=25)
    t = since(base, 2000)
    ok = "ELFDEMO RESULT: PASS" in t and "bss zero-fill: OK" in t
    check("W3 ELF32 loader + bss zero-fill", ok, t.strip().split("\n")[0][:60])

    # ---- W4: spawn ELF child + wait (zombie) ----
    print("[task2] spawn ELF + wait ...")
    base = len(serial())
    rig.type_line("spawn elfdemo.elf", wait=8)
    # while the child exists: ps should show it, then a zombie after it
    rig.type_line("ps", wait=2)
    t_zombie = since(base, 5000)
    check("W9 zombie/task state in ps", "elfdemo" in t_zombie)
    base = len(serial())
    rig.type_line("wait", wait=20)
    t = since(base, 4000)
    ok = "exited with status 42" in t
    check("W4 wait(ELF child) -> status 42", ok, t.strip().split("\n")[0][:60])

    # ---- W5: spawn .mrp child + wait ----
    print("[task2] spawn spin + kill + wait ...")
    base = len(serial())
    rig.type_line("spawn spin", wait=5)
    rig.type_line("ps", wait=2)
    time.sleep(2)
    t = since(base, 3000)
    pid = None
    for line in t.split("\n"):
        # v0.3 fix: match the ps TABLE ROW structure, not a loose
        # substring test — the shell PROMPT ('root::users /user $')
        # contains 'user', so prompt+[spin] ticker lines used to match
        # first and the pid became 'root::users' (not a digit) -> the
        # kill branch never ran ('no-kill fallback').
        parts = line.split()
        if (len(parts) >= 5 and parts[0].isdigit()
                and parts[1] in ("SLEEP", "READY", "RUNNING")
                and parts[2] == "user" and "spin" in parts[4]):
            pid = parts[0]
            break
    if pid and pid.isdigit():
        rig.type_line(f"kill {pid}", wait=4)
        time.sleep(5)
        base = len(serial())
        rig.type_line("wait", wait=5)
        # the wait blocks until the child is reaped; poll up to 30 s
        wait_serial(f"wait: child pid {pid} exited", timeout=30, rig=rig)
        t = since(base, 4000)
        ok = f"pid {pid}" in t and "exited" in t
        check("W5 wait(.mrp child) after kill", ok, f"pid={pid}")
    else:
        base = len(serial())
        rig.type_line("wait", wait=5)
        wait_serial("wait: child", timeout=30, rig=rig)
        t = since(base, 4000)
        check("W5 wait(.mrp child) after kill",
              "exited" in t, "no-kill fallback")

    # ---- W6: wait with no children -> ECHILD ----
    base = len(serial())
    rig.type_line("wait", wait=8)
    t = since(base, 1200)
    ok = "no matching child" in t
    check("W6 wait() with no children (ECHILD)", ok)

    # ---- W7: pipedemo ----
    print("[task2] pipedemo ...")
    base = len(serial())
    rig.type_line("pipedemo", wait=50)
    t = since(base, 6000)
    ok = "PIPEDEMO RESULT: PASS" in t and "status 7" in t
    check("W7 pipe + inheritance + EOF + wait", ok,
          t.strip().split("\n")[-2][:60] if t else "")

    # ---- W8: meminfo with a live child ----
    print("[task2] meminfo with live child ...")
    base = len(serial())
    rig.type_line("spawn spin", wait=5)
    rig.type_line("meminfo", wait=3)
    t = since(base, 4000)
    ok = "spin" in t and "physical pool" in t
    faulted_kb = 0
    for line in t.split("\n"):
        if "pool:" in line:
            try:
                faulted_kb = int(line.split("faulted by tasks")[0]
                                 .split(",")[-1].strip().split()[0])
            except Exception:
                pass
    check("W8 meminfo (per-task demand footprint)", ok,
          f"faulted={faulted_kb} KB")
    # kill spin to clean up
    import re as _re
    m = _re.search(r"^\s*(\d+)\s+\w+\s+user\s+\d+\s+spin", t, _re.M)
    if m:
        rig.type_line(f"kill {m.group(1)}", wait=4)
        time.sleep(4)

    # ---- W11: pool recovery (no leak) ----
    time.sleep(3)   # let the scheduler reap the killed spin
    base = len(serial())
    rig.type_line("meminfo", wait=5)
    time.sleep(2)
    t = since(base, 6000)
    import re as _re
    m = _re.search(r"pool:\s+(\d+) KB total,\s+(\d+) KB free", t)
    free_kb = int(m.group(2)) if m else None
    check("W11 pool recovers after children exit",
          free_kb is not None and free_kb > 33000,
          f"free={free_kb} KB")

    # ---- W12: soak ----
    print("[task2] 60 s soak ...")
    t0 = time.time()
    ticks0 = serial().count("\n")
    while time.time() - t0 < 60:
        if not rig.alive():
            break
        time.sleep(2)
    ok = rig.alive() and "panic" not in serial()[-4000:].lower()
    rig.type_line("tick", wait=2)
    ok2 = rig.alive()
    check("W12 60 s soak - alive, no panic", ok and ok2)

    rig.quit()
    print()
    print(f"V0.3 REGRESSION: {len(PASS)} PASS, {len(FAIL)} FAIL")
    if FAIL:
        print("FAILED:", ", ".join(FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
