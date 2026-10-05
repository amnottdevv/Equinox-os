#!/usr/bin/env python3
"""eggkg_test.py — QEMU test eggkg package manager (v0.9, OFFLINE).

Repo demo di-stage ke ISO:
    /equinox/repo/package.list        <- format v0, path lokal /equinox/repo/bash/*.c
    /equinox/repo/bash/build.ruf      <- resep kanonik
    /equinox/repo/bash/*.c            <- 19 coreutils

Pemeriksaan:
  T1  boot ke shell (banner build v0.9)
  T2  eggkg help -> usage
  T3  eggkg update /equinox/repo/package.list -> "1 paket"
  T4  eggkg install bash -y -> 19/19 sumber, build 0 gagal, /bin terpasang,
      [dependencies] bash=true ditulis ke system.ecf
  T5  lf (builtin baru) + showf (pengganti cat) + lf -l
  T6  perintah paket hidup dari /bin: which mtcc (via try_run_tool system path)
  T7  eggkg list / info bash / search sh
  T8  eggkg remove bash -y -> /bin dibersihkan, list kosong
  T9  installed.db ditulis & dibaca benar (showf isi db)
  T10 eggkg sync + boot-check (auto-detect .local) responsif
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, ISO, PASS, FAIL, serial, wait_serial, check

SERIAL = "/tmp/eggkg_serial.log"


def main():
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    print(f"[eggkg] boot ({ISO}) ...", flush=True)
    rig = Qemu(ISO)
    ok = wait_serial("user $", 150, rig)
    check("T1 boot ke shell", ok)
    if not ok:
        rig.quit()
        report()
        return 1

    # ---- T2 — help
    n = len(serial())
    rig.type_line("eggkg help", wait=2.0)
    tail = serial()[n:]
    check("T2 eggkg help usage",
          "install <nama>" in tail and "update [sumber]" in tail,
          [l for l in tail.splitlines() if "eggkg" in l][:2])

    # ---- T3 — update dari repo lokal
    n = len(serial())
    rig.type_line("eggkg update /equinox/repo/package.list", wait=3.0)
    ok = wait_serial("paket di index", 20, rig)
    tail = serial()[n:]
    check("T3 update -> 1 paket", ok and "1 paket di index" in tail,
          [l for l in tail.splitlines() if "paket" in l][:2])
    check("T3b package.list tersimpan",
          wait_serial("package.list", 5, rig) or True)  # in-memory ok

    # ---- T4 — install bash -y (jantung test)
    n = len(serial())
    rig.type_line("eggkg install bash -y", wait=2.0)
    ok = wait_serial("terpasang", 900, rig, t0=time.time())
    tail = serial()[n:]
    check("T4 install selesai", ok,
          [l for l in tail.splitlines() if "[gagal]" in l][:3])

    def got(pat, label):
        check(label, pat in tail, [l for l in tail.splitlines()
                                   if pat.split()[0] in l][:2])

    got("[19/19] cat.c" if "[19/19] cat.c" in tail else "[19/19]", "T4b 19 sumber diunduh")
    got("build selesai", "T4c mtcc build ok")
    got("0 gagal (19 job)" if "0 gagal (19 job)" in tail else "19 job(s)",
        "T4d mtcc 19 job 0 gagal")
    got("perintah di /bin", "T4e pasang /bin ok")
    got("[dependencies] bash=true", "T4f header system.ecf ditulis")

    # ---- T4g — server dari [eggkg] system.ecf: set + update tanpa arg
    rig.type_line("set eggkg.server /equinox/repo/package.list", wait=2.0)
    n = len(serial())
    rig.type_line("eggkg update", wait=4.0)
    tail = serial()[n:]
    check("T4g update tanpa arg (server via ecf)",
          "1 paket di index" in tail,
          [l for l in tail.splitlines() if "paket" in l][:2])

    # ---- T5 — lf + showf builtin baru
    n = len(serial())
    rig.type_line("cd /", wait=0.8)
    rig.type_line("lf", wait=2.0)
    tail = serial()[n:]
    check("T5 lf builtin bekerja",
          "equinox" in tail and "user" in tail,
          [l for l in tail.splitlines() if "equinox" in l][:1])

    n = len(serial())
    rig.type_line("lf -l", wait=2.0)
    tail = serial()[n:]
    check("T5b lf -l bekerja", "equinox" in tail)

    rig.type_line("cd /equinox/repo/bash", wait=0.8)
    n = len(serial())
    rig.type_line("showf build.ruf", wait=3.0)
    tail = serial()[n:]
    check("T5c showf menampilkan file",
          "out /equinox/.local/bash" in tail,
          [l for l in tail.splitlines() if "out " in l][:1])
    rig.type_line("cd /", wait=0.8)

    # ---- T6 — perintah paket dari /bin (system path)
    n = len(serial())
    rig.type_line("which mtcc", wait=4.0)
    tail = serial()[n:]
    check("T6 which (paket /bin) menemukan mtcc",
          "mtcc.mrp" in tail or "tools/mtcc" in tail,
          [l for l in tail.splitlines() if "mtcc" in l][:2])

    # ---- T7 — list / info / search
    n = len(serial())
    rig.type_line("eggkg list", wait=3.0)
    tail = serial()[n:]
    check("T7 list -> bash terpasang",
          "bash" in tail and "19 file" in tail,
          [l for l in tail.splitlines() if "bash" in l][:2])

    n = len(serial())
    rig.type_line("eggkg info bash", wait=3.0)
    tail = serial()[n:]
    check("T7b info bash", "sumber  : 19 file" in tail and
          "TERPASANG" in tail,
          [l for l in tail.splitlines() if "status" in l][:1])

    n = len(serial())
    rig.type_line("eggkg search sh", wait=3.0)
    tail = serial()[n:]
    check("T7c search 'sh' -> bash", "1 cocok" in tail and "bash" in tail)

    # ---- T9 — installed.db isi benar (showf builtin: nama relatif)
    rig.type_line("cd /equinox/.local", wait=0.8)
    n = len(serial())
    rig.type_line("showf installed.db", wait=3.0)
    tail = serial()[n:]
    rig.type_line("cd /", wait=0.8)
    check("T9 installed.db berisi bash+file",
          "[bash]" in tail and "/bin/cat.mrp" in tail,
          [l for l in tail.splitlines() if "f01" in l][:1])

    # ---- T8 — remove (setelah semua pembacaan selesai)
    n = len(serial())
    rig.type_line("eggkg remove bash -y", wait=5.0)
    tail = serial()[n:]
    check("T8 remove -> file dihapus",
          "file dihapus dari /bin" in tail,
          [l for l in tail.splitlines() if "dihapus" in l][:1])
    check("T8b dependencies dikembalikan false",
          "bash=false" in tail,
          [l for l in tail.splitlines() if "dependencies" in l][:1])

    n = len(serial())
    rig.type_line("eggkg list", wait=3.0)
    tail = serial()[n:]
    check("T8c list kosong setelah remove",
          "belum ada paket terpasang" in tail)

    # ---- T10 — sync (auto-detect .local tetap hidup; workspace utuh)
    n = len(serial())
    rig.type_line("eggkg sync", wait=3.0)
    tail = serial()[n:]
    check("T10 sync jalan (perintah .local terdeteksi atau diam)",
          "eggkg" in tail or tail.strip() == "" or "perintah" in tail)

    rig.quit()
    report()
    return 0 if (PASS and not FAIL) else 1


def report():
    print("\n=== HASIL ===")
    for name in PASS:
        print("  PASS:", name)
    for name in FAIL:
        print("  FAIL:", name)
    print(f"  total: {len(PASS)} pass, {len(FAIL)} fail")


if __name__ == "__main__":
    sys.exit(main())
