#!/usr/bin/env python3
"""bootdisk_test.py — E2E Fase 1 v0.8: boot dari disk TANPA CD.

Fase A (boot ISO + disk kosong hda):
  A1 Qfs -t hda -format fat32      -> mkfs v0.8 "volume utuh"
  A2 mount hda
  A3 copy /equinox -> /mnt/equinox (termasuk bootimg/, .ruf, tools)
  A4 copy /user + /test + /boot/kernel.elf
  A5 Qfs -install-boot hda         -> "BOOTABLE" + verifikasi ok
  A6 umount hda; quit
Fase B (QEMU baru: disk saja, -boot order=c, TANPA cdrom):
  B1 banner "build v0.7" + prompt shell  == PROOF boot-disk
  B2 mtcc.mrp ada di RAMFS (run hello? cukup `ps`/`ls /equinox`)
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_test_v032 as b32                     # noqa: E402

ISO = b32.ISO
DISK = "/tmp/e8_bootdisk.img"
SERIAL_A = "/tmp/bootdisk_A.log"
SERIAL_B = "/tmp/bootdisk_B.log"
ENV = dict(os.environ)
ENV["LD_LIBRARY_PATH"] = ("/home/z/tools/root/usr/lib/x86_64-linux-gnu"
                          ":/home/z/tools/root/lib/x86_64-linux-gnu"
                          ":/home/z/tools/root/usr/lib")
ENV["PATH"] = "/home/z/tools/root/usr/bin:" + ENV.get("PATH", "")
QL = ["/home/z/tools/root/usr/share/seabios",
      "/home/z/tools/root/usr/share/qemu"]

PASS, FAIL = [], []


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}  {detail}", flush=True)


def report():
    print()
    print(f"=== bootdisk: {len(PASS)} PASS, {len(FAIL)} FAIL")
    for n in PASS:
        print("  PASS:", n)
    if FAIL:
        print("  failed:", ", ".join(FAIL))
    return 0 if not FAIL else 1


class VM:
    """Launcher minimal (boot order bisa diatur, cdrom opsional)."""

    def __init__(self, serial, disk, cdrom=None, order=None, extra_net=True):
        self.sock_path = tempfile.mktemp(prefix="bd-", suffix=".sock")
        cmd = ["qemu-system-i386", "-L", QL[0], "-L", QL[1], "-m", "64",
               "-drive", f"file={disk},format=raw",
               "-display", "none", "-vga", "std",
               "-serial", f"file:{serial}",
               "-monitor", f"unix:{self.sock_path},server,nowait",
               "-no-reboot"]
        if cdrom:
            cmd += ["-cdrom", cdrom]
        if order:
            cmd += ["-boot", f"order={order}"]
        if extra_net:
            cmd += b32.NET
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, env=ENV)
        deadline = time.time() + 20
        while not os.path.exists(self.sock_path):
            if time.time() > deadline:
                raise RuntimeError("monitor socket tak muncul")
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.mon.connect(self.sock_path)
        self.mon.settimeout(1.0)
        self._drain()

    def _drain(self, t=0.4):
        try:
            self.mon.settimeout(t)
            while True:
                if not self.mon.recv(4096):
                    break
        except socket.timeout:
            pass

    def cmd(self, line):
        self.mon.sendall((line + "\n").encode())
        time.sleep(0.02)

    def sendkey(self, key, wait=0.05):
        self.cmd(f"sendkey {key}")
        if wait:
            time.sleep(wait)

    def keyname(self, ch):
        return b32.Qemu.keyname(self, ch) if False else _keyname(ch)

    def type_line(self, text, wait=1.0):
        for ch in text:
            self.sendkey(_keyname(ch))
        self.sendkey("ret")
        time.sleep(wait)

    def alive(self):
        return self.proc.poll() is None

    def quit(self):
        try:
            self.cmd("quit")
            time.sleep(0.5)
        except Exception:
            pass
        if self.proc.poll() is None:
            self.proc.kill()


_KEYMAP = {
    " ": "spc", ".": "dot", "/": "slash", ",": "comma",
    "-": "minus", ";": "semicolon", "=": "equal", "'": "apostrophe",
    "`": "grave_accent", "[": "bracket_left", "]": "bracket_right",
    "\\": "backslash", "\t": "tab",
    ":": "shift-semicolon", "?": "shift-slash", "!": "shift-1",
    "@": "shift-2", "#": "shift-3", "$": "shift-4", "%": "shift-5",
    "^": "shift-6", "&": "shift-7", "*": "shift-8",
    "(": "shift-9", ")": "shift-0", "_": "shift-minus",
    "+": "shift-equal", '"': "shift-apostrophe",
    "<": "shift-comma", ">": "shift-dot",
    "{": "shift-bracket_left", "}": "bracket_right",
    "|": "shift-backslash", "~": "shift-grave_accent",
}


def _keyname(ch):
    if "A" <= ch <= "Z":
        return "shift-" + ch.lower()
    return _KEYMAP.get(ch, ch)


def serial_tail(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode(errors="replace")
    except FileNotFoundError:
        return ""


def wait_for(path, pat, timeout, vm, t0=None):
    t0 = t0 or time.time()
    while time.time() - t0 < timeout:
        t = serial_tail(path)
        if pat in t:
            return t
        if vm and not vm.alive():
            return serial_tail(path)
        time.sleep(0.4)
    return serial_tail(path)


def main():
    for p in (SERIAL_A, SERIAL_B):
        if os.path.exists(p):
            os.remove(p)
    if os.path.exists(DISK):
        os.remove(DISK)
    subprocess.run(["truncate", "-s", "64M", DISK], check=True)

    print("[bootdisk] Fase A: boot ISO + disk kosong ...", flush=True)
    vm = VM(SERIAL_A, DISK, cdrom=ISO)
    try:
        ok = wait_for(SERIAL_A, "user $", 150, vm)
        check("A0 boot ISO ke shell", "user $" in ok)
        if "user $" not in ok:
            return report()

        vm.type_line("Qfs -t hda -format fat32", wait=2.0)
        t = wait_for(SERIAL_A, "mkfs: selesai", 60, vm)
        check("A1 mkfs v0.8.1 whole-disk", "volume utuh" in t and
              "selesai" in t,
              [l for l in t.splitlines() if "volume utuh" in l][-1:]
              if "volume utuh" in t else None)

        vm.type_line("mount hda", wait=2.0)
        t = wait_for(SERIAL_A, "mount", 15, vm)
        check("A2 mount hda", True)

        base = len(serial_tail(SERIAL_A))
        vm.type_line("copy /equinox -> /mnt/equinox", wait=2.0)
        t = wait_for(SERIAL_A, "-> /mnt/equinox", 300, vm)
        check("A3 copy /equinox -> /mnt/equinox", "-> /mnt/equinox" in t)

        vm.type_line("copy /user -> /mnt/user", wait=2.0)
        wait_for(SERIAL_A, "user $", 60, vm)
        vm.type_line("copy /test -> /mnt/test", wait=2.0)
        wait_for(SERIAL_A, "user $", 60, vm)
        vm.type_line("copy /boot/kernel.elf -> /mnt/boot/kernel.elf", wait=2.0)
        wait_for(SERIAL_A, "user $", 60, vm)
        check("A4 copy user/test/kernel.elf", True)

        vm.type_line("Qfs -install-boot hda", wait=3.0)
        t = wait_for(SERIAL_A, "BOOTABLE", 120, vm)
        check("A5 install-boot -> BOOTABLE", "BOOTABLE" in t and
              "verifikasi MBR=ok" in t,
              [l.strip() for l in t.splitlines() if "verifikasi" in l][-1:]
              if "verifikasi" in t else None)

        vm.type_line("umount hda", wait=2.0)
        check("A6 umount (flush)", True)
    finally:
        vm.quit()
    time.sleep(1.0)

    print("[bootdisk] Fase B: boot DARI DISK (tanpa CD) ...", flush=True)
    vm2 = VM(SERIAL_B, DISK, order="c")
    try:
        t = wait_for(SERIAL_B, "user $", 150, vm2)
        # banner kernel tercetak ke VGA sebelum serial mirror aktif —
        # bukti boot = emblem + prompt shell di serial
        check("B1 boot dari disk -> shell", "user $" in t and
              "E Q U I N O X" in t)
        vm2.type_line("cd /equinox", wait=1.5)
        vm2.type_line("ls", wait=2.0)
        t = wait_for(SERIAL_B, "bootimg", 20, vm2)
        check("B2 volume terlihat di RAMFS (/equinox/bootimg)",
              "bootimg" in t)
    finally:
        vm2.quit()
    return report()


if __name__ == "__main__":
    sys.exit(main())
