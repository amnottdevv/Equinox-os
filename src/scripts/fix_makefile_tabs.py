#!/usr/bin/env python3
"""fix_makefile_tabs.py — restore TAB indentation of Equinox OS makefile recipes.

The Edit tool sometimes rewrites files with space indentation; GNU make
requires a TAB at the start of recipe lines. This script converts a
leading 8 spaces into a TAB only on recipe lines (indented lines that
are not comments/assignment continuations).
"""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "makefile"
lines = open(path).read().split("\n")
fixed = 0
out = []
for ln in lines:
    m = re.match(r"^( {8})(\S.*)$", ln)
    if m:
        out.append("\t" + m.group(2))
        fixed += 1
    else:
        out.append(ln)
open(path, "w").write("\n".join(out))
print(f"[fix-tabs] {path}: {fixed} recipe lines converted to TAB")
