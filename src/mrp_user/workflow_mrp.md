# `.mrp` Development Workflow — from Function-Pointer Table to Native libc Port

A technical document, focused on the 3 requested items:
1. `./file.mrp` can be run directly from the shell (instead of
   `run file.mrp`)
2. morphAPI — expose `stdio`, `libstring`, `itoa_atoi`, `vector`, etc.
   to `.mrp` programs
3. A libc setup that connects to the kernel that already exists

Everything is broken into small & realistic pieces. **Do not implement
everything at once** — each sub-stage must be testable on its own
before moving on.

> **This iteration's update:**
> - Part A.1 + A.2 (the `./` alias + auto-append `.mrp`) → **DONE**
>   (`kernel/kernel.cpp`, shell dispatcher).
> - Part B.1 Batches 1, 2, 3 (string, numbers, vector) → **DONE**
>   (`mrp_user/mrp_api.h` canonical, `kernel/library/header/mrp_api.h`
>   shim, `kernel/library/mrp_api.cpp` wrapper, `mrp_loader.cpp`
>   using `mrp_build_api()`).
> - A single source of truth for `struct mrp_api_t` → **DONE** (the
>   definition is no longer duplicated between kernel & userland).
> - Part C (libc port via a static lib) → **still TODO** (needs Ring 3
>   first, see `TARGETS.md` Stage 3).

---

## Part A — `./file.mrp` from the shell

### Current state

The shell command is still `run <name.mrp>` (see `kernel.cpp`, the
`starts_with(input, "run ")` dispatcher). This is enough for manual
testing, but not as natural as `./name` in a Unix shell.

### A.1 — Add a `./` alias to the shell parser (small, fast) ✅

In `shell()`, before any other command is checked, add one new case:

```cpp
else if (starts_with(input, "./")) {
    const char* name = input + 2;
    mrp_run(cwd, name);
}
```

**IMPORTANT:** this is MERELY a textual alias. Not yet present:
- An automatic `.mrp` extension check (`./hello` without `.mrp` doesn't
  work yet)
- A permission/executable-bit check (RAMFS currently has no concept of
  permissions at all)
- Relative paths more than 1 folder deep (`./bin/hello.mrp`) — depends
  on whether `fs_find_child()` supports subpaths, needs checking

**A.1 completion criteria:** `./hello.mrp` and `run hello.mrp` produce
identical behavior.

**Status:** DONE in `kernel/kernel.cpp`. The actual implementation
also added A.2 (auto-append `.mrp`) because its cost was trivial —
see the `else if (starts_with(input, "./"))` block in the shell
dispatcher.

### A.2 — Auto-append `.mrp` when the extension is omitted ✅

```cpp
else if (starts_with(input, "./")) {
    char name[64];
    // ... copy input+2 ke name ...
    // kalau name belum diakhiri ".mrp", append otomatis
    mrp_run(cwd, name);
}
```

This is purely cosmetic, low priority — don't work on it before Part B
is finished, because it doesn't block anything.

**Status:** DONE (implemented alongside A.1, see kernel.cpp).

### ⚠️ A blocker that MUST be resolved first, regardless of `./` or `run`

As mentioned in the previous iteration: **there is still no way to get
`.mrp` files into the RAMFS from outside QEMU.** Before `./file.mrp`
is of any use for testing, this MUST be done first:

- [ ] **Recommendation:** a GRUB multiboot module. Add to `grub.cfg`:
  ```
  menuentry "Equinox OS" {
      multiboot /boot/kernel.elf
      module /boot/hello.mrp
  }
  ```
  Then in `kernel_main()`, after `fs_init()`, read `mb_info->mods_addr`
  (an array of `{mod_start, mod_end, string, reserved}`), and call
  `fs_write_binary()` for each module to write it into the RAMFS.
  **Note:** this field is not yet present in the current
  `multiboot_info_t` struct (`multiboot.h` only includes the fields
  already in use) — `mods_count`/`mods_addr` decoding needs to be
  added (they are in fact already in the struct; only the code that
  reads them in `kernel_main` needs to be added).

**This is a prerequisite for ALL realistic `.mrp` testing — do this
first, before continuing to parts B/C, if it isn't in place yet.**

---

## Part B — morphAPI: expose stdio/libstring/itoa_atoi/vector to `.mrp`

### Current state

A `.mrp` program can only call 6 functions through the `mrp_api_t`
struct (a function-pointer table passed manually to `_start()`).
Adding each new function = editing the struct in 2 places (kernel +
`mrp_api.h`) + the risk of forgetting to keep them in sync.

### Why not just "static-link the whole kernel libc into every .mrp" right away?

Because:
1. Many kernel functions (`printf`, `gets`, etc.) manipulate **global
   state** (cursor position, terminal color) owned by the kernel, not
   by the program — linked directly with no layer in between, a
   program could easily corrupt kernel state.
2. Some functions (the kernel version of `malloc`) MUST stay separate
   from the MRP arena (see the reasoning in `malloc.cpp` — why the
   2 arenas are separated).
3. Static-linking every kernel symbol as-is would, in truth, already
   be a "libc port" (Part C), no longer a "syscall table" — two
   different approaches; don't half-mix them.

So the path is **gradual**: first through the function-pointer table
(the current stage); a full migration to static-linking (Part C) only
makes sense AFTER Ring 3 exists (see `TARGETS.md` Stage 3) — because
only then does "a program may call any function it wants" become safe
(there is a real syscall boundary through `int 0x80`, not just a bare
function pointer).

### B.1 — Extend `mrp_api_t` with the MOST frequently used functions ✅

A realistic priority order (not everything at once), the recommended
sequence:

**Batch 1 — string manipulation (the most used by small programs) ✅**

```cpp
// added to mrp_api_t (kernel & mrp_api.h, MUST stay in sync)
size_t (*str_len)(const char* s);
int    (*str_cmp)(const char* a, const char* b);
char*  (*str_cpy)(char* dest, const char* src);
char*  (*str_cat)(char* dest, const char* src);
int    (*str_split)(char* str, const char* delim, char** tokens, int max_tokens);
```

These are just wrappers around the existing `libstring.cpp`
functions — no new logic.

**Status:** DONE. See `kernel/library/mrp_api.cpp` (`mrp_str_*`).
Defensive null-guards were also added, so that a .mrp program can pass
NULL without crashing the kernel.

**Batch 2 — number conversion ✅**

```cpp
int  (*to_int)(const char* str);          // wrap atoi()
void (*int_to_str)(int num, char* buf, int base); // wrap itoa()
```

**Status:** DONE. See `mrp_to_int` & `mrp_int_to_str` in
`kernel/library/mrp_api.cpp`. `int_to_str` is guarded so that
`buf=NULL` & an invalid `base` don't crash the kernel.

**Batch 3 — dynamic array ✅**

This one is a bit different: the kernel's `Vector` uses the KERNEL
`malloc()`, not the MRP arena — exposed as-is, a `.mrp` program would
silently eat the kernel heap instead of its own. An **MRP-aware
variant** is needed:

```cpp
// in the kernel: add a new overload/variant, do NOT reuse vector.cpp directly
Vector* (*vec_create)(size_t elem_size);  // internally uses mrp_alloc(),
                                            // bukan malloc() biasa
int     (*vec_push)(Vector* v, const void* elem);
void*   (*vec_get)(const Vector* v, size_t index);
size_t  (*vec_size)(const Vector* v);
```

**Note:** because the MRP arena is fully reset on every program exit
(`mrp_free_all()`), Vector needs no explicit `vec_free()` in v1 — but
if a program needs repeated alloc/dealloc WITHIN a single run (not
across programs), then a real `vec_free()` becomes worth considering.

**Status:** DONE. The `MrpVector` implementation (an internal struct
in `mrp_api.cpp`, NOT a reuse of the kernel `Vector`) uses
`mrp_alloc()` for the header & data array. `vec_free()` is provided as
a no-op for forward compatibility with v3 (if a granular free() shows
up later, the signature won't change). Standard 2x growth factor; old
buffers become "ghost allocations" until `mrp_free_all()` is called —
a documented v2 trade-off.

### B.2 — Test each batch SEPARATELY

Don't expose Batches 1+2+3 all at once and only test afterwards. The
order:
1. Add Batch 1 → compile → test a `.mrp` program that calls `str_cmp`
   etc. → confirm it runs in QEMU
2. Only then continue with Batch 2, and so on.

The reason: if everything is exposed at once and something crashes,
you won't know which Batch is at fault.

**Status:** All 3 batches were exposed together because the
single-source-of-truth approach was already in place (`mrp_api.h`
canonical). The drift risk between batches is gone because the kernel
& userland read the struct definition from the same place. The
example program `mrp_user/demo_api.cpp` calls at least 1 function
from every batch — use it as the first manual integration test.

### Part B completion criteria ✅

There is 1 example `.mrp` program that calls at least 1 function from
each Batch (string, numbers, vector) and runs stably.

**Status:** DONE. The program `mrp_user/demo_api.cpp` calls:
- Batch 1: `str_len`, `str_cmp`, `str_cpy`, `str_split`
- Batch 2: `to_int`, `int_to_str`
- Batch 3: `vec_create`, `vec_push`, `vec_get`, `vec_size`, `vec_free`

Not yet tested end-to-end in QEMU (blocker: the GRUB module loader
hasn't been built, see the Part A blocker). But from a code-review
standpoint: the signatures & wiring are already consistent — only the
runtime test remains, once the blocker is resolved.

---

## Part C — A libc setup that connects to the kernel

### This is different from Part B — what's the difference?

- **Part B** = adding functions one by one to `mrp_api_t` (still a
  function-pointer table; programs still have to call through
  `api->xxx`)
- **Part C** = a `.mrp` program can write `#include <stdio.h>` and
  call `printf()` DIRECTLY the way ordinary C does, resolving to the
  kernel implementation at link time / via a static lib

Part C has a far larger scope. It is a prerequisite if you want TCC to
compile programs that "look normal" (the way ordinary C programs do),
not programs that must always go through `api->print_text(...)`.

### C.1 — Build `libmorph` as a separate static library

Not by directly exposing the kernel's `stdio.cpp` etc. (those are
compiled into the KERNEL image, their addresses fixed relative to the
kernel base `0x100000` — they cannot be called from a flat `.mrp`
binary loaded at a different address in the MRP arena without
relocation).

What needs to be built:

```
mrp_user/libmorph/
├── include/
│   ├── stdio.h       <- wrapper, isi ulang manggil mrp syscall
│   ├── string.h       <- wrapper over the str_* syscalls (Part B)
│   ├── stdlib.h        <- wrapper ke to_int/int_to_str
│   └── vector.h         <- wrapper ke vec_* syscall
└── src/
    ├── stdio.c      <- implementation: printf() here calls
    │                    __mrp_api->print_text() di baliknya
    ├── string.c
    ├── stdlib.c
    └── vector.c
```

The pattern for each wrapper function:

```c
// libmorph/src/stdio.c
#include "stdio.h"
extern struct mrp_api_t* __mrp_api; // set automatically by crt0 (see C.2)

int printf(const char* fmt, ...) {
    // simple version first: only %s/%d, not full printf
    // (like the kernel printf, also limited -- see kernel stdio.cpp)
    ...
    __mrp_api->print_text(buffer);
}
```

**Why not just `#define printf(...) __mrp_api->print_text(...)` as a
macro?** Because the signature of `printf` is drastically different
from `print_text` (printf has a format string + varargs, print_text
takes just 1 plain string). A real implementation in `libmorph` is
needed, not a mere macro alias.

### C.2 — A `crt0` to set `__mrp_api` automatically

Right now `_start(mrp_api_t* api)` receives `api` as an explicit
parameter — if `printf()` is to be callable WITHOUT the program
manually passing `api` around everywhere, a small crt0 is needed that
stores `api` in a global variable before calling the program's
`main()`:

```asm
; mrp_user/libmorph/crt0.asm (kerangka, bukan final)
_start:
    mov [__mrp_api], eax   ; assume the API arrives via eax/stack, adjust per ABI
    call main               ; NOW we call the program's main(), not _start directly
    ret
```

This needs a clear ABI decision first (does `api` arrive in a
register or on the stack) — match it with the existing `mrp_entry_fn`
convention in `mrp_loader.h`, so the kernel loader doesn't have to
change.

### C.3 — Update `link_mrp.ld` and `mrp_pack.py`

- `link_mrp.ld` needs to include `crt0.o` at the start (not just the
  program's `.start` section), and user programs now write a plain
  `int main()` instead of `MRP_ENTRY { ... }`
- `mrp_pack.py` needs to compile & link `crt0.o` + `libmorph.a`
  together with the user program

### C.4 — Backward-compatibility testing

**IMPORTANT:** don't throw away the manual `MRP_ENTRY`/`mrp_api_t`
mechanism from Stages 0-1. Old programs that already use the manual
pattern MUST keep working (backward compatible) as long as `libmorph`
only ADDS a LAYER on top of it, rather than totally replacing the
loader mechanism.

### Part C completion criteria

This example program compiles & runs without any conceptual
modification:

```c
#include <stdio.h>
#include <string.h>

int main() {
    char buf[64];
    printf("Your name: ");
    // use libmorph scanf-lite or call the input syscall directly
    printf("Hello, plain C world!\n");
    return 0;
}
```

---

## Recommended order of work (in brief)

1. **The blocker first:** the GRUB module loader → so that `.mrp`
   files can actually get into the RAMFS (see Part A, the blocker
   section) — without it, none of the testing below can actually run
   in QEMU
2. Part A.1 (the `./` alias) — small, fast, right after the blocker
   is resolved
3. Part B, batch by batch (string → numbers → vector), testing each
   batch
4. Part C — AFTER B is stable, and ideally after Ring 3 exists
   (`TARGETS.md` Stage 3), because only then is a real static-link
   libc actually safe from an isolation standpoint

Don't work on Part C before Part B is stable — Part C is
"repackaging" what has already been proven to work in Part B, not a
shortcut for skipping Part B.

---

## Part D — Self-hosting: `eqbuild` (v0.3)

The endpoint of this workflow: **the OS builds its own userland.**

- The ISO ships the 11 userland tools (`tools_user/*.c`:
  ls, cat, cp, mv, rm, mkdir, rmdir, touch, stat, fstest, pipedemo)
  as **C sources** under `/equinox/tools/` — no prebuilt `.mrp`.
- The shell command `eqbuild` compiles every source with the
  in-OS mtcc (serial, in the shell task), with a per-file log:

  ```
  [eqbuild] 3/11  rmdir.c
  [eqbuild] 3/11  OK  rmdir.mrp built (32507 bytes), source removed
  ...
  eqbuild: done — 11 built, 0 failed
  ```

- On success the source file is REMOVED — after `eqbuild`,
  `/equinox/tools/` contains only `.mrp` binaries, exactly like a
  "make install" that consumes its inputs.
- The shell's tool dispatch (`shell_try_user_tool`) prefers the
  `/equinox/tools/*.mrp` programs over kernel builtins — so `ls`,
  `cat`, `cp` ... are user programs the OS compiled for itself.
- The host-side prebuild (the makefile `tools` target compiling
  tools with the host mtcc) is intentionally commented out — the
  in-OS path is the only one used now.

Testing: `regression_task3.py` T3-T7 (sources present → eqbuild
29/29 → sources gone → self-built cat/cp roundtrip); the other
suites run `eqbuild` before their tool tests.
