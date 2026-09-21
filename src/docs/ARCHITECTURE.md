# Arsitektur — Gambaran Teknis v0.3

Dokumen ini memetakan bagaimana Equinox OS tersusun dan bekerja, dari
power-on sampai OS meng-compile tool-nya sendiri. Semua nomor konkrit
(RTC, alamat, ukuran) merujuk ke kode sumber terkini.

## 1. Alur boot

```
GRUB (ISO, multiboot)
  │  load kernel.elf + modul (mtcc.mrp, games, tools/*.c, .wad…)
  ▼
boot/start.asm      — VBE 1360x768x32 (fallback teks VGA), set GDT,
  │                   staging modul di 0x2800000 (12 MB), entry kernel
  ▼
kernel.cpp          — heap arena, paging, IDT, console, PS/2, PCI
  │                   enumeration, task scheduler, RAMFS dari modul,
  │                   FAT32 auto-mount /mnt, nettask + DHCP
  ▼
shell (kernel/shell.cpp) — prompt root::users / $
  │
  └─ eqbuild → mtcc (in-OS) → 29 tools .mrp → userland siap
```

Boot log bergaya Linux `[ OK ]` mencatat setiap tahap + file RAMFS yang
dimuat; semuanya juga termirror ke serial COM1 (`-serial file:…`).

## 2. Layout source

```
boot/            start.asm (VBE + staging modul) + grub.cfg template
kernel/
  kernel.cpp     entry + integrasi subsistem
  shell.cpp      shell + eqbuild + try_run_tool dispatcher
  library/       paging, malloc (2-region), task, syscall, stdio
                 (multi-console + canvas), libc, idt/timer/ps2,
                 ata, fs_ram, fs_fat32(+write), pci, elf, mrp_loader,
                 mrp_api, audio, math, random…
  net/           lwIP glue, ne2000, nic.c (registry), httpd, tls_client
  gui/           LVGL 9 + app (file manager, settings, editor)
mrp_user/        SDK Morph.h, packer .mrp, elf_link.ld, TCC.md,
                 TARGETS.md, workflow_mrp.md
tools_user/      29 source C tools userland (di-compile eqbuild)
games/ Libgame/  snake, breakout, pong + framework
test/            sample .c untuk mtcc (hello, libc, gfxclip, …)
mtcc.c           compiler C in-OS (source kanonik)
doomgeneric/     port DOOM → doom.mrp
third_party/     lwIP 2.1.3 + BearSSL 0.6 (+ root CA anchors)
scripts/         helper build + harness regresi QEMU
docs/            dokumentasi (folder ini)
```

## 3. Memori

| Area | Alamat/ukuran | Isi |
| --- | --- | --- |
| Kernel image + heap | heap 2 MB di bawah 0x500000 + gap 0x2702000–0x2800000 (±1 MB) = **±3.1 MB total** | heap kernel 2-region; coalesce/realloc hanya merge blok yang benar-benar bersebelahan fisik |
| Module staging | 0x2800000, 12 MB | modul GRUB (mtcc.mrp, doom.mrp, source tools, WAD — WAD zero-copy) |
| Pool user | bitmap halaman fisik | halaman 4 KB untuk demand paging task |
| VMA per task | arena MRP (window 0x500000+, 2 MB) + stack 1 MB | di-*reserve* via `PTE_DEMAND`; #PF (isr_14 → `task_demand_fault`) mengalokasikan + zero-fill per sentuhan pertama |
| Task ELF | segmen [0x800000, 0x2000000), heap 2 MB @ 0x2000000 | loader ELF32 statis |
| Page dir per task | PDE 1–9 untuk window user | CR3 task-specific; kernel di-identitas |

Arena MRP per task ada di field `Task::mrp_arena` (layout identik dengan
heap_arena — dijaga `static_assert`), jadi malloc/free di program user
benar-benar per-proses dan di-reset saat exit.

## 4. Multitasking

- **Scheduler**: round-robin preemptive di timer 100 Hz (quantum 1
  tick), FPU state `fxsave/fxrstor` per task, trampoline entry ring 3.
- **Console virtual**: tiap task punya mirror sel + kursor sendiri; F1/F2
  memilih console aktif; printf task background menulis ke console-nya
  sendiri, bukan menimpa layar.
- **Siklus hidup**: `spawn/spawn2` → exit → USER child jadi **zombie**
  (slot + status ditahan untuk parent) → `wait` mengambil status; anak
  yatim di-reap saat parent mati; tabel penuh → zombie tertua dicuri.
  `MAX_TASKS` = 8.
- **Pipe**: `SYS_PIPE` ring 4 KB kernel, ujung ref-counted, diwariskan
  lintas spawn; read/write blocking, EOF & broken-pipe terdeteksi.
- **Idle discipline**: kalau task mem-block dirinya dan tak ada yang
  READY, scheduler `hlt` dengan interrupt on sampai interrupt membangunkan
  sesuatu — sleep presisi per tick, nettask tidak busy-spin.

## 5. Syscall (int 0x80, ring 3, append-only)

Nomor #0–#54, tabel dispatch di `syscall.cpp`. Kelompok besar:

- **I/O & konsol** (#0–#10): put_char, warna, clear, getkey nonblok/blok,
  gfx (putpixel/fillrect/blit).
- **Proses** (#36–#39, #49, #52): spawn, yield, taskinfo (ps), kill,
  wait, spawn2.
- **Memori** (#51): meminfo (pool + faulted per task).
- **File** (#11–#35, #40–#48): versi lama + API file lengkap `open2`
  (O_CREAT/TRUNC/APPEND/EXCL/DIR), unlink/mkdir/rmdir/rename/stat/
  readdir/fstat, fd writable per-task dengan write-through FAT per close.
- **Pipe** (#50) dan **grafis per-task** (#53 set_clip, #54 draw_line
  Bresenham). Errno negatif: `SYS_ECHILD` (-13), `SYS_EPERM` (-14).

Tiap task punya tabel fd, cwd, args sendiri (field di `struct Task`) —
syscall file beroperasi pada konteks task pemanggil.

## 6. Filesystem

- **RAMFS** (`fs_ram.cpp`): tree sederhana, di-populate dari modul GRUB
  saat boot; `dist/` di host mencerminkan layout-nya.
- **FAT32** (`fs_fat32*.cpp`): mount partisi primary pertama di `/mnt`
  (lazy RAMFS mirror → `ls/cat/edit/mget` transparan). Tulis: write-through
  per close, LFN + 8.3 mangling dengan anti-collision ke short name
  asli, FSInfo, direktori tumbuh on-demand, rollback bila I/O gagal,
  arena cache disk 8 MB di luar heap kernel. Teruji host-side
  (mtools oracle + mini-fsck) sampai utilisasi 100%.
- **Bridge**: fs node RAMFS dapat menunjuk cluster disk; cache dijatuhkan
  saat fd terakhir tertutup.

## 7. Jaringan

lwIP 2.1.3 mode NO_SYS (polling kooperatif) lewat `nettask` kernel:

```
ne2k_isa (0x300/IRQ9) → RX ring (diluar IRQ) → lwIP → DHCP/DNS/ICMP/TCP
                                   │
                    ┌──────────────┴───────────────┐
              mget (client HTTP/S)            httpd (server :80)
              BearSSL TLS 1.2                  status + file RAMFS
              9 root CA Mozilla, RDRAND+jitter
              fallback parse-only (terwarnai)
```

NIC diabstraksi `nic.c` registry (probe/send/recv/mac/irq/overflow);
menambah driver = satu struct + satu baris registrasi.

## 8. Grafis

- Framebuffer linear 1360x768@32 (VESA DISPI; fallback teks VGA dengan
  snap warna).
- **Multi-console**: mirror sel per console + canvas piksel per task.
- **Semantik canvas**: draw pertama task → snapshot layar teks + clear
  hitam (console_canvas_mark sebelum piksel pertama); teks program
  grafis tetap ke serial + mirror; exit → console teks di-render ulang.
- **Draw window per task**: set_clip membatasi putpixel/fillrect/line
  (interseksi clip kernel); draw_line Bresenham focus-gated.
- LVGL 9 (file manager/settings/editor) di atas framebuffer + mouse PS/2.

## 9. Format program & compiler

- **MRP**: flat binary ring-3 + header; loader `mrp_loader.cpp`, API
  Morph (mrp_api). Program memakai arena malloc sendiri.
- **ELF32**: ET_EXEC statis dimap ke window demand; `elfdemo.elf`
  dibuat `gcc -m32 -nostdlib -T mrp_user/elf_link.ld`.
- **mtcc** (mtcc.c): lexer → parser → kode 32-bit langsung; preprocessor
  mini yang men-splice `<morph.h>`/`<multitasking.h>`/`<fileio.h>`;
  libc di-render lokal di program (printf family 5 konversi, `%u`
  unsigned sejati, sscanf, ctype, string extras, qsort, rand);
  undefined-reference dideteksi dengan nomor baris call-site.
- **eqbuild** (shell.cpp): iterasi `/equinox/tools/*.c` → `mtcc c file`
  per file → log `i/N OK nama.mrp` → hapus source. RAMFS dibangun ulang
  tiap boot dari ISO, jadi boot baru selalu membawa source lagi —
  eqbuild idempoten per sesi. Parity in-OS vs host: byte-identical.

## 10. Build & harness

- Makefile host-mode `g++ -m32` (atau cross i686-elf), dependency
  tracking `-MMD -MP`, target: `all/run/run-disk/pack/mtcc/tools/
  doom/diskimg/test/test-fat32`.
- Harness regresi QEMU (scripts/): `regression_task3.py` (boot, lspci,
  eqbuild 29/29, tools battery, gfx pixel-proof, libc, soak),
  `regression_task2.py` (fstest 24/24 demand paging, ELF, wait/pipe/
  zombie), `regression_v03.py` (core + tools), `regression_multitask.py`
  (canvas, game, F1/F2, DOOM) — total **101/101** pada ISO rilis.
