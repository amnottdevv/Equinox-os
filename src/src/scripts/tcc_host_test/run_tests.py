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
  5. `-format elf` — a static ELF32 is written, its header is parsed HERE
     in Python (independently of mtcc's own checker) and the program is
     executed at the ELF link base 0x01000000
  6. CLI flags — -multiple-files, a comma-separated file list, -q (silent),
     an unknown option (reported, never treated as a file) and the
     `[ERROR] <file>:<line>: <message>` diagnostic style
  7. `make` mode — a two-directive .ruf recipe drives the shared recipe
     engine (echo/src/out) and produces a valid .mrp
  8. `make` v4 — `multiple_file = True` (satu program dari daftar file
     ATAU direktori yang di-walk), baris `job … from … to … format elf`
     (satu baris = N sumber -> SATU keluaran), `set key = value`
     (tulis-.ecf yang HANYA dieksekusi setelah build sukses), plus
     regresi v3 (resep `key:=value` + walk + exclude tetap 3 job)
"""

import os
import struct
import subprocess
import sys
import tempfile

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


def run_host(mode, path, stdin_data=None, timeout=60, args=None):
    """`path` may be a single source or a LIST of sources — mtcc links a file
    list into ONE program (shared symbol table + one fixup list).
    `args` carries CLI options placed BEFORE the sources (-format, -o, -q, …)
    — the same flag grammar as the in-OS driver."""
    paths = path if isinstance(path, (list, tuple)) else [path]
    return subprocess.run(
        [BIN, mode] + list(args or []) + list(paths),
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
    ("struct.c", os.path.join(SAMPLES, "struct.c"), None,
     "p=(3,7)\n"
     "pp->x=42\n"
     "q=(42,7)\n"
     "r=(1,2)\n"
     "box=0,0..10,20\n"
     "sum=60\n"
     "m.c[0]=68\n"
     "dx=4\n"
     "rc=9,111,222\n"
     "EXIT=0\n"),
    ("swenum.c", os.path.join(SAMPLES, "swenum.c"), None,
     "RED=0 GREEN=5 BLUE=6\n"
     "sw=222\n"
     "g1=10\n"
     "g2=20\n"
     "g3=35\n"
     "g9=-1\n"
     "weekend=0\n"
     "EXIT=0\n"),
    # Stage 5: `sizeof` (type-name, variable, and constant contexts) and
    # `static` locals (data-area storage: constant init once, zero otherwise,
    # survives between calls). Every number is derivable from the mtcc type
    # rules — see the header comment of lang05.c.
    ("lang05.c", os.path.join(SAMPLES, "lang05.c"), None,
     "sizeof(char)=1\n"
     "sizeof(int)=4\n"
     "sizeof(struct Point)=8\n"
     "sizeof(struct Rec)=12\n"
     "sizeof(union Mix)=4\n"
     "sizeof(struct Small)=4\n"
     "sizeof(Pt)=8\n"
     "sizeof(struct Point*)=4\n"
     "sizeof(Name)=4\n"
     "sizeof(arr)=20\n"
     "sizeof(buf)=10\n"
     "sizeof(one)=8\n"
     "probe=4 122\n"
     "sizeof(gwords)=16\n"
     "csz=8\n"
     "sp=11,22\n"
     "tbl=65,0,0\n"
     "sizeof(sp)=8 sizeof(tbl)=4\n"
     "tick=101,102\n"
     "zeroed=7,14\n"
     "deep=6\n"
     "plain=7\n"
     "EXIT=0\n"),
    # Stage 3: struct/union INITIALIZER `{...}` — global, local, nested,
    # array-of-struct, union (first member), char fields, char* = "lit".
    ("sinit.c", os.path.join(SAMPLES, "sinit.c"), None,
     "g=10,20 p=5,6 l=2,3,7 loc=7,10,11 arr=1,3 u=99\n"
     "mix=65,7,66,8 m=67,9,68,10 hp=hi3\n"
     "EXIT=0\n"),
    # Stage 3: MULTI-FILE link — two .c files (+ one shared .h) compiled into
    # ONE program: cross-file calls via forward fixups, `extern` global with a
    # deferred data fixup, relative #include "mf_shared.h", and the <morph.h>
    # include guard holding across the file list (no "duplicate global").
    ("multifile.c", [os.path.join(SAMPLES, "mf_main.c"),
                     os.path.join(SAMPLES, "mf_helper.c")], None,
     "dx=6 total=42 done\n"
     "EXIT=0\n"),
    # ...and the REVERSED order: definitions come first, so mf_main.c's
    # header prototypes re-declare already-defined functions. A prototype
    # after a definition must be accepted (only a second BODY is an error).
    ("multifile_rev.c", [os.path.join(SAMPLES, "mf_helper.c"),
                         os.path.join(SAMPLES, "mf_main.c")], None,
     "dx=6 total=42 done\n"
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
    # extended 0.5 syscall self-test: SYS_MMAP/SYS_SBRK (the 0x2000000
    # window), SYS_FORK (depth-first snapshot: the child's writes must
    # never reach the parent = COW isolation), SYS_WAIT, and
    # SYS_SOCKET/SYS_NET (the host harness has no server -> the
    # documented SKIP path). The SAME file is the in-OS test:
    # scripts/ex2_test.py boots QEMU and runs
    # `mtcc /equinox/tools/ex2.c` there, against the real kernel.
    ("ex2.c", os.path.join(MORPHOS, "tools_user", "ex2.c"), None,
     "== ex2: fork / sbrk / mmap / socket ==\n"
     "[PASS] mmap returned a window\n"
     "[PASS] mmap window writable (page faulted in)\n"
     "[PASS] sbrk(0) reports the program break\n"
     "[PASS] sbrk(8192) returns the OLD break\n"
     "[PASS] break advanced by 8192\n"
     "[PASS] heap page inside the break is writable\n"
     "child: pid=2 mmap=c0de1234\n"
     "child: wrote 87654321\n"
     "parent: child=2 reaped=2 status=0 mmap=c0de1234\n"
     "[PASS] wait() returned the forked child\n"
     "[PASS] child exit status is 0\n"
     "[PASS] parent page unchanged (COW isolation)\n"
     "[PASS] socket(AF_INET, SOCK_STREAM) opened\n"
     "[SKIP] net() connect refused (-11) - no host server\n"
     "ex2 RESULT fails=0\n"
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
    first = path[0] if isinstance(path, (list, tuple)) else path
    base = os.path.basename(first)
    mrp = os.path.join(HERE, base.rsplit(".", 1)[0] + ".mrp")
    if not os.path.exists(mrp):
        bad(f"B:{name}", f"file {mrp} missing")
        continue
    vok, why = validate_mrp_file(mrp)
    if not vok:
        bad(f"B:{name}", f".mrp invalid: {why}")
    else:
        ok(f"B:{name} (+ .mrp valid, {os.path.getsize(mrp)} byte)")

# v0.3.2: the libc is REAL FILES in libc/ (RAMFS /equinox/libc in-OS).
# Every module check-compiles standalone (mtcc --lib semantics: no
# main, bare-ret entry stub) through its explicit dependency includes.
# ---------------------------------------------------------------------------
# v0.4 — `-format elf` (static ELF32) + the shared CLI flag grammar.
# The ELF is parsed HERE in Python, independently of mtcc's own
# mtcc_elf_check(), so an emitter bug cannot validate its own output.
# ---------------------------------------------------------------------------
print("== format elf: -c writes a valid static ELF32 that runs ==")
ELF_BASE = 0x01000000
elf_out = os.path.join(HERE, "elf_check.elf")
if os.path.exists(elf_out):
    os.remove(elf_out)
r = run_host("c", os.path.join(SAMPLES, "hello.c"),
             args=["-format", "elf", "-o", os.path.join(HERE, "elf_check")])
if r.returncode != 0:
    bad("elf:c", f"rc={r.returncode} stderr={r.stderr[-300:]}")
elif not os.path.exists(elf_out):
    bad("elf:c", f"{elf_out} was not written")
else:
    b = open(elf_out, "rb").read()
    err = []
    if b[:4] != b"\x7fELF": err.append("magic")
    if b[4] != 1 or b[5] != 1: err.append("class/endian")
    if struct.unpack_from("<H", b, 16)[0] != 2: err.append("not ET_EXEC")
    if struct.unpack_from("<H", b, 18)[0] != 3: err.append("not EM_386")
    if struct.unpack_from("<I", b, 24)[0] != ELF_BASE: err.append("entry")
    if struct.unpack_from("<I", b, 28)[0] != 52: err.append("phoff")
    if struct.unpack_from("<H", b, 44)[0] != 1: err.append("phnum")
    ph = 52
    p_type, p_off, p_va, p_pa, p_fs, p_ms, p_fl, p_al = struct.unpack_from(
        "<IIIIIIII", b, ph)
    if p_type != 1: err.append("p_type")
    if p_off != 84: err.append("p_offset")
    if p_va != ELF_BASE: err.append("p_vaddr")
    if p_fs != len(b) - 84: err.append("p_filesz")
    if p_ms != p_fs: err.append("p_memsz")
    if p_fl != 7: err.append("p_flags")
    if not (0x00800000 <= ELF_BASE < 0x02000000): err.append("window")
    if "EXIT=0" not in r.stdout: err.append("did not run")
    if err:
        bad("elf:c", f"{', '.join(err)} ({len(b)} byte)")
    else:
        ok(f"elf:c (+ELF32 valid, {len(b)} byte, run at 0x{ELF_BASE:x})")
os.remove(elf_out) if os.path.exists(elf_out) else None

print("== CLI flags: -q, -multiple-files, commas, unknown option ==")
mf1 = os.path.join(SAMPLES, "mf_main.c")
mf2 = os.path.join(SAMPLES, "mf_helper.c")

r = run_host("run", [mf1, mf2], args=["-multiple-files"])
if r.returncode == 0 and "EXIT=0" in r.stdout and "dx=6 total=42" in r.stdout:
    ok("cli:-multiple-files links two files")
else:
    bad("cli:-multiple-files", f"rc={r.returncode} {r.stderr[-200:]}")

r = run_host("run", [mf1 + "," + mf2])
if r.returncode == 0 and "dx=6 total=42" in r.stdout:
    ok("cli:comma-separated file list")
else:
    bad("cli:commas", f"rc={r.returncode} {r.stderr[-200:]}")

r = run_host("run", os.path.join(SAMPLES, "hello.c"), args=["-q"])
if r.returncode == 0 and r.stderr.strip() == "" and "EXIT=0" in r.stdout:
    ok("cli:-q silences all chatter (errors still print)")
else:
    bad("cli:-q", f"rc={r.returncode} stderr={r.stderr[-200:]!r}")

r = run_host("run", os.path.join(SAMPLES, "hello.c"), args=["--bogus"])
if r.returncode != 0 and "unknown option" in r.stderr and "[ERROR]" in r.stderr:
    ok("cli:unknown option is reported, not treated as a file")
else:
    bad("cli:unknown option", f"rc={r.returncode} {r.stderr[-200:]!r}")

r = run_host("run", os.path.join(SAMPLES, "negtest.c"))
if r.returncode != 0 and "[ERROR]" in r.stderr and "negtest.c:" in r.stderr:
    ok("cli:[ERROR] file:line: message")
else:
    bad("cli:[ERROR]", f"rc={r.returncode} {r.stderr[-200:]!r}")

# ---------------------------------------------------------------------------
# make mode — `mtcc_host make <file.ruf>` drives the SAME recipe engine as
# the in-OS `mtcc -make` (it was the one CLI mode the suite never covered).
# A two-directive recipe (echo/src/out) is enough to prove the walk, the
# job loop, the [COMPILE]/[MAKE] tags and the produced .mrp.
# ---------------------------------------------------------------------------
print("== make mode: a .ruf recipe builds one job end-to-end ==")
import shutil
MK_TMP = os.path.join(HERE, "tmp_make_case")
MK_SRC = os.path.join(MK_TMP, "src")
MK_OUT = os.path.join(MK_TMP, "out")
shutil.rmtree(MK_TMP, ignore_errors=True)
os.makedirs(MK_SRC)
shutil.copy(os.path.join(SAMPLES, "hello.c"), os.path.join(MK_SRC, "hello.c"))
MK_RUF = os.path.join(MK_TMP, "t.ruf")
with open(MK_RUF, "w") as fh:
    fh.write(f'echo "mini ruf build"\nsrc {MK_SRC}\nout {MK_OUT}\n')
r = run_host("make", MK_RUF)
mk_prod = os.path.join(MK_OUT, "hello.mrp")
mk_err = []
if r.returncode != 0:
    mk_err.append(f"rc={r.returncode}")
if "EXIT=0" not in r.stdout:
    mk_err.append("no EXIT=0")
if "[MAKE]" not in r.stderr:
    mk_err.append("no [MAKE] tag")
if "done: 1 ok, 0 failed (1 job(s))" not in r.stderr:
    mk_err.append(f"summary: {r.stderr.strip().splitlines()[-2:]}")
if "[COMPILE]" not in r.stderr:
    mk_err.append("no per-job [COMPILE] line")
if not os.path.exists(mk_prod):
    mk_err.append("product missing")
else:
    vok, why = validate_mrp_file(mk_prod)
    if not vok:
        mk_err.append(f"product invalid: {why}")
if mk_err:
    bad("make:recipe", "; ".join(mk_err))
else:
    ok(f"make:recipe ([MAKE] + {os.path.basename(mk_prod)} valid)")
shutil.rmtree(MK_TMP, ignore_errors=True)

# ---------------------------------------------------------------------------
# make mode v4 — the multi-file recipe dialect:
#   multiple_file = True     semua sumber `src` -> SATU program
#   job <n> from a.c & b.c   SATU baris = N sumber -> SATU keluaran,
#     to out [format elf]    dengan format per job
#   set key = value          tulis-.ecf, hanya setelah build sukses
# Plus dua kasus regresi v3 (resep `key:=value` + walk + exclude, dan
# direktif tak dikenal). Semua kasus menulis resepnya sendiri ke
# tmp_make_v4/.
# ---------------------------------------------------------------------------
print("== make v4: multiple_file / job+format / set-ecf ==")
V4_TMP = os.path.join(HERE, "tmp_make_v4")
shutil.rmtree(V4_TMP, ignore_errors=True)
os.makedirs(V4_TMP)


def run_ruf(name, text):
    p = os.path.join(V4_TMP, name)
    with open(p, "w") as fh:
        fh.write(text)
    return run_host("make", p)


def elf_header_ok(path, base=ELF_BASE):
    """Validasi ELF32 secara independen (mirip kasus elf:c)."""
    try:
        with open(path, "rb") as fh:
            b = fh.read()
    except OSError as exc:
        return False, str(exc)
    err = []
    if len(b) < 84:                       err.append("too short")
    if b[:4] != b"\x7fELF":               err.append("magic")
    if b[4] != 1:                         err.append("not ELF32")
    if b[5] != 1:                         err.append("not little-endian")
    if struct.unpack_from("<H", b, 16)[0] != 2: err.append("not ET_EXEC")
    if struct.unpack_from("<H", b, 18)[0] != 3: err.append("not EM_386")
    if struct.unpack_from("<I", b, 24)[0] != base: err.append("entry")
    phoff = struct.unpack_from("<I", b, 28)[0]
    if struct.unpack_from("<H", b, 42)[0] != 32: err.append("phentsize")
    if struct.unpack_from("<H", b, 44)[0] != 1: err.append("phnum != 1")
    if phoff != 52:                       err.append("phoff")
    if struct.unpack_from("<I", b, phoff)[0] != 1: err.append("not PT_LOAD")
    if struct.unpack_from("<I", b, phoff + 8)[0] != base: err.append("p_vaddr")
    if struct.unpack_from("<I", b, phoff + 12)[0] != base: err.append("p_paddr")
    return (not err), ", ".join(err)


# --- M1: mode A + daftar file eksplisit -> SATU program ter-link -----------
mf_dir = os.path.join(V4_TMP, "mf")
os.makedirs(mf_dir)
for f in ("mf_main.c", "mf_helper.c", "mf_shared.h"):
    shutil.copy(os.path.join(SAMPLES, f), os.path.join(mf_dir, f))
out1 = os.path.join(V4_TMP, "out1")
r = run_ruf("m1.ruf",
            f"name := mfprog\n"
            f"multiple_file = True\n"
            f"out {out1}\n"
            f"src {mf_dir}/mf_main.c, {mf_dir}/mf_helper.c\n")
mk_err = []
if r.returncode != 0:
    mk_err.append(f"rc={r.returncode} {r.stderr[-200:]}")
if "done: 1 ok, 0 failed (1 job(s))" not in r.stderr:
    mk_err.append("summary")
if "[COMPILE]" not in r.stderr or "mf_main.c + " not in r.stderr:
    mk_err.append(f"no multi-file [COMPILE]: {r.stderr.strip().splitlines()[:3]}")
m1_prod = os.path.join(out1, "mfprog.mrp")
if not os.path.exists(m1_prod):
    mk_err.append("product missing")
else:
    vok, why = validate_mrp_file(m1_prod)
    if not vok:
        mk_err.append(f"product invalid: {why}")
if mk_err:
    bad("makev4:multiple-files-list", "; ".join(mk_err))
else:
    ok("makev4: multiple_file + explicit list -> ONE mfprog.mrp")

# --- M2: mode A + direktori yang di-walk juga SATU program ----------------
out2 = os.path.join(V4_TMP, "out2")
r = run_ruf("m2.ruf",
            f"multiple_file = True\n"
            f"out {out2}\n"
            f"src {mf_dir}\n")
mk_err = []
if r.returncode != 0:
    mk_err.append(f"rc={r.returncode} {r.stderr[-200:]}")
if "done: 1 ok, 0 failed (1 job(s))" not in r.stderr:
    mk_err.append("summary (walk must fold into ONE job)")
for need in ("mf_main.c", "mf_helper.c"):
    if need not in r.stderr:
        mk_err.append(f"{need} not in [COMPILE]")
prods = [f for f in os.listdir(out2) if f.endswith(".mrp")] if os.path.isdir(out2) else []
if len(prods) != 1:
    mk_err.append(f"expected 1 product, got {prods}")
else:
    vok, why = validate_mrp_file(os.path.join(out2, prods[0]))
    if not vok:
        mk_err.append(f"product invalid: {why}")
if mk_err:
    bad("makev4:multiple-files-walk", "; ".join(mk_err))
else:
    ok(f"makev4: multiple_file + walked dir -> one {prods[0]}")

# --- M3: baris `job` dengan `format elf` ----------------------------------
out3 = os.path.join(V4_TMP, "out3")
r = run_ruf("m3.ruf",
            f"job hello from {SAMPLES}/hello.c to {out3}/hello_prog format elf\n")
mk_err = []
if r.returncode != 0:
    mk_err.append(f"rc={r.returncode} {r.stderr[-200:]}")
if "done: 1 ok, 0 failed (1 job(s))" not in r.stderr:
    mk_err.append("summary")
m3_prod = os.path.join(out3, "hello_prog.elf")
if not os.path.exists(m3_prod):
    mk_err.append("hello_prog.elf missing")
else:
    eok, why = elf_header_ok(m3_prod)
    if not eok:
        mk_err.append(f"bad ELF: {why}")
if mk_err:
    bad("makev4:job-format-elf", "; ".join(mk_err))
else:
    ok("makev4: `job … to … format elf` writes a valid ELF32")

# --- M4: ekstensi `to` bertentangan dengan format = error, bukan diam -----
r = run_ruf("m4.ruf",
            f"job bad from {SAMPLES}/hello.c to {V4_TMP}/clash.mrp format elf\n")
mk_err = []
if r.returncode == 0:
    mk_err.append("rc=0 (a conflict must fail)")
if "conflicts with format" not in r.stderr:
    mk_err.append(f"no conflict message: {r.stderr.strip().splitlines()[-3:]}")
if "done: 0 ok, 1 failed" not in r.stderr:
    mk_err.append("summary")
if os.path.exists(os.path.join(V4_TMP, "clash.mrp")):
    mk_err.append("conflicting product was written")
if mk_err:
    bad("makev4:job-ext-conflict", "; ".join(mk_err))
else:
    ok("makev4: `to x.mrp format elf` fails loudly, writes nothing")

# --- M5: `set` + `ecf` — merge INI yang tidak merusak key lain ------------
ecf1 = os.path.join(V4_TMP, "test.ecf")
ECF1_SEED = ("# generated by the suite\n"
             "[net]\n"
             "driver = e1000\n"
             "[eggkg]\n"
             "local = /old\n"
             "keep = me\n")
with open(ecf1, "w") as fh:
    fh.write(ECF1_SEED)
out5 = os.path.join(V4_TMP, "out5")
r = run_ruf("m5.ruf",
            f"multiple_file = True\n"
            f"out {out5}\n"
            f"src {SAMPLES}/hello.c\n"
            f"ecf {ecf1}\n"
            f"set eggkg.local = {out5}\n"
            f"set path_ecf.local_file = $buildir\n")
mk_err = []
if r.returncode != 0:
    mk_err.append(f"rc={r.returncode} {r.stderr[-200:]}")
if "set eggkg.local" not in r.stderr or "ok" not in r.stderr:
    mk_err.append("no [MAKE] set … ok line")
try:
    with open(ecf1) as fh:
        new = fh.read()
except OSError as exc:
    new = ""
    mk_err.append(str(exc))
for want in (f"local = {out5}", "# generated by the suite",
             "driver = e1000", "keep = me",
             f"path_ecf.local_file = {out5}"):
    if want not in new:
        mk_err.append(f"missing: {want!r}")
if "/old" in new:
    mk_err.append("old value of eggkg.local survived")
if new.count("[net]") != 1 or new.count("[eggkg]") != 1:
    mk_err.append(f"sections damaged: {new!r}")
if mk_err:
    bad("makev4:set-ecf", "; ".join(mk_err))
else:
    ok("makev4: set merges into .ecf (target replaced, rest untouched)")

# --- M6: build gagal -> `set` TIDAK menyentuh konfigurasi -----------------
ecf2 = os.path.join(V4_TMP, "failing.ecf")
with open(ecf2, "w") as fh:
    fh.write(ECF1_SEED)
r = run_ruf("m6.ruf",
            f"multiple_file = True\n"
            f"src {SAMPLES}/negtest.c\n"
            f"ecf {ecf2}\n"
            f"set eggkg.local = must-not-appear\n")
mk_err = []
if r.returncode == 0:
    mk_err.append("rc=0 (a failing build must fail the recipe)")
with open(ecf2) as fh:
    after = fh.read()
if after != ECF1_SEED:
    mk_err.append(f"ecf changed after a FAILED build: {after!r}")
if "must-not-appear" in after:
    mk_err.append("set ran despite failures")
if mk_err:
    bad("makev4:set-gated-on-success", "; ".join(mk_err))
else:
    ok("makev4: `set` only runs after a fully successful build")

# --- M7: `format elf` global berlaku untuk pekerjaan walk (v3) ------------
out7 = os.path.join(V4_TMP, "out7")
hello_dir = os.path.join(V4_TMP, "hello_only")
os.makedirs(hello_dir)
shutil.copy(os.path.join(SAMPLES, "hello.c"), os.path.join(hello_dir, "hello.c"))
r = run_ruf("m7.ruf", f"format elf\nsrc {hello_dir}\nout {out7}\n")
mk_err = []
if r.returncode != 0:
    mk_err.append(f"rc={r.returncode} {r.stderr[-200:]}")
m7_prod = os.path.join(out7, "hello.elf")
if not os.path.exists(m7_prod):
    mk_err.append("hello.elf missing")
else:
    eok, why = elf_header_ok(m7_prod)
    if not eok:
        mk_err.append(f"bad ELF: {why}")
if mk_err:
    bad("makev4:global-format-elf", "; ".join(mk_err))
else:
    ok("makev4: global `format elf` — a v3 walk writes .elf")

# --- M8: daftar .c tanpa multiple_file = penjelasan yang jelas ------------
r = run_ruf("m8.ruf", f"src {SAMPLES}/hello.c\n")
mk_err = []
if r.returncode == 0:
    mk_err.append("rc=0")
if "multiple_file = True" not in r.stderr:
    mk_err.append(f"no guidance: {r.stderr.strip().splitlines()[-2:]}")
if mk_err:
    bad("makev4:list-without-flag", "; ".join(mk_err))
else:
    ok("makev4: .c list without multiple_file is rejected with a fix hint")

# --- M9/M10: regresi v3 — resep LAMA harus berperilaku persis sama --------
# (fixtures_make/ adalah resep v3 asli: `key:=value`, walk rekursif,
#  `exclude`, SATU .c = SATU job — tidak boleh terpengaruh v4.)
v3_dir = os.path.join(V4_TMP, "v3")
for sub in ("tools", "libc", "games"):
    shutil.copytree(os.path.join(HERE, "fixtures_make", sub),
                    os.path.join(v3_dir, sub))
r = run_ruf("m9.ruf",
            f'# fixture mtcc -make — bentuk key:=value ala sketch\n'
            f'echo "memulai compiling"\n'
            f'v3dir := {v3_dir}\n'          # $var di nilai src (ala `src $pathsrc`)
            f'src := $v3dir\n'
            f'exclude := games\n')
mk_err = []
if r.returncode != 0:
    mk_err.append(f"rc={r.returncode} {r.stderr[-200:]}")
if "done: 3 ok, 0 failed (3 job(s))" not in r.stderr:
    mk_err.append(f"summary: {r.stderr.strip().splitlines()[-2:]}")
v3_prods = [os.path.join(v3_dir, "tools", "hello.mrp"),
            os.path.join(v3_dir, "tools", "sumsq.mrp"),
            os.path.join(v3_dir, "libc", "mkmath.mrp")]
for p in v3_prods:
    if not os.path.exists(p):
        mk_err.append(f"missing {os.path.relpath(p, v3_dir)}")
    else:
        vok, why = validate_mrp_file(p)
        if not vok:
            mk_err.append(f"invalid {p}: {why}")
if os.path.exists(os.path.join(v3_dir, "games", "snake_stub.mrp")):
    mk_err.append("exclude was ignored")
if mk_err:
    bad("makev3:recipe-unchanged", "; ".join(mk_err))
else:
    ok("makev4: v3 recipe (`key:=value`, `src $var`, walk, exclude) -> 3 jobs")

r = run_ruf("m10.ruf", "bogus_directive x\n")
mk_err = []
if r.returncode == 0:
    mk_err.append("rc=0")
if "unknown directive" not in r.stderr:
    mk_err.append(f"no message: {r.stderr.strip().splitlines()[-2:]}")
if mk_err:
    bad("makev3:bad-directive", "; ".join(mk_err))
else:
    ok("makev4: unknown directive still fails loudly (v3 error path)")

shutil.rmtree(V4_TMP, ignore_errors=True)

print("== lib mode: every libc module check-compiles standalone ==")
LIBC_DIR = os.path.join(MORPHOS, "libc")
for fname in sorted(os.listdir(LIBC_DIR)):
    if not fname.endswith(".c"):
        continue
    r = run_host("lib", os.path.join(LIBC_DIR, fname))
    if r.returncode != 0 or "EXIT=0" not in r.stdout:
        bad(f"lib:{fname}", f"rc={r.returncode} stderr={r.stderr[-300:]}")
    else:
        ok(f"lib:{fname}")

print("== negative test: bad source must fail cleanly ==")
r = run_host("run", os.path.join(SAMPLES, "negtest.c"))
if r.returncode == 0:
    bad("negtest", "should have failed to compile, but exited 0")
elif "error" not in r.stderr.lower():
    bad("negtest", f"stderr without an error message: {r.stderr[-200:]}")
else:
    ok(f"negtest (rc={r.returncode}, message: {r.stderr.strip().splitlines()[0][:70]})")

# v0.5 — misuse of the NEW keywords must fail loudly, never mis-compile.
# The sources live in a temp dir so no stray files are left in the tree.
NEG_CASES = [
    ("sizeof(void)",     "int x = sizeof(void);",   "sizeof(void) has no size"),
    ("sizeof expr",      "int x = sizeof(1 + 2);",  "sizeof expects ( type-name )"),
    ("sizeof unknown",   "int x = sizeof(nope);",   "sizeof: unknown identifier"),
    ("static no type",   "static = 1;",             "expected a type after static"),
]
with tempfile.TemporaryDirectory() as td:
    for i, (nm, body, want) in enumerate(NEG_CASES):
        p = os.path.join(td, f"neg{i}.c")
        with open(p, "w") as fh:
            fh.write(f"int main() {{\n    {body}\n    return 0;\n}}\n")
        r = run_host("run", p)
        if r.returncode == 0:
            bad(f"neg:{nm}", "compiled, but it should have failed")
        elif want not in r.stderr:
            bad(f"neg:{nm}", f"expected {want!r} in stderr, got: {r.stderr[-200:]}")
        else:
            ok(f"neg:{nm} -> {r.stderr.strip().splitlines()[0].split(': ', 2)[-1][:60]}")

print(f"\n== RESULT: {PASS} PASS, {FAIL} FAIL ==")
sys.exit(1 if FAIL else 0)
