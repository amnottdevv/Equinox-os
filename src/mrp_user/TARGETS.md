# Equinox OS — Target Jangka Panjang: dari `.mrp` sampai jalanin DOOM

Dokumen ini nyatet target besar yang lagi dituju, dan kenapa itu HARUS
dipecah jadi tahap-tahap kecil dulu. **Jangan loncat tahap** — tiap tahap
di sini nge-assume tahap sebelumnya udah kebukti jalan di QEMU beneran,
bukan cuma "kelihatannya bener di kode".

## TL;DR realita

> "Bisa jalanin TCC yang dimodifikasi" dan "bisa jalanin DOOM" itu BUKAN
> fitur — itu **hasil akhir** dari puluhan sub-sistem yang masing-masing
> harus bener duluan: memory protection, syscall lengkap, file I/O,
> input timing yang presisi, grafis frame-buffer cepat, dan (buat DOOM)
> floating point + performa yang cukup.

Kalau salah satu subsistem di bawah masih rapuh (misal heap masih suka
korup, atau belum ada validasi bound sama sekali), lanjut ke tahap
berikutnya cuma bakal numpuk bug yang susah dilacak sumbernya —
apakah dari loader, dari libc port-an, dari TCC, atau dari game-nya
sendiri. Makanya urutannya penting, bukan cuma checklist.

---

## Tahap 0 — SELESAI ✅
- [x] Format `.mrp` (header + validasi + checksum)
- [x] Free-list `malloc` dengan split/coalesce/canary
- [x] Loader dasar: load -> `call` entry -> balik ke shell
- [x] Syscall minimal: print, input, alloc, tick

## Tahap 1 — `.mrp` yang beneran kepake (bukan cuma demo)
**Tujuan: program `.mrp` bisa dipakai buat nulis tool shell kecil yang
berguna, bukan cuma "hello world".**

- [ ] File I/O syscall (`fs_read`, `fs_write`, `fs_list`) — lihat
      `workflow_mrp.md` buat detail
- [ ] `exit(code)` + loader baca return status, tampilin di shell
- [x] Command `./file.mrp` langsung dari shell (gak perlu ketik `run`)
      — **SELESAI di `kernel/kernel.cpp`**, plus auto-append `.mrp`.
- [ ] Validasi bound dasar: cek `code_size` gak ngelebihin sisa MRP
      arena SEBELUM alokasi (udah ada), tapi belum ada proteksi program
      nulis ke alamat sembarangan (masih Ring 0 penuh)
- [x] morphAPI v1: subset `libstring`, `itoa_atoi` bisa dipanggil
      langsung dari program `.mrp` (bukan lewat function-pointer table
      manual kayak sekarang, tapi lewat static-linked libc port)
      — **SELESAI sampai v2**: `mrp_user/mrp_api.h` sekarang adalah
      single source of truth (kernel side cuma shim). Batch 1 string,
      Batch 2 angka, Batch 3 vector (MRP-aware) sudah di-expose lewat
      `kernel/library/mrp_api.cpp`. Bagian C (libc port via static lib)
      masih TODO, tapi bottleneck-nya Ring 3 (Tahap 3), bukan API
      surface-nya.

**Kriteria lulus tahap ini:** ada minimal 3 program `.mrp` non-trivial
yang jalan stabil berkali-kali tanpa crash kernel (misal: kalkulator,
text formatter, simple game tebak angka pakai `get_key`+`get_tick`).

## Tahap 2 — Proteksi memori dasar (prasyarat WAJIB sebelum Ring 3)
**Tujuan: kernel gak collapse cuma gara-gara 1 program `.mrp` buggy.**

- [ ] Paging diaktifkan (identity-map dulu, belum perlu virtual memory
      canggih) — sekarang OS ini SAMA SEKALI belum setup CR3/PDE
- [ ] Guard page / bound check di MRP arena biar overflow gampang
      kedeteksi (bukan cuma korup diam-diam)
- [ ] Watchdog sederhana: kalau program `.mrp` hang > N detik, ada cara
      manual interrupt (bukan cuma reset QEMU)

**Kriteria lulus:** program `.mrp` yang sengaja ditulis buggy (null
pointer deref, buffer overflow) menyebabkan **error yang bisa
di-recover**, bukan triple fault / reboot paksa.

## Tahap 3 — Ring 3 (User Mode) — "final boss" versi kecil
**Tujuan: program `.mrp` beneran terisolasi dari kernel.**

- [ ] GDT dengan segment Ring 3 (code + data)
- [ ] TSS (Task State Segment) buat switch privilege level
- [ ] Syscall via `int 0x80` (ganti function-pointer table dari Tahap 0-1)
- [ ] Per-program stack terpisah, bukan pinjam stack kernel
- [ ] Page fault handler yang bisa bedain "program nakal" (kill program,
      kernel tetap hidup) vs "bug kernel beneran"

**Kriteria lulus:** program `.mrp` yang sengaja nulis ke alamat kernel
(`*(int*)0x100000 = 0xDEAD;`) di-kill oleh kernel, kernel tetap jalan
normal setelahnya. Ini demo paling penting sebelum lanjut — kalau demo
ini gagal, JANGAN lanjut ke tahap 4.

## Tahap 4 — Port TCC (Tiny C Compiler) yang dimodifikasi
**Tujuan: compile C di DALAM Equinox OS, bukan cross-compile dari host.**

TCC dipilih karena codebase-nya kecil & self-contained dibanding GCC/
Clang, tapi tetap ini kerjaan besar: TCC assume ada libc (malloc, file
I/O, dll) dan target output ELF/PE standar.

- [x] **Milestone 1 — mtcc 0.1 (SELESAI, v6):** compiler subset-C
      satu file (`mrp_user/tcc.cpp`) yang jalan SEBAGAI program `.mrp`
      di dalam Equinox OS — `run tcc.mrp hello.c` compile & run, `run
      tcc.mrp -c hello.c` menghasilkan `hello.mrp` standalone yang bisa
      `run` langsung. Runtime program hasil compile = syscall int 0x80
      (gak ada libc). Teruji di harness host (interpreter x86-32):
      4 sample × 2 mode lulus byte-per-byte, file .mrp tervalidasi
      independen. Ini "konfirmasi jalur": format .mrp + syscall ABI +
      loader siap menerima compiler beneran. Detail + limitasi:
      `mrp_user/TCC.md`.
- [ ] **Milestone 2 — bahasa yang lebih C:** preprocessor minimal
      (#include RAMFS / #define konstanta), struct sederhana,
      unsigned semantics, switch. (mtcc atau mulai dari tcc asli —
      putuskan berdasarkan apakah Tahap 3 sudah stabil.)
- [ ] Port TCC's backend codegen buat output **format `.mrp`** langsung
      (bukan ELF) — ini bagian paling teknis, TCC punya `tcc_output_type`
      yang perlu diarahkan ke writer custom
- [ ] TCC butuh libc minimal buat compile dirinya sendiri (self-hosting)
      — pastikan morphAPI dari Tahap 1 cukup lengkap dulu
- [ ] TCC butuh file I/O buat baca source `.c` dan tulis output — depend
      Tahap 1
- [ ] TCC jalan SEBAGAI program `.mrp` (idealnya di Ring 3 dari Tahap 3,
      biar kalau TCC crash gara-gara source aneh, gak bawa kernel)
- [ ] Test: compile & jalanin `hello.c` sederhana DARI DALAM Equinox OS,
      hasilnya `.mrp` valid yang bisa langsung `./hello.mrp`
      — **sebagian tercapai oleh mtcc milestone 1** (hello.c → hello.mrp
      → run, tanpa sentuh toolchain host); tersisa membuktikannya dengan
      TCC asli.

**Kriteria lulus:** `tcc hello.c -o hello.mrp && ./hello.mrp` jalan di
dalam Equinox OS itu sendiri, gak nyentuh toolchain host sama sekali.

## Tahap 5 — DOOM (target paling jauh, paling berat)
**Tujuan: DOOM shareware (`doom1.wad`) jalan dengan frame rate kepake.**

DOOM engine (`doomgeneric`/`doomgeneric-fbdev` biasanya jadi basis port
hobby-OS) butuh:

- [ ] **Floating point** — DOOM pakai `float`/`double` di beberapa
      tempat (walau versi asli banyak fixed-point). Pastikan FPU init
      (udah ada `fninit` di `kernel_main`) + libc math dasar
      (`math_pi.h` udah ada tapi perlu diperluas)
- [ ] **Frame buffer cepat** — VESA linear framebuffer udah ada, tapi
      DOOM butuh blit ratusan ribu pixel per frame; perlu profiling,
      kemungkinan perlu optimasi `memcpy`/`fill_rect` (SIMD kalau CPU
      target dukung, atau minimal loop yang di-unroll)
- [ ] **Timing presisi** — `get_tick()` yang ada sekarang cukup buat
      animasi sederhana, tapi game loop 35 tick/detik DOOM butuh timer
      yang lebih presisi (PIT/APIC timer, bukan cuma polling)
- [ ] **Input real-time** — keyboard state polling yang gak nge-block
      (udah ada `get_key()` non-blocking, tapi perlu multi-key state
      buat gerak+shoot bersamaan)
- [ ] **Memory footprint** — DOOM WAD file bisa puluhan MB; MRP arena
      4MB SANGAT tidak cukup, perlu redesign alokasi memori jauh lebih
      besar + kemungkinan swap/streaming asset dari "disk" (RAMFS)
- [ ] Port `doomgeneric` I/O layer ke morphAPI (gantiin `DG_DrawFrame`,
      `DG_GetKey`, `DG_SleepMs` dengan versi Equinox OS)

**Kriteria lulus (realistis, bukan "60 FPS ultra"):** DOOM boot ke main
menu, bisa mulai level 1, gerak + shoot + collision jalan, walau cuma
10-15 FPS. Itu udah pencapaian besar buat hobby OS 32-bit.

---

## Aturan main biar gak burnout / gak stuck

1. **Satu tahap, satu milestone yang bisa di-demo.** Kalau gak bisa
   didemoin ("coba jalanin X, lihat hasilnya Y"), berarti scope-nya
   masih kekabur, pecah lagi jadi lebih kecil.
2. **Jangan mulai Tahap N+1 kalau Tahap N belum stabil 2-3 sesi
   berturut-turut tanpa regresi.** Heap corruption/reboot random itu
   sinyal "balik ke tahap sebelumnya", bukan "tambah fitur buat nutupin".
3. **TCC dan DOOM itu MASING-MASING proyek berbulan-bulan** kalau
   dikerjain serius (bukan hobiis paruh waktu). Realistis: anggap tiap
   tahap di atas itu beberapa minggu-bulan kerja hobi, bukan beberapa
   sesi chat.
4. Kalau di tengah jalan ngerasa "kayaknya lompat langsung ke DOOM lebih
   seru", itu godaan yang wajar tapi PASTI berujung debugging neraka
   tanpa fondasi Tahap 1-3. TCC sendiri realistisnya lebih deket
   (Tahap 4) daripada DOOM (Tahap 5) — kalau harus pilih salah satu
   dulu buat dikejar total, TCC dulu.
