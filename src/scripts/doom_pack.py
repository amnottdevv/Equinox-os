#!/usr/bin/env python3
"""
doom_pack.py — build dist/equinox/games/doom.mrp for Equinox OS.

Compiles the doomgeneric core (ozkl/doomgeneric, C) + the Equinox OS port
(mrp_user/doom/doomgeneric_morphos.c + morphos_libc.c) into a single
.mrp user program, then packs it with the standard mrp_pack header.

Pipeline (mirrors mrp_user/mrp_pack.py but multi-file):
  1. gcc -m32 -std=gnu99 -O2 -ffreestanding -nostdinc ...
     each core .c  (CMAP256 + 320x200: DG_ScreenBuffer is 8bpp, the
     kernel SYS_BLIT scales + converts; DOOMGENERIC_RESX/Y are forced)
  2. link all .o with link_mrp.ld at 0x500010 + 32-bit libgcc.a
  3. verify _start at image offset 0 (nm + objdump, like mrp_pack)
  4. objcopy -O binary, zero-pad .bss, prepend the 18-byte .mrp header
     (reuses mrp_pack.pack/pad_bss so header/checksum stay in sync)

Excluded sources: other platform ports (sdl/xlib/win/allegro/emscripten/
linuxvt/soso/sosox), i_main.c (we provide _start), sound backends
(i_sdl*/i_allegro*), and files with no remaining references in this
configuration (i_scale.c — CMAP256 uses a 1:1 memcpy; mus2mid.c/memio.c
— GUS music path; i_cdmus.c — CD audio stubs; dummy.c).

Usage: python3 scripts/doom_pack.py [output.mrp]
Env:   DOOM_CC (default gcc), DOOM_CXX (default g++), DOOM_KEEP=1 keep objs
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
DG_SRC = os.path.join(ROOT, "doomgeneric", "doomgeneric")
PORT_DIR = os.path.join(ROOT, "mrp_user", "doom")
COMPAT = os.path.join(PORT_DIR, "compat")
OUT_DEFAULT = os.path.join(ROOT, "dist", "equinox", "games", "doom.mrp")

sys.path.insert(0, os.path.join(ROOT, "mrp_user"))

# v10.9 FIX (DOOM .bss bug): mrp_pack derives its nm tool from
# CROSS_OBJCOPY (default i686-elf-objcopy -> i686-elf-nm) which does
# not exist in host builds — pad_bss() then SILENTLY SKIPS zero-padding
# .bss, shipping programs whose statics are arena garbage (DOOM:
# numlumps garbage -> giant calloc -> "Couldn't realloc lumpinfo").
# Force the host tools before importing mrp_pack.
os.environ.setdefault("MRP_NM", "nm")
os.environ.setdefault("MRP_OBJCOPY", "objcopy")

import mrp_pack  # noqa: E402  (pack/pad_bss/verify_entry_at_zero reuse)

CC = os.environ.get("DOOM_CC", "gcc")
CXX = os.environ.get("DOOM_CXX", "g++")
OBJCOPY = os.environ.get("DOOM_OBJCOPY", "objcopy")
NM = os.environ.get("DOOM_NM", "nm")
LIBGCC32 = os.environ.get(
    "DOOM_LIBGCC32",
    os.path.expanduser("~/tools/root/usr/lib/gcc/i686-linux-gnu/14/libgcc.a"))

EXCLUDE = {
    # other platform ports + their entry points
    "doomgeneric_sdl.c", "doomgeneric_xlib.c", "doomgeneric_win.c",
    "doomgeneric_allegro.c", "doomgeneric_emscripten.c",
    "doomgeneric_linuxvt.c", "doomgeneric_soso.c", "doomgeneric_sosox.c",
    "i_main.c",                       # we ship our own _start
    # sound backends (FEATURE_SOUND is not defined -> stub driver in i_sound.c)
    "i_sdlsound.c", "i_sdlmusic.c", "i_allegrosound.c", "i_allegromusic.c",
    # unreferenced in the CMAP256/no-sound configuration
    "i_scale.c", "mus2mid.c", "memio.c", "i_cdmus.c",
}

PORT_SOURCES = [
    os.path.join(PORT_DIR, "morphos_libc.c"),
    os.path.join(PORT_DIR, "doomgeneric_morphos.c"),
]

# Assembled separately (no -std/-nostdinc flags needed for pure asm).
PORT_ASM = [os.path.join(PORT_DIR, "doom_start.S")]


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit(f"[doom] command failed ({r.returncode}): {' '.join(cmd)}")


def gcc_include_dir():
    d = subprocess.run([CC, "-print-file-name=include"],
                       capture_output=True, text=True).stdout.strip()
    if not os.path.isdir(d):
        sys.exit(f"[doom] cannot locate gcc builtin include dir: {d}")
    return d


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else OUT_DEFAULT
    os.makedirs(os.path.dirname(out), exist_ok=True)

    gcc_inc = gcc_include_dir()
    cflags = [
        CC, "-m32", "-std=gnu99", "-O3", "-march=i686",
        "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-strict-aliasing",
        "-fno-pic", "-fno-pie", "-fno-asynchronous-unwind-tables",
        "-fcf-protection=none",
        "-nostdinc",
        "-isystem", gcc_inc,
        "-I", COMPAT, "-I", DG_SRC, "-I", PORT_DIR,
        "-DDOOMGENERIC_RESX=320", "-DDOOMGENERIC_RESY=200",
        "-DCMAP256",
        "-c",
    ]

    core = sorted(f for f in os.listdir(DG_SRC)
                  if f.endswith(".c") and f not in EXCLUDE)
    if len(core) < 30:
        sys.exit(f"[doom] suspiciously few core sources found: {core}")

    keep = os.environ.get("DOOM_KEEP") == "1"
    work = "doom_build_tmp" if keep else tempfile.mkdtemp(prefix="doom-")
    os.makedirs(work, exist_ok=True)

    objs = []
    for i, asm in enumerate(PORT_ASM):
        obj = os.path.join(work, f"asm-{i:02d}-{os.path.basename(asm)}.o")
        print(f"[doom] (asm) {os.path.basename(asm)}")
        run([CC, "-m32", "-c", asm, "-o", obj])
        objs.append(obj)

    all_srcs = [os.path.join(DG_SRC, f) for f in core] + PORT_SOURCES
    total = len(all_srcs)
    for i, src in enumerate(all_srcs):
        obj = os.path.join(work, f"{i:03d}-{os.path.basename(src)}.o")
        print(f"[doom] ({i+1}/{total}) {os.path.basename(src)}")
        run(cflags + [src, "-o", obj])
        objs.append(obj)

    elf = os.path.join(work, "doom.elf")
    print("[doom] linking ...")
    run([CXX, "-m32", "-nostdlib", "-static", "-fno-pie", "-no-pie",
         "-T", os.path.join(ROOT, "mrp_user", "link_mrp.ld"),
         "-o", elf] + objs + [LIBGCC32])

    # entry sanity: _start must sit at the image base (offset 0)
    mrp_pack.verify_entry_at_zero(elf)
    nm_out = subprocess.run([NM, elf], capture_output=True, text=True).stdout
    start_vma = None
    for line in nm_out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == "_start":
            start_vma = int(parts[0], 16)
            break
    if start_vma != 0x500010:
        sys.exit(f"[doom] FATAL: _start at 0x{start_vma:x}, expected "
                 f"0x500010 (MRP_LOAD_BASE). Entry would jump to garbage.")
    print(f"[doom] entry OK: _start at 0x{start_vma:x} (image offset 0)")

    bin_path = os.path.join(work, "doom.bin")
    run([OBJCOPY, "-O", "binary", elf, bin_path])
    pad = mrp_pack.pad_bss(elf, bin_path)
    # hard-verify: .bss MUST be covered by the image (see env fix note)
    bss_end = None
    for line in subprocess.run([NM, elf], capture_output=True,
                               text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == "_mrp_bss_end":
            bss_end = int(parts[0], 16)
    if bss_end is not None:
        need = bss_end - 0x500010
        have = os.path.getsize(bin_path)
        if have < need:
            sys.exit(f"[doom] FATAL: image {have} bytes < .bss end "
                     f"(needs {need}) — statics would be arena garbage")
        print(f"[doom] image covers .bss: {have} >= {need} bytes OK")
    mrp_pack.pack(bin_path, out)

    if not keep:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
    print(f"[doom] DONE -> {out}")


if __name__ == "__main__":
    main()
