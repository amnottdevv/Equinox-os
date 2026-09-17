# .mrp Loader + Free-list malloc — v1

## Yang berubah / ditambah

**Kernel (`kernel/library/`)**
- `malloc.cpp` — diganti total dari bump allocator jadi **free-list allocator**
  (split + coalesce + canary). Dua arena terpisah:
  - Kernel heap: `0x300000 - 0x500000` (2 MB) — `malloc/free/calloc/realloc` biasa
  - MRP arena: `0x500000 - 0x900000` (4 MB) — khusus program `.mrp`, di-reset
    total tiap kali program baru mau jalan (`mrp_heap_init()`) dan sesudah
    program selesai (`mrp_free_all()`)
- `header/malloc.h` — deklarasi baru: `heap_check_integrity()`,
  `get_heap_free_blocks()`, `get_heap_largest_free()`, `mrp_alloc()`, dll.
- `header/mrp_format.h` — format header `.mrp` (18 byte) + `is_valid_mrp()`
- `header/mrp_loader.h` + `mrp_loader.cpp` — loader: validasi -> copy ke MRP
  arena -> `call` ke entry point -> `mrp_free_all()`
- `fs_ram.cpp` / `fs_ram.h` — tambah `fs_write_binary()` (byte-safe, bukan
  `strcpy`-based, supaya file `.mrp` yang isinya machine code gak kepotong
  di byte `0x00`)
- `kernel.cpp` — command shell baru: `run <nama.mrp>`

**Userland (`mrp_user/`)**
- `mrp_api.h` — struct `mrp_api_t` (HARUS sama persis dengan versi kernel)
  + macro `MRP_ENTRY`
- `link_mrp.ld` — linker script yang naro `_start` di offset 0
- `hello.cpp` — contoh program: print + baca input + print lagi
- `mrp_pack.py` — compile .cpp -> link -> objcopy -> bungkus header `.mrp`

## Cara pakai

```bash
# compile + pack sekaligus (butuh i686-elf-g++ di PATH)
python3 mrp_user/mrp_pack.py mrp_user/hello.cpp hello.mrp

# atau kalau udah punya .bin sendiri (misal dari toolchain lain / TCC nanti)
python3 mrp_user/mrp_pack.py --from-bin program.bin program.mrp hello.mrp
```

Lalu masukkan `hello.mrp` ke RAMFS Equinox OS dan jalankan:
```
root@equinox:/$ run hello.mrp
```

## ⚠️ Yang BELUM diselesaikan (perlu kamu putuskan/lakukan sendiri)

0. **[FIXED — riwayat bug]** Versi awal `link_mrp.ld` link ke alamat `0`,
   padahal code beneran dijalanin di `0x500010` (MRP arena). Karena compile
   pakai `-fno-pic`, itu bikin semua akses ke variabel statis/string
   literal salah alamat begitu program run (baca garbage / crash). Sudah
   diperbaiki: `link_mrp.ld` sekarang link ke `0x500010`, persis alamat
   yang dikasih `mrp_alloc()` pas runtime (deterministik karena
   `mrp_heap_init()` selalu reset arena dulu sebelum load code baru).
   **Kalau kamu ubah `sizeof(block_header)` di `malloc.cpp` atau posisi
   `MRP_HEAP_START`, angka `0x500010` di `link_mrp.ld` WAJIB disesuaikan
   juga** — lihat komentar detail di file itu.
1. **Cara file `.mrp` masuk ke RAMFS.** RAMFS Equinox OS sekarang cuma bisa
   diisi lewat shell (`ccfile`, text only) — belum ada jalur buat masukin
   file biner dari luar QEMU. Opsi yang paling gampang: tambah **GRUB
   multiboot module** (`module /boot/hello.mrp` di `grub.cfg`), lalu baca
   `mb_info->mods_addr` di `kernel_main()` dan panggil `fs_write_binary()`
   buat naro isinya ke RAMFS saat boot. Belum saya buatkan karena scope
   kali ini fokus loader + malloc, tapi ini blocker berikutnya yang bakal
   kamu hit begitu mau tes `run` beneran di QEMU.
2. **`.bss` di program user bisa nge-gembungin ukuran `.mrp`** — `objcopy
   -O binary` menuliskan `.bss` sebagai byte nol literal di file. Untuk
   program kecil gapapa, tapi hindari buffer statis gede (`static char
   buf[100000];`), pakai `api->alloc()` runtime kalau butuh banyak.
3. **Belum ada dukungan global constructor** (static object dengan
   constructor non-trivial). Untuk sekarang, cukup pakai variabel statis
   polos / POD.
4. **Masih Ring 0** — program `.mrp` punya akses PENUH ke memori kernel,
   bukan sandbox. Ini emang rencananya baru diperbaiki di step Ring 3.

## Kenapa desainnya begini (ringkas)

- **Kenapa bukan literal string biner sebagai signature?** Signature teks
  panjang gak jelas ("0110101...") mahal dicek & rawan gagal karena
  encoding/whitespace beda. Magic 4-byte (`"MRP1"`) + checksum jauh lebih
  murah dan gak ambigu.
- **Kenapa `entry_offset` selalu 0?** Linker script naro `_start` di paling
  depan lewat section `.start`. Ini ngilangin kebutuhan parsing symbol
  table ELF di packer — cukup asumsi offset 0, diverifikasi otomatis oleh
  `mrp_pack.py` pakai `nm` sebagai sanity check.
- **Kenapa syscall lewat struct function-pointer, bukan `int 0x80`?** Di
  Ring 0 gak ada privilege transition, jadi `call` lewat pointer udah
  cukup dan jauh lebih simpel. Pola ABI-nya (satu struct berisi semua
  "syscall") sengaja mirip int 0x80 supaya program yang udah ditulis
  sekarang gak perlu ditulis ulang pas migrasi ke Ring 3 nanti — cuma
  mekanisme pemanggilannya yang beda.
