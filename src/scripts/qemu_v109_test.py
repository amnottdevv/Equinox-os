#!/usr/bin/env python3
"""
qemu_v109_test.py — Equinox OS v10.9 "Fase A" regression (DOOM prep).

Tests:
  T1  boot to shell prompt
  T2  boot log shows the big module staged zero-copy ("bigtest.wad")
  T3  `memmap`    -> new layout: 33 MB arena + 12 MB module staging
  T4  `syscalls`  -> 30 blit + 31 setpalette listed
  T5  `ring`      -> CPL 0, CS=0x08
  T6  `run /equinox/tools/blittest.mrp`
      -> 16 MB user arena malloc+verify, /bigtest.wad 300000-byte read via
         zero-copy path, 100 animated blit frames (fps report), letterbox
         4:3, "BLITTEST PASS"
  T6b visual: screendump mid-animation -> thousands of distinct colors
      (proves palette + scaled blit really paint the LFB)
  T7  `mtcc /test/libcmini.c` -> "RINGMINI PASS" (in-OS compiler regression)

Usage: python3 qemu_v109_test.py [iso]
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

MATCH16 = {}
MATCH14 = {}
for code, g in enumerate(GLYPHS):
    if 32 <= code <= 126:
        MATCH16.setdefault(tuple(g), chr(code))
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
        out_lines.append("".join(chars))
    return out_lines

def screen_text(ppm_path):
    lines = ocr_screen(ppm_path)
    return "\n".join(l.replace("\x01", "?") for l in lines)

def count_distinct_colors(ppm_path, step=97):
    """Sample pixels, return (distinct_color_count, nonblack_ratio)."""
    w, h, px = read_ppm(ppm_path)
    colors = set()
    nonblack = 0
    total = 0
    for o in range(0, len(px) - 2, 3 * step):
        c = (px[o], px[o + 1], px[o + 2])
        colors.add(c)
        total += 1
        if c != (0, 0, 0):
            nonblack += 1
    return len(colors), (nonblack / total if total else 0)

# ------------------------------------------------------------- QEMU ----
class Qemu:
    def __init__(self, iso):
        self.sock_path = tempfile.mktemp(prefix="qmon-", suffix=".sock")
        self.dumpdir = tempfile.mkdtemp(prefix="qdump-")
        self.dump_n = 0
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = (
            os.path.expanduser("~/tools/root/usr/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/lib/x86_64-linux-gnu")
            + ":" + os.path.expanduser("~/tools/root/usr/lib"))
        self.proc = subprocess.Popen(
            [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "64", "-cdrom", iso,
             "-display", "none", "-vga", "std", "-nic", "none",
             "-monitor", "unix:" + self.sock_path + ",server,nowait",
             "-no-reboot"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        deadline = time.time() + 15
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
RESULTS = []

def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    RESULTS.append((name, status))
    print(f"[{status}] {name}" + (f"  ({detail})" if detail else ""))
    return cond

def main():
    print(f"ISO: {ISO}")
    q = Qemu(ISO)
    try:
        # T1: boot to shell — poll the screen so we skip the countdown at
        # the RIGHT moment (a stray key after the prompt would leak a
        # leading space into the next shell command).
        print("[..] booting (polling for countdown / prompt)...")
        booted = False
        for _ in range(40):
            time.sleep(1)
            s = q.screen("bootpoll")
            if "root::users" in s and "$" in s:
                booted = True
                break
            if "press any key to skip" in s:
                q.cmd("sendkey spc", timeout=5)
                time.sleep(1)
        check("T1 boot to shell", booted)
        s = q.screen("boot")

        # T2: module ingestion — bigtest.wad in RAMFS root with full size
        # (functional zero-copy proof is T6; here we check the RAMFS node).
        # NOTE: `ls` takes no path argument — cd to / first.
        q.type_str("cd /\n")
        time.sleep(0.8)
        q.type_str("ls -l\n")
        time.sleep(1.5)
        s = q.screen("lsroot")
        check("T2 bigtest.wad in RAMFS (300000 bytes)",
              "bigtest.wad" in s and "300000" in s)
        q.type_str("cd /user\n")
        time.sleep(0.8)

        # T3: memmap
        q.type_str("memmap\n")
        time.sleep(1.5)
        s = q.screen("memmap")
        check("T3 memmap 33MB arena", "MRP arena 33 MB" in s)
        check("T3 memmap staging 12MB", "module staging 12 MB" in s)
        check("T3 memmap user stack 1MB", "user stack 1 MB" in s)

        # T4: syscalls 30/31
        q.type_str("syscalls\n")
        time.sleep(1.5)
        s = q.screen("syscalls")
        check("T4 syscall 30 blit", "30  blit" in s)
        check("T4 syscall 31 setpalette", "31  setpalette" in s)

        # T5: ring
        q.type_str("ring\n")
        time.sleep(1.5)
        s = q.screen("ring")
        check("T5 ring 0 CS=0x08", "ring 0" in s and "CS=0x08" in s)

        # T6: blittest (16MB arena + zero-copy wad + 100 blit frames)
        q.type_str("run /equinox/tools/blittest.mrp\n")
        time.sleep(8.0)               # typing + arena verify + anim start
        mid = q.dump("blitmid")
        ndistinct, nonblack = count_distinct_colors(mid)
        print(f"[..] mid-animation colors={ndistinct} nonblack={nonblack:.2%}")
        check("T6b blit paints LFB (palette in use, nonblack>50%)",
              ndistinct >= 200 and nonblack > 0.5,
              f"colors={ndistinct}, nonblack={nonblack:.1%}")

        # wait for BLITTEST PASS (poll OCR)
        passed = False
        fps_line = ""
        for _ in range(40):
            time.sleep(2)
            s = q.screen("blitpoll")
            if "BLITTEST PASS" in s:
                passed = True
                for line in s.splitlines():
                    if "stretch frames in" in line:
                        fps_line = line.strip()
                break
        check("T6 blittest PASS", passed, fps_line)
        check("T6 16MB arena + wad zero-copy read",
              passed and "SUMMARY arena=16MB wad=300000" in s)

        # T7: in-OS mtcc regression
        q.type_str("mtcc /test/libcmini.c\n")
        ok = False
        for _ in range(30):
            time.sleep(2)
            s = q.screen("mtcc")
            if "RINGMINI PASS" in s:
                ok = True
                break
        check("T7 mtcc libcmini RINGMINI PASS", ok)

    finally:
        q.kill()

    npass = sum(1 for _, st in RESULTS if st == "PASS")
    print(f"\n===== v10.9 Fase A: {npass}/{len(RESULTS)} PASS =====")
    for name, st in RESULTS:
        print(f"  {st}  {name}")
    sys.exit(0 if npass == len(RESULTS) else 1)

if __name__ == "__main__":
    main()
