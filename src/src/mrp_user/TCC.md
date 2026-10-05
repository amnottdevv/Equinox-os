# mtcc 0.3 — Equinox OS TinyCC: compile C **inside** Equinox OS

> *"tcc can compile files inside the morp OS itself"* — now it can
> (a meaningful subset of C, not full C — see the Language section).

`mrp_user/mtcc.cpp` is a single-file, TinyCC-style C-subset compiler
(single-pass, no AST, direct x86-32 codegen) that **runs as a regular
`.mrp` program inside Equinox OS** and uses the `int 0x80` syscall for
all of its I/O needs. This is the first milestone of **Stage 4 in
TARGETS.md** ("compile C INSIDE Equinox OS, not cross-compiled from the
host").

## How to use (in Equinox OS)

```
root::users / $ mtcc test/hello.c          # compile & run immediately (tcc -run style)
root::users / $ mtcc -c test/hello.c       # compile → hello.mrp di RAMFS root
root::users / $ run hello.mrp              # jalanin hasil compile (standalone!)
root::users / $ mtcc --debug test/primes.c # info compiler (file size, code/data,
                                           #  size, exit code) + output program
```

`mtcc` is a **global shell command** (dispatched to `mtcc.mrp`
automatically, no `run` prefix needed). Without `--debug`, the output is
exactly the same as `run`: only the program's output, with no compiler
chatter. `--debug` (or `-d`) turns on the `[mtcc] ...` lines — banner,
file byte count, code/data sizes, exit code.

The `-c` output mode writes to the **RAMFS root** under the name
`<basename>.mrp` (`hello.c` → `hello.mrp`). If the file already exists,
it is fully overwritten (binary-safe, via the `mkfile` syscall).

## Build & put it into the ISO

```bash
make clean && make      # kernel + ISO (REQUIRED — old binaries lack syscalls 15/16)
make pack               # compile all mrp_user/*.cpp -> dist/*.mrp (including mtcc.mrp)
                        # + copy test/*.c → dist/ (sample .c)
make iso                # new ISO: GRUB modules = all .mrp + .c files in dist/
make run                # boot QEMU
```

The `.c` files in `dist/` automatically become multiboot modules →
`mrp_bootloader` loads them into the RAMFS `/test` at boot → so
`mtcc test/hello.c` works right away. The test/ samples are also used
by the host harness (`make test`).

**Quick run without rebuilding the ISO** (VGA text mode, still full
functionality):

```bash
qemu-system-i386 -m 64 -kernel dist/kernel.elf \
    -initrd "dist/mtcc.mrp,dist/hello.c"
```

## Supported language (a deliberate subset — v0.1)

| Category | Supported | Not yet |
|---|---|---|
| Types | `int`, `char`, `void`, pointers 1–2 levels deep, 1D arrays | struct/union, float/double, unsigned (int is processed as **signed**), long/short, typedef |
| Statements | if/else, while, do-while, for (+declaration in the C99 init), return, break, continue, blocks | switch, goto |
| Operators | `= += -= *= /= %= <<= >>= &= \|= ^=`, `+ - * / %`, `<< >>`, `& \| ^ ~`, `&& \|\| !`, `== != < > <= >=`, unary `- + * &`, `++/--` (pre/post), `?:` | comma operator, sizeof |
| Globals | scalars + arrays + constant init `{...}`/string (zero-init) | non-constant initializers |
| Functions | forward prototypes, recursion, max 8 params, max 12 args | variadic, function pointer |
| Other | `//` and `/* */` comments, hex/decimal/char literals with escapes, **default parameter `= const` (v10.8)** | function pointer, struct/typedef, cast, array-of-pointer |

Important semantic notes:
- `>>` is an **arithmetic** shift (int is treated as signed, as in an
  ordinary C compiler).
- Division by zero → exception #DE → the ring-3 fault handler TERMINATES
  just the program (`crashde.mrp` demo) — the kernel and the other tasks
  keep running. Same for wild pointer accesses (`crashptr.mrp`).
- Global variables must be declared **before** use (single-pass).
- `p - p` yields the raw byte difference (not divided by the element
  size).

## Builtins — the runtime is the OS itself

Compiled programs **have no libc**; the functions below are emitted as
inline `int 0x80` with stable syscall numbers
(`kernel/library/header/syscall.h` — the single source of truth; tcc.cpp
includes that same header when building the .mrp):

| Function | Syscall | Notes |
|---|---|---|
| `print(str)` | 10 | no automatic newline |
| `printint(num)` | 11 | **unsigned** (the semantics of the kernel's `print_int`) |
| `getkey()` | 8 | non-blocking, -1 if empty |
| `readline(buf, maxlen)` | 9 | blocking, returns the length |
| `write(fd, buf, len)` | 4 | console (fd 1/2) |
| `open(path)` / `read(fd,buf,len)` / `close(fd)` | 6/5/7 | RAMFS read-only, sequential |
| `mkfile(path, buf, len)` | 16 | create/overwrite binary — used by tcc itself |
| `malloc(size)` | 12 | from the MRP arena (no per-allocation free yet) |
| `sleep(ms)` / `gettick()` / `getpid()` | 14/13/3 | |
| `getargs(buf, maxlen)` | 15 | arguments of the last `run` |
| `exit(status)` | 1 | returns to the caller (the shell) |
| `exec(path)` | 2 | **always rejected: EBUSY (-9)** — see Limitations |

### Morph.h builtins (v10.3 — same names as the SDK header)

BEFORE v10.8, mtcc had no preprocessor, so `#include <Morph.h>` could
not be used in-OS. Instead, the Morph.h file API names are available
directly as builtins — source written in the "Morph.h style" compiles
on both paths (hosted using the real header, in-OS using the builtins):

| Function | Syscall | Notes |
|---|---|---|
| `file_open(path)` | 6 | alias of `open` |
| `file_read(fd, buf, len)` | 5 | alias of `read` |
| `file_close(fd)` | 7 | alias of `close` |
| `file_write(path, buf, len)` | 16 | alias of `mkfile` — **create or fully OVERWRITE** |
| `file_read_all(path, buf, maxlen)` | 17 | reads the whole file in one call |
| `file_size(path)` | 18 | file size (for buffer allocation) |
| `file_exists(path)` | 19 | cheap probe: 1/0, no fd |

The standard "file edit" pattern: `file_size` → `file_read_all` →
modify in memory → `file_write` (overwrite). A full example:
`test/morphio.c` (a 16-stage regression, runs under the host `make
test` as well as under `mtcc test/morphio.c` in-OS).

## Architecture (tcc-style: one pass, no AST)

```
source .c --lexer--> tokens --recursive-descent parser--> direct codegen
                                                        │
                    ┌───────────────────────────────────┤
                    ▼                                   ▼
             code buffer (x86-32)                 data buffer
             rel32 antar-fungsi + FIXUP           string literal + global
                    │                                   │
        ┌───────────┴───────────┐                       │
        ▼ mode RUN              ▼ mode -c               │
  patch dgn alamat buffer   patch dgn base 0x500010 ────┤
  -> call the entry directly  (MRP_LOAD_BASE, the same     │
    (`tcc -run` style)        │   address as link_mrp.ld)  │
                           ▼                            │
                [header .mrp 18 byte][code][pad][data] ─┘
                -> SYS_MKFILE into RAMFS -> `run out.mrp` works
```

- **Fixups**: a list of 4-byte locations whose contents are only known
  later (forward jumps, forward calls, global addresses). In `-c` mode
  they are patched with `0x500010` — exactly the address returned by
  the first `mrp_alloc()` in a freshly reset arena (deterministic; see
  the comments in `link_mrp.ld`).
- **Header + checksum** are not hardcoded twice: tcc.cpp `#include`s
  the **kernel's mrp_format.h** and validates its own image
  (`is_valid_mrp`) before writing it to the RAMFS.
- **Assignment without an AST** (a rollback trick): the LHS is first
  parsed as a value; if the next token is an assignment operator →
  roll back the lexer+code state → re-parse through the lvalue path.
  Strings are safe because dedup is idempotent.
- **Internal calling convention** (compiled code only ever calls
  compiled code + builtins): arguments are evaluated left→right and
  then pushed (parse order = push order, no AST needed to reverse the
  order); the callee reads param i at `[ebp + 8 + 4*(n-1-i)]`.

## Already tested (host + in-OS)

The test harness `scripts/tcc_host_test/` (runs on ordinary Linux):
- **The x86-32 interpreter** executes exactly the instruction set that
  mtcc emits (a closed set — a stray instruction makes the test go red
  immediately).
- 12 samples × 2 modes (run & compile→.mrp→execute at base 0x500010,
  exactly as the kernel loader does): **the output must be identical
  to the expectation byte-for-byte**.
- The `.mrp` files produced by `-c` mode are validated **independently
  of Python** (magic/version/size/entry/rotate-xor checksum — the same
  algorithm as the kernel).
- Syscall-number sync between mtcc.cpp ↔ kernel/syscall.h is checked
  automatically.
- Negative tests: bad source → a clear line-N error message, not a
  crash.
- **Anti-clean-memory safety net**: all host-mode `os_alloc()` memory
  is first filled with the dirty `0xA5` pattern — bugs that "depend on
  zeroed memory" (see v2.5) immediately go red on the host too, not
  only in-OS.

```
== HASIL: 27 PASS, 0 FAIL ==
```

**In-OS (booting the real ISO in qemu + font-OCR)**: 16/16 — VESA
boot, the `root::users / $` prompt, mtcc quiet & `--debug` (the full
primes.c + exit code), the editor (title/status/gutter/syntax), exiting
the editor, unknown command. Plus a multi-run sequence: `mtcc hello →
primes → primes → edge → -c primes → run primes.mrp → exec_test → arr
→ varidx → pl1..pl5` — all correct.

### Important bug already fixed — "unknown identifier" on the 2nd run (v2.5)

**Symptom**: `mtcc test/hello.c` then `mtcc test/primes.c` →
`mtcc: error line 7: unknown identifier: sieve` (the historical
message before the fix). The FIRST run after boot was always correct;
subsequent runs fail depending on the previous program. The host tests
were always green (a fresh process per test).

**Root cause**: the global table (`S.gvars`) was allocated from the
**recycled .mrp arena** between mtcc runs (same address, old contents,
not zeroed). Global name registration copied the name **without a NUL
terminator** → the new name kept the old name's tail: `sieve` + `ing`
(leftover from `greeting` in hello.c) = `sieveing` →
`find_gvar("sieve")` failed. Why the first run was safe: fresh RAM =
zero. Why pl*.c→primes was safe: 1-letter names + the tail bytes were
still zero.

**Fix** (two layers, mutually reinforcing):
1. `parse_gvar_one` explicitly writes `g->name[nlen] = '\0'` (clamped
   to `MTCC_NAME_MAX-1`).
2. `mtcc_compile` zeroes the entire gvar table at allocation (the same
   principle as zero-initializing the data area).

In addition: mtcc errors now include **the failed identifier's name**
and (when one occurs) a `[dbg] fn/gv/lc/sd/fx/pos/tok + table contents`
state dump — it was precisely this kind of diagnostic that led to the
root cause within a single repro. The `0xA5` safety net in the host
harness keeps this class of bug red if it ever comes back.

## Limitations (honest — no false expectations)

1. **exec() from compiled code is always rejected** (`SYS_EBUSY` = -9,
   fix for V3 audit #1): the caller's code lives in the same MRP arena
   as the exec target — the kernel rejects nested execs so that the
   arena is not reset, which would overwrite the code that is currently
   running (before the fix: a deterministic panic). The calling program
   stays alive after `exec()` returns. The correct pattern: compile
   first (`-c`), then `run output.mrp` from the shell.
2. **Ring 0**: compiled programs (and tcc itself) have full memory
   access — a wild pointer = kernel panic. Isolation arrives in Stage 3
   (Ring 3), not now.
3. **Borrowed kernel stack**: very deep recursion (>~1000 frames) can
   exhaust the stack → panic. `fib(20)` is safe, `fib(1000)` is not.
4. Compiled `malloc()` has no `free()` (the arena is reset per run —
   the current MRP arena design).
5. Compile buffers: source ≤ 96KB, code ≤ 384KB, data ≤ 128KB per
   program (enough for shell tools; anything larger → a clean error
   message, not an overflow).
6. A compiled program's `.bss` is written as literal zeros in the .mrp
   file (the same as every other .mrp program — avoid giant global
   arrays).

## Distance to the real TinyCC (Fabrice Bellard)

mtcc = the pipeline's foundation: source→code→`.mrp`→run, plus the
syscall ABI & tooling (getargs/mkfile, .c files as ISO modules). What
is still MISSING, and realistically needs weeks to months:

1. **Preprocessor** (#include/#define) — before porting the real tcc,
   this is the biggest blocker for real programs.
2. Struct/union/typedef, multidimensional arrays, unsigned semantics.
3. Porting the **real libtcc** (~80k lines): needs a libc port in
   userland (malloc with free, buffered stdio, etc.) — the path runs
   through Stage 3 (Ring 3) first, so that a tcc crash doesn't take
   the kernel down with it.
4. An ELF→`.mrp` backend writer in the real tcc (the Stage 4 checklist
   in TARGETS.md).

The recommended order remains as in TARGETS.md: stabilize first
(heap/paging), then Ring 3, then the real tcc. This mtcc serves as
"proof of the path" that the whole .mrp pipeline + syscalls is ready
to receive a real compiler.

## Mini preprocessor + libc prelude (v10.8)

mtcc 0.3 has a single-pass preprocessor (cpp semantics: macros are
defined when encountered, `#ifdef` sees the macro table as it stands
at that moment, includes are spliced in place):

| Directive | Behavior |
|---|---|
| `#include <morph.h>` | splices in the built-in **libc prelude**. Aliases: `<stdio.h>` `<stdlib.h>` `<string.h>` `<Morph.h>` (all the same prelude) |
| `#include "file.h"` | reads a RAMFS file (relative to the process cwd), recursive splicing up to 8 levels; `#ifndef` guards work |
| `#define NAME value` | object-like macro; substitution on identifier boundaries, strings/chars/comments untouched; **function-like macros are rejected** with a clear message |
| `#undef NAME` | removes the macro |
| `#ifdef` / `#ifndef` / `#else` / `#endif` | conditional inclusion (a per-file stack, not shared across includes) |
| `#pragma` / `#error` / `#` | ignored |

Programs WITHOUT directives pass through verbatim — the binary stays
small; the prelude is only pulled in when `#include`d.

### Prelude contents (`#include <morph.h>`)

- **memory/string**: `memcpy memset memmove memcmp memchr strlen strcmp
  strncmp strcpy strncpy strcat strncat strchr strrchr strstr`
- **conversion**: `atoi strtol itoa utoa`
- **user-space heap**: `malloc free calloc realloc` — free-list +
  split + coalesce on top of `__arena_alloc` chunks (the 4MB kernel
  arena, reset when a program exits). `free()` genuinely returns
  memory.
- **printf family (v0.3, FR-19 — LOCALLY rendered)**:
  `printf sprintf snprintf` share one in-program renderer (up to 5
  conversion args per call): `%d %i %u %x %X %o %p %c %s` with width,
  zero-pad, left-align; `%u` is a TRUE unsigned render (binary long
  division by 10 — the full 0..4294967295 range) — no kernel ABI
  round-trip, no 3-conversion limit. `sscanf` (5 pointer outputs)
  joined in v0.3.
- **ctype (v0.3)**: `isalpha isdigit isalnum isspace isupper
  islower isprint toupper tolower`
- **string extras (v0.3)**: `strdup strtok strspn strcspn
  strcasecmp strncasecmp`
- **stdio extras (v0.3)**: `puts fputs fputc fgetc remove
  rename strerror`
- **misc**: `abs`, `rand`/`srand` (xorshift), `time()` (seconds since
  boot), `getenv()` (always NULL — no environment block yet),
  `abort()` (exit 134), `lseek(fd,off,whence)`, `ring()` (the
  caller's CPL — a program can prove for itself that it is ring 3).
- **graphics (v0.3, FR-17/18)**: `set_clip(x|w<<16, y|h<<16)`
  and `draw_line(x0|y0<<16, x1|y1<<16, color)` wrappers over the
  `__gfx_clip2`/`__gfx_line3` builtins (syscalls 53/54).
- **stdio FILE I/O**: `fopen(path, mode=0)` with mode `"r"` = a
  direct fd (`fseek` via the `lseek` syscall), `"w"`/`"a"` = a
  dynamic write buffer flushed by `fclose` through `file_write`.
  `fread fwrite fseek ftell fclose`. Handles 1..8 (0 = NULL).
  ⚠ **`fread`/`fgetc`/`fwrite` take the `fopen()` SLOT (1..8), not a
  raw `f_open()` fd** — passing a raw fd makes the `__fio_mode` check
  fail and every read returns EOF instantly (the v0.3 tools-release
  hunt). Raw fds work with `read`/`write`/`close`/`lseek` and the
  `fileio.h` wrappers; line-oriented tools should use
  `fopen`/`fgetc`/`fclose` end-to-end.
- **arrays**: sizes are plain NUMBER literals, max **4096 elements**
  (`char buf[512]` ok; `char buf[8*64]` is a parse error, `char
  buf[32768]` is rejected) — bigger stores come from `malloc()`
  (the 2 MB MRP arena), and `char* p = malloc(n)` works (pointer
  assignment is not type-strict).
- **sort**: `qsort_int(int*, n)` and `qsort_str(char**, n)` — a
  generic qsort needs function pointers (not yet in the mtcc subset);
  the hosted Morph.h path has a fully generic `qsort_()`.
- **misc**: `time()` (seconds since boot), `getenv()` (always NULL —
  no environment block yet), `abort()` (exit 134),
  `lseek(fd,off,whence)`, `ring()` (the caller's CPL — a program can
  prove for itself that it is ring 3).

### A small language extension: default parameters

`int fopen(char* path, char* mode = 0)` — a parameter with a constant
default value; a call with fewer arguments automatically pushes the
default (C++-style). This is what makes `printf("hi\n")` valid without
varargs.

### 3 old compiler bugs found & fixed during the v10.8 audit

1. `gen_logor`/`gen_logand` always overwrote the expression's result
   type with `int` — `(g + 5)[0]` (indexing a parenthesized expression)
   failed to compile.
2. `gen_lvalue` for `p[i] = v` used the pointer's **slot address**,
   not its **value** — assignment through a pointer index wrote to the
   stack. The value path (`v = p[i]`) was correct; the lvalue path was
   wrong (never exposed by the old tests).
3. Array-of-pointer (`char* a[5]`) really is still unsupported
   (documented; workaround: an `int a[5]` holding addresses, with loose
   assignment).
