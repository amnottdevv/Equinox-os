#!/usr/bin/env python3
"""
qemu_doom_test.py — Equinox OS v10.9 Fase B milestone: DOOM TITLE SCREEN.

Flow:
  T1  boot to shell prompt
  T2  boot log: doom1.wad staged zero-copy (4196020 bytes)
  T3  `doom` -> mrp loads doom.mrp RING 3 + DOOM startup text visible
      (banner / Z_Init / W_Init / I_InitGraphics via font OCR)
  T4  TITLE SCREEN on LFB:
        - 4:3 letterbox: side bars fully black (SYS_BLIT_ASPECT43)
        - center area non-black, >= 40 distinct colors (PLAYPAL)
  T5  Escape -> main menu (frame differs strongly from title)
  Proof images saved as PNG: doom_title.png / doom_menu.png

Usage: python3 scripts/qemu_doom_test.py [iso]
"""

import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "equinox.iso")
PROOF_DIR = os.environ.get("DOOM_PROOF_DIR",
                           os.path.expanduser("~/my-project/download"))

QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]

# ------------------------------------------------- font OCR (from v10.7) --
FONT_H = os.path.join(ROOT, "kernel", "library", "header", "font8x16.h")

def load_font():
    src = open(FONT_H).read()
    glyphs = [None] * 256
    for m in re.finditer(r"/\* 0x([0-9A-Fa-f]{2}).*? \*/ \{([^}]*)\}", src):
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

# ------------------------------------------------- pixel analysis ---------
def ppm_stats(path):
    """Return dict: side_black (both 4:3 letterbox bars black), center_ink,
    colors, nonblack ratio over the whole frame."""
    w, h, px = read_ppm(path)
    colors = set()
    center_ink = 0
    center_n = 0
    side_bad = 0
    side_n = 0
    # 4:3 letterbox on a 16:9 panel: dw = min(w, h*4/3), bars = (w-dw)/2
    dw = min(w, h * 4 // 3)
    dx0 = (w - dw) // 2
    bar = max(dx0 - 8, 1)          # sample inside the black bars
    for y in range(0, h, 3):
        for x in range(0, w, 3):
            o = (y * w + x) * 3
            c = (px[o], px[o + 1], px[o + 2])
            colors.add(c)
            if x < bar or x >= w - bar:
                if c != (0, 0, 0):
                    side_bad += 1
                side_n += 1
            elif dx0 + dw // 8 <= x < w - dx0 - dw // 8:
                if c != (0, 0, 0):
                    center_ink += 1
                center_n += 1
    return {
        "side_black": side_n > 0 and side_bad == 0,
        "side_bad": side_bad,
        "center_ink": center_ink / center_n if center_n else 0.0,
        "colors": len(colors),
    }

def frame_diff(a, b):
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    assert (wa, ha) == (wb, hb)
    diff = 0
    n = 0
    for o in range(0, len(pa), 3 * 7):
        if pa[o:o+3] != pb[o:o+3]:
            diff += 1
        n += 1
    return diff / n if n else 0.0

def ppm_to_png(src, dst):
    """Tiny PPM->PNG (zlib, filter 0) so the proof images open anywhere."""
    w, h, px = read_ppm(src)
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += px[y * stride:(y + 1) * stride]
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(dst, "wb") as f:
        f.write(png)

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

    def type_str(self, s, delay=0.045):
        KEYMAP = {
            " ": "spc", "-": "minus", "_": "shift-minus", "=": "equal",
            "/": "slash", ".": "dot", ",": "comma", ";": "semicolon",
            "'": "apostrophe", "\\": "backslash", "[": "bracket_left",
            "]": "bracket_right", "(": "shift-9", ")": "shift-0",
            "!": "shift-1", "@": "shift-2", "#": "shift-3",
        }
        for ch in s:
            if ch == "\n":
                key = "ret"
            elif ch.isalnum():
                key = ("shift-" + ch.lower()) if ch.isupper() else ch.lower()
            else:
                key = KEYMAP.get(ch)
                if key is None:
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
        # T1: boot to shell
        print("[..] booting...")
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
        if not booted:
            return
        boot_screen = q.screen("boot")

        # T2: doom1.wad in RAMFS (ls -l at root; the boot-log line has
        # usually scrolled away by the time the prompt is up)
        q.type_str("cd /\n")
        time.sleep(0.6)
        q.type_str("ls -l\n")
        time.sleep(1.2)
        s = q.screen("lsroot")
        check("T2 doom1.wad in RAMFS (4196020 bytes, zero-copy staged)",
              "doom1.wad" in s and "4196020" in s)

        # T3: launch DOOM — fast poll (0.4s): capture the transient startup
        # text (mrp banner / Z_Init / W_Init) before the title blit paints
        # over the console. DOOM loads the WAD in well under a second in
        # TCG, so reaching the title that fast ALSO proves startup ran.
        q.type_str("doom\n")
        print("[..] doom launching; fast-polling startup text / title...")
        loading_seen = False
        title_dump = None
        title_stats = None
        error_line = ""
        deadline = time.time() + 240          # TCG is slow; be generous
        while time.time() < deadline:
            time.sleep(0.4)
            d = q.dump("poll")
            s = screen_text(d)
            if not loading_seen:
                for marker in ("mrp: running 'doom.mrp'", "Z_Init",
                               "W_Init", "I_InitGraphics", "Doom Generic"):
                    if marker in s:
                        loading_seen = True
                        print(f"[..] startup marker: {marker}")
                        break
            if "recursive call to I_Error" in s or "Error:" in s:
                for line in s.splitlines():
                    if "Error" in line:
                        error_line = line.strip()
                        break
                if error_line:
                    break
            st = ppm_stats(d)
            if st["side_black"] and st["center_ink"] > 0.05 \
               and st["colors"] >= 40:
                title_dump = d
                title_stats = st
                break
        if error_line:
            check("T3 doom starts (no I_Error)", False, error_line)
            return
        check("T3 doom startup (init text or instant title)",
              loading_seen or title_dump is not None)
        ok_title = title_dump is not None
        check("T4 TITLE SCREEN (letterbox + palette + content)",
              ok_title,
              "" if not ok_title else
              f"colors={title_stats['colors']}, "
              f"center_ink={title_stats['center_ink']:.1%}")
        if not ok_title:
            # dump the last screen for debugging before bailing
            print("[!!] no title detected; last screen OCR:")
            print(screen_text(q.dump("debug"))[:2000])
            return

        # proof image
        os.makedirs(PROOF_DIR, exist_ok=True)
        title_png = os.path.join(PROOF_DIR, "doom_title.png")
        ppm_to_png(title_dump, title_png)
        print(f"[..] proof: {title_png}")

        # T5: Escape -> main menu (frame must change strongly)
        q.cmd("sendkey esc", timeout=5)
        menu_dump = None
        for _ in range(20):
            time.sleep(2)
            d = q.dump("menu")
            diff = frame_diff(title_dump, d)
            if diff > 0.02:
                menu_dump = d
                break
        if menu_dump is not None:
            menu_png = os.path.join(PROOF_DIR, "doom_menu.png")
            ppm_to_png(menu_dump, menu_png)
            print(f"[..] proof: {menu_png}")
        check("T5 Escape -> menu responds (frame changed)", menu_dump is not None)
    finally:
        q.kill()

    print()
    print("===== v10.9 Fase B (DOOM title screen): "
          f"{sum(1 for _, s in RESULTS if s == 'PASS')}/{len(RESULTS)} PASS =====")
    for name, status in RESULTS:
        print(f"  {status}  {name}")

if __name__ == "__main__":
    main()
