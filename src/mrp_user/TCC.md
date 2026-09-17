# mtcc 0.3 — Equinox OS TinyCC: compile C **di dalam** Equinox OS

> *"tcc nya bisa compile file di dalam os morp sendiri"* — sekarang bisa
> (subset C yang meaningful, bukan C lengkap — baca bagian Bahasa).

`mrp_user/mtcc.cpp` adalah compiler subset-C satu file bergaya TinyCC
(single-pass, tanpa AST, codegen langsung x86-32) yang **berjalan sebagai
program `.mrp` biasa di dalam Equinox OS** dan memakai syscall `int 0x80`
untuk semua kebutuhan I/O-nya. Ini milestone pertama **Tahap 4
TARGETS.md** ("compile C di DALAM Equinox OS, bukan cross-compile dari host").

## Cara pakai (di Equinox OS)

```
root::users / $ mtcc test/hello.c          # compile & langsung jalan (ala tcc -run)
root::users / $ mtcc -c test/hello.c       # compile → hello.mrp di RAMFS root
root::users / $ run hello.mrp              # jalanin hasil compile (standalone!)
root::users / $ mtcc --debug test/primes.c # info compiler (file size, code/data,
                                           #  size, exit code) + output program
```

`mtcc` adalah **command global shell** (dispatch ke `mtcc.mrp` otomatis,
tanpa prefix `run`). Tanpa `--debug` output-nya persis seperti `run`:
hanya output program, tanpa chatter compiler. `--debug` (atau `-d`)
menyalakan baris `[mtcc] ...` — banner, jumlah byte file, ukuran
kode/data, exit code.

Output mode `-c` menulis ke **RAMFS root** dengan nama `<basename>.mrp`
(`hello.c` → `hello.mrp`). Kalau file sudah ada → di-overwrite total
(binary-safe, lewat syscall `mkfile`).

## Build & masukin ke ISO

```bash
make clean && make      # kernel + ISO (WAJIB — binary lama gak punya syscall 15/16)
make pack               # compile semua mrp_user/*.cpp → dist/*.mrp (termasuk mtcc.mrp)
                        # + copy test/*.c → dist/ (sample .c)
make iso                # ISO baru: module GRUB = semua .mrp + .c di dist/
make run                # boot QEMU
```

File `.c` di `dist/` otomatis jadi multiboot module → `mrp_bootloader`
memasukkannya ke RAMFS `/test` saat boot → langsung bisa `mtcc test/hello.c`.
Sample test/ juga dipakai harness host (`make test`).

**Jalan cepat tanpa rebuild ISO** (mode VGA text, tetap full fungsi):

```bash
qemu-system-i386 -m 64 -kernel dist/kernel.elf \
    -initrd "dist/mtcc.mrp,dist/hello.c"
```

## Bahasa yang didukung (subset sengaja — v0.1)

| Kategori | Didukung | Belum |
|---|---|---|
| Tipe | `int`, `char`, `void`, pointer 1–2 level, array 1D | struct/union, float/double, unsigned (int diproses **signed**), long/short, typedef |
| Statement | if/else, while, do-while, for (+deklarasi di init C99), return, break, continue, blok | switch, goto |
| Operator | `= += -= *= /= %= <<= >>= &= \|= ^=`, `+ - * / %`, `<< >>`, `& \| ^ ~`, `&& \|\| !`, `== != < > <= >=`, unary `- + * &`, `++/--` (pre/post), `?:` | comma operator, sizeof |
| Global | skalar + array + init konstanta `{...}`/string (zero-init) | init non-konstanta |
| Fungsi | prototipe forward, rekursi, max 8 param, max 12 arg | variadik, function pointer |
| Lain | komentar `//` dan `/* */`, literal hex/desimal/char dengan escape, **default parameter `= const` (v10.8)** | function pointer, struct/typedef, cast, array-of-pointer |

Catatan semantik penting:
- `>>` = **arithmetic** shift (int dianggap signed, seperti compiler C biasa).
- Pembagian dengan nol → exception #DE → **kernel panic** (belum ada
  proteksi ring 3 — sama seperti akses pointer liar).
- Variabel global harus dideklarasi **sebelum** dipakai (single-pass).
- `p - p` menghasilkan selisih byte mentah (tidak dibagi ukuran elemen).

## Builtin — runtime-nya adalah OS itu sendiri

Program hasil compile **tidak punya libc**; fungsi berikut di-emit jadi
`int 0x80` inline dengan nomor syscall stabil
(`kernel/library/header/syscall.h` — single source of truth, tcc.cpp
include header yang sama saat build .mrp):

| Fungsi | Syscall | Catatan |
|---|---|---|
| `print(str)` | 10 | tanpa newline otomatis |
| `printint(num)` | 11 | **unsigned** (semantik kernel `print_int`) |
| `getkey()` | 8 | non-blocking, -1 kalau kosong |
| `readline(buf, maxlen)` | 9 | blocking, return panjang |
| `write(fd, buf, len)` | 4 | console (fd 1/2) |
| `open(path)` / `read(fd,buf,len)` / `close(fd)` | 6/5/7 | RAMFS read-only, sequential |
| `mkfile(path, buf, len)` | 16 | create/overwrite binary — dipakai tcc sendiri |
| `malloc(size)` | 12 | dari MRP arena (belum ada free per-alokasi) |
| `sleep(ms)` / `gettick()` / `getpid()` | 14/13/3 | |
| `getargs(buf, maxlen)` | 15 | argumen `run` terakhir |
| `exit(status)` | 1 | kembali ke pemanggil (shell) |
| `exec(path)` | 2 | **selalu ditolak: EBUSY (-9)** — lihat Limitasi |

### Builtins Morph.h (v10.3 — nama sama dengan SDK header)

SEBELUM v10.8 mtcc tidak punya preprocessor, sehingga `#include <Morph.h>` tidak bisa
dipakai in-OS. Sebagai gantinya, nama-nama API file Morph.h tersedia
langsung sebagai builtin — source yang ditulis "gaya Morph.h" compile
dua jalur (hosted pakai header asli, in-OS pakai builtin):

| Fungsi | Syscall | Catatan |
|---|---|---|
| `file_open(path)` | 6 | alias `open` |
| `file_read(fd, buf, len)` | 5 | alias `read` |
| `file_close(fd)` | 7 | alias `close` |
| `file_write(path, buf, len)` | 16 | alias `mkfile` — **create atau TIMPA total** |
| `file_read_all(path, buf, maxlen)` | 17 | baca seluruh file sekali panggil |
| `file_size(path)` | 18 | ukuran file (untuk alokasi buffer) |
| `file_exists(path)` | 19 | probe murah: 1/0, tanpa fd |

Pola "edit file" standar: `file_size` → `file_read_all` → ubah di
memori → `file_write` (timpa). Contoh lengkap: `test/morphio.c`
(regression 16-tahap, jalan di `make test` host maupun `mtcc
test/morphio.c` in-OS).

## Arsitektur (bergaya tcc: satu pass, tanpa AST)

```
source .c ──lexer──> token ──parser recursive-descent──> codegen langsung
                                                        │
                    ┌───────────────────────────────────┤
                    ▼                                   ▼
             code buffer (x86-32)                 data buffer
             rel32 antar-fungsi + FIXUP           string literal + global
                    │                                   │
        ┌───────────┴───────────┐                       │
        ▼ mode RUN              ▼ mode -c               │
  patch dgn alamat buffer   patch dgn base 0x500010 ────┤
  → panggil entry langsung │  (MRP_LOAD_BASE, sama      │
    (ala `tcc -run`)       │   dengan link_mrp.ld)      │
                           ▼                            │
                [header .mrp 18 byte][code][pad][data] ─┘
                → SYS_MKFILE ke RAMFS → bisa `run out.mrp`
```

- **Fixup**: daftar lokasi 4-byte yang baru tahu isinya belakangan
  (jump maju, call forward, alamat global). Mode `-c` patch dengan
  `0x500010` — persis alamat yang diberikan `mrp_alloc()` pertama di arena
  yang di-reset (deterministik, lihat komentar `link_mrp.ld`).
- **Header + checksum** tidak di-hardcode dua kali: tcc.cpp
  `#include` **mrp_format.h kernel** dan memvalidasi image-nya sendiri
  (`is_valid_mrp`) sebelum menulis ke RAMFS.
- **Assignment tanpa AST** (trik rollback): parse dulu LHS sebagai nilai;
  kalau token berikutnya operator assignment → rollback state lexer+code
  → parse ulang lewat jalur lvalue. String aman karena dedup idempotent.
- **Calling convention internal** (kode hasil compile cuma memanggil
  kode hasil compile + builtin): argumen dievaluasi kiri→kanan lalu
  di-push (urutan parse = urutan push, gak perlu AST buat balik urutan);
  callee baca param i di `[ebp + 8 + 4*(n-1-i)]`.

## Sudah diuji (host + in-OS)

Test harness `scripts/tcc_host_test/` (jalan di Linux biasa):
- **Interpreter x86-32** mengeksekusi persis set instruksi yang mtcc emit
  (set tertutup — instruksi liar = test langsung merah).
- 12 sample × 2 mode (run & compile→.mrp→jalankan di base 0x500010 persis
  seperti loader kernel): **output harus identik dengan harapan byte-per-byte**.
- File `.mrp` hasil mode `-c` divalidasi **independen dari Python**
  (magic/version/size/entry/checksum rotate-xor — algoritma sama dgn kernel).
- Sinkron nomor syscall mtcc.cpp ↔ kernel/syscall.h dicek otomatis.
- Negative test: source salah → pesan error baris-N yang jelas, bukan crash.
- **Jaring anti-memori-bersih**: semua `os_alloc()` mode host diisi pola
  kotor `0xA5` dulu — bug "bergantung memori nol" (lihat v2.5) langsung
  merah di host juga, bukan cuma in-OS.

```
== HASIL: 27 PASS, 0 FAIL ==
```

**In-OS (boot ISO asli di qemu + font-OCR)**: 16/16 — boot VESA, prompt
`root::users / $`, mtcc quiet & `--debug` (primes.c lengkap + exit code),
editor (title/status/gutter/syntax), exit editor, unknown command.
Plus sekuens multi-run: `mtcc hello → primes → primes → edge → -c primes →
run primes.mrp → exec_test → arr → varidx → pl1..pl5` semuanya benar.

### Bug penting yang sudah difix — "identifier tidak dikenal" di run ke-2 (v2.5)

**Gejala**: `mtcc test/hello.c` lalu `mtcc test/primes.c` →
`mtcc: error line 7: identifier tidak dikenal: sieve`. Run PERTAMA setelah
boot selalu benar; run berikutnya gagal tergantung program sebelumnya.
Host test selalu hijau (proses baru per test).

**Akar masalah**: tabel global (`S.gvars`) dialokasikan dari **arena .mrp
yang didaur ulang** antar run mtcc (alamat sama, isi lama, tidak di-zero).
Registrasi nama global menyalin nama **tanpa NUL terminator** → nama baru
menempel ekor nama lama: `sieve` + `ing` (sisa `greeting` dari hello.c)
= `sieveing` → `find_gvar("sieve")` gagal. Mengapa run pertama aman: RAM
segar = nol. Mengapa pl*.c→primes aman: nama 1 huruf + ekor masih nol.

**Fix** (dua lapis, saling menguatkan):
1. `parse_gvar_one` menulis `g->name[nlen] = '\0'` eksplisit (clamp ke
   `MTCC_NAME_MAX-1`).
2. `mtcc_compile` me-zero seluruh tabel gvar saat alokasi (prinsip sama
   dengan zero-init data area).

Plus: error mtcc sekarang menyertakan **nama identifier yang gagal** dan
(bila terjadi) dump state `[dbg] fn/gv/lc/sd/fx/pos/tok + isi tabel` —
diagnostik seperti inilah yang menuntun ke akar masalah ini dalam satu
repro. Jaring `0xA5` di harness host menjaga kelas bug ini tetap merah
kalau kambuh.

## Limitasi (jujur — jangan bikin ekspektasi palsu)

1. **exec() dari kode hasil compile selalu ditolak** (`SYS_EBUSY` = -9,
   FIX audit V3 #1): kode caller hidup di arena MRP yang sama dengan kode
   target exec — kernel menolak nested exec supaya arena tidak di-reset
   dan menimpa kode yang sedang berjalan (sebelum fix: panic deterministik).
   Program pemanggil tetap hidup setelah `exec()` balik. Pola yang benar:
   compile dulu (`-c`), lalu `run hasil.mrp` dari shell.
2. **Ring 0**: program hasil compile (dan tcc sendiri) punya akses penuh
   memori — pointer liar = kernel panic. Isolasi datang di Tahap 3
   (Ring 3), bukan sekarang.
3. **Stack pinjam kernel**: rekursi sangat dalam (>~1000 frame) bisa
   menghabiskan stack → panic. `fib(20)` aman, `fib(1000)` tidak.
4. `malloc()` hasil compile gak punya `free()` (arena di-reset per run —
   desain MRP arena yang sekarang).
5. Buffer compile: source ≤ 96KB, kode ≤ 384KB, data ≤ 128KB per program
   (cukup untuk tool shell; kebesaran → pesan error bersih, bukan overflow).
6. `.bss` program hasil compile ditulis sebagai nol literal di file .mrp
   (sama seperti program .mrp lain — hindari global array raksasa).

## Jarak ke TinyCC asli (Fabrice Bellard)

mtcc = fondasi pipeline-nya: source→code→`.mrp`→run, plus syscall ABI &
tooling (getargs/mkfile, ISO module .c). Yang BELUM dan realistis
butuh waktu berminggu-minggu—bulan:

1. **Preprocessor** (#include/#define) — sebelum port tcc asli, ini
   penghambat terbesar buat program nyata.
2. Struct/union/typedef, array multidimensi, unsigned semantik.
3. Port **libtcc** asli (~80k baris): butuh libc port di userland
   (malloc dengan free, stdio buffered, dsb.) — jalannya lewat Tahap 3
   (Ring 3) dulu supaya tcc crash gak bawa kernel.
4. ELF→`.mrp` backend writer di tcc asli (Tahap 4 checklist TARGETS.md).

Urutan yang disarankan tetap seperti TARGETS.md: stabilkan dulu
(heap/paging), baru Ring 3, baru tcc asli. mtcc ini jadi
"konfirmasi jalur" bahwa seluruh pipeline .mrp + syscall siap
menerima compiler beneran.

## Preprocessor mini + libc prelude (v10.8)

mtcc 0.3 punya preprocessor satu-pass (semantik cpp: makro didefinisikan
saat ditemui, `#ifdef` melihat tabel makro saat itu, include di-splice
di posisinya):

| Directive | Perilaku |
|---|---|
| `#include <morph.h>` | splice **libc prelude** built-in. Alias: `<stdio.h>` `<stdlib.h>` `<string.h>` `<Morph.h>` (semuanya prelude yang sama) |
| `#include "file.h"` | baca file RAMFS (relatif cwd proses), splice rekursif maks 8 level; guard `#ifndef` bekerja |
| `#define NAME nilai` | macro object-like; substitusi identifier-boundary, string/char/komentar tidak disentuh; **function-like macro ditolak** dengan pesan jelas |
| `#undef NAME` | hapus makro |
| `#ifdef` / `#ifndef` / `#else` / `#endif` | inklusi kondisional (stack per-file, tidak menyeberang include) |
| `#pragma` / `#error` / `#` | diabaikan |

Program TANPA directive lolos verbatim — binary tetap kecil; prelude
hanya masuk kalau di-`#include`.

### Isi prelude (`#include <morph.h>`)

- **memory/string**: `memcpy memset memmove memcmp memchr strlen strcmp
  strncmp strcpy strncpy strcat strncat strchr strrchr strstr`
- **konversi**: `atoi strtol itoa utoa`
- **heap user-space**: `malloc free calloc realloc` — free-list + split +
  coalesce di atas chunk `__arena_alloc` (arena kernel 4MB, di-reset saat
  program exit). `free()` benar-benar mengembalikan memori.
- **printf family**: `printf(fmt, a=0, b=0, c=0)` via syscall 28
  (maks 3 konversi per panggilan — slot arg ABI kernel), `snprintf`
  renderer penuh (`%d %u %x %X %c %s %%`, width, zero-pad, left-align),
  `sprintf`
- **stdio FILE I/O**: `fopen(path, mode=0)` mode `"r"` = fd langsung
  (`fseek` via syscall `lseek`), `"w"`/`"a"` = write-buffer dinamis yang
  di-flush `fclose` lewat `file_write`. `fread fwrite fseek ftell fclose`.
  Handle 1..8 (0 = NULL).
- **sort**: `qsort_int(int*, n)` dan `qsort_str(char**, n)` — qsort
  generik butuh function pointer (belum ada di subset mtcc); jalur
  hosted Morph.h punya `qsort_()` generik penuh.
- **misc**: `time()` (detik sejak boot), `getenv()` (selalu NULL — belum
  ada environment block), `abort()` (exit 134), `lseek(fd,off,whence)`,
  `ring()` (CPL pemanggil — program membuktikan sendiri dia ring 3).

### Ekstensi bahasa kecil: default parameter

`int fopen(char* path, char* mode = 0)` — parameter dengan nilai default
konstanta; call dengan argumen kurang otomatis push default (bergaya
C++). Inilah yang membuat `printf("hi\n")` sah tanpa varargs.

### 3 bug compiler lama yang ditemukan & difix saat audit v10.8

1. `gen_logor`/`gen_logand` selalu menimpa tipe hasil ekspresi dengan
   `int` — `(g + 5)[0]` (index ekspresi dalam kurung) gagal compile.
2. `gen_lvalue` untuk `p[i] = v` memakai **alamat slot** pointer, bukan
   **nilainya** — assignment lewat index pointer menulis ke stack.
   Jalur value (`v = p[i]`) benar; jalur lvalue salah (tak pernah
   ter-expose oleh test lama).
3. Array-of-pointer (`char* a[5]`) memang belum didukung (didokumentasikan;
   workaround: `int a[5]` berisi alamat, assignment longgar).
