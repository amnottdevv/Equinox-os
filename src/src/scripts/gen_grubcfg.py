#!/usr/bin/env python3
"""
gen_grubcfg.py — generate a grub.cfg ISO with the `module` lines INSIDE
the menuentry (a GRUB requirement: `module` is only valid after
`multiboot` within the same entry — module lines outside a menuentry are
ignored/error out at config parse time, so the modules never load).

Usage:
    gen_grubcfg.py <template> <output> <dist_dir> <file1> <file2> ...

The template contains a `#MODULES_HERE` marker (on its own line, inside
the menuentry, after the `multiboot` line). The marker is replaced with
one `    module /boot/<rel>` line per file, indented 4 spaces to keep
things tidy.

v10.6: <dist_dir> is the dist/ root. Each file's path RELATIVE to dist_dir
becomes both the ISO path and the module cmdline, e.g.

    dist/morph/tools/mtcc.mrp ->
        module /boot/morph/tools/mtcc.mrp morph/tools/mtcc.mrp

The kernel-side mrp_bootloader uses that relative cmdline as the RAMFS
destination path, so the ISO directory structure maps 1:1 onto the RAMFS
hierarchy (/morph/tools, /morph/games, ...). Flat files (dist/hello.c,
dist/foo.mrp) keep the old single-component behavior (root, or /test for
.c samples).
"""

import os
import sys


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: gen_grubcfg.py <template> <output> <dist_dir> [files...]")

    template, output, dist_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    files = sys.argv[4:]

    with open(template) as f:
        lines = f.read().splitlines()

    out = []
    for line in lines:
        if line.strip() == "#MODULES_HERE":
            for path in files:
                # Path relative to dist/ -> ISO path + module cmdline.
                rel = os.path.relpath(os.path.abspath(path),
                                      os.path.abspath(dist_dir))
                rel = rel.replace(os.sep, "/")
                # GRUB 2.12: the module cmdline = the arguments AFTER the
                # path (argv+1). If empty, the kernel does not know the
                # module's name — so the rel path is written explicitly as
                # the cmdline (doubling as its RAMFS routing path, see
                # mrp_bootloader.cpp).
                out.append(f"    module /boot/{rel} {rel}")
        else:
            out.append(line)

    with open(output, "w") as f:
        f.write("\n".join(out) + "\n")

    print(f"[grubcfg] {output}: {len(files)} module line(s) inside menuentry")


if __name__ == "__main__":
    main()
