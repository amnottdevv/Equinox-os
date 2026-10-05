#!/usr/bin/env python3
"""build_test.py — QEMU regression `equinoxinstall -build` v0.7.

Bentuk baru:
  -build <nama>    satu tool dari /equinox/tools via `mtcc -c`
  -build mtcc      status self-host yang jujur (mtcc.c di luar subset)
  -build copy      builtin shell — tanpa sumber, daftar .c tersedia
  -build *.ruf     glob — build semua .ruf di /equinox (default dir)
Regresi:
  -build <file.ruf>  jalur v0.6 (eqmini: 3 games, cepat)
Internet (user menyediakan akses internet, slirp NAT + DNS):
  dns example.com / mget http://example.com/

  B1  boot ke shell
  B2  -build ls        -> mtcc -c ok ("wrote /equinox/tools/ls.mrp" +
                          "[build] ls.c -> ls.mrp — ok")
  B3  -build mtcc      -> pesan status self-host
  B4  -build copy      -> tidak ada sumber + catatan builtin
  B5  -build eqmini    -> "[build] selesai — semua job ok" (regresi v0.6)
  B6  dns example.com  -> resolve sukses lewat slirp 10.0.2.3
  B7  mget example.com -> "saved (HTTP 200"
  B8  -build *.ruf     -> "[build] glob '*.ruf': 3 .ruf dibangun"
                          (eq 22 + eqfull 81 + eqmini 23 + v3t1/2 2+2 job; sumber v3test ikut ke-glob)
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32                     # noqa: E402

# NIC e1000 (jalur jaringan yang teruji oleh e1000_test.py)
b32.SERIAL = "/tmp/build_serial.log"
b32.NET = ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"]

SERIAL = b32.SERIAL
ISO = b32.ISO
check, serial, wait_serial = b32.check, b32.serial, b32.wait_serial


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


def main():
    if not os.path.exists(ISO):
        print(f"[build] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[build] boot (ISO + e1000 + slirp) ...", flush=True)
    rig = b32.Qemu(ISO)
    ok = wait_serial("user $", 150, rig, t0=time.time())
    check("B1 boot ke shell", ok)
    if not ok:
        rig.quit()
        report()
        return 1

    # ---- B2: -build wc (satu tool via mtcc -c; v0.9.2: ls pindah
    #      ke repo eggkg, wc mewakili tool base) ---------------------
    base = len(serial())
    rig.type_line("equinoxinstall -build wc", wait=2.0)
    t = window(base, "[build] wc.c -> wc.mrp", 120, rig)
    check("B2 -build wc ok",
          "wrote /equinox/tools/wc.mrp" in t and
          "[build] wc.c -> wc.mrp — ok" in t,
          [l.strip() for l in t.splitlines()
           if "wrote" in l or "[build]" in l][-2:])

    # ---- B3: -build mtcc (status self-host) -------------------------
    base = len(serial())
    rig.type_line("equinoxinstall -build mtcc", wait=1.5)
    t = window(base, "self-compile", 20, rig)
    check("B3 -build mtcc status self-host",
          "self-compile belum didukung" in t and "make mtcc" in t,
          [l.strip() for l in t.splitlines() if "mtcc" in l][:2])

    # ---- B4: -build copy (builtin, tanpa sumber) --------------------
    base = len(serial())
    rig.type_line("equinoxinstall -build copy", wait=1.5)
    t = window(base, "builtin shell", 20, rig)
    check("B4 -build copy -> daftar + catatan builtin",
          "tidak ada sumber 'copy.c'" in t and "builtin shell" in t and
          "  - wc.c" in t,
          [l.strip() for l in t.splitlines()
           if "tidak ada sumber" in l or "builtin" in l][:2])

    # ---- B5: regresi -build <file.ruf> (eqmini: repo/bash, 23 job) ---
    base = len(serial())
    rig.type_line("equinoxinstall -build /equinox/eqmini.ruf", wait=2.0)
    t = window(base, "[build] selesai — semua job ok", 600, rig)
    check("B5 -build eqmini.ruf (regresi v0.6, v0.9.2: repo/bash)",
          "[build] selesai — semua job ok" in t and
          "ake] selesai: 23 ok, 0 gagal (23 job)" in t,
          [l.strip() for l in t.splitlines() if "selesai" in l][-2:])

    # ---- B6: internet — DNS lewat slirp ------------------------------
    base = len(serial())
    rig.type_line("dns example.com", wait=3.0)
    t = window(base, "dns: example.com ->", 45, rig)
    check("B6 dns example.com resolve", "dns: example.com ->" in t,
          [l.strip() for l in t.splitlines() if l.startswith("dns:")][:2])

    # ---- B7: internet — HTTP ke example.com (NAT keluar) -------------
    base = len(serial())
    rig.type_line("mget http://example.com/", wait=5.0)
    t = window(base, "saved (HTTP", 60, rig)
    check("B7 mget http://example.com/ saved",
          "saved (HTTP 200" in t,
          [l.strip() for l in t.splitlines() if "saved" in l or
           ("mget:" in l and "failed" in l)][:2])

    # ---- B8: -build *.ruf (glob semua .ruf di /equinox) --------------
    # v0.9.3: v3t1/v3t2 (regresi ruf v3) ikut ke-glob — sumbernya
    # dibuat dulu supaya 2 .ruf v3 build 2 job masing-masing.
    rig.type_line("cd /equinox/.local", wait=1.0)
    rig.type_line("cdir v3test", wait=1.0)
    rig.type_line("cd v3test", wait=1.0)
    rig.type_line("cdir src", wait=1.0)
    rig.type_line("cd src", wait=1.0)
    rig.type_line('ccfile hello.c << int main() { print("halo-v3"); return 42; }',
                  wait=1.5)
    rig.type_line('ccfile world.c << int main() { print("world-v3"); return 7; }',
                  wait=1.5)
    rig.type_line("cd /", wait=1.0)
    time.sleep(1)
    base = len(serial())
    rig.type_line("equinoxinstall -build *.ruf", wait=2.0)
    t = window(base, "[build] glob '*.ruf'", 1500, rig)
    ok8 = ("[build] glob '*.ruf': 5 .ruf dibangun" in t and
           "ake] selesai: 22 ok, 0 gagal (22 job)" in t and
           "ake] selesai: 23 ok, 0 gagal (23 job)" in t and
           "ake] selesai: 81 ok, 0 gagal (81 job)" in t and
           t.count("ake] selesai: 2 ok, 0 gagal (2 job)") >= 2)
    check("B8 -build *.ruf glob (eq+eqfull+eqmini)", ok8,
          [l.strip() for l in t.splitlines()
           if "glob" in l or "selesai:" in l][-5:])

    rig.quit()
    report()
    return 0 if (b32.PASS and not b32.FAIL) else 1


def report():
    print(f"\n[build] RESULT: {len(b32.PASS)} PASS, {len(b32.FAIL)} FAIL")
    for name in b32.PASS:
        print("  PASS:", name)
    if b32.FAIL:
        print("  failed:", ", ".join(b32.FAIL))


if __name__ == "__main__":
    sys.exit(main())
