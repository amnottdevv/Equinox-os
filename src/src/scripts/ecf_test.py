#!/usr/bin/env python3
"""ecf_test.py — Equinox OS v0.2 Beta: format .ecf + builtin `set` + `eqgu`.

25 checks, dua fase (satu QEMU pada satu waktu):

  PHASE 1 — boot ISO TANPA disk (RAMFS), 22 checks
    E1  boot to shell
    E2  `set` tanpa argumen        -> daftar kosong
    E3  `set net.driver`           -> "tidak ada kunci"
    E4  `set net.driver e1000`     -> tulis /boot/system.ecf + PERLU REBOOT
    E5  `set` (daftar)             -> net.driver = e1000
    E6  `set net.driver zzz`       -> nilai tak valid (skema)
    E7  `set foo.bar x`            -> kunci tak dikenal
    E8  `cat system.ecf`           -> isi persis hasil tulis
    E9  patch IN-PLACE             -> e1000 -> ne2000, tetap SATU baris
    E10 `set -w out.ecf`           -> "ditulis ... (1 entri)" + isi
    E11 `set -w` tanpa argumen
    E12 `set -a` tanpa argumen
    E13 `set -a nope.ecf`          -> "gagal membaca"
    E14 `save a.ecf` + `set -a`    -> 1 diterapkan, 0 tak dikenal, 0 error
    E15 `set` (store ikut)         -> net.driver = none
    E16 `set -q`                   -> usage "set: builtin .ecf"
    E17 berkas .ecf BERSKHEMA [net] dibuat lewat EDITOR + `set -a`
    E18 `eqgu` tanpa argumen
    E19 `eqgu foo.txt`             -> ditolak (bukan .c)
    E20 `eqgu ok.c` -> ketik kode valid -> Ctrl+S -> "Cek: OK"
    E21 Ctrl+Q -> "Editor closed." (keluar bersih)
    E22 `eqgu bad.c` -> kode rusak -> Ctrl+S -> "Cek: GAGAL" + "mtcc: error"

  PHASE 2 — citra FAT32 EQDISK milik tes ini, 3 checks
    E23 boot + `cd /mnt` / `pwd`  -> volume kepasang di /mnt
    E24 `cdir boot` + `set net.driver none` -> /mnt/boot/system.ecf
         (ifconfig masih UP — net.driver hanya dibaca saat boot)
    E25 boot ulang (disk yang sama) -> ifconfig "net: down (no NIC)"
         = net.driver=none dihormati net_init. (Baris boot `nic: ...`
           tidak masuk COM1: serial_init() dipanggil sesudah net_init.)

Catatan penulisan: `save X << "teks"` menulis isi TEPAT tanpa newline,
jadi berkas .ecf fase-1 hasil `save` selalu satu baris; berkas berskema
(E17) dibuat dengan editor (Enter = newline).

Usage:  python3 scripts/ecf_test.py
        make test-ecf
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from boot_test_v032 import (Qemu, ISO, SERIAL, check, PASS, FAIL,   # noqa: E402
                            serial, wait_serial)

DISK = "/tmp/opencode/ecf_disk.img"
MKFS = os.path.join(HERE, "make_fat32_img.py")


# --------------------------------------------------------------- helpers
def mark():
    """Offset serial berikutnya = awal jendela perintah berikutnya."""
    return len(serial())


def tail(s0, n=320):
    return serial()[s0:][-n:]


def wait_for(s0, pat, timeout=25):
    """Tunggu `pat` muncul di serial SETELAH offset s0."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        w = serial()[s0:]
        if pat in w:
            return True, w
        if not os.path.exists(SERIAL):
            return False, w
        time.sleep(0.3)
    return False, serial()[s0:]


def type_str(q, text, delay=0.07):
    """Ketik TEKS saja (tanpa Enter) — utk isi editor."""
    for ch in text:
        if ch == "\n":
            q.sendkey("ret", wait=delay)
        else:
            q.sendkey(q.keyname(ch), wait=delay)


def fresh_serial():
    if os.path.exists(SERIAL):
        os.remove(SERIAL)


# ------------------------------------------------------------ phase 1
def phase1():
    print("[ecf] PHASE 1 — RAMFS, tanpa disk ...", flush=True)
    fresh_serial()
    rig = Qemu(ISO)
    ok = wait_serial("user $", 180, rig)
    check("E1 boot to shell", ok, tail(0))
    if not ok:
        rig.quit()
        return False

    # E2  `set` daftar kosong
    s0 = mark()
    rig.type_line("set", wait=1.2)
    ok, w = wait_for(s0, "ecf: (kosong)", 20)
    check("E2 set tanpa argumen -> daftar kosong", ok, w[-320:])

    # E3  `set net.driver` — kunci belum ada
    s0 = mark()
    rig.type_line("set net.driver", wait=1.2)
    ok, w = wait_for(s0, "ecf: tidak ada kunci 'net.driver'", 20)
    check("E3 set KEY tanpa VALUE -> kunci tak ada", ok, w[-320:])

    # E4  tulis entri pertama
    s0 = mark()
    rig.type_line("set net.driver e1000", wait=1.5)
    ok, w = wait_for(s0, "ecf: net.driver = e1000 -> /boot/system.ecf", 25)
    reboot = "ecf: PERLU REBOOT" in w
    check("E4 set net.driver e1000 -> /boot/system.ecf + reboot",
          ok and reboot, w[-360:])

    # E5  daftar
    s0 = mark()
    rig.type_line("set", wait=1.2)
    ok, w = wait_for(s0, "net.driver = e1000", 20)
    check("E5 set (daftar) menampilkan entri", ok, w[-320:])

    # E6  nilai tak valid
    s0 = mark()
    rig.type_line("set net.driver zzz", wait=1.5)
    ok, w = wait_for(s0, "tidak valid", 20)
    sch = "pilihan: ne2000|e1000|none" in w
    check("E6 nilai di luar skema ditolak", ok and sch, w[-360:])

    # E7  kunci tak dikenal
    s0 = mark()
    rig.type_line("set foo.bar x", wait=1.5)
    ok, w = wait_for(s0, "ecf: kunci tak dikenal: foo.bar", 20)
    check("E7 kunci di luar skema ditolak", ok, w[-320:])

    # E8  isi berkas = apa yang ditulis
    rig.type_line("cd /boot", wait=1.0)
    s0 = mark()
    rig.type_line("cat system.ecf", wait=1.5)
    ok, w = wait_for(s0, "net.driver = e1000", 20)
    check("E8 /boot/system.ecf berisi persis", ok, w[-320:])

    # E9  patch IN-PLACE (nilai lebih panjang: delta > 0)
    rig.type_line("set net.driver ne2000", wait=2.0)
    time.sleep(1.0)                       # benar-benar selesai dulu
    s0 = mark()
    rig.type_line("cat system.ecf", wait=1.5)
    ok, w = wait_for(s0, "net.driver = ne2000", 20)
    ones = w.count("net.driver = ")
    check("E9 patch in-place (1 baris, e1000->ne2000)",
          ok and ones == 1 and "e1000" not in w, w[-360:])

    # E10 `set -w out.ecf` + isi berkas keluaran
    s0 = mark()
    rig.type_line("set -w out.ecf", wait=1.5)
    ok, w = wait_for(s0, "ecf: ditulis /boot/out.ecf (1 entri)", 25)
    s1 = mark()
    rig.type_line("cat out.ecf", wait=1.5)
    ok2, w2 = wait_for(s1, "net.driver = ne2000", 20)
    check("E10 set -w menulis seluruh store", ok and ok2,
          (w + w2)[-400:])

    # E11 `set -w` tanpa argumen
    s0 = mark()
    rig.type_line("set -w", wait=1.5)
    ok, w = wait_for(s0, "ecf: set -w butuh nama file", 20)
    check("E11 set -w tanpa argumen", ok, w[-320:])

    # E12 `set -a` tanpa argumen
    s0 = mark()
    rig.type_line("set -a", wait=1.5)
    ok, w = wait_for(s0, "ecf: set -a butuh nama file", 20)
    check("E12 set -a tanpa argumen", ok, w[-320:])

    # E13 `set -a` berkas tak ada
    s0 = mark()
    rig.type_line("set -a nope.ecf", wait=1.5)
    ok, w = wait_for(s0, "ecf: gagal membaca /boot/nope.ecf", 20)
    check("E13 set -a berkas hilang", ok, w[-320:])

    # E14 buat berkas .ecf satu baris + `set -a`
    rig.type_line('save a.ecf << "net.driver = none"', wait=2.0)
    s0 = mark()
    rig.type_line("set -a a.ecf", wait=2.0)
    ok, w = wait_for(s0, "ecf: 1 diterapkan, 0 tak dikenal, 0 error", 25)
    check("E14 set -a: 1 diterapkan / 0 tak dikenal / 0 error",
          ok, w[-360:])

    # E15 store ikut berubah
    s0 = mark()
    rig.type_line("set", wait=1.2)
    ok, w = wait_for(s0, "net.driver = none", 20)
    check("E15 store aktif ikut diperbarui", ok, w[-320:])

    # E16 usage
    s0 = mark()
    rig.type_line("set -q", wait=1.5)
    ok, w = wait_for(s0, "set: builtin .ecf", 20)
    check("E16 opsi tak dikenal -> usage", ok, w[-320:])

    # E17 berkas berskroma [net] dibuat lewat EDITOR
    s0 = mark()
    rig.type_line("edit sec.ecf", wait=2.5)
    type_str(rig, "[net]")
    rig.sendkey("ret", wait=0.4)
    type_str(rig, "driver = e1000")
    rig.sendkey("ret", wait=0.4)
    time.sleep(1.0)
    rig.sendkey("ctrl-s", wait=2.0)
    ok_save, w = wait_for(s0, "Saved.", 30)
    rig.sendkey("ctrl-q", wait=2.0)
    ok_q, w = wait_for(s0, "Editor closed.", 30)
    s1 = mark()
    rig.type_line("set -a sec.ecf", wait=2.0)
    ok_a, w2 = wait_for(s1, "ecf: 1 diterapkan, 0 tak dikenal, 0 error", 25)
    check("E17 .ecf berskema [net] lewat editor + set -a",
          ok_save and ok_q and ok_a, (w + w2)[-420:])

    # E18 `eqgu` tanpa argumen
    s0 = mark()
    rig.type_line("eqgu", wait=1.5)
    ok, w = wait_for(s0, "eqgu: butuh nama berkas .c", 20)
    check("E18 eqgu tanpa argumen", ok, w[-320:])

    # E19 `eqgu` file bukan .c
    s0 = mark()
    rig.type_line("eqgu foo.txt", wait=1.5)
    ok, w = wait_for(s0, "eqgu: hanya berkas .c", 20)
    check("E19 eqgu menolak berkas non-.c", ok, w[-320:])

    # E20 `eqgu ok.c` — kode valid, Ctrl+S -> Cek: OK
    s0 = mark()
    rig.type_line("eqgu ok.c", wait=3.0)
    type_str(rig, 'int main(){print("hi\\n");return 0;}')
    time.sleep(1.0)
    rig.sendkey("ctrl-s", wait=3.0)
    ok, w = wait_for(s0, "Cek: OK", 60)
    check("E20 eqgu + Ctrl+S -> Cek: OK", ok, w[-420:])

    # E21 Ctrl+Q keluar bersih
    s0 = mark()
    rig.sendkey("ctrl-q", wait=2.5)
    ok, w = wait_for(s0, "Editor closed.", 25)
    check("E21 Ctrl+Q menutup editor bersih", ok, w[-320:])

    # E22 `eqgu bad.c` — kode rusak, Ctrl+S -> Cek: GAGAL
    s0 = mark()
    rig.type_line("eqgu bad.c", wait=3.0)
    type_str(rig, "int main(){ /* unterminated")
    time.sleep(1.0)
    rig.sendkey("ctrl-s", wait=3.0)
    ok, w = wait_for(s0, "Cek: GAGAL", 60)
    err = "mtcc: error" in w
    check("E22 eqgu + Ctrl+S -> Cek: GAGAL + mtcc: error",
          ok and err, w[-460:])
    rig.sendkey("ctrl-q", wait=2.0)

    rig.quit()
    return True


# ------------------------------------------------------------ phase 2
def phase2():
    print("[ecf] PHASE 2 — citra EQDISK sendiri ...", flush=True)
    os.makedirs(os.path.dirname(DISK), exist_ok=True)
    r = subprocess.run([sys.executable, MKFS, "--size-mb", "64",
                        "--out", DISK],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("[ecf] gagal membangun citra EQDISK:\n" +
              (r.stdout + r.stderr)[-400:])
        return False

    class QemuDisk(Qemu):
        EXTRA = ["-boot", "order=d",
                 "-drive", f"file={DISK},format=raw,if=ide,index=0,media=disk"]

    fresh_serial()
    rig = QemuDisk(ISO)
    ok = wait_serial("user $", 180, rig)
    if not ok:
        check("E23 boot dengan disk -> shell + cd /mnt", False, tail(0))
        rig.quit()
        return False

    s0 = mark()
    rig.type_line("cd /mnt", wait=1.5)
    rig.type_line("pwd", wait=1.5)
    ok, w = wait_for(s0, "/mnt", 20)
    check("E23 boot dengan disk -> shell + cd /mnt",
          ok and "no such directory" not in w, w[-320:])

    rig.type_line("cdir boot", wait=2.0)
    s0 = mark()
    rig.type_line("set net.driver none", wait=2.0)
    ok, w = wait_for(s0, "ecf: net.driver = none -> /mnt/boot/system.ecf", 25)
    # baseline: net MASIH naik (net.driver hanya dibaca saat boot)
    s1 = mark()
    rig.type_line("ifconfig", wait=3.0)
    ok2, w2 = wait_for(s1, "HWaddr", 25)
    check("E24 set menulis /mnt/boot/system.ecf (net masih UP)",
          ok and ok2, (w + w2)[-420:])
    rig.quit()

    # boot ulang — disk yang sama, konfigurasi harus dihormati net_init.
    # (nic: ... tidak pernah masuk COM1 — serial_init() dipanggil SETELAH
    #  net_init() di kernel.cpp, jadi buktinya lewat ifconfig.)
    fresh_serial()
    rig = QemuDisk(ISO)
    ok = wait_serial("user $", 180, rig)
    s0 = mark()
    rig.type_line("ifconfig", wait=3.0)
    ok2, w = wait_for(s0, "net: down (no NIC)", 25)
    check("E25 boot ulang -> net.driver=none dihormati (ifconfig down)",
          ok and ok2, w[-420:])
    rig.quit()
    return True


# ---------------------------------------------------------------- main
def main():
    if not os.path.exists(ISO):
        print(f"[ecf] ISO missing: {ISO}")
        return 1

    t0 = time.time()
    if not phase1():
        print("[ecf] phase 1 gagal total")
    if not phase2():
        print("[ecf] phase 2 gagal total")

    print(f"\n[ecf] {len(PASS) + len(FAIL)} checks, "
          f"{len(PASS)} PASS / {len(FAIL)} FAIL "
          f"({time.time() - t0:.0f} s)")
    if FAIL:
        print("[ecf] GAGAL: " + ", ".join(FAIL))
        return 1
    print("[ecf] SEMUA LULUS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
