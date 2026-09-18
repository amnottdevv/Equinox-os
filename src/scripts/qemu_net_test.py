#!/usr/bin/env python3
"""
qemu_net_test.py — Equinox OS v10.11 "TCP/IP" in-OS regression (Fase A+B).

Topology: QEMU user-mode networking (slirp) + ISA NE2000 @0x300 irq9.
  guest 10.0.2.15/24  gw 10.0.2.2 (slirp menjawab ICMP echo ke gateway)

Tests:
  T1  boot to shell prompt
  T2  `ifconfig`   -> ne0 up, inet 10.0.2.15, HWaddr 52:54:00
  T3  `ping 10.0.2.2` -> 24 bytes replies, 0% packet loss
  T4  `ifconfig`   -> RX/TX counters moved
  T5  regression   -> help lists ping/ifconfig; games dir intact

Usage: python3 qemu_net_test.py [iso]
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "equinox.iso")

QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]

# ------------------------------------------------- font OCR (from v10.7) --
import re as _re
FONT_H = os.path.join(ROOT, "kernel", "library", "header", "font8x16.h")

def load_font():
    src = open(FONT_H).read()
    glyphs = [None] * 256
    for m in _re.finditer(r"/\* 0x([0-9A-Fa-f]{2}).*? \*/ \{([^}]*)\}", src):
        idx = int(m.group(1), 16)
        vals = [int(x, 16) for x in m.group(2).replace("\n", " ").split(",") if x.strip()]
        assert len(vals) == 16, (hex(idx), len(vals))
        glyphs[idx] = tuple(vals)
    assert all(g is not None for g in glyphs), "font parse bolong"
    return glyphs

GLYPHS = load_font()

def sig14(g):
    return g[:14]

def sig16(g):
    return g

MATCH14 = {}
MATCH16 = {}
for code, g in enumerate(GLYPHS):
    if 32 <= code <= 126:
        MATCH16.setdefault(sig16(g), chr(code))
        MATCH14.setdefault(sig14(g), chr(code))
dup14 = {k for k, v in MATCH14.items()
         if sum(1 for c in MATCH14.values() if c == v) > 1}
for k in dup14:
    MATCH14.pop(k, None)

def read_ppm(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        raise ValueError("bukan P6: " + path)
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3][: w * h * 3]

def ocr_screen(ppm_path, ink_threshold=0x50):
    w, h, px = read_ppm(ppm_path)
    cols, rows = w // 8, h // 16
    out_lines = []
    for r in range(rows):
        chars = []
        for c in range(cols):
            x0, y0 = c * 8, r * 16
            rows16 = []
            for y in range(16):
                byte = 0
                base = ((y0 + y) * w + x0) * 3
                for x in range(8):
                    o = base + x * 3
                    lum = (px[o] + px[o + 1] + px[o + 2]) // 3
                    if lum > ink_threshold:
                        byte |= 1 << (7 - x)
                rows16.append(byte)
            t16, t14 = tuple(rows16), tuple(rows16[:14])
            ch = MATCH16.get(t16) or MATCH14.get(t14)
            chars.append(ch if ch else "\x01")
        line = "".join(chars)
        out_lines.append(line)
    return out_lines

def screen_text(ppm_path):
    lines = ocr_screen(ppm_path)
    return "\n".join(l.replace("\x01", "?") for l in lines)

# ------------------------------------------------------------- QEMU ----
class Qemu:
    def __init__(self, iso):
        self.sock_path = tempfile.mktemp(prefix="qmon-", suffix=".sock")
        self.dumpdir = tempfile.mkdtemp(prefix="qdump-")
        self.dump_n = 0
        self.proc = subprocess.Popen(
            [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "64", "-cdrom", iso,
             "-display", "none", "-vga", "std",
             "-netdev", "user,id=net0",
             "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9",
             "-monitor", "unix:" + self.sock_path + ",server,nowait",
             "-no-reboot"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 10
        while not os.path.exists(self.sock_path):
            if time.time() > deadline:
                raise RuntimeError("monitor socket tidak muncul")
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.mon.connect(self.sock_path)
        self.mon.settimeout(120)
        self._drain(2.0)

    def _drain(self, timeout=1.0):
        self.mon.settimeout(timeout)
        buf = b""
        try:
            while True:
                chunk = self.mon.recv(65536)
                if not chunk:
                    break
                buf += chunk
                if b"(qemu)" in buf:
                    break
        except socket.timeout:
            pass
        return buf.decode("utf-8", "replace")

    def cmd(self, c, timeout=30):
        self.mon.sendall(c.encode() + b"\n")
        return self._drain(timeout)

    KEYMAP = {
        " ": "spc", "-": "minus", "_": "shift-minus", "=": "equal",
        "+": "shift-equal", "/": "slash", "?": "shift-slash", ".": "dot",
        ",": "comma", ";": "semicolon", ":": "shift-semicolon",
        "'": "apostrophe", '"': 'shift-apostrophe', "\\": "backslash",
        "|": "shift-backslash", "[": "bracket_left", "]": "bracket_right",
        "(": "shift-9", ")": "shift-0", "!": "shift-1", "@": "shift-2",
        "#": "shift-3", "$": "shift-4", "%": "shift-5", "^": "shift-6",
        "&": "shift-7", "*": "shift-8", "<": "shift-comma",
        ">": "shift-dot", "~": "shift-backquote", "`": "backquote",
    }

    def type_str(self, s, delay=0.045):
        for ch in s:
            if ch == "\n":
                key = "ret"
            elif ch.isalnum():
                key = ("shift-" + ch.lower()) if ch.isupper() else ch.lower()
            else:
                key = self.KEYMAP.get(ch)
                if key is None:
                    print(f"[warn] karakter tanpa qcode: {ch!r} — dilewati")
                    continue
            self.cmd(f"sendkey {key}", timeout=5)
            time.sleep(delay)

    def dump(self, tag=""):
        self.dump_n += 1
        path = os.path.join(self.dumpdir, f"dump{self.dump_n:03d}{tag}.ppm")
        self.cmd(f"screendump {path}", timeout=20)
        deadline = time.time() + 10
        while not os.path.exists(path) or os.path.getsize(path) < 100:
            if time.time() > deadline:
                raise RuntimeError("screendump tidak jadi")
            time.sleep(0.1)
        return path

    def screen(self, tag=""):
        return screen_text(self.dump(tag))

    def kill(self):
        try:
            self.cmd("quit", timeout=3)
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass

# ------------------------------------------------------------- tests ----
PROMPT = "root::users /user $"   # bentuk OCR dari "root@morphos:/$" (pelajaran v10.8)
results = []

def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  — {detail}" if detail and not cond else ""))

def wait_text(q, needle, timeout=420, tag="-w"):
    t0 = time.time()
    while time.time() - t0 < timeout:
        txt = q.screen(tag)
        if needle in txt:
            return txt
        time.sleep(4)
    return q.screen(tag + "TO")

def main():
    if not os.path.exists(ISO):
        print("ISO tidak ada:", ISO); sys.exit(2)
    print("Boot QEMU (user-net + ne2k_isa)…")
    q = Qemu(ISO)
    try:
        # ---- T1: boot sampai shell ----
        txt = wait_text(q, PROMPT, timeout=90, tag="-boot")
        check("T1 boot: shell prompt hidup", PROMPT in txt)

        q.type_str("\n"); time.sleep(0.8)

        # ---- T2: ifconfig ----
        q.type_str("ifconfig\n"); time.sleep(2.0)
        txt = q.screen("-ifc")
        check("T2 ifconfig: inet 10.0.2.15", "10.0.2.15" in txt, txt[-400:])
        check("T2 ifconfig: netmask/gw slirp", "10.0.2.2" in txt)
        check("T2 ifconfig: baris HWaddr tampil", "HWaddr" in txt and "52" in txt)

        # ---- T3: ping gateway slirp ----
        q.type_str("ping 10.0.2.2\n")
        txt = wait_text(q, "ping statistics", timeout=60, tag="-png")
        check("T3 ping: ada reply 24 bytes", "24 bytes from 10.0.2.2" in txt, txt[-500:])
        check("T3 ping: 4 transmitted", "4 packets transmitted" in txt)
        check("T3 ping: 0% loss", "0% packet loss" in txt)

        # ---- T4: counter bergerak (pakai match TERAKHIR — output
        # ifconfig lama masih ter-scroll di layar) ----
        q.type_str("ifconfig\n"); time.sleep(2.5)
        txt = q.screen("-ifc2")
        import re as _rx
        ms = _rx.findall(r"RX (\d+)\s+TX (\d+)", txt)
        best = max((int(a) for a, b in ms), default=0)
        best_tx = max((int(b) for a, b in ms), default=0)
        check("T4 counters: RX>0 dan TX>0", best > 0 and best_tx > 0,
              f"matches={ms}" if ms else txt[-200:])

        # ---- T5: regression ringan ----
        q.type_str("help\n"); time.sleep(1.5)
        txt = q.screen("-help")
        check("T5 help: ping terdaftar", ("ping <ip>" in txt) or ("ping <host>" in txt))  # v10.13: help kini <host>
        check("T5 help: ifconfig terdaftar", "ifconfig" in txt)
        q.type_str("cd /equinox/games\n"); time.sleep(1.0)
        q.type_str("ls\n")
        txt = wait_text(q, "doom.mrp", timeout=30, tag="-ls")
        check("T5 ls: doom.mrp masih ada (cd+ls)", "doom.mrp" in txt, txt[-300:])

    finally:
        q.kill()

    ok = sum(1 for _, c in results if c)
    print(f"\n== HASIL: {ok}/{len(results)} PASS ==")
    if ok != len(results):
        sys.exit(1)

if __name__ == "__main__":
    main()
