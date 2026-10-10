#!/usr/bin/env python3
"""mtcc_cli_test.py — in-OS regression untuk CLI mtcc yang baru (profesional).

Semua pemeriksaan berjalan DI DALAM QEMU supaya jalur nyata yang diuji:
RAMFS -> spawn mtcc.mrp -> status line berwarna -> image -> loader OS.

  C1   boot ke shell
  C2   mtcc -c /test/hello.c        baris [COMPILE] / [LINK] / [OUTPUT]
                                    (Bahasa Inggris, rapi)
  C3   warna hijau pada tag [COMPILE] — console kernel memang mem-parse
       escape ANSI (stdio.cpp ansi_feed/ansi_sgr_apply), jadi ESC[32m
       ikut terekam di log serial
  C4   run /test/hello.mrp          produk -c tetap berjalan
  C5   mtcc -c -format elf ...      ELF32 ET_EXEC, satu PT_LOAD @ 0x01000000
  C6   run <prog>.elf               loader ELF OS mengeksekusinya (magic sniff)
  C7   -multiple-files + -o         multi-file eksplisit, nama output sendiri
  C8   pemisah koma a.c,b.c         daftar file dipisah koma juga sah
  C9   opsi tak dikenal             [ERROR] unknown option '...' (bukan diam)
  C10  error kompilasi              [ERROR] <file>:<line>: <pesan>
  C11  --help                       ringkasan (-format, -o, -multiple-files)
  C12  resep v4 multiple_file       beberapa `src` -> SATU link -> di-run
  C13  resep v4 job + format elf    satu baris -> satu .elf -> di-run OS
  C14  resep v4 set + ecf           tulis key ke .ecf setelah build hijau,
                                    key lain & seed tidak tersentuh (cat)
  C15  daftar .c tanpa multiple_file ditolak dengan arahan perbaikan
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32                     # noqa: E402

b32.SERIAL = "/tmp/mtcc_cli_serial.log"
SERIAL = b32.SERIAL
ISO = b32.ISO
check, serial, wait_serial = b32.check, b32.serial, b32.wait_serial

GREEN = "\x1b[32m"


def window(base, pat, timeout, rig, limit=6000):
    """Tunggu `pat` muncul di log serial SETELAH posisi `base`."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        t = serial()[base:]
        if pat in t:
            return t
        if not rig.alive():
            return t
        time.sleep(0.3)
    return serial()[base:base + limit]


def run_cmd(rig, cmd, want, timeout, wait=2.0):
    """Ketik `cmd`, tunggu `want` muncul; return potongan log + status."""
    base = len(serial())
    rig.type_line(cmd, wait=wait)
    t = window(base, want, timeout, rig)
    return t, want in t


def lines(t, key):
    return [l.strip() for l in t.splitlines() if key in l]


def main():
    if not os.path.exists(ISO):
        print(f"[mtcc-cli] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[mtcc-cli] boot ...", flush=True)
    rig = b32.Qemu(ISO)
    ok = wait_serial("user $", 150, rig, t0=time.time())
    check("C1 boot ke shell", ok)
    if not ok:
        rig.quit()
        report()
        return 1

    # ---- C2: baris status -c (Inggris, bertag) --------------------------
    t, hit = run_cmd(rig, "mtcc -c /test/hello.c", "/test/hello.mrp (", 90)
    check("C2 mtcc -c -> [COMPILE]/[LINK]/[OUTPUT]",
          hit and "[COMPILE]" in t and "[LINK]" in t and "[OUTPUT]" in t,
          lines(t, "[OUTPUT]")[:2] or lines(t, "[COMPILE]")[:2])

    # ---- C3: tag [COMPILE] berwarna hijau (ESC[32m) ---------------------
    check("C3 warna hijau ESC[32m pada [COMPILE]",
          GREEN in t and "[COMPILE]" in t,
          [l for l in t.splitlines() if "[COMPILE]" in l][:1])

    # ---- C4: produk -c berjalan ----------------------------------------
    t, hit = run_cmd(rig, "run /test/hello.mrp",
                     "Hello from C compiled inside Equinox OS!", 60)
    check("C4 run /test/hello.mrp", hit, lines(t, "Hello from")[:1])

    # ---- C5: -format elf ------------------------------------------------
    t, hit = run_cmd(rig,
                     "mtcc -c -format elf -o /test/hello_elf /test/hello.c",
                     "/test/hello_elf.elf (", 90)
    check("C5 mtcc -c -format elf menulis .elf",
          hit and "hello_elf.elf" in t and "[OUTPUT]" in t,
          lines(t, "[OUTPUT]")[:2])

    # ---- C6: OS menjalankan ELF32 hasil mtcc ----------------------------
    t, hit = run_cmd(rig, "run /test/hello_elf.elf",
                     "Hello from C compiled inside Equinox OS!", 60)
    check("C6 run /test/hello_elf.elf (loader ELF)", hit,
          lines(t, "Hello from")[:1])

    # ---- C7: -multiple-files + -o --------------------------------------
    t, hit = run_cmd(rig,
                     "mtcc -c -multiple-files -o /mfprog3.mrp "
                     "/test/mf_main.c /test/mf_helper.c",
                     "/mfprog3.mrp (", 120, wait=3.0)
    check("C7 -multiple-files + -o /mfprog3.mrp",
          hit and "[COMPILE]" in t and "[LINK]" in t,
          lines(t, "[OUTPUT]")[:2])
    t, hit = run_cmd(rig, "run /mfprog3.mrp", "dx=6 total=42 done", 60)
    check("C7b run /mfprog3.mrp", hit, lines(t, "dx=")[:1])

    # ---- C8: pemisah koma ----------------------------------------------
    t, hit = run_cmd(rig, "mtcc /test/mf_main.c,/test/mf_helper.c",
                     "dx=6 total=42 done", 120, wait=3.0)
    check("C8 daftar file dipisah koma", hit,
          lines(t, "dx=")[:1] or lines(t, "[COMPILE]")[:1])

    # ---- C9: opsi tak dikenal ------------------------------------------
    t, hit = run_cmd(rig, "mtcc --bogus /test/hello.c", "unknown option", 40)
    check("C9 opsi tak dikenal -> [ERROR]", hit and "[ERROR]" in t,
          lines(t, "[ERROR]")[:2])

    # ---- C10: pesan error bergaya CLI ----------------------------------
    t, hit = run_cmd(rig, "mtcc /test/negtest.c", "negtest.c:", 60)
    check("C10 [ERROR] <file>:<line>: <pesan>",
          hit and "[ERROR]" in t,
          lines(t, "[ERROR]")[:2])

    # ---- C11: --help ----------------------------------------------------
    t, hit = run_cmd(rig, "mtcc --help", "usage :", 40)
    check("C11 mtcc --help menampilkan ringkasan (-format, -o)",
          hit and "-format" in t and "-multiple-files" in t,
          lines(t, "usage")[:1])

    # ---- C12: resep v4 — multiple_file = True -> SATU program ----------
    # Resepnya ditulis di dalam OS dengan `echo … > / >> …` (redirect shell
    # sudah teruji), lalu dijalankan lewat mtcc -make dan hasilnya di-run.
    rig.type_line("echo name := v4prog > /test/v4a.ruf", wait=1.5)
    rig.type_line("echo multiple_file = True >> /test/v4a.ruf", wait=1.5)
    rig.type_line("echo out /test/v4out >> /test/v4a.ruf", wait=1.5)
    rig.type_line("echo src /test/mf_main.c, /test/mf_helper.c >> /test/v4a.ruf",
                  wait=1.5)
    t, hit = run_cmd(rig, "mtcc -make /test/v4a.ruf",
                     "done: 1 ok, 0 failed (1 job(s))", 150, wait=3.0)
    check("C12 multiple_file = True -> 1 job, [COMPILE] multi-file",
          hit and "[COMPILE]" in t,
          lines(t, "[COMPILE]")[:2] or lines(t, "[MAKE]")[:3])
    t, hit = run_cmd(rig, "run /test/v4out/v4prog.mrp",
                     "dx=6 total=42 done", 60)
    check("C12b run v4prog.mrp (dua file jadi SATU program)", hit,
          lines(t, "dx=")[:1])

    # ---- C13: baris `job` + `format elf` -------------------------------
    rig.type_line("echo job h from /test/hello.c to /test/v4out/hello4 "
                  "format elf > /test/v4b.ruf", wait=1.5)
    t, hit = run_cmd(rig, "mtcc -make /test/v4b.ruf",
                     "done: 1 ok, 0 failed (1 job(s))", 120, wait=3.0)
    check("C13 `job … to … format elf` -> hello4.elf",
          hit and "[COMPILE]" in t and "hello4.elf" in t,
          lines(t, "[COMPILE]")[:2] or lines(t, "[MAKE]")[:3])
    t, hit = run_cmd(rig, "run /test/v4out/hello4.elf",
                     "Hello from C compiled inside Equinox OS!", 60)
    check("C13b run hello4.elf (produk resep v4 dijalankan loader OS)", hit,
          lines(t, "Hello from")[:1])

    # ---- C14: `set` -> .ecf, hanya setelah build hijau -----------------
    rig.type_line("echo keepthis = seed > /test/v4.ecf", wait=1.5)
    rig.type_line("echo [eggkg] >> /test/v4.ecf", wait=1.5)
    rig.type_line("echo local = /old >> /test/v4.ecf", wait=1.5)
    rig.type_line("echo multiple_file = True > /test/v4c.ruf", wait=1.5)
    rig.type_line("echo out /test/v4out >> /test/v4c.ruf", wait=1.5)
    rig.type_line("echo src /test/hello.c >> /test/v4c.ruf", wait=1.5)
    rig.type_line("echo ecf /test/v4.ecf >> /test/v4c.ruf", wait=1.5)
    rig.type_line("echo set eggkg.local = /test/v4out >> /test/v4c.ruf",
                  wait=1.5)
    t, hit = run_cmd(rig, "mtcc -make /test/v4c.ruf",
                     "set eggkg.local", 120, wait=3.0)
    check("C14 `set key = value` berjalan setelah build hijau",
          hit and "ok" in t and "/test/v4.ecf" in t,
          lines(t, "set ")[:2])
    t, hit = run_cmd(rig, "cat /test/v4.ecf", "local = /test/v4out", 40,
                     wait=2.0)
    check("C14b ecf ter-merge (nilai lama /old hilang, seed tetap)",
          hit and "keepthis = seed" in t and "[eggkg]" in t and
          "/old" not in t,
          lines(t, "local =")[:3])

    # ---- C15: daftar .c tanpa multiple_file = ditolak + arahan ---------
    rig.type_line("echo src /test/hello.c > /test/v4d.ruf", wait=1.5)
    t, hit = run_cmd(rig, "mtcc -make /test/v4d.ruf", "multiple_file = True",
                     60, wait=3.0)
    # guard memakai tag [MAKE] (bukan [ERROR]) — teksnya yang jadi patokan
    check("C15 daftar .c tanpa multiple_file ditolak + arahan fix",
          hit and "[MAKE]" in t and "ERROR" in t, lines(t, "[MAKE]")[:2])

    rig.quit()
    report()
    return 1 if b32.FAIL else 0


def report():
    print(f"\n[mtcc-cli] RESULT: {len(b32.PASS)} PASS, {len(b32.FAIL)} FAIL")


if __name__ == "__main__":
    sys.exit(main())
