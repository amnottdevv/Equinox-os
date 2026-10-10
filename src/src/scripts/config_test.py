#!/usr/bin/env python3
"""config_test.py — in-OS regression untuk ecf_caller + `config`/`call` (T1).

Menjalankan shell Equinox DI DALAM QEMU untuk memvalidasi lapisan dispatch
ecf_caller + builtin `config` + `call` + resolver .config/.

  C1  boot ke shell
  C2  `config callers`      daftar aksi terdaftar (set.key/path_local/active)
  C3  `call set.path_local /equinox/.local/tp`  -> tulis eggkg.local
  C4  `set eggkg.local`     nilai terbaca balik dari store aktif
  C5  `call set.key foo bar` -> tulis key bebas foo=bar
  C6  `set foo`             foo terbaca = bar
  C7  `call no.such.action` -> [ERROR] unknown action
  C8  `config`              ringkas store aktif
  C9  `config get eggkg local` (bila .config/eggkg.ecf ada) atau info none
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32                     # noqa: E402

b32.SERIAL = "/tmp/config_test_serial.log"
SERIAL = b32.SERIAL
ISO = b32.ISO
check, serial, wait_serial = b32.check, b32.serial, b32.wait_serial


def window(base, pat, timeout, rig, limit=6000):
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
    base = len(serial())
    rig.type_line(cmd, wait=wait)
    t = window(base, want, timeout, rig)
    return t, want in t


def main():
    if not os.path.exists(ISO):
        print(f"[config-test] ISO missing: {ISO}")
        return 1
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[config-test] boot ...", flush=True)
    rig = b32.Qemu(ISO)
    ok = wait_serial("user $", 150, rig, t0=time.time())
    check("C1 boot ke shell", ok)
    if not ok:
        rig.quit()
        report()
        return 1

    # ---- C2: config callers menampilkan aksi bawaan --------------------
    t, hit = run_cmd(rig, "config callers", "set.path_local", 30)
    check("C2 config callers (set.key/path_local/active)",
          hit and "set.key" in t and "set.active" in t,
          [l.strip() for l in t.splitlines() if "set." in l][:3])

    # ---- C3: call set.path_local -> tulis eggkg.local -------------------
    t, hit = run_cmd(rig, "call set.path_local /equinox/.local/tp",
                     "eggkg.local = /equinox/.local/tp", 30)
    check("C3 call set.path_local",
          hit and "[OK]" in t,
          [l.strip() for l in t.splitlines() if "eggkg.local" in l][:1])

    # ---- C4: nilai terbaca balik dari store aktif -----------------------
    t, hit = run_cmd(rig, "set eggkg.local", "/equinox/.local/tp", 30)
    check("C4 set eggkg.local terbaca balik",
          hit, [l.strip() for l in t.splitlines() if "local" in l][:1])

    # ---- C5: call set.key foo bar --------------------------------------
    t, hit = run_cmd(rig, "call set.key foo bar", "foo = bar", 30)
    check("C5 call set.key foo bar", hit and "[OK]" in t,
          [l.strip() for l in t.splitlines() if "foo" in l][:1])

    # ---- C6: set foo -> bar --------------------------------------------
    t, hit = run_cmd(rig, "set foo", "bar", 30)
    check("C6 set foo terbaca = bar", hit,
          [l.strip() for l in t.splitlines() if "foo" in l][:1])

    # ---- C7: aksi tak dikenal -> [ERROR] unknown action -----------------
    t, hit = run_cmd(rig, "call no.such.action", "unknown action", 30)
    check("C7 call aksi tak dikenal -> [ERROR]",
          hit and "no.such.action" in t,
          [l.strip() for l in t.splitlines() if "unknown" in l][:1])

    # ---- C8: config ringkas --------------------------------------------
    t, hit = run_cmd(rig, "config", "store aktif", 30)
    check("C8 config (ringkas store + tool)",
          hit and "system.ecf" in t,
          [l.strip() for l in t.splitlines() if "store" in l][:1])

    # ---- C9: config get eggkg local (info none bila belum ada) ----------
    t, hit = run_cmd(rig, "config get eggkg local", "config:", 30)
    check("C9 config get eggkg local (ada/tidak = pesan jelas)",
          hit,
          [l.strip() for l in t.splitlines()
           if "config:" in l or "= " in l][:1])

    # ---- T2: lapisan config eggkg ---------------------------------------
    # C10: eggkg init -> seed .config/eggkg.ecf (resep [update]+[install])
    t, hit = run_cmd(rig, "eggkg init", "[OK]", 40)
    check("C10 eggkg init (seed .config/eggkg.ecf)",
          hit and "eggkg.ecf" in t,
          [l.strip() for l in t.splitlines() if "eggkg.ecf" in l][:1])

    # C11: config show eggkg -> tampilkan isi resep (ada langkah pkg.*)
    t, hit = run_cmd(rig, "config show eggkg", "pkg.install_bin", 30)
    check("C11 config show eggkg (resep punya pkg.install_bin)",
          hit,
          [l.strip() for l in t.splitlines() if "pkg." in l][:2])

    # C12: eggkg plan -> parse + validasi atomik (3 langkah [install])
    t, hit = run_cmd(rig, "eggkg plan", "3", 30)
    check("C12 eggkg plan (validasi atomik resep [install])",
          hit and ("valid" in t or "OK" in t or "langkah" in t),
          [l.strip() for l in t.splitlines()
           if "langkah" in t or "valid" in t or "install" in l][:3])

    # C13: config callers menampilkan aksi eggkg terdaftar saat boot
    t, hit = run_cmd(rig, "config callers", "pkg.db_record", 30)
    check("C13 config callers (eggkg aksi: compile_ruf_eggkg/install/db)",
          hit and "mtcc.compile_ruf_eggkg" in t and "pkg.install_bin" in t,
          [l.strip() for l in t.splitlines()
           if "pkg." in l or "mtcc." in l][:3])

    # ---- T3: mtcc.ecf ----------------------------------------------------
    # C14: config init mtcc -> seed .config/mtcc.ecf
    t, hit = run_cmd(rig, "config init mtcc", "mtcc.ecf", 30)
    check("C14 config init mtcc (seed .config/mtcc.ecf)",
          hit and "[OK]" in t,
          [l.strip() for l in t.splitlines() if "mtcc.ecf" in l][:1])

    # C15: config get mtcc format.default -> mrp (default bawaan)
    t, hit = run_cmd(rig, "config get mtcc format.default", "mrp", 30)
    check("C15 config get mtcc format.default = mrp",
          hit,
          [l.strip() for l in t.splitlines() if "format" in l][:1])

    rig.quit()
    report()
    return 1 if b32.FAIL else 0


def report():
    print(f"\n[config-test] RESULT: {len(b32.PASS)} PASS, {len(b32.FAIL)} FAIL")
    for name in b32.PASS:
        print("  PASS:", name)
    if b32.FAIL:
        print("  failed:", ", ".join(b32.FAIL))


if __name__ == "__main__":
    sys.exit(main())
