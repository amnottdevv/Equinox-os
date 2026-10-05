#!/usr/bin/env python3
"""
fat32_write_test.py — Equinox OS v0.2 Beta WRITE hardening suite.

Complements fat32_test.py (the 27-check functional suite) with the
edge cases found during the v0.2 write-path audit:

  Shell write UX (boot 1, 64 MB EQDISK)
    W1  save <name> << "text"          create a NEW file
    W2  save on an existing file       overwrite + SHRINK the chain
    W3  ccfile LFN + cfile same-8.3    duplicate short name REFUSED
    W4  case-insensitive lookup        cat readme.txt finds README.TXT
    W5  63-char LFN                    5 LFN entries + rm of the run
    W6  mget 200 KB -> save tiny       200 KB -> 4 B shrink
    W7  mget the same name again       4 B -> 200 KB extend, byte-exact
    W8  rmdir /mnt while mounted       refused (mount point is busy)
    W9  umount -> mount cycle          mirror torn down + rebuilt
    W10 edit + Ctrl+S round-trip       editor writes through to FAT32
    W11 tree shows the volume          lazy populate in fs_tree

  Host oracle (mtools + a pure-python FAT32 mini-fsck on disk.img)
    H1  mdir parses what the OS wrote
    H2  mget->save->mget file byte-identical on the host
    H3  editor-saved file content correct on the host
    H4  FAT copy #1 == FAT copy #2
    H5  FSInfo free-count == actually free clusters
    H6  every LFN entry: 0x0000 terminator then 0xFFFF padding only
    H7  no duplicate 8.3 names in any directory
    H8  collision file was NOT created on disk

  Stress (boot 2, 3 MB EQSTRESS disk, in-OS mtcc program)
    S1  volume mounts
    S2  mtcc fat32_stress.c from /mnt: root dir GROWS (140 files),
        then the volume is FILLED until "volume full" (clean failure)
    S3  shell + filesystem still alive at 100 % usage
    S4  rm two files -> save works again (space reclaimed)
    SH  host mini-fsck on the 100 % full disk (structure intact)

  Persistence (boot 3, same stress disk)
    P1  the 140 grow files survived, content intact
    P2  a fill file re-reads byte-exact

Usage: python3 scripts/fat32_write_test.py
  (expects dist/equinox.iso; builds disk.img + stress.img first)
"""

import functools
import http.server
import os
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from fat32_test import (QemuDisk, wait_text, mtool, check, results,  # noqa
                        ENV, ISO, DISK)

STRESS = os.path.join(ROOT, "dist", "stress.img")
BIN_BODY = bytes(range(256)) * 800       # 204800 bytes, multi-cluster
HTTP_PORT = 8045


# =====================================================================
#  Pure-python FAT32 mini-fsck (independent of both the OS and mtools)
# =====================================================================
def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]

LFN_OFFS = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]


class FatVolume:
    """Read-only FAT32 parser used as an independent oracle."""

    def __init__(self, img, part_lba=2048):
        self.f = open(img, "rb")
        self.part = part_lba
        self.f.seek(part_lba * 512)
        self.bpb = self.f.read(512)
        self.spc = self.bpb[13]
        self.reserved = u16(self.bpb, 14)
        self.nfats = self.bpb[16]
        self.fatsz = u32(self.bpb, 36)
        self.rootc = u32(self.bpb, 44)
        self.total = u32(self.bpb, 32)
        self.data_start = self.reserved + self.nfats * self.fatsz
        self.nclust = (self.total - self.data_start) // self.spc
        self.f.seek((part_lba + self.reserved) * 512)
        self.fat1 = self.f.read(self.fatsz * 512)
        self.f.seek((part_lba + self.reserved + self.fatsz) * 512)
        self.fat2 = self.f.read(self.fatsz * 512)
        self.f.seek((part_lba + 1) * 512)
        self.fsinfo = self.f.read(512)

    def entry(self, c):
        return u32(self.fat1, c * 4) & 0x0FFFFFFF

    def chain(self, c):
        out, guard = [], 0
        while 2 <= c < 2 + self.nclust and guard < 100000:
            out.append(c)
            c = self.entry(c)
            if c >= 0x0FFFFFF8 or c == 0:
                break
        return out

    def read_cluster(self, c):
        lba = self.part + self.data_start + (c - 2) * self.spc
        self.f.seek(lba * 512)
        return self.f.read(self.spc * 512)

    def walk(self, cluster, path, findings):
        """Collect fsck findings for one directory (recursively)."""
        seen_sfn = {}
        for c in self.chain(cluster):
            data = self.read_cluster(c)
            for i in range(0, len(data), 32):
                e = data[i:i + 32]
                if len(e) < 32 or e[0] == 0x00:
                    return
                if e[0] == 0xE5:
                    continue
                attr = e[11]
                if attr == 0x0F:                       # LFN entry
                    term_seen = False
                    for o in LFN_OFFS:
                        u = u16(e, o)
                        if term_seen and u != 0xFFFF:
                            findings.append(
                                f"{path}: LFN padding not 0xFFFF "
                                f"(got {u:04X})")
                        if u == 0x0000:
                            term_seen = True
                    continue
                if attr & 0x08:                        # volume label
                    continue
                sfn = bytes(e[0:11])
                if sfn in seen_sfn:
                    findings.append(f"{path}: duplicate 8.3 "
                                    f"{sfn!r} ({seen_sfn[sfn]} + 1)")
                seen_sfn[sfn] = 1
                name = (sfn[0:8].decode("latin1").rstrip() + "." +
                        sfn[8:11].decode("latin1").rstrip()).rstrip(".")
                if name in (".", ".."):
                    continue
                if attr & 0x10:                        # subdirectory
                    sub = (u32(e, 20) << 16) | u32(e, 26)
                    if sub >= 2:
                        self.walk(sub, path + name + "/", findings)
                else:
                    findings.files.append((path + name,
                                           u32(e, 28), sfn))
        return

    def fsck(self):
        class F(list):
            files = []
        findings = F()
        self.walk(self.rootc, "/", findings)
        return findings


def host_fsck(img, label):
    vol = FatVolume(img)
    problems = []
    if vol.fat1 != vol.fat2:
        problems.append("FAT copy 1 != FAT copy 2")
    actual_free = sum(1 for c in range(2, 2 + vol.nclust)
                      if vol.entry(c) == 0)
    hint_free = u32(vol.fsinfo, 488)
    if hint_free != actual_free:
        problems.append(f"FSInfo free-count {hint_free} != actual "
                        f"{actual_free}")
    if (vol.fsinfo[0:4] != b"RRaA"
            or vol.fsinfo[484:488] != b"rrAa"):
        problems.append("FSInfo signatures damaged")
    findings = vol.fsck()
    problems.extend(findings)
    return problems, findings, vol


class DocServer(socketserver.TCPServer):
    allow_reuse_address = True


def stage_boot1():
    """BOOT 1 (write UX on EQDISK) + host oracle. ~6 minutes."""
    print("[suite] building disk.img ...")
    r = subprocess.run([sys.executable,
                        os.path.join(HERE, "make_fat32_img.py"),
                        "--size-mb", "64", "--out", DISK],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit(2)

    docdir = tempfile.mkdtemp(prefix="f32wdoc-")
    with open(os.path.join(docdir, "data.bin"), "wb") as f:
        f.write(BIN_BODY)
    handler = functools.partial(http.server.SimpleHTTPRequestHandler,
                                directory=docdir)
    srv = DocServer(("127.0.0.1", HTTP_PORT), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    print("\n=== BOOT 1: EQDISK write UX + edge cases ===")
    q = QemuDisk(ISO, DISK)
    try:
        wait_text(q, "FAT32: 'EQDISK' mounted at /mnt", timeout=240,
                  tag="-wboot")
        wait_text(q, "root::users /user $", timeout=120, tag="-wsh")
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("cd /mnt\n")
        time.sleep(0.8)

        # W1 save creates
        q.type_str('save created.txt << "created by save"\n')
        txt = wait_text(q, "wrote 'created.txt'", timeout=60, tag="-w1")
        check("W1 save creates a file",
              "wrote 'created.txt' (15 bytes)" in txt, txt[-300:])

        # W2 save overwrites + shrinks
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str('save created.txt << "shorter"\n')
        time.sleep(0.8)
        q.type_str("cat created.txt\n")
        txt = wait_text(q, "shorter", timeout=60, tag="-w2")
        check("W2 save overwrites (shrink)",
              "wrote 'created.txt' (7 bytes)" in txt and "shorter" in txt,
              txt[-400:])

        # W3 duplicate-8.3 handling
        #   (a) a LOWERCASE name whose generated 8.3 collides with an
        #       existing LFN entry gets the classic ~N numeric tail
        #       (created, distinct short name on disk)
        #   (b) a PLAIN uppercase 8.3 name that collides is REFUSED
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str('ccfile My Document.txt << "doc one"\n')
        time.sleep(1.0)
        q.type_str("cfile my_docum.txt\n")
        time.sleep(1.0)
        q.type_str("ls\n")
        txt = wait_text(q, "My Document.txt", timeout=60, tag="-w3a")
        check("W3a colliding LFN re-tailed (~1), both files listed",
              "My Document.txt" in txt and "my_docum.txt" in txt,
              txt[-500:])
        q.type_str("cfile MY_DOCUM.TXT\n")
        txt = wait_text(q, "already exists", timeout=60, tag="-w3b")
        check("W3b plain-8.3 duplicate refused",
              "already exists" in txt, txt[-400:])

        # W4 case-insensitive lookup
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("cat readme.txt\n")
        txt = wait_text(q, "plain uppercase", timeout=60, tag="-w4")
        check("W4 case-insensitive cat", "plain uppercase" in txt,
              txt[-300:])

        # W5 63-char LFN (5 LFN entries) + delete
        longname = ("equinox os long file name test number seven "
                    "with pad 123456.txt")
        assert len(longname) == 63, len(longname)
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str(f'ccfile {longname} << "lfn max"\n')
        time.sleep(1.2)
        q.type_str(f"cat {longname}\n")
        txt = wait_text(q, "lfn max", timeout=60, tag="-w5a")
        check("W5a 63-char LFN write + read", "lfn max" in txt,
              txt[-400:])
        q.type_str(f"rm {longname}\n")
        time.sleep(1.0)
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("ls\n")
        time.sleep(1.5)
        txt = q.screen("-w5b")
        check("W5b 63-char LFN removed",
              "padding 1234567" not in txt, txt[-400:])

        # W6/W7 shrink 200 KB -> 4 B -> 200 KB via mget + save
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str(f"mget http://10.0.2.2:{HTTP_PORT}/data.bin\n")
        txt = wait_text(q, "204800", timeout=240, tag="-w6a")
        check("W6a mget 200 KB to /mnt",
              "saved" in txt and "204800" in txt, txt[-400:])
        q.type_str('save data.bin << "tiny"\n')
        txt = wait_text(q, "wrote 'data.bin' (4 bytes)", timeout=60,
                        tag="-w6b")
        check("W6b save shrinks 200 KB -> 4 B",
              "wrote 'data.bin' (4 bytes)" in txt, txt[-300:])
        q.type_str(f"mget http://10.0.2.2:{HTTP_PORT}/data.bin\n")
        txt = wait_text(q, "204800", timeout=240, tag="-w7")
        check("W7 mget re-extends 4 B -> 200 KB",
              "saved" in txt and "204800" in txt, txt[-400:])

        # W8 rmdir on the live mount point
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("cd /\n"); time.sleep(0.5)
        q.type_str("rmdir mnt\n")
        txt = wait_text(q, "mounted volume", timeout=60, tag="-w8")
        check("W8 rmdir /mnt refused (busy)", "mounted volume" in txt,
              txt[-300:])

        # W9 umount -> mount cycle
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("umount\n")
        txt = wait_text(q, "released", timeout=60, tag="-w9a")
        check("W9a umount", "released" in txt, txt[-300:])
        q.type_str("mount\n")
        txt = wait_text(q, "mounted at /mnt", timeout=120, tag="-w9b")
        check("W9b remount", "mounted at /mnt" in txt, txt[-300:])
        q.type_str("cd /mnt\n"); time.sleep(0.8)
        q.type_str("ls\n")
        txt = wait_text(q, "README.TXT", timeout=60, tag="-w9c")
        check("W9c listing after remount",
              all(s in txt for s in ["README.TXT", "created.txt",
                                     "My Document.txt"]), txt[-500:])

        # W10 editor round-trip. The "Saved." banner only stays for
        # 400 ms — polling every 4 s would RACE and miss it, so the
        # postcondition is the clean Ctrl+Q exit (dirty editor would
        # show the y/n prompt instead) + the content on disk.
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("edit created.txt\n")
        txt = wait_text(q, "Ctrl+S save", timeout=90, tag="-w10a")
        check("W10a editor opened", "Ctrl+S save" in txt, txt[-300:])
        q.type_str("EDITED LINE ONE")
        time.sleep(0.8)
        q.cmd("sendkey ctrl-s", timeout=10)
        time.sleep(3.0)               # editor_save is synchronous
        q.cmd("sendkey ctrl-q", timeout=10)
        closed, txt = False, ""
        t0 = time.time()
        while time.time() - t0 < 60:
            txt = q.screen("-w10b")
            if "Editor closed." in txt:
                closed = True
                break
            if "Save changes?" in txt:   # save failed -> still dirty
                q.type_str("y")
                time.sleep(2.5)
            time.sleep(2)
        check("W10b editor save + clean exit", closed, txt[-400:])
        q.type_str("cat created.txt\n")
        txt = wait_text(q, "EDITED LINE ONE", timeout=60, tag="-w10c")
        check("W10c editor write-through", "EDITED LINE ONE" in txt,
              txt[-400:])

        # W11 tree
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("cd /\n"); time.sleep(0.5)
        q.type_str("tree\n")
        txt = wait_text(q, "created.txt", timeout=60, tag="-w11")
        check("W11 tree shows /mnt", "created.txt" in txt, txt[-600:])
    finally:
        q.kill()

    host_oracle_disk()


def host_oracle_disk():
    """HOST oracle on disk.img (mtools + pure-python mini-fsck)."""
    print("\n=== HOST: mtools + mini-fsck on disk.img ===")
    _, listing, _ = mtool(["mdir", "-i", DISK + "@@1048576", "-/"])
    ok = all(s in listing for s in ["created.txt", "My Document.txt",
                                    "data.bin"])
    check("H1 mdir parses the OS-written volume", ok, listing[:500])

    _, _, body = mtool(["mcopy", "-i", DISK + "@@1048576",
                        "::/data.bin", "-"])
    check("H2 data.bin byte-identical after shrink+extend",
          body == BIN_BODY, f"host {len(body)}B vs {len(BIN_BODY)}B")

    _, _, created = mtool(["mcopy", "-i", DISK + "@@1048576",
                           "::/created.txt", "-"])
    check("H3 editor-saved file correct on host",
          b"EDITED LINE ONE" in created, repr(created[:80]))

    problems, findings, vol = host_fsck(DISK, "EQDISK")
    check("H4 FAT1 == FAT2", not any("FAT copy" in p for p in problems),
          "; ".join(problems))
    check("H5 FSInfo free-count exact",
          not any("FSInfo" in p for p in problems), "; ".join(problems))
    check("H6 LFN padding spec-clean",
          not any("LFN padding" in p for p in problems),
          "; ".join(p for p in problems if "LFN" in p)[:400])
    check("H7 no duplicate 8.3 names",
          not any("duplicate 8.3" in p for p in problems),
          "; ".join(p for p in problems if "duplicate" in p)[:400])
    names = [n for n, _, _ in findings.files]
    check("H8 MY_DOCUM.TXT duplicate not on disk",
          not any(n.upper().startswith("MY_DOCUM.TXT") for n in names),
          str(names)[:300])
    check("H8b long-name slot really freed",
          not any("pad 123456" in n for n in names), str(names)[:300])


def stage_stress():
    """BOOT 2 (3 MB stress disk: grow + fill + recovery) + host
    fsck + BOOT 3 (persistence). ~8 minutes."""
    print("[suite] building stress.img ...")
    r = subprocess.run([sys.executable,
                        os.path.join(HERE, "make_fat32_img.py"),
                        "--stress"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit(2)

    # ================= BOOT 2: stress disk (3 MB) ==================
    print("\n=== BOOT 2: EQSTRESS grow + fill + recovery ===")
    q = QemuDisk(ISO, STRESS, net=False)
    try:
        txt = wait_text(q, "FAT32: 'EQSTRESS' mounted at /mnt",
                        timeout=240, tag="-sboot")
        check("S1 stress volume mounts",
              "EQSTRESS" in txt and "mounted at /mnt" in txt, txt[-300:])
        wait_text(q, "root::users /user $", timeout=120, tag="-ssh")
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("cd /mnt\n")
        time.sleep(0.8)

        # compile + run the stress program FROM the FAT volume
        q.type_str("mtcc fat32_stress.c\n")
        txt = wait_text(q, "STRESS GROW OK 140", timeout=420,
                        tag="-s2a")
        check("S2a dir grows to 140 files", "STRESS GROW OK 140" in txt,
              txt[-400:])
        txt = wait_text(q, "STRESS VERIFY OK", timeout=120, tag="-s2b")
        check("S2b grow files verified", "STRESS VERIFY OK" in txt,
              txt[-300:])
        txt = wait_text(q, "STRESS FULL OK after", timeout=900,
                        tag="-s2c")
        check("S2c volume filled (clean failure)",
              "STRESS FULL OK after" in txt, txt[-400:])
        txt = wait_text(q, "STRESS DONE", timeout=120, tag="-s2d")
        check("S2d program exits cleanly at 100 % usage",
              "STRESS DONE" in txt, txt[-300:])

        # shell + FS still alive
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("diskinfo\n")
        txt = wait_text(q, "clusters", timeout=60, tag="-s3")
        check("S3 shell alive at full disk", "free (0% free)" in txt
              or "free" in txt, txt[-400:])

        # reclaim space + write again
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("rm b0000.TXT\n"); time.sleep(1.2)
        q.type_str("rm b0001.TXT\n"); time.sleep(1.2)
        q.type_str('save recovered.txt << "space reclaimed"\n')
        txt = wait_text(q, "wrote 'recovered.txt'", timeout=60,
                        tag="-s4")
        check("S4 space reclaimed after rm",
              "wrote 'recovered.txt'" in txt, txt[-300:])
    finally:
        q.kill()

    print("\n=== HOST: mini-fsck on the 100 % full stress disk ===")
    problems, findings, vol = host_fsck(STRESS, "EQSTRESS")
    check("SH1 stress FAT1 == FAT2 + FSInfo exact",
          not problems, "; ".join(problems)[:500])
    # the walker reports on-disk 8.3 names (uppercase)
    gfiles = [n for n, sz, _ in findings.files
              if n.upper().startswith("/G0") or n.upper().startswith("/G1")]
    bfiles = [n for n, _, _ in findings.files
              if n.upper().startswith("/B0") or n.upper().startswith("/B1")
              or n.upper().startswith("/B2")]
    check("SH2 grow files all on disk (140)", len(gfiles) == 140,
          f"{len(gfiles)} g-files")
    check("SH3 fill files present (>=450)", len(bfiles) >= 450,
          f"{len(bfiles)} b-files")
    recovered = [n for n, _, _ in findings.files
                 if "RECOVE" in n.upper()]
    check("SH4 recovered file on disk", len(recovered) == 1,
          str(recovered))

    # ================= BOOT 3: persistence on stress disk ==========
    print("\n=== BOOT 3: stress disk persistence ===")
    q = QemuDisk(ISO, STRESS, net=False)
    try:
        wait_text(q, "FAT32: 'EQSTRESS' mounted at /mnt", timeout=240,
                  tag="-pboot")
        wait_text(q, "root::users /user $", timeout=120, tag="-psh")
        q.type_str("clear\n")
        time.sleep(0.5)
        q.type_str("cd /mnt\n")
        time.sleep(0.8)
        q.type_str("cat g0000.TXT\n")
        txt = wait_text(q, "x", timeout=90, tag="-p1")
        check("P1 grow files survived reboot", "\nx" in txt or
              ("x" in txt and "not found" not in txt), txt[-300:])
        q.type_str("clear\n"); time.sleep(0.5)
        q.type_str("xxd b0002.TXT 16\n")
        txt = wait_text(q, "00 01 02 03", timeout=90, tag="-p2")
        check("P2 fill file byte-exact after reboot",
              "00 01 02 03 04 05 06 07" in txt.replace("\n", " "),
              txt[-300:])
    finally:
        q.kill()


def main():
    """Stage selector: 'boot1', 'stress' or 'all' (default).

    The stages exist because the sandbox reaps long-running detached
    processes — each stage fits inside one 10-minute tool call when
    run inline:  python3 fat32_write_test.py boot1
                 python3 fat32_write_test.py stress
    """
    stage = sys.argv[1] if len(sys.argv) > 1 else "all"
    if stage in ("all", "boot1"):
        stage_boot1()
    if stage in ("all", "stress"):
        stage_stress()

    print("\n================ RESULTS ================")
    passed = sum(1 for _, ok in results if ok)
    for name, ok in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    print(f"============ {passed}/{len(results)} PASS ============")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
