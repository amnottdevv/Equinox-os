#!/usr/bin/env python3
"""
check_api_consistency.py — verifikasi single source of truth mrp_api_t

Lakukan 3 cek:
  1. Pastikan `struct mrp_api_t` HANYA didefinisikan di mrp_user/mrp_api.h.
     Cari `struct mrp_api_t {` di seluruh codebase. Harus exactly 1 match.
  2. Pastikan kernel/library/header/mrp_api.h adalah shim yang hanya
     #include "../../../mrp_user/mrp_api.h".
  3. Pastikan mrp_loader.h TIDAK lagi define struct mrp_api_t (cukup
     #include "mrp_api.h").

Exit code:
  0 = semua check pass
  1 = ada inconsistency (print detail ke stderr)
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

errors = []
notes  = []

def grep(pattern, *files):
    """Return list of (file, line_no, line) matches."""
    out = []
    for f in files:
        if not os.path.isfile(f):
            continue
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                for i, line in enumerate(fh, 1):
                    if re.search(pattern, line):
                        out.append((f, i, line.rstrip()))
        except Exception as e:
            errors.append(f"cannot read {f}: {e}")
    return out

# ---- Collect all .h and .cpp files (skip lvgl vendor tree) ----
all_files = []
for dirpath, dirnames, filenames in os.walk(ROOT):
    parts = dirpath.split(os.sep)
    if "lvgl" in parts and "gui" in parts:
        continue
    if "build" in parts or "dist" in parts:
        continue
    for name in filenames:
        if name.endswith((".h", ".hpp", ".cpp", ".c", ".cc")):
            all_files.append(os.path.join(dirpath, name))

# ---- Check 1: count `struct mrp_api_t {` ----
struct_def_pattern = r"struct\s+mrp_api_t\s*\{"
struct_defs = []
for f in all_files:
    matches = grep(struct_def_pattern, f)
    struct_defs.extend(matches)

if len(struct_defs) == 0:
    errors.append("CHECK 1 FAIL: tidak ada definisi `struct mrp_api_t {` di codebase (kenapa?)")
elif len(struct_defs) > 1:
    errors.append(f"CHECK 1 FAIL: ditemukan {len(struct_defs)} definisi `struct mrp_api_t {{` (harusnya 1):")
    for f, n, line in struct_defs:
        errors.append(f"  {f}:{n}: {line}")
else:
    f, n, line = struct_defs[0]
    expected = os.path.join(ROOT, "mrp_user", "mrp_api.h")
    if os.path.abspath(f) != os.path.abspath(expected):
        errors.append(f"CHECK 1 FAIL: definisi ada di {f}, bukan di {expected}")
    else:
        notes.append(f"CHECK 1 OK: single source of truth -> {f}:{n}")

# ---- Check 2: kernel/library/header/mrp_api.h is a shim ----
shim_path = os.path.join(ROOT, "kernel", "library", "header", "mrp_api.h")
if not os.path.isfile(shim_path):
    errors.append(f"CHECK 2 FAIL: shim file tidak ada: {shim_path}")
else:
    with open(shim_path, encoding="utf-8", errors="replace") as fh:
        shim_content = fh.read()
    if re.search(r"struct\s+mrp_api_t\s*\{", shim_content):
        errors.append(f"CHECK 2 FAIL: shim {shim_path} tidak boleh define struct mrp_api_t (cukup #include)")
    if not re.search(r'#include\s+"[\.\./]*mrp_user/mrp_api\.h"', shim_content):
        errors.append(f"CHECK 2 FAIL: shim {shim_path} harus #include mrp_user/mrp_api.h (relative path)")
    else:
        notes.append(f"CHECK 2 OK: shim {shim_path} hanya #include -> mrp_user/mrp_api.h")

# ---- Check 3: mrp_loader.h includes mrp_api.h, tidak define struct ----
loader_h = os.path.join(ROOT, "kernel", "library", "header", "mrp_loader.h")
if not os.path.isfile(loader_h):
    errors.append(f"CHECK 3 FAIL: file tidak ada: {loader_h}")
else:
    with open(loader_h, encoding="utf-8", errors="replace") as fh:
        loader_content = fh.read()
    if re.search(r"struct\s+mrp_api_t\s*\{", loader_content):
        errors.append(f"CHECK 3 FAIL: {loader_h} masih define `struct mrp_api_t {{` (harusnya dihapus, ganti #include)")
    if not re.search(r'#include\s+"mrp_api\.h"', loader_content):
        errors.append(f"CHECK 3 FAIL: {loader_h} harus #include \"mrp_api.h\"")
    else:
        notes.append(f"CHECK 3 OK: {loader_h} #include mrp_api.h, tidak duplikat definisi")

# ---- Check 4: mrp_loader.cpp pakai mrp_build_api() ----
loader_cpp = os.path.join(ROOT, "kernel", "library", "mrp_loader.cpp")
if os.path.isfile(loader_cpp):
    with open(loader_cpp, encoding="utf-8", errors="replace") as fh:
        c = fh.read()
    if "mrp_build_api()" not in c:
        errors.append(f"CHECK 4 FAIL: {loader_cpp} tidak memanggil mrp_build_api() (harusnya, supaya single point of update)")
    elif "api.print_text = print_string;" in c:
        errors.append(f"CHECK 4 FAIL: {loader_cpp} masih isi api.print_text inline (harusnya di mrp_api.cpp via mrp_build_api)")
    else:
        notes.append(f"CHECK 4 OK: {loader_cpp} pakai mrp_build_api() (no inline table)")

# ---- Check 5: mrp_api.cpp mengisi SEMUA field mrp_api_t ----
api_cpp = os.path.join(ROOT, "kernel", "library", "mrp_api.cpp")
if os.path.isfile(api_cpp):
    with open(api_cpp, encoding="utf-8", errors="replace") as fh:
        api_cpp_content = fh.read()
    expected_fields = [
        "api_version",
        "print_text", "print_int", "read_line", "get_key", "alloc", "get_tick",
        "str_len", "str_cmp", "str_cpy", "str_cat", "str_split",
        "to_int", "int_to_str",
        "vec_create", "vec_push", "vec_get", "vec_size", "vec_free",
    ]
    missing = [f for f in expected_fields if f"api.{f}" not in api_cpp_content]
    if missing:
        errors.append(f"CHECK 5 FAIL: mrp_api.cpp tidak set field: {', '.join(missing)}")
    else:
        notes.append(f"CHECK 5 OK: mrp_api.cpp set semua {len(expected_fields)} field mrp_api_t")

    # Cross-check: jumlah field di struct mrp_api_t di header harus == jumlah expected_fields
    header_path = os.path.join(ROOT, "mrp_user", "mrp_api.h")
    with open(header_path, encoding="utf-8", errors="replace") as fh:
        header_content = fh.read()
    m = re.search(r"struct\s+mrp_api_t\s*\{(.*?)\};", header_content, re.DOTALL)
    if m:
        body = m.group(1)
        fields_in_header = re.findall(r"\(\s*\*\s*(\w+)\s*\)", body)
        if "api_version" in body:
            fields_in_header_with_version = fields_in_header + ["api_version"]
        else:
            fields_in_header_with_version = fields_in_header
        header_set = set(fields_in_header_with_version)
        expected_set = set(expected_fields)
        if header_set != expected_set:
            only_header = header_set - expected_set
            only_expected = expected_set - header_set
            if only_header:
                errors.append(f"CHECK 5b FAIL: field di header tapi gak di list check: {only_header}")
            if only_expected:
                errors.append(f"CHECK 5b FAIL: field di list check tapi gak di header: {only_expected}")
        else:
            notes.append(f"CHECK 5b OK: {len(header_set)} field di struct match {len(expected_set)} field di mrp_api.cpp")
else:
    errors.append(f"CHECK 5 FAIL: file tidak ada: {api_cpp}")

# ---- Check 6: shell dispatcher ada handler "./" ----
kernel_cpp = os.path.join(ROOT, "kernel", "kernel.cpp")
if os.path.isfile(kernel_cpp):
    with open(kernel_cpp, encoding="utf-8", errors="replace") as fh:
        k = fh.read()
    if 'starts_with(input, "./")' not in k:
        errors.append(f"CHECK 6 FAIL: kernel.cpp tidak ada handler './' (workflow A.1)")
    else:
        notes.append(f"CHECK 6 OK: kernel.cpp punya handler './' (workflow A.1)")
    if 'mrp_run(cwd, name)' not in k:
        errors.append(f"CHECK 6b FAIL: kernel.cpp tidak panggil mrp_run()")

# ---- Check 7: GRUB multiboot module loader (workflow Bagian A blocker) ----
mrp_bootloader_cpp = os.path.join(ROOT, "kernel", "library", "mrp_bootloader.cpp")
if not os.path.isfile(mrp_bootloader_cpp):
    errors.append(f"CHECK 7 FAIL: file tidak ada: {mrp_bootloader_cpp}")
else:
    with open(mrp_bootloader_cpp, encoding="utf-8", errors="replace") as fh:
        mb = fh.read()
    if "mrp_bootloader_load_modules" not in mb:
        errors.append(f"CHECK 7 FAIL: {mrp_bootloader_cpp} tidak define mrp_bootloader_load_modules")
    elif "MULTIBOOT_INFO_MODS" not in mb:
        errors.append(f"CHECK 7 FAIL: {mrp_bootloader_cpp} tidak cek flag MULTIBOOT_INFO_MODS (bisa baca garbage)")
    elif "fs_write_binary" not in mb:
        errors.append(f"CHECK 7 FAIL: {mrp_bootloader_cpp} tidak panggil fs_write_binary (gak masuk RAMFS)")
    else:
        notes.append(f"CHECK 7 OK: {mrp_bootloader_cpp} cek MODS flag + panggil fs_write_binary")

    # Check kernel.cpp panggil mrp_bootloader_load_modules
    if os.path.isfile(kernel_cpp):
        if "mrp_bootloader_load_modules" not in k:
            errors.append(f"CHECK 7b FAIL: kernel.cpp tidak panggil mrp_bootloader_load_modules di kernel_main()")
        else:
            notes.append(f"CHECK 7b OK: kernel.cpp memanggil mrp_bootloader_load_modules di kernel_main()")

# ---- Check 8: grub.cfg punya `module` directive ----
grub_cfg = os.path.join(ROOT, "boot", "grub", "grub.cfg")
if not os.path.isfile(grub_cfg):
    errors.append(f"CHECK 8 FAIL: grub.cfg tidak ada: {grub_cfg}")
else:
    with open(grub_cfg, encoding="utf-8", errors="replace") as fh:
        grub = fh.read()
    if "module " not in grub and "module\t" not in grub:
        errors.append(f"CHECK 8 FAIL: grub.cfg tidak punya `module` directive (file .mrp gak akan di-load GRUB)")
    else:
        n_modules = grub.count("module ") + grub.count("module\t")
        notes.append(f"CHECK 8 OK: grub.cfg punya {n_modules} `module` directive(s)")

# ---- Check 9: Makefile copy .mrp ke ISO ----
makefile_path = os.path.join(ROOT, "makefile")
if os.path.isfile(makefile_path):
    with open(makefile_path, encoding="utf-8", errors="replace") as fh:
        mk = fh.read()
    if "*.mrp" not in mk:
        errors.append(f"CHECK 9 FAIL: Makefile tidak copy *.mrp ke ISO (file .mrp gak masuk ISO)")
    else:
        notes.append(f"CHECK 9 OK: Makefile copy *.mrp ke ISO")
    if "pack:" not in mk:
        errors.append(f"CHECK 9b FAIL: Makefile tidak punya target `pack` (convenience)")
    else:
        notes.append(f"CHECK 9b OK: Makefile punya target `pack`")

# ---- Check 10: vector.h punya named struct (bukan anonymous) ----
vector_h = os.path.join(ROOT, "kernel", "library", "header", "vector.h")
if os.path.isfile(vector_h):
    with open(vector_h, encoding="utf-8", errors="replace") as fh:
        vh = fh.read()
    if re.search(r"typedef\s+struct\s*\{", vh):
        errors.append(f"CHECK 10 FAIL: vector.h masih pakai anonymous struct (gak bisa forward-declare di mrp_api.h)")
    elif re.search(r"typedef\s+struct\s+Vector\s*\{", vh):
        notes.append(f"CHECK 10 OK: vector.h pakai named struct (Vector) -- bisa di-forward-declare")
    else:
        errors.append(f"CHECK 10 FAIL: vector.h format tidak dikenali")

# ---- Print report ----
print("=" * 60)
print(" Equinox OS MRP API Consistency Check")
print("=" * 60)
for n in notes:
    print(f"  [OK]   {n}")
print()
if errors:
    print(f"  FAILED: {len(errors)} issue(s):")
    for e in errors:
        print(f"  [FAIL] {e}")
    sys.exit(1)
else:
    print("  ALL CHECKS PASSED.")
    sys.exit(0)
