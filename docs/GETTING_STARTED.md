# Getting Started — Building and Running Equinox OS

This document answers the most common question — *"how do I build and
run it?"* — from zero: host prerequisites, every `make` target, every
QEMU invocation variant, and what to do on the first boot. For
installing the OS onto a persistent disk, see
[INSTALL.md](INSTALL.md); for the package manager, see
[PACKAGES.md](PACKAGES.md).

## 1. Prerequisites (Linux host)

| Requirement | Notes |
| --- | --- |
| `g++`/`gcc` with `-m32` | or a cross toolchain `i686-elf-` |
| `nasm` | assembles `boot/start.asm` |
| `grub-mkrescue` + `i386-pc` modules | builds the bootable ISO (needs `xorriso`) |
| `mtools` | builds the FAT32 test images (`dist/disk.img`, `dist/equinox.img`) |
| Python 3 | build helpers + QEMU regression harnesses |
| 32-bit `libgcc.a` | from `gcc-multilib`, or point `LIBGCC32_DIR` at it |
| `qemu-system-i386` | runs the OS |

Debian/Ubuntu one-liner:

```sh
sudo apt install build-essential gcc-multilib nasm grub-pc-bin xorriso \
                 mtools python3 qemu-system-x86
```

> Custom toolchain location: point `LIBGCC32_DIR` at the folder holding
> the 32-bit `libgcc.a` and add its `bin/` to `PATH`. The makefile
> auto-detects `/usr/lib/grub/i386-pc` and `~/tools/root/usr/lib/grub/i386-pc`
> for `grub-mkrescue`, and falls back to `grub-mkimage` from `PATH`.

## 2. Build

```sh
make            # kernel.elf + dist/equinox.iso   (the main artifact)
make mtcc       # pack the in-OS compiler          -> dist/equinox/tools/mtcc.mrp
make tools      # stage tool .c sources            -> dist/equinox/tools/
make libc       # stage libc sources + headers     -> dist/equinox/libc/
make pack       # pack mrp_user/*.cpp programs     -> .mrp
make bootimg    # grub-mkimage boot.img + core.img -> RAMFS /equinox/bootimg
make diskimg    # 64 MB FAT32 test disk            -> dist/disk.img
make img        # EQUINOXBASE install image        -> dist/equinox.img
make test       # host-side mtcc test harness (no QEMU)
```

`make` alone produces a complete ISO: kernel + RAMFS modules (tool and
libc **sources**, mtcc.mrp, boot images). `dist/` mirrors the RAMFS
layout inside the OS. Note that the ISO ships **sources, not binaries**
— the userland is compiled *inside the OS* on first boot (see
[SELF_HOSTING.md](SELF_HOSTING.md)).

## 3. Running in QEMU

### 3a. Fastest way — make targets

```sh
make run         # ISO + user-mode networking (httpd reachable at localhost:8080)
make run-disk    # + 64 MB FAT32 disk as primary master (auto-mounted at /mnt)
make run-img     # + dist/equinox.img (EQUINOXBASE label -> promoted to /)
make run-e1000   # install image + Intel E1000 PCI NIC instead of the NE2000
make run-ahci    # disk hangs off an ich9-AHCI controller (SATA) instead of PIIX IDE
```

### 3b. Manual QEMU — ISO only

```sh
qemu-system-i386 -m 256 -cdrom dist/equinox.iso \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

### 3c. Manual QEMU — ISO + FAT32 disk (recommended)

```sh
qemu-system-i386 -m 256 -boot order=d -cdrom dist/equinox.iso \
    -drive file=dist/disk.img,format=raw,if=ide,index=0,media=disk \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

### 3d. Manual QEMU — SATA via AHCI and the E1000 NIC

```sh
# SATA disk on an AHCI controller (QEMU ich9-ahci, 8086:2922):
# the disk lands on ahci.0 port 0 -> block slot 4 -> Qfs name "hde"
qemu-system-i386 -m 256 -boot order=d -cdrom dist/equinox.iso \
    -device ahci,id=ahci \
    -drive file=dist/disk.img,format=raw,if=none,id=hd0 \
    -device ide-hd,drive=hd0,bus=ahci.0 \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9

# Intel PRO/1000 PCI NIC (8086:100E) instead of the NE2000:
qemu-system-i386 -m 256 -cdrom dist/equinox.iso \
    -netdev user,id=net0 -device e1000,netdev=net0
```

### Option reference

| Option | Purpose |
| --- | --- |
| `-m 256` | Guest RAM — **256 MB recommended** (kernel heap + module staging + GUI arena; 64 MB works but is tight) |
| `-cdrom dist/equinox.iso` | Boot the Equinox ISO |
| `-boot order=d` | Boot from CD-ROM first (disk variants) |
| `-drive …,if=ide,index=0` | IDE primary master; the first FAT32 partition auto-mounts at `/mnt` |
| `-device ahci,id=ahci` + `-device ide-hd,bus=ahci.0` | Attach the disk to the AHCI (SATA) controller instead — handled by the AHCI driver + block layer |
| `-netdev user,id=net0` | slirp user-mode networking: guest `10.0.2.15`, gateway/host `10.0.2.2`, DNS `10.0.2.3` (DHCP runs at boot) |
| `hostfwd=tcp::8080-:80` | The guest `httpd` is reachable from the host browser: `http://localhost:8080/` |
| `-device ne2k_isa,…,iobase=0x300,irq=9` | NE2000 ISA NIC (default driver) |
| `-device e1000,netdev=net0` | Intel PRO/1000 PCI NIC (E1000 driver, `8086:100E`) |

### 3e. Useful variations

```sh
# Mirror the debug console (COM1) to a file — the primary debugging aid:
qemu-system-i386 -m 256 -cdrom dist/equinox.iso -serial file:serial.log \
    -netdev user,id=net0 -device ne2k_isa,netdev=net0,iobase=0x300,irq=9

# QEMU monitor (optional): add -monitor stdio, then "info registers",
# "xp /8wx addr", etc.

# Pure filesystem/multitasking test, no networking:
qemu-system-i386 -m 256 -cdrom dist/equinox.iso
```

Note: QEMU user-mode networking does not forward ICMP — use
`tcpping <host> [port]` from inside the OS instead of `ping`.

## 4. First steps after boot

The Linux-style `[ OK ]` boot log scrolls by (mirrored to COM1), then
the shell prompt appears:

```
root::users / $
```

**Step 1 — build the userland** (the ISO carries sources, not binaries):

```
root::users / $ equinoxinstall
```

The 4-phase wizard builds every shipped tool with the in-OS compiler —
pick `0)` *build in place* to skip the disk dance and just compile into
RAMFS. Progress looks like (33 jobs: 20 tool sources + 13 libc modules,
two parallel mtcc tasks):

```
[4/4] build userland from /equinox
[info] : kompilator in-OS siap: mtcc.mrp (352104 bytes)
[info] : libc  — 13 modul  di /equinox/libc (check-compile)
[info] : tools — 20 sumber di /equinox/tools
[info] : parallel pool: 2 compiler thread(s), 33 job(s) total
[info] : [ 1/33] T1 libc/convert.c   -> convert.mrp    4120 B  module verified
[info] : [14/33] T2 tools/cat.c      -> cat.mrp       35210 B  installed
       ...
summary — libc 13/13 verified, tools 20/20, 0 failed
parallel build: 33 job(s) on 2 thread(s), wall 11.8 s
```

**Step 2 — install the bash package** (the classic coreutils are *not*
bundled with the ISO; they arrive as a package, see
[PACKAGES.md](PACKAGES.md)):

```
root::users / $ eggkg update && eggkg install bash -y
```

**Step 3 — explore:**

```sh
ls /mnt                      # with a disk attached (run-disk / run-ahci)
grep -n printf /test/hello.c | tr a-z A-Z    # pipes work out of the box
mtcc /test/hello.c           # compile + run C, live, in the OS
lspci                        # PCI table: PIIX/AHCI/E1000 as applicable
ps                           # task table; F1/F2 switch consoles
mget https://raw.githubusercontent.com/torvalds/linux/master/README
doom -iwad /mnt/doom1.wad    # DOOM straight off the FAT32 disk
```

## 5. Controls

| Key | Function |
| --- | --- |
| `F1` / `F2` | Switch virtual console (each has its own task/shell) |
| PS/2 keyboard | Shell, editor, games |
| Mouse (PS/2) | LVGL desktop + DOOM |
| `Ctrl+Alt+G` (QEMU) | Grab/release mouse into the guest |

## 6. Troubleshooting

| Symptom | Cause & fix |
| --- | --- |
| `boot failed: could not read from CDROM` | `grub-mkrescue` used the wrong platform — make sure `grub-pc-bin` is installed (`i386-pc` modules); the makefile passes `-d` automatically when the folder exists |
| Link error on `-lgcc` | 32-bit `libgcc.a` missing — install `gcc-multilib` or set `LIBGCC32_DIR=.../32` |
| `missing separator` from make | A text editor converted recipe TABs to spaces — restore real TABs (recipes must start with TAB) |
| Boot hang / black screen | Check `serial.log` (every boot stage reports to COM1); try `-vga std` |
| No network | Keep `-netdev`/`-device` on one line; check the DHCP banner at boot; for E1000 use `make run-e1000` or `-device e1000`; verify `[net] driver` in `system.ecf` (see [CONFIGURATION.md](CONFIGURATION.md)) |
| Tools print "Unknown command" | Run `equinoxinstall` first — a fresh boot carries sources, not binaries (that is the self-hosting design) |
| `ls`/`cat`/`cp` behave like the simple builtins | The full coreutils arrive with `eggkg install bash`; until then the shell builtins/aliases cover the basics |
| Disk not visible | Plain IDE: `/mnt` after `run-disk`. AHCI/SATA: `make run-ahci`, disk appears as `hde` — check with `Qfs -list-disk` |

## 7. Automated test suites (optional)

```sh
make test            # host-side mtcc harness (no QEMU)
make test-e1000      # E1000 bring-up: lspci 8086:100E, DHCP, mget over PCI NIC
make test-ahci       # SATA via AHCI: detect, mount, byte-exact I/O, reboot persistence
make test-wizard     # interactive equinoxinstall on a blank disk -> boots as /
make test-ecf        # set builtin, eqgui editor, system.ecf steers net driver
make test-install    # manual install path (Qfs, mount, copy, set -x scripts)
make test-img        # EQUINOXBASE base volume promoted to /, survives reboot
make test-mnt        # equinoxinstall onto a plain FAT32 volume mounted at /mnt

python3 scripts/regression_task3.py      # v0.3 core suite (boot, tools, gfx, libc, soak)
python3 scripts/regression_task2.py      # processes/memory/ELF/pipe
python3 scripts/regression_v03.py        # syscalls + tools
python3 scripts/regression_multitask.py  # canvas/games/F1-F2/DOOM
```

All suites must PASS on a release ISO — 101 core checks plus the
dedicated driver/installer suites.
