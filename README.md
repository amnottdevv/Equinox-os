# Equinox OS

<p align="center">
  <img src="https://img.shields.io/badge/architecture-i686%20%2F%2032--bit-6f42c1?style=for-the-badge" alt="i686 32-bit" />
  <img src="https://img.shields.io/badge/kernel-monolithic-8A4FFF?style=for-the-badge" alt="Monolithic kernel" />
  <img src="https://img.shields.io/badge/boot-GRUB%20Multiboot-2D2D2D?style=for-the-badge" alt="GRUB Multiboot" />
  <img src="https://img.shields.io/badge/language-C%2FC%2B%2B%2FASM-00599C?style=for-the-badge" alt="C C++ Assembly" />
</p>

<p align="center">
  <img src="https://img.shields.io/github/license/equinoxosproject/Equinox-os?style=flat-square" alt="License" />
  <img src="https://img.shields.io/github/repo-size/equinoxosproject/Equinox-os?style=flat-square" alt="Repository size" />
  <img src="https://img.shields.io/github/commit-activity/y/equinoxosproject/Equinox-os?style=flat-square" alt="Commit activity" />
  <img src="https://img.shields.io/github/last-commit/equinoxosproject/Equinox-os?style=flat-square" alt="Last commit" />
  <a href="https://github.com/equinoxosproject/Equinox-os/actions/workflows/sync-release-source.yaml"><img src="https://github.com/equinoxosproject/Equinox-os/actions/workflows/sync-release-source.yaml/badge.svg" alt="Release source sync" /></a>
</p>

<p align="center">
  <strong>A small, experimental 32-bit x86 operating system built from the ground up.</strong><br />
  Boot into a graphical framebuffer console, use a real shell, run ring-3 programs,
  browse a FAT32 disk, download files over HTTP(S), and launch DOOM inside QEMU.
</p>

---

## Overview

**Equinox OS** is a freestanding hobby operating system for i686 PCs. It uses a custom kernel written primarily in C++ and C, with an Assembly entry point, and is booted through GRUB Multiboot.

The project is intentionally hands-on: rather than hiding behind a large runtime, Equinox implements and integrates the pieces that make a usable small OS interesting:

- Multiboot handoff and early boot memory staging
- 32-bit protected mode, GDT, TSS, paging, and ring 3 execution
- A kernel shell with history, path handling, diagnostics, and self-tests
- A RAM filesystem populated from GRUB modules
- A write-through FAT32 subsystem backed by an ATA/IDE PIO driver
- A VESA framebuffer console with VGA text fallback
- An i386-friendly networking stack using lwIP and an NE2000 ISA device
- An HTTP/HTTPS client, HTTP server, DNS, DHCP, ICMP, and TCP tools
- MRP flat-binary user programs and an in-OS C compiler named `mtcc`
- LVGL-based graphical applications
- Games, demos, and a ring-3 DOOM port

> **Status:** experimental hobby OS. It is designed for learning, experimentation, and emulation—not production hardware or security-critical workloads.

## Current release

The repository currently contains the **v0.2 Beta** feature set, including the disk subsystem and persistent FAT32 writes.

- Release: [v0.2-Beta](https://github.com/equinoxosproject/Equinox-os/releases/tag/v0.2-Beta)
- Source archive: attached to the release as the `SOURCE.zip` artifact
- Target architecture: 32-bit x86 / i686
- Primary runtime: QEMU with GRUB and an emulated NE2000/IDE device

## Highlights

### Boot and graphics

- GRUB Multiboot kernel handoff
- Custom Assembly entry point in [`src/boot/start.asm`](src/boot/start.asm)
- Early relocation of Multiboot modules into a dedicated staging area
- Explicit `.bss` clearing before entering C++ code
- Flat GDT with kernel code/data, user code/data, and a runtime-patched TSS descriptor
- GRUB VBE framebuffer request with a Bochs/QEMU DISPI mode override when available
- 1366×768×32 target mode, with hardware-dependent fallback
- VESA framebuffer console with automatic VGA text-mode fallback
- Purple/white Equinox boot identity and structured `[ OK ]` initialization log

### Kernel and userland

- Monolithic kernel architecture
- PIT timer, IDT, PIC remapping, PS/2 keyboard, and PS/2 mouse support
- Paging with supervisor/user page separation
- TSS-based transitions from ring 3 back to ring 0
- `int 0x80` syscall interface
- MRP flat-binary loader for user programs
- In-kernel shell with command history and canonicalized paths
- User home directory at `/user` when the standard RAMFS layout is available
- Built-in editor, file manager, system diagnostics, calculator, color tools, and self-tests

### Filesystems and storage

Equinox has two complementary storage models:

1. **RAMFS** — populated at boot from GRUB Multiboot modules. Large module-backed files such as a WAD can be exposed without copying them into the small kernel heap.
2. **FAT32** — an ATA-backed persistent filesystem mounted at `/mnt` when a compatible disk is present.

The FAT32 implementation includes:

- MBR partition probing
- FAT32 BPB and FSInfo validation
- FAT12/FAT16 shape rejection
- 512-byte sector support
- Lazy directory mirroring into the common `fs_node` tree
- Case-insensitive matching for disk-backed names
- Long file names with 8.3 alias generation and collision handling
- Read, create, overwrite, truncate, delete, and directory creation
- Write-through updates for FAT copies, directory entries, and FSInfo
- Directory growth across clusters
- Rollback handling for several mid-write failures
- A bounded sector-granular write path
- An 8 MiB file-content arena outside the 2 MiB kernel heap

This common filesystem abstraction allows shell commands, syscalls, the GUI file manager, and applications such as DOOM to work with RAMFS and FAT32-backed files through the same tree interface.

### Networking

The networking layer uses **lwIP 2.1.3** in `NO_SYS` cooperative-polling mode and an emulated NE2000 ISA NIC. Available functionality includes:

- DHCP
- DNS resolution
- ICMP ping
- TCP connectivity probing
- HTTP downloads with redirects
- A small HTTP server on port 80
- TLS 1.2 client support for `https://` through vendored BearSSL 0.6

The HTTPS client uses portable BearSSL implementations because the kernel cannot assume normal hosted OS FPU/SSE state. Certificate validation uses embedded compatible trust anchors where possible and reports an explicit warning when a server chain cannot be fully verified by the supported i386 path.

### Applications and demos

- `mtcc` — compile and run C source inside the OS
- MRP user programs and syscall demonstrations
- Snake, Breakout, and Pong through the small Libgame framework
- LVGL demo, file manager, settings, and code editor
- DOOM through the `doomgeneric` port, running as a ring-3 `.mrp` program

## Architecture at a glance

```text
                    +-----------------------------+
                    |       GRUB / Multiboot      |
                    +--------------+--------------+
                                   |
                    +--------------v--------------+
                    | boot/start.asm               |
                    | GDT, stack, module staging  |
                    +--------------+--------------+
                                   |
                    +--------------v--------------+
                    | Equinox kernel               |
                    | IDT/PIC | timer | paging     |
                    | TSS/ring 3 | shell | syscalls |
                    +------+----------+-------------+
                           |          |
             +-------------v--+   +---v----------------+
             | RAMFS / MRP     |   | Device + network   |
             | modules         |   | ATA/FAT32, NE2000 |
             +-----------------+   | lwIP, BearSSL    |
                                   +---------+----------+
                                             |
                                   +---------v----------+
                                   | User programs      |
                                   | mtcc, games, DOOM  |
                                   +--------------------+
```

### Memory layout concepts

The early boot code relocates Multiboot modules before clearing `.bss`, avoiding overlap between GRUB-loaded modules and kernel memory. The project reserves separate regions for the kernel heap, MRP user arena, user stack/trampoline, module staging, and the FAT32 file-content arena.

These addresses are part of the current QEMU-oriented memory design. If you change the kernel layout, linker script, paging map, or module sizes, review [`src/boot/start.asm`](src/boot/start.asm), [`src/linker.ld`](src/linker.ld), and the paging implementation together.

## Quick start

### Host requirements

The build is primarily documented for Linux. Install:

- `gcc` and `g++` with 32-bit support, or an `i686-elf` cross compiler
- 32-bit `libgcc.a` when using host compiler mode
- NASM
- GRUB utilities with `i386-pc` modules
- `xorriso`
- `mtools`
- Python 3
- QEMU with `qemu-system-i386`

On Debian/Ubuntu-like systems, a typical starting point is:

```sh
sudo apt install build-essential gcc-multilib g++-multilib \
  nasm grub-pc-bin grub-common xorriso mtools python3 qemu-system-x86
```

Package names can differ between distributions. A cross compiler is also supported and is preferred when you want a more isolated freestanding toolchain.

### Build the ISO

```sh
cd src
make
```

The main artifacts are generated under `src/dist/`:

```text
src/dist/kernel.elf
src/dist/equinox.iso
```

### Run in QEMU

```sh
cd src
make run
```

The default network configuration emulates an NE2000 device and forwards the guest HTTP server to the host:

```text
http://localhost:8080/
```

Equivalent manual invocation:

```sh
qemu-system-i386 \
  -m 64 \
  -cdrom dist/equinox.iso \
  -netdev user,id=net0,hostfwd=tcp::8080-:80 \
  -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

### Run with a FAT32 disk

Build the demo disk and boot with it attached:

```sh
cd src
make diskimg
make run-disk
```

The demo image is a 64 MiB FAT32 disk with an MBR and sample files. When detected, it is mounted at `/mnt`.

Useful commands inside the OS:

```text
ls /mnt
cat /mnt/README.TXT
diskinfo
cd /mnt
save hello.txt << "hello from Equinox OS"
mget http://10.0.2.2:8022/data.bin
doom -iwad /mnt/doom1.wad
```

## Build targets

Run these from `src/`:

| Command | Purpose |
| --- | --- |
| `make` | Build the kernel and bootable ISO |
| `make pack` | Compile and pack user tools, games, and test files |
| `make mtcc` | Pack the root `mtcc.c` compiler into an MRP program |
| `make doom` | Build and pack the DOOM MRP program |
| `make diskimg` | Create the 64 MiB FAT32 demo disk |
| `make run` | Boot the ISO with QEMU user-mode networking |
| `make run-disk` | Boot ISO plus the FAT32 disk image |
| `make test` | Run host-side compiler tests |
| `make test-fat32` | Run FAT32 QEMU tests and host-side mtools checks |
| `make test-doom-disk` | Verify DOOM loading from the FAT32 disk |
| `make iso` | Rebuild only the ISO target |
| `make clean` | Remove generated build, distribution, and ISO directories |

### Toolchain selection

The Makefile checks toolchains in this order:

1. `i686-elf-g++.exe`
2. `i686-elf-g++`
3. Host `g++ -m32`

For host compiler mode, configure paths when your distribution does not expose the required 32-bit files in standard locations:

```sh
make \
  LIBGCC32_DIR="$HOME/tools/root/usr/lib/gcc/i686-linux-gnu/14" \
  INC32_DIR="$HOME/tools/inc32/usr/include/i386-linux-gnu"
```

## Shell command reference

The shell is part of the kernel and provides both everyday filesystem commands and low-level diagnostics.

### Files and navigation

| Command | Description |
| --- | --- |
| `ls`, `ls -l` | List the current directory |
| `pwd` | Print the working directory |
| `cd <path>` | Change directory, including `.` and `..` handling |
| `tree` | Print a directory tree |
| `cat <file>` | Print a file byte-by-byte |
| `xxd <file> [n]` | Hex dump the first `n` bytes |
| `cfile <name>` | Create an empty file |
| `ccfile <name> << "text"` | Create a file with text |
| `save <name> << "text"` | Create or overwrite a file |
| `cdir <name>` | Create a directory |
| `rm <name>` | Delete a file or empty directory |
| `edit <file>` | Open the built-in editor |
| `mount` / `umount` | Attach or detach the FAT32 volume at `/mnt` |
| `diskinfo` | Show ATA and FAT32 layout/cache information |

### Programs and debugging

| Command | Description |
| --- | --- |
| `run <program.mrp> [args]` | Run an MRP program |
| `./program.mrp [args]` | Run a relative MRP path |
| `mtcc <file.c>` | Compile and run C in the guest |
| `doom [args]` | Launch DOOM; requires `doom1.wad` |
| `info` | Show OS and heap information |
| `memmap` | Print memory map and ring-3 status |
| `ring` | Show current privilege level |
| `syscalls` | List the syscall ABI |
| `sctest` | Exercise the `int 0x80` syscall path |
| `panic [message]` | Deliberately exercise kernel panic handling |

### Network

| Command | Description |
| --- | --- |
| `ifconfig` | Show network interface information |
| `ping <host>` | Send ICMP echo requests |
| `tcpping <host> [port]` | Test TCP connectivity and RTT |
| `dns <hostname>` | Resolve a hostname |
| `mget <url> [-port n]` | Download an HTTP(S) resource |
| `httpd` | Show/start the in-OS HTTP server |

`mget` supports `http://`, `https://`, redirects, and an explicit `-port` override. The priority is:

```text
-port option > port in URL > scheme default
```

HTTP defaults to port 80 and HTTPS to port 443.

## Repository layout

```text
src/
├── boot/                 Multiboot entry point and GRUB configuration
├── kernel/
│   ├── kernel.cpp        Boot sequence and kernel shell
│   ├── library/          Memory, filesystem, syscalls, drivers, UI, libc
│   │   ├── fs_ram.cpp     RAMFS and RAMFS/FAT32 bridge
│   │   ├── fs_fat32.cpp  FAT32 mount, cache, reads, and lazy mirror
│   │   └── fs_fat32_write.cpp  FAT32 write-through operations
│   ├── net/              lwIP glue, NE2000, HTTP server, TLS client
│   └── gui/              LVGL integration and graphical applications
├── mrp_user/             Ring-3 tools and MRP packer
├── games/                MRP games using Libgame
├── Libgame/              Small game framework
├── doomgeneric/          DOOM portability layer and Equinox port
├── test/                 C samples and test inputs
├── third_party/          lwIP, BearSSL, LVGL, and other dependencies
├── scripts/              Build, packaging, image, and QEMU test scripts
├── mtcc.c                Canonical source for the in-OS C compiler
├── linker.ld             Kernel linker script
└── makefile              Main build orchestration
```

The release-source synchronization workflow keeps the source archive from a published or edited GitHub Release available under the repository's `src/` tree. Generated build output should remain separate from source files.

## Testing and verification

The project contains both host-side and guest-side checks. The FAT32 test flow is especially important because it exercises the boundary between an emulated disk, the kernel's write-through implementation, and an independent host-side filesystem view.

Recommended validation loop:

```sh
cd src
make clean
make pack
make diskimg
make
make test
make test-fat32
```

For changes involving ring 3, paging, syscalls, or executable loading, also run the QEMU smoke tests and manually verify:

```text
ring
memmap
sctest
run hello.mrp
```

For changes involving boot modules or GRUB configuration, verify that the boot log lists the expected files and that `.mrp` programs are available from the shell.

## Development notes

### Freestanding constraints

This is not a hosted C++ application. Code runs without a normal operating-system runtime, standard library, process model, or libc. Be careful with:

- stack usage, especially in interrupt-sensitive code
- implicit compiler-generated runtime calls
- floating-point/SSE instructions in kernel and interrupt paths
- ownership of heap memory versus zero-copy module memory
- 32-bit pointer assumptions
- direct hardware I/O and interrupt context
- cache flushing and on-disk consistency

The Makefile applies `-mgeneral-regs-only` to selected interrupt, paging, syscall, networking, and other sensitive compilation units. Changes to those units should be reviewed with the same constraint in mind.

### Safe change workflow

1. Make one focused change.
2. Rebuild from a clean tree when changing build, linker, boot, or memory code.
3. Run the relevant host tests.
4. Boot under QEMU and inspect the complete boot log.
5. Exercise the affected shell command or syscall.
6. For storage changes, reboot with the same disk image and verify persistence.
7. For userland changes, test both successful execution and an intentional fault path.

## Third-party software and licensing

- [lwIP 2.1.3](https://savannah.nongnu.org/projects/lwip/) — BSD-3-Clause
- [BearSSL 0.6](https://bearssl.org/) — MIT
- [LVGL](https://lvgl.io/) — MIT
- [doomgeneric](https://github.com/ozkl/doomgeneric) — see its included license and documentation
- QEMU/SeaBIOS — used for emulation and testing

The shareware DOOM WAD is not automatically downloaded or bundled by the build system. Provide `doom1.wad` yourself where the relevant scripts expect it, and review the applicable licensing terms before distributing artifacts.

## Contributing

Contributions are welcome, especially in these areas:

- hardware abstraction and additional emulated devices
- filesystem robustness and recovery tooling
- syscall documentation and userland examples
- QEMU regression tests
- build reproducibility and cross-toolchain support
- shell usability and documentation
- performance measurements for ATA, FAT32, networking, and framebuffer paths

When opening an issue or pull request, include:

- host operating system and toolchain versions
- QEMU version and command line
- exact build target used
- complete boot log or failure output
- whether the issue reproduces with ISO-only and disk-backed boots
- a minimal reproduction when possible

## License

Equinox OS code is provided under the repository's [MIT License](LICENSE), except for third-party components and assets which retain their respective licenses.

---

<p align="center">
  Built to understand the machine one subsystem at a time.<br />
  <strong>Equinox OS</strong> — kernel, shell, disk, network, and games in one small experiment.
</p>
