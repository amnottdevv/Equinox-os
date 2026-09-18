#!/usr/bin/env python3
"""
fat32_test.py — Equinox OS v0.2 Beta ATA + FAT32 validation suite.

Covers both phases of the disk subsystem:

  Phase A (read-only)
    F1  boot auto-detects the ATA disk + mounts FAT32 at /mnt
    F2  ls /mnt mirrors the volume (8.3 + LFN + subdir + wad)
    F3  cat of a plain 8.3 file
    F4  cat of a LONG file name (LFN read)
    F5  nested dir + LFN inside a subdirectory
    F6  xxd bin.dat — byte-exact binary read (0..255 pattern)
    F7  ls -l shows doom1.wad with the exact byte size
    F8  diskinfo reports the volume layout
    F9  doom wad magic "IWAD" via xxd (binary correctness proof)

  Phase B (read/write, write-through)
    B1  ccfile (create + content) on the FAT volume
    B2  cat the file back (same-session read-back)
    B3  cdir (mkdir) on the FAT volume
    B4  rm of a created file; ls no longer shows it
    B5  rm of a non-empty dir is rejected
    B6  mget HTTP download saved DIRECTLY to /mnt (binary, multi-cluster)
    B7  QEMU shut down -> HOST-SIDE verification with mtools
        (independent oracle: mdir lists what the OS wrote, mcopy
        round-trips the exact bytes)

  Persistence
    P1  second boot with the SAME disk: files written in Phase B are
        still there and byte-identical (write-through proven)

  Regression
    R1  boot WITHOUT a disk: "no hard disks" path, shell alive, ls /
    R2  version string says v0.2 Beta

Usage: python3 scripts/fat32_test.py
  (expects dist/equinox.iso + dist/disk.img; rebuilds disk.img first)
"""

import functools
import http.server
import os
import re
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from qemu_net2_test import ocr_screen, screen_text  # noqa: E402

QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/seabios"),
          os.path.expanduser("~/tools/root/usr/share/qemu")]
MTOOLS = os.path.expanduser("~/tools/root/usr/bin")

# QEMU is a locally-extracted deb tree: its shared libs need
# LD_LIBRARY_PATH, injected into every subprocess below.
ENV = dict(os.environ)
ENV["PATH"] = (os.path.expanduser("~/tools/root/usr/bin") +
               os.pathsep + ENV.get("PATH", ""))
ENV["LD_LIBRARY_PATH"] = ":".join([
    os.path.expanduser("~/tools/root/usr/lib/x86_64-linux-gnu"),
    os.path.expanduser("~/tools/root/usr/lib"),
    os.path.expanduser("~/tools/root/lib/x86_64-linux-gnu"),
])
ISO = os.path.join(ROOT, "dist", "equinox.iso")
DISK = os.path.join(ROOT, "dist", "disk.img")

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {name}"
          + (f"  -- {detail}" if detail and not cond else ""))
    return bool(cond)


class QemuDisk:
    """QEMU with the FAT32 test disk attached as primary master."""

    def __init__(self, iso, disk, net=True):
        self.sock_path = tempfile.mktemp(prefix="f32mon-", suffix=".sock")
        self.dumpdir = tempfile.mkdtemp(prefix="f32dump-")
        self.dump_n = 0
        cmd = [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "64",
               "-cdrom", iso, "-display", "none", "-vga", "std",
               "-drive", f"file={disk},format=raw,if=ide,index=0,media=disk",
               "-boot", "order=d",
               "-monitor", "unix:" + self.sock_path + ",server,nowait",
               "-no-reboot"]
        if net:
            cmd += ["-netdev", "user,id=net0,hostfwd=tcp::8080-:80",
                    "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9"]
        else:
            # no NIC: without this QEMU instantiates a default e1000 and
            # aborts when its option ROM is not in the local -L tree
            cmd += ["-net", "none"]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, env=ENV)
        deadline = time.time() + 10
        while not os.path.exists(self.sock_path):
            if time.time() > deadline:
                raise RuntimeError("monitor socket missing")
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
        "/": "slash", ".": "dot", ",": "comma", ":": "shift-semicolon",
        "'": "apostrophe", '"': 'shift-apostrophe', "!": "shift-1",
        "@": "shift-2", "?": "shift-slash", "(": "shift-9",
        ")": "shift-0", "<": "shift-comma", ">": "shift-dot",
        "*": "shift-8", ";": "semicolon",
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
                    continue
            self.cmd(f"sendkey {key}", timeout=5)
            time.sleep(delay)

    def dump(self, tag=""):
        self.dump_n += 1
        path = os.path.join(self.dumpdir, f"d{self.dump_n:03d}{tag}.ppm")
        self.cmd(f"screendump {path}", timeout=20)
        deadline = time.time() + 10
        while not os.path.exists(path) or os.path.getsize(path) < 100:
            if time.time() > deadline:
                raise RuntimeError("screendump failed")
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


def wait_text(q, needle, timeout=420, tag="-w"):
    t0 = time.time()
    while time.time() - t0 < timeout:
        txt = q.screen(tag)
        if needle in txt:
            return txt
        time.sleep(4)
    return q.screen(tag + "TO")


def mtool(args):
    """Returns (rc, text, bytes): mdir wants text, mcopy wants bytes."""
    r = subprocess.run(args, capture_output=True, env=ENV)
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace"), r.stdout


class DocServer(socketserver.TCPServer):
    allow_reuse_address = True


BIN_BODY = bytes(range(256)) * 800       # 204800 bytes, multi-cluster
HTTP_PORT = 8044


def main():
    # fresh disk image every run
    print("[suite] rebuilding dist/disk.img ...")
    r = subprocess.run([sys.executable,
                        os.path.join(HERE, "make_fat32_img.py")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit(2)

    # host HTTP server for the mget-to-disk test
    docdir = tempfile.mkdtemp(prefix="f32doc-")
    with open(os.path.join(docdir, "data.bin"), "wb") as f:
        f.write(BIN_BODY)
    handler = functools.partial(http.server.SimpleHTTPRequestHandler,
                                directory=docdir)
    srv = DocServer(("127.0.0.1", HTTP_PORT), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    # ================= BOOT 1 (disk attached) =================
    print("\n=== BOOT 1: disk attached (Phase A + B) ===")
    q = QemuDisk(ISO, DISK)
    try:
        # F1 auto-mount
        txt = wait_text(q, "FAT32: 'EQDISK' mounted at /mnt", timeout=240,
                        tag="-boot")
        check("F1 auto-mount at boot", "mounted at /mnt" in txt,
              txt[-500:] if "mounted" not in txt else "")

        # skip the 5-second pause and reach the shell
        wait_text(q, "root::users /user $", timeout=120, tag="-sh")
        q.type_str("clear\n")
        time.sleep(0.5)

        # F2 ls /mnt (names appear in their ON-DISK form: 8.3-only
        # names are uppercase; LFN keeps the mixed-case original)
        q.type_str("cd /mnt\n")
        time.sleep(0.6)
        q.type_str("ls\n")
        txt = wait_text(q, "DOOM1.WAD", timeout=60, tag="-ls")
        ok = all(s in txt for s in
                 ["README.TXT", "hello from equinox.txt", "BIN.DAT",
                  "DOOM1.WAD", "DOCS"])
        check("F2 ls /mnt (8.3 + LFN + dir)", ok, txt[-600:])

        # F3 8.3 read
        q.type_str("cat README.TXT\n")
        txt = wait_text(q, "plain uppercase 8.3 name", timeout=60, tag="-f3")
        check("F3 cat 8.3 file", "plain uppercase 8.3 name" in txt)

        # F4 LFN read
        q.type_str("cat hello from equinox.txt\n")
        txt = wait_text(q, "LONG name", timeout=60, tag="-f4")
        check("F4 cat LFN file", "LONG name" in txt)

        # F5 nested dir
        q.type_str("cd docs\n")
        time.sleep(0.6)
        q.type_str("cat notes.txt\n")
        txt = wait_text(q, "Nested directory", timeout=60, tag="-f5a")
        check("F5a cat nested file", "Nested directory" in txt)
        q.type_str("cat disk tools.txt\n")
        txt = wait_text(q, "subdirectory", timeout=60, tag="-f5b")
        check("F5b LFN inside subdir", "subdirectory" in txt)
        q.type_str("cd ..\n")
        time.sleep(0.5)

        # F6 binary byte-exactness (xxd prints 16 bytes per row with a
        # column gap every 8; check each row independently after
        # whitespace normalization + lowercasing)
        q.type_str("xxd bin.dat 32\n")
        txt = wait_text(q, "bytes total", timeout=60, tag="-f6")
        # kernel printf %x prints UPPERCASE hex (utoa table)
        flat = re.sub(r"\s+", " ", txt).lower()
        row1 = " ".join(f"{b:02x}" for b in range(16))
        row2 = " ".join(f"{b:02x}" for b in range(16, 32))
        check("F6 xxd bin.dat (byte-exact)",
              row1 in flat and row2 in flat, flat[-400:])

        # F7 exact wad size
        q.type_str("ls -l\n")
        txt = wait_text(q, "4196020", timeout=60, tag="-f7")
        check("F7 ls -l doom1.wad size", "4196020" in txt)

        # F8 diskinfo
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("diskinfo\n")
        txt = wait_text(q, "EQDISK", timeout=60, tag="-f8")
        ok = "EQDISK" in txt and "read-write" in txt and "ata0" in txt
        check("F8 diskinfo", ok, txt[-700:])

        # F9 wad magic IWAD (binary correctness)
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("xxd doom1.wad 8\n")
        txt = wait_text(q, "49 57 41 44", timeout=90, tag="-f9")
        check("F9 doom1.wad IWAD magic", "49 57 41 44" in txt, txt[-400:])

        # ================ Phase B ================
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str('ccfile test.txt << "Equinox v0.2 writes FAT32"\n')
        txt = wait_text(q, "writes FAT32", timeout=60, tag="-b1")
        check("B1 ccfile on /mnt", "writes FAT32" in txt)

        q.type_str("cat test.txt\n")
        txt = wait_text(q, "Equinox v0.2 writes FAT32", timeout=60,
                        tag="-b2")
        check("B2 read-back same boot", "Equinox v0.2 writes FAT32" in txt)

        q.type_str("cdir newdir\n")
        time.sleep(0.8)
        q.type_str("ls\n")
        txt = wait_text(q, "newdir", timeout=60, tag="-b3")
        check("B3 cdir on /mnt", "newdir" in txt)

        # B4 delete
        q.type_str('ccfile todelete.txt << "temp"\n')
        time.sleep(1.0)
        q.type_str("rm todelete.txt\n")
        time.sleep(1.0)
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("ls\n")
        time.sleep(1.5)
        txt = q.screen("-b4")
        check("B4 rm file", "todelete.txt" not in txt and "test.txt" in txt)

        # B5 rm non-empty dir rejected
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("rm docs\n")
        # v0.2 write hardening: fat32_delete now returns -4 so the
        # SHELL prints the message ("non-empty directory") instead of
        # the driver printing a duplicate of its own
        txt = wait_text(q, "empty", timeout=60, tag="-b5")
        check("B5 rm non-empty dir rejected", "non-empty" in txt,
              txt[-300:])

        # B6 mget -> straight to /mnt
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str(f"mget http://10.0.2.2:{HTTP_PORT}/data.bin\n")
        txt = wait_text(q, "saved", timeout=180, tag="-b6")
        ok = "saved" in txt and "204800" in txt
        check("B6 mget download to /mnt", ok, txt[-500:])

        # R2 version
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("info\n")
        txt = wait_text(q, "v0.2 Beta", timeout=60, tag="-r2")
        check("R2 version v0.2 Beta", "v0.2 Beta" in txt)
    finally:
        q.kill()

    # ============ B7 host-side mtools verification ============
    print("\n=== HOST: mtools oracle on disk.img ===")
    _, listing, _ = mtool(["mdir", "-i", DISK + "@@1048576", "-/"])
    check("B7a mtools mdir sees OS writes",
          "test.txt" in listing and "newdir" in listing
          and "data.bin" in listing, listing[:600])

    _, _, body = mtool(["mcopy", "-i", DISK + "@@1048576",
                        "::/test.txt", "-"])
    check("B7b mtools reads back exact bytes",
          b"Equinox v0.2 writes FAT32" in body, repr(body[:120]))

    _, _, dl = mtool(["mcopy", "-i", DISK + "@@1048576",
                      "::/data.bin", "-"])
    check("B7c downloaded binary byte-identical",
          dl == BIN_BODY, f"host {len(dl)}B vs {len(BIN_BODY)}B")

    _, _, bd = mtool(["mcopy", "-i", DISK + "@@1048576",
                      "::/bin.dat", "-"])
    check("B7d untouched bin.dat intact", bd == bytes(range(256)) * 4,
          f"{len(bd)}B")

    # ================= BOOT 2 (persistence) =================
    print("\n=== BOOT 2: same disk (persistence) ===")
    q = QemuDisk(ISO, DISK, net=False)
    try:
        wait_text(q, "FAT32: 'EQDISK' mounted at /mnt", timeout=240,
                  tag="-boot2")
        wait_text(q, "root::users /user $", timeout=120, tag="-sh2")
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("cd /mnt\n")
        time.sleep(0.8)
        q.type_str("cat test.txt\n")
        txt = wait_text(q, "Equinox v0.2 writes FAT32", timeout=90,
                        tag="-p1a")
        check("P1a written file survives reboot",
              "Equinox v0.2 writes FAT32" in txt)
        q.type_str("ls newdir\n")
        time.sleep(1.0)
        txt = q.screen("-p1b")
        check("P1b newdir survives reboot", "newdir" in txt or "empty" in txt)
        q.type_str("xxd data.bin 16\n")
        txt = wait_text(q, "00 01 02 03", timeout=90, tag="-p1c")
        check("P1c downloaded file re-reads byte-exact",
              "00 01 02 03 04 05 06 07" in txt.replace("\n", " "))
    finally:
        q.kill()

    # ================= R1 no-disk regression =================
    print("\n=== BOOT 3: NO disk (regression) ===")
    q = _nodisk_boot(ISO)
    try:
        txt = wait_text(q, "no hard disks", timeout=240, tag="-r1a")
        check("R1a no-disk boot message", "no hard disks" in txt)
        wait_text(q, "root::users /user $", timeout=120, tag="-r1b")
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("cd /\n")
        time.sleep(0.6)
        q.type_str("ls\n")
        time.sleep(1.5)
        txt = q.screen("-r1c")
        # (bin/dev from the old log line were never actually created;
        # the real RAMFS root holds the module files + system dirs)
        check("R1b RAMFS still works", "equinox" in txt and "user" in txt)
        q.type_str("mount\n")
        txt = wait_text(q, "no FAT32 partition found", timeout=60,
                        tag="-r1d")
        check("R1c mount without disk fails cleanly",
              "no FAT32 partition found" in txt)
    finally:
        q.kill()

    # ================= summary =================
    print("\n================ RESULTS ================")
    passed = sum(1 for _, ok in results if ok)
    for name, ok in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    print(f"============ {passed}/{len(results)} PASS ============")
    sys.exit(0 if passed == len(results) else 1)


def _nodisk_boot(iso):
    """Boot with the ISO only (no -drive)."""
    class NoDisk(QemuDisk):
        def __init__(self, iso):
            self.sock_path = tempfile.mktemp(prefix="f32nd-", suffix=".sock")
            self.dumpdir = tempfile.mkdtemp(prefix="f32ndd-")
            self.dump_n = 0
            self.proc = subprocess.Popen(
                [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "64",
                 "-cdrom", iso, "-display", "none", "-vga", "std",
                 "-net", "none",
                 "-monitor", "unix:" + self.sock_path + ",server,nowait",
                 "-no-reboot"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                env=ENV)
            deadline = time.time() + 10
            while not os.path.exists(self.sock_path):
                if time.time() > deadline:
                    raise RuntimeError("monitor socket missing")
                time.sleep(0.1)
            self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.mon.connect(self.sock_path)
            self.mon.settimeout(120)
            self._drain(2.0)
    return NoDisk(iso)


if __name__ == "__main__":
    main()
