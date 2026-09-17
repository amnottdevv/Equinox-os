# Equinox OS

A 32-bit x86 monolithic-kernel hobby operating system, written in C++17,
booted by GRUB (multiboot), drawing to a VESA framebuffer, and able to run
DOOM (shareware) at ring 3 — plus a real TCP/IP stack that can download
files from the internet.

Current version: **v0.1 Beta**

---

## Features

- **Custom bootloader handoff** — GRUB loads the ELF kernel + RAMFS modules;
  `start.asm` sets up a VBE 1360x768x32 mode and a module staging area.
- **Boot experience** — a Linux-style `[ OK ]` boot log that lists every
  included file with its size, followed by the Equinox emblem (a
  purple/white gradient) and a straight drop into the shell.
- **VESA framebuffer console** — 1360x768 @ 32 bpp (DISPI override), with an
  automatic VGA text fallback and 24-bit RGB text colors.
- **RAM filesystem** — a tree-structured RAMFS populated at boot from GRUB
  multiboot modules (programs, samples, WADs via zero-copy staging).
- **Ring 3 userland** — paging with user/supervisor split, TSS-based
  privilege transitions, `int 0x80` syscalls (append-only, #0..#35).
- **MRP programs** — a tiny flat-binary executable format, a loader, and
  `mtcc` — an in-OS C compiler that compiles and runs C source live.
- **LVGL GUI** — apps written against LVGL 9 (file manager, settings,
  code editor) on top of the framebuffer + PS/2 mouse driver.
- **TCP/IP stack** — lwIP 2.1.3 (NO_SYS cooperative polling) on an NE2000
  ISA NIC: DHCP, DNS, ICMP ping, TCP, an HTTP client (`mget`) and an
  HTTP server (`httpd`).
- **DOOM** — the doomgeneric port running as a ring-3 `.mrp` program
  (~70 fps, mouse look, WASD, screenshots).
- **Games & tools** — snake, breakout, pong (Libgame framework), plus a
  calculator, hex tools, memory/map inspectors and more in the shell.

## Building

Requirements (Linux host):

- `g++`/`gcc` with `-m32` support (or an `i686-elf-` cross toolchain)
- `nasm`, `grub-mkrescue` (with `i386-pc` modules), `xorriso`, `mtools`
- Python 3 (build + test scripts)
- 32-bit `libgcc.a` (e.g. from `gcc-multilib`, or point
  `LIBGCC32_DIR` at one)

```sh
make            # kernel.elf + dist/equinox.iso
make pack       # pack mrp_user/*.cpp + games/*.cpp into dist/equinox/
make mtcc       # pack the in-OS compiler (root mtcc.c) -> tools/mtcc.mrp
make doom       # build doomgeneric -> games/doom.mrp
make test       # host-side mtcc test harness (no QEMU needed)
make run        # boot the ISO in QEMU with user-mode networking
```

`dist/` mirrors the in-OS RAMFS layout (`equinox/tools`, `equinox/games`,
flat `.mrp` in the root, `.c` samples under `/test`, `.wad` data in the
root). The WAD (`doom1.wad`, shareware) must be supplied manually —
licensing forbids automatic downloads.

## Running

```sh
qemu-system-i386 -m 64 -cdrom dist/equinox.iso \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

- Guest address: `10.0.2.15` (DHCP); the QEMU host side is `10.0.2.2`,
  DNS `10.0.2.3`.
- `hostfwd=tcp::8080-:80` exposes the in-OS `httpd` at
  `http://localhost:8080/` on the host.

## Shell reference (selection)

| Command | Description |
| --- | --- |
| `help` | full command list |
| `ls`, `cd`, `pwd`, `tree`, `cat`, `rm` | RAMFS navigation |
| `edit <file>` | built-in text editor |
| `mtcc <file.c>` | compile & run C, in-OS |
| `run <x.mrp>` / `./x.mrp` | run an MRP program (ring 3) |
| `ifconfig` | network interface info |
| `ping <host>` | ICMP echo (ICMP is not relayed by QEMU user-net — use `tcpping`) |
| `tcpping <host> [port]` | TCP connectivity probe + RTT |
| `dns <hostname>` | resolve a name via DNS |
| `mget <url> [-port <n>]` | HTTP download to the RAMFS (any file type) |
| `httpd` | mini web server on :80 (status page + RAMFS files) |
| `doom [args]` | run DOOM (needs `doom1.wad`) |
| `syscalls`, `sctest`, `memmap`, `ring` | introspection & self-tests |

### mget — the HTTP client

```
mget <url> [-port <n>]
  url   : http://host[:port]/path    (https not supported - no TLS)
          host[:port]/path           (http:// assumed)
  -port : force the destination port (overrides the URL)
  saves : <basename> into the current directory (any file type)
examples:
  mget http://10.0.2.2:8022/data.json
  mget example.com/file.json -port 8080
```

Port priority: `-port` option > `:port` in the URL > scheme default (80).
Redirects (301/302/303/307/308) are followed (max 3 hops). The response
`Content-Type` and HTTP status are always reported. `https://` URLs are
rejected with a clear message (no TLS stack yet).

## Repository layout

```
boot/          start.asm (VBE + module staging) + grub.cfg template
kernel/        the kernel
  kernel.cpp   shell + boot sequence
  library/     core services: memory, fs, syscalls, drivers, UI, libc...
  net/         lwIP glue + NE2000 driver + httpd
  gui/         LVGL apps (file manager) + the LVGL 9 tree
mrp_user/      ring-3 tools (hello, demos, crash tests) + Morph.h SDK
games/         snake, breakout, pong (Libgame)
Libgame/       tiny game framework for .mrp programs
test/          .c samples for the in-OS mtcc compiler
doomgeneric/   the doomgeneric port (built into doom.mrp)
third_party/   lwIP 2.1.3 sources
scripts/       build helpers + QEMU test harnesses
mtcc.c         the in-OS C compiler (canonical source)
makefile       top-level build
```

## Third-party components

- **lwIP 2.1.3** — BSD-3-Clause, `third_party/lwip-2.1.3` (COPYING included)
- **LVGL 9** — MIT, `kernel/gui/lvgl`
- **doomgeneric** — id Software DOOM license (shareware WAD required)
- SEABIOS via QEMU for testing only

## License

Hobby-project code written for Equinox OS itself; see the third-party
directories for their respective licenses.
