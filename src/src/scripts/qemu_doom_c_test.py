#!/usr/bin/env python3
"""qemu_doom_c_test.py — Fase C (v10.10): input modern + performa.

Test:
  T1  boot + doom -warp 1 -> E1M1 render
  T2  W hold 2s -> kamera maju (frame diff besar)
  T3  A hold 1.5s -> strafe kiri (frame diff besar)
  T4  mouse_move dx besar -> putaran view (frame diff besar)
  T5  LMB tahan -> fire (muzzle flash, frame diff besar)
  T6  RMB -> next weapon (senjata berubah, frame diff)
  T7  ` (backquote) -> screenshot /shot001.ppm (peek shot_number==1
      + OCR pesan console)
  T8  OCR "[fps] N" >= 25 -> sanity performa (v10.10 target)

Semua pengukuran visual via screendump + frame_diff (infrastruktur
qemu_doom_test.py). Peek memori fisik via monitor `xp` untuk state
internal (shot_number).
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_doom_test import Qemu, frame_diff, ppm_stats, read_ppm
from fps_measure import load_font, ocr_left_column, sendkey_text

ROOT = os.path.expanduser("~/morphos")
ISO = os.path.join(ROOT, "dist", "equinox.iso")

# alamat simbol build final (doom.mrp -O3 v10.10; vaddr == phys)
SHOT_NUMBER = 0x5AD644
SHOT_PENDING = 0x5AD648
FPS_TOTAL = 0x5AD64C      # akumulasi frame — tidak pernah reset

RESULTS = []


def report(name, ok, info=""):
    RESULTS.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}  {info}")


def wait_boot(q, secs=50):
    time.sleep(secs)
    q.type_str("")  # flush
    q.cmd("sendkey ret")
    time.sleep(0.8)


def peek(q, addr):
    out = q.cmd(f"xp /1dw 0x{addr:x}", timeout=10)
    m = re.search(r"^\s*[0-9a-f]+:\s+(-?\d+)", out, re.M)
    return int(m.group(1)) if m else None


def ocr_fps(q, font):
    p = q.dump("fps")
    lines = ocr_left_column(p, font)
    vals = []
    for ln in lines:
        m = re.search(r"\[fps\]\s*(\d+)", ln)
        if m:
            vals.append(int(m.group(1)))
    return (vals[-1] if vals else None), lines


def main():
    font = load_font()
    print(f"ISO: {ISO}")
    q = Qemu(ISO)
    try:
        # ---------- T1: boot + E1M1 ----------
        print("[..] boot 50s + doom -warp 1 -fps ...")
        wait_boot(q)
        sendkey_text_generic(q, "doom -warp 1 -fps")
        q.cmd("sendkey ret")
        time.sleep(22)
        p0 = q.dump("e1m1")
        st = ppm_stats(p0)
        ok = st["colors"] > 100 and st["center_ink"] > 0.30
        report("T1 E1M1 render", ok,
               f"(colors={st['colors']} center_ink={st['center_ink']*100:.0f}%)")

        # baseline frame utk diff
        fa = q.dump("base1")
        time.sleep(0.5)

        # ---------- T2: W maju ----------
        q.cmd("sendkey w 2000")
        time.sleep(2.2)
        fb = q.dump("wmove")
        d_move = frame_diff(fa, fb)
        report("T2 W = maju (movement)", d_move > 0.02,
               f"(diff={d_move*100:.1f}%)")

        # ---------- T3: A strafe ----------
        fa = q.dump("base2")
        q.cmd("sendkey a 1500")
        time.sleep(1.7)
        fc = q.dump("amove")
        d_strafe = frame_diff(fa, fc)
        report("T3 A = strafe kiri", d_strafe > 0.02,
               f"(diff={d_strafe*100:.1f}%)")

        # ---------- T4: mouse turn ----------
        fa = q.dump("base3")
        # gerakan mouse besar: beberapa step ke kanan
        for _ in range(6):
            q.cmd("mouse_move 300 0")
            time.sleep(0.05)
        time.sleep(0.8)
        fd = q.dump("mturn")
        d_turn = frame_diff(fa, fd)
        report("T4 mouse dx = putar view", d_turn > 0.02,
               f"(diff={d_turn*100:.1f}%)")

        # ---------- T5: RMB weapon next (SEBELUM fire: kondisi bersih,
        #     terbukti pola visual besar; fire dulu bisa memancing zombie
        #     menyerang / player mati -> weapon switch diabaikan) ----------
        fa = q.dump("base5")
        q.cmd("mouse_button 4")
        time.sleep(0.5)
        q.cmd("mouse_button 0")
        time.sleep(0.8)
        ff = q.dump("rmb")
        d_weap = frame_diff(fa, ff)
        report("T5 klik kanan = ganti senjata", d_weap > 0.005,
               f"(diff={d_weap*100:.1f}%)")

        # ---------- T6: LMB fire ----------
        fa = q.dump("base4")
        q.cmd("mouse_button 1")
        time.sleep(0.7)
        fe = q.dump("fire")
        q.cmd("mouse_button 0")
        d_fire = frame_diff(fa, fe)
        report("T6 klik kiri = tembak", d_fire > 0.005,
               f"(diff={d_fire*100:.1f}%)")

        # ---------- T7: ` screenshot ----------
        shot0 = peek(q, SHOT_NUMBER)
        q.cmd("sendkey grave_accent")
        time.sleep(2.5)     # PPM write via libc buffer + MKFILE
        shot1 = peek(q, SHOT_NUMBER)
        msgs = []
        p = q.dump("shotmsg")
        for ln in ocr_left_column(p, font):
            if "shot" in ln.lower():
                msgs.append(ln.strip())
        ok = shot0 is not None and shot1 == shot0 + 1
        report("T7 ` = screenshot /shotNNN.ppm", ok,
               f"(shot_number {shot0} -> {shot1}; msg={msgs[-1] if msgs else '-'})")

        # ---------- T8: FPS sanity (peek fps_total = presisi; OCR bonus) ----
        time.sleep(1.5)
        a = peek(q, FPS_TOTAL)
        time.sleep(2.5)
        b = peek(q, FPS_TOTAL)
        rate = (b - a) / 2.5 if (a is not None and b is not None) else None
        fps_ocr, lines = ocr_fps(q, font)
        ok = rate is not None and rate >= 25
        report("T8 performa (fps >= 25)", ok,
               f"(peek rate={rate and f'{rate:.0f}' or '?'}; ocr={fps_ocr})")

    finally:
        q.kill()

    print()
    npass = sum(1 for _, ok in RESULTS if ok)
    print(f"===== v10.10 Fase C (input modern + perf): "
          f"{npass}/{len(RESULTS)} PASS =====")
    for name, ok in RESULTS:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    sys.exit(0 if npass == len(RESULTS) else 1)


def sendkey_text_generic(q, text):
    """Ketik string ke console shell Equinox OS (nama key QEMU benar)."""
    KEYMAP = {
        " ": "spc", "-": "minus", ".": "dot", "/": "slash",
    }
    for k in text:
        if k in KEYMAP:
            q.cmd(f"sendkey {KEYMAP[k]}")
        else:
            q.cmd(f"sendkey {k}")
        time.sleep(0.03)


if __name__ == "__main__":
    main()
