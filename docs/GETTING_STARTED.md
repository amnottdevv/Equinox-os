# Getting Started — Build & Menjalankan Equinox OS di QEMU

Dokumen ini menjawab pertanyaan paling sering: *"command run QEMU-nya
gimana?"* — lengkap dari nol (build) sampai semua variasi menjalankan OS,
plus catatan troubleshooting.

## 1. Prasyarat (host Linux)

| Kebutuhan | Keterangan |
| --- | --- |
| `g++` / `gcc` dengan `-m32` | atau cross toolchain `i686-elf-` |
| `nasm` | assembler boot (`boot/start.asm`) |
| `grub-mkrescue` + modul `i386-pc` | pembuat ISO bootable (butuh `xorriso`) |
| `mtools` | pembuat `dist/disk.img` FAT32 |
| Python 3 | script build & harness tes |
| `libgcc.a` 32-bit | dari `gcc-multilib`, atau arahkan `LIBGCC32_DIR` |

Debian/Ubuntu: `sudo apt install build-essential gcc-multilib nasm
grub-pc-bin xorriso mtools python3`.

> Jika toolchain ada di lokasi kustom (di-extract tanpa root), point
> `LIBGCC32_DIR` ke folder libgcc 32-bit dan `PATH` ke binari
> `grub-mkrescue` — makefile otomatis memakai `/usr/lib/grub/i386-pc`
> atau `~/tools/root/usr/lib/grub/i386-pc`.

## 2. Build

```sh
make            # kernel.elf + dist/equinox.iso  (hasil utama)
make mtcc       # pack compiler in-OS  -> dist/equinox/tools/mtcc.mrp
make tools      # stage 29 source .c tools -> dist/equinox/tools/ (eqbuild source)
make pack       # pack program mrp_user/*.cpp + games/*.cpp
make doom       # build doomgeneric      -> dist/equinox/games/doom.mrp
make diskimg    # disk uji FAT32 64 MB   -> dist/disk.img
make test       # harness mtcc di host (tanpa QEMU)
```

`make` saja sudah cukup untuk ISO lengkap: kernel + modul RAMFS +
source tools + compiler. `dist/` mencerminkan layout RAMFS di dalam OS.

## 3. Menjalankan di QEMU

### 3a. Cara paling cepat — make

```sh
make run        # ISO + jaringan user-mode (httpd terekspos di localhost:8080)
make run-disk   # + disk FAT32 64 MB ter-attach (auto-mount di /mnt)
```

### 3b. Command QEMU manual — ISO saja

```sh
qemu-system-i386 -m 64 -cdrom dist/equinox.iso \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

### 3c. Command QEMU manual — ISO + disk FAT32 (rekomendasi)

```sh
qemu-system-i386 -m 64 -boot order=d -cdrom dist/equinox.iso \
    -drive file=dist/disk.img,format=raw,if=ide,index=0,media=disk \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

### Penjelasan opsi

| Opsi | Fungsi |
| --- | --- |
| `-m 64` | RAM 64 MB untuk guest — cukup (kernel heap ±3 MB, pool user terpisah, staging modul 12 MB) |
| `-cdrom dist/equinox.iso` | boot dari ISO Equinox |
| `-boot order=d` | boot dari CD-ROM dulu (varian disk) |
| `-drive file=dist/disk.img,…,if=ide,index=0` | disk IDE primary master; partisi FAT32 pertama auto-mount di `/mnt` |
| `-netdev user,id=net0` | jaringan user-mode slirp: guest `10.0.2.15`, gateway/host `10.0.2.2`, DNS `10.0.2.3` (DHCP otomatis saat boot) |
| `hostfwd=tcp::8080-:80` | web server `httpd` di guest bisa dibuka dari browser host: `http://localhost:8080/` |
| `-device ne2k_isa,netdev=net0,iobase=0x300,irq=9` | NIC NE2000 ISA (driver bawaan) |

### 3d. Variasi lain yang berguna

```sh
# Log serial (mirror debug console COM1) ke file — cara utama men-debug:
qemu-system-i386 -m 64 -cdrom dist/equinox.iso -serial file:serial.log \
    -netdev user,id=net0 -device ne2k_isa,netdev=net0,iobase=0x300,irq=9

# Kumpulkan log QEMU monitor sendiri (opsional):
#   tambahkan: -monitor stdio   lalu ketik "info registers", "xp /8wx addr", dll.

# Boot cepat tanpa jaringan (uji murni filesystem/multitasking):
qemu-system-i386 -m 64 -cdrom dist/equinox.iso
```

Catatan: ICMP `ping` tidak diteruskan oleh user-net QEMU — pakai
`tcpping <host> [port]` dari dalam OS.

## 4. Hal pertama yang dilakukan setelah boot

Boot log gaya Linux `[ OK ]` akan lewat, lalu muncul prompt:

```
root::users / $
```

Langkah penting #1 — **build userland-nya sendiri**:

```
root::users / $ eqbuild
[eqbuild] 1/29  cat.c
mtcc: wrote cat.mrp (35210 bytes)
[eqbuild] 1/29  OK  cat.mrp built, source removed
... (29 file)
[eqbuild] 29 built, 0 failed
```

Setelah itu semua tools siap dipakai. Beberapa hal untuk dicoba:

```sh
ls                    # isi RAMFS root
eqbuild               # (sudah) — 29 tools di-compile in-OS
grep printf /test/hello.c
mtcc /test/hello.c    # compile & run C langsung di dalam OS
lspci                 # tabel PCI hasil enumerasi boot
ps                    # daftar task; switch console: F1/F2
mget https://raw.githubusercontent.com/torvalds/linux/master/README
doom -iwad /mnt/doom1.wad    # DOOM langsung dari disk FAT32
```

Dengan disk ter-attach (`run-disk`): `ls /mnt`, `cat /mnt/README.TXT`,
`cd /mnt && mget http://10.0.2.2:8022/file` (unduh langsung ke disk,
persisten antar reboot), `doom -iwad /mnt/doom1.wad`.

## 5. Kontrol & interaksi

| Tombol | Fungsi |
| --- | --- |
| `F1` / `F2` | pindah console virtual (tiap console punya task/shell sendiri) |
| Keyboard PS/2 | input shell, editor, game (WASD + mouse untuk DOOM) |
| `Ctrl+Alt+G` (QEMU) | lepas/masang mouse ke guest |

## 6. Troubleshooting

| Gejala | Sebab & solusi |
| --- | --- |
| `boot failed: could not read from CDROM` | `grub-mkrescue` memakai platform salah / modul `i386-pc` tidak ketemu — pastikan paket grub-pc-bin terpasang; makefile otomatis memakai `-d` ke folder `i386-pc` bila ada |
| Link error `-lgcc` | `libgcc.a` 32-bit tidak ketemu — install `gcc-multilib` atau set `LIBGCC32_DIR=.../32` |
| `error: unknown keyword` di make | pastikan `make` (GNU make) dan tab recipe tidak rusak — edit makefile via editor yang menjaga TAB |
| Boot hang / layar hitam | cek `serial.log` (boot melaporkan semua tahap ke COM1); coba `-vga std` |
| Tidak ada jaringan | pastikan opsi `-netdev`/`-device` satu baris utuh; cek banner DHCP saat boot |
| Tools "Unknown command" | tools baru tersedia setelah `eqbuild` dijalankan (boot baru membawa source, bukan binari — itu desain self-hosting) |

## 7. Harness tes otomatis (opsional)

```sh
python3 scripts/regression_task3.py      # suite utama v0.3 (2 part otomatis)
python3 scripts/regression_task2.py      # proses/memori/ELF/pipe
python3 scripts/regression_v03.py        # core syscall + tools
python3 scripts/regression_multitask.py  # canvas/game/F1-F2/DOOM
```

Total 101 pemeriksaan; semua harus PASS pada ISO rilis.
