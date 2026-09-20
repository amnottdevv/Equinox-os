# Equinox OS — Long-Term Targets: from `.mrp` to running DOOM

This document records the big goals being pursued and why they MUST be broken
into small stages first. **Do not skip stages** — each stage assumes the
previous one has been proven to work in real QEMU, not merely "looks right in
the code".

## TL;DR reality check

> "Running a modified TCC" and "running DOOM" are NOT features — they are the
> **end results** of dozens of subsystems that each have to be correct first:
> memory protection, a complete syscall set, file I/O, precise input timing,
> a fast framebuffer, and (for DOOM) floating point and enough performance.

If any subsystem below is still fragile (say the heap still corrupts, or
there is no bounds validation at all), moving to the next stage only stacks
bugs whose origin is hard to trace — the loader? the ported libc? TCC? the
game itself? That is why the order matters, not just the checklist.

---

## Stage 0 — DONE
- [x] `.mrp` format (header + validation + checksum)
- [x] Free-list `malloc` with split/coalesce/canary
- [x] Basic loader: load -> `call` entry -> return to the shell
- [x] Minimal syscalls: print, input, alloc, tick

## Stage 1 — `.mrp` programs that are actually useful (not just demos)
**Goal: `.mrp` programs can be used to write small, useful shell tools, not
just "hello world".**

- [x] File I/O syscalls — **DONE in v0.3 (FR-01)**: open2 with O_* flags,
      write() through an fd, lseek, stat/fstat, readdir, unlink/mkdir/
      rmdir/rename (syscalls #40-#48). ls/cat/cp/rm/mkdir/rmdir/touch/
      stat/mv ship as USERLAND .mrp tools (tools_user/, built by
      `make tools`) — no longer kernel builtins.
- [x] `exit(code)` + the loader reporting the return status to the shell
- [x] `./file.mrp` directly from the shell (no need to type `run`)
      — **DONE in `kernel/kernel.cpp`**, plus automatic `.mrp` appending.
- [x] Basic bounds validation — bounds pre-checked AND programs run at
      **Ring 3** (since v10.7): the syscall layer validates every user
      pointer (uaccess, SYS_EFAULT) and the fault handler kills only the
      offending task (crashptr/crashstk/crashkmem demos).
- [x] morphAPI v1: `libstring`, `itoa_atoi` subsets callable directly from
      a `.mrp` program (not through a manual function-pointer table, but
      through a static-linked libc port)
      — **DONE up to v2**: `mrp_user/mrp_api.h` is now the single source
      of truth (the kernel side is just a shim). Batch 1 strings, Batch 2
      numbers, Batch 3 vectors (MRP-aware) are already exposed via
      `kernel/library/mrp_api.cpp`. The C part (libc port as a static lib)
      is still TODO, but the bottleneck is Ring 3 (Stage 3), not the API
      surface.

**Exit criteria for this stage:** at least 3 non-trivial `.mrp` programs that
run stably, repeatedly, without crashing the kernel (e.g. a calculator, a
text formatter, a simple number-guessing game using `get_key`+`get_tick`).

## Stage 2 — Basic memory protection (MANDATORY prerequisite for Ring 3)
**Goal: the kernel does not collapse because of one buggy `.mrp` program.**

- [x] Paging enabled (identity-mapped first; fancy virtual memory not
      needed yet) — this OS did not set up CR3/PDE AT ALL at the time of
      writing
      — **DONE** (v10.7 supervisor/user split); since **v0.3** the
      per-task VMA windows are DEMAND-PAGED: the reservation is a
      non-present PTE_DEMAND marker and the #PF handler (isr_14 ->
      task_demand_fault) allocates + zero-fills one physical page per
      first touch. Physical memory is only consumed by pages actually
      touched (a 24 MB DOOM arena costs only what DOOM touches);
      `meminfo` / SYS_MEMINFO report reserved vs faulted per task.
- [x] Guard pages / bound checks in the MRP arena so overflows are easy to
      detect (instead of silently corrupting)
      — **DONE**: supervisor guard page at 0x2601000 (stack overflow ->
      program killed, kernel survives — crashstk demo) + uaccess checks
      on every syscall pointer; demand pages make out-of-window access
      a clean kill, not silent corruption.
- [x] A simple watchdog: if a `.mrp` program hangs for > N seconds, there is a
      manual way to interrupt it (not just resetting QEMU)
      — **DONE** (Phase C): `ps` + `kill <pid>` (works on blocked
      pipe/wait readers too); since v0.3 a killed child is waitable
      via `wait`.

**Exit criteria:** a deliberately buggy `.mrp` program (null pointer deref,
buffer overflow) causes a **recoverable error**, not a triple fault / forced
reboot. — **ACHIEVED** (crashptr/crashstk/crashkmem demos + the v0.3
regression 11/11, including pool recovery after exits).

## Stage 3 — Ring 3 (User Mode) — the small version of the "final boss"
**Goal: `.mrp` programs are truly isolated from the kernel.**

- [ ] GDT with Ring 3 segments (code + data)
- [ ] TSS (Task State Segment) for privilege-level switches
- [ ] Syscalls via `int 0x80` (replacing the function-pointer table of
      Stages 0-1)
- [ ] A separate per-program stack, not borrowed kernel stack
- [ ] A page-fault handler that can tell a "misbehaving program" (kill the
      program, kernel survives) from a "real kernel bug"

**Exit criteria:** a `.mrp` program deliberately writing to a kernel address
(`*(int*)0x100000 = 0xDEAD;`) gets killed by the kernel, which keeps running
normally afterwards. This is the most important demo before continuing — if
it fails, do NOT go to Stage 4.

## Stage 4 — Porting a modified TCC (Tiny C Compiler)
**Goal: compile C INSIDE Equinox OS, not cross-compile from the host.**

TCC was chosen because its codebase is small & self-contained compared to
GCC/Clang, but it is still a big undertaking: TCC assumes a libc (malloc,
file I/O, etc.) and standard ELF/PE output.

- [x] **Milestone 1 — mtcc 0.1 (DONE, v6):** a single-file subset-C compiler
      (`mrp_user/tcc.cpp`) that runs AS a `.mrp` program inside Equinox OS —
      `run tcc.mrp hello.c` compiles & runs, `run tcc.mrp -c hello.c`
      produces a standalone `hello.mrp` that can be `run` directly. The
      compiled program's runtime = int 0x80 syscalls (no libc). Proven on a
      host harness (x86-32 interpreter): 4 samples × 2 modes passed
      byte-for-byte; the .mrp files validated independently. This is the
      "path confirmation": the .mrp format + syscall ABI + loader are ready
      for a real compiler. Details + limitations: `mrp_user/TCC.md`.
- [ ] **Milestone 2 — a more C-like language:** minimal preprocessor
      (#include from RAMFS / #define constants), simple structs, unsigned
      semantics, switch. (mtcc, or start from real tcc — decide based on
      how stable Stage 3 is.)
      — **mostly DONE by v0.3:** the mini-preprocessor splices
      `<morph.h>` / `<multitasking.h>` / `<fileio.h>` (+ #define /
      #ifdef) since v10.8, and FR-19 added the libc layer. Still open:
      structs, unsigned semantics, switch.
- [ ] Port TCC's backend codegen to emit the **`.mrp` format** directly
      (not ELF) — the most technical part; TCC has a `tcc_output_type`
      that needs to be pointed at a custom writer
- [x] **mtcc libc (FR-19, v0.3):** printf/sprintf/snprintf with a
      LOCAL render (true unsigned %u via binary long division), sscanf,
      ctype, string extras, qsort_int/qsort_str, rand/abs/strerror —
      the prelude is the libc; 54 self-test stages pass in-OS
      (`mtcc /test/libc.c`).
- [x] **Link-time honesty (FR-20, v0.3):** declared-but-undefined
      functions are reported with the first call site's line number
      (`mtcc /test/undeftest.c`).
- [ ] TCC needs a minimal libc to compile itself (self-hosting) — make sure
      the Stage 1 morphAPI is complete enough first
      — **the userland side is DONE (v0.3 `eqbuild` + TOOLS
      RELEASE):** the OS compiles its own tool sources with the in-OS
      mtcc at boot — 29 programs now (ls/cat/cp/... plus
      grep/head/tail/wc/sort/uniq/cut/tr/rev/nl/more/find/which/diff/
      strings/cksum/basename/dirname; the `help` builtin was removed — the command list lives in the README).
      Compiling *mtcc itself* in-OS remains.
- [ ] TCC needs file I/O to read `.c` sources and write output — depends
      on Stage 1
- [ ] TCC runs AS a `.mrp` program (ideally at Ring 3 from Stage 3, so a
      TCC crash on weird input does not take the kernel down)
      — **DONE for mtcc** (ring-3 .mrp since Stage 3; the userland
      tools it builds are also ring-3 .mrp programs).
- [ ] Test: compile & run a simple `hello.c` FROM INSIDE Equinox OS, the
      output being a valid `.mrp` that can be `./hello.mrp`-ed directly
      — **partially achieved by mtcc milestone 1** (hello.c → hello.mrp
      → run, without touching the host toolchain); proving it with real
      TCC remains. **v0.3 note:** `eqbuild` now does this for the
      WHOLE userland (29 tools), removing their sources afterwards.
      Golden-parity check added by the tools release: the in-OS mtcc
      produces BYTE-IDENTICAL .mrp images to the host compiler
      (cksum-verified on head.mrp/wc.mrp).

**Exit criteria:** `tcc hello.c -o hello.mrp && ./hello.mrp` works inside
Equinox OS itself, without touching the host toolchain at all.
— **mtcc satisfies this** (and `eqbuild` proves it at scale); real-TCC
porting remains optional.

## Stage 5 — DOOM (the furthest, heaviest target)
**Goal: DOOM shareware (`doom1.wad`) runs at a usable frame rate.**

The DOOM engine (`doomgeneric`/`doomgeneric-fbdev` is the usual hobby-OS port
base) needs:

- [ ] **Floating point** — DOOM uses `float`/`double` in several places
      (although much of the original is fixed-point). Make sure the FPU is
      initialized (`fninit` already runs in `kernel_main`) + a basic libc
      math (`math_pi.h` exists but needs extending)
- [ ] **A fast framebuffer** — the VESA linear framebuffer exists, but DOOM
      needs to blit hundreds of thousands of pixels per frame; profiling
      needed, likely `memcpy`/`fill_rect` optimizations (SIMD if the target
      CPU supports it, or at least unrolled loops)
- [ ] **Precise timing** — the current `get_tick()` is fine for simple
      animation, but DOOM's 35-tick-per-second game loop needs a more
      precise timer (PIT/APIC timer, not just polling)
- [ ] **Real-time input** — non-blocking keyboard state polling
      (non-blocking `get_key()` exists, but multi-key state is needed for
      moving + shooting simultaneously)
- [ ] **Memory footprint** — a DOOM WAD can be tens of MB; a 4 MB MRP arena
      is FAR from enough; the memory allocation needs a much larger design
      + possibly asset swap/streaming from "disk" (RAMFS)
- [ ] Port the `doomgeneric` I/O layer to the morphAPI (replace
      `DG_DrawFrame`, `DG_GetKey`, `DG_SleepMs` with Equinox OS versions)

**Exit criteria (realistic, not "60 FPS ultra"):** DOOM boots to the main
menu, can start level 1, moving + shooting + collision work, even at only
10-15 FPS. That is already a big achievement for a 32-bit hobby OS.

---

## Ground rules to avoid burnout / getting stuck

1. **One stage, one demonstrable milestone.** If it cannot be demoed ("run X,
   watch Y happen"), the scope is still blurry — split it further.
2. **Do not start Stage N+1 until Stage N has been stable for 2-3 sessions
   in a row without regressions.** Heap corruption / random reboots are
   "go back a stage" signals, not "add a feature to cover it up" signals.
3. **TCC and DOOM are EACH multi-month projects** if taken seriously (not
   part-time hobbyist level). Be realistic: treat each stage above as
   weeks-to-months of hobby work, not a few chat sessions.
4. If halfway through you feel "jumping straight to DOOM sounds more fun",
   that is a normal temptation but it ALWAYS ends in debugging hell without
   the Stage 1-3 foundations. Realistically, TCC (Stage 4) is closer than
   DOOM (Stage 5) — if you must pick one to pursue first, pick TCC.
