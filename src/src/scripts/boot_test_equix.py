#!/usr/bin/env python3
"""boot_test_equix.py — EquiX DE v0.2 QEMU regression.

  T1   boot ke shell
  T2   `desktop` -> serial "desktop aktif" + "backbuffer" (double-buffer)
  T3   tema monokrom: wallpaper hitam->abu, taskbar hitam, titlebar
       aktif abu-terang / idle abu-gelap, body putih (R==G==B)
  T4   teks MENU hitam-di-putih & jam putih-di-hitam
  T5   klik MENU -> launcher popup (panel + teks)
  T6   CALCULATOR: klik 7 9 * 6 = sqrt -> serial "[equix:calc] = -> 474"
       dan "sqrt -> 21.77"; display + tombol terverifikasi pixel
  T6f  ANTI-FLICKER canary: 3 screendump SAAT mouse bergerak — tombol
       MENU & body jendela tidak pernah hitam (bug v0.1: rasterClear
       ke LFB terlihat sebagai layar hitam tiap render)
  T7   drag jendela About -> posisi titlebar berubah
  T8   klik "Exit to shell" -> serial "desktop selesai"
  T9   shell hidup kembali + QEMU alive
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, read_ppm, check, PASS, FAIL, serial, wait_serial
from boot_test_v032 import ISO

PNG1 = "/home/z/my-project/download/equinox_desktop_v02.png"
PNG2 = "/home/z/my-project/download/equinox_desktop_launcher_v02.png"
PNG3 = "/home/z/my-project/download/equinox_desktop_calc_v02.png"


def px(img, x, y):
    w, h, data = img
    i = (y * w + x) * 3
    return (data[i], data[i + 1], data[i + 2])


def mono(c, tol=8):
    """channel R/G/B hampir sama (tema monokrom)"""
    return max(c) - min(c) <= tol


def has_text_white(img, x0, x1, y0, y1, minpx=6):
    w, h, data = img
    cnt = 0
    for y in range(y0, y1):
        base = y * w
        for x in range(x0, x1):
            i = (base + x) * 3
            if data[i] > 200 and data[i + 1] > 200 and data[i + 2] > 200:
                cnt += 1
    return cnt >= minpx


def has_text_black(img, x0, x1, y0, y1, minpx=6):
    w, h, data = img
    cnt = 0
    for y in range(y0, y1):
        base = y * w
        for x in range(x0, x1):
            i = (base + x) * 3
            if data[i] < 60 and data[i + 1] < 60 and data[i + 2] < 60:
                cnt += 1
    return cnt >= minpx


def save_png(img, path):
    try:
        from PIL import Image
        Image.frombytes("RGB", (img[0], img[1]), img[2]).save(path)
        print(f"  PNG: {path}")
    except Exception as e:
        print(f"  (PNG skip: {e})")


def mouse_move(q, dx, dy, step=120):
    """gerak relatif via QEMU monitor (PS/2 delta)"""
    while dx != 0 or dy != 0:
        sx = max(-step, min(step, dx))
        sy = max(-step, min(step, dy))
        q.cmd(f"mouse_move {sx} {sy}", timeout=0.25)
        dx -= sx
        dy -= sy
        time.sleep(0.06)


def goto(q, x, y, cur=(680, 384)):
    mouse_move(q, x - cur[0], y - cur[1])
    return (x, y)


def click(q):
    q.cmd("mouse_button 1", timeout=0.3)
    time.sleep(0.35)
    q.cmd("mouse_button 0", timeout=0.3)
    time.sleep(0.5)


def click_at(q, x, y, cur):
    cur = goto(q, x, y, cur)
    click(q)
    return cur


def main():
    print("== boot ==")
    q = Qemu(ISO)
    cur = (680, 384)
    try:
        check("T1 boot ke shell", wait_serial("root::users", 40, rig=q))

        print("== start desktop ==")
        q.type_line("desktop", wait=3.0)
        ok = wait_serial("desktop aktif", 20, rig=q)
        check("T2a desktop aktif", ok)
        check("T2b backbuffer/double-buffer",
              "backbuffer" in serial() and "double-buffer" in serial())

        time.sleep(2.0)
        path = q.dump("equix1")
        img = q and read_ppm(path)
        w, h, _ = img
        print(f"  screenshot {w}x{h}")

        # T3: tema monokrom (semua cek: R==G==B)
        c_tl = px(img, 60, 60)
        check("T3a wallpaper kiri-atas hitam", sum(c_tl) < 130 and mono(c_tl), str(c_tl))
        c_br = px(img, 1300, 700)
        check("T3b wallpaper kanan-bawah abu", 200 < sum(c_br) < 480 and mono(c_br), str(c_br))
        c_bar = px(img, w // 2, h - 22)
        check("T3c taskbar hitam", sum(c_bar) < 110 and mono(c_bar), str(c_bar))
        # About (90,64,420x250) tak-fokus: titlebar abu gelap
        c_tb = px(img, 300, 80)
        check("T3d titlebar idle abu gelap", 150 < sum(c_tb) < 330 and mono(c_tb), str(c_tb))
        # Calculator (980,110,292x400) fokus: titlebar abu terang
        c_ct = px(img, 1120, 126)
        check("T3e titlebar aktif abu terang", 400 < sum(c_ct) < 660 and mono(c_ct), str(c_ct))
        c_body = px(img, 200, 200)
        check("T3f body About putih", sum(c_body) > 620 and mono(c_body), str(c_body))

        # T4: teks MENU hitam di tombol putih & jam putih di panel gelap
        check("T4a teks MENU hitam", has_text_black(img, 14, 100, h - 37, h - 5))
        check("T4b jam RTC putih", has_text_white(img, w - 126, w - 12, h - 37, h - 5))

        # kalkulator awal: display gelap + tombol angka abu gelap + '=' terang
        c_disp = px(img, 1024, 190)
        check("T4c calc display gelap", sum(c_disp) < 110 and mono(c_disp), str(c_disp))
        c_dig = px(img, 1024, 304)     # tombol '7' (row1 col0)
        check("T4d tombol angka abu gelap", 90 < sum(c_dig) < 210 and mono(c_dig), str(c_dig))
        c_eq = px(img, 1228, 472)      # tombol '=' (row4 col3)
        check("T4e tombol = terang", sum(c_eq) > 620 and mono(c_eq), str(c_eq))
        check("T4f teks '0' putih di display",
              has_text_white(img, 1230, 1256, 174, 198, minpx=8))

        save_png(img, PNG1)

        print("== buka launcher ==")
        cur = click_at(q, 56, h - 29, cur)
        time.sleep(1.2)
        path2 = q.dump("equix2")
        img2 = read_ppm(path2)
        c_lc = px(img2, 130, h - 200)   # panel launcher monokrom gelap
        check("T5a launcher popup muncul",
              12 <= c_lc[0] <= 70 and mono(c_lc), str(c_lc))
        check("T5b teks launcher ada", has_text_white(img2, 28, 240, h - 215, h - 60))
        save_png(img2, PNG2)

        # tutup launcher: klik area kosong wallpaper
        cur = click_at(q, 500, 384, cur)
        time.sleep(0.6)

        print("== calculator: 7 9 * 6 = sqrt ==")
        # grid: gx0=994 gy0=224 bw=60 bh=48 gap=8
        # row2 = "4","5","6","-" -> '6' ada di col2 (x 1130..1190)
        cur = click_at(q, 1024, 304, cur)   # 7
        cur = click_at(q, 1160, 304, cur)   # 9
        cur = click_at(q, 1228, 304, cur)   # *
        cur = click_at(q, 1160, 360, cur)   # 6 (row2 col2!)
        cur = click_at(q, 1228, 472, cur)   # =
        check("T6a kalkulator hitung 79*6=474",
              wait_serial("[equix:calc] = -> 474", 8, rig=q))
        cur = click_at(q, 1024, 472, cur)   # sqrt
        check("T6b kalkulator sqrt(474)",
              wait_serial("[equix:calc] sqrt -> 21.77", 8, rig=q))
        time.sleep(0.8)
        path3 = q.dump("equix3")
        img3 = read_ppm(path3)
        # display kini berisi angka hasil (teks putih lebar)
        check("T6c display menampilkan hasil",
              has_text_white(img3, 1150, 1256, 174, 198, minpx=12))
        save_png(img3, PNG3)

        print("== anti-flicker canary (dump saat mouse bergerak) ==")
        # bug v0.1: saat render, LFB di-rasterClear -> area terang jadi
        # hitam. v0.2 double-buffer: LFB hanya frame utuh.
        # CATATAN: titik cek HARUS bebas glyph teks (mis. (56,746)
        # jatuh di huruf 'N' 'MENU' -> selalu gelap meski render benar)
        ok_flicker = True
        details = []
        for i in range(3):
            q.cmd(f"mouse_move {-200} 0", timeout=0.25)   # sweep kiri, in-bounds
            p = q.dump(f"equixf{i}")
            imf = read_ppm(p)
            c_menu1 = px(imf, 24, 746)      # tombol MENU bg putih (bebas teks)
            c_menu2 = px(imf, 90, 746)      # bg putih sisi kanan (bebas teks)
            c_body = px(imf, 460, 250)      # body About (bebas teks konten)
            if sum(c_menu1) < 500 or sum(c_menu2) < 500:
                ok_flicker = False
                details.append(f"menu={c_menu1}/{c_menu2}")
            if sum(c_body) < 500:
                ok_flicker = False
                details.append(f"body={c_body}")
            time.sleep(0.2)
        cur = (cur[0] - 600, cur[1])
        check("T6f anti-flicker: tak ada frame hitam saat gerak",
              ok_flicker, "; ".join(details))

        print("== drag jendela About ==")
        cur = goto(q, 240, 80, cur)
        time.sleep(0.3)
        q.cmd("mouse_button 1", timeout=0.3)
        time.sleep(0.35)
        mouse_move(q, 250, 120, step=60)
        time.sleep(0.35)
        q.cmd("mouse_button 0", timeout=0.3)
        time.sleep(1.0)
        cur = (490, 200)   # posisi mouse aktual setelah drag
        path4 = q.dump("equix4")
        img4 = read_ppm(path4)
        # About difokuskan saat drag -> titlebar abu terang di posisi baru.
        # (460,200): titlebar bebas glyph (teks 'About EquiX' berakhir x=440,
        # tombol mulai x=711); (240,80): posisi lama -> wallpaper gelap
        c_new = px(img4, 460, 200)
        moved = (480 < sum(c_new) < 700 and mono(c_new))
        c_old = px(img4, 240, 80)
        cleared = sum(c_old) < 150
        check("T7 drag jendela bekerja", moved and cleared,
              f"new={c_new} old={c_old}")

        print("== exit via launcher ==")
        cur = click_at(q, 56, h - 29, cur)
        time.sleep(1.0)
        # item Exit = index 3 -> teks di y = 566+3*36+8+8 ; panel 674..706
        exit_y = 566 + 3 * 36 + 16
        cur = click_at(q, 130, exit_y, cur)
        time.sleep(1.5)
        ok = wait_serial("desktop selesai", 10, rig=q)
        check("T8 exit ke shell", ok)

        time.sleep(1.0)
        check("T9 QEMU hidup", q.alive())
    finally:
        q.quit()

    print()
    print(f"PASS {len(PASS)} / FAIL {len(FAIL)}")
    if FAIL:
        print("FAILED:", FAIL)
        sys.exit(1)


if __name__ == "__main__":
    main()
