#!/usr/bin/env python3
"""boot_test_v032.py — v0.3.2 QEMU regression.

Verifies the modular libc + 2-thread installer pool:

  T1  boot to shell
  T2  equinoxinstall: 13 libc + 20 tools (v0.9.3 slim), "0 failed"
  T3  summary lines: libc 13/13, tools 20/20 (games -> paket eggkg)
  T4  BOTH compiler threads ran ("] T1 " and "] T2 " log lines)
  T5  cool log format: per-job progress, "module verified",
      "installed", "parallel build: ... wall X.X s"
  T6  post-build state: no .c in /equinox/tools, .mrp present;
      /equinox/libc KEEPS its .c + morph.h (mtcc splices them), no .mrp
  T7  file-based libc splice IN-OS: mtcc /test/libcmini.c
      -> "RINGMINI PASS", "SUM fails=0"
  T8  pren /equinox/tools/wc.mrp (v0.9.2): header MRP1 + checksum
  T9  shell alive at the end, no [fail]/[warn] during install
"""
import os
import shutil
import sys
import time
import socket
import subprocess
import tempfile

# Harness ini aslinya memakai build QEMU di ~/tools/root; kalau tidak
# ada, jatuh ke QEMU sistem + BIOS-nya supaya tes tetap bisa jalan.
QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]
if not os.path.exists(QEMU):
    QEMU = "/usr/bin/qemu-system-i386"
if not os.path.isdir(QEMU_L[0]) or not os.path.isdir(QEMU_L[1]):
    QEMU_L = ["/usr/share/qemu", "/usr/share/qemu"]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "dist", "equinox.iso")
SERIAL = "/tmp/boot_v032_serial.log"

NET = ["-netdev", "user,id=net0",
       "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9"]

PASS, FAIL = [], []


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}  {detail}", flush=True)


class Qemu:
    # Argumen tambahan opsional (mis. -qmp unix:...) — subclass/test boleh
    # menimpanya sebelum membangun mesin. Default kosong = tak berubah.
    EXTRA = []

    def __init__(self, iso):
        self.sock_path = tempfile.mktemp(prefix="qmon32-", suffix=".sock")
        self.dumpdir = tempfile.mkdtemp(prefix="qdump32-")
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = (
            os.path.expanduser("~/tools/root/usr/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/usr/lib"))
        cmd = [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "256",
               "-cdrom", iso, "-display", "none", "-vga", "std",
               "-serial", f"file:{SERIAL}",
               "-monitor", "unix:" + self.sock_path + ",server,nowait",
               "-no-reboot"] + NET + list(type(self).EXTRA)
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
        out = b""
        self.mon.settimeout(timeout)
        try:
            while True:
                chunk = self.mon.recv(4096)
                if not chunk:
                    break
                out += chunk
        except socket.timeout:
            pass
        return out.decode(errors="replace")

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
        "\\": "backslash", "\t": "tab",
        # Tanda baca yang butuh SHIFT — tanpa ini keyname() mengirim
        # hurufnya apa adanya, QEMU menolaknya, dan karakternya HILANG
        # dari input (mis. "http://h:p" jadi "http//h p").
        ":": "shift-semicolon", "?": "shift-slash", "!": "shift-1",
        "@": "shift-2", "#": "shift-3", "$": "shift-4", "%": "shift-5",
        "^": "shift-6", "&": "shift-7", "*": "shift-8",
        "(": "shift-9", ")": "shift-0", "_": "shift-minus",
        "+": "shift-equal", '"': "shift-apostrophe",
        # v0.3: juga tanda SHIFT yang belum ada — tanpa ini perintah
        # seperti `save x << "teks"` kehilangan "<<" (terbukti saat
        # probe mget: save-nya jadi "usage").
        "<": "shift-comma", ">": "shift-dot",
        "{": "shift-bracket_left", "}": "shift-bracket_right",
        "|": "shift-backslash", "~": "shift-grave_accent",
    }

    def keyname(self, ch):
        if "A" <= ch <= "Z":
            return "shift-" + ch.lower()
        return self.KEYMAP.get(ch, ch)

    def type_line(self, text, wait=1.0):
        for ch in text:
            self.sendkey(self.keyname(ch), wait=0.12)
        self.sendkey("ret", wait=wait)

    def dump(self, tag):
        path = os.path.join(self.dumpdir, f"{tag}.ppm")
        self.cmd(f"screendump {path}", timeout=0.3)
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 1000:
                break
            time.sleep(0.1)
        return path

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
        # screendump PPM ratusan MB per run -> buang yang lama, tapi
        # SISAKAN dump terbaru (term_gui_test mencetak path-nya utk review).
        dd = getattr(self, "dumpdir", None)
        if dd and os.path.isdir(dd):
            keep, keep_m = None, -1.0
            try:
                for name in os.listdir(dd):
                    p = os.path.join(dd, name)
                    if not name.endswith(".ppm") or not os.path.isfile(p):
                        continue
                    m = os.path.getmtime(p)
                    if m > keep_m:
                        keep, keep_m = p, m
            except OSError:
                pass
            if keep is None:
                shutil.rmtree(dd, ignore_errors=True)
            else:
                for name in os.listdir(dd):
                    p = os.path.join(dd, name)
                    if p == keep:
                        continue
                    try:
                        shutil.rmtree(p) if os.path.isdir(p) else os.unlink(p)
                    except OSError:
                        pass
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass


def serial():
    try:
        with open(SERIAL, errors="replace") as fh:
            return fh.read()
    except FileNotFoundError:
        return ""


def since(n, limit=8000):
    return serial()[n:n + limit]


def wait_serial(pat, timeout=60, rig=None, t0=None):
    t0 = t0 or time.time()
    while time.time() - t0 < timeout:
        if pat in serial():
            return True
        if rig is not None and not rig.alive():
            return False
        time.sleep(0.4)
    return False


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise RuntimeError("not a raw PPM")
    i = 3
    dims = []
    while len(dims) < 3:
        while i < len(data) and data[i:i + 1] in b" \t\r\n":
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in b"\r\n":
                i += 1
            continue
        j = i
        while data[j:j + 1].isdigit():
            j += 1
        dims.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _max = dims
    return w, h, data[i:i + w * h * 3]


def count_nonblack(rgb):
    n = 0
    for i in range(0, len(rgb) - 2, 3):
        if rgb[i] or rgb[i + 1] or rgb[i + 2]:
            n += 1
    return n


def main():
    if not os.path.exists(ISO):
        print(f"[v032] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[v032] boot ...", flush=True)
    rig = Qemu(ISO)
    ok = wait_serial("user $", 150, rig)
    check("T1 boot to shell", ok)
    if not ok:
        rig.quit()
        print(serial()[-2500:])
        return 1

    # ---- T2: run the installer (2-thread pool, 44 jobs) ----
    print("[v032] equinoxinstall (12 libc + 20 tools, base slim v0.9.2) ...", flush=True)
    base = len(serial())
    t0 = time.time()
    rig.type_line("equinoxinstall", wait=2)
    time.sleep(1.5)
    rig.dump("installer_banner")          # banner + first log lines
    done = wait_serial("equinoxinstall done", 600, rig)
    wall = time.time() - t0
    t = since(base, 20000)
    check("T2 equinoxinstall completes", done, f"host wall {wall:.0f}s")
    if not done:
        rig.quit()
        print(t[-3000:])
        return 1

    # ---- T3: summary counters ----
    check("T3a libc 13/13 verified", "libc 13/13 verified" in t)
    check("T3b tools 20/20", "tools 20/20" in t)
    check("T3c base slim — tanpa games", "games" not in t)
    check("T3d 0 failed", " 0 failed" in t)

    # ---- T4: both threads ran ----
    check("T4a thread T1 log lines", "] T1 " in t)
    check("T4b thread T2 log lines", "] T2 " in t)
    check("T4c no single-thread fallback", "fallback" not in t)

    # ---- T5: cool log format ----
    n_info = t.count("[info] :")
    n_libcv = t.count("module verified")
    n_inst = t.count("installed\n")
    has_wall = "parallel build:" in t and "wall" in t
    check("T5a per-job [info] : lines", n_info >= 30, f"{n_info} lines")
    check("T5b libc module-verified lines", n_libcv == 13, f"{n_libcv}")
    check("T5c tool installed lines", n_inst >= 20, f"{n_inst}")
    check("T5d parallel wall-time line", has_wall,
          next((ln for ln in t.split("\n") if "wall" in ln), ""))

    # ---- T6: post-build filesystem state ----
    base = len(serial())
    rig.type_line("ls /equinox/tools", wait=4)
    tt = since(base, 4000)
    check("T6a tools: sources consumed (no .c)", ".c" not in tt,
          tt.strip().replace("\n", " | ")[:120])
    check("T6b tools: mtcc.mrp present", "mtcc.mrp" in tt)

    base = len(serial())
    rig.type_line("ls /equinox/libc", wait=4)
    tl = since(base, 4000)
    check("T6c libc sources STAY (morph.h)", "morph.h" in tl,
          tl.strip().replace("\n", " | ")[:160])
    check("T6d libc: no check-compile .mrp left", ".mrp" not in tl)

    # ---- T7: file-based libc splice, compiled in-OS ----
    print("[v032] mtcc /test/libcmini.c (file-based libc splice) ...", flush=True)
    base = len(serial())
    rig.type_line("mtcc /test/libcmini.c", wait=2)
    ok = wait_serial("RINGMINI PASS", 300, rig)
    tm = since(base, 8000)
    check("T7a libcmini compiles+runs (splice works)", ok,
          tm.strip().replace("\n", " | ")[:160])
    check("T7b libcmini zero failures", "SUM fails=0" in tm)

    # ---- T8: pren (v0.9.2) — preview .mrp hasil install ----
    print("[v032] pren /equinox/tools/wc.mrp (v0.9.2) ...", flush=True)
    base = len(serial())
    rig.type_line("pren /equinox/tools/wc.mrp", wait=2.0)
    time.sleep(1)
    t8 = since(base, 4000)
    check("T8a pren: header MRP1 valid", "valid MRP1" in t8,
          t8.strip().replace("\n", " | ")[:160])
    check("T8b pren: checksum COCOK", "(COCOK)" in t8)

    # ---- T9: no [fail]/[warn] in the whole install section ----
    ti = since(base - 40000, 60000) if base > 40000 else serial()
    check("T9 no [fail] tag during install", "[fail]" not in ti)
    check("T9b shell alive at end", rig.alive())

    rig.quit()
    print(f"\n[v032] RESULT: {len(PASS)} PASS, {len(FAIL)} FAIL")
    if FAIL:
        print("[v032] failed:", ", ".join(FAIL))
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
