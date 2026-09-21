#!/usr/bin/env python3
"""regression_multitask.py — full QEMU regression (v2).

v2 fixes vs v1:
  * fast monitor I/O — sendkey drains 0.15 s (v1 drained 6 s per key!)
  * snake timing — the snake dies at the wall after ~5.2 s with no
    steering, so every canvas check now happens INSIDE the alive
    window, and the game is parked (F1) while we verify the frozen
    background; it is quit with 'q' while still alive (never leave a
    stray 'q' to poison the next command line)
  * doom is only typed after the shell prompt is confirmed back
  * boot detection matches the actual banner ("v 0 . 2" / "$ ")
  * round selectable: 1 = games/console, 2 = mtcc in-OS compile

Verifies:
  T1  boot OK, shell prompt (no reboot)
  T2  snake canvas drawn on console 0 (game ALIVE)
  T3  F1: new console clean — NO snake pixels leak onto terminal 2
  T3b snake frozen in background (parked at the draw gate)
  T4  F2: canvas restored (game resumes)
  T5  snake quits cleanly on 'q' (input path to ring-3 games)
  T6  doom RUNS (24 MB arena) — title screen pixels
  T7  F1/F2 while doom runs (shell over doom, canvas restored)
  T8  ps / kill / switch work across consoles
  T9  long-run: no spontaneous reboot, shell responsive
  M1-5 mtcc compiles multitask.c in-OS, spawn from C, no reboot
"""
import os
import sys
import time
import socket
import struct
import subprocess
import tempfile

sys.path.insert(0, "/home/z/my-project/equinox_os/extracted/equinox_os_v0.2_beta/scripts")
from fps_measure import load_font  # noqa: E402

QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]
ISO = "/home/z/my-project/equinox_os/extracted/equinox_os_v0.2_beta/dist/equinox.iso"
SERIAL = "/tmp/regen_serial.log"

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
               "-nic", "none", "-serial", f"file:{SERIAL}",
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

    # QEMU sendkey needs key NAMES, not literal chars, for punctuation.
    KEYMAP = {
        " ": "spc", ".": "dot", "/": "slash", ",": "comma",
        "-": "minus", ";": "semicolon", "=": "equal", "'": "apostrophe",
        "`": "grave_accent", "[": "bracket_left", "]": "bracket_right",
        "\\": "backslash", "\t": "tab",
    }

    def type_line(self, text, wait=0.8):
        for ch in text:
            self.sendkey(self.KEYMAP.get(ch, ch), wait=0.05)
        self.sendkey("ret", wait=wait)

    def dump(self, tag):
        path = os.path.join(self.dumpdir, f"{tag}.ppm")
        # NOTE: QEMU's monitor sends no reply for screendump — a long
        # drain here would block for the FULL timeout (15 s in v2,
        # which let the snake die at the wall before F1). Short drain;
        # the file-existence loop below is the real completion signal.
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


# ---- PPM analysis ----
def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise RuntimeError("not a raw PPM")
    i, dims = 2, []
    while len(dims) < 3:
        while data[i:i + 1] in b" \t\r\n":
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in b"\r\n":
                i += 1
            continue
        j = i
        while data[j:j + 1] not in b" \t\r\n":
            j += 1
        dims.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _max = dims
    return w, h, data[i:i + w * h * 3]


def count_colors(ppm_path, targets):
    """Count pixels whose RGB matches any target (given as 0xRRGGBB ints).
    v2: bytes.count() — C speed. (v1's pure-Python loop took 2-4 s per
    dump, and the snake hit the wall while Python was still counting.)"""
    w, h, rgb = read_ppm(ppm_path)
    n = 0
    for t in targets:
        needle = bytes(((t >> 16) & 255, (t >> 8) & 255, t & 255))
        n += rgb.count(needle)
    return n


def unique_colors(ppm_path, cap=200000):
    w, h, rgb = read_ppm(ppm_path)
    s = set()
    for p in range(0, w * h):
        off = p * 3
        s.add((rgb[off], rgb[off + 1], rgb[off + 2]))
        if len(s) > cap:
            break
    return len(s)


def text_rows(ppm_path, font, max_rows=8, max_cols=100):
    """OCR top-left text area (8x16 font, bright glyphs)."""
    w, h, rgb = read_ppm(ppm_path)
    rows = []
    for row in range(max_rows):
        line = ""
        for col in range(max_cols):
            bm = []
            for gy in range(16):
                b = 0
                for gx in range(8):
                    x, y = col * 8 + gx, row * 16 + gy
                    if x >= w or y >= h:
                        b = b  # out of range: leave zero bits
                        continue
                    off = (y * w + x) * 3
                    lum = (rgb[off] * 299 + rgb[off + 1] * 587
                           + rgb[off + 2] * 114) // 1000
                    if lum > 96:
                        b |= 1 << (7 - gx)
                bm.append(b)
            best, best_d, ch = "?", 999, " "
            for code in range(32, 127):
                d = sum(bin(bm[i] ^ font[code][i]).count("1") for i in range(16))
                if d < best_d:
                    best_d, best, ch = d, chr(code), chr(code)
            line += ch if best_d <= 24 else " "
        rows.append(line.rstrip())
    return rows


SNAKE_COLORS = [0x22CC44, 0xAAFF66, 0xFF4444, 0x224488, 0x1C1024, 0x100818]


def ser_read():
    with open(SERIAL, "rb") as f:
        return f.read().decode("latin1", "replace")


def wait_serial(token, timeout_s, poll=0.3):
    """Wait until token appears in the serial log."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        s = ser_read()
        if token in s:
            return True
        time.sleep(poll)
    return False


# ---------------------------------------------------------------- round 1
def round1():
    font = load_font()
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    print("[regen] round 1: booting QEMU (45 s)...", flush=True)
    q = Qemu(ISO)
    try:
        time.sleep(45)
        check("T1 qemu alive after boot", q.alive())
        ser = ser_read()
        check("T1 boot reached shell prompt",
              "user $" in ser or "v 0 . 2" in ser,
              f"serial {len(ser)} B")

        # ---------- T2: snake on console 0 (game ALIVE at dump time) ----
        q.type_line("snake", wait=0.4)
        time.sleep(1.0)                    # t≈1.4: canvas drawn, game alive
        dA = q.dump("A_snake")
        nA = count_colors(dA, SNAKE_COLORS)
        check("T2 snake canvas drawn", nA > 3000, f"snake px={nA}")

        # ---------- T3: F1 -> console 2 must be CLEAN -------------------
        q.sendkey("f1", wait=0.5)          # t≈2: park the game
        dB = q.dump("B_f1")
        nB = count_colors(dB, SNAKE_COLORS)
        rowsB = text_rows(dB, font)
        btext = " ".join(rowsB)
        check("T3 no snake pixels on new terminal", nB < 100, f"leak px={nB}")
        check("T3 new shell visible",
              "user $" in btext or "console" in btext.lower()
              or "shell" in btext.lower(), btext[:120])
        ser = ser_read()
        check("T3 new-shell message (EN)",
              "new console active" in ser or "new shell pid" in ser)

        time.sleep(2.5)                    # parked for 2.5 s — stays clean
        dC = q.dump("C_f1_still")
        nC = count_colors(dC, SNAKE_COLORS)
        check("T3b snake frozen in background", nC < 100, f"leak px={nC}")

        # ---------- T4: F2 -> canvas restored ---------------------------
        q.sendkey("f2", wait=0.6)          # t≈5.6: wake + resume
        dD = q.dump("D_f2")
        nD = count_colors(dD, SNAKE_COLORS)
        # v0.3: threshold 3000 -> 2000. The scheduler fix (real
        # blocking sleeps) slowed the snake's frame pacing, so at
        # t≈5.6 s the body is shorter than the old (instant-sleep)
        # calibration: measured 2832 px with the restore WORKING.
        # 0-ish = text console; >2000 = the game canvas is back.
        check("T4 canvas restored on F2", nD > 2000, f"snake px={nD}")

        # ---------- T5: quit snake WHILE ALIVE with 'q' -----------------
        q.sendkey("q", wait=0.2)           # game still alive (~2.9 s run)
        ok = wait_serial("game over - score", 5.0)
        ser = ser_read()
        check("T5 snake clean exit on 'q'", ok)
        check("T5 'q' reached the game (not the shell)",
              "Unknown command: 'q" not in ser)
        wait_serial("user $", 4.0)         # prompt back
        time.sleep(1.0)

        # ---------- T6: doom ---------------------------------------------
        q.type_line("doom", wait=1.0)
        print("  [info] doom loading (75 s)...", flush=True)
        time.sleep(75)
        dE = q.dump("E_doom")
        uniqE = unique_colors(dE)
        ser = ser_read()
        check("T6 doom started (loader msg)",
              "mrp: running 'doom.mrp'" in ser)
        # 24 MB hint + code + page-align lands at 25272 KB — 'arena 25'
        check("T6 doom 24MB arena", "arena 25" in ser)
        # palette-based title (320x200@8bpp upscaled): unique colors
        # stay in the 100-300 range — a text console or blank screen
        # would show far fewer; the 'shell text gone' check separates
        # it from text mode.
        check("T6 doom title screen pixels", uniqE > 50,
              f"unique colors={uniqE}")
        rowsE = text_rows(dE, font)
        etxt = " ".join(rowsE)
        check("T6 shell text gone (graphics mode)",
              "user $" not in etxt, etxt[:80])

        # ---------- T7: F1/F2 while doom runs ---------------------------
        # F2 = console_next_used(active, -1): cycles DOWN through used
        # consoles. From the new console (2) it lands on 1, not 0 — so we
        # press F2 twice to come back to the doom console (2 -> 1 -> 0).
        q.sendkey("f1", wait=1.0)
        dF = q.dump("F_doom_f1")
        rowsF = text_rows(dF, font)
        ftxt = " ".join(rowsF)
        check("T7 shell visible over doom (F1)",
              "user $" in ftxt or "console" in ftxt.lower(), ftxt[:100])
        q.sendkey("f2", wait=0.8)          # 2 -> 1 (text console)
        q.sendkey("f2", wait=0.8)          # 1 -> 0 (doom canvas)
        dG = q.dump("G_doom_f2")
        rowsG = text_rows(dG, font)
        gtxt = " ".join(rowsG)
        nG = count_colors(dG, SNAKE_COLORS)
        check("T7 doom canvas restored (F2 x2)",
              "user $" not in gtxt and "equinox" not in gtxt.lower(),
              gtxt[:80])

        # ---------- T8: ps / kill / switch from the text console --------
        q.sendkey("f1", wait=1.0)          # console 3 (fresh shell)
        q.type_line("ps", wait=1.5)
        ser = ser_read()
        check("T8 ps table", "PID" in ser and "STATE" in ser)
        q.type_line("spawn spin", wait=2.0)   # 60 s — survives the test
        ser = ser_read()
        check("T8 spawn spin",
              "spawn: task pid" in ser and "running in the background" in ser)
        pid = None
        for tok in ser.split("spawn: task pid ")[1:]:
            num = ""
            for ch in tok:
                if ch.isdigit():
                    num += ch
                else:
                    break
            if num:
                pid = int(num)
        if pid:
            q.type_line(f"kill {pid}", wait=2.0)
            ser = ser_read()
            check(f"T8 kill {pid}",
                  f"kill: pid {pid} marked dead" in ser)
        else:
            check("T8 kill (pid parse)", False, "no pid found")
        q.type_line("switch 0", wait=1.5)
        ser = ser_read()
        check("T8 switch 0", "[console] active: 0" in ser)
        # console 0 now runs doom — its shell cannot take commands
        # (doom owns the input). Return to a text console with F2
        # (hardware-level): console_next_used(0, -1) wraps to 3.
        q.sendkey("f2", wait=1.5)
        dT8 = q.dump("T8_f2_back")
        rowsT8 = text_rows(dT8, font, max_rows=6)
        t8txt = " ".join(rowsT8)
        check("T8 back on a text console (F2 wrap)",
              "user $" in t8txt or "ps" in t8txt or "switch" in t8txt,
              t8txt[:80])

        # ---------- T9: long-run stability -------------------------------
        print("  [info] long-run soak (60 s)...", flush=True)
        time.sleep(60)
        check("T9 no spontaneous reboot (60 s)", q.alive())
        before = ser_read()
        q.type_line("ps", wait=2.0)
        after = ser_read()
        check("T9 shell responsive after soak",
              len(after) > len(before), f"{len(before)} -> {len(after)} B")
    finally:
        q.quit()


# ---------------------------------------------------------------- round 2
def round2():
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    print("[regen] round 2: mtcc /test/multitask.c", flush=True)
    q2 = Qemu(ISO)
    try:
        time.sleep(45)
        q2.type_line("mtcc /test/multitask.c", wait=1.0)
        print("  [info] compiling in-OS (150 s)...", flush=True)
        ok = wait_serial("== multitasking demo ==", 150)
        ser = ser_read()
        check("M1 mtcc compiled multitask.c", ok,
              ser[-300:] if not ok else "")
        check("M2 spawn from C program",
              "spawn ok: hello.mrp is now pid" in ser)
        check("M3 parent/child concurrent", "parent alive (lap" in ser)
        check("M4 parent done + clean exit", "parent done" in ser)
        time.sleep(25)                     # background child finishes
        check("M5 no reboot after mtcc run", q2.alive())
        ser = ser_read()
        check("M5 child finished (hello output)", "hello" in ser.lower())
    finally:
        q2.quit()


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    t0 = time.time()
    if which in ("1", "all"):
        round1()
    if which in ("2", "all"):
        round2()
    print()
    print(f"==== RESULT ({time.time()-t0:.0f}s): "
          f"{len(PASS)} PASS, {len(FAIL)} FAIL ====")
    if FAIL:
        for f in FAIL:
            print(f"  FAILED: {f}")
        sys.exit(1)


if __name__ == "__main__":
    main()
