# FAQ

The questions people actually ask about Equinox OS — answered straight,
without the fluff.

## Can Equinox be installed to a `*.img` file that boots like a real device?

Yes. The manual install path produces exactly that:

1. Create a disk image on the host (a raw `.img`), attach it in QEMU
   with `-drive format=raw,file=disk.img`, and boot the live ISO.
2. Inside the OS: `Qfs -list-disk`, `Qfs -t hdX -format fat32`,
   `mount hdX`, copy over `/equinox` (tools, libc, `mtcc.mrp`,
   boot images), write `/mnt/equinox/conf/system.ecf`, then
   `Qfs -install-boot hdX`.

That `.img` now carries GRUB (`boot.img` + `core.img` at LBA 1), a
FAT32 partition, and the whole userland — it boots standalone, and you
can `dd` it to a USB stick if the disk layout is PATA/SATA. The wizard
(`equinoxinstall`) automates the same steps; it is not a different
mechanism.

## Can I make a bootable Equinox FAT32 image *manually*, without `equinoxinstall`?

Absolutely — that is the "Path B" manual install, and it is fully
supported:

```sh
Qfs -list-disk
Qfs -t hdX -format fat32     # MBR + FAT32 @ LBA 2048, label EQUINOXBASE
mount hdX
copy dist/equinox /mnt/equinox
set net.driver e1000
set -w /mnt/equinox/conf/system.ecf
Qfs -install-boot hdX
```

Key points: the volume must be labelled `EQUINOXBASE` (that is what
promotes it to `/` at boot), and `Qfs -install-boot` needs
`/equinox/bootimg/{boot.img,core.img}` present — both ship on the ISO.

## Why doesn't Equinox ship a full bash? Isn't that weird for a Unix-like?

A bit, on purpose. The ISO is deliberately thin: it carries the
*sources* and the compiler, not a finished coreutils. Full GNU-style
coreutils (`ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, …) arrive as the
**bash package** through `eggkg`, built *inside the OS* with `mtcc`:

```sh
eggkg update
eggkg install bash -y
```

Once installed, those tools take over the corresponding shell
aliases, and `[dependencies] bash = true` in `system.ecf` re-links
them into `/bin` on every boot. The idea: Equinox demonstrates a
fully self-hosting build pipeline, and package delivery doubles as
the demo.

## How do I write C programs on Equinox?

In `mtcc`'s C subset, straight from the shell:

```c
// /test/hello.c
#include <morph.h>
int main(void) {
    print("hello from Equinox\n");
    return 0;
}
```

```sh
mtcc /test/hello.c        # compile AND run, live
mtcc -c hello.c           # compile only -> hello.mrp
mtcc -make build.ruf      # multi-file build from a recipe
```

The subset has: `int`/`char`/`void`, 1–2 level pointers, 1D arrays,
if/else, while/do/for, switch-free logic, and the mini libc/morph API
(`print`, `readline`, `fb_info`, `put_pixel`, …). **No** structs,
floats, 2D arrays, `sizeof`, `typedef`, or varargs — by design, so the
compiler stays small enough to self-host. Edit with the built-in
editor (`edit <file>`), compile, run. The `equinoxinstall` wizard and
`eggkg` use the exact same compiler, so anything that builds on the
ISO builds for packages.

## Where do my files live? RAMFS or the disk?

- **RAMFS** (the boot image): everything from GRUB modules —
  `/test`, `/equinox/tools`, `/equinox/libc`, `mtcc.mrp`, `doom.wad`.
  Fast, but gone on reboot.
- **FAT32** (the disk): `/mnt` on the live ISO; on an installed
  system the `EQUINOXBASE` volume *is* `/`. Anything you want to keep
  belongs here — use `save`, `edit`, `copy`.

Rule of thumb: `/mnt` and the installed root survive reboot; bare
RAMFS paths do not.

## Can I write my own package for `eggkg`?

Yes — everything is documented in [PACKAGES.md](PACKAGES.md): put your
C sources plus a `build.ruf` (ruf v3) in a repo, and
`eggkg install <name>` fetches, compiles with `mtcc -make`, and
installs the `.mrp` products into `/bin`. If it compiles with `mtcc`,
it installs.

## How do I change the NIC driver?

It is one key in `system.ecf`:

```sh
set net.driver e1000      # or ne2000 | none
set -w /equinox/conf/system.ecf
# reboot to apply
```

`net.driver` is read once at boot by `net_nic_init()`, so the change
takes effect on the next boot. `ifconfig` confirms what came up.

## How do I check what is in a disk / partition?

```sh
Qfs -list-disk        # all 8 slots, FAT32 flag, label, state
diskinfo              # detail on a slot
```

`hda`–`hdd` are PATA, `hde`–`hdh` are SATA (AHCI). Slots with no
drive just show as empty.
