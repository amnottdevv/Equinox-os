#!/usr/bin/env python3
"""
qemu_v107_test.py — Ring 3 test suite untuk Equinox OS v10.7.

Metodologi sama seperti suite v10.4-v10.6 (font-OCR harness):
  - QEMU headless (-display none -vga std) + monitor lewat unix socket
  - ketik perintah via `sendkey`, baca layar via `screendump` (PPM)
  - OCR: cocokkan cell 8x16 layar dengan glyph font8x16.h

Tes inti (yang diminta user "handler biar ga langsung panic"):
  T5-T8: 4 program crash di ring 3 (div0, NULL ptr, tulis memori kernel,
         stack overflow) -> program dibunuh, SHELL HARUS TETAP HIDUP.
  T9:    mtcc self-host compile + run -> binary buatan in-OS juga ring 3.
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "dist", "equinox.iso")
FONT_H = os.path.join(ROOT, "kernel", "library", "header", "font8x16.h")
QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/seabios"),   # bios-256k
          os.path.expanduser("~/tools/root/usr/share/qemu")]      # kvmvapic dll

# ---------------------------------------------------------------- font ----

def load_font():
    """font8x16[256][16] dari header C -> list of 16-byte tuples."""
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

def sig16(g):
    return g

MATCH14 = {}
MATCH16 = {}
for code, g in enumerate(GLYPHS):
    if 32 <= code <= 126:
        MATCH16.setdefault(sig16(g), chr(code))
        MATCH14.setdefault(sig14(g), chr(code))
# karakter yang identik di 14 baris atas (mis. space vs _) hanya boleh
# diresolv lewat MATCH16; buang dari MATCH14 agar tidak salah tebak.
dup14 = {k for k, v in MATCH14.items()
         if sum(1 for c in MATCH14.values() if c == v) > 1}
for k in dup14:
    MATCH14.pop(k, None)

# ---------------------------------------------------------------- PPM ----

def read_ppm(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        raise ValueError("bukan P6: " + path)
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3][: w * h * 3]

# ---------------------------------------------------------------- OCR ----

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
        # buang trailing "kosong-setengah-isi" (NUL cell match '\x01')
        out_lines.append(line)
    return out_lines

def screen_text(ppm_path):
    lines = ocr_screen(ppm_path)
    return "\n".join(l.replace("\x01", "?") for l in lines)

def count_saturated(ppm_path, thresh=40):
    """Hitung pixel "berwarna" (max-min kanal > thresh) — dipakai bukti
    canvas game aktif (font 5x7 libgame TIDAK terbaca OCR console)."""
    w, h, px = read_ppm(ppm_path)
    n = 0
    for o in range(0, len(px) - 2, 3):
        r, g, b = px[o], px[o + 1], px[o + 2]
        if max(r, g, b) - min(r, g, b) > thresh:
            n += 1
    return n

# ------------------------------------------------------------- monitor ----

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

# --------------------------------------------------------------- tests ----

PROMPT = "root::users /user $"   # format prompt v10.6 (bentuk terbaca OCR)
results = []

def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  — {detail}" if detail and not cond else ""))

def wait_prompt(q, timeout=90):
    """Tunggu baris prompt muncul di layar (setelah output panjang)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        txt = q.screen("-wp")
        if PROMPT in txt.split("\n")[-6:]:
            return txt
        time.sleep(2)
    return q.screen("-wpTO")

def main():
    if not os.path.exists(ISO):
        print("ISO tidak ada:", ISO); sys.exit(2)
    print("Boot QEMU…")
    q = Qemu(ISO)
    try:
        # ---- T1: boot sampai shell ----
        # Tunggu boot lengkap (grub + kernel + countdown 5 detik) TANPA
        # mengirim skip-key: tombol skip yang nyasar di buffer keyboard
        # pernah jadi leading-space " memmap" di gets() pertama.
        time.sleep(22)
        txt = wait_prompt(q, timeout=60)
        check("T1 boot: shell prompt hidup di /user", PROMPT in txt)

        # Flush sisa buffer apa pun dengan satu ENTER kosong (baris kosong
        # = no-op di shell) supaya perintah pertama bersih.
        q.type_str("\n"); time.sleep(1.0)

        # ---- T2: memmap (paging + TSS aktif) ----
        q.type_str("memmap\n"); time.sleep(1.5)
        txt = q.screen("-memmap")
        check("T2 memmap: paging ON", "paging: ON" in txt)
        check("T2 memmap: region USER tampil", "USER" in txt and "MRP arena" in txt)
        check("T2 memmap: TSS ESP0 terpasang", "ring 3: TSS ESP0=0x" in txt)

        # ---- T3: ringstats (regresi v10.5) ----
        q.type_str("ringstats\n"); time.sleep(1.5)
        txt = q.screen("-ring")
        check("T3 ringstats: 3 ring buffer tampil",
              "kbd" in txt and "mouse" in txt and "audio" in txt)

        # ---- T4: hello.mrp via dispatch (exit NORMAL dari ring 3) ----
        q.type_str("hello\n"); time.sleep(1.5)
        txt = q.screen("-hello")
        check("T4 hello: program ring 3 jalan", "Halo dari program .mrp!" in txt)
        q.type_str("morph\n"); time.sleep(1.5)
        txt = q.screen("-hello2")
        check("T4 hello: readline dari ring 3 + balik shell",
              "Halo, morph!" in txt and PROMPT in txt.split("Halo, morph!")[-1])

        # ---- T5: crashde — division by zero ----
        q.type_str("run crashde.mrp\n"); time.sleep(2.5)
        txt = q.screen("-crashde")
        check("T5 crashde: laporan Division By Zero",
              "Division By Zero" in txt and "[user] program faulted" in txt)
        check("T5 crashde: kontrol balik ke shell (bukan panic)",
              "control returned to shell" in txt and
              "Kernel panic" not in txt)
        q.type_str("tick\n"); time.sleep(1.2)
        txt = q.screen("-tick5")
        check("T5 crashde: shell masih eksekusi perintah baru", "Timer ticks:" in txt)

        # ---- T6: crashptr — NULL pointer ----
        q.type_str("run crashptr.mrp\n"); time.sleep(2.5)
        txt = q.screen("-crashptr")
        check("T6 crashptr: Page Fault CR2=0",
              "Page Fault" in txt and "CR2=0x00000000" in txt)
        check("T6 crashptr: petunjuk NULL pointer", "(NULL pointer?)" in txt)

        # ---- T7: crashkmem — tulis memori kernel ----
        q.type_str("run crashkmem.mrp\n"); time.sleep(2.5)
        txt = q.screen("-crashkmem")
        check("T7 crashkmem: tulis kernel heap DITOLAK paging",
              "Page Fault" in txt and "write/user" in txt)
        check("T7 crashkmem: TIDAK ada 'LOLLOS' (proteksi bekerja)",
              "LOLLOS" not in txt)

        # ---- T8: crashstk — stack overflow ke guard page ----
        q.type_str("run crashstk.mrp\n"); time.sleep(2.5)
        txt = q.screen("-crashstk")
        check("T8 crashstk: Page Fault di region guard",
              "Page Fault" in txt and "CR2=0x0090" in txt)

        # ---- T9: mtcc self-host (compile in-OS lalu run) ----
        # PENTING: `mtcc file.c` TANPA -c = mode "tcc -run" (program
        # dieksekusi langsung dari memori — crash-nya membunuh mtcc).
        # Untuk menguji pipeline lengkap: -c (tulis .mrp) lalu run.
        print("  (mtcc compile ~1-2 menit, sabar…)")
        q.type_str("mtcc -c /test/divzero.c\n")
        t0 = time.time()
        ok_compile = False
        while time.time() - t0 < 300:
            txt = q.screen("-mtcc")
            # marker sukses: "wrote divzero.mrp (N bytes) — run divzero.mrp"
            if ("wrote" in txt and "bytes" in txt):
                ok_compile = True
                break
            if "Unknown command" in txt or "error" in txt.lower():
                break
            time.sleep(5)
        check("T9 mtcc: compile in-OS selesai (-c, tulis .mrp)", ok_compile)
        q.type_str("run divzero.mrp\n"); time.sleep(2.5)
        txt = q.screen("-divzero")
        check("T9 divzero (binary mtcc): dibunuh rapi di ring 3",
              "Division By Zero" in txt and "control returned to shell" in txt)
        q.type_str("pwd\n"); time.sleep(1.2)
        txt = q.screen("-pwd9")
        check("T9 shell hidup setelah kill program buatan mtcc", "/user" in txt)

        # ---- T10: snake (regresi game v10.6) ----
        # Dump CEPAT (2.5 s): snake tak-terkontrol menabrak dinding
        # ~5.4 s (kepala start di tengah, 8 langkah/detik) — dump 5 s
        # pernah menangkap lg_clear() kematian SETENGAH JALAN (layar
        # atas hitam = fill 1M pixel belum selesai), bukan bug kernel.
        q.type_str("snake\n"); time.sleep(2.5)
        txt = q.screen("-snake")
        sat = count_saturated(q.dump("-snakepx"))
        check("T10 snake: canvas game aktif (ular hijau + makanan merah)",
              (PROMPT not in txt) and sat > 500, f"sat={sat}")
        q.type_str("q"); time.sleep(4)
        txt = q.screen("-snakeq")
        check("T10 snake: keluar game kembali ke shell", PROMPT in txt)

        # ---- T11: syscalls listing + exit normal hello lagi ----
        q.type_str("clear\n"); time.sleep(1.0)   # cegah baris awal scroll
        q.type_str("syscalls\n"); time.sleep(1.5)
        txt = q.screen("-sysc")
        check("T11 syscalls: baris v10.7 tampil",
              "int 0x80" in txt and "ring 3" in txt)
        q.type_str("hello\n"); time.sleep(1.5)
        q.type_str("ring3\n"); time.sleep(1.5)
        txt = q.screen("-hello3")
        check("T11 hello: jalan lagi setelah 4 kill (arena bersih)",
              "Halo dari program .mrp!" in txt)

    finally:
        q.kill()

    print("\n===== RING 3 SUITE (v10.7) =====")
    npass = sum(1 for _, ok in results if ok)
    for name, ok in results:
        print(f"  {'ok ' if ok else 'XX '} {name}")
    print(f"TOTAL: {npass}/{len(results)} PASS")
    sys.exit(0 if npass == len(results) else 1)

if __name__ == "__main__":
    main()
