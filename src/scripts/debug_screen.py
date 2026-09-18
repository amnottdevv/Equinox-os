#!/usr/bin/env python3
"""debug_screen.py — boot QEMU, jalankan perintah, screendump, OCR semua console.
Pemakaian: python3 debug_screen.py "doom -warp 1 -fps" [wait_secs] [out.png]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from fps_measure import Mon, load_font, ocr_left_column, read_ppm


def ocr_full(ppm_path, font, max_cols=120):
    """OCR seluruh layar (bukan cuma kolom kiri)."""
    w, h, rgb = read_ppm(ppm_path)
    cols = min(max_cols, w // 8)
    out = []
    for row in range(h // 16):
        line = ""
        for col in range(cols):
            bm = []
            for gy in range(16):
                b = 0
                for gx in range(8):
                    x = col * 8 + gx
                    y = row * 16 + gy
                    off = (y * w + x) * 3
                    lum = (rgb[off] * 299 + rgb[off + 1] * 587
                           + rgb[off + 2] * 114) // 1000
                    if lum > 96:
                        b |= 1 << (7 - gx)
                bm.append(b)
            best, best_d = "?", 999
            for code in range(32, 127):
                d = sum(bin(bm[i] ^ font[code][i]).count("1") for i in range(16))
                if d < best_d:
                    best_d, best = d, chr(code)
            line += best if best_d <= 24 else " "
        out.append(line.rstrip())
    return out


def main():
    cmdline = sys.argv[1] if len(sys.argv) > 1 else "doom -warp 1 -fps"
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
    out_png = sys.argv[3] if len(sys.argv) > 3 else "/tmp/debug_screen.ppm"

    font = load_font()
    mon = Mon()
    try:
        print("[dbg] boot 50s...")
        time.sleep(50)
        mon.sendkey("ret")
        time.sleep(1.0)
        for k in cmdline:
            mon.sendkey(k if k != " " else "spc", None)
        mon.sendkey("ret")
        print(f"[dbg] '{cmdline}'; tunggu {wait}s")
        time.sleep(wait)
        # tekan W sebentar untuk melihat respons
        mon.sendkey("w", 1500)
        time.sleep(1.0)
        if mon.screendump(out_png):
            lines = ocr_full(out_png, font)
            shown = 0
            for i, ln in enumerate(lines):
                if ln.strip():
                    print(f"  {i:3d}| {ln}")
                    shown += 1
            if shown == 0:
                print("[dbg] layar console KOSONG (semua hitam?)")
    finally:
        mon.quit()


if __name__ == "__main__":
    main()
