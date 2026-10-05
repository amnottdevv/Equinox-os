#!/usr/bin/env python3
"""eggkg_debug2.py — eksperimen cepat (1 job mtcc): apakah file .ruf
yang dibuat RUNTIME (bukan staged GRUB) bisa dibaca mtcc via sys_open,
di path biasa (/user/egg) dan di path ber-segmen .local."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, ISO, serial, wait_serial

def main():
    rig = Qemu(ISO)
    ok = wait_serial("user $", 150, rig)
    print("boot:", ok, flush=True)
    if not ok:
        rig.quit(); return 1

    # siapkan dir + .c kecil
    for cmd in ["cd /user", "cdir egg", "cd /user/egg",
                'save t.c << "int main(){return 0;}"',
                'echo src /user/egg > zz.ruf',
                'echo out /user/egg >> zz.ruf']:
        rig.type_line(cmd, wait=0.8)

    # A — runtime .ruf di path biasa
    n = len(serial())
    rig.type_line("mtcc -make /user/egg/zz.ruf", wait=2.0)
    okA = wait_serial("selesai:", 300, rig, t0=time.time())
    tA = serial()[n:]
    print("A /user/egg/zz.ruf:", okA,
          [l for l in tA.splitlines() if "error" in l or "selesai" in l][:2], flush=True)

    # B — runtime .ruf di path .local (copy dari A)
    rig.type_line("copy /user/egg/zz.ruf /equinox/.local/bash/zz.ruf", wait=1.5)
    n = len(serial())
    rig.type_line("mtcc -make /equinox/.local/bash/zz.ruf", wait=2.0)
    okB = wait_serial("selesai:", 300, rig, t0=time.time())
    tB = serial()[n:]
    print("B /equinox/.local/bash/zz.ruf:", okB,
          [l for l in tB.splitlines() if "error" in l or "selesai" in l][:2], flush=True)

    # C — egg_write_file style: overwrite staged build.ruf lewat eggkg
    #     (cukup update+install; kita cuma mau lihat log [3/4]-nya —
    #     pakai interupsi cepat: cek log tulis resep saja, JANGAN tunggu build)
    rig.type_line("eggkg update /equinox/repo/package.list", wait=3.0)
    n = len(serial())
    rig.type_line("eggkg install bash -y", wait=2.0)
    okC = wait_serial("resep:", 200, rig, t0=time.time())
    print("C resep ditulis:", okC, flush=True)
    # beri mtcc 60 dtk untuk error pembuka; cukup untuk melihat 'cannot read'
    time.sleep(70)
    tC = serial()[n:]
    err = [l for l in tC.splitlines() if "error" in l or "resep" in l]
    print("C log:", err[:4], flush=True)
    rig.quit()
    return 0

if __name__ == "__main__":
    sys.exit(main())
