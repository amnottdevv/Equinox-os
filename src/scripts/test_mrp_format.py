#!/usr/bin/env python3
"""
test_mrp_format.py — Verifikasi format file .mrp sesuai spec mrp_format.h

Lakukan cek yang SAMA PERSIS dengan is_valid_mrp() di kernel:
1. Cek magic = "MRP1"
2. Cek version = 1
3. Cek code_size > 0
4. Cek code_size == (total_len - 18)
5. Cek entry_offset < code_size
6. Cek checksum cocok (rotate-xor algorithm)

Plus tambahan:
- Dump hex dump header + first 64 byte code
- Disassemble first instructions (kalau objdump available) untuk lihat entry
"""

import os
import struct
import sys
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")

HEADER_FMT = "<4sBIIBI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 18

def mrp_checksum(data):
    """Harus identik dengan mrp_checksum() di mrp_format.h (rotate-xor)."""
    checksum = 0x811C9DC5
    MASK32 = 0xFFFFFFFF
    for byte in data:
        checksum = ((checksum << 5) | (checksum >> 27)) & MASK32
        checksum ^= byte
        checksum &= MASK32
    return checksum

REASON_STR = {
    0: "OK",
    1: "ERR_TOO_SMALL",
    2: "ERR_BAD_MAGIC",
    3: "ERR_BAD_VERSION",
    4: "ERR_SIZE_MISMATCH",
    5: "ERR_BAD_ENTRY",
    6: "ERR_BAD_CHECKSUM",
    7: "ERR_EMPTY_CODE",
}

def validate_mrp(file_data):
    """Mirror is_valid_mrp() di kernel/library/header/mrp_format.h."""
    total_len = len(file_data)
    if total_len < HEADER_SIZE:
        return False, 1, "file < 18 byte"
    magic, version, entry_offset, code_size, flags, checksum = struct.unpack(
        HEADER_FMT, file_data[:HEADER_SIZE]
    )
    if magic != b"MRP1":
        return False, 2, f"magic={magic!r}, expect b'MRP1'"
    if version != 1:
        return False, 3, f"version={version}, expect 1"
    if code_size == 0:
        return False, 7, "code_size == 0"
    if code_size != (total_len - HEADER_SIZE):
        return False, 4, f"code_size={code_size}, actual={total_len - HEADER_SIZE}"
    if entry_offset >= code_size:
        return False, 5, f"entry_offset={entry_offset} >= code_size={code_size}"
    code = file_data[HEADER_SIZE:]
    computed = mrp_checksum(code)
    if computed != checksum:
        return False, 6, f"checksum=0x{checksum:08x}, computed=0x{computed:08x}"
    return True, 0, "OK"


def hexdump(data, n=64):
    """Dump first n bytes as hex."""
    lines = []
    for i in range(0, min(len(data), n), 16):
        chunk = data[i:i+16]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {i:04x}  {hex_part:<48}  {ascii_part}")
    return "\n".join(lines)


def disasm_entry(code_bytes, n=32):
    """Disassemble first n bytes of code via objdump (i386)."""
    try:
        # Tulis ke temp file karena /dev/stdin gak reliable di semua env.
        import tempfile
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tf:
            tf.write(code_bytes[:n])
            tmp_path = tf.name
        try:
            result = subprocess.run(
                ["objdump", "-D", "-b", "binary", "-m", "i386",
                 "--adjust-vma=0x500010", tmp_path],
                capture_output=True, timeout=5
            )
            out = result.stdout.decode("utf-8", errors="replace")
            lines = []
            for l in out.splitlines():
                # Baris disasm format: "  500010:\t55              \tpush   %ebp"
                if ":" in l and "\t" in l and not l.startswith("Disassembly"):
                    lines.append("  " + l.strip())
            return "\n".join(lines[:8]) if lines else "  (no disasm output)"
        finally:
            os.unlink(tmp_path)
    except Exception as e:
        return f"  (disasm failed: {e})"


def main():
    if not os.path.isdir(DIST):
        print(f"FAIL: dist/ directory tidak ada ({DIST})")
        return 1

    mrp_files = sorted(f for f in os.listdir(DIST) if f.endswith(".mrp"))
    if not mrp_files:
        print(f"FAIL: tidak ada file .mrp di {DIST}")
        print("Build dulu: make pack  (atau)  python3 mrp_user/mrp_pack.py ...")
        return 1

    print("=" * 70)
    print(" Equinox OS .mrp Format Validation Test")
    print("=" * 70)
    print(f" Header format: {HEADER_FMT} (size={HEADER_SIZE} byte)")
    print(f" Found {len(mrp_files)} .mrp file(s) in {DIST}/")
    print()

    all_ok = True
    for fname in mrp_files:
        path = os.path.join(DIST, fname)
        with open(path, "rb") as f:
            data = f.read()

        print(f"--- {fname} ({len(data)} byte) ---")
        ok, reason, detail = validate_mrp(data)
        if ok:
            magic, version, entry_offset, code_size, flags, checksum = struct.unpack(
                HEADER_FMT, data[:HEADER_SIZE]
            )
            print(f"  [OK] valid .mrp")
            print(f"       magic        = {magic!r}")
            print(f"       version      = {version}")
            print(f"       entry_offset = 0x{entry_offset:x}")
            print(f"       code_size    = {code_size} byte")
            print(f"       flags        = 0x{flags:02x}")
            print(f"       checksum     = 0x{checksum:08x}")
            print(f"  Header + first 64 byte code:")
            print(hexdump(data, 64))
            print(f"  First instructions (disasm i386):")
            print(disasm_entry(data[HEADER_SIZE:], 32))
        else:
            print(f"  [FAIL] {REASON_STR.get(reason, '?')}: {detail}")
            all_ok = False
        print()

    print("=" * 70)
    if all_ok:
        print(f" ALL {len(mrp_files)} FILE(S) PASSED VALIDATION")
        print(" File siap di-bundle ke ISO via `make` (grub.cfg akan include).")
        return 0
    else:
        print(" SOME FILES FAILED -- see above")
        return 1


if __name__ == "__main__":
    sys.exit(main())
