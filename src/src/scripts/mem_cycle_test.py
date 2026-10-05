"""mem_cycle_test.py — v0.9.1: bukti tidak ada kebocoran memori pada siklus
eggkg install/remove. meminfo diambil sebelum & sesudah 3 siklus penuh."""
import time
from boot_test_v032 import Qemu, ISO, PASS, FAIL, serial, wait_serial, check

def pool_free(log):
    """Ambil 'pool: <total> KB total, <free> KB free' terakhir."""
    import re
    m = re.findall(r"pool:\s*(\d+) KB total,\s*(\d+) KB free", log)
    return (int(m[-1][0]), int(m[-1][1])) if m else (None, None)

def main():
    rig = Qemu(ISO)
    try:
        ok = wait_serial("user $", 150, rig)
        check("boot", ok)

        rig.type_line("meminfo", wait=2.0)
        time.sleep(1.5)
        free0, _ = pool_free(serial())
        check("meminfo awal", free0 is not None, f"free={free0} KB")

        rig.type_line("eggkg update /equinox/repo/package.list", wait=2.5)
        t = wait_serial("1 paket", 30, rig)
        time.sleep(1.0)
        check("update index", "1 paket" in serial())

        for i in range(3):
            rig.type_line("eggkg install bash -y", wait=3.0)
            t = wait_serial("0 gagal", 150, rig)
            time.sleep(2.0)
            t = serial()
            check(f"install #{i+1} 19 job 0 gagal", "0 gagal" in t)
            rig.type_line("eggkg remove bash -y", wait=2.5)
            t = wait_serial("dihapus", 60, rig)
            time.sleep(1.5)
            t = serial()
            check(f"remove #{i+1} bersih", "dihapus" in t)

        rig.type_line("meminfo", wait=2.0)
        time.sleep(1.5)
        free1, _ = pool_free(serial())
        check("meminfo akhir", free1 is not None, f"free={free1} KB")
        if free0 is not None and free1 is not None:
            loss = free0 - free1
            check(f"pool free turun <= 1 MB ({loss} KB)", loss <= 1024)
    finally:
        try:
            rig.proc.kill()
        except Exception:
            pass
    print(f"\n== mem_cycle: {len(PASS)} PASS / {len(FAIL)} FAIL ==")
    return 0 if not FAIL else 1

if __name__ == "__main__":
    raise SystemExit(main())
