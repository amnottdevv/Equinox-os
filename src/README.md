# Equinox OS

A 32-bit x86 monolithic-kernel hobby operating system, written in C++17,
booted by GRUB (multiboot), drawing to a VESA framebuffer, with a real
TCP/IP stack that can download files from the internet over HTTP**S**, and
a disk subsystem (ATA PIO + FAT32 read/write) that can mount a hard disk,
run DOOM straight from it, and persist downloads across reboots.

Current version: **v0.3 Beta**

User-facing documentation (Bahasa Indonesia) lives in [`docs/`](docs/) —
release notes, QEMU quick start, command reference and an architecture
overview.

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
- **ATA + FAT32 disk subsystem (v0.2)** — a polling PIO IDE driver
  (primary/secondary, master/slave, LBA28/LBA48, ATAPI detection, MBR
  partition parsing) with a full FAT32 driver: read AND write. The first
  FAT32 partition is auto-mounted at `/mnt` at boot with a lazy RAMFS
  mirror — `ls`, `cd`, `cat`, `rm`, the GUI file manager and every
  syscall work on disk files transparently. Writes are write-through:
  `mget` a file into `/mnt`, power off, and it is still there on the next
  boot. LFN (long file names) are read and written with 8.3 mangling
  (numeric-tail collision handling against the REAL on-disk short
  names), FSInfo free-cluster accounting is maintained, and directory
  clusters grow on demand. The write path is hardened: sector-granular
  writes (any cluster size), rollback on mid-write I/O failure, and a
  host-verified mini-fsck (FAT copies identical, FSInfo exact, LFN
  padding spec-clean) up to 100 % disk usage. Big files cache in an
  8 MB disk arena beyond the kernel heap — that is how
  `doom -iwad /mnt/doom1.wad` runs at full speed straight off the disk.
- **Ring 3 userland** — paging with user/supervisor split, TSS-based
  privilege transitions, `int 0x80` syscalls (append-only, #0..#54 —
  full file API since v0.3: open2/unlink/mkdir/rmdir/rename/stat/
  readdir/fstat + free; process & memory model since v0.3:
  wait/pipe/meminfo/spawn2; per-task graphics window since v0.3:
  setclip/drawline).
- **Demand paging (v0.3)** — per-task VMA windows are RESERVED
  with non-present `PTE_DEMAND` markers; the #PF handler hands out
  one zero-filled physical page per first touch. A 24 MB DOOM arena
  only costs the pages DOOM actually touches. `meminfo` + syscall 51
  report the pool + per-task footprint (reserved vs faulted).
- **ELF32 loader (v0.3)** — static ELF32/i386 executables run
  next to `.mrp` (PT_LOAD segments validated + mapped into the demand
  window, entry = e_entry, bss zero-filled on demand). Build them with
  the host `gcc -m32 -nostdlib -T mrp_user/elf_link.ld` (see
  `tools_user/elfdemo.c` → `make elfdemo`).
- **wait / pipes (v0.3)** — POSIX-style process plumbing:
  SYS_WAIT blocks until a child exits (zombies hold the slot + status
  for their parent; killed children stay waitable), SYS_PIPE is a 4 KB
  kernel ring with ref-counted ends, fd inheritance across spawn,
  blocking read/write, EOF and broken-pipe detection. `pipedemo`
  demonstrates the full `child | parent` flow.
- **PCI bus enumeration (v0.3, FR-12)** — a config-space driver
  (0xCF8/0xCFC, type-0 cycles) scans the bus at boot — host bridge,
  PIIX3 ISA/IDE bridges, VGA, USB — with BAR decoding and
  PCI-to-PCI bridge recursion. `lspci` prints the table; the NIC
  layer queries it for recognized Ethernet controllers.
- **NIC driver registry (v0.3, FR-13)** — `kernel/net/nic.c`
  abstracts the NIC behind one struct (probe/send/recv/mac/irq/
  overflow) so the lwIP glue never names a driver. ne2000-isa is the
  first registered driver; recognized-but-undriven PCI NICs (e1000,
  pcnet32, eepro100) are reported honestly instead of silently
  ignored. Adding a driver = one struct + one registry line.
- **Per-task draw window + Bresenham (v0.3, FR-17/18)** —
  syscall 53 `set_clip(x|w<<16, y|h<<16)` confines a task's drawing
  (put_pixel / fill_rect / draw_line are all clipped), syscall 54
  `draw_line` is a focus-gated Bresenham line. Canvas semantics are
  honest: the first draw of a task snapshots the text screen and
  clears to black — graphics programs get a clean canvas, text output
  goes to the serial mirror + cell mirror, and the console is
  re-rendered from the mirror when the program exits.
- **In-OS libc (v0.3, FR-19)** — mtcc's `<morph.h>` prelude is
  a real libc subset rendered LOCALLY: printf/sprintf/snprintf
  (%d %i %u %x %X %o %p %c %s, width/pad/align; %u is a true unsigned
  render with binary long division), sscanf, the ctype family,
  string extras (strdup/strtok/strspn/strcspn/strcasecmp),
  qsort_int/qsort_str, rand/srand, abs, puts/fputs/fputc/fgetc,
  remove/rename, strerror — plus the existing memory/file/multitask
  wrappers.
- **Undefined-reference detection (v0.3, FR-20)** — mtcc now
  reports declared-but-never-defined functions as link errors with
  the first call site's line number (`undeftest.c` in `/test`
  demonstrates it).
- **Self-hosting userland — `eqbuild` (v0.3)** —
  the ISO ships **29** userland tools as **C sources**: the file set
  (`ls`, `cat`, `cp`, `mv`, `mkdir`, `rmdir`, `rm`, `touch`, `stat`,
  `fstest`, `pipedemo`) plus the v0.3 tools release (`grep`, `head`,
  `tail`, `wc`, `sort`, `uniq`, `cut`, `tr`, `rev`, `nl`, `more`,
  `find`, `which`, `diff`, `strings`, `cksum`, `basename`, `dirname`);
  the `eqbuild` shell command compiles them all with the in-OS mtcc
  (per-file log, `x/y OK`, source removed on success) and only then
  does the shell use them. The OS builds its own userland at boot.
- **Scheduler idle discipline (v0.3 fix)** — a task that
  blocks itself (sleep/wait/pipe/console-gate) with nothing else
  runnable now HALTs (hlt) with interrupts on until an interrupt
  changes the picture, instead of silently cancelling the sleep;
  the nettask heartbeat no longer busy-spins while the shell
  sleeps. sleep(ms) is exact to the tick (verified: sleep(3000) =
  300 ticks + 3.0 s wall).
- **MRP programs** — a tiny flat-binary executable format, a loader, and
  `mtcc` — an in-OS C compiler that compiles and runs C source live.
- **LVGL GUI** — apps written against LVGL 9 (file manager, settings,
  code editor) on top of the framebuffer + PS/2 mouse driver.
- **TCP/IP stack** — lwIP 2.1.3 (NO_SYS cooperative polling) on an NE2000
  ISA NIC: DHCP, DNS, ICMP ping, TCP, an HTTP(S) client (`mget`) and an
  HTTP server (`httpd`).
- **TLS 1.2 client** — BearSSL 0.6 vendored into the kernel: `mget` can
  fetch `https://` URLs (including github.com) with strict certificate
  chain validation against 9 embedded Mozilla root CAs, and an honest
  warned fallback for chains it cannot verify (e.g. P-384 ECDSA roots).
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
make diskimg    # build dist/disk.img (64 MB FAT32 test/demo disk)
make run        # boot the ISO in QEMU with user-mode networking
make run-disk   # same, with the FAT32 disk attached (auto-mounted /mnt)
make test       # host-side mtcc test harness (no QEMU needed)
make test-fat32 # full FAT32 suite in QEMU (27 checks + mtools oracle)
make test-doom-disk # DOOM booting straight from the FAT32 disk
```

`dist/` mirrors the in-OS RAMFS layout (`equinox/tools`, `equinox/games`,
flat `.mrp` in the root, `.c` samples under `/test`, `.wad` data in the
root). The WAD (`doom1.wad`, shareware) must be supplied manually —
licensing forbids automatic downloads.

## Running

```sh
# with the FAT32 disk (recommended — the v0.2 experience):
qemu-system-i386 -m 64 -boot order=d -cdrom dist/equinox.iso \
    -drive file=dist/disk.img,format=raw,if=ide,index=0,media=disk \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9

# or just the ISO:
qemu-system-i386 -m 64 -cdrom dist/equinox.iso \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

- Guest address: `10.0.2.15` (DHCP); the QEMU host side is `10.0.2.2`,
  DNS `10.0.2.3`.
- `hostfwd=tcp::8080-:80` exposes the in-OS `httpd` at
  `http://localhost:8080/` on the host.
- The FAT32 disk auto-mounts at `/mnt` (boot log:
  `FAT32: 'EQDISK' mounted at /mnt (58.0 MB free)`). Try
  `ls /mnt`, `cat /mnt/README.TXT`, `xxd /mnt/bin.dat 32`,
  `doom -iwad /mnt/doom1.wad`, or `cd /mnt && mget http://...` to
  download files straight onto the (persistent) disk.

## Shell reference (selection)

| Command | Description |
| --- | --- |
| `ls`, `cd`, `pwd`, `tree`, `cat`, `rm` | filesystem navigation (RAMFS and FAT32 `/mnt` alike) |
| `grep`, `head`, `tail`, `wc`, `nl` | text tools — search (`-i -n -c -v`, mini-regex `. * ^ $`), first/last N lines, counts, numbered lines (userland, eqbuild) |
| `sort`, `uniq`, `cut`, `tr`, `rev` | sort lines (`-r`), collapse adjacent duplicates (`-c`), cut fields (`-d`, `-f N,M-N`), translate/delete chars (ranges `a-z`), reverse lines |
| `more`, `find`, `which`, `diff` | pager (23-line pages), recursive file search (`-name`), locate a command on the system path, line-by-line file compare |
| `strings`, `cksum`, `basename`, `dirname` | printable runs, byte checksum + size, path component split |
| `cfile`, `ccfile`, `save`, `cdir` | create file / create with content / **create-or-overwrite** (`save name << "text"`) / create directory — write-through on `/mnt` |
| `diskinfo` | ATA drives + mounted FAT32 volume details (layout, free clusters, arena) |
| `mount` / `umount` | attach / detach the FAT32 disk at `/mnt` |
| `xxd <file> [n]` | hex dump of the first n bytes (default 64) — works on disk files |
| `edit <file>` | built-in text editor |
| `mtcc <file.c>` | compile & run C, in-OS |
| `eqbuild` | **self-hosting**: compile every `/equinox/tools/*.c` source (29 tools) with the in-OS mtcc, install the `.mrp` tools, remove the sources |
| `lspci` | PCI bus table (devices, classes, BARs, IRQ routing) |
| `run <x.mrp>` / `./x.mrp` | run an MRP program (ring 3) |
| `ifconfig` | network interface info |
| `ping <host>` | ICMP echo (ICMP is not relayed by QEMU user-net — use `tcpping`) |
| `tcpping <host> [port]` | TCP connectivity probe + RTT |
| `dns <hostname>` | resolve a name via DNS |
| `mget <url> [-port <n>]` | HTTP download to the RAMFS (any file type) |
| `httpd` | mini web server on :80 (status page + RAMFS files) |
| `doom [args]` | run DOOM (needs `doom1.wad`) |
| `syscalls`, `sctest`, `memmap`, `ring` | introspection & self-tests |

### mget — the HTTP(S) client

```
mget <url> [-port <n>]
  url   : http://host[:port]/path     plain HTTP
          https://host[:port]/path    TLS 1.2 (BearSSL)
          host[:port]/path            http:// assumed
  -port : force the destination port (overrides the URL)
  saves : <basename> into the current directory (any file type)
examples:
  mget http://10.0.2.2:8022/data.json
  mget https://raw.githubusercontent.com/torvalds/linux/master/README
  mget https://github.com/octocat/Hello-World
```

Port priority: `-port` option > `:port` in the URL > scheme default
(80 http / 443 https). Redirects (301/302/303/307/308) are followed
(max 3 hops, across schemes and hosts). The response `Content-Type`
and HTTP status are always reported.

**HTTPS** rides on BearSSL 0.6 (TLS 1.2, ECDHE_RSA / ECDHE_ECDSA,
AES-GCM / ChaCha20-Poly1305). Each connection first tries strict chain
validation against 9 embedded Mozilla root CAs (DigiCert, ISRG/Let's
Encrypt, GTS, Amazon, USERTrust, AAA, GlobalSign — the subset whose
keys BearSSL can verify on i386: RSA + ECDSA P-256). When the server's
chain cannot be verified — an unknown root, or a P-384 ECDSA hierarchy
like github.com's current Sectigo chain — the transfer automatically
retries in parse-only mode: every record is still authenticated and
encrypted (the leaf key is extracted for the ECDHE signature check),
but the trust decision is skipped, and the shell prints a clear
`ENCRYPTED but NOT VERIFIED` warning. Entropy comes from RDRAND when
available (jitter mix otherwise), and certificate validity windows
are checked against the CMOS RTC clock.

## Repository layout

```
boot/          start.asm (VBE + module staging) + grub.cfg template
docs/          release documentation (QEMU run, commands, architecture)
kernel/        the kernel
  kernel.cpp   shell + boot sequence
  library/     core services: memory, fs, syscalls, drivers, UI, libc...
    drivers/ata.cpp          ATA/IDE PIO driver (LBA28/48, MBR parse)
    fs_fat32.cpp             FAT32 mount + read path (Phase A)
    fs_fat32_write.cpp       FAT32 write path (Phase B, write-through)
    fs_ram.cpp               RAMFS + the FAT32 backing-store bridge
  net/         lwIP glue + NE2000 driver + httpd + TLS client (BearSSL)
  gui/         LVGL apps (file manager) + the LVGL 9 tree
mrp_user/      ring-3 tools (hello, demos, crash tests) + Morph.h SDK
tools_user/   userland tool SOURCES (29 tools: ls/cat/cp/... + the
               v0.3 tools release grep/head/tail/wc/sort/uniq/cut/tr/
               rev/nl/more/find/which/diff/strings/cksum/basename/
               dirname — shipped as .c, compiled in-OS by `eqbuild`;
               the prebuilt host step is
               commented out of the makefile)
games/         snake, breakout, pong (Libgame)
Libgame/       tiny game framework for .mrp programs
test/          .c samples for the in-OS mtcc compiler (gfxclip.c:
               per-task clip demo; undeftest.c: FR-20 link-error demo;
               libc.c: the FR-19 libc self-test; sleeptest.c: timing)
doomgeneric/   the doomgeneric port (built into doom.mrp)
third_party/   lwIP 2.1.3 + BearSSL 0.6 sources (incl. TLS root anchors)
scripts/       build helpers + QEMU test harnesses
mtcc.c         the in-OS C compiler (canonical source)
makefile       top-level build
```

## Third-party components

- **lwIP 2.1.3** — BSD-3-Clause, `third_party/lwip-2.1.3` (COPYING included)
- **BearSSL 0.6** — MIT, `third_party/bearssl` (LICENSE.txt included);
  TLS 1.2 client for `mget https://` — portable constant-time
  implementations only (AES-NI/SSE2 paths compiled out: no XMM state
  in ring 0)
- **LVGL 9** — MIT, `kernel/gui/lvgl`
- **doomgeneric** — id Software DOOM license (shareware WAD required)
- SEABIOS via QEMU for testing only

## License

Hobby-project code written for Equinox OS itself; see the third-party
directories for their respective licenses.
