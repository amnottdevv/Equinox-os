#!/usr/bin/env python3
"""installer_wizard_test.py — wizard `equinoxinstall` 4 langkah (v0.4).

Skenario (DUA disk di IDE):
  hda : 128 MB KOSONG  -> dipilih sebagai target -> diformat
        (fat32_mkfs: MBR + partisi FAT32 0x0C @ LBA 2048, label
         EQUINOXBASE) lalu dipakai sebagai volume build di /mnt
  hdb : citra FAT32 label EQDISK (dibangun sendiri oleh test, bukan
        dist/disk.img yang bisa tertimpa image EQUINOXBASE) ->
        cukup sebagai menu (membuktikan daftar disk multi-slot)

Boot #1 — jalankan wizard:
  W1  boot ke shell (ISO + 2 disk, -boot order=d)
  W2  [1/4] tabel disk: hda "kosong", hdb FAT32, opsi 0 in place
  W3  [2/4] format hda -> "MBR + partisi FAT32 0x0C @ LBA 2048"
  W4  baris kompatibilitas "build base = FAT32 /mnt"
  W5  layout disalin ke volume (> 0 file)
  W6  [3/4] pilihan NIC 2 -> hda/boot/system.ecf = e1000
  W7  [4/4] build selesai, summary "0 failed"
  W8  diskinfo: volume terpasang di /mnt, label EQUINOXBASE
  W9  isi /mnt terlihat (boot, equinox, test, user, README.TXT)
  W10 /mnt/boot berisi kernel.elf + system.ecf (= e1000)
  W11 hasil build (.mrp) mendarat di /mnt/equinox/tools

Boot #2 — reboot dgn KEDUA NIC (ne2000 ISA + e1000 PCI), jadi driver
yang dipilih boot/system.ecf TERLIHAT di ifconfig:
  W12 volume EQUINOXBASE ter-promosi menjadi ROOT (/)
  W13 README.TXT + hasil build bertahan (persisten)
  W14 sumber .c TETAP ADA (mode target tidak membuang sumber)
  W15 ifconfig: driver e1000-pci  -> pilihan system.ecf dihormati
      (tanpa system.ecf urutan registrasi memilih ne2000-isa)

Usage: python3 installer_wizard_test.py [iso]
"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32
from boot_test_v032 import Qemu, ISO, SERIAL, serial, since, wait_serial, check

MKFS = os.path.join(HERE, "make_fat32_img.py")
BLANK_MB = 128
# NIC kedua: Qemu.__init__ SELALU menyusul dengan modul global NET dari
# boot_test_v032, jadi NIC harus MENGGANTI NET itu (bukan ditambah ke
# EXTRA — id net0 kembar membuat QEMU langsung keluar saat CLI parsing).
DEFAULT_NET = list(b32.NET)
NIC_BOTH = ["-netdev", "user,id=net0",
            "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9",
            "-netdev", "user,id=net1",
            "-device", "e1000,netdev=net1"]


def sh(q, line, wait=1.5):
    base = len(serial())
    q.type_line(line, wait=wait)
    return since(base)


def boot(drives, nic=False):
    Qemu.EXTRA = ["-boot", "order=d"]
    for i, d in enumerate(drives):
        Qemu.EXTRA += ["-drive",
                       f"file={d},format=raw,if=ide,index={i},media=disk"]
    b32.NET = list(NIC_BOTH) if nic else list(DEFAULT_NET)
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    return Qemu(ISO)


def main():
    if not os.path.exists(ISO):
        print("[wiz] dist/equinox.iso belum ada — jalankan `make`")
        return 2
    if not os.path.exists(MKFS):
        print("[wiz] belum ada:", MKFS)
        return 2

    blank = tempfile.mktemp(prefix="wiz-blank-", suffix=".img")
    other = tempfile.mktemp(prefix="wiz-other-", suffix=".img")
    with open(blank, "wb") as f:
        f.truncate(BLANK_MB * 1024 * 1024)
    # hdb: citra EQDISK milik test — dist/disk.img adalah artefak
    # bersama yang bisa berubah labelnya (bila EQUINOXBASE, volume ini
    # ikut ter-promosi ke / dan pilihan disk wizard jadi kabur).
    r = subprocess.run([sys.executable, MKFS, "--size-mb", "64",
                        "--out", other],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("[wiz] gagal membangun citra EQDISK:\n" + r.stdout + r.stderr)
        return 2

    q = None
    try:
        # ================= BOOT #1 : wizard =====================
        print(f"[wiz] boot #1 — hda {BLANK_MB} MB kosong + hdb EQDISK",
              flush=True)
        q = boot([blank, other])
        ok = wait_serial("root::users", 150, q)
        check("W1 boot ke shell (ISO + 2 disk)", ok)
        if not ok:
            print(serial()[-2500:])
            return 1

        base = len(serial())
        q.type_line("equinoxinstall", wait=2.0)
        t = wait_serial("target [0]:", 60, q)
        win = since(base, 20000)
        check("W2 [1/4] tabel disk: hda kosong + hdb FAT32 + opsi 0",
              t and "hda" in win and "hdb" in win
              and "kosong" in win and "build in place" in win,
              [l for l in win.split("\n") if ") h" in l or "in place" in l][:4])

        q.type_line("1", wait=1.5)                    # pilih hda
        ok = wait_serial("Format hda jadi base equinox?", 45, q)
        check("W3 [2/4] disk kosong ditawari format", ok)
        base = len(serial())
        q.type_line("y", wait=2.0)
        t = wait_serial("label EQUINOXBASE", 60, q)
        check("W3b fat32_mkfs: MBR + partisi FAT32 0x0C @ LBA 2048",
              t, [l for l in since(base, 8000).split("\n")
                  if "diformat" in l][:1])

        t = wait_serial("build base = FAT32 /mnt", 45, q)
        check("W4 build base = FAT32 /mnt (sumber & hasil di volume)",
              t)
        t = wait_serial("layout disalin ke volume:", 180, q)
        nf = 0
        full = serial()
        for l in full.split("\n"):
            if "layout disalin ke volume:" in l:
                print(f"[W5 DEBUG] found line: {repr(l)}")
                try:
                    after_colon = l.split("layout disalin ke volume:")[1]
                    print(f"[W5 DEBUG] after_colon: {repr(after_colon)}")
                    nf = int(after_colon.split("file")[0].strip())
                    print(f"[W5 DEBUG] nf={nf}")
                except (ValueError, IndexError) as e:
                    print(f"[W5 DEBUG] parse error: {e}")
                    nf = 0
                break
        check("W5 layout disalin ke volume (> 0 file)", t and nf > 0,
              f"{nf} file")

        t = wait_serial("pilihan [1]:", 45, q)
        check("W6 [3/4] prompt driver NIC muncul", t)
        base = len(serial())
        q.type_line("2", wait=2.0)                    # e1000
        t = wait_serial("system.ecf -> hda/equinox/conf/system.ecf = e1000", 45, q)
        check("W6b system.ecf ditulis = e1000", t,
              [l for l in since(base, 6000).split("\n")
               if "system.ecf" in l][:1])

        ok = wait_serial("equinoxinstall done", 600, q)
        win = since(base, 200000)
        check("W7 build selesai + 0 failed",
              ok and "0 failed" in win,
              [l for l in win.split("\n")
               if "summary" in l or "done —" in l][:2])

        # v0.9.2: baris status live [5/5] menyusul SETELAH baris
        # "equinoxinstall done" — shell meng-flush input yang menumpuk
        # saat kembali dari builtin, jadi drain dulu dengan perintah
        # sync sebelum mengetik diskinfo (kalau tidak, "diskinfo"
        # terelan dan W8 menangkap jendela kosong).
        sh(q, "pwd", wait=2.5)
        out = sh(q, "diskinfo", wait=8.0)
        check("W8 diskinfo: volume terpasang di /mnt, label EQUINOXBASE",
              "FAT32 volume at /mnt" in out and "EQUINOXBASE" in out,
              [l for l in out.split("\n") if l.strip()][:14])

        sh(q, "cd /mnt")
        out = sh(q, "ls")
        low = out.lower()
        check("W9 isi /mnt: boot, equinox, test, user, README",
              all(k in low for k in ("boot", "equinox", "test", "user"))
              and "readme.txt" in low, out[-300:])
        out = sh(q, "cat README.TXT")
        check("W9b /mnt/README.TXT berlabel EQUINOXBASE",
              "EQUINOXBASE" in out, out[-200:])

        sh(q, "cd /mnt/equinox/conf")
        out = sh(q, "ls")
        low = out.lower()
        check("W10 /mnt/equinox/conf: system.ecf (lokasi kanonik v0.9.2)",
              "system.ecf" in low, out[-300:])
        out = sh(q, "cat system.ecf")
        check("W10b system.ecf berisi e1000", "e1000" in out, out[-120:])

        sh(q, "cd /mnt/equinox/tools")
        out = sh(q, "ls")
        check("W11 hasil build ada di /mnt/equinox/tools",
              ".mrp" in out.lower(), out[-300:])
        q.quit()
        q = None

        # ================= BOOT #2 : persistensi + system.ecf ====
        print("[wiz] boot #2 — promosi ke / + kedua NIC", flush=True)
        q = boot([blank, other], nic=True)
        ok = wait_serial("root::users", 150, q)
        check("W12 boot ulang", ok)
        if ok:
            out = sh(q, "diskinfo", wait=2.5)
            check("W12b EQUINOXBASE ter-promosi jadi ROOT (/)",
                  "FAT32 volume at / (install base)" in out
                  and "EQUINOXBASE" in out,
                  [l for l in out.split("\n")
                   if "FAT32 volume" in l or "label" in l][:2])

            sh(q, "cd /")
            out = sh(q, "ls")
            low = out.lower()
            check("W13 isi root (/) bertahan (README, boot, equinox)",
                  "readme.txt" in low and "boot" in low
                  and "equinox" in low, out[-300:])

            sh(q, "cd /equinox/tools")
            out = sh(q, "ls")
            low = out.lower()
            check("W13b hasil build (.mrp) persisten setelah reboot",
                  "wc.mrp" in low, out[-300:])
            check("W14 sumber .c tidak dibuang (mode target)",
                  "wc.c" in low, out[-300:])

            out = sh(q, "ifconfig", wait=3.0)
            check("W15 ifconfig: driver e1000-pci (system.ecf dihormati)",
                  "driver e1000-pci" in out and "inet 10.0.2.15" in out,
                  [l.strip() for l in out.split("\n")
                   if "driver" in l or "inet" in l][:2])
    finally:
        if q is not None:
            try:
                q.quit()
            except Exception:
                pass
        time.sleep(0.5)
        for p in (blank, other):
            try:
                os.unlink(p)
            except OSError:
                pass

    print("\n== hasil ==")
    for n in b32.PASS:
        print(f"  [PASS] {n}")
    for n in b32.FAIL:
        print(f"  [FAIL] {n}")
    print(f"\nPASS {len(b32.PASS)} / FAIL {len(b32.FAIL)}")
    if b32.FAIL:
        print("FAILED:", b32.FAIL)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
