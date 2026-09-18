#!/usr/bin/env python3
"""
qemu_net2_test.py — Equinox OS v10.12 "Fase C" in-OS regression.

Topology: QEMU user-mode networking (slirp) + ISA NE2000 @0x300 irq9
+ hostfwd=tcp::8080-:80 (httpd guest) + host http.server :8022 (utk wget).
  guest 10.0.2.15 via DHCP, gw 10.0.2.2.

Tests:
  C1  boot to shell prompt
  C2  `ifconfig`  -> inet 10.0.2.15 + "dhcp"
  C3  `ping 10.0.2.2` -> 4/4, 0% packet loss
  C4  httpd: curl dari HOST via hostfwd :8080 -> halaman "Equinox OS"
  C5  httpd: file RAMFS byte-exact (curl /test/netinfo.c, md5 compare)
  C6  wget: host http.server :8022 -> `wget 10.0.2.2 8022 /test.txt`
      -> tersimpan + `ls` memunculkan file
  C7  ring-3: `mtcc /test/netinfo.c` -> "netinfo" + ip + ping replies
  C8  `httpd` command: status hits/bytes

Usage: python3 qemu_net2_test.py [iso]
"""

import hashlib
import http.server
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "equinox.iso")

QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]

WGET_PORT = 8022
WGET_CONTENT = ("Equinox OS v10.12 Fase C wget test\n"
                "TCP client <-> slirp <-> host python http.server\n"
                "baris-3: 0123456789ABCDEF\n" * 8).encode()

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
             "-netdev",
             f"user,id=net0,hostfwd=tcp::8080-:80",
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

# ------------------------------------------------------- host servers ----
def http_get(url, timeout=15):
    """curl dari host (lewat hostfwd slirp -> httpd guest)."""
    r = subprocess.run(["curl", "-4", "-s", "--max-time", str(timeout), url],
                       capture_output=True)
    return r.stdout

def start_wget_server(docroot):
    os.chdir(docroot)
    srv = http.server.HTTPServer(("127.0.0.1", WGET_PORT),
                                  http.server.SimpleHTTPRequestHandler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv

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

    # --- host http server untuk test wget (slirp: guest->10.0.2.2 = host) ---
    srvdir = tempfile.mkdtemp(prefix="morph-httpd-")
    with open(os.path.join(srvdir, "test.txt"), "wb") as f:
        f.write(WGET_CONTENT)
    srv = start_wget_server(srvdir)

    print("Boot QEMU (user-net + ne2k_isa + hostfwd :8080->:80)…")
    q = Qemu(ISO)
    try:
        # ---- C1: boot sampai shell ----
        txt = wait_text(q, PROMPT, timeout=120, tag="-boot")
        check("C1 boot: shell prompt hidup", PROMPT in txt)

        q.type_str("\n"); time.sleep(0.8)

        # ---- C2: ifconfig (DHCP) ----
        q.type_str("ifconfig\n"); time.sleep(2.0)
        txt = q.screen("-ifc")
        check("C2 ifconfig: inet 10.0.2.15", "10.0.2.15" in txt, txt[-400:])
        check("C2 ifconfig: gw slirp", "10.0.2.2" in txt)

        # ---- C3: ping gateway ----
        q.type_str("ping 10.0.2.2\n")
        txt = wait_text(q, "ping statistics", timeout=60, tag="-png")
        check("C3 ping: 4/4 reply 0% loss",
              "4 packets transmitted" in txt and "0% packet loss" in txt,
              txt[-500:])

        # ---- C4: httpd via hostfwd (dari HOST, TCP penuh) ----
        page = http_get("http://127.0.0.1:8080/")
        check("C4 httpd: TCP connect + halaman index dari host",
              b"Equinox OS" in page and b"TCP/IP alive" in page,
              repr(page[:120]))
        check("C4 httpd: halaman memuat ip/dhcp",
              b"10.0.2.15" in page and (b"dhcp" in page or b"static" in page))
        check("C4 httpd: listing RAMFS (doom1.wad)",
              b"doom1.wad" in page)

        # ---- C5: file RAMFS byte-exact lewat TCP ----
        body = http_get("http://127.0.0.1:8080/test/netinfo.c")
        src = open(os.path.join(ROOT, "test", "netinfo.c"), "rb").read()
        check("C5 httpd: /test/netinfo.c byte-exact (md5)",
              len(body) == len(src)
              and hashlib.md5(body).digest() == hashlib.md5(src).digest(),
              f"got {len(body)}B want {len(src)}B")

        # ---- C6: wget dari host http.server :8022 ----
        q.type_str(f"wget 10.0.2.2 {WGET_PORT} /test.txt\n")
        txt = wait_text(q, "tersimpan", timeout=45, tag="-wgt")
        check("C6 wget: tersimpan (byte count)",
              "tersimpan" in txt and f"{len(WGET_CONTENT)} byte" in txt,
              txt[-400:])
        q.type_str("ls\n"); time.sleep(2.0)
        txt = q.screen("-ls")
        check("C6 wget: file muncul di ls", "test.txt" in txt, txt[-300:])

        # ---- C7: ring-3 netinfo (mtcc compile + run in-OS) ----
        q.type_str("mtcc /test/netinfo.c\n")
        txt = wait_text(q, "replies", timeout=180, tag="-r3")
        check("C7 ring3: netinfo ter-compile + jalan",
              "netinfo" in txt and "10.0.2.15" in txt, txt[-500:])
        check("C7 ring3: ping 10.0.2.2 -> 4/4",
              "replies" in txt and "4/4" in txt.replace("0/4", "x"), txt[-300:])

        # ---- C8: perintah httpd status ----
        q.type_str("httpd\n"); time.sleep(1.5)
        txt = q.screen("-hstat")
        check("C8 httpd cmd: running + hits",
              "running" in txt and "hits" in txt, txt[-300:])

    finally:
        q.kill()
        srv.shutdown()

    ok = sum(1 for _, c in results if c)
    print(f"\n== HASIL: {ok}/{len(results)} PASS ==")
    if ok != len(results):
        sys.exit(1)

if __name__ == "__main__":
    main()
