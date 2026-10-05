#!/usr/bin/env python3
"""boot_test_equix2.py — EquiX DE v0.3 (layer-cache + dirty-rect) regression.

Verifikasi tiga fix utama v0.4.2:

  T2   RAM DINAMIS   : boot log "RAM detected: 63 MB" + "GUI arena 11 MB"
                       (dulu: arena GUI dipatok 10 MB — bug user)
  T5   MOUSE KE BAWAH: kursor mencapai y=767 (dasar layar/taskbar) —
                       serial "[equix] cursor mencapai dasar layar (y=767)"
                       (dulu: "kaya ke block gitu")
  T7   ANTI-LAG      : stats exit — rata2 px/frame jauh di bawah
                       full-screen (1.044.480 px), fps layak; respons
                       kursor terlihat < 0.5 s setelah gerakan.

  T1   boot ke shell
  T3   desktop aktif + layer-cache + first-frame line
  T4   tema monokrom + taskbar + calculator (pixel)
  T6   kalkulator mouse: 7 9 * 6 = sqrt -> 474, 21.77
  T6b  kalkulator KEYBOARD: 12*4= -> 48
  T7c  anti-flicker canary (dump saat mouse bergerak)
  T8   drag jendela About
  T9   exit via MENU -> "desktop selesai" + stats
  T10  shell + QEMU hidup
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import Qemu, read_ppm, check, PASS, FAIL, serial, wait_serial
from boot_test_v032 import ISO

DL = "/home/z/my-project/download"
PNG1 = DL + "/equinox_desktop_v03_main.png"
PNG2 = DL + "/equinox_desktop_v03_calc.png"


def px(img, x, y):
    w, h, data = img
    i = (y * w + x) * 3
    return (data[i], data[i + 1], data[i + 2])


def mono(c, tol=8):
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


def count_white(img, x0, x1, y0, y1):
    w, h, data = img
    cnt = 0
    for y in range(y0, y1):
        base = y * w
        for x in range(x0, x1):
            i = (base + x) * 3
            if data[i] > 215 and data[i + 1] > 215 and data[i + 2] > 215:
                cnt += 1
    return cnt


def save_png(img, path):
    try:
        from PIL import Image
        Image.frombytes("RGB", (img[0], img[1]), img[2]).save(path)
        print(f"  PNG: {path}")
    except Exception as e:
        print(f"  (PNG skip: {e})")


def mouse_move(q, dx, dy, step=120):
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
    time.sleep(0.3)
    q.cmd("mouse_button 0", timeout=0.3)
    time.sleep(0.45)


def click_at(q, x, y, cur):
    cur = goto(q, x, y, cur)
    click(q)
    return cur


KEY_EXTRA = {"*": "shift-8"}


def keyname(ch):
    if ch in KEY_EXTRA:
        return KEY_EXTRA[ch]
    if "A" <= ch <= "Z":
        return "shift-" + ch.lower()
    KM = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "=": "equal"}
    return KM.get(ch, ch)


def main():
    print("== boot ==")
    q = Qemu(ISO)
    cur = (680, 384)
    try:
        check("T1 boot ke shell", wait_serial("user $", 40, rig=q))

        print("== start desktop ==")
        q.type_line("desktop", wait=3.0)
        ok = wait_serial("desktop aktif", 30, rig=q)
        check("T3a desktop aktif", ok)
        check("T3b pipeline layer-cache", "layer-cache" in serial())
        check("T3c first-frame timing", "first frame:" in serial())

        # T2 dicek SETELAH desktop aktif: mirror serial console baru
        # menyala di akhir kernel_main (serial_init) — baris boot_log
        # tahap awal tidak lewat COM1. Log arena dinamis desktop +
        # ga_stats (DINAMIS) adalah bukti RAM dipakai menyeluruh.
        s = serial()
        m_arena = re.search(r"arena dinamis 0x3400000-0x([0-9A-Fa-f]+) \((\d+) MB\)", s)
        check("T2a RAM dideteksi dinamis", bool(m_arena),
              m_arena.group(0) if m_arena else "-")
        arena_mb = int(m_arena.group(2)) if m_arena else 0
        arena_end = int(m_arena.group(1), 16) if m_arena else 0
        check("T2b arena GUI dinamis >= 10 MB (bukan cap 10)",
              arena_mb >= 10 and "DINAMIS" in s and arena_end > 0x3E00000,
              f"arena={arena_mb} MB, end=0x{arena_end:x}")

        time.sleep(2.0)
        path = q.dump("equix3a")
        img = q and read_ppm(path)
        w, h, _ = img
        print(f"  screenshot {w}x{h}")

        c_tl = px(img, 60, 60)
        check("T4a wallpaper kiri-atas hitam", sum(c_tl) < 130 and mono(c_tl), str(c_tl))
        c_br = px(img, 1300, 700)
        check("T4b wallpaper kanan-bawah abu", 200 < sum(c_br) < 480 and mono(c_br), str(c_br))
        c_bar = px(img, w // 2, h - 22)
        check("T4c taskbar hitam", sum(c_bar) < 110 and mono(c_bar), str(c_bar))
        c_tb = px(img, 300, 80)
        check("T4d titlebar idle abu gelap", 150 < sum(c_tb) < 330 and mono(c_tb), str(c_tb))
        c_ct = px(img, 1120, 126)
        check("T4e titlebar aktif abu terang", 400 < sum(c_ct) < 660 and mono(c_ct), str(c_ct))
        c_body = px(img, 200, 200)
        check("T4f body About putih", sum(c_body) > 620 and mono(c_body), str(c_body))
        c_disp = px(img, 1024, 190)
        check("T4g calc display gelap", sum(c_disp) < 110 and mono(c_disp), str(c_disp))
        c_dig = px(img, 1024, 304)
        check("T4h tombol angka abu gelap", 90 < sum(c_dig) < 210 and mono(c_dig), str(c_dig))
        c_eq = px(img, 1228, 472)
        check("T4i tombol = terang", sum(c_eq) > 620 and mono(c_eq), str(c_eq))
        check("T4j teks '0' putih di display",
              has_text_white(img, 1150, 1256, 174, 198, minpx=8))
        check("T4k teks MENU hitam", has_text_black(img, 14, 100, h - 37, h - 5))
        save_png(img, PNG1)

        print("== T5: mouse ke DASAR layar (bug 'keblock') ==")
        mouse_move(q, 0, 400)          # dorong ke bawah — clamp 767
        mouse_move(q, 0, 120)
        check("T5a kursor mencapai y=767",
              wait_serial("[equix] cursor mencapai dasar layar (y=767)", 8, rig=q))
        cur = (680, 767)
        time.sleep(0.4)
        # v0.4.2 edge-clamp: sprite HARUS tetap terlihat saat mentok y=767
        # (bug lama: sprite 16x16 tumus di bawah layar -> kursor hilang)
        pedge = q.dump("equix3edge")
        iedge = read_ppm(pedge)
        wed = count_white(iedge, 664, 700, 750, 768)
        check("T5b kursor TERLIHAT saat mentok dasar (edge-clamp)",
              wed >= 12, f"white={wed}")
        # geser di dalam taskbar — tetap terlihat
        mouse_move(q, 40, -23)         # (720, 744)
        cur = (720, 744)
        time.sleep(0.4)
        pfb = q.dump("equix3bottom")
        ifb = read_ppm(pfb)
        white = count_white(ifb, 706, 740, 740, 764)
        check("T5c kursor terlihat di taskbar (px putih)", white >= 12, f"white={white}")

        print("== T6: calculator mouse: 7 9 * 6 = sqrt ==")
        cur = goto(q, 680, 384, cur)
        cur = click_at(q, 1024, 304, cur)   # 7
        cur = click_at(q, 1160, 304, cur)   # 9
        cur = click_at(q, 1228, 304, cur)   # *
        cur = click_at(q, 1160, 360, cur)   # 6 (row2 col2)
        cur = click_at(q, 1228, 472, cur)   # =
        check("T6a kalkulator hitung 79*6=474",
              wait_serial("[equix:calc] = -> 474", 8, rig=q))
        cur = click_at(q, 1024, 472, cur)   # sqrt
        check("T6b kalkulator sqrt(474)",
              wait_serial("[equix:calc] sqrt -> 21.77", 8, rig=q))
        time.sleep(0.8)
        path3 = q.dump("equix3calc")
        img3 = read_ppm(path3)
        check("T6c display menampilkan hasil",
              has_text_white(img3, 1150, 1256, 174, 198, minpx=12))
        save_png(img3, PNG2)

        print("== T6b: calculator KEYBOARD: 12*4= ==")
        cur = click_at(q, 1150, 190, cur)   # klik body calc = fokus
        for ch in "12*4=":
            q.sendkey(keyname(ch), wait=0.15)
        check("T6d keyboard 12*4=48",
              wait_serial("[equix:calc] = -> 48", 8, rig=q))

        print("== T7: anti-lag ==")
        # T7a: respons kursor — gerak jauh, 0.5s, dump, kursor harus di posisi
        cur = goto(q, 1200, 300, cur)
        time.sleep(0.5)
        pr = q.dump("equix3resp")
        imr = read_ppm(pr)
        white = count_white(imr, 1196, 1222, 296, 322)
        check("T7a kursor responsif (<0.5s setelah gerak)", white >= 12, f"white={white}")

        # T7c: anti-flicker canary — dump SAAT mouse bergerak
        ok_flicker = True
        details = []
        for i in range(3):
            q.cmd(f"mouse_move {-200} 0", timeout=0.25)
            p = q.dump(f"equix3f{i}")
            imf = read_ppm(p)
            c_menu1 = px(imf, 24, 746)
            c_menu2 = px(imf, 90, 746)
            c_body = px(imf, 460, 250)
            if sum(c_menu1) < 500 or sum(c_menu2) < 500:
                ok_flicker = False
                details.append(f"menu={c_menu1}/{c_menu2}")
            if sum(c_body) < 500:
                ok_flicker = False
                details.append(f"body={c_body}")
            time.sleep(0.2)
        cur = (cur[0] - 600, cur[1])
        check("T7c anti-flicker: tak ada frame hitam saat gerak",
              ok_flicker, "; ".join(details))

        print("== T8: drag jendela About ==")
        cur = goto(q, 240, 80, cur)
        time.sleep(0.3)
        q.cmd("mouse_button 1", timeout=0.3)
        time.sleep(0.3)
        mouse_move(q, 250, 120, step=60)
        time.sleep(0.3)
        q.cmd("mouse_button 0", timeout=0.3)
        time.sleep(1.0)
        cur = (490, 200)
        path4 = q.dump("equix3drag")
        img4 = read_ppm(path4)
        c_new = px(img4, 460, 200)
        moved = (480 < sum(c_new) < 700 and mono(c_new))
        c_old = px(img4, 240, 80)
        cleared = sum(c_old) < 150
        check("T8 drag jendela bekerja", moved and cleared,
              f"new={c_new} old={c_old}")

        print("== T9: exit + stats anti-lag ==")
        cur = click_at(q, 56, h - 29, cur)
        time.sleep(1.0)
        exit_y = 558 + 8 + 3 * 36 + 8
        cur = click_at(q, 130, exit_y, cur)
        time.sleep(1.5)
        ok = wait_serial("desktop selesai", 10, rig=q)
        check("T9a exit ke shell", ok)

        s = serial()
        m_stats = re.search(
            r"\[equix\] stats: (\d+) frame, (\d+) px blit, rata2 (\d+) px/frame, ~(\d+) fps",
            s)
        if m_stats:
            frames = int(m_stats.group(1))
            px_tot = int(m_stats.group(2))
            avg_px = int(m_stats.group(3))
            fps = int(m_stats.group(4))
            print(f"  stats: {frames} frame, {px_tot} px, avg {avg_px} px/frame, {fps} fps")
            check("T9b dirty-rect nyata (avg px/frame << full 1044480)",
                  avg_px < 60000, f"avg={avg_px}")
            check("T9c fps layak (>=10)", fps >= 10, f"fps={fps}")
            check("T9d ada aktivitas render (>=30 frame)", frames >= 30,
                  f"frames={frames}")
        else:
            check("T9b stats terparse", False, "pattern tidak ketemu")

        time.sleep(1.0)
        check("T10 QEMU hidup", q.alive())
    finally:
        q.quit()

    print()
    print(f"PASS {len(PASS)} / FAIL {len(FAIL)}")
    if FAIL:
        print("FAILED:", FAIL)
        sys.exit(1)


if __name__ == "__main__":
    main()
