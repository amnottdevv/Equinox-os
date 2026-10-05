"""ab_test_v09.py — A/B: jalankan mtcc -make /equinox/eq.ruf di v0.9 murni."""
import time
from boot_test_v032 import Qemu, PASS, FAIL, serial, wait_serial, check

ISO_AB = "/tmp/ab-v09/dist/equinox.iso"

def main():
    rig = Qemu(ISO_AB)
    try:
        ok = wait_serial("user $", 150, rig)
        check("boot", ok)
        rig.type_line("mtcc -make /equinox/eq.ruf", wait=3.0)
        t = wait_serial("ok,", 120, rig)
        time.sleep(2.0)
        t = serial()
        fault = "faulted" in t or "Page Fault" in t
        check("79-job mtcc -make: crash=%s" % fault, not fault)
    finally:
        try:
            rig.proc.kill()
        except Exception:
            pass
    print(f"\n== AB: {len(PASS)} PASS / {len(FAIL)} FAIL ==")
    return 0 if not FAIL else 1

if __name__ == "__main__":
    raise SystemExit(main())
