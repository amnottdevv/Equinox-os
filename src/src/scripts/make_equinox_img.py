#!/usr/bin/env python3
"""make_equinox_img.py — build the equinox OS INSTALL disk image.

Creates dist/equinox.img: a 128 MB IDE disk image with an MBR partition
table (one type-0x0C FAT32 LBA partition at LBA 2048) holding a FAT32
volume with the label **EQUINOXBASE**.

Why the label matters: at boot the kernel mounts the first FAT32
partition it finds.  A volume labelled EQUINOXBASE is promoted to the
ROOT filesystem (root->backing = 1) instead of being mounted at /mnt,
so every path the shell uses (/user, /equinox/tools, ...) already
points AT THE DISK — write-through, persistent, and boot modules that
are already installed are skipped instead of rewritten (see
mrp_bootloader_load_modules()).

The volume mirrors the RAMFS layout produced by the GRUB-module path:

    /boot/kernel.elf        kernel (for a later GRUB-in-MBR boot)
    /equinox/tools/*        .mrp + .c + .elf  (equinoxinstall sources)
    /equinox/games/*        .mrp + .c + .wad
    /equinox/libc/*         the 12 guarded modules + morph.h
    /test/*.c               mtcc sample sources (flat dist/*.c)
    /user/                  user home (empty, created by the OS too)
    /*.mrp, /*.wad          flat dist/ modules -> RAMFS root
    README.TXT              what this image is

Everything is built with mtools (no root needed); the MBR is written
by hand with struct — 16 bytes at offset 446 (same as
scripts/make_fat32_img.py).

Usage:
  python3 scripts/make_equinox_img.py [--size-mb 128] [--out dist/equinox.img]
  make img && make run-img     # boot the ISO with this disk attached
"""
import glob
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")
MTOOLS_BIN = os.path.join(os.path.expanduser("~"), "tools/root/usr/bin")

PART_START_LBA = 2048          # 1 MB offset (classic alignment)
SECTOR = 512
LABEL = "EQUINOXBASE"          # <- FAT32 driver promotes this to /


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"[mtools] {' '.join(cmd)}\n{r.stdout}{r.stderr}")
        sys.exit(1)


def mtool(args, img):
    env = dict(os.environ)
    env["PATH"] = MTOOLS_BIN + os.pathsep + env.get("PATH", "")
    return subprocess.run(args + ["-i", img], capture_output=True,
                          text=True, env=env)


def main():
    size_mb = 128
    out = os.path.join(DIST, "equinox.img")
    args = sys.argv[1:]
    while args:
        a = args.pop(0)
        if a == "--size-mb":
            size_mb = int(args.pop(0))
        elif a == "--out":
            out = args.pop(0)
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0

    # ---- what goes in (the same globs the makefile feeds GRUB) ----
    flat_mrp = sorted(glob.glob(os.path.join(DIST, "*.mrp")))
    flat_c = sorted(glob.glob(os.path.join(DIST, "*.c")))
    flat_wad = sorted(glob.glob(os.path.join(DIST, "*.wad")))
    tools = sorted(glob.glob(os.path.join(DIST, "equinox", "tools", "*")))
    games = sorted(glob.glob(os.path.join(DIST, "equinox", "games", "*")))
    libc = sorted(glob.glob(os.path.join(DIST, "equinox", "libc", "*")))
    kernel = os.path.join(DIST, "kernel.elf")

    if not os.path.exists(kernel):
        print(f"[img] {kernel} belum ada — jalankan `make` dulu")
        return 1

    os.makedirs(os.path.dirname(out), exist_ok=True)
    fs_img = out + ".fs"

    # ---- 1. FAT32 filesystem in a plain file ------------------
    print(f"[img] creating {size_mb} MB FAT32 volume '{LABEL}' (mformat)...")
    with open(fs_img, "wb") as f:
        f.truncate((size_mb - 1) * 1024 * 1024)   # room for the MBR pad
    r = mtool(["mformat", "-F", "-v", LABEL, "::"], fs_img)
    if r.returncode != 0:
        print(f"[mformat] {r.stdout}{r.stderr}")
        return 1

    # ---- 1b. /boot/kernel.elf: STRIPPED copy -----------------
    # v0.4: the ISO ships the kernel as a GRUB module too
    # (boot/grub/grub.cfg: `module /boot/kernel-min.elf boot/kernel.elf`,
    # an objcopy --strip-debug image). mrp_bootloader SKIPS a module whose
    # destination already has the SAME SIZE — so the volume must carry the
    # stripped image as well, otherwise every boot of a base volume would
    # rewrite kernel.elf through FAT32 and the pristine-image hash test
    # (base_img_test T13: boot must stay read-only) would break.
    kern_out = kernel
    if os.path.exists(kernel):
        stripped = out + ".kern"
        rr = subprocess.run(["objcopy", "--strip-debug", kernel, stripped],
                            capture_output=True, text=True)
        if rr.returncode == 0 and os.path.getsize(stripped) > 0:
            kern_out = stripped
            print(f"[img] /boot/kernel.elf = stripped "
                  f"({os.path.getsize(kern_out)} B)")
        elif os.path.exists(stripped):
            os.unlink(stripped)

    # mformat puts small volumes' sector count in tot16 with tot32 = 0;
    # the FAT32 spec requires tot16 = 0 / count in tot32 (same fix as
    # make_fat32_img.py — the Equinox driver rejects the other encoding).
    with open(fs_img, "r+b") as f:
        f.seek(0)
        bpb = bytearray(f.read(512))
        tot16 = struct.unpack_from("<H", bpb, 19)[0]
        tot32 = struct.unpack_from("<I", bpb, 32)[0]
        if tot32 == 0 and tot16 != 0:
            struct.pack_into("<I", bpb, 32, tot16)
            struct.pack_into("<H", bpb, 19, 0)
            f.seek(0)
            f.write(bpb)
            print(f"[img] BPB normalized: tot16={tot16} -> tot32")

    # ---- 2. directory tree ------------------------------------
    # (created parent-first; mtools has no mkdir -p)
    dirs = ["boot", "user", "test",
            "equinox", "equinox/tools", "equinox/games", "equinox/libc"]
    for d in dirs:
        r = mtool(["mmd", "::/" + d], fs_img)
        if r.returncode != 0 and "Already exists" not in (r.stdout + r.stderr):
            print(f"[mmd {d}] {r.stdout}{r.stderr}")
            return 1

    def put_file(src, dest):
        r = mtool(["mcopy", src, "::/" + dest], fs_img)
        if r.returncode != 0:
            print(f"[mcopy {dest}] {r.stdout}{r.stderr}")
            return 1
        return 0

    def put_text(dest, text):
        tmp = fs_img + ".txt"
        with open(tmp, "w") as f:
            f.write(text)
        rc = put_file(tmp, dest)
        os.unlink(tmp)
        return rc

    def count_items(files):
        n = 0
        for p in files:
            n += put_file(p, os.path.relpath(p, DIST).replace(os.sep, "/"))
        return n

    print("[img] filling volume...")
    bad = 0
    bad += put_file(kern_out, "boot/kernel.elf")
    bad += count_items(flat_mrp)       # -> /  (RAMFS root equivalent)
    bad += count_items(flat_wad)       # -> /
    bad += count_items(tools)          # -> /equinox/tools
    bad += count_items(games)          # -> /equinox/games
    bad += count_items(libc)           # -> /equinox/libc

    # flat dist/*.c are mtcc samples: mrp_bootloader routes them to
    # RAMFS /test, so they belong under /test/ here (NOT at the root).
    for p in flat_c:
        bad += put_file(p, "test/" + os.path.basename(p))

    bad += put_text("README.TXT",
                    "equinox OS INSTALL image (EQUINOXBASE)\r\n"
                    "=====================================\r\n"
                    "\r\n"
                    "FAT32 volume, partition type 0x0C at LBA 2048.\r\n"
                    "Label EQUINOXBASE = the kernel mounts this volume as\r\n"
                    "the ROOT filesystem (/user, /equinox, /test, ...),\r\n"
                    "write-through, so builds and files survive a reboot.\r\n"
                    "\r\n"
                    "Boot it:\r\n"
                    "  make run-img\r\n"
                    "or from the shell: equinoxinstall\r\n")
    if bad:
        print(f"[img] {bad} file(s) gagal disalin")
        return 1

    # ---- 3. assemble disk.img: MBR + pad + filesystem ----------
    print("[img] assembling MBR + partition table...")
    fs_size = os.path.getsize(fs_img)
    part_secs = fs_size // SECTOR
    with open(fs_img, "rb") as f:
        fs_data = f.read()
    with open(out, "wb") as f:
        f.write(b"\x00" * (PART_START_LBA * SECTOR))   # pad + MBR area
        f.write(fs_data)

    # CHS fields are legacy 3-byte values (H, S|C-hi, C-lo): LBA-only
    # entries use the max-1023 trick. Total entry = 16 bytes.
    chs_max = bytes([0xFE, 0xFF, 0xFF])
    entry = struct.pack("<B", 0x00) + chs_max      # NOT bootable: SeaBIOS must boot the CD
    entry += struct.pack("<B", 0x0C) + chs_max     # type FAT32 LBA + CHS end
    entry += struct.pack("<II", PART_START_LBA, part_secs)
    assert len(entry) == 16, f"MBR entry must be 16 bytes, got {len(entry)}"
    mbr = bytearray(b"\x00" * 512)
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    # This is a DATA disk: the signature must stay (the kernel checks it
    # before parsing the partition table) but there is no boot code.
    # SeaBIOS jumps to offset 0 whenever 0x55AA is present, so a zeroed
    # area would loop forever and the CD would never boot. INT 18h is the
    # classic "no boot device here -> try the next one" signal.
    mbr[0:2] = b"\xCD\x18"
    with open(out, "r+b") as f:
        f.seek(0)
        f.write(bytes(mbr))

    if kern_out != kernel and os.path.exists(kern_out):
        os.unlink(kern_out)
    os.unlink(fs_img)
    total = os.path.getsize(out)
    print(f"[img] wrote {out} ({total/1024/1024:.1f} MB, "
          f"partition {part_secs} sectors at LBA {PART_START_LBA}, "
          f"label {LABEL})")
    print(f"[img] run: qemu-system-i386 -m 64 -boot order=d "
          f"-cdrom dist/equinox.iso "
          f"-drive file={out},format=raw,if=ide,index=0,media=disk")
    return 0


if __name__ == "__main__":
    sys.exit(main())
