#!/usr/bin/env python3
"""term_gui_test.py — uji jendela Terminal di desktop EquiX (v0.4.5).

Arsitektur yang diuji: jendela Terminal = "layar" bagi shell task yang
berjalan di konsolnya sendiri (task_create_shell, jalur F1). Output shell
masuk cell mirror konsol -> digambar compose() ke jendela; keystroke
disuntikkan ke ring konsol terminal lewat keyboard_push_scancode().

  T1  boot ke shell
  T2  desktop aktif
  T3  buka Terminal via launcher MENU (mouse)
  T4  shell task terminal ter-spawn ("new console active" di serial)
  T5  perintah asli shell.cpp jalan dari GUI ("info" -> serial)
  T6  output TERLIHAT di jendela (OCR layar: teks putih-di-hitam)
  T7  ESC saat terminal fokus TIDAK keluar desktop (masuk ke terminal)
  T10 F1/F2 ditelan selama console terkunci (console_lock)
  T11 tutup Terminal (X) -> buka lagi: shell di-spawn ulang, jalan lagi
  T13 8x buka/tutup berulang: tidak bocor slot task / konsol
  T14 program .mrp (spawn spin) jalan di konsol terminal + tampil di jendela
  T8  keluar via MENU > Exit to shell -> stats + kembali ke shell
  T9  konsol & task terminal dilepas (tidak bocor) + shell HIDUP
  T15 ps: SELURUH sesi terminal (shell + anaknya) ikut mati saat jendela
      ditutup — tak ada task tersisa di konsol non-0 / program spin
  T12 console_unlock: F1/F2 berfungsi lagi setelah desktop ditutup

Fase 2 — SCROLLBACK (roda mouse / dua jari touchpad / PgUp-PgDn):
  T16a perangkat menyetujui paket 4 byte (dz) di desktop
  T16b/c penanda riwayat dieksekusi lalu tergusur keluar layar
  T16d/e roda mouse menggeser ke riwayat (injeksi QMP input-send-event
         tombol wheel-up — HMP `mouse_move ... dz` memakai REL_Z yang
         DIABAIKAN ps2_mouse_event() QEMU; roda = tombol), penanda
         terbaca lagi + badge "scrollback -N baris" di titlebar
  T16f/g roda ke bawah -> tampilan live (badge hilang)
  T16h/i PgUp / PgDn (jalur term_scroll yang sama)
  T16j mengetik saat di riwayat -> kembali ke live + output baru

Semua tes roda/masuk mode riwayat menaruh kursor di ATAS jendela
Terminal lebih dulu — hit_window() adalah pengarah wheel, jadi wheel
dengan kursor di taskbar memang sengaja diabaikan.
"""
import json
import os
import re
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_test_v032 import (Qemu, ISO, SERIAL, serial, since, wait_serial,
                            check, read_ppm, PASS, FAIL)
import boot_test_v032 as b32
from qemu_v107_test import screen_text

# mesin ini memakai QEMU sistem (harness lama menunjuk ~/tools/root)
b32.QEMU = "/usr/bin/qemu-system-i386"

# ---- QMP: injeksi tombol wheel (roda mouse) ------------------------
# QEMU's PS/2 driver IGNORES REL_Z (HMP `mouse_move dx dy dz`), it takes
# wheel motion from INPUT_BUTTON_WHEEL_UP/DOWN only — so the wheel has to
# be injected through QMP input-send-event.
QMP_SOCK = tempfile.mktemp(prefix="qmp-equix-", suffix=".sock")
Qemu.EXTRA = ["-qmp", "unix:" + QMP_SOCK + ",server,nowait"]


class Qmp:
    """Klien QMP mini (satu perintah = satu objek JSON per baris)."""

    def __init__(self, path):
        end = time.time() + 15
        while not os.path.exists(path):
            if time.time() > end:
                raise RuntimeError("socket QMP tidak muncul: " + path)
            time.sleep(0.1)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.s.settimeout(0.4)
        self.buf = b""
        self._read_obj(lambda o: "QMP" in o, 5.0)      # greeting
        self.call("qmp_capabilities")

    def _read_obj(self, pred, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                if line.strip():
                    o = json.loads(line)
                    if pred(o):
                        return o
                continue
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                break
            self.buf += d
        return None

    def call(self, cmd, **args):
        msg = {"execute": cmd}
        if args:
            msg["arguments"] = args
        self.s.sendall((json.dumps(msg) + "\n").encode())
        o = self._read_obj(lambda x: "return" in x or "error" in x, 5.0)
        if o is None:
            raise RuntimeError("QMP tak menjawab: " + cmd)
        return o

    def wheel(self, up, steps=1):
        """Satu detent roda ke atas (up=True) / ke bawah."""
        btn = "wheel-up" if up else "wheel-down"
        for _ in range(steps):
            o = self.call("input-send-event",
                          events=[{"type": "btn",
                                   "data": {"down": True, "button": btn}}])
            if "error" in o:
                raise RuntimeError("input-send-event: " +
                                   o["error"].get("desc", "?"))
            time.sleep(0.05)
b32.QEMU_L = ["/usr/share/qemu", "/usr/share/qemu"]

TASKBAR_H = 44.0
LAUNCH_H = 36 * (5 + 1) + 16          # APP_COUNT = 5


def mouse_move(q, dx, dy, step=120):
    while dx != 0 or dy != 0:
        sx = max(-step, min(step, dx))
        sy = max(-step, min(step, dy))
        q.cmd(f"mouse_move {sx} {sy}", timeout=0.3)
        dx -= sx
        dy -= sy
        time.sleep(0.06)


def goto(q, x, y, cur):
    mouse_move(q, x - cur[0], y - cur[1])
    return (x, y)


def click(q):
    q.cmd("mouse_button 1", timeout=0.3)
    time.sleep(0.25)
    q.cmd("mouse_button 0", timeout=0.3)
    time.sleep(0.5)


def click_at(q, x, y, cur):
    cur = goto(q, x, y, cur)
    click(q)
    return cur


def ocr(q, tag):
    path = q.dump(tag)
    txt = screen_text(path)
    w, h, _ = read_ppm(path)
    return txt, (w, h)


def main():
    if not os.path.exists(ISO):
        print("[term] ISO missing:", ISO)
        return 2
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[term] boot ...", flush=True)
    q = Qemu(ISO)
    try:
        qmp = Qmp(QMP_SOCK)
    except Exception as e:
        qmp = None
        print("[term] QMP gagal:", e, flush=True)
    try:
        ok = wait_serial("user $", 150, q)
        check("T1 boot ke shell", ok)
        if not ok:
            q.quit()
            print(serial()[-2500:])
            return 1

        # ---- T2: desktop ----
        q.type_line("desktop", wait=3.0)
        check("T2 desktop aktif", wait_serial("desktop aktif", 40, q))

        path = q.dump("term_desktop")
        W, H, _ = read_ppm(path)
        cur = (W // 2, H // 2)
        print(f"[term] layar {W}x{H}", flush=True)

        # ---- T3: launcher MENU -> baris "Terminal" ----
        ly = H - TASKBAR_H - LAUNCH_H - 6
        cur = click_at(q, 56, int(H - TASKBAR_H + 22), cur)   # tombol MENU
        txt, _ = ocr(q, "term_menu")
        cur = click_at(q, 130, int(ly + 8 + 4 * 36 + 18), cur)  # baris ke-5
        time.sleep(2.0)

        # ---- T4: shell task terminal spawn ----
        spawn = wait_serial("new console active", 20, q)
        check("T4 shell task terminal ter-spawn", spawn)
        check("T4b layer Terminal tidak dialokasikan (arena tidak penuh)",
              "layer 4 'Terminal' gagal" not in serial())

        # ---- T5: perintah asli shell.cpp dieksekusi ----
        base = len(serial())
        q.type_line("info", wait=1.5)
        check("T5 perintah shell dieksekusi (serial)",
              "Equinox OS 0.4 Beta" in since(base), since(base)[-300:])

        # ---- T6: output terlihat di jendela (OCR putih-di-hitam) ----
        time.sleep(1.0)
        txt, _ = ocr(q, "term_echo")
        with open("/tmp/term_gui_ocr.txt", "w") as fh:
            fh.write(txt)
        check("T6 output terlihat di jendela Terminal",
              "Equinox OS 0.4 Beta" in txt,
              "OCR dump di /tmp/term_gui_ocr.txt")

        # ---- T7: ESC masuk ke terminal, BUKAN keluar desktop ----
        q.sendkey("esc", wait=0.6)
        check("T7a ESC tidak mengeluarkan desktop",
              "desktop selesai" not in serial())
        base = len(serial())
        q.type_line("zz-after-esc", wait=1.5)
        check("T7b terminal masih menerima input setelah ESC",
              "Unknown command: 'zz-after-esc'" in since(base),
              since(base)[-300:])
        txt, _ = ocr(q, "term_esc")
        check("T7c output setelah ESC terbaca di jendela",
              "zz-after-esc" in txt)

        # ---- T10: F1/F2 ditelan selama console terkunci ----
        base = len(serial())
        q.sendkey("f1", wait=1.0)
        q.sendkey("f2", wait=1.0)
        check("T10 F1/F2 ditelan console_lock (tanpa output baru)",
              len(serial()) == base, f"delta {len(serial()) - base} byte")
        check("T10b desktop tetap hidup setelah F1/F2",
              "desktop selesai" not in serial())

        # ---- T11: tutup Terminal (tombol X) lalu buka lagi ----
        spawns = serial().count("new console active")
        cur = click_at(q, 1020, 112, cur)                  # X titlebar Terminal
        time.sleep(1.5)
        cur = click_at(q, 56, int(H - TASKBAR_H + 22), cur)   # MENU
        time.sleep(0.4)
        cur = click_at(q, 130, int(ly + 8 + 4 * 36 + 18), cur)  # baris Terminal
        check("T11 shell terminal di-spawn ulang setelah close",
              serial().count("new console active") > spawns,
              f"{spawns} -> {serial().count('new console active')}")
        base = len(serial())
        q.type_line("info", wait=1.5)
        check("T11b shell BARU menerima input",
              "Equinox OS 0.4 Beta" in since(base), since(base)[-300:])
        time.sleep(1.0)
        txt, _ = ocr(q, "term_reopen")
        check("T11c isi terminal hasil-reopen tampil di jendela",
              "Equinox OS 0.4 Beta" in txt)

        # ---- T13: 8x buka/tutup — tak ada bocor slot task / konsol ----
        # (task_create_shell mengambil 1 task + 1 konsol tiap buka; bila
        #  penutupan tidak melepasnya, siklus ke-7/8 akan gagal spawn
        #  karena MAX_TASKS = N_CONSOLES = 8.)
        base_spawn = serial().count("new console active")
        leaks = []
        for cyc in range(1, 9):
            cur = click_at(q, 1020, 112, cur)                 # X titlebar
            time.sleep(0.8)
            cur = click_at(q, 56, int(H - TASKBAR_H + 22), cur)      # MENU
            time.sleep(0.4)
            cur = click_at(q, 130, int(ly + 8 + 4 * 36 + 18), cur)  # Terminal
            time.sleep(1.2)
            n = serial().count("new console active")
            if n < base_spawn + cyc:
                leaks.append(f"siklus {cyc}: spawn {n} (min {base_spawn + cyc})")
        check("T13 8x buka/tutup tanpa bocor task/konsol", not leaks,
              "; ".join(leaks))
        base = len(serial())
        q.type_line("info", wait=1.5)
        check("T13b terminal masih hidup setelah 8 siklus",
              "Equinox OS 0.4 Beta" in since(base), since(base)[-300:])

        # ---- fase 2: SCROLLBACK (roda mouse / dua jari touchpad / PgUp) ----
        # T16a: perangkat menyetujui paket 4 byte (byte ke-4 = dz)
        mlines = [ln for ln in serial().split("\n") if "[equix] mouse" in ln]
        check("T16a negosiasi roda mouse (pak4 byte) di desktop",
              any("wheel=1" in l for l in mlines), mlines)
        # T16b: satu baris penanda, lalu output panjang agar ia TERGUSUR
        base = len(serial())
        q.type_line("zz-hist-marker", wait=1.2)
        check("T16b penanda riwayat dieksekusi",
              "Unknown command: 'zz-hist-marker'" in since(base),
              since(base)[-300:])
        for i in range(16):
            q.type_line("zz%02d" % i, wait=0.5)
        time.sleep(1.5)
        txt, _ = ocr(q, "term_sb_live")
        check("T16c penanda tergusur keluar layar (riwayat terisi)",
              "zz-hist-marker" not in txt,
              "OCR dump /tmp: lihat qdump-*/term_sb_live")

        # T16d/e: RODA MOUSE -> geser riwayat.
        # Kursor HARUS di atas jendela Terminal: hit_window() adalah
        # pengarah wheel (kursor di taskbar/menu = event roda diabaikan
        # dengan sengaja). Injeksi lewat QMP tombol wheel-up (bukan HMP
        # mouse_move ... dz — REL_Z diabaikan ps2_mouse_event() QEMU).
        cur = goto(q, 700, 300, cur)
        time.sleep(0.4)
        got = ""
        if qmp is not None:
            for i in range(14):
                qmp.wheel(True, steps=2)           # roda ke ATAS -> riwayat
                time.sleep(0.30)
                txt, _ = ocr(q, f"term_sb_wheel{i}")
                if "zz-hist-marker" in txt:
                    got = txt
                    break
        wlines = [ln.strip() for ln in serial().split("\n")
                  if "[term] wheel" in ln or "[term] scroll" in ln]
        check("T16d roda mouse menggeser tampilan ke riwayat",
              any("dz=" in l for l in wlines) and bool(got),
              wlines[-6:])
        check("T16e penanda riwayat TERBACA kembali + badge titlebar",
              bool(got) and "scrollback" in got.lower(),
              "badge harus terbaca OCR sebagai 'scrollback -N baris'; "
              f"log: {wlines[-4:]}")

        # T16f/g: roda ke bawah -> kembali LIVE (badge hilang, penanda
        # tergusur lagi — artinya tampilan benar-benar mengikuti baris terbaru)
        if qmp is not None:
            for i in range(12):
                qmp.wheel(False, steps=2)
                time.sleep(0.06)
        time.sleep(0.8)
        txt, _ = ocr(q, "term_sb_back")
        check("T16f roda ke bawah mengembalikan tampilan live",
              "scrollback" not in txt.lower(),
              [ln.strip() for ln in serial().split("\n")
               if "[term] wheel->off" in ln][-3:])
        check("T16g saat live penanda kembali tergusur",
              "zz-hist-marker" not in txt)

        # T16h/i: PgUp/PgDn (fallback papan ketik — jalur term_scroll sama)
        base = len(serial())
        q.sendkey("pgup", wait=1.0)
        txt, _ = ocr(q, "term_sb_pgup")
        klines = [ln.strip() for ln in since(base).split("\n")
                  if "[term]" in ln]
        check("T16h PgUp mengaktifkan scrollback", "scrollback" in txt.lower(),
              klines)
        base = len(serial())
        q.sendkey("pgdn", wait=1.0)
        txt, _ = ocr(q, "term_sb_pgdn")
        check("T16i PgDn kembali ke tampilan live",
              "scrollback" not in txt.lower(),
              [ln.strip() for ln in since(base).split("\n") if "[term]" in ln])

        # T16j: MENGETIK saat tampilan di riwayat -> kembali ke live
        q.sendkey("pgup", wait=0.8)
        txt, _ = ocr(q, "term_sb_typ_pre")
        pre = "scrollback" in txt.lower()
        base = len(serial())
        q.type_line("info", wait=1.5)
        time.sleep(1.0)
        txt, _ = ocr(q, "term_sb_typ_post")
        check("T16j mengetik mengembalikan tampilan ke live + output baru",
              pre and "scrollback" not in txt.lower()
              and "Equinox OS 0.4 Beta" in txt,
              f"pre={pre} log={[ln.strip() for ln in since(base).split(chr(10)) if '[term]' in ln]}")

        # ---- T14: program .mrp berjalan di konsol terminal ----
        base = len(serial())
        q.type_line("spawn spin", wait=2.5)
        check("T14 program .mrp (spin) dieksekusi dari jendela Terminal",
              "[spin] pid" in since(base), since(base)[-300:])
        time.sleep(1.5)
        txt, _ = ocr(q, "term_spin")
        check("T14b output program TERLIHAT di jendela Terminal",
              "spinning" in txt)
        check("T14c GUI tetap hidup walau program grafis/anak jalan",
              "desktop selesai" not in serial())
        # tutup jendela -> SELURUH sesi (shell + spin) harus ikut mati
        cur = click_at(q, 1020, 112, cur)
        time.sleep(2.0)

        # ---- T8: keluar via MENU > Exit to shell ----
        cur = click_at(q, 56, int(H - TASKBAR_H + 22), cur)   # MENU
        time.sleep(0.4)
        cur = click_at(q, 130, int(ly + 8 + 5 * 36 + 18), cur)  # Exit
        check("T8 keluar ke shell", wait_serial("desktop selesai", 30, q))
        check("T8b stats exit dicetak", "stats:" in serial())

        # ---- T9: shell hidup lagi setelah exit ----
        time.sleep(2.0)
        q.type_line("echo balik-ke-shell", wait=1.5)
        check("T9 shell teks hidup setelah exit",
              "balik-ke-shell" in serial()[len(serial()) - 4000:])

        # ---- T15: ps — tak ada sisa SESI terminal setelah desktop keluar ----
        # (task_create_shell + anak-anaknya memakai konsol non-0; bila
        #  penutupan jendela tidak membunuh seluruh sesi, spin/konsolnya
        #  masih ada di tabel — slot task bocor.)
        time.sleep(1.5)
        base = len(serial())
        q.type_line("ps", wait=2.5)
        ps = since(base, limit=20000)
        rows = []
        for ln in ps.split("\n"):
            m = re.match(r"^\s*(-?\d+)\s+(\S+)\s+(\S+)\s+(-?\d+)\s+(\S.*)$", ln)
            if m:
                rows.append((int(m.group(1)), m.group(2), int(m.group(4)),
                             m.group(5)))
        leaked = [r for r in rows if r[2] >= 1]
        check("T15 ps: tak ada task di konsol non-0 setelah desktop keluar",
              not leaked,
              "; ".join(f"pid{r[0]} con{r[2]} {r[3]}" for r in leaked))
        check("T15b ps: tak ada sisa program anak terminal (spin)",
              "spin" not in ps, [ln for ln in ps.split("\n") if "spin" in ln])
        print("  ps rows:", rows, flush=True)

        # ---- T12: console_unlock — F1 berfungsi lagi setelah desktop ----
        q.sendkey("f1", wait=2.0)
        check("T12 F1 membuat konsol baru (lock dilepas saat exit)",
              "[task] new shell pid" in serial())
        q.sendkey("f2", wait=1.5)
        base = len(serial())
        q.type_line("echo f2-balik", wait=1.5)
        check("T12b F2 kembali & shell konsol lama hidup",
              "f2-balik" in since(base), since(base)[-300:])

        # simpan screenshot utk review
        p = q.dump("term_final")
        print("[term] dump:", p, flush=True)
        return 0 if not FAIL else 1
    finally:
        q.quit()
        print("\n== hasil ==")
        for n in PASS:
            print(f"  [PASS] {n}")
        for n in FAIL:
            print(f"  [FAIL] {n}")


if __name__ == "__main__":
    sys.exit(main())
