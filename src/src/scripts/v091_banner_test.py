"""v091_banner_test.py — v0.9.3 identity + eqbash feature markers:
info mencetak "Fitur build 0.4 Beta", RAM 256 MB, meminfo menunjukkan
task "eqbash", dan builtin baru (ccfile/show/pren/delfile) berfungsi."""
import time
from boot_test_v032 import Qemu, ISO, PASS, FAIL, serial, wait_serial, check

def main():
    rig = Qemu(ISO)
    try:
        ok = wait_serial("user $", 150, rig)
        check("T1 boot ke prompt", ok)

        rig.type_line("info", wait=2.5)
        time.sleep(1.0)
        log = serial()
        check("T2 info: Fitur build 0.4 Beta", "Fitur build 0.4 Beta" in log)
        check("T3 info: RAM 256 MB", "RAM: 256 MB" in log)

        rig.type_line("meminfo", wait=2.5)
        time.sleep(1.0)
        log = serial()
        check("T4 meminfo: heap + task eqbash",
              "heap" in log.lower() and "eqbash" in log)

        rig.type_line("lf", wait=1.5)
        ok = wait_serial("equinox", 8, rig)
        check("T5 lf (ls alias) responsif", ok)

        rig.type_line('ccfile t092.txt << "halo v092"', wait=1.5)
        time.sleep(0.5)
        rig.type_line("show t092.txt", wait=1.5)
        time.sleep(1.0)
        log = serial()
        check("T6 ccfile << + show (cat)", "halo v092" in log)

        rig.type_line("pren /equinox/tools/mtcc.mrp", wait=2.0)
        time.sleep(1.0)
        log = serial()
        check("T7 pren: header MRP1 valid", "valid MRP1" in log)
        check("T8 pren: checksum COCOK", "(COCOK)" in log)

        rig.type_line("delfile t092.txt", wait=1.5)
        time.sleep(1.0)
        log = serial()
        check("T9 delfile menghapus", "dihapus" in log)

        rig.type_line("eggkg help", wait=2.5)
        ok = wait_serial("eggkg", 10, rig)
        check("T10 eggkg tersedia", ok)
    finally:
        try:
            rig.proc.kill()
        except Exception:
            pass
    print(f"\n== v092_banner: {len(PASS)} PASS / {len(FAIL)} FAIL ==")
    return 0 if not FAIL else 1

if __name__ == "__main__":
    raise SystemExit(main())
