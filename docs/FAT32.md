# FAT32 — the on-disk filesystem

Equinox ships exactly one persistent filesystem: **FAT32**, grown and
verified in-house (`kernel/library/fs_fat32*.cpp`). It holds
`/equinox` (tools, libc sources, `mtcc.mrp`), `/user`, `/test`, and
`system.ecf` on an installed disk. RAMFS holds the live boot image.

## Geometry Equinox writes

`fat32_mkfs()` produces:

- a new **whole-disk MBR**;
- one partition, type **`0x0C`** (FAT32 LBA), starting at **LBA 2048**
  — leaving 1 MB for `boot.img`/`core.img` when `Qfs -install-boot`
  later writes them;
- FSInfo sector + FAT32 label **`EQUINOXBASE`**.

A volume labelled `EQUINOXBASE` is promoted to the **root** filesystem
at boot (everything else mounts at `/mnt`). That is how an installed
disk becomes `/`.

## Feature set (fs_fat32.cpp)

- Read and write with **LFN** + 8.3 short-name anti-collision
  mangling;
- directory entries grown on demand;
- FSInfo kept in sync for free-cluster counts;
- **rollback on I/O failure** — a failed write restores the previous
  clusters rather than corrupting the chain;
- an 8 MB disk cache outside the kernel heap;
- write-through on every `close`.

## Block underpinnings

All sector I/O funnels through the block layer
(`kernel/library/drivers/blk.cpp`): 8 fixed slots, PATA on 0–3
(``hda``–``hdd``), AHCI on 4–7 (``hde``–``hdh``). `blk_read`/`blk_write`
bounded to 128 sectors per call, MBR parsing for the partition probe.
Details: [DRIVERS.md](DRIVERS.md).

## Mounting

- The first FAT32 partition of the first PATA disk auto-mounts at
  `/mnt` (lazy RAMFS mirror, so `ls/cat/edit/mget` work transparently).
- `mount hdX` / `umount` builtins; `diskinfo` to inspect.
- `EQUINOXBASE` volumes are auto-promoted to `/` at boot.

## Host-side verification

The write path is tested against an **mtools oracle** plus a mini check
up to 100% volume utilization (`scripts/make_fat32_img.py`,
`fat32_write_test.py`, `regression_task2.py`). `make test` runs the
host harness without QEMU.

## When something looks wrong

| Symptom | Check |
| --- | --- |
| Disk not visible | `Qfs -list-disk` — is a slot `siap`/`ready`? AHCI disks land on `hde` with `make run-ahci`. |
| Boots to FAT32 probe errors | The slot scan runs `mbr_read_partitions`; no partition table ⇒ the disk was never `-format fat32`'ed. |
| `/mnt` empty | Is the volume labelled `EQUINOXBASE`? If so it became `/`, not `/mnt`. |
| Slow heavy writes | Expected: write-through per close; batch copies with `copy` rather than many tiny shell redirects. |
