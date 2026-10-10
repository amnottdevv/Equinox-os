# Release notes — 0.4 Beta

**0.4 Beta** turns Equinox from "a kernel that can compile its tools"
into a **self-installing, package-driven operating system**: a real
installer wizard, a package manager with a Git-hosted repository, a
validated configuration store, a disk tool, and a storage/network
driver expansion (AHCI SATA + block layer, Intel E1000).

Regression status: 101 core QEMU checks PASS, plus dedicated 0.4
suites — installer wizard (15), AHCI (17), E1000, `.ecf` (25), manual
install (23), base image (15), `/mnt` build (7), eggkg bash build.

## Highlights

### 1. `equinoxinstall` — the in-OS installer wizard
Four phases from the running OS: pick a target disk (or *build in
place*) → format/copy the EQUINOXBASE layout → pick the network driver
(written into `system.ecf`) → compile the userland onto the target with
live status; optional `[5/5]` installs the GRUB bootloader so the disk
boots without the CD. Shortcuts: `-compile <dir>`, `-build
<file.ruf|name|*.ruf>`. *(The v0.3 `eqbuild` loop lives on inside it as
the compile phase.)*

### 2. `eggkg` + the Eggkg-l repository
Gentoo-style package manager built into the shell: `update / install /
remove / list / search / info / sync`. Packages are C sources + a ruf
v3 recipe, fetched over HTTPS from
[Eggkg-l](https://github.com/amnottdevv/Eggkg-l) (local/offline paths
work too), verified with in-kernel SHA-256, compiled with `mtcc -make`
and installed into `/bin` with `installed.db` + boot-time `.local`
sync. `dependencies.bash=true` auto-activates the coreutils at boot.

### 3. The bash package replaces bundled coreutils
**The base ISO no longer bundles the bash-class tools** (`ls cat cp mv
mkdir rmdir rm touch stat` and friends). The canonical copies live in
Eggkg-l as the `bash` package; the shell keeps eqbash builtins
(`lf`, `showf`, `cdir`, `cfile`, `ccfile`, `save`, `delfile`, `deldir`,
`pren`, `copy`, `move`, `del`) and **bash-name aliases** so the thin
base stays fully usable, and `eggkg install bash` swaps in the full
package. Text filters (`grep`, `wc`, `sort`, …) remain in the base.

### 4. Storage: block layer + AHCI (SATA)
New generic block layer (`blk.cpp`, 8 fixed slots: PATA 0–3, SATA 4+,
`hda`–`hdh` naming, bus-neutral MBR parsing) with a full **AHCI 1.x
SATA driver** (`ahci.cpp`): ABAR/BAR5, GHC.AE, engine stop/start with
CR/FR wait, DET/ATAPI checks, command list + FIS receive + PRDT DMA,
READ/WRITE DMA (EXT) + FLUSH CACHE, self-healing port restart.
PATA keeps the classic **ATA PIO** driver. FAT32 gains an in-kernel
**formatter** (`fat32_mkfs`: MBR + type-0x0C whole-disk partition @
LBA 2048, label EQUINOXBASE).

### 5. Networking: Intel E1000
New PCI NIC driver (`e1000.c`, `8086:100E/100F`): EEPROM MAC, RX ring
32×2048 B, TX ring 8×16 B descriptors, read-to-clear ICR — behind the
same `nic.c` registry as the NE2000 (adding a NIC = one struct + one
registration). Boot-time driver selection via `system.ecf`
(`[net] driver = ne2000|e1000|none`), wizard step [3/4] writes it;
`make run-e1000` / `scripts/e1000_test.py`.

### 6. `Qfs` — the disk tool
`Qfs -list-disk` (hda–hdh with size/label/state), `Qfs -t hdX -format
fat32`, `Qfs -install-boot hdX` (GRUB `boot.img`+`core.img` staged in
RAMFS `/equinox/bootimg/`). Paired with the `mount/umount` builtins and
the recursive `copy`/`del` commands this is the manual install path.

### 7. Configuration: `.ecf` + `set` + eqshell scripts
INI-lite config store (`ecf.c`) with the `set` builtin:
`set`, `set <key> <val>`, `set list`, `-a` apply, `-w` write,
`-d` register system config, `-b` base pivot, `-x` run `.es` script.
`system.ecf` lives in `/equinox/conf/` (legacy `boot/` fallback,
`active.conf` overlay). eqshell scripts start with `[Eqshell]`,
journal to `/eqshell.log`. Schema keys: `net.driver`,
`dependencies.bash`, `eggkg.server/mirror/local`.

### 8. Build system: ruf v3 + `mtcc -make`
Recipes gain real variables (`name := "bash"`), jobs and
`copy/move ? to` install steps; `equinoxinstall -build` and `eggkg`
share the same engine. mtcc itself: line-numbered undefined-reference
errors, `%u` true unsigned, host/OS byte-identical output.

### 9. Shell & desktop
Shell: **pipes, glob, redirection, history**; eqbash builtins;
`xxd`; `->` copy syntax; multi-line `save`. Desktop: **EquiX** vector
desktop (ThorVG) with dynamic GUI arena (`guiarena.cpp`), LVGL apps,
`desktop`/`tvgdemo`/`tvgbench` commands.

### 10. Memory
Dynamic RAM top from the GRUB memory map (`paging_set_ram_top`,
tables clamped 16–128 MB), demand-paged per-task arenas, **256 MB
recommended** (`-m 256`, 64 MB minimum), region above `ram_top`
deliberately unmapped.

## Compatibility notes

- **Bash tools**: if you scripted against `ls/cat/cp` you will see the
  builtins/aliases until `eggkg install bash` — behavior-compatible,
  richer once installed.
- **eqbuild (0.3)**: superseded by `equinoxinstall`; `-compile` /
  `-build` cover the old flows.
- **`boot/system.ecf`** → moved to `/equinox/conf/system.ecf` (legacy
  path still read).
- **Syscall table unchanged** (#0–#54, append-only) — userland from
  0.3 rebuilds as-is.

## Known limitations

- ATAPI (CD over AHCI) is detected and skipped, not mounted.
- mtcc remains a C subset (no struct/union **by value**, no float, no `sizeof`); `struct`/`union`, `enum`, `typedef` and `switch` are supported — see `docs/MTCC_LANGUAGE.md`.
- ICMP `ping` does not traverse QEMU slirp; use `tcpping`.
- `MAX_TASKS` = 8; fd table 16/task; single user (`root`).
- The GUI arena assumes ≤128 MB mapped RAM (tables clamp); DMA
  buffers are identity-mapped BSS — RAM-hungry additions need bounce
  buffers.

## Roadmap (post-0.4)

- ATAPI/CDROM over AHCI; NCQ command scheduling.
- Real multi-user + permissions; more packages (games data, toolchain).
- Network: second NIC concurrent, UDP tools, larger TLS suite.
- mtcc: structs by value, float, larger preprocessor.
- Installer: partition-level (non-whole-disk) installs.
