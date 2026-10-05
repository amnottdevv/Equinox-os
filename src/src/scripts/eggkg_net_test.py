#!/usr/bin/env python3
"""eggkg_net_test.py — eggkg ONLINE test: repo GitHub ASLI user.

Server: https://raw.githubusercontent.com/amnottdevv/Eggkg-l/main/
        (package.list format v0, URL blob -> konversi raw otomatis)

  N1  boot (ISO + e1000 + slirp)
  N2  net hidup (dns example.com)
  N3  eggkg update https://raw.githubusercontent.com/.../package.list
      -> N paket, 20 sumber (19 .c + build.ruf)
  N4  eggkg install bash -y  -> unduh 20 file dari GitHub, build.ruf
      dari repo dipakai (bukan sintesis), mtcc 19 job 0 gagal, /bin,
      [dependencies] bash=true
  N5  which mtcc (perintah paket hidup) + eggkg list
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boot_test_v032
from boot_test_v032 import Qemu, ISO, PASS, FAIL, serial, wait_serial, check

boot_test_v032.NET = ["-netdev", "user,id=net0",
                      "-device", "e1000,netdev=net0"]
SERIAL = "/tmp/boot_v032_serial.log"

GH_LIST = ("https://raw.githubusercontent.com/amnottdevv/Eggkg-l/"
           "main/package.list")


def report():
    print("\n=== HASIL ===")
    for name in PASS:
        print("  PASS:", name)
    for name in FAIL:
        print("  FAIL:", name)
    print(f"  total: {len(PASS)} pass, {len(FAIL)} fail")


def main():
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    print(f"[eggkg-net] boot ({ISO}) ...", flush=True)
    rig = Qemu(ISO)
    ok = wait_serial("user $", 150, rig)
    check("N1 boot ke shell", ok)
    if not ok:
        rig.quit(); report(); return 1

    # N2 — net hidup
    n = len(serial())
    rig.type_line("dns example.com", wait=2.0)
    ok = wait_serial(".", 30, rig)
    tail = serial()[n:]
    check("N2 DNS slirp hidup", "->" in tail,
          [l for l in tail.splitlines() if "->" in l][:1])
    if "->" not in tail:
        print("[eggkg-net] NET DOWN — skip sisa test (lingkungan "
              "tanpa internet)", flush=True)
        rig.quit(); report(); return 1

    # N3 — update dari repo GitHub asli
    n = len(serial())
    rig.type_line(f"eggkg update {GH_LIST}", wait=3.0)
    ok = wait_serial("paket di index", 120, rig, t0=time.time())
    tail = serial()[n:]
    check("N3 update GitHub -> 2 paket",
          ok and "2 paket di index" in tail,
          [l for l in tail.splitlines() if "paket" in l or "gagal" in l][:2])

    # N4 — install dari GitHub (20 unduhan + build 19 job)
    n = len(serial())
    rig.type_line("eggkg install bash -y", wait=2.0)
    ok = wait_serial("terpasang", 900, rig, t0=time.time())
    tail = serial()[n:]
    check("N4 install GitHub selesai", ok,
          [l for l in tail.splitlines() if "[gagal]" in l][:3])
    check("N4b build.ruf repo dipakai (bukan sintesis)",
          "(repo tanpa build.ruf" not in tail and
          "[ok] (resep" in tail,
          [l for l in tail.splitlines() if "resep" in l][:2])
    check("N4c 19 job 0 gagal", "0 gagal (19 job)" in tail)

    # N5 — verifikasi
    n = len(serial())
    rig.type_line("which mtcc", wait=5.0)
    tail = serial()[n:]
    check("N5 which (paket /bin) jalan", "tools/mtcc" in tail,
          [l for l in tail.splitlines() if "mtcc" in l][:2])

    n = len(serial())
    rig.type_line("eggkg list", wait=3.0)
    tail = serial()[n:]
    check("N5b list -> bash 19 file", "19 file" in tail,
          [l for l in tail.splitlines() if "bash" in l][:2])

    rig.quit()
    report()
    return 0 if (PASS and not FAIL) else 1


if __name__ == "__main__":
    sys.exit(main())
