# Referensi Command — Shell & Tools Userland

Prompt shell: `root::users / $`. Command **userland** (tabel di bawah
dengan tanda ✓) adalah program `.mrp` ring-3 yang di-compile **di dalam
OS** oleh `eqbuild` — jalankan `eqbuild` dulu setelah boot, lalu semua
tools siap dipakai. Command lain adalah builtin kernel.

Sintaks umum: `command [argumen...]`; path absolut (`/mnt/...`) maupun
relatif (`./file`, `dir/file`) diterima; glob/pipes antar-command belum
didukung (pipe ada di API syscall, lihat `pipedemo`).

## 1. Filesystem — navigasi & manipulasi

| Command | Jenis | Fungsi / contoh |
| --- | --- | --- |
| `ls [dir]`, `ls -l` | builtin | isi direktori (RAMFS dan `/mnt` sama saja) — `ls /mnt`, `ls -l` |
| `cd <dir>` | builtin | pindah direktori — `cd /equinox/tools` |
| `pwd` | builtin | direktori kerja sekarang |
| `tree [dir]` | builtin | pohon direktori rekursif |
| `cat <file>` | ✓ | tampilkan isi file — `cat /mnt/README.TXT` |
| `cp <src> <dst>` | ✓ | copy file (RAMFS / disk) |
| `mv <src> <dst>` | ✓ | pindah / rename |
| `rm <file>` | builtin/✓ | hapus file (write-through di `/mnt`) |
| `mkdir <dir>`, `rmdir <dir>` | builtin/✓ | buat / hapus direktori |
| `touch <file>` | ✓ | buat file kosong / update waktu |
| `stat <file>` | ✓ | metadata file (ukuran, tipe, waktu) |
| `cfile <name>`, `ccfile <name> <text>` | builtin | buat file / buat dengan isi satu baris |
| `save <name> << "text"` | builtin | buat/timpa file dengan teks multi-baris |
| `cdir <name>` | builtin | buat direktori |
| `edit <file>` | builtin | editor teks bawaan (arrow/PgUp/PgDn/Home/End/Tab; **Ctrl+S** simpan, **Ctrl+Q** keluar) |
| `xxd <file> [n]` | builtin | hex dump n byte pertama (default 64) — `xxd /mnt/bin.dat 32` |
| `mount` / `umount` | builtin | pasang / lepas volume FAT32 di `/mnt` |
| `diskinfo` | builtin | drive ATA + detail volume (layout, free cluster, arena) |
| `fm` | builtin | file manager GUI (LVGL + mouse) |

## 2. Tools teks (18 tools baru + set file lama — semua ✓ eqbuild)

| Command | Fungsi | Contoh |
| --- | --- | --- |
| `grep [-i] [-n] [-c] [-v] PATTERN FILE...` | cari baris; mini-regex `.` `X*` `^` `$`; multi-file (prefix `nama:`) | `grep -n printf /test/hello.c` |
| `head [-n N] FILE` | N baris pertama (default 10) | `head -n 5 /test/libc.c` |
| `tail [-n N] FILE` | N baris terakhir (default 10, maks 64) | `tail -n 3 serial.log` |
| `wc [-l -w -c] FILE` | hitung baris/kata/byte | `wc -l /test/hello.c` |
| `sort [-r] FILE` | urutkan baris (asc/desc) | `sort -r names.txt` |
| `uniq [-c] FILE` | hapus duplikat bersebelahan (+ hitung) | `sort x.txt` lalu `uniq -c x.txt` |
| `cut -d DELIM -f LIST FILE` | ambil kolom — LIST: `N`, `N-M`, `N-`, koma | `cut -d : -f 1 users.txt` |
| `tr SET1 SET2 FILE` / `tr -d SET FILE` | translasi / hapus karakter, range `a-z` didukung, escape `\n \t \r \0` | `tr a-z A-Z data.txt` |
| `rev FILE` | balik tiap baris | `rev data.txt` |
| `nl FILE` | nomori baris (`%6d`) | `nl /test/hello.c` |
| `more FILE` | pager 23 baris/halaman — sembarang tombol = lanjut, `q` = keluar | `more /test/libc.c` |
| `find [dir] [-name SUBSTR]` | cari rekursif (depth 8) | `find / -name mrp` |
| `which NAME` | lokasi command di path sistem (`.` `/` `/bin` `/equinox/tools` `/equinox/games`) | `which grep` |
| `diff FILE1 FILE2` | bandingkan baris (20 diff pertama, gaya `</>`) | `diff a.txt b.txt` |
| `strings FILE [minlen]` | deret karakter printable | `strings doom.mrp 8` |
| `cksum FILE...` | checksum 32-bit + ukuran | `cksum /equinox/tools/head.mrp` |
| `basename PATH` | nama file dari path | `basename /mnt/doom1.wad` → `doom1.wad` |
| `dirname PATH` | direktori dari path | `dirname /mnt/doom1.wad` → `/mnt` |
| `fstest` | ✓ suite 24 uji syscall file | `fstest` |
| `pipedemo` | ✓ demo pipe + wait (child menulis, parent membaca) | `pipedemo` |

## 3. Multitasking & proses

| Command | Fungsi |
| --- | --- |
| `ps` | tabel task: pid, nama, state (RUNNING/READY/SLEEP/BLOCKED/DEAD), CPU, memori |
| `kill <pid>` | matikan task |
| `switch <n>` | pindah console virtual (sama dengan tombol **F1**/**F2**) |
| `wait [pid]` | block sampai child exit (`wait` = child mana pun) — status exit ikut tercetak |
| `spawn <path.mrp> [args]` | buat task baru dengan args |
| `yield` | beri CPU ke task berikutnya |
| `sleep <ms>` | tidur task (sudah presisi per tick — 3000 ms = 300 tick) |
| `meminfo` | pool user: total/free, halaman faulted per task, reserved vs faulted, zombie |

## 4. Compiler & self-hosting

| Command | Fungsi |
| --- | --- |
| `mtcc <file.c>` | compile **dan jalankan** source C di dalam OS — `mtcc /test/hello.c` |
| `eqbuild` | **self-hosting**: compile semua `/equinox/tools/*.c` (29 tools) dengan mtcc in-OS, install `.mrp`, hapus source; log per file `i/29 OK nama` |
| `run <x.mrp>` / `./x.mrp` / `./x.elf` | jalankan program MRP / ELF32 di ring 3 |
| `elfdemo.elf` | demo loader ELF statis (exit status 42, coba `spawn` + `wait`) |

## 5. Hardware & introspeksi

| Command | Fungsi |
| --- | --- |
| `lspci` | tabel bus PCI hasil enumerasi boot: vendor/device, class, BAR, IRQ |
| `cpu` | info CPU (CPUID: vendor, fitur) |
| `info` | banner sistem: versi, RAM, uptime |
| `memmap` | peta memori fisik / area kernel |
| `malloc` / `alloc <n>` / `free <addr>` | status & uji heap kernel (total ±3.1 MB, 2-region) |
| `syscalls` | daftar syscall #0–#54 dengan signature |
| `sctest` | self-test jalur syscall |
| `ring` / `ringstats` | uji & statistik ring buffer |
| `tick` | nilai & laju timer (100 Hz) |
| `random` / `math <expr>` / `calc <expr>` | RNG & kalkulator ekspresi |
| `hex <n>` / `dec <n>` | konversi hex⇄desimal |
| `testconv` `teststr` `testvector` | self-test konversi/string/vector libc |
| `mouse` | uji driver mouse PS/2 |

## 6. Jaringan

| Command | Fungsi |
| --- | --- |
| `ifconfig` | status interface (IP dari DHCP, MAC, statistik) |
| `ping <host>` | ICMP echo — di QEMU user-net ICMP tidak diteruskan, pakai `tcpping` |
| `tcpping <host> [port]` | probe TCP + RTT |
| `dns <hostname>` | resolusi nama via DNS (10.0.2.3) |
| `mget <url> [-port <n>]` | unduh HTTP/**HTTPS** ke direktori sekarang; ikuti redirect (maks 3) — `mget https://github.com/octocat/Hello-World` |
| `httpd` | web server :80 (status + file RAMFS) — dari host: `http://localhost:8080/` |
| `nettask` | status kernel network task |
| `netdbg` | statistik/diagnosa jaringan |

## 7. Grafis, GUI, game & multimedia

| Command | Fungsi |
| --- | --- |
| `gui` | buka desktop LVGL (settings, editor, file manager) |
| `settings` | panel pengaturan GUI |
| `color <n>` | set warna teks |
| `clock` | jam/tanggal (RTC) |
| `doom [args]` | DOOM (perlu `doom1.wad`, shareware) — `doom -iwad /mnt/doom1.wad` |
| `snake` / `breakout` / `pong` | game `.mrp` Libgame (jalan dengan `run` atau langsung namanya) |
| `beep` / `song` | uji audio PC speaker |

## 8. Lain-lain

| Command | Fungsi |
| --- | --- |
| `echo <teks>` | cetak teks |
| `clear` | bersihkan layar |
| `reboot` | reboot mesin |
| `panic [teks]` | uji panic handler (sengaja) |
| `help` | *(dihapus di v0.3)* — daftar command ada di dokumen ini & README |

## Contoh sesi lengkap

```sh
root::users / $ eqbuild                 # 1. build 29 tools in-OS
root::users / $ mtcc /test/hello.c      # 2. compile & run C live
root::users / $ grep -n printf /test/hello.c
root::users / $ ls /mnt                 # 3. disk FAT32 ter-mount
root::users / $ cd /mnt && save catatan.txt << "tes persisten"
root::users / $ cat catatan.txt
root::users / $ mget http://10.0.2.2:8022/data.json
root::users / $ ps                      # 4. lihat task; F1/F2 ganti console
root::users / $ doom -iwad /mnt/doom1.wad
```
