# EGGKG — Package Manager Equinox OS (Desain v1)

> Status: **IMPLEMENTASI v0.9 — SELESAI & TERUJI** (build 2026-10-03).
> eggkg adalah **builtin shell** (deviasi terdokumentasi dari §5: jaringan
> lwIP/BearSSL + spawn mtcc tinggal di ring 0; versi userland menyusul saat
> ring-3 net API tersedia). Teruji QEMU: offline 23/23 PASS
> (scripts/eggkg_test.py) + online repo GitHub asli 8/8 PASS
> (scripts/eggkg_net_test.py, raw.githubusercontent.com TLS).
> Pelajaran implementasi penting lihat §14.

---

## 1. Ringkasan

eggkg adalah package manager Equinox OS dengan filosofi **Gentoo/portage mini**:
paket didistribusikan sebagai **sumber kode + resep build (`.ruf` v2)**, lalu
dibangun **di mesin tujuan** oleh `mtcc` lewat `equinoxinstall -build`. Tidak ada
biner pra-jadi di repo — jadi tidak ada masalah ABI, dan repo tetap kecil.

Konsep inti (sesuai usulan, dengan koreksi):

| Usulan | Keputusan desain |
|---|---|
| bash jadi bagian eggkg, install bash dulu | ✅ Ya. Paket pertama = `bash`. Catatan jujur: GNU bash asli (150 ribu baris, penuh `struct`) **tidak mungkin** dikompilasi mtcc. "bash" = shell baru gaya POSIX ditulis khusus Equinox (`eqbash`) + coreutils ringan |
| Tanpa bash: cfile, ccfile, save, ls→`lf`, showf | ✅ Ya. Builtin kernel di-rename: `ls`→`lf`, `cat`→`showf`. Nama standar (`ls`, `cat`, `mkdir`, ...) jadi milik paket bash |
| Folder paket `equinox/.local/bash/build.ruf` | ✅ Ya, dipertahankan persis. `.local/<pkg>/` = workspace (sumber + build.ruf) |
| `eggkg install nama`, `eggkg update` | ✅ Ya. Plus `remove`, `list`, `search`, `info` |
| `system.ecf` header `[dependencies] bash = True` | ✅ Ya, dengan 2 koreksi: ejaan benar `[dependencies]`, dan nilai `true/false` huruf kecil (konsisten schema ecf) |
| Upgrade `.ruf` agar `equinoxinstall -build /path/build.ruf` | ✅ Ya → format `.ruf` v2. Jalur build **tidak berubah**: eggkg menurunkan v1 murni untuk mtcc |
| Server GitHub, `bash = ["url1","url2"]` | ⚠️ Ide mirror-list dipertahankan, tapi ditaruh di field `mirror` **per seksi paket**, karena butuh `version`, `sha256`, `size` agar eggkg bisa cek integritas & versi |
| Ada animasi | ✅ Ya → library `egg_progress` (spinner + progress bar). Rinci di §9 |

Fondasi yang **sudah ada** di OS (diroktes langsung ke kode, Oktober 2026):

- `mget` mendukung **HTTPS** (TLS 1.2 BearSSL, validasi chain ke CA Mozilla
  embedded — `kernel/net/tls_client.c`, `net_lwip.c`). Jadi
  `https://raw.githubusercontent.com/...` bisa dijangkau **hari ini**, tanpa
  kerja TLS baru.
- Shell punya **system path 4 direktori**: `/`, `/bin`, `/equinox/tools`,
  `/equinox/games` (shell.cpp:499-509) — program `.mrp` di `/bin` otomatis
  ditemukan, dan **userland menimpa builtin** (`try_run_tool`). Infrastruktur
  "paket menyediakan perintah" sudah separuh jadi.
- Task 11 sudah memindahkan `cat/grep/wc/echo` ke userland + pipe/redirect/glob
  di eqshell → animasi bisa fallback rapi saat output di-pipe.
- `ecf.c` = parser INI + schema `{key, nilai-boleh, perlu-reboot}` — menambah
  `[dependencies]` dan `[eggkg]` = 4 baris schema.
- `equinoxinstall -build /path/ke/file.ruf` sudah teruji menerima path absolut.

---

## 2. Peta Folder

```
/equinox/
  .local/                          <- rumah eggkg (usulan user, dipertahankan)
    index.idx                      <- salinan index dari server (eggkg update)
    installed.db                   <- basis data paket terpasang (INI)
    cache/
      bash-0.1.0.ruf               <- unduhan mentah (untuk reinstall offline)
    bash/                          <- workspace per paket (usulan user)
      build.ruf                    <- resep v1 MURNI yang diturunkan eggkg (untuk mtcc)
      src/
        main.c
        ...
  bin/                             <- tujuan [install] bin (SUDAH system-path dir!)
    bash.mrp
    ls.mrp
  tools/                           <- tool inti bawaan ISO (termasuk eggkg.mrp)
  games/
  etc/ -> (via /mnt/boot saat terpasang) system.ecf
```

Catatan keputusan:

- **`/bin` dipilih** sebagai tujuan biner paket karena sudah menjadi system
  path dir ke-2 di shell — nol perubahan resolusi perintah. `/equinox/tools`
  tetap milik tool inti ISO; pemisahan "inti vs paket" jadi jelas.
- `.local/<pkg>/` adalah *workspace* (sumber + resep), bukan tempat biner.
  Biner hasil build dikop ke `/bin` sesuai seksi `[install]`.
- FAT32 tidak punya symlink → `[install]` selalu **copy**.

---

## 3. Format `.ruf` v2

`.ruf` v1 (sekarang): baris `echo/src/exclude/out/lib` + baris telanjang
ber-path. v2 adalah **superset INI** — tetap satu file, tapi punya metadata
untuk eggkg:

```ini
# bash-0.1.0.ruf — paket eggkg
[package]
name    = bash
version = 0.1.0
desc    = Shell + coreutils standar Equinox
author  = equinox
license = MIT
deps    =                        # kosong, atau "coreutils, grep" dipisah koma

[build]                          # == semantik v1, dibaca mtcc -make
src /equinox/.local/bash/src
out bash
echo "membangun bash 0.1.0"

[install]                        # dibaca eggkg (bukan mtcc)
bin = bash -> /bin/bash.mrp
etc = bashrc -> /etc/bashrc

[src:main.c]                     # sumber ter-embed (heredoc, opsional)
<<EGGKG_SRC_main.c
/* isi file main.c apa adanya */
int main(void) { return 0; }
<<EGGKG_END
```

Aturan:

1. **mtcc tidak perlu diubah.** eggkg yang mem-parsing v2, mengekstrak seksi
   `[src:*]` ke `.local/<pkg>/src/`, lalu **menurunkan `build.ruf` v1 murni**
   (hanya isi `[build]`) ke `.local/<pkg>/build.ruf`, dan memanggil
   `equinoxinstall -build /equinox/.local/<pkg>/build.ruf` — persis perintah
   yang diusulkan.
2. File `.ruf` v1 lama tetap sah sebagai paket minimal (eggkg menganggapnya
   `[build]` tanpa `[package]`, name diambil dari nama file).
3. Sumber ter-embed pakai **heredoc teks** (bukan base64) — konsisten dengan
   mekanisme `save << "teks"` yang sudah ada, dan tetap bisa dibaca manusia.
   Terminator unik per file mencegah tabrakan isi. Aset biner (base64) baru
   dipertimbangkan di v2 format.
4. Alternatif tanpa embed: seksi `[package]` boleh punya `srcurl = <url>` —
   eggkg mengunduh arsip sumber terpisah. Dipakai untuk paket besar nanti.

---

## 4. Server GitHub: layout repo + `index.idx`

> **Update 2026-10-03 — server pertama sudah tayang.** Repo
> `amnottdevv/Eggkg-l` memakai format **`package.list`**:
>
> ```
> bash = ["https://github.com/amnottdevv/Eggkg-l/blob/main/bash/cat.c", "..."]
> ```
>
> Keputusan: eggkg v0 mem-parse format ini apa adanya — konversi
> `blob/main/` → `raw.githubusercontent.com/.../main/` wajib dilakukan
> parser (URL blob = halaman HTML, bukan file mentah), lalu tiap `.c`
> diunduh ke `/equinox/.local/<pkg>/src/`, `build.ruf` diambil dari
> `<pkg>/build.ruf` di repo yang sama, dan build dijalankan:
> `mtcc -make /equinox/.local/<pkg>/build.ruf`. Format `index.idx`
> (dengan version/sha256/size) tetap menjadi jalur upgrade integritas
> di fase E4.

Satu repo GitHub = satu kanan repositori. Dipakai **raw.githubusercontent.com**
(sudah terbukti jangkauan mget HTTPS).

### 4.1 Layout repo

```
equinox-pkg/eggkg-repo/            (nama placeholder — bisa diganti)
  index.idx                        <- DATABASE (satu file, INI)
  pkg/
    bash/
      bash-0.1.0.ruf               <- satu file per rilis (v2, sumber ter-embed)
      bash-0.2.0.ruf
    coreutils/
      coreutils-0.1.0.ruf
  tools/
    mkindex.py                     <- generator index (dijalankan di PC)
```

### 4.2 Format `index.idx`

```ini
[meta]
format   = 1
updated  = 2026-10-03
revision = 7

[bash]
version = 0.1.0
desc    = Shell + coreutils standar Equinox
size    = 24680
sha256  = 9f2c...                 # hash file .ruf (bukan isi sumber)
url     = https://raw.githubusercontent.com/equinox-pkg/eggkg-repo/main/pkg/bash/bash-0.1.0.ruf
mirror  = https://cdn.jsdelivr.net/gh/equinox-pkg/eggkg-repo@main/pkg/bash/bash-0.1.0.ruf,http://mirror.local/eggkg/bash-0.1.0.ruf
deps    =

[coreutils]
version = 0.1.0
...
```

**Koreksi atas usulan `bash = ["url1","url2"]`:** daftar URL polos rapuh karena
eggkg tidak tahu versi, ukuran, dan hash — jadi tidak bisa mendeteksi unduhan
korup, tunggakan versi, maupun menampilkan "bash 0.1.0 tersedia". Bentuknya
tetap hidup sebagai field **`mirror`** (daftar URL dipisah koma, fallback
berurutan), sementara `url` = primer. Nambah paket = tambah 1 seksi + 1 folder
di `pkg/` — tetap segampang usulan awal.

### 4.3 Nambah paket (alur kontributor)

```
1. fork repo, buat pkg/<nama>/<nama>-<versi>.ruf   (v2, sumber ter-embed)
2. python3 tools/mkindex.py                        (scan pkg/ -> sha256+size -> tulis index.idx)
3. Pull request -> merge
4. Di Equinox: eggkg update && eggkg install <nama>
```

`mkindex.py` (~80 baris, PC) menghitung sha256/size otomatis — index tidak
pernah diedit tangan, jadi hash tidak akan pernah salah.

### 4.4 Kanal & versioning

- Branch `main` = stabil. Tag rilis (`v1`, `v2`) menyimpan index beku untuk
  reproduksi.
- eggkg bisa dipatok kanal lewat `[eggkg] server=...` di system.ecf (§7).

---

## 5. Perintah `eggkg`

eggkg sendiri adalah tool userland (`/equinox/tools/eggkg.mrp`, ditulis gaya
mtcc: C tanpa `struct`, arena RAM) yang ikut ISO — jadi tidak ada masalah
ayam-telur: eggkg tersedia bahkan sebelum paket pertama terpasang.

| Perintah | Fungsi |
|---|---|
| `eggkg update` | Unduh `index.idx` dari server (+mirror) → `/equinox/.local/index.idx`. Lapor jumlah paket |
| `eggkg install <nama>` | Resolve di index → tutup dependency (rekursif) → unduh `.ruf` → cek `sha256`+`size` → ekstrak sumber → turunkan `build.ruf` v1 → `equinoxinstall -build` → salin `[install]` → catat `installed.db` → post-hook |
| `eggkg remove <nama>` | Hapus file tercatat di installed.db + cek reverse-dependency dulu |
| `eggkg list` | Paket terpasang (nama + versi) |
| `eggkg search <pola>` | Cari di index.idx (glob satu `*`) |
| `eggkg info <nama>` | Metadata paket (dari index / installed.db) |
| `eggkg upgrade <nama>` | install ulang bila versi index > terpasang (fase lanjut) |

Alur `eggkg install bash` (v1):

```
1  resolve "bash" di .local/index.idx      (belum ada? -> "jalanin eggkg update dulu")
2  dep closure: deps kosong -> [bash]
3  per paket yang belum terpasang / versi beda:
   a. unduh url -> mirror fallback        -> .local/cache/bash-0.1.0.ruf
   b. verifikasi size + sha256
   c. ekstrak [src:*]                     -> .local/bash/src/...
   d. turunkan build.ruf v1               -> .local/bash/build.ruf
   e. equinoxinstall -build /equinox/.local/bash/build.ruf
   f. salin [install]: bin -> /bin/...
   g. catat installed.db
4  post-hook: paket bash -> tawar tulis [dependencies] bash=true (y/n)
5  ringkasan + petunjuk
```

---

## 6. Basis Data Terpasang — `installed.db`

INI polos (sama gaya ecf), dibaca/ditulis eggkg tanpa library baru:

```ini
[meta]
format = 1

[bash]
version = 0.1.0
date    = 2026-10-03
files   = /bin/bash.mrp,/etc/bashrc
deps    =
```

`eggkg remove` memakai `files` untuk hapus bersih; reverse-dependency dicek
dulu (paket lain yang `deps` memuat nama ini → tolak + sebut).

---

## 7. Konfigurasi: `system.ecf`

### 7.1 Seksi baru

```ini
[dependencies]
bash = true

[eggkg]
server = https://raw.githubusercontent.com/equinox-pkg/eggkg-repo/main/index.idx
mirror = http://mirror.local/eggkg/index.idx
local  = /equinox/.local
```

### 7.2 Perubahan kode (kecil, presisi)

`kernel/library/ecf.c` — schema bertambah 4 entri (pola sama seperti
`net.driver`):

```c
{ "dependencies.bash", "true|false", 0 },
{ "eggkg.server",      NULL,         0 },
{ "eggkg.mirror",      NULL,         0 },
{ "eggkg.local",       NULL,         0 },
```

Parser ecf sudah menggabungkan `[section]` + key → `section.key` otomatis
(ecf.c:264), jadi tidak ada perubahan parser.

### 7.3 Semantik `[dependencies]`

- `dependencies.bash = true` berarti **auto-detect**: saat shell disiapkan,
  sistem cek `/bin/bash.mrp` ada → jalankan bash sebagai shell interaktif;
  tidak ada → lanjut eqshell + satu peringatan:
  `eggkg: dependencies.bash=true tapi /bin/bash.mrp tidak ada — pakai eqshell`.
- Bash keluar/crash → kembali ke eqshell (eqshell = mode penyelamatan yang tak
  pernah hilang).
- `eggkg install bash` menulis seksi ini **setelah konfirmasi y/n**; nilainya
  tidak memaksa reboot (flag schema = 0).

### 7.4 Koreksi ejaan

Header usulan tertulis `[dependecies]` → dipakai `[dependencies]` (benar).
Nilai `True` kapital → `true` (ecf_same case-sensitive, schema enum
`true|false`).

---

## 8. Rename Builtin & Lini Dasar Tanpa Bash

Tujuan: nama standar Unix diserahkan ke paket; kernel tetap punya lini dasar
penyelamatan yang tidak tabrakan.

| Lama | Baru (builtin eqshell) | Catatan |
|---|---|---|
| `ls`, `ls -l` | `lf`, `lf -l` | alias `ls` DIHAPUS dari builtin; `ls` hanya hidup jika `ls.mrp` ada (paket) |
| `cat` | `showf <file>` | `cat` = milik paket (cat.mrp Task 11 sudah ada); builtin jadi `showf` |
| `cfile`, `ccfile`, `save`, `cdir`, `cd`, `pwd`, `mount`, `Qfs`, `set`, `copy`, `del` | tetap | lini dasar tak tersentuh |

Perilaku saat nama paket belum ada, mis. user mengetik `ls`:

```
ls: perintah tidak ada (bash belum terpasang)
    lini dasar: lf, showf, save, cfile, ccfile
    pasang:     eggkg install bash
```

Pesan hint ini penting agar muscle memory lama tidak menggantung. Semua dokumen
(`commands_quickref.md`, `COMMANDS.md`) di-update bersamaan.

---

## 9. Spesifikasi Animasi — `egg_progress`

Library kecil (header-only, `mrp_user/egg_progress.h`) yang dipakai eggkg dan
boleh dipakai tool lain (mis. equinoxinstall). Semua render satu baris pakai
`\r` + pad spasi; aman serial & konsol.

### 9.1 Komponen

**Spinner (progres tak tentu)** — frame `| / - \` cadang 100 ms:

```
[/] mengekstrak sumber bash...
```

**Bar (progres tentu)** — lebar 20 kolom:

```
[##########----------] 50%  12.1/24.3 KB
```

**Baris status selesai/gagal:**

```
[ok] bash-0.1.0.ruf terunduh (24.3 KB)
[gagal] sha256 tidak cocok (harus 9f2c..., dapat 00aa...)
```

**Header tahap bernomor:**

```
[2/4] mengunduh bash-0.1.0
```

### 9.2 Contoh output penuh `eggkg install bash`

```
eggkg: install bash
  [1/4] membaca index .................. [ok] (revision 7, 12 paket)
  [2/4] mengunduh bash-0.1.0
        [###############-----] 75%  18.2/24.3 KB
        [ok] sha256 terverifikasi
  [3/4] build (equinoxinstall -build)
        membangun bash 0.1.0
        src/main.c ....................... [ok]
        src/line.c ....................... [ok]
        out bash ......................... [ok]
  [4/4] memasang /bin/bash.mrp .......... [ok]
        tulis [dependencies] bash=true ke system.ecf? (y/n) y
        [ok]

  bash 0.1.0 terpasang. reboot (atau ketik bash) untuk mulai.
```

### 9.3 Aturan fallback

- **Output bukan tty** (di-pipe / di-redirect — mekanisme Task 11): tanpa `\r`
  dan tanpa bar progres; hanya baris tahap + `[ok]/[gagal]`. Jadi
  `eggkg install bash > log` tetap bersih.
- Spinner dipanggil dari loop nyata (per-chunk unduh, per-file ekstrak) — bukan
  thread terpisah, jadi tidak ada race dengan scheduler single-core.

### 9.4 Batas jujur untuk v1

Saat eggkg masih memanggil `mget` sebagai proses terpisah, bar persentase
unduhan tidak bisa dirender eggkg (mget yang pegang output). v1 memakai
spinner + ukuran akhir dari mget. Bar penuh datang di v1.1 ketika transport
http dipindah in-process (panggilan jaringan langsung di eggkg). Tahap
ekstrak/build/pasang tetap beranimasi penuh sejak v1.

---

## 10. Isi Paket `bash-0.1.0` (Scope Jujur)

GNU bash tidak mungkin dikompilasi mtcc (tanpa `struct/typedef/sizeof`).
`bash` versi Equinox = **eqbash**, shell userland bergaya mtcc:

- `bash.mrp` — loop readline → tokenize → glob (`*`, satu bintang, pola yang
  sama dengan eqshell Task 11) → pipe `|` dan redirect `> >> <` → resolve
  PATH (`/bin`, `/equinox/tools`) → spawn + wait (syscall pipe/wait sudah ada).
- coreutils sebagai `.mrp` terpisah agar bisa dipakai **tanpa** bash:
  `ls.mrp`, `mkdir.mrp`, `cp.mrp`, `mv.mrp`, `rm.mrp` (melengkapi cat/grep/wc/
  echo dari Task 11; `touch/stat` sudah ada di lini tool).
- `/etc/bashrc` — prompt + alias minimal.

Setelah bash aktif, pengalaman berubah level: `ls -l | grep eq` dan
`cat baca.txt > salin.txt` jadi idiom harian, sementara eqshell tetap ada di
bawahnya sebagai penyelamat.

---

## 11. Integritas & Keamanan

1. **sha256 + size** di index; eggkg menolak file yang tidak cocok (implement
   SHA-256 murni C loop ~150 baris di eggkg — tanpa struct, aman mtcc).
2. **Mirror fallback berurutan**: primer gagal/time-out → mirror berikutnya;
   semua gagal → pesan + saran `eggkg update`.
3. **Bukan root-of-trust kriptografis**: v1 belum ada signature. HTTPS ke
   raw.githubusercontent.com sudah melindungi jalur transport; signature paket
   (minisign-style) masuk backlog v2.
4. **remove berpagar**: reverse-dep check + file list hanya dari installed.db
   (tidak ada rm_rf liar).

---

## 12. Rencana Implementasi

Urutan sesuai usulan: **install → build → settings local folder**.

| Fase | Isi | Sentuhan kode utama | Bukti jadi |
|---|---|---|---|
| E1 | Format `.ruf` v2 + scaffold repo GitHub + `mkindex.py` | parser v2 (sisi eggkg), repo contoh `pkg/bash` | mkindex menghasilkan index.idx sah |
| E2 | `eggkg.mrp` inti: update/install/remove/list/search/info + installed.db + SHA-256 + spawn `equinoxinstall -build` | `mrp_user/eggkg.cpp` baru; nol perubahan mtcc | QEMU: install bash dari repo `file://` (ISO/disk) |
| E3 | Settings: `[dependencies]` + `[eggkg]` di ecf.c; auto-activate bash saat shell siap; rename `ls`→`lf`, `cat`→`showf` + pesan hint | ecf.c (+4 schema), shell.cpp (rename + spawn bash), docs | reboot → langsung prompt bash |
| E4 | Transport online: `eggkg update` via mget HTTPS (slirp) + mirror fallback + animasi penuh | eggkg spawn mget; egg_progress.h | QEMU slirp: `eggkg update` → `eggkg install bash` dari raw.githubusercontent.com |
| E5 | Lanjutan: upgrade, cache reinstall, kanal, signature | — | — |

Rencana uji (mengikuti harness `scripts/boot_test_v032.py`):

- `eggkg_test.py` — E1-E3: update offline (repo file:// di disk image), install
  bash end-to-end, sha256 salah ditolak, remove berpagar reverse-dep, rename
  lf/showf + hint, reboot → bash aktif.
- `eggkg_net_test.py` — E4: update via HTTPS slirp nyata, fallback mirror,
  animasi fallback saat di-pipe.

Catatan ketergantungan: tidak diblokir fase mana pun. Boot-from-disk (v0.8)
sudah jadi, jadi hasil eggkg **persisten** di disk; pipe/redirect Task 11
dipakai untuk fallback animasi.

---

## 13. Keputusan Terbuka (perlu setuju, tidak memblokir desain)

1. **Nama binary**: `eggkg` (dipakai di dokumen ini). Alias pendek `egg` bisa
   jadi symlink... FAT32 tak punya symlink → salinan kecil atau cukup satu nama.
2. **Nama repo GitHub**: `equinox-pkg/eggkg-repo` masih placeholder.
3. **eqbash sebagai pengganti "bash"**: GNU bash asli mustahil via mtcc; jika
   nama `bash` terasa berlebihan, alternatif: paket bernama `eqsh`.
4. **Rename keras** `ls`→`lf` / `cat`→`showf`: akan merusak kebiasaan lama —
   ada jendela transisi dengan pesan hint (§8) supaya aman.

---

## 14. Pelajaran Implementasi v0.9 (catatan engineering)

1. **BSS budget**: kernel BSS wajib berakhir di bawah 0x500000
   (`MRP_HEAP_START`; heap kernel = `_bss_end + 64 KB` s.d. 0x500000 + ext
   0x2702000-0x2800000 ≈ 1,09 MB total). Statik eggkg ~1 MB membuat
   `_bss_end` = 0x5CEB5C menimpa arena MRP → kernel panic page fault
   (CR2 0x5E03EA) bahkan pada `echo x > file`. Solusi: tabel paket dinamis
   (`malloc`), buffer fetch 64 KB (biasa) / 384 KB (arsip, dibebaskan
   sebelum build). `_bss_end` final = 0x4E4B9C.
2. **Heap live-ISO sempit** (~1,09 MB): 19 × 33 KB `.mrp` terduplikasi ke
   /bin = OOM. Solusi: **move** via `fs_ram_relink_node` (zero-copy, RAMFS);
   salin+hapus hanya untuk FAT (konten di fat arena, bukan kernel heap).
3. **NUL-termination transport**: `net_eggkg_fetch` wajib NUL-terminate body
   setelah memmove header-stripping. Tanpa itu `egg_v1_recipe` membaca sisa
   body pra-memmove → build.ruf terbaca ganda → mtcc 38 job (19×2).
4. **package.list v0**: build.ruf boleh jadi ANGGOTA daftar sumber (repo
   Eggkg-l melakukannya) — eggkg mendeteksi `.ruf`, menurunkannya lewat
   `egg_v1_recipe`, dan tidak menaruhnya di `src/`.
5. **Parser multi-format**: package.list mendukung satu baris ~1,5 KB,
   multi-baris, kutip/koma/bracket bebas; URL `github.com/.../blob/`
   dikonversi otomatis ke raw; path absolut `/...` = repo offline di FS.
6. **`showf`/`lf`** = nama baru builtin cat/ls; `ls`/`cat` tetap hidup
   (jendela transisi §8) dan kena timpa paket via system path.
7. **`[dependencies] bash=true`** ditulis install (y/n atau `-y`), di-patch
   `set`/remove, dibaca `eggkg_boot_check()` saat shell siap → sinkron
   .local → /bin + satu baris status; diam total bila tak ada paket.
