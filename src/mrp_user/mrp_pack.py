#!/usr/bin/env python3
"""
mrp_pack.py — Compiler + packer for .mrp programs (Equinox OS Runnable Program)

Flow:
    1. Compile the .cpp source -> object file (i686-elf-g++, freestanding, -m32)
    2. Link with link_mrp.ld -> flat ELF (base address 0)
    3. objcopy -O binary -> pure raw machine code (the code from _start,
       which the linker script placed at offset 0)
    4. Wrap it with an 18-byte header (magic, version, entry_offset,
       code_size, flags, checksum) -> the final .mrp file

IMPORTANT: the header format & checksum algorithm here MUST MATCH
kernel/library/header/mrp_format.h EXACTLY. If one of them changes,
the other must change too, or every .mrp file will be rejected by the
loader (checksum/size mismatch).

Requires: the i686-elf-g++ cross compiler (or change CROSS below if your
toolchain has a different name, e.g. i686-linux-gnu-g++ with -ffreestanding).

Usage:
    python3 mrp_pack.py hello.cpp hello.mrp
    python3 mrp_pack.py hello.cpp hello.mrp --keep-temp   # for debugging
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# ---- Change these if your cross-compiler names differ ----
CROSS_CXX = os.environ.get("MRP_CXX", "i686-elf-g++")
CROSS_OBJCOPY = os.environ.get("MRP_OBJCOPY", "i686-elf-objcopy")

# Host toolchain (g++/gcc without a prefix) defaults to PIE on Debian/Ubuntu
# — the fixed-address (0x500010) link script cannot be PIE. Bare-metal
# i686-elf-* tools are non-PIE by default, so these flags are only added
# when the host compiler is used.
_IS_HOST_TOOLCHAIN = os.path.basename(CROSS_CXX) in ("g++", "gcc", "clang++", "c++")
_NO_PIE = ["-no-pie"] if _IS_HOST_TOOLCHAIN else []

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# Kernel headers (relative to mrp_user/). Useful when a .mrp program wants
# to reuse kernel types (Vector, fs_node, etc.) — not mandatory right now,
# but provided so the toolchain is ready once morphAPI v3 starts exposing
# complex types.
KERNEL_HEADER_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "kernel", "library", "header"))
# Libgame (relative to mrp_user/) — game framework header for games/*.cpp
# (v10.6). Games #include "libgame.h"; libgame.h itself #includes "Morph.h"
# which resolves via SCRIPT_DIR below.
LIBGAME_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "Libgame"))
LINKER_SCRIPT = os.path.join(SCRIPT_DIR, "link_mrp.ld")

MRP_MAGIC = b"MRP1"
MRP_VERSION = 1
MRP_FLAG_NONE = 0x00

# Header struct format, MUST match the __attribute__((packed)) struct
# mrp_header in mrp_format.h:
#   uint8_t  magic[4]
#   uint8_t  version
#   uint32_t entry_offset
#   uint32_t code_size
#   uint8_t  flags
#   uint32_t checksum
# Little-endian (i686), packed (no padding) -> total 18 byte.
HEADER_FMT = "<4sBIIBI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 18, f"Header size mismatch: {HEADER_SIZE} != 18 (check HEADER_FMT)"


def mrp_checksum(data: bytes) -> int:
    """MUST be identical to mrp_checksum() in mrp_format.h (rotate-xor)."""
    MASK32 = 0xFFFFFFFF
    checksum = 0x811C9DC5
    for byte in data:
        checksum = ((checksum << 5) | (checksum >> 27)) & MASK32
        checksum ^= byte
        checksum &= MASK32
    return checksum


def run(cmd, **kwargs):
    print("  $", " ".join(cmd))
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        sys.exit(f"[mrp_pack] Command failed (exit {result.returncode}): {' '.join(cmd)}")


def compile_and_link(src_path: str, work_dir: str) -> str:
    obj_path = os.path.join(work_dir, "prog.o")
    elf_path = os.path.join(work_dir, "prog.elf")
    bin_path = os.path.join(work_dir, "prog.bin")

    # FIX(mtcc.c): .c files are accepted as .mrp program sources.
    # The mtcc source is written in C style (no classes/templates), but it
    # is still compiled as C++ (-x c++) so it stays 100% identical to the
    # old mrp_user/mtcc.cpp build — g++ itself also treats .c as C++,
    # only with a deprecation warning; -x c++ makes it explicit and
    # warning-free.
    lang_flags = ["-x", "c++"] if src_path.endswith(".c") else []

    print(f"[mrp_pack] Compiling {src_path} ...")
    run([
        CROSS_CXX,
        "-std=gnu++17", "-m32",
        "-ffreestanding", "-fno-exceptions", "-fno-rtti",
        "-fno-pic", "-fno-pie",
        "-Wall", "-Wextra",
        "-I", SCRIPT_DIR,
        "-I", KERNEL_HEADER_DIR,
        "-I", LIBGAME_DIR,
        *lang_flags,
        "-c", src_path,
        "-o", obj_path,
    ])

    print(f"[mrp_pack] Linking with {LINKER_SCRIPT} ...")
    run([
        CROSS_CXX,
        "-m32", "-ffreestanding", "-nostdlib",
        "-fno-pie",
        "-T", LINKER_SCRIPT,
        "-o", elf_path,
        obj_path,
    ] + _NO_PIE)

    # Sanity check: make sure _start really sits at offset 0 in the linked
    # ELF (the linker script puts section .start first, but we verify so a
    # silent breakage cannot slip in if someone edits the linker script).
    verify_entry_at_zero(elf_path)

    print(f"[mrp_pack] objcopy to raw binary ...")
    run([CROSS_OBJCOPY, "-O", "binary", elf_path, bin_path])

    # FIX .bss: zero-pad the binary until it covers .bss (see pad_bss).
    pad_bss(elf_path, bin_path)

    return bin_path


def verify_entry_at_zero(elf_path: str):
    """
    Verify that _start sits at offset 0 from the start of the binary
    (after objcopy).

    Concept:
      link_mrp.ld places section `.start` (containing the _start function)
      as the VERY FIRST thing inside `.text`:
          .text : { *(.start) *(.text) *(.text.*) }
      Because `.start` is folded into `.text` (not a separate section in
      the ELF output), objdump -h will only show `.text` (not `.start`).

      But what matters: the VMA (link-time address) of symbol _start MUST
      equal the VMA of section .text. If they match, _start sits at the
      start of .text -> after objcopy -O binary, _start is at byte 0 of
      the binary file -> entry_offset=0 is valid.

    The _start VMA is expected to be 0x500010 (per MRP_LOAD_BASE in
    link_mrp.ld), the same as the .text VMA. If either differs, the linker
    script was changed without updating the packer, or the .start section
    did not land inside .text.
    """
    CROSS_OBJDUMP = os.environ.get("MRP_OBJDUMP", CROSS_OBJCOPY.replace("objcopy", "objdump"))
    nm_tool = os.environ.get("MRP_NM", CROSS_OBJCOPY.replace("objcopy", "nm"))
    # v10.9 FIX: on HOST builds (CROSS_OBJCOPY = host objcopy), deriving
    # the tool names yields plain "nm"/"objdump", which is safe; but if a
    # stale env still points at an uninstalled i686-elf-*, do NOT silently
    # skip (a skipped pad_bss = .bss statics full of arena garbage — the
    # DOOM v10.9 bug). Fall back to the host tools.
    if shutil.which(nm_tool) is None:
        nm_tool = "nm"
    if shutil.which(CROSS_OBJDUMP) is None:
        CROSS_OBJDUMP = "objdump"

    # Step 1: get the _start VMA from nm
    start_vma = None
    try:
        nm_result = subprocess.run([nm_tool, elf_path],
                                   capture_output=True, text=True)
        for line in nm_result.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == "_start":
                start_vma = int(parts[0], 16)
                break
    except FileNotFoundError:
        print("[mrp_pack] (skip _start verification: nm not found)")
        return

    if start_vma is None:
        print("[mrp_pack] WARNING: symbol _start not found via nm. "
              "Make sure you use the MRP_ENTRY macro, not a manual `void _start(...)`.")
        return

    # Step 2: get the .text VMA from objdump -h
    text_vma = None
    try:
        od_result = subprocess.run([CROSS_OBJDUMP, "-h", elf_path],
                                   capture_output=True, text=True)
    except FileNotFoundError:
        # objdump not available -- use nm alone with the expected VMA.
        expected = 0x500010
        if start_vma == expected:
            print(f"[mrp_pack] OK (nm-only): _start VMA = 0x{start_vma:x} "
                  f"(matches MRP_LOAD_BASE in link_mrp.ld) -> entry_offset=0 valid.")
        else:
            print(f"[mrp_pack] WARNING (nm-only): _start VMA = 0x{start_vma:x}, "
                  f"expected 0x{expected:x}. Check MRP_LOAD_BASE in link_mrp.ld.")
        return

    for line in od_result.stdout.splitlines():
        parts = line.split()
        # Format: Idx Name Size VMA LMA File off Algn
        if len(parts) >= 7 and parts[0].isdigit() and parts[1] == ".text":
            try:
                text_vma = int(parts[3], 16)
            except ValueError:
                pass
            break

    if text_vma is None:
        print("[mrp_pack] WARNING: section .text not found in objdump -h. "
              "Entry offset verification skipped.")
        return

    # Step 3: compare
    if start_vma == text_vma:
        print(f"[mrp_pack] OK: _start VMA (0x{start_vma:x}) == .text VMA "
              f"(0x{text_vma:x}) -> _start at the start of .text -> entry_offset=0 valid.")
    else:
        print(f"[mrp_pack] WARNING: _start VMA (0x{start_vma:x}) != .text VMA "
              f"(0x{text_vma:x}). _start may NOT be at offset 0. "
              f"Check link_mrp.ld and make sure *(.start) comes first inside .text.")


def pad_bss(elf_path: str, bin_path: str) -> int:
    """
    CRITICAL FIX: objcopy -O binary does NOT include the .bss section
    (NOBITS) — the binary stops at the end of .rodata/.data. As a result
    the program's static variables (state S, flags, tables) live in arena
    memory that is NOT zeroed — and the arena allocator places the very
    NEXT allocation's BLOCK HEADER exactly there -> statics get
    overwritten -> the program breaks in random ways (the lexer reads
    garbage pointers, the g_debug flag flips on its own, etc.).

    Solution: pad the binary with ZEROS until it covers the whole .bss
    (up to the _mrp_bss_end symbol from link_mrp.ld), so the loader
    copies those zeros and statics are always zeroed + the arena cannot
    stomp them (the image block includes .bss).

    Returns the number of padding bytes (0 if not needed / symbol absent).
    """
    nm_tool = os.environ.get("MRP_NM", CROSS_OBJCOPY.replace("objcopy", "nm"))
    if shutil.which(nm_tool) is None:
        nm_tool = "nm"   # v10.9: never skip silently (.bss statics!)
    bss_end = None
    try:
        nm_result = subprocess.run([nm_tool, elf_path],
                                   capture_output=True, text=True)
        for line in nm_result.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == "_mrp_bss_end":
                bss_end = int(parts[0], 16)
                break
    except FileNotFoundError:
        print("[mrp_pack] (skip .bss padding: nm not found)")
        return 0

    if bss_end is None:
        print("[mrp_pack] WARNING: symbol _mrp_bss_end not found — "
              ".bss NOT padded! The program may break. Check link_mrp.ld.")
        return 0

    LOAD_BASE = 0x500010          # MRP_LOAD_BASE (link_mrp.ld)
    need = bss_end - LOAD_BASE
    cur = os.path.getsize(bin_path)
    if need <= cur:
        return 0
    pad = need - cur
    with open(bin_path, "ab") as f:
        f.write(b"\x00" * pad)
    print(f"[mrp_pack] .bss padded with {pad} zero bytes (up to 0x{bss_end:x}) — "
          f"statics safe from arena block headers.")
    return pad


def pack(bin_path: str, out_path: str, entry_offset: int = 0, flags: int = MRP_FLAG_NONE):
    with open(bin_path, "rb") as f:
        code = f.read()

    if len(code) == 0:
        sys.exit("[mrp_pack] Error: binary output is empty (0 bytes). Check that "
                  "_start really got linked in (dead-stripped?)")

    if entry_offset >= len(code):
        sys.exit(f"[mrp_pack] Error: entry_offset ({entry_offset}) >= code_size "
                  f"({len(code)}), which is invalid.")

    checksum = mrp_checksum(code)

    header = struct.pack(
        HEADER_FMT,
        MRP_MAGIC,
        MRP_VERSION,
        entry_offset,
        len(code),
        flags,
        checksum,
    )

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(code)

    print(f"[mrp_pack] Done -> {out_path}")
    print(f"           code_size    = {len(code)} byte")
    print(f"           entry_offset = {entry_offset}")
    print(f"           checksum     = 0x{checksum:08x}")
    print(f"           total file   = {HEADER_SIZE + len(code)} byte")


def main():
    parser = argparse.ArgumentParser(description="Compile & pack a .mrp program for Equinox OS")
    parser.add_argument("source", help="Program source file .cpp/.cc/.cxx/.c (mtcc.c is used as-is, compiled as C++)")
    parser.add_argument("output", help="Output .mrp file name")
    parser.add_argument("--keep-temp", action="store_true",
                         help="Keep the intermediate .o/.elf/.bin files for debugging")
    parser.add_argument("--from-bin", metavar="BIN_FILE",
                         help="Skip compiling, directly wrap an existing .bin file")
    args = parser.parse_args()

    if args.from_bin:
        pack(args.from_bin, args.output)
        return

    if not args.source.endswith((".cpp", ".cc", ".cxx", ".c")):
        sys.exit("[mrp_pack] Source must be a .cpp/.cc/.cxx/.c file (or use --from-bin)")

    if args.keep_temp:
        work_dir = os.path.join(os.path.dirname(os.path.abspath(args.output)) or ".",
                                 "mrp_build_tmp")
        os.makedirs(work_dir, exist_ok=True)
        bin_path = compile_and_link(args.source, work_dir)
        pack(bin_path, args.output)
        print(f"[mrp_pack] Intermediate files kept in: {work_dir}")
    else:
        with tempfile.TemporaryDirectory() as work_dir:
            bin_path = compile_and_link(args.source, work_dir)
            pack(bin_path, args.output)


if __name__ == "__main__":
    main()
