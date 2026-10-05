"""pipe_glob_smoke.py — v0.9.1: verifikasi cepat fitur v0.8 (pipe,
redirect stdin, glob) di atas layout memori baru. Satu boot, waits ketat."""
import time
from boot_test_v032 import Qemu, ISO, PASS, FAIL, serial, wait_serial, check

def main():
    rig = Qemu(ISO)
    try:
        ok = wait_serial("user $", 150, rig)
        check("F0 boot ke shell", ok)

        rig.type_line("eggkg update /equinox/repo/package.list", wait=2.5)
        t = wait_serial("1 paket", 30, rig)
        time.sleep(1.0)
        t = serial()
        check("F5 eggkg update (repo offline)", "1 paket" in t)

        rig.type_line("eggkg install bash -y", wait=3.0)
        t = wait_serial("0 gagal", 150, rig)
        time.sleep(3.0)
        t = serial()
        check("F5b eggkg install bash -> 23 job 0 gagal", "0 gagal" in t)

        rig.type_line("cat /equinox/eq.ruf | grep src", wait=2.5)
        time.sleep(1.5)
        t = serial()
        check("F6 pipe cat|grep", "src /equinox" in t)

        rig.type_line("cat /equinox/eqmini.ruf | wc -c", wait=2.5)
        time.sleep(1.5)
        t = serial()
        check("F7 pipe cat|wc -c", "(stdin)" in t)

        rig.type_line("wc -c < /equinox/eq.ruf", wait=2.5)
        time.sleep(1.5)
        t = serial()
        check("F8 stdin redirect <", "(stdin)" in t)

        rig.type_line("Qfs | grep x", wait=2.5)
        time.sleep(1.5)
        t = serial()
        check("F9 Qfs | grep x", "bukan program .mrp" in t)

        rig.type_line("cd /equinox", wait=1.0)
        rig.type_line("echo *.ruf", wait=2.5)
        time.sleep(1.0)
        t = serial()
        check("F10 glob echo *.ruf", "eqmini.ruf" in t)
    finally:
        try:
            rig.proc.kill()
        except Exception:
            pass
    print(f"\n== pipe_glob_smoke: {len(PASS)} PASS / {len(FAIL)} FAIL ==")
    return 0 if not FAIL else 1

if __name__ == "__main__":
    raise SystemExit(main())
