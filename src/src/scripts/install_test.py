#!/usr/bin/env python3
"""install_test.py — alur instalasi MANUAL tanpa wizard (v0.5, 23 check).

Skenario: ISO + DUA citra IDE yang dibangun sendiri oleh test ini
(dist/disk.img adalah artefak BERSAMA — jangan disentuh):

  hda (IDE 0, slot 0)  FAT32 label EQDISK, auto-mount di /mnt saat boot
                       berisi /hello.es (skrip eqshell, ditanam via mtools)
  hdb (IDE 1, slot 1)  disk KOSONG (tanpa partisi) -> target `Qfs -format`

  I1   boot ke shell (ISO + 2 disk)
  I2   Qfs tanpa argumen -> bantuan
  I3   Qfs -list-disk : hda (EQDISK, mounted) + hdb (tanpa FAT32)
  I4   Qfs -t hda -format fat32 DITOLAK (volume sedang terpasang)
  I5   Qfs -t hdd -format fat32 -> "tidak ada disk"
  I6   umount hda -> "/mnt released"
  I7   Qfs -list-disk -> hda bukan mounted lagi
  I8   mount hda -> mounted lagi di /mnt
  I9   mount hdd -> "tidak ada disk"
  I10  `umount` / `mount` TANPA argumen = perilaku lama (regresi)
  I11  Qfs -t hdb -format fat32 -> sukses, label EQUINOXBASE
  I12  copy 1 file ke path baru -> direktori tujuan DIBUAT otomatis
  I13  copy ulang -> idempoten ("sudah ada")
  I14  copy pohon (cp -r, tujuan baru) -> isi jadi tujuan
  I15  copy glob `<dir>/*` -> isi direktori
  I16  copy guard -> subpohon beririsan dengan tujuan DILEWATI
  I17  del file
  I18  del pohon rekursif
  I19  del / -> ditolak
  I20  set -d FILE : clone base (value NONE), `set` menulis ke target,
       pointer active.conf di system.ecf, reset
  I21  set -b PATH : base.path tercatat + "diterapkan saat reboot"
  I22  set -x FILE : skrip .es jalan + /eqshell.log berisi transkrip
  I23  equinoxinstall -compile <dir> : HANYA fase [4/4]

Usage:  python3 scripts/install_test.py
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

MKFS = os.path.join(HERE, "make_fat32_img.py")
PART_OFF = 2048 * 512          # offset partisi EQDISK (LBA 2048)

SCRIPT_ES = """[Eqshell]
Log=True
# komentar ini dilewati

diskinfo
Qfs -list-disk
"""


def mtool(args, img):
    env = dict(os.environ)
    r = subprocess.run(args + ["-i", f"{img}@@{PART_OFF}"], capture_output=True,
                       text=True, env=env)
    return r.returncode == 0, (r.stdout + r.stderr)


def build_images(disk1, disk2):
    """hda = EQDISK berisi /hello.es ; hdb = disk kosong 64 MB."""
    r = subprocess.run([sys.executable, MKFS, "--size-mb", "64", "--out",
                        disk1], capture_output=True, text=True)
    if r.returncode != 0:
        return False, r.stdout + r.stderr

    tmp = disk1 + ".es"
    with open(tmp, "w") as f:
        f.write(SCRIPT_ES)
    ok, out = mtool(["mcopy", tmp, "::/hello.es"], disk1)
    os.unlink(tmp)
    if not ok:
        return False, out

    with open(disk2, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    return True, "ok"


def boot(disk1, disk2):
    Qemu.EXTRA = ["-boot", "order=d",
                  "-drive", f"file={disk1},format=raw,if=ide,index=0,media=disk",
                  "-drive", f"file={disk2},format=raw,if=ide,index=1,media=disk"]
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    return Qemu(ISO)


def sh(q, line, wait=1.8):
    base = len(serial())
    q.type_line(line, wait=wait)
    return since(base, 60000)


def main():
    if not os.path.exists(ISO):
        print("[install] belum ada:", ISO, "— jalankan `make`")
        return 2
    if not os.path.exists(MKFS):
        print("[install] belum ada:", MKFS)
        return 2

    work = tempfile.mkdtemp(prefix="equix-inst-")
    disk1 = os.path.join(work, "hda.img")
    disk2 = os.path.join(work, "hdb.img")
    ok, log = build_images(disk1, disk2)
    if not ok:
        print("[install] gagal membangun citra:\n" + log)
        return 2
    print("[install] boot (hda = EQDISK + hdb = kosong)", flush=True)

    q = None
    try:
        q = boot(disk1, disk2)
        ok = wait_serial("root::users", 150, q)
        check("I1 boot ke shell (ISO + 2 disk)", ok)
        if not ok:
            print(serial()[-2500:])
            return 1

        # ---------- Qfs ----------
        out = sh(q, "Qfs")
        check("I2 Qfs tanpa argumen -> bantuan",
              "bantuan ini" in out and "Qfs -list-disk" in out,
              [l for l in out.split("\n") if "Qfs" in l][:3])

        out = sh(q, "Qfs -list-disk")
        hda = [l for l in out.split("\n") if l.startswith("hda")]
        hdb = [l for l in out.split("\n") if l.startswith("hdb")]
        check("I3 Qfs -list-disk: hda EQDISK (mounted) + hdb tanpa FAT32",
              hda and hdb and "label=EQDISK" in hda[0]
              and "mounted" in hda[0] and "FAT32=0" in hdb[0],
              hda[:1] + hdb[:1])

        out = sh(q, "Qfs -t hda -format fat32")
        check("I4 Qfs -format DITOLAK pada volume yang terpasang",
              "sedang terpasang" in out and "umount" in out,
              out.strip()[-200:])

        out = sh(q, "Qfs -t hdd -format fat32")
        check("I5 Qfs -format pada slot kosong -> tidak ada disk",
              "hdd" in out and "tidak ada disk" in out,
              out.strip()[-200:])

        # ---------- mount / umount ----------
        out = sh(q, "umount hda", wait=2.0)
        check("I6 umount hda -> /mnt released",
              "/mnt released" in out, out.strip()[-200:])

        out = sh(q, "Qfs -list-disk")
        check("I7 setelah umount, hda bukan mounted",
              hda and "mounted" not in [l for l in out.split("\n")
                                        if l.startswith("hda")][0],
              [l for l in out.split("\n") if l.startswith("hda")])

        out = sh(q, "mount hda", wait=2.5)
        check("I8 mount hda -> volume terpasang lagi",
              "mounted at /mnt" in out or "mounted as ROOT" in out,
              out.strip()[-200:])

        out = sh(q, "mount hdd", wait=2.0)
        check("I9 mount hdd -> tidak ada disk",
              "hdd" in out and "tidak ada disk" in out,
              out.strip()[-200:])

        o = sh(q, "umount", wait=2.0)
        o += sh(q, "mount", wait=2.5)
        check("I10 `umount`/`mount` TANPA argumen tetap perilaku lama",
              "/mnt released" in o and "mounted at /mnt" in o,
              o.strip()[-300:])

        out = sh(q, "Qfs -t hdb -format fat32", wait=6.0)
        out += sh(q, "Qfs -list-disk")
        check("I11 Qfs -t hdb -format fat32 -> EQUINOXBASE",
              "format hdb -> FAT32 label EQUINOXBASE" in out
              and "label=EQUINOXBASE" in out,
              [l for l in out.split("\n") if l.startswith("hdb")])

        # ---------- copy / del ----------
        out = sh(q, "copy /equinox/tools/mtcc.mrp /mnt/t1", wait=4.0)
        check("I12 copy file ke path baru (dir dibuat otomatis)",
              "1 file" in out and "/mnt/t1" in out, out.strip()[-250:])

        out = sh(q, "copy /equinox/tools/mtcc.mrp /mnt/t1", wait=4.0)
        check("I13 copy ulang -> idempoten (sudah ada, dilewati)",
              "sudah ada" in out, out.strip()[-250:])

        out = sh(q, "copy /equinox/libc /mnt/libc", wait=8.0)
        o2 = sh(q, "ls /mnt/libc", wait=2.0)
        check("I14 copy pohon (cp -r, tujuan baru = isi jadi tujuan)",
              "membuat /mnt/libc" in out and ".c" in o2.lower(),
              o2.strip()[-300:])

        out = sh(q, "copy /equinox/repo/bash/* -> /mnt/gdir", wait=8.0)
        o2 = sh(q, "ls /mnt/gdir", wait=2.0)
        check("I15 copy glob <dir>/* -> isi direktori tersalin",
              "membuat /mnt/gdir" in out and ".c" in o2.lower(),
              (out.strip()[-200:] + " | " + o2.strip()[-200:]))

        out = sh(q, "copy /mnt/* -> /mnt", wait=6.0)
        check("I16 copy guard: subpohon beririsan DILEWATI, 0 file",
              "beririsan dengan tujuan" in out and "0 file" in out,
              out.strip()[-400:])

        out = sh(q, "del /mnt/t1", wait=3.0)
        check("I17 del file", "/mnt/t1 dihapus" in out, out.strip()[-200:])

        out = sh(q, "del /mnt/libc", wait=5.0)
        o2 = sh(q, "ls /mnt", wait=2.0)
        check("I18 del pohon rekursif",
              "dihapus" in out and "libc" not in o2,
              out.strip()[-250:])

        out = sh(q, "del /", wait=2.0)
        check("I19 del / DITOLAK", "menolak hapus /" in out,
              out.strip()[-200:])

        # ---------- set -d / set -b ----------
        acc = sh(q, "set net.driver e1000", wait=2.0)
        acc += sh(q, "set -d drivers.ecf", wait=2.5)
        acc += sh(q, "set net.driver ne2000", wait=2.0)
        acc += sh(q, "cat /mnt/equinox/conf/system.ecf", wait=2.0)
        acc += sh(q, "cat /mnt/equinox/conf/drivers.ecf", wait=2.0)
        acc += sh(q, "set -d", wait=2.0)
        check("I20 set -d: clone base (NONE), tulis ke target, pointer, reset",
              "dibuat dari" in acc and "key = NONE" in acc
              and "target = /mnt/equinox/conf/drivers.ecf" in acc
              and "net.driver = e1000" in acc
              and "net.driver = ne2000" in acc
              and "target kembali ke system.ecf" in acc,
              [l for l in acc.split("\n")
               if "target" in l or "NONE" in l or "net.driver" in l][:8])

        out = sh(q, "set -b /", wait=2.5)
        check("I21 set -b PATH -> base.path tercatat (efektif saat reboot)",
              "base.path = /" in out and "diterapkan saat reboot" in out,
              out.strip()[-250:])

        # ---------- set -x (skrip .es) ----------
        base = len(serial())
        q.type_line("set -x /mnt/hello.es", wait=1.0)
        ok = wait_serial("es: selesai", 60, q)
        out = since(base, 60000)
        log = sh(q, "cat /eqshell.log", wait=2.5)
        check("I22 set -x menjalankan skrip .es + /eqshell.log transkrip",
              ok and "es: menjalankan /mnt/hello.es (Log=True)" in out
              and "es: selesai (2 baris, 0 error)" in out
              and "> diskinfo" in log and "== eqshell:" in log,
              (out.strip()[-300:] + " || LOG: " + log.strip()[-300:]))

        # ---------- equinoxinstall -compile ----------
        base = len(serial())
        q.type_line("equinoxinstall -compile /equinox", wait=1.0)
        ok = wait_serial("equinoxinstall done", 420, q)
        out = since(base, 600000)
        check("I23 equinoxinstall -compile <dir> hanya fase [4/4]",
              ok and "[compile] sumber: /equinox" in out
              and "[4/4] build userland dari /equinox" in out
              and "[1/4]" not in out and "0 failed" in out,
              [l for l in out.split("\n")
               if "compile]" in l or "[4/4]" in l or "summary" in l][:5])
    finally:
        if q is not None:
            try:
                q.quit()
            except Exception:
                pass
        time.sleep(0.5)
        shutil.rmtree(work, ignore_errors=True)

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
