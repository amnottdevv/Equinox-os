# Dokumentasi Equinox OS — v0.3 Beta

Equinox OS adalah sistem operasi hobby 32-bit x86 dengan kernel monolitik
yang ditulis dalam C++17, di-boot oleh GRUB (multiboot), menggambar ke
framebuffer VESA 1360x768, punya stack TCP/IP nyata yang bisa mengunduh
file dari internet lewat HTTP**S**, subsistem disk (ATA + FAT32
baca/tulis) yang persisten antar reboot — dan sejak v0.3, **OS ini
meng-compile userland-nya sendiri dari dalam OS** (`eqbuild`, 29 tools).

Status rilis: **v0.3 Beta** — regresi QEMU **101/101 PASS**.

## Isi folder ini

| File | Isi |
| --- | --- |
| [GETTING_STARTED.md](GETTING_STARTED.md) | Prasyarat, cara build, dan **semua command menjalankan QEMU** (ISO saja, dengan disk FAT32, jaringan, serial debug) |
| [RELEASE_NOTES.md](RELEASE_NOTES.md) | Catatan rilis v0.3 Beta: **fitur unggulan**, **fitur tambahan**, perbaikan bug penting, batasan yang diketahui, roadmap v0.4 |
| [COMMANDS.md](COMMANDS.md) | Referensi lengkap: command shell bawaan + **29 tools userland** yang di-compile in-OS, dengan contoh pemakaian |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Gambaran teknis: alur boot, memory map, scheduler, syscall #0–#54, filesystem, jaringan, grafis, format MRP & pipeline self-hosting |

Dokumen teknis bahasa Inggris yang lebih rinci tetap ada di root repo:
`README.md` (feature list + shell reference), `RELEASE_v0.3.md`
(changelog teknis), `mrp_user/TCC.md` (compiler), `mrp_user/TARGETS.md`
(roadmap FR), `mrp_user/workflow_mrp.md` (alur kerja MRP).

## Quick start (3 langkah)

```sh
# 1. Build kernel + ISO
make

# 2. Boot di QEMU (dengan jaringan user-mode + httpd di localhost:8080)
make run

# 3. Di dalam OS: build userland-nya sendiri
eqbuild
```

Setelah `eqbuild` selesai, seluruh 29 tools (`ls`, `cat`, `grep`, `tail`,
`sort`, `wc`, …) langsung bisa dipakai di shell. Prompt shell:
`root::users / $`.

## Spesifikasi singkat

| | |
| --- | --- |
| Arsitektur | 32-bit x86 (i386), kernel monolitik C++17 |
| Boot | GRUB / multiboot, ISO hybrid |
| Mode grafis | VESA 1360x768 @ 32 bpp (fallback teks VGA otomatis) |
| Privilege | ring 0 / ring 3 nyata (TSS), syscall `int 0x80` #0–#54 |
| Multitasking | round-robin preemptive, 2 console virtual (F1/F2), demand paging |
| Filesystem | RAMFS + FAT32 read-write (LFN, write-through, auto-mount `/mnt`) |
| Jaringan | lwIP 2.1.3 + NE2000: DHCP, DNS, ICMP, TCP, HTTP(S) client & server |
| Compiler | `mtcc` — compiler C in-OS + libc subset, pipeline self-hosting `eqbuild` |
| Ukuran ISO | ± 13.5 MB (termasuk source 29 tools + DOOM) |
| Emulator uji | QEMU (`qemu-system-i386`, RAM 64 MB) |
