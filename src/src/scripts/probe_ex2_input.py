#!/usr/bin/env python3
"""probe_ex2_input.py — is the mtcc command line reaching the guest?

Types two commands after boot and dumps the serial log, so a dropped
keystroke sequence can be isolated (ex2_test.py saw NO echo at all for
`mtcc /equinox/tools/ex2.c`).
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from boot_test_v032 import Qemu, serial, wait_serial, ISO, SERIAL


def main():
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    rig = Qemu(ISO)
    try:
        if not wait_serial("user $", 180, rig):
            print(serial()[-2000:])
            return 1

        print("[probe] type: echo probe-ok", flush=True)
        rig.type_line("echo probe-ok", wait=2)
        time.sleep(2)

        print("[probe] type: mtcc /equinox/tools/ex2.c", flush=True)
        rig.type_line("mtcc /equinox/tools/ex2.c", wait=2)
        ok = wait_serial("ex2 RESULT", 180, rig)
        print("[probe] ex2 ran:", ok, flush=True)
        print("----- serial tail -----")
        print(serial()[-3000:])
    finally:
        rig.quit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
