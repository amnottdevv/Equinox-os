#!/usr/bin/env python3
"""mnt_build_test.py — equinoxinstall pada disk FAT32 BIASA (bukan base).

Skenario: ISO + citra FAT32 label EQDISK (dibangun sendiri oleh test,
lihat build_eqdisk) — volume terpasang di /mnt,
bukan sebagai ROOT, jadi pohon /user & /equinox tetap di RAMFS. Persis
kasus "kalau ada /mnt -> tempat hasil build di FAT32".

  T1  boot ke shell (ISO + disk)
  T2  diskinfo : volume di /mnt (bukan install base), label EQDISK
  T3  equinoxinstall : "build base = FAT32 /mnt" (bukan RAMFS, bukan /)
  T4  summary "0 failed" + ekspor artefak ke /mnt/equinox/...
  T5  /mnt/equinox/tools berisi .mrp hasil build (ada di DISK)
  T6  /equinox/tools (RAMFS) tetap terisi — tool tetap bisa dijalankan

Image disk ASLI tidak disentuh: tes menyalinnya ke /tmp (installer
membuang sumber .c setelah build).

Usage:  python3 scripts/mnt_build_test.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from boot_test_v032 import (Qemu, ISO, SERIAL, serial, since, wait_serial,
                            check, PASS, FAIL)
import boot_test_v032 as b32

DISK = os.path.join(b32.ROOT, "dist", "disk.img")   # rujukan `make diskimg`
MKFS = os.path.join(HERE, "make_fat32_img.py")


def boot(img):
    # -boot order=d: MBR ber-0x55AA tanpa kode boot tidak boleh dijalankan
    Qemu.EXTRA = ["-boot", "order=d",
                  "-drive", f"file={img},format=raw,if=ide,index=0,media=disk"]
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    return Qemu(ISO)


def sh(q, line, wait=1.5):
    base = len(serial())
    q.type_line(line, wait=wait)
    return since(base)


def build_eqdisk(path):
    """Citra FAT32 label EQDISK milik tes ini.

    dist/disk.img itu artefak BERSAMA (`make diskimg`): bila labelnya
    tertimpa jadi EQUINOXBASE, kernel mempromosinya ke ROOT (/) dan
    seluruh skenario "volume biasa di /mnt" gugur (T2-T5). Maka test
    ini membuat citranya sendiri — deterministik, sama seperti
    fat32_write_test yang juga membangun ulang disk-nya sendiri.
    """
    r = subprocess.run([sys.executable, MKFS, "--size-mb", "64",
                        "--out", path],
                       capture_output=True, text=True)
    return r.returncode == 0, r.stdout + r.stderr


def main():
    if not os.path.exists(ISO):
        print("[mnt] belum ada:", ISO, "— jalankan `make`")
        return 2
    if not os.path.exists(MKFS):
        print("[mnt] belum ada:", MKFS)
        return 2

    work = tempfile.mktemp(prefix="equix-mnt-", suffix=".img")
    ok, log = build_eqdisk(work)
    if not ok:
        print("[mnt] gagal membangun citra EQDISK:\n" + log)
        return 2
    print("[mnt] boot (disk EQDISK:", os.path.basename(work), ")",
          flush=True)

    q = None
    try:
        q = boot(work)
        ok = wait_serial("root::users", 150, q)
        check("T1 boot ke shell (ISO + disk)", ok)
        if not ok:
            print(serial()[-2500:])
            return 1

        out = sh(q, "diskinfo", wait=2.0)
        check("T2 disk biasa terpasang di /mnt (bukan install base)",
              "FAT32 volume at /mnt" in out and "EQDISK" in out
              and "install base" not in out,
              [l for l in out.split("\n")
               if "FAT32 volume" in l or "label" in l][:3])

        base = len(serial())
        q.type_line("equinoxinstall", wait=1.0)
        # wizard v0.4: [1/4] pilih disk -> 1 (volume EQDISK di /mnt);
        # [2/4] ganti label jadi EQUINOXBASE? -> "n" (volume tes ini
        # dipakai apa adanya); [3/4] driver NIC -> Enter = ne2000.
        q.type_line("1", wait=1.5)
        q.type_line("n", wait=1.5)
        q.type_line("", wait=1.5)
        ok = wait_serial("equinoxinstall done", 300, q)
        # jendela besar: keluaran install + ekspor ke /mnt jauh > 8000 char,
        # dan since() memotong TEPAT di batasnya (summary jadi "games 3/")
        out = since(base, 120000)
        check("T3a equinoxinstall: build base = FAT32 /mnt",
              "build base = FAT32 /mnt" in out,
              [l for l in out.split("\n") if "build base" in l])
        check("T3b semua job terpasang, 0 failed",
              ok and "0 failed" in out and "no .c sources" not in out,
              [l for l in out.split("\n")
               if "summary" in l or "done —" in l or "no .c" in l])
        exp = [l.strip() for l in out.split("\n") if "/mnt/equinox/" in l]
        check("T4 artefak hasil build diekspor ke /mnt/equinox/...",
              len(exp) > 0, exp[:4])

        cd5 = sh(q, "cd /mnt/equinox/tools")
        out = sh(q, "ls")
        check("T5 /mnt/equinox/tools (di DISK) berisi .mrp hasil build",
              "/mnt/equinox/tools" in cd5.lower() and ".mrp" in out.lower(),
              out[-300:])

        cd6 = sh(q, "cd /equinox/tools")
        out = sh(q, "ls")
        check("T6 /equinox/tools (RAMFS) tetap terisi — tool tetap jalan",
              "/equinox/tools" in cd6.lower()
              and "ls.mrp" in out.lower() and "mtcc.mrp" in out.lower(),
              out[-300:])
    finally:
        if q is not None:
            try:
                q.quit()
            except Exception:
                pass
        time.sleep(0.5)
        try:
            os.unlink(work)
        except OSError:
            pass

    print("\n== hasil ==")
    for n in PASS:
        print(f"  [PASS] {n}")
    for n in FAIL:
        print(f"  [FAIL] {n}")
    print(f"\nPASS {len(PASS)} / FAIL {len(FAIL)}")
    if FAIL:
        print("FAILED:", FAIL)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
