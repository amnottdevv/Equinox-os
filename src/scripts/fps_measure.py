#!/usr/bin/env python3
"""fps_measure.py — ukur FPS DOOM (build dengan -fps) via QEMU + OCR console.

DOOM -fps menulis "[fps] N" ke console kernel; put_char_vesa merendernya
di atas framebuffer. Kolom console 0..20 jatuh di area letterbox kiri
(168 px) yang TIDAK pernah ditimpa SYS_BLIT — jadi teks persisten dan
bisa di-OCR dari screendump dengan font CP437 8x16 kernel.

Pemakaian: python3 fps_measure.py [--secs 30] [--mode e1m1|title]
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.expanduser("~/morphos")
ISO = os.path.join(ROOT, "dist", "morphos.iso")
FONT_H = os.path.join(ROOT, "kernel", "library", "header", "font8x16.h")
QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]
ENV = dict(os.environ)
ENV["LD_LIBRARY_PATH"] = (
    os.path.expanduser("~/tools/root/usr/lib/x86_64-linux-gnu") + ":"
    + os.path.expanduser("~/tools/root/lib/x86_64-linux-gnu") + ":"
    + os.path.expanduser("~/tools/root/usr/lib"))


# ---------------------------------------------------------------- font ----
def load_font():
    """Parse font8x16[256][16] dari header C."""
    txt = open(FONT_H).read()
    txt = re.sub(r"/\*.*?\*/", "", txt, flags=re.S)   # buang komentar dulu
    body = txt[txt.index("{", txt.index("font8x16")):]
    body = body[:body.index("};")]
    glyph_sets = re.findall(r"\{([^{}]*)\}", body)
    font = []
    for g in glyph_sets:
        vals = [int(v, 0) for v in g.replace("\n", " ").split(",") if v.strip()]
        font.append(vals)
    assert len(font) == 256 and all(len(g) == 16 for g in font), \
        f"font parse aneh: {len(font)}"
    return font


def read_ppm(path):
    """Parse binary PPM (P6) -> (w, h, bytes RGB)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("bukan P6")
    pos = 2
    vals = []
    while len(vals) < 3:
        # skip whitespace + comments
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        m = re.match(rb"(\d+)", data[pos:])
        vals.append(int(m.group(1)))
        pos += len(m.group(1))
    pos += 1  # single whitespace after maxval
    w, h, _ = vals
    return w, h, data[pos:pos + w * h * 3]


def ocr_left_column(ppm_path, font):
    """OCR kolom console 0..20 (area letterbox kiri). Return list of strings."""
    w, h, rgb = read_ppm(ppm_path)
    chars = []
    for row in range(h // 16):
        line = ""
        for col in range(21):
            # bitmap 8x16: threshold luminance
            bm = []
            for gy in range(16):
                b = 0
                for gx in range(8):
                    x = col * 8 + gx
                    y = row * 16 + gy
                    off = (y * w + x) * 3
                    lum = (rgb[off] * 299 + rgb[off + 1] * 587
                           + rgb[off + 2] * 114) // 1000
                    if lum > 96:
                        b |= 1 << (7 - gx)
                bm.append(b)
            # match glyph: pilih hamming distance terkecil di ASCII printable
            best, best_d = "?", 999
            for code in range(32, 127):
                d = sum(bin(bm[i] ^ font[code][i]).count("1")
                        for i in range(16))
                if d < best_d:
                    best_d, best = d, chr(code)
            line += best if best_d <= 24 else " "
        chars.append(line.rstrip())
    return chars


# ---------------------------------------------------------------- qemu ----
class Mon:
    def __init__(self):
        self.sock_path = "/tmp/fpsmeas.sock"
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)
        self.proc = subprocess.Popen(
            [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "64",
             "-cdrom", ISO, "-display", "none", "-vga", "std", "-nic", "none",
             "-monitor", "unix:" + self.sock_path + ",server,nowait",
             "-no-reboot"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
        deadline = time.time() + 15
        while not os.path.exists(self.sock_path):
            if time.time() > deadline:
                raise RuntimeError("monitor socket tidak muncul")
            time.sleep(0.1)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(self.sock_path)
        self.s.settimeout(60)
        time.sleep(1.0)
        self.drain()

    def drain(self, t=0.5):
        self.s.settimeout(t)
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    break
        except socket.timeout:
            pass

    def cmd(self, line, wait=0.1):
        self.s.settimeout(30)
        self.s.sendall((line + "\n").encode())
        time.sleep(wait)
        self.drain(0.3)

    def sendkey(self, keys, hold_ms=None):
        if hold_ms is None:
            self.cmd("sendkey " + keys, 0.05)
        else:
            self.cmd(f"sendkey {keys} {hold_ms}", 0.05)

    def screendump(self, path):
        self.s.settimeout(30)
        self.s.sendall(("screendump " + path + "\n").encode())
        t0 = time.time()
        while time.time() - t0 < 10:
            if os.path.exists(path):
                s1 = os.path.getsize(path)
                time.sleep(0.06)
                if os.path.exists(path) and os.path.getsize(path) == s1 \
                        and s1 > 1000:
                    self.drain(0.25)
                    return True
            time.sleep(0.05)
        self.drain(0.25)
        return os.path.exists(path)

    def quit(self):
        try:
            self.cmd("quit", 0.3)
        except Exception:
            pass
        try:
            self.proc.terminate()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


KEYMAP = {
    " ": "spc", "-": "minus", "=": "equal", "[": "bracket_left",
    "]": "bracket_right", ";": "semicolon", "'": "apostrophe",
    "`": "grave_accent", ",": "comma", ".": "dot", "/": "slash",
    "\\": "backslash",
}


def sendkey_text(mon, text):
    """Kirim string teks via QEMU monitor sendkey (dengan nama key benar)."""
    for k in text:
        if k == "\n":
            mon.sendkey("ret")
        elif k in KEYMAP:
            mon.sendkey(KEYMAP[k])
        elif k.isupper():
            mon.sendkey("shift-" + k.lower())
        else:
            mon.sendkey(k)


# ---------------------------------------------------------------- main ----
def extract_fps(lines):
    """Cari baris '[fps] N' terakhir; return int atau None."""
    val = None
    for ln in lines:
        m = re.search(r"\[fps\]\s*(\d+)", ln)
        if m:
            val = int(m.group(1))
    return val


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=30)
    ap.add_argument("--mode", choices=["e1m1", "title"], default="e1m1")
    ap.add_argument("--label", default="run")
    ap.add_argument("--peek", type=lambda x: int(x, 0), default=None,
                    help="alamat fisik fps_frames (mode memory peek)")
    args = ap.parse_args()

    font = load_font()
    print(f"[fps-meas:{args.label}] mode={args.mode} durasi={args.secs}s "
          f"peek={hex(args.peek) if args.peek else None}")
    mon = Mon()

    def peek_frames():
        """Baca uint32 memori fisik guest via monitor xp (format desimal)."""
        mon.s.settimeout(30)
        mon.s.sendall(f"xp /1dw 0x{args.peek:x}\n".encode())
        time.sleep(0.2)
        mon.s.settimeout(2.0)
        buf = b""
        try:
            while True:
                d = mon.s.recv(65536)
                if not d:
                    break
                buf += d
        except socket.timeout:
            pass
        # format: "00000000005976f0:         42"
        m = re.search(rb"^\s*[0-9a-f]+:\s+(-?\d+)", buf, re.M)
        return int(m.group(1)) if m else None

    try:
        print("[fps-meas] boot 50s...")
        time.sleep(50)
        mon.sendkey("ret")
        time.sleep(1.0)

        cmdline = ("doom -warp 1 -fps" if args.mode == "e1m1"
                   else "doom -fps")
        sendkey_text(mon, cmdline)
        mon.sendkey("ret")
        print(f"[fps-meas] '{cmdline}' diketik; tunggu render 25s...")
        time.sleep(25)

        readings = []
        dump = f"/tmp/fps_{args.label}.ppm"
        t_end = time.time() + args.secs
        prev = None
        prev_t = None
        while time.time() < t_end:
            if args.peek:
                if args.mode == "e1m1":
                    mon.sendkey("w", 1800)   # gerak terus: beban realistis
                t0 = time.time()
                v = peek_frames()
                t1 = time.time()
                if v is not None and prev is not None and v >= prev \
                        and t1 > prev_t:
                    fps = (v - prev) / (t1 - prev_t)
                    readings.append(fps)
                    print(f"[fps-meas]   counter={v} "
                          f"dt={t1 - prev_t:.2f}s fps={fps:.1f}")
                prev = v
                prev_t = t1
                time.sleep(0.8)
                continue
            if args.mode == "e1m1":
                mon.sendkey("w", 1800)
            if mon.screendump(dump):
                lines = ocr_left_column(dump, font)
                v = extract_fps(lines)
                if v is not None:
                    readings.append(v)
                    print(f"[fps-meas]   fps={v}")
                else:
                    tail = [l for l in lines if l.strip()][-3:]
                    print(f"[fps-meas]   (belum ada angka; tail={tail})")
            time.sleep(1.0)

        if readings:
            rs = sorted(readings)
            med = rs[len(rs) // 2]
            print(f"[fps-meas:{args.label}] HASIL: median={med:.1f} fps "
                  f"min={rs[0]:.1f} max={rs[-1]:.1f} n={len(rs)}")
        else:
            print(f"[fps-meas:{args.label}] GAGAL: tidak ada pembacaan")
    finally:
        mon.quit()


if __name__ == "__main__":
    main()
