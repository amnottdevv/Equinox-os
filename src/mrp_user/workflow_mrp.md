# Workflow Pengembangan `.mrp` — dari Function-Pointer Table ke Native libc Port

Dokumen teknis, fokus 3 hal yang diminta:
1. `./file.mrp` bisa langsung dijalankan dari shell (bukan `run file.mrp`)
2. morphAPI — expose `stdio`, `libstring`, `itoa_atoi`, `vector`, dll ke
   program `.mrp`
3. Config libc yang nyambung ke kernel yang udah ada

Semua dipecah kecil & realistis. **Jangan implement semua sekaligus** —
tiap sub-tahap harus bisa di-test sendiri sebelum lanjut.

> **Update iterasi ini:**
> - Bagian A.1 + A.2 (alias `./` + auto-append `.mrp`) → **SELESAI**
>   (`kernel/kernel.cpp`, dispatcher shell).
> - Bagian B.1 Batch 1, 2, 3 (string, angka, vector) → **SELESAI**
>   (`mrp_user/mrp_api.h` canonical, `kernel/library/header/mrp_api.h`
>   shim, `kernel/library/mrp_api.cpp` wrapper, `mrp_loader.cpp`
>   menggunakan `mrp_build_api()`).
> - Single-source-of-truth untuk `struct mrp_api_t` → **SELESAI**
>   (definisi gak lagi diduplikasi antara kernel & userland).
> - Bagian C (libc port via static lib) → **masih TODO** (butuh Ring 3
>   dulu, lihat `TARGETS.md` Tahap 3).

---

## Bagian A — `./file.mrp` dari shell

### Kondisi sekarang
Command shell-nya masih `run <nama.mrp>` (lihat `kernel.cpp`, dispatcher
`starts_with(input, "run ")`). Ini udah cukup buat testing manual, tapi
belum senatural `./nama` di shell Unix.

### A.1 — Tambah alias `./` di parser shell (kecil, cepat) ✅
Di `shell()`, sebelum command lain dicek, tambah 1 case baru:

```cpp
else if (starts_with(input, "./")) {
    const char* name = input + 2;
    mrp_run(cwd, name);
}
```

**PENTING:** ini BARU sekadar alias tulisan. Belum ada:
- Cek extension `.mrp` otomatis (`./hello` tanpa `.mrp` belum jalan)
- Cek permission/executable-bit (RAMFS sekarang gak ada konsep permission
  sama sekali)
- Path relatif yang lebih dari 1 folder (`./bin/hello.mrp`) — tergantung
  `fs_find_child()` support subpath atau nggak, perlu dicek

**Kriteria selesai A.1:** `./hello.mrp` dan `run hello.mrp` menghasilkan
perilaku identik.

**Status:** SELESAI di `kernel/kernel.cpp`. Implementasi aktual juga
menambahkan A.2 (auto-append `.mrp`) karena cost-nya trivial — lihat
blok `else if (starts_with(input, "./"))` di dispatcher shell.

### A.2 — Auto-append `.mrp` kalau extension gak disebut ✅
```cpp
else if (starts_with(input, "./")) {
    char name[64];
    // ... copy input+2 ke name ...
    // kalau name belum diakhiri ".mrp", append otomatis
    mrp_run(cwd, name);
}
```
Ini kosmetik doang, prioritas rendah — jangan dikerjain sebelum Bagian B
kelar, karena gak nge-block apapun.

**Status:** SELESAI (ikut dikerjakan bareng A.1, lihat kernel.cpp).

### ⚠️ Blocker yang HARUS diselesaikan duluan, terlepas dari `./` atau `run`
Sama seperti disebut di iterasi sebelumnya: **belum ada jalur masukin
file `.mrp` ke RAMFS dari luar QEMU.** Sebelum `./file.mrp` ada gunanya
buat ditest, ini WAJIB kelar dulu:

- [ ] **Rekomendasi:** GRUB multiboot module. Tambah di `grub.cfg`:
  ```
  menuentry "Equinox OS" {
      multiboot /boot/kernel.elf
      module /boot/hello.mrp
  }
  ```
  Lalu di `kernel_main()`, setelah `fs_init()`, baca `mb_info->mods_addr`
  (array of `{mod_start, mod_end, string, reserved}`), dan panggil
  `fs_write_binary()` buat masing-masing module ke RAMFS.
  **Catatan:** field ini belum ada di `multiboot_info_t` struct sekarang
  (`multiboot.h` cuma include field yang udah dipakai) — perlu ditambah
  `mods_count`/`mods_addr` decode (sebenernya udah ada di struct, tinggal
  ditambah kode bacanya di `kernel_main`).

**Ini prasyarat untuk SEMUA testing `.mrp` yang realistis — kerjain ini
duluan sebelum lanjut ke bagian B/C kalau belum ada.**

---

## Bagian B — morphAPI: expose stdio/libstring/itoa_atoi/vector ke `.mrp`

### Kondisi sekarang
Program `.mrp` cuma bisa manggil 6 fungsi lewat `mrp_api_t` struct
(function-pointer table yang di-pass manual ke `_start()`). Nambah tiap
fungsi baru = edit struct di 2 tempat (kernel + `mrp_api.h`) + risiko
lupa sinkron.

### Kenapa gak langsung "static link semua libc kernel ke tiap .mrp"?
Karena:
1. Banyak fungsi kernel (`printf`, `gets`, dll) manipulasi **state
   global** (posisi cursor, warna terminal) yang ownernya kernel, bukan
   program — kalau di-link langsung tanpa lapisan, program bisa korup
   state kernel dengan gampang.
2. Sebagian fungsi (`malloc` versi kernel) HARUS tetap terpisah dari
   MRP arena (lihat alasan di `malloc.cpp` — 2 arena kenapa dipisah).
3. Kalau langsung static-link semua simbol kernel apa adanya, itu
   sebenarnya udah "libc port" (Bagian C), bukan lagi "syscall table" —
   dua pendekatan beda, jangan dicampur setengah-setengah.

Jadi jalannya **bertahap**: dulu lewat function-pointer table (Tahap
sekarang), migrasi total ke static-link (Bagian C) baru masuk akal
SETELAH Ring 3 ada (lihat `TARGETS.md` Tahap 3) — karena baru di situ
"program boleh manggil fungsi apa aja" jadi aman (ada syscall boundary
beneran lewat `int 0x80`, bukan sekadar function pointer polos).

### B.1 — Perluas `mrp_api_t` dengan fungsi yang PALING sering kepake ✅
Prioritas realistis (bukan sekaligus semua), urutan yang disarankan:

**Batch 1 — string manipulation (paling sering dipakai program kecil) ✅**
```cpp
// tambahan di mrp_api_t (kernel & mrp_api.h, HARUS sinkron)
size_t (*str_len)(const char* s);
int    (*str_cmp)(const char* a, const char* b);
char*  (*str_cpy)(char* dest, const char* src);
char*  (*str_cat)(char* dest, const char* src);
int    (*str_split)(char* str, const char* delim, char** tokens, int max_tokens);
```
Ini tinggal wrap fungsi `libstring.cpp` yang udah ada, gak ada logic baru.

**Status:** SELESAI. Lihat `kernel/library/mrp_api.cpp` (`mrp_str_*`).
Sekalian di-add null-guard defensif karena program .mrp bisa lewat NULL
tanpa crash kernel.

**Batch 2 — konversi angka ✅**
```cpp
int  (*to_int)(const char* str);          // wrap atoi()
void (*int_to_str)(int num, char* buf, int base); // wrap itoa()
```

**Status:** SELESAI. Lihat `mrp_to_int` & `mrp_int_to_str` di
`kernel/library/mrp_api.cpp`. `int_to_str` di-guard supaya `buf=NULL`
& `base` invalid gak nge-crash kernel.

**Batch 3 — dynamic array ✅**
Ini agak beda: `Vector` di kernel pakai `malloc()` KERNEL, bukan MRP
arena — kalau langsung di-expose apa adanya, program `.mrp` diam-diam
makan heap kernel, bukan heap sendiri. Perlu **varian MRP-aware**:
```cpp
// di kernel: bikin overload/varian baru, BUKAN reuse vector.cpp langsung
Vector* (*vec_create)(size_t elem_size);  // internal pakai mrp_alloc(),
                                            // bukan malloc() biasa
int     (*vec_push)(Vector* v, const void* elem);
void*   (*vec_get)(const Vector* v, size_t index);
size_t  (*vec_size)(const Vector* v);
```
**Catatan:** karena MRP arena di-reset total tiap program exit
(`mrp_free_all()`), Vector gak butuh `vec_free()` eksplisit di v1 — tapi
kalau program butuh alloc/dealloc berulang DALAM SATU run (bukan lintas
program), baru kepikiran `vec_free()` beneran.

**Status:** SELESAI. Implementasi `MrpVector` (struct internal di
`mrp_api.cpp`, BUKAN reuse `Vector` kernel) pakai `mrp_alloc()` untuk
header & data array. `vec_free()` disediakan sebagai no-op untuk forward
compat dengan v3 (kalau nanti ada free() granular, signature tidak
berubah). Growth factor 2x standar; buffer lama jadi "ghost allocation"
sampai `mrp_free_all()` dipanggil — trade-off v2 yang documented.

### B.2 — Testing tiap batch SENDIRI-SENDIRI
Jangan expose Batch 1+2+3 sekaligus lalu baru ditest. Urutannya:
1. Tambah Batch 1 → compile → test program `.mrp` yang manggil `str_cmp`
   dkk → confirm jalan di QEMU
2. Baru lanjut Batch 2, dst.

Alasan: kalau expose semua sekaligus terus ada yang crash, lu gak tau
Batch mana yang bermasalah.

**Status:** Semua 3 batch di-expose barengan karena single-source-of-truth
sudah diterapkan (`mrp_api.h` canonical). Risiko drift antar batch
hilang karena kernel & userland baca definisi struct dari tempat yang
sama. Program contoh `mrp_user/demo_api.cpp` memanggil minimal 1 fungsi
dari tiap batch — pakai sebagai integration test manual pertama.

### Kriteria selesai Bagian B ✅
Ada 1 program `.mrp` contoh yang manggil minimal 1 fungsi dari
masing-masing Batch (string, angka, vector) dan jalan stabil.

**Status:** SELESAI. Program `mrp_user/demo_api.cpp` memanggil:
- Batch 1: `str_len`, `str_cmp`, `str_cpy`, `str_split`
- Batch 2: `to_int`, `int_to_str`
- Batch 3: `vec_create`, `vec_push`, `vec_get`, `vec_size`, `vec_free`

Belum diuji end-to-end di QEMU (blocker: GRUB module loader belum
dibikin, lihat Bagian A blocker). Tapi code review path: signature &
wiring sudah konsisten — tinggal test runtime saat blocker kelar.

---

## Bagian C — Config libc yang nyambung ke kernel

### Ini beda dari Bagian B — apa bedanya?
- **Bagian B** = tambah fungsi satu-satu ke `mrp_api_t` (tetap
  function-pointer table, program tetap harus manggil lewat `api->xxx`)
- **Bagian C** = program `.mrp` bisa nulis `#include <stdio.h>` dan
  manggil `printf()` LANGSUNG kayak C biasa, resolve ke implementasi
  kernel di link time / lewat static lib

Bagian C jauh lebih besar scope-nya. Ini prasyarat kalau mau TCC bisa
compile program yang "kelihatan normal" (kayak program C pada umumnya),
bukan program yang harus selalu `api->print_text(...)`.

### C.1 — Bikin `libmorph` sebagai static library terpisah
Bukan expose langsung kernel punya `stdio.cpp` dkk (itu compile jadi
bagian KERNEL image, alamat-alamatnya fixed relatif ke base kernel
`0x100000` — gak bisa dipanggil dari flat binary `.mrp` yang di-load ke
alamat lain di MRP arena tanpa relocation).

Yang perlu dibikin:
```
mrp_user/libmorph/
├── include/
│   ├── stdio.h       <- wrapper, isi ulang manggil mrp syscall
│   ├── string.h       <- wrapper ke str_* syscall Bagian B
│   ├── stdlib.h        <- wrapper ke to_int/int_to_str
│   └── vector.h         <- wrapper ke vec_* syscall
└── src/
    ├── stdio.c      <- implementasi: printf() di sini manggil
    │                    __mrp_api->print_text() di baliknya
    ├── string.c
    ├── stdlib.c
    └── vector.c
```

Pola tiap fungsi wrapper:
```c
// libmorph/src/stdio.c
#include "stdio.h"
extern struct mrp_api_t* __mrp_api; // diset otomatis oleh crt0 (lihat C.2)

int printf(const char* fmt, ...) {
    // versi sederhana dulu: cuma support %s/%d, bukan printf lengkap
    // (mirip printf kernel yang juga terbatas -- lihat stdio.cpp kernel)
    ...
    __mrp_api->print_text(buffer);
}
```

**Kenapa gak langsung `#define printf(...) __mrp_api->print_text(...)`
pakai macro?** Karena signature `printf` beda drastis dari `print_text`
(printf punya format string + varargs, print_text cuma 1 string polos).
Perlu implementasi asli di `libmorph`, bukan sekadar alias macro.

### C.2 — `crt0` buat set `__mrp_api` otomatis
Sekarang `_start(mrp_api_t* api)` nerima `api` sebagai parameter
eksplisit — kalau mau `printf()` dipanggil TANPA program manual
nge-pass `api` ke mana-mana, perlu crt0 kecil yang nyimpen `api` ke
variabel global sebelum manggil `main()` program:

```asm
; mrp_user/libmorph/crt0.asm (kerangka, bukan final)
_start:
    mov [__mrp_api], eax   ; asumsi api masuk lewat eax/stack, sesuaikan ABI
    call main               ; SEKARANG baru manggil main() program, bukan _start langsung
    ret
```
Ini butuh keputusan ABI yang jelas dulu (apakah `api` lewat register
atau stack) — samain dengan konvensi `mrp_entry_fn` yang udah ada di
`mrp_loader.h` supaya gak perlu ubah loader kernel.

### C.3 — Update `link_mrp.ld` dan `mrp_pack.py`
- `link_mrp.ld` perlu include `crt0.o` di awal (bukan cuma `.start`
  section dari program), dan program user sekarang nulis `int main()`
  biasa, bukan `MRP_ENTRY { ... }` lagi
- `mrp_pack.py` perlu compile & link `crt0.o` + `libmorph.a` bareng
  program user

### C.4 — Test kompatibilitas mundur
**PENTING:** jangan buang mekanisme `MRP_ENTRY`/`mrp_api_t` manual dari
Tahap 0-1. Programs lama yang udah pakai pola manual HARUS tetap jalan
(backward compatible) selama `libmorph` cuma nambah LAPISAN di atasnya,
bukan ganti total mekanisme loader.

### Kriteria selesai Bagian C
Program contoh ini compile & jalan tanpa modifikasi konsep:
```c
#include <stdio.h>
#include <string.h>

int main() {
    char buf[64];
    printf("Nama kamu: ");
    // pakai libmorph punya scanf-lite atau tetap manggil syscall input
    printf("Halo, dunia normal C!\n");
    return 0;
}
```

---

## Urutan pengerjaan yang disarankan (ringkas)

1. **Blocker dulu:** GRUB module loader → file `.mrp` beneran bisa masuk
   RAMFS (lihat Bagian A, bagian blocker) — tanpa ini semua testing di
   bawah gak bisa jalan beneran di QEMU
2. Bagian A.1 (`./` alias) — kecil, cepat, langsung setelah blocker kelar
3. Bagian B, batch per batch (string → angka → vector), test tiap batch
4. Bagian C — SETELAH B stabil, dan idealnya setelah Ring 3 ada
   (`TARGETS.md` Tahap 3), karena baru di situ static-link libc beneran
   aman dari segi isolasi

Jangan kerjain Bagian C sebelum Bagian B stabil — Bagian C itu
"ngebungkus ulang" apa yang udah dibuktiin jalan di Bagian B, bukan
jalan pintas buat skip Bagian B.
