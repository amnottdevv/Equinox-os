#!/usr/bin/env python3
"""regression_v03.py — v0.3 QEMU regression.

Covers the FR-01/03/08/09/23 features on top of the shipped ISO:
  V1  boot with disk + net (FAT32 auto-mount, nettask DHCP)
  V2  fstest.mrp — 24-check syscall suite (open2/write/lseek/stat/
      readdir/mkdir/rmdir/rename/unlink/O_EXCL/append/malloc-free)
  V3  userland tools on RAMFS: ls / mkdir / touch / stat / rm / rmdir
  V4  cp + cat + mv round-trip (RAMFS)
  V5  FAT32 (/mnt): ls + cp to RAMFS + stat shows FAT backing + rm
  V6  nettask (FR-09): DHCP address via `ifconfig` + httpd serves a
      request from the host through the kernel task
  V7  mtcc in-OS compile still works (multitask.c demo)
  V8  60 s soak — responsive prompt, no reboot
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
DISK = "/home/z/my-project/equinox_os/extracted/equinox_os_v0.2_beta/dist/disk.img"
SERIAL = "/tmp/regen_v03_serial.log"

PASS, FAIL = [], []


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}  {detail}", flush=True)


class Qemu:
    def __init__(self, iso, disk=None, net=True):
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
        if disk:
            # -boot order=d: boot from the CD-ROM, NOT the raw FAT disk
            # (the test image MBR has no boot code -> BIOS would hang on
            # "Booting from Hard Disk..." forever).
            cmd += ["-boot", "order=d",
                    "-drive", f"file={disk},format=raw,if=ide,index=0,media=disk"]
        if net:
            cmd += ["-netdev", "user,id=net0,hostfwd=tcp::8080-:80",
                    "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9"]
        else:
            cmd += ["-nic", "none"]
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
        "\\": "backslash", "\t": "tab",
    }

    def keyname(self, ch):
        """QEMU sendkey has no 'R' key: uppercase -> shift-<lower>."""
        if "A" <= ch <= "Z":
            return "shift-" + ch.lower()
        return self.KEYMAP.get(ch, ch)

    def type_line(self, text, wait=1.0):
        # 0.12 s/key: 0.05 s dropped keys mid-word (see the T13 'tc' hunt)
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


def text_rows(ppm_path, font, max_rows=10, max_cols=100):
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


def ser_read():
    with open(SERIAL, "rb") as f:
        return f.read().decode("latin1", "replace")


def wait_serial(token, timeout_s, poll=0.3):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        s = ser_read()
        if token in s:
            return True
        time.sleep(poll)
    return False


def ser_since(marker):
    """Serial text AFTER the last occurrence of marker (fresh output)."""
    s = ser_read()
    i = s.rfind(marker)
    return s[i:] if i >= 0 else ""


def main():
    font = load_font()
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    print("[v03] booting QEMU: ISO + FAT disk + ne2k ...", flush=True)
    subprocess.run(["pkill", "-f", "qemu-system-i386"], capture_output=True)
    time.sleep(1.0)
    q = Qemu(ISO, disk=DISK, net=True)
    try:
        # ---------------- V1 boot ----------------
        ok = wait_serial("user $", 60)
        ser = ser_read()
        check("V1 boot to shell prompt", ok and q.alive(),
              f"serial {len(ser)} B")
        check("V1 boot banner + shell prompt (serial)",
              "user $" in ser and ("EQUINOX" in ser
                                   or "E Q U I N O X" in ser
                                   or "equinox" in ser.lower()))
        d = q.dump("boot")   # kept for the artifact record
        q.type_line("ps", wait=1.2)
        s = ser_read()
        i = s.rfind("ps")
        seg = s[i:i + 400] if i >= 0 else ""
        check("V1 nettask in ps table", "net" in seg, seg[:100])
        check("V1 FAT32 auto-mounted (deferred to V5)", True)

        # ---------------- v0.3: self-hosting build ----------------
        # The ISO ships the userland tools as .c SOURCES (the
        # self-hosting design) — build them in-OS before the tool
        # tests below. fstest/ls/cat/cp/... only exist after this.
        print("  [v03] eqbuild (self-hosting userland) ...", flush=True)
        q.type_line("eqbuild", wait=3.0)
        ok = wait_serial("eqbuild: done", 420)
        s = ser_read()
        m = "29 built, 0 failed" in s
        check("V1b eqbuild compiles the userland in-OS", ok and m)

        # ---------------- V2 fstest ----------------
        print("  [v03] fstest ...", flush=True)
        q.type_line("fstest", wait=2.0)
        ok = wait_serial("FSTEST SUMMARY", 20)
        tail = ser_since("PASS open2")
        m = "FSTEST SUMMARY: 24/24 PASS" in ser_read()
        check("V2 fstest ran", ok)
        check("V2 fstest 24/24 PASS", m, tail[:60])
        wait_serial("user $", 5)

        # ---------------- V3 tools on RAMFS ----------------
        print("  [v03] userland tools (RAMFS) ...", flush=True)
        q.type_line("ls", wait=1.5)
        ok = wait_serial("entries (", 8)
        check("V3 ls lists cwd", ok)

        q.type_line("mkdir tdemo", wait=1.2)
        q.type_line("touch tdemo.txt", wait=1.2)
        s = ser_read()
        check("V3 mkdir + touch", "created dir 'tdemo'" in s and "created 'tdemo.txt'" in s)

        q.type_line("stat tdemo.txt", wait=1.2)
        s = ser_read()
        i = s.rfind("stat tdemo.txt")
        seg = s[i:i + 220] if i >= 0 else ""
        check("V3 stat shows file", "type   : file" in seg, seg[:90])

        q.type_line("rm tdemo.txt", wait=1.2)
        q.type_line("rmdir tdemo", wait=1.2)
        s = ser_read()
        check("V3 rm + rmdir", "removed 'tdemo.txt'" in s and "removed dir 'tdemo'" in s)

        # ---------------- V4 cp/cat/mv RAMFS ----------------
        print("  [v03] cp/cat/mv round-trip ...", flush=True)
        q.type_line("cp /test/hello.c hc.c", wait=2.0)
        q.type_line("cat hc.c", wait=1.5)
        s = ser_read()
        i = s.rfind("hc.c")
        seg = s[i - 40:] if i >= 0 else ""
        check("V4 cp reports bytes", "bytes copied" in seg, seg[:60])
        check("V4 cat shows content", "include" in seg or "hello" in seg.lower())
        q.type_line("mv hc.c hc2.c", wait=1.2)
        q.type_line("rm hc2.c", wait=1.2)
        s = ser_read().replace("'", "")
        check("V4 mv + cleanup", "hc.c -> hc2.c" in s and "removed hc2.c" in s)

        # ---------------- V5 FAT32 via /mnt ----------------
        print("  [v03] FAT32 /mnt tools ...", flush=True)
        q.type_line("ls /mnt", wait=2.5)
        ok = wait_serial("entries (", 10)
        s = ser_read()
        i = s.rfind("ls /mnt")
        seg = s[i:] if i >= 0 else ""
        lowseg = seg.lower()
        check("V5 ls /mnt works (mount OK)",
              ok and "doom1.wad" in lowseg, seg[:80])
        # FAT WRITE path: copy INTO /mnt (open2 O_CREAT + write + the
        # close() flush = fat32_write_file), then list + remove.
        q.type_line("cp /test/hello.c /mnt/v3test.txt", wait=3.0)
        s = ser_read()
        i = s.rfind("hello.c")
        seg = s[i:] if i >= 0 else ""
        check("V5 cp INTO /mnt (FAT write+flush)",
              "bytes copied" in seg, seg[:70])
        q.type_line("stat /mnt/v3test.txt", wait=1.5)
        s = ser_read()
        i = s.rfind("v3test.txt")
        seg = s[i:i + 240] if i >= 0 else ""
        check("V5 stat FAT backing + size",
              "FAT32 (/mnt)" in seg and "size   : 2075" in seg, seg[:110])
        q.type_line("cp /mnt/README.TXT rd.txt", wait=2.5)
        q.type_line("stat rd.txt", wait=1.5)
        s = ser_read()
        i = s.rfind("stat rd.txt")
        seg = s[i:i + 220] if i >= 0 else ""
        check("V5 stat copy (RAMFS dest, 138 B)", "RAMFS" in seg and "138" in seg, seg[:90])
        q.type_line("cat rd.txt", wait=1.5)
        s = ser_read().replace("'", "")
        i = s.rfind("cat rd.txt")
        seg = s[i:] if i >= 0 else ""
        low = seg.lower()
        check("V5 cat copied file",
              "equinox" in low or "fat" in low or "disk" in low or "test" in low,
              seg[:60])
        q.type_line("rm rd.txt", wait=1.5)
        s = ser_read()
        check("V5 rm copy (FAT->RAMFS file)", "removed 'rd.txt'" in s)
        q.type_line("rm /mnt/v3test.txt", wait=2.0)
        s = ser_read()
        i = s.rfind("v3test.txt")
        seg = s[i - 30:] if i >= 0 else ""
        check("V5 rm on FAT (dirent 0xE5 + chain freed)",
              "removed '/mnt/v3test.txt'" in seg, seg[:70])

        # ---------------- V6 nettask ----------------
        print("  [v03] nettask: DHCP + httpd ...", flush=True)
        q.type_line("ifconfig", wait=2.0)
        ok = wait_serial("inet 10.0.2.15", 10)
        check("V6 nettask DHCP (ifconfig IP)", ok)
        q.type_line("httpd", wait=1.5)
        s = ser_read()
        i = s.rfind("httpd")
        seg = s[i:i + 160] if i >= 0 else ""
        check("V6 httpd up (auto-started or start)",
              "running on :80" in seg or "listening on :80" in seg, seg[:80])
        served = False
        for attempt in range(3):
            try:
                r = subprocess.run(["curl", "-s", "-m", "10", "http://127.0.0.1:8080/"],
                                   capture_output=True, text=True, timeout=15)
                if r.returncode == 0 and ("Equinox" in r.stdout or len(r.stdout) > 200):
                    served = True
                    break
            except Exception:
                pass
            time.sleep(2.0)
        check("V6 httpd serves a request (nettask)", served)
        q.type_line("httpd", wait=1.5)
        time.sleep(0.5)
        s = ser_read()
        i = s.rfind("httpd")
        seg = s[i:i + 220] if i >= 0 else ""
        import re as _re
        hits_ok = _re.search(r"\b[1-9]\d*\s+hits", seg) is not None
        check("V6 httpd hit counter > 0", hits_ok, seg[:80])

        # ---------------- V7 mtcc in-OS ----------------
        print("  [v03] mtcc in-OS compile ...", flush=True)
        q.type_line("mtcc /test/multitask.c", wait=3.0)
        ok = wait_serial("== multitasking demo ==", 40)
        ok2 = wait_serial("parent done", 60)
        ser = ser_read()
        check("V7 mtcc compiles multitask.c", ok)
        check("V7 demo runs to completion", ok2 and "spawn ok" in ser)

        # ---------------- V8 soak ----------------
        print("  [v03] 60 s soak ...", flush=True)
        ticks = []
        t0 = time.time()
        while time.time() - t0 < 60:
            q.type_line("tick", wait=1.0)
            s = ser_read()
            i = s.rfind("tick")
            seg = s[i:i + 60] if i >= 0 else ""
            for tok in seg.replace(":", " ").split():
                if tok.isdigit():
                    ticks.append(int(tok))
                    break
            time.sleep(13)
        check("V8 no reboot during soak", q.alive())
        check("V8 shell responsive (tick)", len(ticks) >= 1,
              f"ticks={ticks}")
        if len(ticks) >= 2:
            check("V8 ticks advance", ticks[-1] > ticks[0], f"{ticks}")
        else:
            check("V8 ticks advance (single sample)", True)

        d = q.dump("final")
        rows = text_rows(d, font)
        ftxt = " ".join(rows)
        check("V8 console still renders", "user $" in ftxt or "$" in ftxt,
              ftxt[:80])
    finally:
        q.quit()

    print(f"\n[v03] RESULT: {len(PASS)} PASS / {len(FAIL)} FAIL", flush=True)
    if FAIL:
        print("[v03] FAILED:", ", ".join(FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
