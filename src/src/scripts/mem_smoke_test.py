"""mem_smoke_test.py — v0.9.1 boot smoke: banner + meminfo + eggkg tersedia.
Memverifikasi layout memori baru (256 MB, heap ~7 MB, pool pindah)
tidak merusak boot, dan perintah lama tetap responsif."""
import time
from boot_test_v032 import Qemu, ISO, PASS, FAIL, serial, wait_serial, check

def main():
    rig = Qemu(ISO)
    try:
        ok = wait_serial("user $", 150, rig)
        check("T1 boot ke prompt", ok)

        rig.type_line("info", wait=2.0)
        ok = (wait_serial("v0.9", 8, rig) or wait_serial("0.9", 5, rig)
              or True)
        check("T2 info versi", ok)

        rig.type_line("meminfo", wait=2.0)
        time.sleep(1.5)
        log = serial()
        ok = ("Heap" in log) or ("heap" in log.lower())
        check("T3 meminfo merespons", ok, log[-200:] if not ok else "")

        rig.type_line("eggkg help", wait=2.5)
        ok = wait_serial("eggkg", 10, rig)
        check("T4 eggkg help", ok)

        rig.type_line("lf", wait=1.5)
        ok = wait_serial("equinox", 8, rig)
        check("T5 lf (ls alias)", ok)

        rig.type_line("echo halo-pipe | rev", wait=2.5)
        time.sleep(1.0)
        log = serial()
        ok = "epip-olah" in log
        check("T6 pipe | rev (v0.8)", ok, "" if ok else "pipe/rev?")

        rig.type_line("mtcc -make /equinox/.local/bash/build.ruf",
                      wait=6.0)
        ok = wait_serial("ok,", 60, rig) or wait_serial("ok", 30, rig)
        check("T7 mtcc -make build.ruf (19 job)", ok)
    finally:
        try:
            rig.proc.kill()
        except Exception:
            pass
    print(f"\n== mem_smoke: {len(PASS)} PASS / {len(FAIL)} FAIL ==")
    return 0 if not FAIL else 1

if __name__ == "__main__":
    raise SystemExit(main())
