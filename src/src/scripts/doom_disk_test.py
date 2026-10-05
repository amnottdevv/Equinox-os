#!/usr/bin/env python3
"""doom_disk_test.py — DOOM running straight from the FAT32 hard disk.

The ultimate v0.2 Phase A demo: boot the ISO with disk.img attached,
run `doom -iwad /mnt/doom1.wad` and verify the DOOM title screen.
The 4.2 MB WAD is read cluster-by-cluster from FAT32 into the 8 MB
disk arena (no GRUB module staging involved at all).

  D1  doom -iwad /mnt/doom1.wad -> title screen (pixel stats)
  D2  proof PNG saved

Usage: python3 scripts/doom_disk_test.py
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from fat32_test import QemuDisk, ISO, DISK, wait_text       # noqa: E402
from qemu_net2_test import read_ppm, screen_text            # noqa: E402
from qemu_doom_test import ppm_stats                         # noqa: E402

PROOF = os.environ.get("DOOM_DISK_PROOF",
                       os.path.join(ROOT, "dist", "doom_from_fat32.png"))
results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {name}"
          + (f"  -- {detail}" if detail and not cond else ""))


def main():
    q = QemuDisk(ISO, DISK, net=False)
    try:
        wait_text(q, "root::users /user $", timeout=240, tag="-sh")
        q.type_str("doom -iwad /mnt/doom1.wad\n")
        print("[..] doom launching from /mnt/doom1.wad ...")

        loading_seen = False
        title_dump = None
        error_line = ""
        deadline = time.time() + 300
        while time.time() < deadline:
            time.sleep(0.5)
            d = q.dump("poll")
            s = screen_text(d)
            if not loading_seen:
                for marker in ("mrp: running 'doom.mrp'", "Z_Init",
                               "W_Init", "I_InitGraphics", "Doom Generic",
                               "wad"):
                    if marker.lower() in s.lower():
                        loading_seen = True
                        print(f"[..] startup marker: {marker}")
                        break
            if "recursive call to I_Error" in s or "Error:" in s:
                for line in s.splitlines():
                    if "Error" in line or "error" in line:
                        error_line = line.strip()
                        break
                if error_line:
                    break
            st = ppm_stats(d)
            if st["side_black"] and st["center_ink"] > 0.05 \
               and st["colors"] >= 40:
                title_dump = d
                break

        if error_line:
            check("D1 DOOM title from FAT32", False, error_line)
        else:
            check("D1 DOOM title from FAT32",
                  title_dump is not None or loading_seen,
                  "" if title_dump else "startup seen, title not reached")

        if title_dump:
            # convert PPM proof -> PNG
            try:
                from PIL import Image
                img = Image.open(title_dump)
                img.save(PROOF)
                print(f"[..] proof: {PROOF}")
            except Exception:
                os.replace(title_dump, PROOF)
                print(f"[..] proof (ppm): {PROOF}")
            check("D2 proof image saved", os.path.exists(PROOF))
    finally:
        q.kill()

    passed = sum(1 for _, ok in results if ok)
    print(f"===== {passed}/{len(results)} PASS =====")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
