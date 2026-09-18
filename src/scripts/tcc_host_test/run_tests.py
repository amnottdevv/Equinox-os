#!/usr/bin/env python3
"""
run_tests.py — test suite mtcc (Equinox OS TinyCC) di host.

Yang diuji:
  1. Mode A (compile & run in-memory, ala `run tcc.mrp file.c`)
  2. Mode B (compile -> .mrp -> validate -> run at base 0x500010, exactly
     seperti mrp_run loader kernel; ala `run tcc.mrp -c file.c` + `run out.mrp`)
     - output mode B HARUS identik mode A
     - the .mrp file is validated INDEPENDENTLY from Python (header + checksum
     rotate-xor - the same algorithm as mrp_format.h / mrp_pack.py)
  3. Negative test: bad source -> exit != 0 + a clear error message (no crash)
  4. Syscall number sync: the SYS_* definitions in tcc.cpp (host section)
     MUST be identical to kernel/library/header/syscall.h.
"""

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Layout: <repo>/scripts/tcc_host_test — the repo root is two levels up
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MORPHOS = ROOT   # <repo> = the Equinox OS root itself
MRP_USER = os.path.join(MORPHOS, "mrp_user")
SAMPLES = os.path.join(MORPHOS, "test")   # the .c samples live in test/
KERNEL_HDR = os.path.join(MORPHOS, "kernel", "library", "header")
MTCC_SRC = os.path.join(MORPHOS, "mtcc.c")   # canonical compiler source (v10.14)
BIN = os.environ.get("MTCC_HOST_BIN", os.path.join(HERE, "mtcc_host"))

PASS = 0
FAIL = 0


def ok(name):
    global PASS
    PASS += 1
    print(f"  PASS  {name}")


def bad(name, detail=""):
    global FAIL
    FAIL += 1
    print(f"  FAIL  {name}: {detail}")


def run_host(mode, path, stdin_data=None, timeout=60):
    return subprocess.run(
        [BIN, mode, path],
        input=stdin_data, capture_output=True, text=True, timeout=timeout,
        cwd=HERE,
    )


def mrp_checksum(data: bytes) -> int:
    """MUST be identical to mrp_checksum() in mrp_format.h (rotate-xor)."""
    checksum = 0x811C9DC5
    for byte in data:
        checksum = ((checksum << 5) | (checksum >> 27)) & 0xFFFFFFFF
        checksum ^= byte
    return checksum


def validate_mrp_file(path):
    """Validasi independen ala is_valid_mrp() kernel — return (ok, reason)."""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 18:
        return False, "too small"
    magic, version, entry, code_size, flags, checksum = struct.unpack(
        "<4sBIIBI", blob[:18])
    if magic != b"MRP1":
        return False, f"magic {magic!r}"
    if version != 1:
        return False, f"version {version}"
    if code_size != len(blob) - 18:
        return False, f"code_size {code_size} != {len(blob) - 18}"
    if entry >= code_size:
        return False, f"entry {entry} >= code_size"
    real = mrp_checksum(blob[18:])
    if real != checksum:
        return False, f"checksum {checksum:08x} != {real:08x}"
    return True, "OK"


# ---------------------------------------------------------------- build
print("== build mtcc_host ==")
r = subprocess.run(
    ["g++", "-std=gnu++17", "-O2", "-w",
     "-I", MRP_USER,
     "-o", BIN,
     os.path.join(HERE, "host_main.cpp"),
     os.path.join(HERE, "interp32.cpp")],
    capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr)
    sys.exit("[run_tests] build failed")
ok("build mtcc_host")

# ---------------------------------------------------------------- sync check
print("== syscall number sync: tcc.cpp vs kernel/syscall.h ==")
def parse_sys_defines(path):
    defs = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#define SYS_"):
                parts = line.split()
                if len(parts) >= 3:
                    try:
                        defs[parts[1]] = int(parts[2])
                    except ValueError:
                        pass
    return defs

kernel_defs = parse_sys_defines(os.path.join(KERNEL_HDR, "syscall.h"))
with open(MTCC_SRC) as f:
    src = f.read()
# Take the #define SYS_ block inside #ifdef MTCC_HOST_TEST (the host section)
host_defs = {}
in_host = False
for line in src.splitlines():
    if "#ifdef MTCC_HOST_TEST" in line:
        in_host = True
    if in_host and line.strip().startswith("#define SYS_"):
        parts = line.split()
        try:
            host_defs[parts[1]] = int(parts[2])
        except (ValueError, IndexError):
            pass
    if in_host and "#else" in line and "build .mrp" in line:
        break
mismatch = {}
for k, v in host_defs.items():
    if k not in kernel_defs:
        mismatch[k] = (v, "<missing in syscall.h>")
    elif kernel_defs[k] != v:
        mismatch[k] = (v, kernel_defs[k])
if mismatch:
    bad("syscall sync", str(mismatch))
else:
    ok(f"syscall sync ({len(host_defs)} SYS_* definitions host == kernel)")

# ---------------------------------------------------------------- cases
CASES = [
    # (name, file, stdin, expected_stdout)
    ("hello.c", os.path.join(SAMPLES, "hello.c"), None,
     "Hello from C compiled inside Equinox OS!\n"
     "42\n144\ncounter=10\nsum=30\n"
     "mtcc runs in Equinox OS!\n"
     "len(greeting)=25\n"
     "255\n1024\n3\n1\n25\n65\n"
     "EXIT=0\n"),
    ("primes.c", os.path.join(SAMPLES, "primes.c"), None,
     "2 3 5 7 11 13 17 19 23 29 31 37 41 43 47 53 59 61 67 71 73 79 83 89 97 \n"
     "count: 25\n"
     "EXIT=25\n"),
    ("guess.c", os.path.join(SAMPLES, "guess.c"), "Morph\n5\n9\n7\n",
     "What is your name? Hello, Morph!\n"
     "Guess a number 1..10 (press enter after each guess):\n"
     "too small\n"
     "too big\n"
     "Correct after 3 guesses. Morph wins!\n"
     "EXIT=0\n"),
    ("edge.c", os.path.join(SAMPLES, "edge.c"), None,
     "120\n14\nedge ok\n4\nand1\nand2\nor1\n111\n9\n7\n8\n"
     "6\n6\n7\n6\n12\nnegok\n9\npos\nabcd\nAbcd\n10\n"
     "EXIT=0\n"),
    ("exec_test.c", os.path.join(SAMPLES, "exec_test.c"), None,
     "exec_test: regression audit V3 #1 (nested exec)\n"
     "exec_test: calling exec(hello.mrp) from inside a program...\n"
     "exec_test: returned -9 SYS_EBUSY - refused, no crash. FIX OK\n"
     "exec_test: the calling code is still alive. FIX V3-1 OK\n"
     "EXIT=0\n"),
    # V4 regression: recycled-arena bug (gvar without NUL, 2nd+ run).
    # pl*.c = minimal bisect from primes.c line 7 - runs together with
    # os_alloc() pre-filled with the dirty pattern 0xA5 (host mode) so the
    # "depends on clean memory" bug class can never pass again.
    ("arr.c", os.path.join(SAMPLES, "arr.c"), None, "5\nEXIT=0\n"),
    ("varidx.c", os.path.join(SAMPLES, "varidx.c"), None, "7\nEXIT=0\n"),
    ("pl1.c", os.path.join(SAMPLES, "pl1.c"), None, "1\nEXIT=0\n"),
    ("pl2.c", os.path.join(SAMPLES, "pl2.c"), None, "1\nEXIT=0\n"),
    ("pl3.c", os.path.join(SAMPLES, "pl3.c"), None, "1\nEXIT=0\n"),
    ("pl4.c", os.path.join(SAMPLES, "pl4.c"), None, "1\nEXIT=0\n"),
    ("pl5.c", os.path.join(SAMPLES, "pl5.c"), None, "1\nEXIT=0\n"),
    # Morph.h file API regression: create / overwrite / size / read_all /
    # fd path / error paths (new syscalls 17-19 + builtins file_*).
    ("morphio.c", os.path.join(SAMPLES, "morphio.c"), None,
     "1 exists  : 0\n"
     "2 write   : 0\n"
     "3 exists  : 1\n"
     "4 size    : 11\n"
     "5 readall : 11\n"
     "6 content : hello world\n"
     "7 edit    : 0\n"
     "8 size2   : 14\n"
     "9 read2   : 14\n"
     "10 streq  : 1\n"
     "11 open   : 3\n"
     "12 read5  : 5\n"
     "13 head   : hello\n"
     "14 close  : 0\n"
     "15 miss   : -3\n"
     "16 rdmiss : -3\n"
     "EXIT=0\n"),
    # Morph.h game API regression: fb_info struct fill (array decay),
    # put_pixel, packed fill_rect ABI, clipping safety, pollkey (no
    # key), mouse_state (center), speaker on/off (new syscalls 20-25).
    # Host fb = 320x240x32 pitch 1280 (interp32 fake VESA).
    ("morphgfx.c", os.path.join(SAMPLES, "morphgfx.c"), None,
     "1 fbinfo  : 0\n"
     "2 avail   : 1\n"
     "3 dims    : 320x240 bpp=32 pitch=1280\n"
     "4 putpix  : 0\n"
     "5 fillrect: 0\n"
     "6 clip    : 0\n"
     "7 pollkey : 0\n"
     "8 mouse   : 0 x=160 y=120 b=0\n"
     "9 tone    : 440Hz\n"
     "10 quiet  : ok\n"
     "SUM dims=320x240x32 avail=1\n"
     "GFX DONE\n"
     "EXIT=0\n"),
    # v10.5 ring buffer regression: note queue capacity (64 -> EBUSY),
    # EINVAL on ms=0, manual speaker flush, pollkey idle. Host has no
    # playback timer so the queue never drains: exactly 64/16. In-OS
    # one timer tick may pop a note mid-loop (64..65) — the QEMU
    # harness asserts the range there.
    ("ringtest.c", os.path.join(SAMPLES, "ringtest.c"), None,
     "1 queued : 64 busy=16\n"
     "2 einval : -6\n"
     "3 manual : ok\n"
     "4 pollkey: 0\n"
     "SUM queued=64 busy=16 einval=-6\n"
     "RING DONE\n"
     "EXIT=0\n"),
    # v10.8 libc prelude: #include <morph.h> + full libc regression.
    # The printf lines exercise syscall 28 (max 3 conversions + %c/%%).
    ("libc.c", os.path.join(SAMPLES, "libc.c"), None,
     "ok 1 define\n"
     "ok 2 strlen\n"
     "ok 3 strcmp\n"
     "ok 4 strncmp\n"
     "ok 5 strcpy\n"
     "ok 6 strncpy-term\n"
     "ok 7 strcat\n"
     "ok 8 strncat\n"
     "ok 9 strchr\n"
     "ok 10 strrchr\n"
     "ok 11 strstr\n"
     "ok 12 strstr-miss\n"
     "ok 13 memcpy\n"
     "ok 14 memmove\n"
     "ok 15 memcmp\n"
     "ok 16 memchr\n"
     "ok 17 atoi\n"
     "ok 18 strtol-hex\n"
     "ok 19 strtol-autodetect\n"
     "ok 20 strtol-octal\n"
     "ok 21 strtol-endp\n"
     "ok 22 strtol-nodigit\n"
     "ok 23 itoa\n"
     "ok 24 itoa-hex\n"
     "ok 25 malloc\n"
     "ok 26 malloc-write\n"
     "ok 27 heap-coalesce\n"
     "ok 28 heap-bigwrite\n"
     "ok 29 calloc-zero\n"
     "ok 30 realloc-copy\n"
     "printf-check d=-42 s=str x=beef\n"
     "printf-c c=Q pct=% neg=-7\n"
     "printf-width 00042|7     |\n"
     "printf-one-arg works\n"
     "ok 31 snprintf-trunc\n"
     "ok 32 snprintf-fmt\n"
     "ok 33 sprintf\n"
     "ok 34 fopen-w\n"
     "ok 35 fwrite\n"
     "ok 36 fclose-w\n"
     "ok 37 file-created\n"
     "ok 38 fopen-r\n"
     "ok 39 fread-all\n"
     "ok 40 fseek-set\n"
     "ok 41 ftell\n"
     "ok 42 fread-after-seek\n"
     "ok 43 fseek-cur-neg\n"
     "ok 44 append-mode\n"
     "ok 45 w-seek-truncate\n"
     "ok 46 open-fd\n"
     "ok 47 lseek-set\n"
     "ok 48 lseek-end\n"
     "ok 49 read-eof\n"
     "ok 50 qsort_int\n"
     "ok 51 qsort_str\n"
     "ok 52 time\n"
     "ok 53 getenv-null\n"
     "ok 54 ring3-selfcheck\n"
     "SUM stages=54 fails=0\n"
     "LIBC ALL PASS\n"
     "EXIT=0\n"),
]

print("== mode A: compile & run in-memory ==")
for name, path, sin, expected in CASES:
    r = run_host("run", path, sin)
    if r.returncode != 0:
        bad(f"A:{name}", f"rc={r.returncode} stderr={r.stderr[-300:]}")
    elif r.stdout != expected:
        bad(f"A:{name}", f"output mismatch:\n--- got ---\n{r.stdout}\n--- expected ---\n{expected}")
    else:
        ok(f"A:{name}")

print("== mode B: compile -> .mrp -> validate -> run at 0x500010 ==")
for name, path, sin, expected in CASES:
    r = run_host("c", path, sin)
    if r.returncode != 0:
        bad(f"B:{name}", f"rc={r.returncode} stderr={r.stderr[-300:]}")
        continue
    if r.stdout != expected:
        bad(f"B:{name}", f"output mismatch:\n--- got ---\n{r.stdout}\n--- expected ---\n{expected}")
        continue
    # the .mrp file must also exist & be valid independently
    base = os.path.basename(path)
    mrp = os.path.join(HERE, base.rsplit(".", 1)[0] + ".mrp")
    if not os.path.exists(mrp):
        bad(f"B:{name}", f"file {mrp} missing")
        continue
    vok, why = validate_mrp_file(mrp)
    if not vok:
        bad(f"B:{name}", f".mrp invalid: {why}")
    else:
        ok(f"B:{name} (+ .mrp valid, {os.path.getsize(mrp)} byte)")

print("== negative test: bad source must fail cleanly ==")
r = run_host("run", os.path.join(SAMPLES, "negtest.c"))
if r.returncode == 0:
    bad("negtest", "should have failed to compile, but exited 0")
elif "error" not in r.stderr:
    bad("negtest", f"stderr without an error message: {r.stderr[-200:]}")
else:
    ok(f"negtest (rc={r.returncode}, message: {r.stderr.strip().splitlines()[0][:70]})")

print(f"\n== RESULT: {PASS} PASS, {FAIL} FAIL ==")
sys.exit(1 if FAIL else 0)
