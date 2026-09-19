# Equinox OS

<p align="center">
  <strong>A small, experimental 32-bit x86 operating system built from the ground up.</strong><br />
  Boot through GRUB, use a framebuffer console and shell, run ring-3 programs,
  access FAT32 storage and networking, and launch DOOM inside QEMU.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/architecture-i686%20%2F%2032--bit-6f42c1?style=for-the-badge" alt="i686 32-bit" />
  <img src="https://img.shields.io/badge/kernel-monolithic-8A4FFF?style=for-the-badge" alt="Monolithic kernel" />
  <img src="https://img.shields.io/badge/boot-GRUB%20Multiboot-2D2D2D?style=for-the-badge" alt="GRUB Multiboot" />
</p>

## Overview

Equinox OS is a freestanding hobby operating system for i686 PCs. It includes a custom monolithic kernel, GRUB Multiboot boot flow, RAMFS, FAT32 read/write support, a VESA framebuffer console, ring-3 user programs, an in-OS C compiler, networking, LVGL applications, games, and a ring-3 DOOM port.

> **Status:** experimental hobby OS for learning, experimentation, and emulation. It is not intended for production or security-critical workloads.

## System requirements

- **Architecture:** 32-bit x86 / i686
- **Minimum RAM while booting:** 24 MB
- **Recommended RAM:** 64 MB
- **Recommended emulator:** QEMU with `qemu-system-i386`
- **Boot method:** GRUB Multiboot

The minimum RAM configuration is intended for booting and core shell functionality. More memory may be required for large RAMFS modules, graphical applications, networking, FAT32 workloads, or DOOM.

## Highlights

- GRUB Multiboot kernel handoff and early module staging
- 32-bit protected mode, GDT, TSS, paging, and ring-3 execution
- VESA framebuffer console with VGA text fallback
- RAMFS populated from GRUB modules
- ATA/IDE PIO and FAT32 read/write support, mounted at `/mnt`
- lwIP networking with NE2000, DHCP, DNS, ICMP, TCP, HTTP and HTTPS
- MRP flat-binary user programs and the `mtcc` in-OS C compiler
- LVGL GUI, file manager, settings, and code editor
- Snake, Breakout, Pong, and ring-3 DOOM

## Quick start

### Host requirements

On Linux, install GCC/G++ with 32-bit support, NASM, GRUB utilities, `xorriso`, `mtools`, Python 3, and QEMU:

```sh
sudo apt install build-essential gcc-multilib g++-multilib \
  nasm grub-pc-bin grub-common xorriso mtools python3 qemu-system-x86
```

### Build and run

```sh
cd src
make
make run
```

The generated ISO is `src/dist/equinox.iso`.

To build and run with the demo FAT32 disk:

```sh
cd src
make diskimg
make run-disk
```

Manual QEMU invocation:

```sh
qemu-system-i386 \
  -m 64 \
  -cdrom dist/equinox.iso \
  -netdev user,id=net0,hostfwd=tcp::8080-:80 \
  -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

## Shell command reference

The shell supports command history with the **Up/Down** arrow keys. The default prompt starts in `/user` when that directory exists. Commands with paths support relative paths, absolute paths, `.`, and `..`.

### General and display

| Command | Description |
| --- | --- |
| `help` | Show the complete command list |
| `clear` | Clear the screen |
| `echo` | Enter echo mode and print a message |
| `info` | Show OS version, build information, RAM, and heap usage |
| `cpu` | Show the CPU vendor string |
| `color <fg> [bg]` | Set foreground and optional background color |
| `color list` | List available colors |
| `color reset` | Restore white text on a black background |
| `clock` | Show the real-time clock and uptime |
| `reboot` | Restart the system |

### Filesystem and navigation

| Command | Description |
| --- | --- |
| `ls` | List the current directory |
| `ls -l` | List directory contents with details |
| `pwd` | Print the working directory |
| `cd <path>` | Change directory |
| `tree` | Display the directory tree |
| `cdir <name>` | Create a directory |
| `cfile <name>` | Create an empty file |
| `ccfile <name> << "content"` | Create a file with text |
| `save <name> << "content"` | Create or overwrite a file |
| `cat <name>` | Display file contents |
| `xxd <file> [n]` | Hex dump the first `n` bytes; default is 64 |
| `rm <name>` | Delete a file or empty directory |
| `rmdir <name>` | Delete an empty directory |
| `edit <file>` | Open the built-in text editor |
| `mount` | Mount the FAT32 volume at `/mnt` |
| `umount` | Unmount the FAT32 volume |
| `diskinfo` | Show ATA drives and FAT32 volume details |

### Programs, compiler, and GUI

| Command | Description |
| --- | --- |
| `run <path.mrp> [args]` | Run an MRP program with arguments |
| `./name.mrp [args]` | Run a local MRP program; `.mrp` may be auto-appended |
| `<tool> [args]` | Run a tool found in `/`, `/bin`, `/equinox/tools`, or `/equinox/games` |
| `mtcc <file.c>` | Compile and run C from any directory |
| `mtcc --debug <file.c>` | Compile and run with verbose compiler output |
| `mtcc -c <file.c>` | Compile to `<file>.mrp`, then run it |
| `settings` | Open the system settings UI |
| `fm [path]` | Open the graphical file manager |
| `gui` | Launch the LVGL GUI demo when LVGL is available |
| `doom [args]` | Run DOOM; requires `doom1.wad` |

Examples:

```text
run hello.mrp
./hello
mtcc /test/hello.c
mtcc --debug main.c
mtcc -c test.c
fm /user
 doom -warp 1
```

### Diagnostics and tests

| Command | Description |
| --- | --- |
| `calc <a> <op> <b>` | Calculate using `+`, `-`, `*`, or `/` |
| `hex <num>` | Convert decimal to hexadecimal |
| `dec <hex>` | Convert hexadecimal to decimal |
| `mem <addr>` | Read a 32-bit value from a memory address |
| `testconv` | Test integer conversion helpers |
| `tick` | Show the timer tick count |
| `sleep <ms>` | Sleep for a number of milliseconds |
| `malloc` | Show heap statistics |
| `alloc <bytes>` | Allocate memory for a heap test |
| `free <addr>` | Exercise the dummy free path |
| `testvector` | Test the dynamic vector implementation |
| `random` | Test the random-number generator |
| `math` | Test `sin`, `cos`, and `tan` |
| `teststr` | Test string splitting and formatting helpers |
| `mouse` | Show PS/2 mouse driver diagnostics |
| `ringstats` | Show keyboard, mouse, and audio ring-buffer statistics |
| `ring` | Show the current CPU privilege level, ring 0 or ring 3 |
| `memmap` | Show the memory map and ring-3 status |
| `syscalls` | List the `int 0x80` syscall ABI |
| `sctest` | Test the syscall layer from the shell |
| `panic [message]` | Trigger a kernel panic for testing |

### System utility commands

| Command | Description |
| --- | --- |
| `sys cwd` | Show the syscall working directory |
| `sys mkdir <name>` | Create a directory through the syscall utility layer |
| `sys touch <name>` | Create a file through the syscall utility layer |
| `sys rm <name>` | Delete a file through the syscall utility layer |

### Audio

| Command | Description |
| --- | --- |
| `beep` | Play a 440 Hz beep for 500 ms |
| `song` | Queue “Twinkle Twinkle” for background playback |

### Networking

| Command | Description |
| --- | --- |
| `ifconfig` | Show network interface information |
| `netdbg` | Probe and debug the NE2000 network device |
| `ping <host>` | Send four ICMP pings to an IP or hostname |
| `tcpping <host> [port]` | Test TCP connectivity; useful with QEMU user networking |
| `dns <hostname>` | Resolve a hostname through the configured DNS server |
| `mget <url> [-port <n>]` | Download an HTTP/HTTPS file into the current directory |
| `httpd` | Show or start the in-OS HTTP server on port 80 |

`mget` accepts `http://`, `https://`, and host/path URLs. The port priority is:

```text
-port option > port in URL > scheme default
```

HTTP uses port 80 and HTTPS uses port 443. In QEMU, forward the guest HTTP server with `hostfwd=tcp::8080-:80`, then browse to `http://localhost:8080/`.

## Build targets

Run these commands from `src/`:

| Command | Purpose |
| --- | --- |
| `make` | Build the kernel and bootable ISO |
| `make pack` | Pack user tools, games, and test files |
| `make mtcc` | Build and pack the in-OS C compiler |
| `make doom` | Build and pack the DOOM MRP program |
| `make diskimg` | Create the 64 MiB FAT32 demo disk |
| `make run` | Boot the ISO with QEMU networking |
| `make run-disk` | Boot the ISO with the FAT32 disk attached |
| `make test` | Run host-side compiler tests |
| `make test-fat32` | Run FAT32 QEMU and host-side checks |
| `make test-doom-disk` | Verify DOOM loading from the FAT32 disk |
| `make iso` | Rebuild only the ISO |
| `make clean` | Remove generated build output |

## Repository layout

```text
src/
├── boot/          Multiboot entry point and GRUB configuration
├── kernel/        Kernel, shell, drivers, memory, filesystem, UI, and syscalls
├── mrp_user/      Ring-3 tools and MRP programs
├── games/         Snake, Breakout, and Pong
├── Libgame/       Small game framework
├── doomgeneric/   DOOM portability layer and Equinox port
├── test/          C samples and test inputs
├── third_party/   lwIP, BearSSL, LVGL, and other dependencies
├── scripts/       Build, packaging, image, and QEMU test scripts
├── mtcc.c         In-OS C compiler source
├── linker.ld      Kernel linker script
└── makefile       Main build orchestration
```

## Testing

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

For ring-3, paging, syscall, or executable-loader changes, also run:

```text
ring
memmap
sctest
run hello.mrp
```

## License

Equinox OS code is provided under the repository's [MIT License](LICENSE), except for third-party components and assets, which retain their respective licenses.
