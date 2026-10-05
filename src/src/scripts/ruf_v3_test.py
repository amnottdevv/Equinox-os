"""ruf_v3_test.py — v0.9.3: .ruf ala Makefile + morph pkg API + ANSI.
Menguji: variabel (:=, $nama case-insensitive), copy/move ? to (multi &),
eksekusi post-build berurutan, pkg_* helpers in-OS, ansi_fg/clear_screen.
File .ruf & pkgtest.c dikirim sebagai modul ISO (multi-baris)."""
import time
from boot_test_v032 import (Qemu, ISO, PASS, FAIL, serial, wait_serial,
                            check, since, read_ppm, count_nonblack)

SRC_DIR = "/equinox/.local/v3test/src"

def sh(rig, line, wait=2.0, settle=0.8):
    base = len(serial())
    rig.type_line(line, wait=wait)
    time.sleep(settle)
    return since(base, 8000)

def main():
    rig = Qemu(ISO)
    try:
        ok = wait_serial("user $", 150, rig)
        check("T1 boot ke prompt", ok)
        if not ok:
            return 1

        # ---- T2: sumber program (dua .c kecil; ccfile = nama saja,
        #      jadi direktori dibuat bertingkat lewat cd + cdir) ----
        sh(rig, "cd /equinox/.local")
        sh(rig, "cdir v3test")
        sh(rig, "cd v3test")
        sh(rig, "cdir src")
        sh(rig, "cd src")
        sh(rig, 'ccfile hello.c << int main() { print("halo-v3"); return 42; }')
        sh(rig, 'ccfile world.c << int main() { print("world-v3"); return 7; }')
        sh(rig, "cd /")

        # ---- T3: v3t1.ruf — variabel + copy post-build ----
        base = len(serial())
        rig.type_line("mtcc -make /equinox/v3t1.ruf", wait=2.0)
        ok = wait_serial("ake] selesai: 2 ok, 0 gagal (2 job)", 300, rig)
        t = since(base, 30000)
        check("T3 build v3t1: 2 job ok (variabel $ diekspansi)", ok,
              t.strip().replace("\n", " | ")[-300:])
        check("T4 copy post-build: out1 -> pkg1 ok",
              "ake] copy: /equinox/.local/v3test/out1 -> "
              "/equinox/.local/v3test/pkg1 ok" in t)

        # ---- T5: hasil copy ada di pkg1/out1 ----
        t = sh(rig, "lf /equinox/.local/v3test/pkg1/out1", wait=1.5, settle=1.0)
        check("T5 hasil copy: hello.mrp + world.mrp ada",
              "hello.mrp" in t and "world.mrp" in t,
              t.strip()[-200:])

        # ---- T6: v3t2.ruf — copy multi-& lalu move ----
        base = len(serial())
        rig.type_line("mtcc -make /equinox/v3t2.ruf", wait=2.0)
        ok = wait_serial("ake] selesai: 2 ok, 0 gagal (2 job)", 300, rig)
        t = since(base, 30000)
        check("T6 move + copy multi-& sukses (urutan antrean)",
              ok and "ake] copy:" in t and "ake] move:" in t
              and "GAGAL" not in t,
              t.strip().replace("\n", " | ")[-300:])

        # ---- T7: pkg API in-OS (morph.h splice 13 modul) ----
        base = len(serial())
        rig.type_line("mtcc /test/pkgtest.c", wait=2.0)
        ok = wait_serial("ok-goto", 180, rig)
        t = since(base, 40000)
        check("T7 pkg API: write/read/copy/move/list/manifest",
              ok and "halo-pkg" in t and "copy=1" in t and "move=1" in t
              and "list=2" in t and "manifest=2" in t,
              t.strip().replace("\n", " | ")[-400:])

        # ---- T8: ANSI teks tercetak ----
        check("T8 ANSI: MERAH-V093 + goto",
              "MERAH-V093" in t and "ok-goto" in t)

        # ---- T9: warna merah benar-benar tampil (screendump) ----
        time.sleep(1)
        shot = rig.dump("ansi_v093")
        try:
            w, h, rgb = read_ppm(shot)
            reds = 0
            for i in range(0, len(rgb), 3):
                r, g, b = rgb[i], rgb[i+1], rgb[i+2]
                if r > 150 and g < 110 and b < 110:
                    reds += 1
        except Exception:
            reds = 0
        check("T9 piksel merah ANSI terlihat", reds > 100, f"{reds} px")
    finally:
        try:
            rig.proc.kill()
        except Exception:
            pass
    print(f"\n== ruf_v3: {len(PASS)} PASS / {len(FAIL)} FAIL ==")
    return 0 if not FAIL else 1

if __name__ == "__main__":
    raise SystemExit(main())
