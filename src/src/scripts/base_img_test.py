#!/usr/bin/env python3
"""base_img_test.py — uji image instalasi EQUINOXBASE (v0.2.1).

Boot dist/equinox.iso DENGAN dist/equinox.img terpasang (IDE primary
master) dan memverifikasi bahwa kernel MEMPROMOSIKAN volume FAT32
ber-label EQUINOXBASE menjadi ROOT filesystem, lalu bahwa build benar-
benar menulis ke disk dan bertahan setelah reboot:

  T1  boot ke shell (ISO + image)
  T2  diskinfo  : volume terpasang di "/" (install base) + label
  T3  isi image terlihat di root (/)   (ls, setelah cd /)
  T4  /README.TXT (file khusus image) terbaca
  T5  /test/*.c  dari image
  T6  equinoxinstall : "build base = FAT32 /" + summary "0 failed"
  T7  hasil build (ls.mrp) terlihat lewat ls
  T8  REBOOT -> hasil build MASIH ADA di /equinox/tools (persisten)
  T9  REBOOT -> /README.TXT masih ada
  T10 REBOOT -> sumber .c di-restore modul GRUB, .mrp tetap ada
  T11 umount DITOLAK (base = root filesystem)
  T12 root tetap hidup setelah umount ditolak
  T13 boot image pristine murni read-only (hash identik -> modul
      GRUB tidak ditulis ulang, jalur '(already installed)' jalan)

Catatan:
  * image asli TIDAK disentuh — tes menyalinnya ke /tmp dulu, karena
    equinoxinstall MEMBUANG sumber .c setelah build sukses.
  * boot WAJIB pakai `-boot order=d`: MBR image ber-0x55AA (wajib untuk
    kernel) tapi berisi INT 18h, jadi firmware yang mencoba disk dulu
    langsung jatuh ke CD.
  * perintah bergaya-userland (ls <path>, cat <abs>) butuh .mrp yang
    belum tentu ada di boot baru — pakai builtin tanpa argumen + cwd.
  * boot kernel tidak mengirim printf-nya ke port serial (hanya banner
    shell), jadi semua asersi memakai perintah shell.

Usage:  python3 scripts/base_img_test.py
"""
import hashlib
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import (Qemu, ISO, SERIAL, serial, since, wait_serial,
                            check, PASS, FAIL)
import boot_test_v032 as b32

IMG = os.path.join(b32.ROOT, "dist", "equinox.img")


def boot(img):
    """Boot ISO + image instalasi; serial file di-reset tiap boot."""
    # -boot order=d WAJIB: image memakai MBR 0x55AA tanpa kode boot
    # (berkas data), jadi SeaBIOS harus boot dari CD dulu — kalau tidak,
    # ia melompat ke 512 byte nol itu dan macet (lihat run-disk/run-img).
    Qemu.EXTRA = ["-boot", "order=d",
                  "-drive", f"file={img},format=raw,if=ide,index=0,media=disk"]
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    return Qemu(ISO)


def sha(path):
    """SHA-256 berkas (dipakai T13: boot harus MEMBACA saja)."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sh(q, line, wait=1.5):
    """Ketik satu perintah shell, kembalikan outputnya sejak sebelumnya."""
    base = len(serial())
    q.type_line(line, wait=wait)
    return since(base)


def main():
    if not os.path.exists(IMG):
        print("[base] image belum ada:", IMG, "— jalankan `make img`")
        return 2
    if not os.path.exists(ISO):
        print("[base] ISO belum ada:", ISO, "— jalankan `make`")
        return 2

    # salinan kerja: tes ini mengubah image (equinoxinstall)
    work = tempfile.mktemp(prefix="equix-base-", suffix=".img")
    shutil.copyfile(IMG, work)
    print("[base] boot #1 (image:", os.path.basename(work), ")", flush=True)

    q = None
    try:
        q = boot(work)
        ok = wait_serial("root::users", 150, q)
        check("T1 boot ke shell (ISO + image)", ok)
        if not ok:
            print(serial()[-2500:])
            return 1

        out = sh(q, "diskinfo", wait=2.0)
        check("T2 volume EQUINOXBASE terpasang sebagai ROOT (/)",
              "FAT32 volume at / (install base)" in out
              and "EQUINOXBASE" in out,
              [l for l in out.split("\n")
               if "FAT32 volume" in l or "label" in l][:3])

        # Shell hanya punya varian builtin `ls` TANPA argumen (varian
        # ber-argumen = tool ls.mrp, belum tentu ada di boot baru) —
        # jadi telusuri lewat cd, bukan path absolut. Nama 8.3 di FAT32
        # sering huruf besar ("EQUINOX", "HELLO.C") -> bandingkan ci.
        sh(q, "cd /")
        out = sh(q, "ls")
        low = out.lower()
        check("T3 isi image terlihat di root (/)",
              "equinox" in low and "readme.txt" in low and "test" in low,
              out[-300:])

        out = sh(q, "cat README.TXT")
        check("T4 /README.TXT (file khusus image) terbaca",
              "EQUINOXBASE" in out, out[-300:])

        sh(q, "cd test")
        out = sh(q, "ls")
        check("T5 /test/*.c dari image", "hello.c" in out.lower(), out[-300:])
        sh(q, "cd /")

        # ---- T6: build ke FAT32 (mtcc menulis .mrp lewat write-through) ----
        base = len(serial())
        q.type_line("equinoxinstall", wait=1.0)
        # wizard v0.4: [1/4] pilih disk -> 1 (satu-satunya volume,
        # EQUINOXBASE terpasang sebagai /), lalu [3/4] driver NIC
        # dengan Enter = default ne2000 (menulis boot/system.ecf).
        q.type_line("1", wait=1.5)
        q.type_line("", wait=1.5)
        ok = wait_serial("equinoxinstall done", 300, q)
        out = since(base, 120000)
        check("T6a equinoxinstall: build base = FAT32 / (EQUINOXBASE)",
              "build base = FAT32 /" in out,
              [l for l in out.split("\n") if "build base" in l])
        check("T6b equinoxinstall: semua job terpasang, 0 failed",
              ok and "0 failed" in out and "no .c sources" not in out,
              [l for l in out.split("\n")
               if "summary" in l or "done —" in l or "no .c" in l])

        out = sh(q, "cd /equinox/tools")
        out = sh(q, "ls")
        check("T7 hasil build (ls.mrp) ada di /equinox/tools",
              "ls.mrp" in out.lower(), out[-300:])
        q.quit()
        q = None

        # ---- T8/T9: reboot -> apa yang ditulis tetap ada ----
        print("[base] boot #2 (persistensi)", flush=True)
        q = boot(work)
        ok = wait_serial("root::users", 150, q)
        check("T8 boot ulang", ok)
        if ok:
            out = sh(q, "cd /equinox/tools")
            out = sh(q, "ls")
            check("T9 hasil build bertahan setelah reboot (persisten)",
                  "ls.mrp" in out.lower(), out[-300:])
            sh(q, "cd /")
            out = sh(q, "cat README.TXT")
            check("T9b /README.TXT masih ada setelah reboot",
                  "EQUINOXBASE" in out, out[-300:])

            # Perilaku yang dikehendaki (dikunci di sini supaya tidak
            # terlihat "aneh"): berkas .c adalah MODUL GRUB. equinoxinstall
            # membuangnya setelah build, lalu mrp_bootloader meng-restore
            # yang hilang pada boot berikutnya (ensure_system_layout) —
            # jadi setelah reboot sumber kembali BERSAMA .mrp hasil build.
            sh(q, "cd /equinox/tools")   # T9b meninggalkan cwd di /
            out = sh(q, "ls")
            low = out.lower()
            check("T10 reboot: sumber .c di-restore GRUB, .mrp tetap ada",
                  "ls.c" in low and "ls.mrp" in low, out[-300:])

            # Base = root: umount harus DITOLAK, kalau tidak shell
            # kehilangan cwd-nya sendiri dan root mati.
            out = sh(q, "umount", wait=1.5)
            check("T11 umount ditolak pada install base",
                  "refusing" in out and "EQUINOXBASE is the ROOT" in out,
                  out[-300:])
            out = sh(q, "ls")
            check("T12 root tetap hidup setelah umount ditolak",
                  ".mrp" in out.lower(), out[-300:])

        # FIX: boot #2 HARUS dimatikan sebelum boot #3 — tanpa ini
        # `q = boot(ro)` di bawah menimpa referensinya dan QEMU boot #2
        # tertinggal hidup (satu-satunya tes yang pakai -drive
        # /tmp/equix-base-*.img). QEMU yatim itu masih menulis ke file
        # serial BERSAMA (/tmp/boot_v032_serial.log) + memperebutkan
        # CPU dengan suite berikutnya -> equix2/fat32 bisa berubah
        # hasilnya. Satu QEMU pada satu waktu.
        if q is not None:
            q.quit()
            q = None
            time.sleep(1.0)

        # ---- T13: boot pada image PRISTINE harus murni READ ----
        # mrp_bootloader me-skip modul yang sudah ada dengan ukuran sama
        # (jalur "(already installed)"). Kalau skip-nya gagal, ~85 modul
        # ditulis ulang lewat FAT32 dan field waktu di dirent ikut
        # berubah -> hash berbeda. Boot di image BARU terpisah, karena
        # boot di atas sudah kotor oleh equinoxinstall.
        ro = tempfile.mktemp(prefix="equix-ro-", suffix=".img")
        shutil.copyfile(IMG, ro)
        sha0 = sha(ro)
        print("[base] boot #3 (image pristine — cek read-only)", flush=True)
        q = boot(ro)
        ok = wait_serial("root::users", 150, q)
        q.quit()
        q = None
        time.sleep(2)
        sha1 = sha(ro)
        check("T13 boot hanya membaca volume (image identik byte-per-byte)",
              ok and sha0 == sha1,
              {"pristine": sha0[:16], "setelah boot": sha1[:16]})
        try:
            os.unlink(ro)
        except OSError:
            pass
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
