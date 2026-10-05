<p align="center">
  <img src="image/Equinox.png" alt="Equinox OS" width="640">
</p>

# Equinox OS

**A 32-bit x86 hobby operating system that compiles its own userland — inside the OS.**

Equinox boots via GRUB into a VESA 1360×768 framebuffer, runs real ring-3
processes with demand paging, talks to the network over a bundled
ls lwIP stack, persists on FAT32 across both PATA (ATA PIO) and SATA (AHCI)
disks — and ships its own C compiler, `mtcc`, driven by the `equinoxinstall`
wizard or the `eggkg` package manager.

!!! tip "Status"
    **0.4 Beta** — regression-tested in QEMU (101 core checks plus dedicated
    suites for the installer, AHCI, E1000 and `.ecf`).

---

## Quick start

=== "1. Build the ISO"

    ```sh
    make
    ```

=== "2. Boot it in QEMU"

    ```sh
    make run        # 256 MB guest RAM, NE2000 NIC, httpd on :8080
    ```

=== "3. Use it"

    ```sh
    equinoxinstall          # compile the userland inside the OS
    eggkg update
    eggkg install bash -y   # coreutils as a package
    ```

---

## Getting the basics

| Document | What it covers |
| --- | --- |
| [Getting Started](GETTING_STARTED.md) | Prerequisites, building, every QEMU run variant, troubleshooting |
| [Installation](INSTALL.md) | Installing to disk — the `equinoxinstall` wizard or fully manual (`Qfs`, `mount`, `copy`) |
| [Architecture](ARCHITECTURE.md) | The full technical map: boot → kernel → userland |
| [Release Notes](RELEASE_NOTES.md) | What changed in 0.4 Beta, known limitations, roadmap |

## Configuration & the shell

| Document | What it covers |
| --- | --- |
| [Configuration](CONFIGURATION.md) | `.ecf`, the `set` builtin, eqshell scripts |
| [`set` builtin](SET.md) | Full reference for every `set` flag + the C API |
| [`system.ecf`](SYSTEM_ECF.md) | Key-by-key schema and boot-time resolution |
| [`.ecf` format](ECF.md) | Grammar, parse/merge rules, limits, validation |
| [eqshell scripts](EQSHELL.md) | `.es` files, `Log=True`, `/eqshell.log` transcripts |
| [Commands](COMMANDS.md) | The complete shell builtin & userland command reference |

## Storage & boot

| Document | What it covers |
| --- | --- |
| [Booting](BOOTING.md) | GRUB images, module staging, `kernel_main` order |
| [Qfs](QFS.md) | Disk tool: `-list-disk`, `-format fat32`, `-install-boot` |
| [FAT32](FAT32.md) | The on-disk filesystem internals |
| [Drivers](DRIVERS.md) | Block layer, ATA PIO, AHCI SATA, NIC drivers |

## Networking

| Document | What it covers |
| --- | --- |
| [Networking](NETWORKING.md) | lwIP 2.1.3, DHCP/DNS, `mget` + TLS, `httpd`, NE2000 vs E1000 |

## Kernel internals

| Document | What it covers |
| --- | --- |
| [Memory](MEMORY.md) | Memory map, demand paging, DMA buffers |
| [Multitasking](MULTITASKING.md) | Tasks, the 100 Hz scheduler, consoles, pipes |
| [System Calls](SYSCALLS.md) | The full `int 0x80` interface, #1–#54 |
| [Graphics & Desktop](GUI.md) | VESA framebuffer, LVGL apps, EquiX/ThorVG |

## Self-hosting

| Document | What it covers |
| --- | --- |
| [Self Hosting](SELF_HOSTING.md) | `mtcc`, MRP1 executables, ruf v3 recipes |
| [MRP1 format](MRP.md) | The `.mrp` header layout, checksum, loader |
| [Packages](PACKAGES.md) | `eggkg` reference, package repo format, writing packages |

---

## At a glance

| Component | Technology |
| --- | --- |
| Kernel | C++17/C, monolithic, multiboot, VESA 1360×768×32 |
| Userland runtime | Ring-3 tasks, demand paging, `int 0x80` syscalls #1–#54 |
| Compiler | `mtcc` — TinyCC-style, single-pass, in-OS |
| Packages | `eggkg` over package.list v0 / index.idx v1, SHA-256 verified |
| Storage | FAT32 (read/write, LFN, FSInfo, rollback) on PATA + AHCI |
| Network | lwIP 2.1.3, BearSSL TLS 1.2, NE2000 + Intel E1000 |
| Desktop | LVGL 9 apps, EquiX desktop (ThorVG vector renderer) |

Historical: [Release/0.1_beta.md](Release/0.1_beta.md). Quick answers: [FAQ](FAQ.md) · full doc map: [nav.md](nav.md).
