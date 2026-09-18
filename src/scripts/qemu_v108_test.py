#!/usr/bin/env python3
"""
qemu_v108_test.py — Equinox OS v10.8 "libc layer" in-OS regression.

Tests:
  T1  boot to shell prompt
  T2  `ring` command        -> CPL 0, CS=0x08, ring map text
  T3  `syscalls` listing    -> 27 lseek / 28 printf / 29 ringinfo present
  T4  in-OS mtcc compile+run /test/libcmini.c
      (prelude splice + string + user heap + FILE I/O + printf + ring)
      -> "RINGMINI PASS", "ring=3", "SUM fails=0"
  T5  file created by the ring-3 program persists (ls)
  T6  regression: memmap + ringstats still alive

Usage: python3 qemu_v108_test.py [iso]
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

# QEMU + BIOS dari toolchain lokal (diekstrak di ~/tools)
QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]

# ------------------------------------------------- font OCR (from v10.7) --
import re as _re
FONT_H = os.path.join(ROOT, "kernel", "library", "header", "font8x16.h")

def load_font():
    """font8x16[256][16] dari header C -> list of 16-byte tuples."""
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
    """PPM -> list baris teks (48 baris x 170 kolom utk 1360x768)."""
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
             "-display", "none", "-vga", "std", "-nic", "none",
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
        "'": "apostrophe", '"': "shift-apostrophe", "\\": "backslash",
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
PROMPT = "root::users /user $"
results = []

def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  — {detail}" if detail and not cond else ""))

def wait_prompt(q, timeout=90):
    t0 = time.time()
    while time.time() - t0 < timeout:
        txt = q.screen("-wp")
        if PROMPT in txt.split("\n")[-6:]:
            return txt
        time.sleep(2)
    return q.screen("-wpTO")

def wait_text(q, needle, timeout=420, tag="-w"):
    """Poll layar sampai `needle` muncul (output panjang / compile lambat)."""
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
    load_font()
    print("Boot QEMU…")
    q = Qemu(ISO)
    try:
        # ---- T1: boot sampai shell ----
        txt = wait_text(q, PROMPT, timeout=90, tag="-boot")
        check("T1 boot: shell prompt hidup", PROMPT in txt)

        q.type_str("\n"); time.sleep(0.8)

        # ---- T2: command `ring` ----
        q.type_str("ring\n"); time.sleep(1.5)
        txt = q.screen("-ring")
        check("T2 ring: CPL 0 untuk shell", "ring 0" in txt, txt[-400:])
        check("T2 ring: selector CS=0x08 SS=0x10", "CS=0x08" in txt and "SS=0x10" in txt)
        check("T2 ring: peta ring 0 vs ring 3", "ring 3" in txt)

        # ---- T3: syscalls listing punya 27/28/29 ----
        q.type_str("syscalls\n"); time.sleep(1.5)
        txt = q.screen("-sysc")
        check("T3 syscalls: 27 lseek terdaftar", "27  lseek" in txt)
        check("T3 syscalls: 28 printf terdaftar", "28  printf" in txt)
        check("T3 syscalls: 29 ringinfo terdaftar", "29  ringinfo" in txt)

        # ---- T4: mtcc in-OS compile & run libcmini.c ----
        # tcc-run mode: `mtcc /test/libcmini.c` — prelude ~20KB di-splice,
        # 48 fungsi dikompilasi interpreter-style: butuh beberapa menit.
        q.type_str("mtcc /test/libcmini.c\n")
        print("  (compiling in-OS: prelude 20KB + 48 functions — polling…)")
        txt = wait_text(q, "RINGMINI", timeout=600, tag="-mtcc")
        check("T4 mtcc+prelude: RINGMINI PASS", "RINGMINI PASS" in txt, txt[-500:])
        check("T4 ring3: program user melaporkan ring=3", "ring=3" in txt)
        check("T4 printf syscall: strlen=6", "strlen=6" in txt)
        check("T4 FILE I/O: mini io=ring", "mini io=ring" in txt)
        check("T4 SUM fails=0", "SUM fails=0" in txt)

        # prompt balik (shell selamat — program exit rapi)
        txt = wait_prompt(q, timeout=30)
        check("T4 shell hidup setelah program exit", PROMPT in txt.split("SUM fails=0")[-1])

        # ---- T5: file buatan program ring 3 persisten ----
        q.type_str("cat libcmini.dat\n"); time.sleep(1.2)
        txt = q.screen("-cat")
        check("T5 cat file buatan program ring 3", "hello ring3 io" in txt)

        # ---- T6: regresi memmap + ringstats ----
        q.type_str("memmap\n"); time.sleep(1.5)
        txt = q.screen("-mm")
        check("T6 memmap: paging + ring 3 status", "ring 3" in txt or "RING 3" in txt)
        q.type_str("ringstats\n"); time.sleep(1.5)
        txt = q.screen("-rs")
        check("T6 ringstats: 3 ring buffer hidup", "kbd" in txt and "mouse" in txt and "audio" in txt)

    finally:
        q.kill()

    npass = sum(1 for _, okk in results if okk)
    print(f"\nHASIL v10.8: {npass}/{len(results)} PASS")
    for name, okk in results:
        if not okk:
            print(f"  FAIL: {name}")
    sys.exit(0 if npass == len(results) else 1)

if __name__ == "__main__":
    main()
