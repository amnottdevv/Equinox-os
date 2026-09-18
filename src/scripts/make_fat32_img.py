#!/usr/bin/env python3
"""make_fat32_img.py — build the Equinox OS FAT32 test/demo disk.

Creates dist/disk.img: a 64 MB IDE disk image with an MBR partition
table (one type-0x0C FAT32 LBA partition at LBA 2048) and a FAT32
volume inside, pre-filled with test files:

  README.TXT              plain 8.3 uppercase (no LFN entries)
  hello from equinox.txt  long file name (LFN) + 8.3 alias
  bin.dat                 1024 known bytes (0..255 x4) for xxd checks
  docs/notes.txt          nested directory
  docs/disk tools.txt     LFN inside a subdirectory
  doom1.wad               (copied when present) -> run DOOM from disk

Everything is built with mtools (no root needed). The MBR is written
by hand with struct — 16 bytes at offset 446.

Usage:  python3 scripts/make_fat32_img.py [--size-mb 64] [--out dist/disk.img]
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MTOOLS_BIN = os.path.join(os.path.expanduser("~"), "tools/root/usr/bin")

PART_START_LBA = 2048          # 1 MB offset (classic alignment)
SECTOR = 512

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"[mtools] {' '.join(cmd)}\n{r.stdout}{r.stderr}")
        sys.exit(1)

def mtool(args, img):
    env = dict(os.environ)
    env["PATH"] = MTOOLS_BIN + os.pathsep + env.get("PATH", "")
    r = subprocess.run(args + ["-i", img], capture_output=True, text=True,
                       env=env)
    return r

def main():
    size_mb = 64
    out = os.path.join(ROOT, "dist", "disk.img")
    stress = False
    args = sys.argv[1:]
    while args:
        a = args.pop(0)
        if a == "--size-mb":
            size_mb = int(args.pop(0))
        elif a == "--out":
            out = args.pop(0)
        elif a == "--stress":
            # small write-stress disk (3 MB) for the fill/recovery suite
            stress = True
            size_mb = 3
            out = os.path.join(ROOT, "dist", "stress.img")

    os.makedirs(os.path.dirname(out), exist_ok=True)
    fs_img = out + ".fs"

    # ---- 1. FAT32 filesystem in a plain file ------------------
    print(f"[disk] creating {size_mb} MB FAT32 volume (mformat)...")
    with open(fs_img, "wb") as f:
        f.truncate((size_mb - 1) * 1024 * 1024)   # room for the MBR pad
    label = "EQSTRESS" if stress else "EQDISK"
    r = mtool(["mformat", "-F", "-v", label, "::"], fs_img)
    if r.returncode != 0:
        print(f"[mformat] {r.stdout}{r.stderr}")
        sys.exit(1)

    # Small volumes (< 32 MB) get the sector count in the FAT16 field
    # (tot16) with tot32 = 0 — the FAT32 spec REQUIRES tot16 = 0 and
    # the count in tot32. Normalize to the standard encoding so both
    # the Equinox driver and every host tool see a proper FAT32.
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
            print(f"[disk] BPB normalized: tot16={tot16} -> tot32")

    # ---- 2. test files ----------------------------------------
    def put(name, data):
        src = out + ".src"
        with open(src, "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode())
        r = mtool(["mcopy", src, "::" + name], fs_img)
        if r.returncode != 0:
            print(f"[mcopy {name}] {r.stdout}{r.stderr}")
            sys.exit(1)
        os.unlink(src)

    def put_file(src_path, name):
        r = mtool(["mcopy", src_path, "::" + name], fs_img)
        if r.returncode != 0:
            print(f"[mcopy {name}] {r.stdout}{r.stderr}")
            sys.exit(1)

    print("[disk] adding test files...")
    # plain 8.3 (no LFN)
    put("README.TXT",
        "Equinox OS v0.2 Beta FAT32 disk\r\n"
        "This file has a plain uppercase 8.3 name (no LFN entries).\r\n"
        "Read by the Equinox FAT32 driver (Phase A).\r\n")
    # long name -> LFN entries + mangled 8.3 alias
    put("hello from equinox.txt",
        "Hello from Equinox OS!\r\n"
        "This file has a LONG name: it is stored with LFN entries\r\n"
        "plus a mangled 8.3 alias (HELLOF~1.TXT).\r\n")
    # nested directory + LFN inside
    mdir = mtool(["mmd", "::docs"], fs_img)
    if mdir.returncode != 0:
        print(f"[mmd] {mdir.stdout}{mdir.stderr}")
        sys.exit(1)
    put("docs/notes.txt",
        "Nested directory listing works: /mnt/docs\r\n")
    put("docs/disk tools.txt",
        "LFN inside a subdirectory: /mnt/docs/disk tools.txt\r\n")
    # binary: bytes 0..255 four times (4096... keep 1024 for speed)
    put("bin.dat", bytes(range(256)) * 4)
    # doom wad when available (run DOOM straight from the disk!)
    wad = os.path.join(ROOT, "dist", "doom1.wad")
    if os.path.exists(wad) and not stress:
        print("[disk] adding doom1.wad (run DOOM from /mnt!)")
        put_file(wad, "doom1.wad")

    # stress mode: the in-OS write-stress program + a seed file
    if stress:
        put_file(os.path.join(ROOT, "scripts", "fat32_stress.c"),
                 "fat32_stress.c")
        put("seed.txt", "seed file written by the HOST (mtools)\r\n")

    # ---- 3. assemble disk.img: MBR + pad + filesystem ----------
    print("[disk] assembling MBR + partition table...")
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
    entry = struct.pack("<B", 0x00) + chs_max          # NOT bootable: SeaBIOS must boot the CD
    entry += struct.pack("<B", 0x0C) + chs_max         # type FAT32 LBA + CHS end
    entry += struct.pack("<II", PART_START_LBA, part_secs)
    assert len(entry) == 16, f"MBR entry must be 16 bytes, got {len(entry)}"
    mbr = bytearray(f.read(512) if False else b"\x00" * 512)
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    with open(out, "r+b") as f:
        f.seek(0)
        f.write(bytes(mbr))

    os.unlink(fs_img)
    total = os.path.getsize(out)
    print(f"[disk] wrote {out} ({total/1024/1024:.1f} MB, "
          f"partition {part_secs} sectors at LBA {PART_START_LBA})")
    print(f"[disk] run: qemu-system-i386 -m 64 -cdrom dist/equinox.iso "
          f"-drive file={out},format=raw,if=ide,index=0,media=disk")

if __name__ == "__main__":
    main()
