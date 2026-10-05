#!/usr/bin/env python3
"""fps_probe.py — ukur FPS DOOM di Equinox OS via QEMU screendump diff.

Metode: boot ISO -> `doom -warp 1` -> tahan W (sendkey w <hold_ms>) supaya
kamera jalan terus -> screendump berkala -> hitung frame unik/detik.
Juga ukur scene TITLE (idle) sebagai pembanding statis.

Pemakaian: python3 fps_probe.py [--secs 12] [--mode e1m1|title]
"""
import argparse
import hashlib
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.expanduser("~/morphos")
ISO = os.path.join(ROOT, "dist", "morphos.iso")
QEMU = os.path.expanduser("~/tools/root/usr/bin/qemu-system-i386")
QEMU_L = [os.path.expanduser("~/tools/root/usr/share/qemu"),
          os.path.expanduser("~/tools/root/usr/share/seabios")]
ENV = dict(os.environ)
ENV["LD_LIBRARY_PATH"] = (
    os.path.expanduser("~/tools/root/usr/lib/x86_64-linux-gnu") + ":"
    + os.path.expanduser("~/tools/root/lib/x86_64-linux-gnu") + ":"
    + os.path.expanduser("~/tools/root/usr/lib"))


class Mon:
    def __init__(self):
        self.sock_path = "/tmp/fpsprobe.sock"
        for f in (self.sock_path,):
            if os.path.exists(f):
                os.unlink(f)
        self.proc = subprocess.Popen(
            [QEMU, "-L", QEMU_L[0], "-L", QEMU_L[1], "-m", "64",
             "-cdrom", ISO, "-display", "none", "-vga", "std", "-nic", "none",
             "-monitor", "unix:" + self.sock_path + ",server,nowait",
             "-no-reboot"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
        deadline = time.time() + 15
        while not os.path.exists(self.sock_path):
            if time.time() > deadline:
                raise RuntimeError("monitor socket tidak muncul")
            time.sleep(0.1)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(self.sock_path)
        self.s.settimeout(60)
        time.sleep(1.0)
        self.drain()

    def drain(self, t=0.6):
        self.s.settimeout(t)
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    break
        except socket.timeout:
            pass

    def cmd(self, line, wait=0.15):
        self.s.settimeout(30)
        self.s.sendall((line + "\n").encode())
        time.sleep(wait)
        self.drain(0.4)

    def sendkey(self, keys, hold_ms=None):
        if hold_ms is None:
            self.cmd("sendkey " + keys, 0.05)
        else:
            self.cmd(f"sendkey {keys} {hold_ms}", 0.05)

    def screendump(self, path):
        self.s.settimeout(30)
        self.s.sendall(("screendump " + path + "\n").encode())
        # tunggu file muncul + stabil
        t0 = time.time()
        while time.time() - t0 < 10:
            if os.path.exists(path):
                s1 = os.path.getsize(path)
                time.sleep(0.05)
                if os.path.exists(path) and os.path.getsize(path) == s1 and s1 > 1000:
                    self.drain(0.3)
                    return True
            time.sleep(0.05)
        self.drain(0.3)
        return os.path.exists(path)

    def quit(self):
        try:
            self.cmd("quit", 0.3)
        except Exception:
            pass
        try:
            self.proc.terminate()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def wait_prompt(mon, secs=45):
    """Tunggu boot ke shell (kirim newline utk prompt)."""
    time.sleep(secs if secs else 30)
    mon.sendkey("ret")
    time.sleep(0.5)


def measure(mon, secs, hold_w=True, interval=0.25):
    """Sample screendump tiap `interval` detik selama `secs` detik.
    Return (unique_changes, samples, wall_secs)."""
    tmpdir = "/tmp/fps_frames"
    shutil.rmtree(tmpdir, ignore_errors=True)
    os.makedirs(tmpdir)
    t0 = time.time()
    last_hash = None
    changes = 0
    samples = 0
    next_dump = time.time()
    end = t0 + secs
    while time.time() < end:
        now = time.time()
        if now < next_dump:
            time.sleep(0.02)
            continue
        next_dump = now + interval
        # tahan W sampai akhir window agar kamera terus bergerak
        if hold_w and end - now > 1.2:
            mon.sendkey("w", 1000)
        p = os.path.join(tmpdir, f"f{samples:05d}.ppm")
        if mon.screendump(p):
            samples += 1
            h = hashlib.md5(open(p, "rb").read()).hexdigest()
            if h != last_hash:
                changes += 1
                last_hash = h
        try:
            os.unlink(p)
        except OSError:
            pass
    return changes, samples, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=12)
    ap.add_argument("--mode", choices=["e1m1", "title"], default="e1m1")
    args = ap.parse_args()

    print(f"[fps-probe] mode={args.mode} secs={args.secs}")
    mon = Mon()
    try:
        # boot ke shell
        print("[fps-probe] booting (50s)...")
        time.sleep(50)
        mon.sendkey("ret")
        time.sleep(1.0)

        if args.mode == "e1m1":
            print("[fps-probe] ketik: doom -warp 1")
            for k in "doom -warp 1":
                mon.sendkey(k if k != " " else "spc", None)
            mon.sendkey("ret")
            print("[fps-probe] tunggu E1M1 render (25s)...")
            time.sleep(25)
        else:
            print("[fps-probe] ketik: doom")
            for k in "doom":
                mon.sendkey(k, None)
            mon.sendkey("ret")
            print("[fps-probe] tunggu title (25s)...")
            time.sleep(25)

        changes, samples, wall = measure(
            mon, args.secs, hold_w=(args.mode == "e1m1"))
        fps_lower = (changes - 1) / wall if changes > 1 else 0.0
        # upper bound: kalau tiap sample interval adalah frame baru
        print(f"[fps-probe] mode={args.mode}: "
              f"changes={changes} samples={samples} wall={wall:.1f}s")
        print(f"[fps-probe] FPS (unique-frame lower bound): {fps_lower:.1f}")
        print(f"[fps-probe] catatan: screendump interval {0.25}s membatasi "
              f"presisi; nilai ini batas BAWAH")
    finally:
        mon.quit()


if __name__ == "__main__":
    main()
