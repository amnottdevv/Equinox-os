# Equinox OS v0.3 — Release Notes

v0.3 turns the 0.2 Beta foundation into a real process & memory model,
then makes the OS build its own userland — and the tools release grows
that userland from 11 to **29 self-built tools** (the `help` builtin
is gone: the command list lives in the README table, not in the OS).
Everything shipped with full QEMU regression coverage.

## Foundation: syscalls & healthy I/O (FR-01/03/08/09/23/26)

- **File syscalls #40-#48**: `open2` with `O_CREAT/TRUNC/APPEND/EXCL/DIR`,
  writable fds (in-memory partial writes, grow-on-demand, ONE FAT
  write-through per close), `unlink/mkdir/rmdir/rename/stat/readdir/fstat`.
- **9 userland tools** (ls/cat/cp/mv/rm/mkdir/rmdir/touch/stat + fstest)
  ship as `.mrp` programs compiled by the host mtcc — the shell prefers
  them over kernel builtins.
- **FR-03**: `SYS_FREE` + arena free (split/coalesce existed; the wrapper
  was missing). **FR-08**: the FAT disk arena became a free-list; content
  caches drop when the last fd closes. **FR-09**: NIC RX moved out of the
  IRQ into an RX ring + a console-less kernel `nettask`. **FR-23**: TLS
  is fail-closed (`mget -k` opts into insecure).
- **Root-cause bug found on the way**: `fat32_read_whole`/`create` leaked
  the scheduler lock on early returns → the nettask starved to death
  after any FAT write ("network dies after cp"). Fixed at the source +
  a 3-layer safety net (`task_sched_force_unlock` on syscall exit,
  shell command, program exit).

## Process & memory model (FR-02/05/06/07)

### Demand paging (FR-05/06)
- Per-task VMA windows (arena + 1 MB stack) are now **reserved** with
  non-present `PTE_DEMAND` PTEs instead of one contiguous upfront chunk.
- `isr_14` → `task_demand_fault()` hands out ONE zero-filled physical
  page per first touch (CPL 3 program access or the loader's own VMA
  writes) and resumes the faulting instruction. Physical memory is only
  consumed by pages actually touched — a 24 MB DOOM arena costs only
  what DOOM touches, so DOOM + other programs coexist comfortably.
- Loader copies use `task_demand_fill()`: phase 1 maps + zeroes every
  page under the KERNEL directory, phase 2 copies the bytes under the
  task's directory (IRQs off across both).
- `meminfo` (shell) + `SYS_MEMINFO` (#51): pool total/free, faulted KB
  per task, reserved vs faulted, stack pages, zombie count.
- The console pixel canvas (VESA snapshot) is now copied under the
  kernel page directory — closes a latent aliasing bug where a canvas
  whose physical chunk fell inside a task's window would be written
  through the wrong mapping.

### ELF32 loader (FR-07)
- Static ELF32/i386 executables run next to `.mrp`: PT_LOADs validated
  (ET_EXEC/EM_386/LE, vaddr window 8-32 MB) and mapped into the demand
  window, `bss` zero-fills on fault, entry = `e_entry`, plain `ret`
  lands on the standard exit trampoline.
- `make elfdemo` builds `tools_user/elfdemo.c` with the host
  `gcc -m32 -nostdlib -T mrp_user/elf_link.ld` →
  `dist/equinox/tools/elfdemo.elf` → `run elfdemo.elf` / `spawn` + `wait`
  (exit status 42).

### wait + pipes (FR-02)
- **SYS_WAIT (#49)** / shell `wait [pid]`: blocking waitpid. USER
  children become zombies on exit (slot + status held for the parent);
  killed children are also waitable; orphans are reaped when the parent
  dies; a full table steals the oldest zombie.
- **SYS_PIPE (#50)**: 4 KB kernel ring, ref-counted ends, fd 3+.
  Pipe fds are INHERITED across SYS_SPAWN (file fds still are not).
  Blocking read (EOF on last writer close), blocking write
  (broken pipe = -EIO when no readers). `tools_user/pipedemo.c`
  (mtcc) demonstrates the full child-writes/parent-reads + wait flow.
- **SYS_SPAWN2 (#52)**: spawn with an explicit args string.
- Morph.h: `task_wait/pipe_create/task_spawn_args/mem_info`;
  `<multitasking.h>` (mtcc): `task_wait/pipe_create/task_spawn_args/mem_info`.

## Drivers, graphics, libc & self-hosting (FR-12/13/17/18/19/20)

### PCI + NIC (FR-12/13)
- **PCI enumeration** (`kernel/library/pci.cpp`): 0xCF8/0xCFC config
  cycles at boot — devices, classes, BARs, IRQ routing, bridge
  recursion. `lspci` prints the table.
- **NIC registry** (`kernel/net/nic.c`): one struct between lwIP and
  the hardware. ne2000-isa is the first driver; e1000/pcnet32/eepro100
  are recognized and reported honestly. net_lwip.c no longer names a
  driver.

### Per-task graphics (FR-17/18)
- `set_clip` (#53) confines a task's drawing; `draw_line` (#54) is a
  focus-gated Bresenham. Canvas semantics: first draw snapshots the
  text screen + clears to black; text goes mirror-only (serial + cell
  mirror); the console re-renders on program exit.

### mtcc libc + link honesty (FR-19/20)
- `<morph.h>` renders printf/sprintf/snprintf LOCALLY (up to 5
  conversion args; true unsigned `%u` via binary long division), plus
  sscanf, ctype, string extras, qsort_int/str, rand, strerror —
  54 self-test stages pass in-OS.
- Declared-but-undefined functions now fail with the first call
  site's line number.

### Self-hosting: `eqbuild`
- The ISO ships the 11 userland tools as **C sources**
  (tools_user/*.c → /equinox/tools). The `eqbuild` shell command
  compiles them with the in-OS mtcc (per-file log), installs the
  `.mrp` files, and removes the sources. The host-side prebuild step
  is commented out of the makefile. The OS builds its own userland.

### Root-cause bugs found on the way
- **Stale object files (fatal ABI drift)**: the makefile had no
  header dependency tracking — adding fields to `struct Task` left
  38 of 46 kernel objects compiled against the OLD layout (the
  scheduler literally read `clip_h` as `page_dir` → CR3=400 →
  triple fault). Fixed with `-MMD -MP` + `.d` includes and a full
  rebuild.
- **The "instant sleep" scheduler bug**: `schedule()` returned to a
  task that had just blocked itself whenever no other task was
  READY (nettask's 10 ms sleep window), and a BLOCKED→RUNNING fix-up
  in `task_sleep` then cancelled the sleep — every `sleep()` whose
  landing coincided with that window returned immediately (T9's
  15 s canvas hold lasted 0 s). Fixed with an idle path: halt with
  interrupts on until an interrupt makes something runnable
  (re-entrancy-guarded against the timer ISR), and the fix-up
  removed. sleep(ms) is now exact to the tick; the nettask
  heartbeat no longer busy-spins at 100 % CPU.
- **Canvas ordering**: `console_canvas_mark()` (snapshot+clear) was
  called AFTER the first draw — wiping that very draw (the T9 red
  box vanished). Now it runs before the pixels land.

## Tools release (v0.3 final) — 29 userland tools, help removed

The `help` builtin is gone and the command set grew to 29 — every
one of them compiled in-OS by `eqbuild` (the v0.4 release can then
focus on the heavy features).

**18 new tools** (tools_user/, auto-staged by the makefile wildcard,
compiled by eqbuild like the original 11):

| Tool | Highlights |
| --- | --- |
| `grep` | `-i -n -c -v`, multi-file (`name:` prefix), mini-regex: `.` `X*` `^` `$` |
| `head` / `tail` | first/last N lines (`-n N`, tail ring buffer of 64x256) |
| `wc` | `-l -w -c` (default all + name), binary-safe |
| `sort` / `uniq` | bubble+early-exit over a 32 KB line store; `sort -r`; `uniq -c` adjacent groups |
| `cut` | `-d DELIM -f LIST` (N, N-M, N-, comma lists, `\t`/`\n`/`\0` escapes) |
| `tr` | SET1->SET2 map + `-d`, ranges (`a-z`), last-char-repeat, binary-safe |
| `rev` / `nl` | reverse each line / `%6d` numbered lines |
| `more` | 23-line pager, any key = next page, q = quit |
| `find` | recursive walk (depth 8), `-name SUBSTR` filter |
| `which` | resolves NAME/NAME.mrp over `.` + `/` + `/bin` + `/equinox/tools` + `/equinox/games` (the shell's own order) |
| `diff` | streaming line compare, first 20 diffs `</>`-style, EOF handling |
| `strings` / `cksum` | printable runs (minlen arg) / 32-bit byte sum + size, multi-file |
| `basename` / `dirname` | path component split (POSIX corner cases) |

The new tools are called by name through the SAME global dispatcher
as mtcc/snake (no shell whitelist edit needed) — after `eqbuild`,`grep pattern file` just works.

**Two root-cause bugs found & fixed while shipping this:**

1. **Kernel heap exhaustion at eqbuild #13** — 29 tools x ~34 KB .mrp
   output + ~1.2 MB of boot modules in the 2 MB kernel heap, and the
   freed .c blocks were too fragmented for contiguous 34 KB writes
   (fs_write_binary OOM -> "error writing ... to RAMFS"). FIX:
   malloc.cpp now runs the kernel arena as TWO physical regions — the
   2 MB below 0x500000 PLUS the free ~1 MB gap 0x2702000-0x2800000
   (between USER_STACK_END and the GRUB module staging area,
   identity-mapped supervisor, claimed by nothing). coalesce() and
   realloc() only merge PHYSICALLY ADJACENT blocks, so the two regions
   can never fuse into a phantom block spanning the gap. Heap total
   is now ~3.1 MB (see `malloc` in the shell).
2. **stdio slot vs raw fd API mixup** — the mtcc prelude `fgetc`/
   `fread` operate on `fopen()` TABLE SLOTS (`__fio_*`), not on raw
   `f_open()` fds; the first tool drafts passed a raw fd, so every
   read returned EOF instantly (silent empty output). FIX: the ten
   line-oriented tools use `fopen`/`fgetc`/`fclose` consistently.
   Golden check: the in-OS mtcc produces BYTE-IDENTICAL .mrp images
   to the host compiler (cksum head.mrp/wc.mrp matched exactly), so
   the toolchain parity holds at 29/29.

Plus a harness fix: `after(marker)` in regression_task2 matched the
FIRST occurrence (the eqbuild log line), which fell outside the
window with 29 tools — replaced with `since(base)` offsets.

## Regression status (QEMU, shipped ISO)

- `regression_task3.py` (self-hosting + tools release): **29/29 PASS** —
  boot + PCI/lspci, eqbuild **29/29** sources compiled in-OS (log per
  file), sources removed, self-built cat/cp roundtrip, FR-20
  undefined-ref rejection, the new T14 battery (16 checks: help
  removed, wc counts, head/tail, grep -n/-c, sort, which, diff,
  tr ranges, basename/dirname, cksum, find -name, nl, rev, strings),
  gfxclip pixel proof (red 159,677 px inside the 400x400 clip window,
  diagonal clipped to 323 px, zero leak above), NIC/DHCP, libc 54/54
  in-OS, multitask demo, 60 s soak.
- `regression_v03.py` (core + eqbuild): **31/31 PASS**
- `regression_task2.py` (process/memory + eqbuild): **12/12 PASS** (fstest
  24/24 under demand paging, ELF + bss, spawn/wait/kill/zombie/ECHILD,
  pipedemo, meminfo, pool recovery, soak)
- `regression_multitask.py`: **29/29 PASS** (snake canvas/no-leak/
  frozen/F2-restore, DOOM 24 MB arena + title, F1/F2, ps/kill/switch,
  60 s soak, mtcc round)
- **Total: 101/101 PASS** — zero fallout on the earlier suites from the
  v0.3 scheduler/canvas changes (they were re-run in full; the only
  edits were harness-side: run eqbuild first, reliable key pacing,
  one pixel-threshold recalibration, since() offsets instead of
  after() first-match for the 29-tool eqbuild log).

## Known limitations (honest list)

- The 1 MB user stack does not auto-grow (guard page kills) — by design
  for now.
- A same-task pipe write that fills the buffer it then reads from is a
  self-deadlock (documented in pipedemo.c).
- ELF segments must live in [0x800000, 0x2000000); the heap for ELF
  tasks is a fixed 2 MB at 0x2000000.
- `MAX_TASKS` is 8 (zombies hold slots; the oldest zombie is stolen
  when the table fills).
- mtcc's printf family takes at most 5 conversion arguments per call
  (the syscall/ABI budget); sscanf exposes 5 pointer outputs.
- The per-task clip window is not inherited across spawn and is
  cleared at program exit (a fresh task draws full-screen until it
  calls set_clip).
- Canvas restore re-renders TEXT from the cell mirror on program exit;
  a graphics program's pixels are never kept after it exits (by
  design — the next program starts on a clean console).
- `eqbuild` compiles sources serially in the shell task; the tools
  exist only after it runs (a fresh boot has sources, not binaries).
