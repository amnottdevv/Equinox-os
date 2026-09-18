# Equinox OS v0.2 Beta — Release Notes

**Date:** 2026-09-18
**Artifacts:** `equinox.iso` (bootable), `disk.img` (64 MB FAT32 demo disk)

v0.2 adds the **disk subsystem**: an ATA/IDE PIO driver and a complete
FAT32 filesystem — read-only in Phase A, full read/write in Phase B.
The OS can now mount a hard disk, browse it with the normal shell
commands, run DOOM straight from it, download files from the internet
onto it, and everything persists across reboots.

---

## What is new in v0.2 Beta

### 1. ATA/IDE PIO driver (`kernel/library/drivers/ata.cpp`)

- Two buses (primary `0x1F0`, secondary `0x170`), master + slave —
  all four slots are probed at boot with IDENTIFY DEVICE (0xEC).
- ATAPI packet devices (QEMU's `-cdrom`) are detected via the
  0xA1 IDENTIFY PACKET signature and reported as CD-ROMs.
- LBA28 transfers everywhere; LBA48 automatically for drives/disks
  beyond 128 GB (QEMU's virtual disk reports LBA48 support — the
  driver verifies it from IDENTIFY words 83/100-103).
- Writes end with FLUSH CACHE (0xE7) so data is durable in the
  backing file even when QEMU is killed.
- **MBR parsing**: the four primary entries, 0x55AA validated, type
  0x0B/0x0C FAT32 (any entry whose boot sector parses as FAT32 is
  accepted too, plus "floppy-style" whole-disk FAT images).
- Boot log example:

      [ OK ] Detecting ATA drives + FAT32 volume (v0.2)
      [ OK ] ATA: primary master "QEMU HARDDISK" (64.0 MB)
      [ OK ] FAT32: 'EQDISK' mounted at /mnt (58.0 MB free)

### 2. FAT32 — Phase A, read (`kernel/library/fs_fat32.cpp`)

- **Mount**: BPB validation, FSInfo free-count (validated, counted by
  a FAT scan when the signature is bad), volume label, cluster/FAT
  layout sanity checks.
- **Lazy RAMFS mirror**: the FAT root becomes the `/mnt` RAMFS node;
  directories populate on the first `ls`/`cd`/lookup, files load on
  the first `cat`/read. Nothing is read that is not needed.
- **LFN read**: long file names are stitched from their 13-char UTF-16
  chunks with the checksum verified against the 8.3 alias; plain 8.3
  names render uppercase exactly as on disk. Name matching on the
  mounted volume is **case-insensitive** (FAT spec behavior) —
  `cat doom1.wad` finds `DOOM1.WAD`.
- **Fast whole-file reads**: cluster chains are walked with
  contiguous-run coalescing (up to 128 sectors per ATA transfer),
  so the 4.2 MB `doom1.wad` streams off the disk in well under a
  second under TCG.
- **8 MB disk arena** at `0x3400000` (right after the GRUB module
  staging area, clamped against the multiboot memory map): file
  caches live OUTSIDE the 2 MB kernel heap. That is what makes
  `doom -iwad /mnt/doom1.wad` possible — the WAD never touches the
  kernel heap.

### 3. FAT32 — Phase B, write (`kernel/library/fs_fat32_write.cpp`)

> **v0.2 final hardening pass.** The write path went through a
> dedicated audit + stress cycle (a 3 MB volume filled to 100 % by an
> in-OS program, with a pure-python mini-fsck on the host). It found
> one **critical data-loss bug** (invisible files after directory
> growth) and several spec/robustness defects — all fixed, details in
> section 3b.

Everything is **write-through**: every operation leaves the FAT, the
directory entries and the FSInfo sector consistent on disk before it
returns. There is no dirty cache to flush, no fsck to run.

- `cfile` / `ccfile` — create files, with **LFN entry generation**
  (name -> 8.3 mangling, `~1..~9` numeric-tail collision handling,
  checksum-tagged LFN entries written in the proper reverse order).
- overwrite/extend/truncate — cluster chains grow (allocated
  contiguously when possible, linked to the tail) or shrink (the
  tail is freed), the dirent's size/first-cluster/timestamps are
  updated from the **CMOS RTC**.
- `cdir` — subdirectories: one allocated + zeroed cluster, `.` and
  `..` entries, dirent run in the parent.
- `rm` — files and empty directories: the whole dirent run (LFN
  entries + 8.3) is marked `0xE5` and the cluster chain is freed.
- Directories **grow on demand**: when a directory is full, one
  cluster is appended and zeroed (FAT32 root included).
- Both FAT copies are always updated; the FSInfo free-cluster
  counter / next-free hint are maintained in memory on every
  allocation and synced to disk **once per operation** (see 3b).
- New shell verb **`save <name> << "text"`** — create **or
  overwrite** a file from the shell (`ccfile` only creates new
  files). Works on RAMFS and write-through on the FAT32 volume.

### 3b. Write-path hardening (v0.2 final)

Eight defects found by audit + stress testing, all fixed:

1. **CRITICAL — files invisible after directory growth.** When a
   directory grew, the new entries were written only into the fresh
   cluster while a trailing `0x00` (end-of-directory) slot could stay
   in the middle of the chain. Every FAT reader — including Equinox
   after a reboot — stopped at the marker: 140 files created, only 2
   visible after reboot, data physically on disk but unreachable.
   Fixed: the free-run now *continues* into the grown cluster, so
   those trailing slots are used (turning `0x00` into real entries)
   instead of being skipped. Verified: all 140 files + 471 fill files
   listed by mtools after a reboot.
2. **LFN padding garbage.** LFN entries kept reading the name buffer
   past its NUL terminator, writing stack/heap garbage into the
   padding (spec: `0x0000` terminator + `0xFFFF` padding). chkdsk and
   fsck.vfat flag such volumes. Fixed + verified by a host-side
   hex-walk of every LFN entry.
3. **Duplicate 8.3 short names.** Collision checks compared against
   the *long* names in the RAM mirror, so `cfile MY_DOCUM.TXT` next
   to an LFN file `My Document.txt` (short name `MY_DOCUM TXT`)
   silently created a duplicate short name. Fixed: every mirror node
   now caches its real on-disk 8.3 name (`fs_node::sfn[11]`) and
   creates compare against it — duplicates are re-tailed (`~1..~9`)
   or refused with "already exists".
4. **Use-after-free of the mount point.** `rmdir /mnt` with a volume
   mounted freed the live node `g_fat.mount_pt` still points at.
   Fixed: deleting a backing node returns POSIX-style "busy" (-9);
   the shell explains "mounted volume (umount first)".
5. **16 KB stack bounce buffer.** The write path bounced a whole
   cluster through a 16 KB stack array (a quarter of the 64 KB kernel
   stack) and refused any cluster size above 16 KB (32/64 KB clusters
   are the Windows default on big volumes). Fixed: sector-granular
   writes through a 512-byte bounce — any cluster size, any stack
   depth.
6. **No rollback on mid-write I/O failure.** An extend wrote the FAT
   link before the data; a failure left the dirent describing a chain
   that no longer matched. Fixed: extensions are rolled back
   (unlink + free the added chain), shrinks defer the tail free until
   after the dirent is consistent, truncation updates the dirent
   *before* freeing the old chain.
7. **FSInfo sector written per cluster.** A 4 MB write meant
   thousands of extra sector writes (the single-sector cache
   thrashed FAT <-> FSInfo on every cluster). Fixed: counters in
   memory, one FSInfo sync per operation.
8. **Smaller ones:** `fs_tree` now lazy-populates FAT directories
   (used to print an empty `/mnt`); `rm` of a non-empty directory
   prints one clear message instead of two different ones; a stale
   dirty cache line can no longer serve old bytes to whole-file
   reads; delete's backwards LFN walk follows the directory chain
   across cluster boundaries (no more orphaned LFN entries).

### 4. Integration: everything works on disk, transparently

The RAMFS `fs_node` gained a backing-store: directories lazily
mirror from disk, files lazily load, and every mutating call
(`fs_create_file`, `fs_write_binary`, `fs_create_dir`,
`fs_delete_node`) is written through when the parent is disk-backed.
Because the whole system sits on `fs_node`, this composes:

- `ls`, `cd`, `cat`, `rm`, `xxd` (new hex-dump command) in the shell
- the **GUI file manager** browses `/mnt` and can create files there
- the **syscall layer** (`SYS_OPEN`/`READ`/`READFILE`) lazy-loads
  disk files — which is exactly how DOOM reads its WAD
- **`mget` saves into the current directory**: `cd /mnt` +
  `mget http://...` downloads **straight onto the FAT32 disk**
  (multi-cluster binaries verified byte-identical on the host)
- new commands: `diskinfo` (drives, partitions, volume layout, free
  clusters, arena usage, cache stats), `mount` / `umount`

### 5. Demo disk (`make diskimg`)

`scripts/make_fat32_img.py` builds `dist/disk.img`: a 64 MB IDE
image with an MBR and one type-0x0C FAT32 partition, pre-filled
with `README.TXT` (plain 8.3), `hello from equinox.txt` (LFN),
`docs/` (nested + LFN inside), `bin.dat` (byte-pattern binary) and
`doom1.wad`. Attach it with:

```sh
make run-disk     # = qemu ... -cdrom equinox.iso -drive disk.img
```

---

## Verification

All numbers from the automated suites run against the shipped ISO:

| Suite | Result |
| --- | --- |
| FAT32 Phase A (mount, ls, cat 8.3/LFN, nested, xxd, sizes, diskinfo, IWAD) | 9/9 |
| FAT32 Phase B (create, read-back, mkdir, rm, rejected rm, mget-to-disk) | 6/6 |
| Host-side mtools oracle (mdir/mcopy round-trips of OS-written files) | 4/4 |
| Persistence (2nd boot with the same disk: files byte-identical) | 3/3 |
| No-disk regression (clean "no hard disks", RAMFS intact, mount fails cleanly) | 3/3 |
| DOOM booting from `/mnt/doom1.wad` (title screen, PNG proof) | 2/2 |
| mget HTTP regression (unchanged v0.1 behavior) | 10/10 |
| TLS regression (example.com, raw.githubusercontent verified, github.com) | 5/5 |
| **Write hardening — UX** (save create/overwrite/shrink, 8.3 collision policy, 63-char LFN, 200 KB -> 4 B -> 200 KB cycle, mount-point busy, umount/mount cycle, editor Ctrl+S round-trip, tree) | 16/16 + 11 host |
| **Write hardening — host fsck** (FAT1 == FAT2, FSInfo exact, LFN 0xFFFF padding, no duplicate 8.3, mtools agreement) | 11/11 |
| **Write hardening — stress** (3 MB volume: root grows to 140 files, filled to 100 % with 471 x 4 KB files by an in-OS mtcc program, clean "volume full", shell alive, rm + write recovery, all files survive the reboot) | 13/13 |
| mtcc host suite (unchanged) | 35/35 |

Notable host-side oracle checks (mtools is an independent FAT
implementation): the 200 KB `data.bin` downloaded by `mget` through
the OS into `/mnt` is **byte-identical** to the file served; the
`test.txt` written by `ccfile` round-trips through `mcopy` with the
exact bytes; untouched files stay untouched.

## Known limitations

- One FAT32 volume mounted at a time (at `/mnt`); MBR primary
  partitions only (no extended/GPT — extended entries are simply not
  matched as FAT).
- No FAT12/FAT16 support (rejected cleanly at probe time).
- File caches in the arena are never reclaimed (a bump allocator by
  design); overwritten/deleted caches are abandoned, not freed.
- 512-byte sectors only. Cluster sizes above 16 KB are no longer a
  problem for writing (the write path is sector-granular now); the
  read path was always cluster-size agnostic.
- Names longer than 63 characters fall back to the 8.3 form
  (`fs_node::name` cap).
- Case-insensitive matching applies to the mounted volume only;
  RAMFS stays case-sensitive as before.
- Volumes written by pre-hardening v0.2 builds may contain the
  mid-chain `0x00` marker described in 3b.1: files created after the
  first directory growth are invisible (the data is still on disk).
  Re-create such volumes, or mount them with a fresh build and
  re-copy the visible files. Volumes written by THIS build are
  clean (verified by the host-side mini-fsck at 100 % usage).
- DOS device names (CON, PRN, ...) are not filtered when creating
  files; such files are legal FAT but some DOS-era tools dislike
  them.

## Upgrading from v0.1

Nothing to do: boot the new ISO. Without a disk attached the system
behaves exactly like v0.1 (three extra boot-log lines at most). With
`disk.img` attached, `/mnt` appears automatically.
