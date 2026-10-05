# Self-hosting — mtcc, MRP1, ruf recipes and the build pipeline

The defining feature of Equinox: **the OS compiles its own userland
from C sources, inside the OS, with its own compiler.** The ISO ships
sources, not binaries; on first boot `equinoxinstall` (or `eggkg`) runs
the in-OS compiler `mtcc` over them.

![Self-hosting pipeline](image/selfhost_pipeline.png)

## The pipeline

1. **Sources on the ISO** — `tools_user/*.c` (text filters), 
   `libc/*.c + *.h` (a 13-module C subset), and `mtcc.mrp` (prebuilt
   compiler). The bash-class coreutils are *not* shipped; they arrive
   as a package (see [PACKAGES.md](PACKAGES.md)).
2. **GRUB module routing** — `mrp_bootloader` routes nested `.c`
   modules into RAMFS `/equinox/tools` and `/equinox/libc`; flat `.c`
   files land in `/test` as ready-made mtcc samples.
3. **Compile in-OS** — `equinoxinstall` spawns `mtcc.mrp` per file
   (or `mtcc -make` over a recipe); each successful compile logs
   `i/N OK name.mrp` and the artifact is a **MRP1** ring-3 executable.
   Host vs in-OS output is verified **byte-identical** by the
   regression harness.
4. **Install & resolve** — artifacts live in `/equinox/tools` (+ `/bin`
   for packages); the shell prefers userland tools over its own
   builtins via the system path: root, `/bin`, `/equinox/tools`,
   `/equinox/games`.

## mtcc — the in-OS C compiler (`mtcc.c`)

A single-pass compiler: **lexer → parser → direct x86-32 codegen**, plus
a small preprocessor that splices `<morph.h>` / `<multitasking.h>` /
`<fileio.h>`. Deliberately a C *subset*:

- no `struct` return/pass by value, no `float`, no `unsigned` (mostly),
  no `switch`, no `sizeof` — the libc and every tool live within these
  limits;
- printf-family with 5 conversions, `%u` as true unsigned, `sscanf`,
  ctype, string extras, `qsort`, `rand` ship in the libc;
- undefined references are rejected **with the call-site line number**;
- output is an **MRP1** flat ring-3 image with an 18-byte header
  (loader: `mrp_loader.cpp`; per-task arena `0x500000…0x2600000` with
  demand paging).

Usage:

| Command | Effect |
| --- | --- |
| `mtcc /test/hello.c` | compile **and run** (dev loop) |
| `mtcc -c <file.c>` | compile only → `<file>.mrp` next to the source |
| `mtcc -make <file.ruf>` | execute a ruf v3 build recipe (many jobs) |

## ruf v3 — build recipes

A `.ruf` file is a tiny makefile dialect understood by `mtcc -make`
(and by `equinoxinstall -build`). v3 adds real variables:

```text
# build.ruf — variables with :=, macros, copy/move ? to
name := "bash"
src  := "/equinox/.local/bash/src"
out  := "/equinox/.local/bash"

job cat    from "$(src)/cat.c"    to "$(out)/cat.mrp"
job grep   from "$(src)/grep.c"   to "$(out)/grep.mrp"
# ...
copy ?  from "$(out)" to "/bin"      # install step, '?' = every product
```

- `job <name> from <.c> to <.mrp>` — one mtcc compile job; the runner
  reports per-job progress and a failure summary at the end.
- `copy ? / move ? from X to Y` — bulk install of all products.
- The bash package's `build.ruf` (in Eggkg-l) is the reference example:
  ~20 jobs, all installed into `/bin` in one `eggkg install bash`.

## equinoxinstall — the driver of the pipeline

| Command | Effect |
| --- | --- |
| `equinoxinstall` | Full 4-phase wizard (disk → layout → NIC config → build) — see [INSTALL.md](INSTALL.md) |
| `equinoxinstall -compile <dir>` | Phase [4/4] only: compile `<dir>/libc` + `<dir>/tools`; on a volume the sources are **kept** (they are the install content) |
| `equinoxinstall -build <file.ruf>` | One recipe via `mtcc -make` |
| `equinoxinstall -build <name>` | One tool from `/equinox/tools` via `mtcc -c` |
| `equinoxinstall -build *.ruf` | Every matching recipe (single-star glob) |

RAMFS builds delete the `.c` after success (the next boot brings them
back); volume builds keep them so the install can be re-compiled
in place after edits.

## MRP1 executables & the morph API

- **MRP1**: flat binary + 18-byte header at `0x500010`; loaded into the
  task's demand-paged arena; per-task malloc arena reset on exit.
- Program calls the kernel through **syscall int 0x80 #1–#54**
  ([SYSCALLS.md](SYSCALLS.md)) wrapped by the **morph API**
  (`mrp_user/Morph.h`, ~20 functions: print, file I/O, gfx, spawn/wait,
  pipe…).
- ELF32 (`ET_EXEC`, static) is also supported — see
  [ARCHITECTURE.md](ARCHITECTURE.md).

## Writing a tool

```c
#include <morph.h>

int main(void) {
    morph_printf("hello from ring 3\n");
    return 42;                       /* reported by wait() */
}
```

```sh
mtcc /test/hello.c       # compile + run immediately
# or as a package product (see PACKAGES.md) -> lands in /bin
```

Constraints: plain C within the mtcc subset, libc subset only, ≤5
printf args, no filesystem global state — and it must compile fast,
because it compiles *inside* an 8-task 256 MB hobby kernel.

## Testing the pipeline

```sh
make test          # host harness: mtcc vs interpreter, libc parity
python3 scripts/eggkg_build_test.py   # full bash package build in-OS
make test-install  # manual install path incl. set -x scripts
```
