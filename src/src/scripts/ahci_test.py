#!/usr/bin/env python3
"""ahci_test.py — v0.5 block layer + AHCI SATA driver suite (17 checks).

Machine (the disk hangs off an AHCI controller, NOT the PIIX IDE bus):

    -cdrom dist/equinox.iso                PIIX IDE — SeaBIOS boots it
    -boot order=d                          ...and only it
    -device ahci,id=ahci                   ich9-ahci 8086:2922, class
                                           01:06, BAR5 = ABAR (4 KB)
    -drive file=<img>,format=raw,if=none,id=hd0
    -device ide-hd,drive=hd0,bus=ahci.0    port 0 -> blk slot 4
                                           -> `hd'e'`

So every sector the FAT32 driver moves goes:

    fs_fat32.cpp -> blk_read/blk_write -> ahci.cpp -> PxCI poll
                    (slot 4)              (command slot 0, one PRDT)

The image is built by THIS script (scripts/make_fat32_img.py) because
dist/disk.img is a shared artifact that may have been relabelled
EQUINOXBASE — that would promote the volume to / instead of mounting
it at /mnt.  The default label here is EQDISK.

Checks
  A1  boot to shell
  A2  VESA boot log: `SATA: ahci port`         (printed BEFORE
  A3  VESA boot log: `FAT32: 'EQDISK' mounted   serial_init(), so it
      at /mnt`                                 only exists on screen —
                                               polled like fat32_test F1)
  A4  lspci lists the controller (8086:2922, class 0106, BAR5 MEM)
  A5  diskinfo: ahci0 / ahci port 0 / SATA LBA48 / EQDISK on ahci0
  A6  FAT sector-cache hits climb over a multi-cluster FAT walk
      (must run FIRST: xxd memoises fat_chain_length)
  A7  cat README.TXT byte-exact
  A8  ls /mnt matches the volume contents
  A9  multi-cluster read: xxd doom1.wad 8 -> IWAD + 4196020 bytes
  A10 byte-exact small read: xxd bin.dat 32
  A11 save + read back
  A12 overwrite (save again) + read back
  A13 rm + ls no longer lists it
  A14 umount -> mount -> README still byte-exact
  A15 no I/O errors anywhere on the serial console
  A16 QEMU still alive at the end of phase 1
  A17 phase 2 (reboot, same image): the written file is still there
      and diskinfo still reports ahci0
"""
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import boot_test_v032 as b32                          # noqa: E402
from boot_test_v032 import (Qemu, ISO, check, PASS, FAIL,  # noqa: E402
                            serial, since, wait_serial)
from qemu_net2_test import screen_text                # noqa: E402

MKFS = os.path.join(HERE, "make_fat32_img.py")

# Own serial log so this suite never races another one on
# /tmp/boot_v032_serial.log.
SERIAL = "/tmp/boot_ahci_serial.log"
b32.SERIAL = SERIAL

README = ("Equinox OS v0.2 Beta FAT32 disk\n"
          "\n"
          "This file has a plain uppercase 8.3 name (no LFN entries).\n"
          "\n"
          "Read by the Equinox FAT32 driver (Phase A).\n")
LS_EXPECT = ["DOOM1.WAD", "BIN.DAT", "DOCS",
             "hello from equinox.txt", "README.TXT"]
PERSIST_NAME = "ahci2.txt"
PERSIST_TEXT = "AHCI persistence probe"
IO_ERRORS = ["FAT32: write failed", "FAT32: read failed",
             "cache flush failed", "IDENTIFY failed", "will not start",
             "save: failed", "ccfile: failed", "disk I/O error",
             "Kernel panic"]


def ahci_args(img):
    return ["-boot", "order=d",
            "-device", "ahci,id=ahci",
            "-drive", f"file={img},format=raw,if=none,id=hd0",
            "-device", "ide-hd,drive=hd0,bus=ahci.0"]


def norm(s):
    """The serial mirror emits CRLF; compare against plain LF."""
    return s.replace("\r\n", "\n").replace("\r", "\n")


def sh(q, line, wait=2.0, limit=9000):
    """Type one shell command and return the serial output it produced."""
    base = len(serial())
    q.type_line(line, wait=wait)
    time.sleep(0.6)
    return norm(since(base, limit))


def build_image():
    path = tempfile.mktemp(prefix="equix-ahci-", suffix=".img")
    r = subprocess.run([sys.executable, MKFS, "--size-mb", "64",
                        "--out", path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("[ahci] gagal membangun citra FAT32:\n" + r.stdout + r.stderr)
        return None
    return path


def boot(img):
    Qemu.EXTRA = ahci_args(img)
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    return Qemu(ISO)


def boot_watch(q, timeout=200):
    """Wait for the shell while also reading the VESA boot log.

    `SATA: ahci port ...` and `FAT32: 'EQDISK' mounted at /mnt` are
    printed during fat32_boot_init(), i.e. BEFORE serial_init() — the
    serial mirror is still a no-op then, so they only ever appear on
    the framebuffer.  Once the module loader starts printing
    `[ OK ] /equinox/...` over them they are gone for good, so we
    screendump from t=0 and OCR every cycle until we have seen both
    (or the shell comes up and the window closes).  Same technique as
    fat32_test F1.
    """
    seen_sata = seen_mnt = False
    t0, n = time.time(), 0
    while time.time() - t0 < timeout:
        if not q.alive():
            return seen_sata, seen_mnt, False
        if not (seen_sata and seen_mnt):
            try:
                txt = screen_text(q.dump("a%02d" % n))
                n += 1
                if "SATA: ahci port" in txt:
                    seen_sata = True
                if "FAT32: 'EQDISK' mounted at /mnt" in txt:
                    seen_mnt = True
            except Exception:
                pass
            if seen_sata and seen_mnt:
                break
        if "root::users" in serial():
            break                       # the log is gone for good now
        time.sleep(0.3)
    shell = wait_serial("root::users", 60, rig=q)
    return seen_sata, seen_mnt, shell


def cache_hits(txt):
    m = re.search(r"cache\s*:\s*(\d+) hits / (\d+) misses", norm(txt))
    return (int(m.group(1)), int(m.group(2))) if m else (-1, -1)


def cleanup(img):
    if img and os.path.exists(img):
        try:
            os.unlink(img)
        except OSError:
            pass


def main():
    if not os.path.exists(ISO):
        print("[ahci] belum ada:", ISO, "— jalankan `make`")
        return 2
    img = build_image()
    if img is None:
        return 2

    print(f"[ahci] boot (disk {os.path.basename(img)}, label EQDISK)",
          flush=True)
    q = None
    try:
        q = boot(img)
        seen_sata, seen_mnt, shell = boot_watch(q)
        check("A1 boot ke shell (ISO + disk AHCI)", shell)
        if not shell:
            print(norm(serial())[-2500:])
            return 1

        check("A2 boot log: `SATA: ahci port`", seen_sata)
        check("A3 boot log: `FAT32: 'EQDISK' mounted at /mnt`", seen_mnt)

        # let the intro/loading screen finish before typing
        time.sleep(5.0)

        # ---------- controller visible to the OS ---------------------
        out = sh(q, "lspci", wait=3.0)
        check("A4 lspci: AHCI controller 8086:2922 class 0106 + BAR5",
              re.search(r"0x8086\s+0x2922\s+0106/00\s+mass storage controller",
                        out) is not None
              and "BAR5 MEM" in out,
              [l for l in out.split("\n") if "0x2922" in l])

        # ---------- the disk itself ----------------------------------
        out = sh(q, "diskinfo", wait=3.0)
        check("A5 diskinfo: ahci0 / ahci port 0 / SATA LBA48 / EQDISK",
              "ahci0: ahci port 0" in out
              and "SATA LBA48" in out
              and '"QEMU HARDDISK"' in out
              and "== FAT32 volume at /mnt ==" in out
              and "label      : EQDISK" in out
              and "partitions : ahci0 @ LBA 2048" in out
              and "secondary master" in out,        # full bus string
              [l for l in out.split("\n")
               if "ahci0" in l or "FAT32 volume" in l])

        sh(q, "clear", wait=1.0)
        sh(q, "cd /mnt", wait=1.5)

        # ---------- sector cache -------------------------------------
        # BEFORE anything else touches doom1.wad: xxd asks for the file
        # size first, which walks the whole 8196-cluster FAT chain
        # (fat_chain_length) — that is the big hit/miss source.  The
        # result is memoised, so this check has to be the FIRST reader.
        h1, m1 = cache_hits(sh(q, "diskinfo", wait=3.0, limit=6000))
        sh(q, "xxd doom1.wad 4096", wait=8.0, limit=3000)
        h2, m2 = cache_hits(sh(q, "diskinfo", wait=3.0, limit=6000))
        check("A6 FAT sector-cache hits climb on a multi-cluster walk",
              h1 >= 0 and h2 > h1, f"{h1} hits -> {h2} hits "
                                   f"(misses {m1} -> {m2})")

        # ---------- reads --------------------------------------------
        out = sh(q, "cat README.TXT", wait=2.5, limit=4000)
        check("A7 cat README.TXT byte-exact", README in out,
              repr(out.replace("\n", "|")[:120]))

        out = sh(q, "ls", wait=2.5, limit=4000)
        check("A8 ls /mnt matches the volume", all(s in out for s in LS_EXPECT),
              [s for s in LS_EXPECT if s not in out])

        out = sh(q, "xxd doom1.wad 8", wait=4.0, limit=4000)
        check("A9 multi-cluster file read (doom1.wad -> IWAD)",
              "49 57 41 44" in out and "|IWAD" in out
              and "4196020 bytes total, showing 8" in out,
              [l for l in out.split("\n") if "IWAD" in l][:1])

        out = sh(q, "xxd bin.dat 32", wait=3.0, limit=4000)
        check("A10 byte-exact small read (bin.dat 0x00..0x1F)",
              "0000  00 01 02 03 04 05 06 07" in out
              and "0010  10 11 12 13 14 15 16 17" in out
              and "(1024 bytes total, showing 32)" in out)

        # ---------- writes -------------------------------------------
        name1 = "ahci1.txt"
        out = sh(q, f'save {name1} << "SATA round trip"', wait=3.0,
                 limit=3000)
        out += sh(q, f"cat {name1}", wait=2.5, limit=3000)
        check("A11 save + read back over AHCI",
              "save: wrote '%s' (15 bytes)" % name1 in out
              and "SATA round trip" in out,
              [l for l in out.split("\n") if "save:" in l])

        out = sh(q, f'save {name1} << "SATA round trip v2"', wait=3.0,
                 limit=3000)
        out += sh(q, f"cat {name1}", wait=2.5, limit=3000)
        check("A12 overwrite + read back",
              "save: wrote '%s' (18 bytes)" % name1 in out
              and "SATA round trip v2" in out
              and "SATA round trip\n" not in out,
              [l for l in out.split("\n") if "save:" in l])

        ls1 = sh(q, "ls", wait=2.5, limit=4000)
        sh(q, f"rm {name1}", wait=2.5, limit=2000)
        ls2 = sh(q, "ls", wait=2.5, limit=4000)
        check("A13 rm + ls no longer lists it",
              name1 in ls1 and name1 not in ls2)

        # marker for phase 2 (kept on the disk)
        sh(q, f'save {PERSIST_NAME} << "{PERSIST_TEXT}"', wait=3.0,
           limit=3000)

        # ---------- umount / remount ---------------------------------
        out = sh(q, "umount", wait=3.0, limit=3000)
        out += sh(q, "mount", wait=3.0, limit=3000)
        out += sh(q, "cat README.TXT", wait=3.0, limit=4000)
        check("A14 umount -> mount -> README still byte-exact",
              "umount: /mnt released" in out
              and "FAT32: 'EQDISK' mounted at /mnt" in out
              and README in out)

        # ---------- health -------------------------------------------
        s = norm(serial())
        bad = [e for e in IO_ERRORS if e in s]
        check("A15 no I/O errors on the serial console", not bad, bad)
        check("A16 QEMU still alive at end of phase 1", q.alive())

        # ---------- phase 2: reboot, same disk -----------------------
        q.quit()
        q = None
        print("[ahci] phase 2: reboot with the same disk ...", flush=True)
        q = boot(img)
        shell = wait_serial("root::users", 200, rig=q)
        time.sleep(5.0)
        if shell:
            sh(q, "clear", wait=1.0)
            sh(q, "cd /mnt", wait=1.5)
            out = sh(q, f"cat {PERSIST_NAME}", wait=3.0, limit=4000)
            out += sh(q, "diskinfo", wait=3.0, limit=6000)
        else:
            out = norm(serial())[-2500:]
        check("A17 reboot: file persisted + ahci0 still mounted",
              shell and PERSIST_TEXT in out
              and "ahci0: ahci port 0" in out
              and "label      : EQDISK" in out,
              [l for l in out.split("\n")
               if "ahci0" in l or PERSIST_TEXT in l][:2])

        if not shell:
            print(norm(serial())[-2500:])

    finally:
        if q is not None:
            try:
                q.quit()
            except Exception:
                pass
        cleanup(img)

    print(f"\n[ahci] PASS {len(PASS)} / FAIL {len(FAIL)}")
    if FAIL:
        print("[ahci] gagal:")
        for n in FAIL:
            print("  -", n)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
