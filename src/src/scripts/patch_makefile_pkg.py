#!/usr/bin/env python3
"""patch_makefile_pkg.py — tambahkan staging paket eggkg bash (v0.9) ke makefile.

4 perubahan:
  1. Definisi variabel PKG_BASH_DIR / PKG_BASH_FILES (setelah ALL_MODULES)
  2. Prasyarat ISO += $(PKG_BASH_FILES)
  3. Argumen gen_grubcfg.py += $(PKG_BASH_FILES)  (module line per file)
  4. Blok staging ISO: dist/equinox/.local/bash -> boot/equinox/.local/bash

Indentasi diambil dari blok RUF_FILES yang ada (anti tab/space mismatch).
"""
import re
import sys

MK = "/home/z/my-project/equinox2/equinox_os_v0.2_beta/makefile"

with open(MK, "r") as f:
    text = f.read()

if "PKG_BASH_DIR" in text:
    print("sudah dipatch — keluar")
    sys.exit(0)

# ---- (1) variabel setelah ALL_MODULES ----
m = re.search(r'^ALL_MODULES\s*=.*\n', text, re.M)
assert m, "ALL_MODULES tidak ketemu"
ins = (
    "# v0.9: eggkg bash package (staging statis) -> RAMFS /equinox/.local/bash/\n"
    "# (kontrak layout eggkg: .local/<pkg>/{build.ruf,src/*.c} + hasil .mrp)\n"
    "PKG_BASH_DIR   = $(DIST_DIR)/equinox/.local/bash\n"
    "PKG_BASH_FILES = $(PKG_BASH_DIR)/build.ruf $(wildcard $(PKG_BASH_DIR)/src/*.c)\n"
)
text = text[:m.end()] + ins + text[m.end():]

# ---- (2) prasyarat ISO ----
old_dep = "elfdemo $(ALL_MODULES) | $(DIST_DIR)"
new_dep = "elfdemo $(ALL_MODULES) $(PKG_BASH_FILES) | $(DIST_DIR)"
assert text.count(old_dep) == 1, "baris dep ISO tidak unik"
text = text.replace(old_dep, new_dep)

# ---- (3) gen_grubcfg args ----
m = re.search(r'^(\s*python3 \S*gen_grubcfg\.py .*)$', text, re.M)
assert m, "baris gen_grubcfg tidak ketemu"
line = m.group(1)
text = text.replace(line, line + " $(PKG_BASH_FILES)", 1)

# ---- (4) blok staging setelah blok RUF_FILES ----
m = re.search(r'^([ \t]*)@if \[ -n "\$\(RUF_FILES\)" \]; then \\$', text, re.M)
assert m, "blok RUF_FILES tidak ketemu"
ind = m.group(1)
# indentasi baris lanjutan = baris 'mkdir' di blok ruf
m2 = re.search(r'\n([ \t]+)mkdir -p \$\(ISO_DIR\)/boot/equinox; \\\n'
               r'[ \t]+cp \$\(RUF_FILES\) \$\(ISO_DIR\)/boot/equinox/; \\\n'
               r'[ \t]+fi\n', text)
assert m2, "isi blok RUF_FILES berubah?"
ind2 = m2.group(1)
end = m2.end()
block = (
    f"{ind}# v0.9: eggkg bash package -> boot/equinox/.local/bash/ (RAMFS\n"
    f"{ind}# /equinox/.local/bash/) — paket contoh siap-build (mtcc -make /\n"
    f"{ind}# equinoxinstall -build) + kontrak layout eggkg (.local/<pkg>).\n"
    f"{ind}@if [ -f \"$(PKG_BASH_DIR)/build.ruf\" ]; then \\\n"
    f"{ind2}mkdir -p $(ISO_DIR)/boot/equinox/.local/bash/src; \\\n"
    f"{ind2}cp $(PKG_BASH_DIR)/build.ruf $(ISO_DIR)/boot/equinox/.local/bash/; \\\n"
    f"{ind2}cp $(PKG_BASH_DIR)/src/*.c $(ISO_DIR)/boot/equinox/.local/bash/src/; \\\n"
    f"{ind}fi\n"
)
text = text[:end] + block + text[end:]

with open(MK, "w") as f:
    f.write(text)

print("patch OK:")
print("  + PKG_BASH_DIR/PKG_BASH_FILES")
print("  + prasyarat ISO")
print("  + gen_grubcfg args")
print("  + blok staging .local/bash")
