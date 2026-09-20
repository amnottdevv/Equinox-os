# Catatan Rilis — v0.3 Beta

v0.3 mengubah fondasi 0.2 Beta menjadi model proses & memori yang nyata,
lalu membuat OS membangun userland-nya sendiri: dari **11 tools menjadi
29 tools yang seluruhnya di-compile di dalam OS** oleh `eqbuild`
(builtin `help` dihapus — daftar command pindah ke dokumentasi/README).
Semua perubahan dikawal regresi QEMU penuh: **101/101 PASS**.

---

## Fitur unggulan (headline v0.3)

### 1. Self-hosting — OS membangun userland-nya sendiri (`eqbuild`)
ISO mengirim **source C** (bukan binari) dari 29 tools userland ke
`/equinox/tools`. Perintah `eqbuild` meng-compile semuanya dengan
compiler in-OS `mtcc` — satu log per file, sumber dihapus setelah
berhasil — lalu barulah shell memakainya. Buktinya konsisten:
output compiler in-OS **byte-identical** dengan compiler host (ceksum
`head.mrp`/`wc.mrp` sama persis), jadi toolchain parity terjaga.

### 2. Multitasking + demand paging
Scheduler round-robin preemptive (quantum per tick, FPU state
`fxsave/fxrstor` per task), 2 console virtual (**F1/F2**), `ps` / `kill`
/ `switch`, `wait` + pipe POSIX-style, zombie & orphan handling.
Per-task VMA di-*reserve* dengan penanda `PTE_DEMAND`; page fault
mengalokasikan satu halaman zero-fill per sentuhan pertama — arena 24 MB
DOOM hanya "membayar" halaman yang benar-benar dipakai, jadi DOOM dan
program lain hidup berdampingan.

### 3. Jaringan nyata sampai HTTPS
lwIP 2.1.3 di atas NE2000 ISA: DHCP, DNS, ICMP, TCP. Client `mget`
mengunduh `http://` dan `https://` (TLS 1.2 via BearSSL, validasi rantai
ketat terhadap 9 root CA Mozilla; fallback parse-only diberi peringatan
jelas). Server `httpd` menyajikan status + file RAMFS di port 80
(dari host: `http://localhost:8080/`).

### 4. FAT32 baca-tulis yang persisten
Driver ATA PIO (LBA28/48, MBR) + FAT32 read-write: LFN dengan 8.3
mangling benar, FSInfo terjaga, direktori tumbuh on-demand,
write-through per close, rollback bila I/O gagal, mini-fsck
host-verified sampai 100% utilisasi. Disk auto-mount di `/mnt`; file
hasil `mget` ke `/mnt` **tetap ada setelah reboot**. DOOM jalan
full-speed langsung dari disk (`doom -iwad /mnt/doom1.wad`).

### 5. Compiler C + libc di dalam OS
`mtcc` meng-compile & menjalankan source C live di ring 3 (prelude
`<morph.h>`/`<multitasking.h>`/`<fileio.h>`). libc subset di-render
**lokal** di program (bukan round-trip ABI): printf/sprintf/snprintf
hingga 5 argumen konversi (`%u` unsigned sejati), sscanf, ctype, string
extras, qsort, rand — 54 tahap self-test lolos in-OS. Fungsi
dideklarasikan-tak-terdefinisi kini gagal di "link" dengan nomor baris
call-site pertama.

### 6. Grafis per-task
VESA 1360x768@32bpp. Setiap task punya *draw window* sendiri
(`set_clip` #53) — putpixel/fillrect/line ter-clip; `draw_line` (#54)
Bresenham focus-gated. Semantik canvas jujur: gambar pertama mensnapshot
layar teks lalu membersihkan hitam; teks tetap termirror ke serial +
cell mirror; console di-render ulang saat program exit.

### 7. PCI + driver registry
Enumerasi bus PCI (0xCF8/0xCFC, BAR, IRQ routing, rekursi bridge) saat
boot — `lspci` menampilkan tabelnya. NIC diabstraksi registry
(`kernel/net/nic.c`); NIC PCI yang dikenali tapi belum berdriver
(e1000/pcnet32/eepro100) dilaporkan jujur, bukan didiamkan.

---

## Fitur tambahan (tools release — 18 tools baru)

Semua di-compile in-OS oleh `eqbuild`, dipanggil lewat dispatcher global
yang sama dengan mtcc/snake:

| Tool | Sorotan |
| --- | --- |
| `grep` | `-i -n -c -v`, multi-file (prefix `nama:`), mini-regex: `.` `X*` `^` `$` |
| `head` / `tail` | N baris pertama/terakhir (`-n N`; tail ring 64×256) |
| `wc` | `-l -w -c` (default ketiganya + nama file), binary-safe |
| `sort` / `uniq` | bubble + early-exit di store 32 KB; `sort -r`; `uniq -c` grup bersebelahan |
| `cut` | `-d DELIM -f LIST` (N, N-M, N-, daftar koma, escape `\t \n \0`) |
| `tr` | map SET1→SET2 + `-d`, range (`a-z`), repeat-char-terakhir |
| `rev` / `nl` | balik tiap baris / nomori baris `%6d` |
| `more` | pager 23 baris per halaman, sembarang tombol lanjut, `q` keluar |
| `find` | rekursif (depth 8), filter `-name SUBSTR` |
| `which` | cari NAME/NAME.mrp di path sistem (urutan shell sendiri) |
| `diff` | bandingkan baris per baris streaming, 20 diff pertama gaya `</>` |
| `strings` / `cksum` | deret printable (minlen) / checksum 32-bit + ukuran, multi-file |
| `basename` / `dirname` | pecah komponen path (pojok-pojok POSIX) |

Ditambah 11 tools yang sudah ada: `ls cat cp mv rm mkdir rmdir touch
stat fstest pipedemo`. Total **29**. Builtin `help` dihapus — command
tidak dikenal dilaporkan apa adanya; daftar command ada di
`docs/COMMANDS.md` dan README.

Fitur platform pendamping: syscall #49–#54 (`wait pipe meminfo spawn2
setclip drawline`), `SYS_ECHILD`/`SYS_EPERM`, loader ELF32 statis
(`elfdemo.elf`, exit status 42 via `spawn`+`wait`), `lspci`, `meminfo`,
`wait [pid]`.

---

## Perbaikan bug akar (yang layak diceritakan)

1. **Scheduler "instant sleep"** — `schedule()` kembali ke task yang
   baru mem-block dirinya sendiri saat tak ada task READY lain, lalu
   fix-up BLOCKED→RUNNING membatalkan sleep. Kini: jalur idle
   `hlt`-dengan-interrupt-on sampai interrupt mengubah keadaan;
   `sleep(3000)` terukur tepat 300 tick / 3.0 s wall; nettask tak lagi
   busy-spin 100% CPU.
2. **ABI drift karena stale object** — makefile tanpa dependency header:
   edit struct `Task` meninggalkan 38/46 object dengan layout lama
   (scheduler membaca `clip_h` sebagai `page_dir` → triple fault).
   Perbaikan: `-MMD -MP` + include `.d` + rebuild bersih.
3. **Urutan canvas** — snapshot+clear dijalankan *setelah* draw pertama
   sehingga menghapus draw itu sendiri. Kini mark sebelum piksel
   pertama.
4. **Heap kernel habis di tool ke-13** — 29 tools × ±34 KB `.mrp` +
   modul boot memenuhi heap 2 MB dan terfragmentasi. Heap kini 2-region
   (±3.1 MB) dengan guard adjacency antar-region di coalesce/realloc.
5. **stdio slot vs raw fd** — prelude `fgetc/fread` bekerja di slot
   tabel `fopen()`, bukan fd mentah `f_open()`; tool generasi pertama
   senyap membaca EOF. Seluruh tools line-oriented kini konsisten pakai
   `fopen/fgetc/fclose`.

## Kualitas rilis

- Regresi QEMU: **101/101 PASS** (4 suite: `regression_task3` 29,
  `regression_task2` 12, `regression_v03` 31, `regression_multitask` 29)
  — termasuk eqbuild 29/29, bukti piksel clip (159.677 px merah di
  dalam jendela 400×400), libc 54/54 in-OS, soak 60 detik.
- Parity compiler in-OS == host (byte-identical, ceksum cocok).
- Smoke tools 22/22 + host pre-validation 29/29.

## Batasan yang diketahui (jujur)

- Stack user 1 MB tidak auto-grow (guard page mematikan task) — by design.
- `MAX_TASKS` = 8; zombie memegang slot (zombie tertua "dicuri" saat
  tabel penuh).
- printf-family mtcc maksimal 5 argumen konversi per panggilan; sscanf
  5 output pointer.
- Pipe write yang memenuhi buffer lalu dibaca task yang sama =
  self-deadlock (terdokumentasi di `pipedemo.c`).
- Segmen ELF harus di `[0x800000, 0x2000000)`; heap task ELF fix 2 MB.
- Clip window tidak diwariskan lintas spawn dan di-reset saat program
  exit; piksel program grafis tidak dipertahankan setelah exit (console
  teks di-render ulang).
- `eqbuild` serial di task shell; tools hanya ada setelah dijalankan
  (boot baru membawa source — sengaja, itu inti self-hosting).
- Satu volume FAT32 mount pada satu waktu (`/mnt`), partisi primary MBR
  saja.

## Arah v0.4

Fitur berat yang sengaja ditunda: preemption/SMP penuh, VFS generik,
driver NIC PCI nyata (e1000), dan *mtcc meng-compile dirinya sendiri*
(self-host compiler penuh). Daftar lengkap ada di
`mrp_user/TARGETS.md` (FR tersisa Stage 4–5).
