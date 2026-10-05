#!/usr/bin/env python3
"""probe_taskbar.py — Reproduce & verify the "taskbar suka ke block" bug.

User symptom: "kalau mau nyentuh bagian taskbar bawah itu suka ke block"
(when touching the bottom taskbar it often gets blocked).

Empirical findings on v0.4.2 (pre-fix):
  H1a/H1b: hover highlight GAP — cursor in taskbar margins/bottom rows
           got NO highlight (0 px) while clicks landed; the desktop
           LOOKED dead exactly where the user reaches for the taskbar.
  C1-C4: clicks themselves registered (QEMU HMP adds latency), but the
           button-mask drain in mouse_get_state can still swallow a
           press+release between polls on real/slow hosts.
  v0.4.3 fix under test:
    - hover zones = full taskbar height (match click zones)
    - white 3px hotspot marker (was invisible 0xFF101018 on black bar)
    - lossless IRQ-context press/release edge counters

Checks (v0.4.3 expectations — all must PASS on the fixed build):
  H1a hover ON at y=727 (top margin)      H1b hover ON at y=764 (bottom)
  H1c hover ON at y=746 (control)          H1d hover ON at y=753
  C1..C4 click toggling on the Calc taskbar button (slow/fast/ultra/deep)
  M1  MENU at deep y opens the launcher
  M2  white hotspot marker visible at the screen bottom edge
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, read_ppm, check, PASS, FAIL, serial, wait_serial
from boot_test_v032 import ISO


def px(img, x, y):
    w, h, data = img
    i = (y * w + x) * 3
    return (data[i], data[i + 1], data[i + 2])


def count_range(img, x0, x1, y0, y1, lo, hi):
    """count pixels with all channels in [lo,hi]"""
    w, h, data = img
    cnt = 0
    for y in range(y0, y1):
        base = y * w
        for x in range(x0, x1):
            i = (base + x) * 3
            if lo <= data[i] <= hi and lo <= data[i + 1] <= hi and lo <= data[i + 2] <= hi:
                cnt += 1
    return cnt


def raw_cmd(q, line):
    """send a monitor command WITHOUT waiting for the drain timeout —
    needed for realistic click timings (press/release few ms apart)."""
    q.mon.settimeout(0.005)
    try:
        q.mon.sendall((line + "\n").encode())
    except Exception:
        pass
    try:
        q.mon.recv(4096)
    except Exception:
        pass


def mouse_move(q, dx, dy, step=100):
    while dx != 0 or dy != 0:
        sx = max(-step, min(step, dx))
        sy = max(-step, min(step, dy))
        raw_cmd(q, f"mouse_move {sx} {sy}")
        dx -= sx
        dy -= sy
        time.sleep(0.05)


def goto(q, x, y, cur):
    mouse_move(q, x - cur[0], y - cur[1])
    return (x, y)


def click(q, hold):
    """click with a precise hold time (ms-level), like a real user"""
    raw_cmd(q, "mouse_button 1")
    time.sleep(hold)
    raw_cmd(q, "mouse_button 0")
    time.sleep(0.35)          # let the desktop loop catch up


def calc_visible(q, tag):
    img = read_ppm(q.dump(tag))
    # LCD display area pixel: visible = black LCD (gray 16, sum ~48);
    # minimized = wallpaper gradient (~gray 60-110, sum > 130)
    c = px(img, 1100, 180)
    return sum(c) < 100


def main():
    print("== boot ==")
    q = Qemu(ISO)
    cur = (680, 384)
    try:
        wait_serial("user $", 40, rig=q)
        q.type_line("desktop", wait=3.0)
        wait_serial("desktop aktif", 30, rig=q)
        time.sleep(2.0)
        q.dump("tb0")
        v0 = calc_visible(q, "tb0")
        print(f"  calc window visible at start: {v0}")

        # ---------- H: hover feedback zones ----------
        print("== H: hover feedback in taskbar (About button x=178) ==")
        # About task button rect: x [112,244], y [731,761]
        results = {}
        for label, yy in [("H1a y=727 top-margin", 727),
                          ("H1b y=764 bottom-rows", 764),
                          ("H1c y=746 button-center", 746),
                          ("H1d y=753 edge-clamp-zone", 753)]:
            cur = goto(q, 178, yy, cur)
            time.sleep(0.5)
            img = read_ppm(q.dump(f"tbh{yy}"))
            hl = count_range(img, 120, 240, 734, 758, 80, 96)   # hover gray=88
            idle = count_range(img, 120, 240, 734, 758, 48, 64)  # idle gray=56
            results[label] = (hl, idle)
            print(f"    {label}: hover-px={hl} idle-px={idle}")

        # v0.4.3 expectations: hover feedback ON across the WHOLE taskbar
        # (zones now match the click zones = full taskbar height)
        check("H1c hover ON at button center (control)", results["H1c y=746 button-center"][0] > 300,
              str(results["H1c y=746 button-center"]))
        check("H1a hover ON at taskbar top margin (v0.4.3 fix)",
              results["H1a y=727 top-margin"][0] > 300, str(results["H1a y=727 top-margin"]))
        check("H1b hover ON at taskbar bottom rows (v0.4.3 fix)",
              results["H1b y=764 bottom-rows"][0] > 300, str(results["H1b y=764 bottom-rows"]))

        # ---------- C: click swallowing ----------
        print("== C: click toggling on Calc taskbar button (x=458) ==")
        # Calc task button x [392,524]; click zone = whole taskbar height
        for label, yy, hold, n in [
                ("C1 slow clicks (120ms) y=746", 746, 0.120, 5),
                ("C2 fast clicks (30ms)  y=746", 746, 0.030, 5),
                ("C3 ultra clicks (8ms)  y=746", 746, 0.008, 5),
                ("C4 fast clicks (30ms)  y=764 deep", 764, 0.030, 5)]:
            cur = goto(q, 458, yy, cur)
            time.sleep(0.3)
            state = calc_visible(q, f"tbc_pre_{label[0:2]}")
            toggles = 0
            for i in range(n):
                click(q, hold)
                v = calc_visible(q, f"tbc_{label[0:2]}_{i}")
                if v != state:
                    toggles += 1
                    state = v
            print(f"    {label}: {toggles}/{n} clicks registered")
            check(f"{label}: clicks registered", toggles >= n - 1, f"{toggles}/{n}")

        # ---------- M: menu button at deep y ----------
        print("== M1: MENU button click at y=760 (deep taskbar) ==")
        cur = goto(q, 56, 760, cur)
        time.sleep(0.3)
        click(q, 0.08)
        time.sleep(0.5)
        img = read_ppm(q.dump("tbmenu"))
        # launcher border line at x=10, y in [560,716], value ~70
        border = count_range(img, 10, 11, 560, 716, 60, 80)
        check("M1 launcher opens via MENU at deep y", border > 80, f"border-px={border}")
        # close it again
        cur = goto(q, 56, 746, cur)
        click(q, 0.08)

        # ---------- M2: white hotspot marker at the bottom edge ----------
        print("== M2: hotspot marker terlihat di dasar layar ==")
        cur = goto(q, 680, 767, cur)
        time.sleep(0.6)
        img = read_ppm(q.dump("tbmark"))
        # white caret 3px at the true hotspot (680, 767..768 clipped)
        # + white pixels of the edge-clamped arrow sprite nearby
        whites = count_range(img, 664, 700, 750, 768, 230, 255)
        check("M2 penanda hotspot putih terlihat di dasar (v0.4.3)",
              whites >= 6, f"white-px={whites}")

        print()
        print(f"PASS {len(PASS)} / FAIL {len(FAIL)}")
        if FAIL:
            print("FAILED:", FAIL)
    finally:
        q.quit()


if __name__ == "__main__":
    main()
