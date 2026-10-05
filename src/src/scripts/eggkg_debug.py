#!/usr/bin/env python3
"""eggkg_debug.py — isolasi bug 'mtcc cannot read build.ruf' hasil
tulis runtime eggkg di /equinox/.local/bash/ (path ber-segmen .local).

Eksperimen:
  E1  equinoxinstall -build /equinox/.local/bash/build.ruf (STAGED, boot)
      -> baseline Task 13, harus sukses
  E2  overwrite build.ruf via shell `save` (runtime write) di path yang
      SAMA -> equinoxinstall -build lagi -> isolasi overwrite-vs-staged
  E3  save zz.ruf (file BARU runtime) di dir .local/bash -> mtcc -make
  E4  eggkg update + install (flow asli yang gagal)
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, ISO, serial, wait_serial

SERIAL = "/tmp/boot_v032_serial.log"

def main():
    if os.path.exists(SERIAL): os.remove(SERIAL)
    rig = Qemu(ISO)
    ok = wait_serial("user $", 150, rig)
    print("boot:", ok)
    if not ok:
        rig.quit(); return 1

    # E1 — staged baseline
    n = len(serial())
    rig.type_line("equinoxinstall -build /equinox/.local/bash/build.ruf", wait=2.0)
    ok1 = wait_serial("[build] selesai", 900, rig, t0=time.time())
    t1 = serial()[n:]
    print("E1 staged baseline:", ok1,
          "| gagal:", [l for l in t1.splitlines() if "gagal" in l or "error" in l][:2])

    # E2 — overwrite via save (runtime write, path sama)
    rig.type_line("cd /equinox/.local/bash", wait=1.0)
    rig.type_line('save build.ruf << "echo overwrite-test\nsrc /equinox/.local/bash/src\nout /equinox/.local/bash\n"', wait=1.5)
    n = len(serial())
    rig.type_line("equinoxinstall -build /equinox/.local/bash/build.ruf", wait=2.0)
    ok2 = wait_serial("[build] selesai", 900, rig, t0=time.time())
    t2 = serial()[n:]
    print("E2 save-overwrite:", ok2,
          "| gagal:", [l for l in t2.splitlines() if "gagal" in l or "error" in l][:2])

    # E3 — file BARU runtime di dir .local
    rig.type_line('save zz.ruf << "echo zz-new-file\nsrc /equinox/.local/bash/src\nout /equinox/.local/bash\n"', wait=1.5)
    n = len(serial())
    rig.type_line("mtcc -make /equinox/.local/bash/zz.ruf", wait=2.0)
    ok3 = wait_serial("selesai:", 900, rig, t0=time.time())
    t3 = serial()[n:]
    print("E3 new runtime file:", ok3,
          "| err:", [l for l in t3.splitlines() if "error" in l][:2],
          "| sum:", [l for l in t3.splitlines() if "selesai" in l][:1])

    rig.quit()
    return 0

if __name__ == "__main__":
    sys.exit(main())
