# Equinox OS

Equinox OS is a 32-bit x86 hobby operating system with a monolithic kernel written in C++, C and NASM. It boots through GRUB Multiboot into a VESA 1360x768 linear framebuffer, runs ring-3 processes with demand paging, and — the part that defines the project — **compiles its own userland from C source inside the OS** with its built-in compiler, `mtcc`.

Beyond the kernel, the system ships with a working set of everyday OS facilities: a shell with pipes, globbing, redirection and scripting; a package manager (`eggkg`) that fetches, compiles and installs software from a Git-hosted package repository over HTTPS; a validated configuration system (`.ecf` files with a `set` builtin); a FAT32 disk tool (`Qfs`) that can format a disk and install a bootable GRUB; a TCP/IP stack with an HTTPS client and server; an LVGL desktop plus a ThorVG-powered vector desktop; and DOOM running from a demand-paged arena off the FAT32 disk.

| | |
| --- | --- |
| **Release** | 0.4 Beta |
| **Target** | i686 (32-bit), QEMU or real hardware |
| **Boot** | GRUB Multiboot (`kernel.elf` + boot modules) |
| **Memory** | 256 MB recommended (QEMU `-m 256`); 64 MB minimum |
| **Language** | C++17 / C / NASM |
| **Display** | VESA 1360x768x32, VGA text fallback |

```text
root::users / $ equinoxinstall                 # the OS builds its own userland in-OS
root::users / $ mtcc /test/hello.c             # compile + run C, inside the OS
root::users / $ eggkg update && eggkg install bash -y
root::users / $ grep -n printf /test/hello.c | tr a-z A-Z > /mnt/out.txt
root::users / $ mget https://github.com/octocat/Hello-World   # HTTPS -> file
root::users / $ doom -iwad /mnt/doom1.wad      # DOOM straight off the disk
```

## Table of contents

1. [Overview](#overview)
2. [Requirements](#requirements)
3. [Quick start](#quick-start)
4. [Architecture](#architecture)
5. [Multitasking & process model](#multitasking--process-model)
6. [Memory management](#memory-management)
7. [Executable formats: MRP and ELF32](#executable-formats-mrp-and-elf32)
8. [mtcc — the in-OS C compiler](#mtcc--the-in-os-c-compiler)
9. [Build recipes: ruf v3 and `mtcc -make`](#build-recipes-ruf-v3-and-mtcc--make)
10. [Self-hosting with `equinoxinstall`](#self-hosting-with-equinoxinstall)
11. [Packages: `eggkg` and the Eggkg-l repository](#packages-eggkg-and-the-eggkg-l-repository)
12. [Configuration: `.ecf`, `set` and eqshell scripts](#configuration-ecf-set-and-eqshell-scripts)
13. [Disk management: `Qfs`](#disk-management-qfs)
14. [System call interface (54 syscalls)](#system-call-interface-54-syscalls)
15. [Storage & filesystems](#storage--filesystems)
16. [Networking](#networking)
17. [Graphics, desktop & games](#graphics-desktop--games)
18. [Drivers](#drivers)
19. [Shell command reference](#shell-command-reference)
20. [Building from source](#building-from-source)
21. [Testing](#testing)
22. [Known limitations](#known-limitations)
23. [Repository layout](#repository-layout)
24. [Third-party components](#third-party-components)

---

## Overview

| Area | What you get |
| --- | --- |
| **Processes** | Ring-3 user programs via TSS-based privilege transitions and `int 0x80`; a private page directory per task; kernel memory is unreachable from user code. |
| **Multitasking** | Preemptive round-robin scheduler (100 Hz), up to 8 tasks, per-task FPU state, virtual consoles (F1 / F2), `spawn` / `wait` / `kill`, zombies, and kernel pipes. |
| **Demand paging** | Task windows are *reserved*, not allocated. A page fault hands out one zero-filled page on first touch: a 24 MB DOOM arena costs only what DOOM actually uses. |
| **Executables** | **MRP1** (18-byte header, checksummed flat binary) and **static ELF32/i386**, both loaded into the demand-paged window. |
| **Compiler** | **mtcc**: a single-pass C compiler with direct x86-32 code generation that runs inside the OS, with a 13-module libc spliced through `<morph.h>`. |
| **Build recipes** | **ruf v3** `.ruf` files: variables with `:=`, `$name` expansion, `copy` / `move ? to` post-build actions — driven by `mtcc -make`. |
| **Self-hosting** | `equinoxinstall` compiles the shipped tool and game sources from C in-OS and installs the results. |
| **Packages** | **eggkg**: update / install / remove / list / search / info / sync; downloads sources over HTTP or HTTPS, builds with `mtcc -make`, installs to `/bin`, tracks them in `installed.db`, verifies sha256 when `index.idx` is present. |
| **Configuration** | **`.ecf`** (INI-lite) with schema validation, an overlay mechanism, and the `set` builtin; **`.es`** shell scripts with command logging. |
| **Syscalls** | 54 append-only syscalls (#1–#54): console, files, processes, pipes, memory, graphics, audio, input, networking. |
| **Storage** | ATA PIO driver (LBA28/LBA48, ATAPI detection, MBR), full **FAT32 read/write** with long file names, write-through consistency, and a RAM filesystem populated from GRUB modules. |
| **Networking** | lwIP 2.1.3: DHCP, DNS, ICMP, TCP. HTTP **and HTTPS** client (`mget`, BearSSL, fail-closed TLS), an in-process fetch transport for eggkg, and an HTTP server on port 80. |
| **Graphics** | 1360x768x32 VESA framebuffer, LVGL 8.3 apps (file manager, settings), an EquiX desktop built on ThorVG vector rendering, per-task clip windows, ANSI color output in the console. |
| **Games** | Snake, Breakout, Pong (self-hosted C sources) and **DOOM** loaded from the FAT32 disk. |

### What is new in 0.4 Beta

- **Dynamic memory:** the RAM top is read from the multiboot information instead of being hardcoded. Paging maps 16–128 MB depending on what is installed, the GUI arena grows with it, and the standard configuration is now **256 MB** (the `info` banner reports `RAM: 256 MB`; all makefile run targets pass `-m 256`).
- **eggkg package manager** (ring-0 builtin) with the [Eggkg-l](https://github.com/amnottdevv/Eggkg-l) repository: `package.list` (format v0) + optional `index.idx` (v1: version / sha256 / size / mirrors), pure-C SHA-256 verification, `/bin` system path, `installed.db`, reverse-dependency guards, and boot-time auto-activation of the `bash` package.
- **`.ecf` configuration + `set` builtin:** schema-validated keys (`net.driver`, `base.path`, `dependencies.bash`, `eggkg.server`, …), write-through patching of the active config file, `active.conf` overlay with `.base.ecf` templates.
- **eqshell scripts (`.es`)** run with `set -x FILE`: `[Eqshell]` header, `Log=True` directive producing an 8 KB transcript at `/eqshell.log`, nesting up to 3 levels.
- **ruf v3 recipes** with variables (`name := "value"`, case-insensitive `$name` expansion) and queued `copy A [& B] ? D` / `move A [& B] ? D` post-build actions; executed by `mtcc -make <file.ruf>`.
- **`equinoxinstall` grew `-build`:** `-build <file.ruf>` (via `mtcc -make`), `-build <tool>` (single tool), `-build *.ruf` (single-star glob). The old `eqbuild` command is gone.
- **`Qfs` disk tool:** `-list-disk`, `-t hdX -format fat32`, and `-install-boot [hdX]` (writes GRUB `boot.img` + `core.img`, generates `grub.cfg`, verifies the result).
- **Shell upgrades:** line-level pipe `|`, output/input redirection `> >> <`, single-star glob `*`, arrow-key history; `eqbash` gained `cdir` / `cfile` / `ccfile` / `save` / `delfile` / `deldir` / `pren` plus `lf` / `showf` aliases; the nine bash-class tools (ls, cat, cp, mv, mkdir, rmdir, rm, touch, stat) moved out of the base ISO into the eggkg `bash` package.
- **`eqgu`**: the built-in editor for `.c` files — re-compiles the file on save and shows compiler diagnostics in place.
- **morph pkg API + ANSI colors:** `libc/pkg.c` adds 20 package-builder functions (`pkg_copy`, `pkg_bin_install`, `pkg_db_add`, …); the console parses ANSI CSI sequences (SGR colors, cursor home, clear line/screen), and `<morph.h>` exposes `ansi_fg`, `ansi_bg`, `ansi_goto`, … to programs.
- **EquiX desktop (ThorVG):** a full-screen vector desktop (`desktop`) with demo/benchmark/info commands `tvgdemo`, `tvgbench`, `tvginfo`, alongside the existing LVGL apps.

---

## Requirements

- **RAM:** 256 MB recommended — this is what the makefile run targets use (`-m 256`) and what the `info` banner reports. The system still boots and runs with 64 MB; fixed kernel regions (module staging at 40 MB, the FAT32 cache arena at 52 MB) set the practical floor. Installed memory is detected from the multiboot information at boot and logged as `RAM detected: N MB`.
- **QEMU** 4.x or newer for the prebuilt images (`qemu-system-i386`).
- To build from source: `build-essential`, `gcc-multilib`, `g++-multilib`, `nasm`, `grub-pc-bin`, `grub-common`, `xorriso`, `mtools`, `python3` (see [Building from source](#building-from-source)).

## Quick start

### Run the prebuilt images

You need only [QEMU](https://www.qemu.org/) and the two images (`equinox.iso`, `disk.img`):

```sh
qemu-system-i386 -m 256 -boot order=d -cdrom equinox.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

- `-boot order=d` boots the ISO. The demo disk carries an MBR without boot code.
- Without `disk.img`, drop the `-drive` line: the OS runs entirely from the RAM filesystem.
- The guest gets `10.0.2.15` by DHCP; the QEMU host is `10.0.2.2`, DNS is `10.0.2.3`.
  The guest web server is reachable from the host at `http://localhost:8080/`.
- To boot from the hard disk instead (after `Qfs -install-boot`), use `-boot order=c`.

### First five minutes

```text
root::users / $ ls /mnt                          # FAT32 disk, auto-mounted
root::users / $ equinoxinstall                   # wizard: build userland in-OS (or: equinoxinstall -compile)
root::users / $ mtcc /test/hello.c               # compile + run a C program
root::users / $ ps                               # task table
root::users / $ spawn /equinox/tools/bgcount.mrp # run a task in the background
root::users / $ eggkg update                     # fetch the package index (Eggkg-l)
root::users / $ eggkg install bash -y            # 19 coreutils into /bin
root::users / $ doom -iwad /mnt/doom1.wad        # DOOM from the disk
```

Press **F1** to open a new virtual console/shell and **F2** to return to the previous one. The shell supports single-star globbing (`ls /equinox/tools/*.c`), pipes (`cat file | grep text`), redirection (`echo hi > /mnt/a.txt`, `>>` to append, `<` for input), and arrow-key history.

### Disk image contents

`disk.img` is a 64 MB IDE image: MBR plus one FAT32 partition (type `0x0C`, label `EQDISK`) holding `README.TXT`, a long-file-name sample, `docs/`, `bin.dat` and `doom1.wad`. Everything you write to `/mnt` is written through to the image.

---

## Architecture

```text
GRUB (Multiboot)  ->  kernel.elf + boot modules (mtcc.mrp, games, tool sources, WAD)
      |
boot/start.asm       VBE 1360x768x32 (VGA text fallback), GDT, module staging @ 0x2800000
      |
kernel/kernel.cpp    RAM detection (multiboot) -> heap arenas -> paging -> IDT/PIC/PIT
      |              -> console -> PS/2 -> PCI -> scheduler -> RAMFS from modules
      |              -> ATA + FAT32 (/mnt) -> NIC + lwIP + nettask (DHCP) -> eggkg boot check
      v
kernel/shell.cpp     prompt "root::users / $"   (eqbash: builtins + global tool dispatch)
      |
      +-- ring-3 programs: .mrp / .elf  <--- int 0x80 --->  syscall layer (54 calls)
      +-- equinoxinstall -> mtcc (in-OS) -> self-built userland
      +-- eggkg -> net fetch -> mtcc -make -> /bin
```

The kernel is monolithic. Every subsystem mirrors its log to serial COM1, which makes the whole system scriptable from the host (the regression suites drive QEMU this way). The boot log lists each subsystem as `[ OK ]` together with every file loaded into the RAM filesystem.

More detail lives in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) (Indonesian).

---

## Multitasking & process model

**Scheduler.** Preemptive round-robin driven by the 100 Hz timer (quantum: 1 tick = 10 ms). Each task owns a 16 KB kernel stack and a separate 16 KB interrupt stack (the `TSS.ESP0` target), plus its own `fxsave` FPU state. When nothing is runnable the scheduler halts with interrupts on until an IRQ makes a task ready, so `sleep(ms)` is exact to the tick and the network task never busy-spins.

**Limits.** `MAX_TASKS = 8`, 16 file descriptors per task.

**Per-task state.** Every task carries its own file-descriptor table, current directory, argument string, private user arena and page directory, virtual console, and graphics clip window. Syscalls always act on the calling task's context.

**Lifecycle.**

```text
spawn / spawn2 --> READY <--> RUNNING --> exit --> ZOMBIE --wait--> slot freed
                     ^  |                            (status held for the parent)
                     |  +--> SLEEP / BLOCKED (pipe, wait, console focus)
```

- A USER child becomes a **zombie** on exit: its slot and exit status are kept until the parent calls `wait`.
- Killed children are also waitable. Orphans are reaped when the parent dies. If the task table is full, the oldest zombie is reclaimed.
- Errors: `SYS_ECHILD` (-13) for `wait` with no children, `SYS_EPERM` (-14).

**Pipes.** `SYS_PIPE` creates a 4 KB kernel ring buffer with reference-counted ends. Pipe descriptors are inherited across `spawn`. Reads block until data arrives and return EOF when the last writer closes. Writes block on a full buffer, and writing with no readers returns a broken-pipe error (`-EIO`). `pipedemo` (built by `equinoxinstall`) demonstrates a child writing to its parent.

**Virtual consoles.** Each task has its own cell mirror and cursor. Background tasks print to their own console instead of overwriting the screen. **F1** opens a new shell on a new console, **F2** focuses the previous one, and `switch <n>` selects a console explicitly. Graphics programs use a per-task *canvas*: the first draw snapshots the text screen and clears to black; the console is redrawn on exit.

---

## Memory management

Physical RAM is detected from the multiboot information at boot (`paging_set_ram_top`). Paging identity-maps 16 to 32 page tables (64–128 MB) depending on installed RAM; the region above the map is deliberately left unmapped. With the recommended 256 MB the paging map is capped at 128 MB and the dynamic GUI arena takes the remainder up to that cap.

| Region | Address / size | Purpose |
| --- | --- | --- |
| Kernel heap | 2 MB at `0x300000-0x500000` **plus** a ~1 MB extension `0x2702000-0x27FFFFF` (about 3 MB total) | Free-list allocator with split, coalesce and canaries. Only *physically adjacent* blocks are ever merged. |
| User page pool | `0x500000-0x2600000` (33 MB) | Bitmap of 4 KB physical pages handed to tasks on demand. Also the default MRP program window. |
| User stack | 1 MB, `0x2602000-0x2702000`, guard page below | Supervisor-only guard at `0x2601000` detects stack overflow. |
| Module staging | `0x2800000`, 12 MB | GRUB modules (`mtcc.mrp`, `doom.mrp`, tool sources, WAD). Big files are referenced zero-copy. |
| FAT32 disk arena | 8 MB at `0x3400000` | File caches for FAT32 files, outside the kernel heap (free-list allocator). |
| GUI arena | `0x3400000` up to `min(RAM top, 128 MB)` — dynamic | Framebuffer canvases and the desktop (LVGL / EquiX). Was a fixed 10 MB before 0.4 Beta. |
| Task window | arena at `0x500000+` (33 MB pool; 24 MB reserved for DOOM) plus the 1 MB stack | Reserved with `PTE_DEMAND`; faulted in per page. |
| ELF window | segments in `[0x800000, 0x2000000)`, 2 MB heap at `0x2000000` | Static ELF32 images. |

**Demand paging.** Reserving a window marks its PTEs non-present with a `PTE_DEMAND` marker. The first access raises `#PF`; `isr_14` calls `task_demand_fault()`, which allocates one physical page, maps it, zero-fills it through the faulting address, and resumes the instruction. Fresh `.bss` therefore reads as zero, and physical memory is consumed only for pages actually touched. Loader copies use a two-phase fill (map and zero under the kernel directory, then copy under the task's directory). `meminfo` (shell) and syscall #51 report pool totals, faulted KB per task, reserved versus faulted, stack pages and zombie count.

**Guard pages.** A supervisor-only guard page sits below each user stack. Overflow kills the offending program and the shell keeps running. The `crash*` programs exercise this: division by zero, NULL dereference, a write into kernel memory from ring 3, and runaway recursion.

---

## Executable formats: MRP and ELF32

### MRP1 (Equinox Runnable Program)

A flat binary behind an 18-byte header. A 4-byte magic and an additive checksum let the loader reject truncated or corrupt files before jumping into them.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `M` `R` `P` `1` |
| 4 | 1 | format version (1) |
| 5 | 4 | entry offset (relative to the code start) |
| 9 | 4 | code size |
| 13 | 1 | flags (`MRP_FLAG_NEEDS_GUI` reserved) |
| 14 | 4 | checksum (rotate-xor over the payload, non-zero seed) |

Programs link at `0x500010` (`mrp_user/link_mrp.ld`). The loader validates, maps the per-task demand window, copies the image, and enters ring 3. Programs use their own per-task arena for `malloc` / `free`; the arena is discarded on exit. `mrp_user/mrp_pack.py` compiles, links and wraps a hosted `.cpp` into a `.mrp`; `mtcc -c` produces one natively (with fixups patched against `0x500010`).

### ELF32

Static `ET_EXEC` / `EM_386` / little-endian executables run beside MRP. The loader validates the program headers (at most 16), requires every `PT_LOAD` segment and the entry point to fall in `[0x800000, 0x2000000)`, maps the span into the demand window, and zero-fills `.bss` on fault. A plain `ret` from `_start` lands on the standard exit trampoline. Build one on the host with:

```sh
gcc -m32 -ffreestanding -fno-pie -static -no-pie -nostdlib -O2 \
    -T mrp_user/elf_link.ld tools_user/elfdemo.c -o elfdemo.elf     # or: make elfdemo
```

Run it with `elfdemo.elf`, `./elfdemo.elf`, or `spawn` + `wait` (exit status 42).

Both formats are launched the same way (`run <file>`, `./file`, or by bare name from any directory — including subpaths such as `equinox/games/snake.mrp`), and both support `spawn`, `wait` and pipes.

---

## mtcc — the in-OS C compiler

`mtcc` (self-reported as `mtcc 0.3`) is a tcc-style, **single-pass** compiler: lexer, recursive-descent parser, and direct x86-32 code generation with no AST. It ships as an ordinary `.mrp` module and runs inside the OS.

```text
mtcc program.c       compile and run immediately (like tcc -run)
mtcc -c program.c    compile to program.mrp, then run it
mtcc -make b.ruf     multi-file build from a .ruf recipe:
                     echo / src / exclude / out / lib — self-hosting
                     without equinoxinstall
mtcc --debug prog.c  verbose compiler info (file, code/data sizes)
flags: -c compile-only   -q quiet   --lib no-main check   -make <file.ruf>
```

**Language subset:** `int`, `char`, `void`, one- and two-level pointers, 1-D arrays; `if/else`, `while`, `do-while`, `for` (with a declaration in the initializer), `return`, `break`, `continue`; every assignment and arithmetic/bitwise/logical/relational operator, pre/post `++ --`, and the ternary operator; global scalars and arrays with constant, list and string initializers; recursion and forward prototypes (up to 8 parameters). Not supported: `struct` / `union`, floating point, `switch`, `sizeof`, `typedef`, 2-D arrays, variadic functions, `static` locals, unsigned semantics.

**Preprocessor:** `#include <morph.h>` (aliases: `<stdio.h>`, `<stdlib.h>`, `<string.h>`; nesting depth max 8), `<multitasking.h>`, `<fileio.h>`, `"file.h"` from RAMFS, object-like `#define`, `#undef`, `#ifdef` / `#ifndef` / `#else` / `#endif` (function-like macros are rejected with a clear message).

**Diagnostics:** calling a declared-but-undefined function fails the build and reports the line number of the first call site.

### Library surface

The libc lives in `/equinox/libc` as **13 modules** spliced together by the `<morph.h>` master include (v0.9.3 added the `pkg` module). Roughly 98 public functions are available through the include, plus 57 compiler builtins that work without any include.

| Header / source | Contents |
| --- | --- |
| `<morph.h>` (13 modules) | **string/memory:** `strlen strcmp strncmp strcpy strncpy strcat strncat strchr strrchr strstr strdup strtok strspn strcspn strcasecmp memcpy memset memmove memcmp memchr` · **convert/parse:** `atoi strtol itoa utoa sscanf abs` · **ctype (13):** `isalnum isalpha iscntrl isdigit isgraph islower isprint ispunct isspace isupper isxdigit tolower toupper` · **heap:** `malloc free calloc realloc` · **stdio:** `printf sprintf snprintf puts fputs fopen fread fwrite fseek ftell fclose fgetc fputc remove rename` · **misc:** `strerror rand srand qsort_int qsort_str time getenv abort` · **graphics:** `draw_line set_clip` |
| `<morph.h>` pkg module (v0.9.3) | 20 package-builder functions layered on syscalls #40–#48: `pkg_is_dir pkg_is_file pkg_exists pkg_size pkg_join pkg_base pkg_mkdir_p pkg_copy_file pkg_copy_path pkg_copy pkg_remove pkg_move pkg_write pkg_append pkg_read pkg_list pkg_count pkg_manifest pkg_bin_install pkg_db_add` |
| `<multitasking.h>` | `task_spawn task_spawn_hint task_yield task_wait task_kill task_pid task_list pipe_create mem_info` |
| `<fileio.h>` | `f_open f_stat f_fstat f_readdir f_mkdir f_rmdir f_unlink f_rename f_free` with `F_*` open flags |
| **Built-ins** | thin syscall wrappers available without any include: `print printint getkey readline write open read close malloc sleep gettick getpid exit exec getargs mkfile lseek ring`, `file_open file_read file_close file_write file_read_all file_size file_exists`, `net_info net_ping`, and the game API `fb_info put_pixel fill_rect draw_line set_clip pollkey mouse_state spk_tone spk_silence snd_beep` |
| **ANSI helpers (v0.9.3)** | `clear_screen ansi_reset ansi_bold ansi_fg(c) ansi_fg_bright(c) ansi_bg(c) ansi_default ansi_goto(row,col) ansi_clear_line` plus `ANSI_BLACK..ANSI_WHITE` color constants |

`printf` / `sprintf` / `snprintf` render **locally** inside the program (`%d %i %u %x %X %o %p %c %s`, with width and flags; true unsigned `%u` over the full 0..4294967295 range), taking up to 5 conversion arguments per call.

### Quality bar

- The compiler emits a **closed instruction set**. A host-side x86-32 interpreter (`scripts/tcc_host_test/`, run with `make test`) executes exactly that set, so any codegen bug outside it is caught immediately.
- **Parity:** the in-OS compiler produces byte-identical `.mrp` images to the host build of the same source.
- Documentation: [`mrp_user/TCC.md`](mrp_user/TCC.md), [`mrp_user/workflow_mrp.md`](mrp_user/workflow_mrp.md), [`docs/EGGKG.md`](docs/EGGKG.md).

---

## Build recipes: ruf v3 and `mtcc -make`

A `.ruf` file is a small build recipe read by `mtcc -make`. One line per directive, `#` starts a comment, and `key value`, `key = value` and `key := value` are all accepted:

```text
echo <text>        print a log line while parsing
src <dir>          source directory (repeatable); a bare path line is read as src
exclude <name>     skip this entry (directory or .c file, any level)
out <dir>          write every .mrp here (default: next to the sources)
lib                all jobs use --lib mode; files under a "libc" directory always do
copy A [& B] ? D   copy files/directories (recursive) to D
move A [& B] ? D   move them to D — both run only AFTER all jobs succeed
```

**v3 additions (0.4 Beta):** variables and post-build actions.

```ruf
pkg    := "bash"
source := "/equinox/.local/$pkg/src"
Target := "/equinox/.local/$pkg"
echo   "eggkg: Build $pkg (19 coreutils)"
src    $source
out    $Target
```

- Variable names are looked up **case-insensitively** (`$pkg` and `$Target` above), and expansion happens in every directive value. A variable must not be named after a directive word (`echo`, `src`, `exclude`, `out`, `lib`, `copy`, `move` — directives win).
- `copy` / `move` take multiple sources separated by `&` (up to 4) and use `?` as the "to" keyword. They are queued and executed in order **after** every compile job succeeds, so broken artifacts never spread.
- The walker recurses to depth 6 and turns only `.c` files into jobs. Limits: 128 jobs, 8 source dirs, 16 excludes, 16 variables, 16 copy/move actions.
- Recipe versions: **v1** is the flat `echo/src/exclude/out/lib` form; **v2** wraps it in INI sections (`[package]`, `[build]`, `[install]`, optional `[src:file.c]` heredocs) and is understood by eggkg, which strips it back to v1 before invoking mtcc; **v3** adds the variables and `copy`/`move ?` actions shown above. v3 recipes require OS 0.4 Beta (mtcc v0.9.3) or newer.

---

## Self-hosting with `equinoxinstall`

The ISO ships the userland as **C source** — `/equinox/tools` (20 tools), `/equinox/games` (snake, breakout, pong), and the 13-module libc — not as binaries. `equinoxinstall` compiles them with the in-OS `mtcc`:

```text
equinoxinstall              installation wizard, 4-5 phases
equinoxinstall -compile <dir>   compile userland only (dir must hold libc/ + tools/)
equinoxinstall -build <file.ruf>   build via mtcc -make
equinoxinstall -build <name>       build one tool (mtcc -c /equinox/tools/<name>.c)
equinoxinstall -build <pola*>      e.g. *.ruf — build every matching recipe
```

The **wizard** walks through: `[1/4]` pick a target disk (`hda`–`hdh`, or build in place), `[2/4]` prepare the target volume (format to FAT32 `EQUINOXBASE` if needed, export the `/equinox` + `/user` + `/test` layout plus `kernel.elf`), `[3/4]` choose the NIC driver (writes `equinox/conf/system.ecf`), `[4/4]` build the userland with a two-job mtcc pool (libc check-compile, then tools and games, with a live progress bar), and optionally `[5/5]` install the GRUB bootloader onto the target disk (`Qfs -install-boot`) so it boots without the CD.

Compiling `mtcc` itself from inside the OS is not supported (self-compilation); the honest answer is that `mtcc.mrp` is packed from the host with `make mtcc`. A failed compile keeps its source, so the command can simply be re-run. The RAM filesystem is rebuilt from the ISO on every boot, so each fresh boot carries the sources again.

Since 0.4 Beta the base ISO is intentionally slim: the nine bash-class tools (`ls cat cp mv mkdir rmdir rm touch stat`) moved to the eggkg **`bash`** package. Until you install it, the shell's own builtins and aliases (`lf`, `showf`, `copy`/`cp`, `del`, `cdir`, `cfile`, `save`, …) cover the same operations.

---

## Packages: `eggkg` and the Eggkg-l repository

`eggkg` is a Gentoo-style package manager implemented as a **ring-0 shell builtin** (`kernel/eggkg.cpp`), so networking and spawning `mtcc` never leave the kernel. Its default server is the [Eggkg-l](https://github.com/amnottdevv/Eggkg-l) GitHub repository.

```text
eggkg update [source]    download package.list (+ index.idx);
                         source = URL or local path
eggkg install <name> [-y]   fetch sources, build with mtcc, install to /bin
eggkg remove <name> [-y]    delete the files recorded in installed.db
eggkg list                installed packages
eggkg search <pattern>    search the index
eggkg info <name>         package metadata
eggkg sync                re-sync .local -> /bin (also runs at boot when
                          [dependencies] bash=true in system.ecf)
```

- **Repository formats.** `package.list` (format **v0**) maps a package name to a list of source URLs (`name = ["url1", "url2", ...]`; GitHub `blob` links are rewritten to `raw.githubusercontent.com` automatically, and local paths serve as offline repos). An optional `index.idx` (format **v1**) adds per-package `version / desc / sha256 / size / url / mirror / deps`; mirrors are tried in order (up to 3).
- **install** runs four stages: read the index, download (archive packages up to 384 KB are extracted from embedded `[src:*]` heredocs; loose sources are fetched file by file), build via `mtcc -make` on the package's `.ruf` recipe (a canonical recipe is synthesized when the server ships none), and install the resulting `.mrp` into **`/bin`** (zero-copy relink on RAMFS, copy on FAT32) with the file list recorded in `installed.db`. sha256 is verified whenever `index.idx` provides it.
- **remove** refuses to remove a package that others depend on, and removing `bash` offers to flip `[dependencies] bash=false` in `system.ecf`.
- **Boot integration:** when `system.ecf` contains `[dependencies] bash=true`, the kernel runs `eggkg sync` automatically at shell start, so packages installed into the workspace survive as long as `/equinox/.local` does.
- **Workspace:** `/equinox/.local/<pkg>/` holds `build.ruf`, `src/` and the build output; override the local dir and server with the `eggkg.local` / `eggkg.server` config keys (see [Configuration](#configuration-ecf-set-and-eqshell-scripts)). Transport is an in-process HTTP/HTTPS fetch (`net_eggkg_fetch`, BearSSL TLS 1.2, `User-Agent: equinox-eggkg/0.9`) — `mget` is not involved.

**The Eggkg-l repository** currently indexes **11 packages** (42 source URLs):

| Package | Description |
| --- | --- |
| `bash` | shell + coreutils class — 19 programs (`cat cp cut diff dirname find grep head mkdir more mv rev rm sort tail tr uniq wc which`) |
| `eqfetch` | mini neofetch |
| `emu-ch8` | CHIP-8 emulator (includes the `bounce.c8` ROM) |
| `wolf` | ray-cast maze shooter |
| `space` | space invader |
| `tetris` | falling blocks |
| `flappy` | flappy-bird style game |
| `ppmview` | PPM (P6/P3) image viewer (includes `demo.ppm`) |
| `bfi` | Brainfuck interpreter (includes `hello.bf`) |
| `bfc` | Brainfuck-to-C transpiler (`mtcc -c` it into a `.mrp`) |
| `sysmon` | interactive system-monitor TUI (`-list-task`, `-kill`, `-spawn`, `-h`) |

The `bash` package's recipes are ruf v3, so Eggkg-l requires **Equinox 0.4 Beta (v0.9.3) or newer**. `package.list` v0 carries no versions or hashes; when the repository publishes an `index.idx`, eggkg verifies sha256 automatically.

---

## Configuration: `.ecf`, `set` and eqshell scripts

### The `.ecf` format

`.ecf` is a lightweight INI dialect that replaced the old boot `nic.cfg` entirely. `#` starts a comment; a `[section]` header prefixes the keys that follow; keys containing a dot are used as-is.

```ini
# /equinox/conf/system.ecf
[net]
driver = ne2000

[dependencies]
bash = true
```

The store is loaded at boot from the first existing file: `/equinox/conf/system.ecf`, `/mnt/equinox/conf/system.ecf`, then the legacy `/boot/system.ecf` locations. Writes go to the mounted volume first (`/mnt/equinox/conf/system.ecf`) so they survive a reboot; the parser is static (no malloc) and tolerant: unknown keys are counted and skipped, invalid values error out, duplicate keys resolve last-wins.

The schema knows seven keys:

| Key | Values | Read by |
| --- | --- | --- |
| `net.driver` | `ne2000` \| `e1000` \| `none` | NIC probe order (needs reboot to apply) |
| `base.path` | any path (`/mnt` promotes the volume to root at boot) | FAT32 boot logic |
| `active.conf` | file name of the overlay config | `set` / ecf store loader |
| `dependencies.bash` | `true` \| `false` | eggkg boot-time sync |
| `eggkg.server` | URL or path | package index source |
| `eggkg.mirror` | URL (reserved) | declared in schema, not yet read |
| `eggkg.local` | directory | package workspace (default `/equinox/.local`) |

### The `set` builtin

```text
set                       list active entries
set KEY                   show one value
set KEY VALUE             change a value (schema-validated, written through)
set -a FILE               apply every entry in FILE
set -w FILE               write the active store to FILE
set -d FILE [-path DIR] [-base SRC]
                          make FILE the main config (fresh keys = NONE)
set -b PATH               record the base.path pivot (applies after reboot)
set -x FILE               run an eqshell script (.es)
```

Every change is validated against the schema (`set net.driver bogus` prints the allowed choices and is rejected), patched **in place** into the active `.ecf` file (comments preserved), and mirrored into the in-memory store. Keys that require a reboot (only `net.driver`) say so. `set -d` creates an overlay: the pointed file becomes the active config, new keys are cloned from a `.base.ecf` template (auto-derived from the file name) with every value set to the `NONE` placeholder, and the `active.conf` pointer is written back to the default config.

### eqshell scripts (`.es`)

`set -x FILE` runs a small script format understood by the shell:

```text
[Eqshell]          <- first meaningful line, required
Log=True           <- directive: capture every command + output
# comment          <- skipped

diskinfo
Qfs -list-disk
```

- The header must be exactly `[Eqshell]`. Directives are `Key = Value` lines; the only recognized key is `Log` (`True/False`) — unknown directives are consumed with a warning.
- Every remaining line is fed verbatim into the normal shell dispatch, so pipes, redirection, globbing, `sleep`, `wait` and all builtins work inside scripts.
- `Log=True` records each command and its output into a transcript that is flushed to **`/eqshell.log`** (8 KB cap) when the script ends — a runnable audit trail, not just echo.
- Limits: script ≤ 4 KB / 512 lines, nesting depth ≤ 3.

---

## Disk management: `Qfs`

`Qfs` is the disk utility builtin (in `kernel/shell.cpp`):

```text
Qfs                     this help
Qfs -list-disk          list disks (hda..hdh)
Qfs -t hdX -format fat32
                        format -> whole-disk FAT32, label EQUINOXBASE
Qfs -install-boot [hdX]
                        install the GRUB bootloader (boot without CD)
```

- `-list-disk` shows every probed IDE slot with size, FAT32 presence, label and state (`base`, `mounted`, `siap`, `tanpa-fat32`).
- `-t hdX -format fat32` refuses to format a mounted volume; formatting produces a whole-disk FAT32 volume with a reserved gap (LBA 1..2047) for bootloader sectors.
- `-install-boot` writes GRUB for real: `boot.img` (512 B) becomes the MBR at LBA 0 (the existing partition table is preserved), `core.img` lands at LBA 1..N, and a `grub.cfg` is generated on the volume (`search label EQUINOXBASE`, `multiboot /boot/kernel.elf`, one `module` line per shipped `.mrp`/`.c`/`.wad`/`.ruf`/`.elf`/`.h`). It then reads the sectors back to verify, and prints `BOOTABLE — reboot tanpa CD (QEMU: -boot order=c)`.
- In the same family: `copy SRC [->] DST` (recursive copy; `cp` is an alias) and `del PATH` (recursive delete; `rm` stays non-recursive).

---

## System call interface (54 syscalls)

`int 0x80`, **EAX** = number, **EBX / ECX / EDX** = arguments, **EAX** = result (negative = error). Number 0 is deliberately unused so that "forgot to set EAX" is detectable. The numbering is append-only, so old binaries keep working. The table below mirrors `kernel/library/header/syscall.h`.

| # | Name | Signature | Notes |
| --- | --- | --- | --- |
| 1 | `exit` | `(status)` | ends the program |
| 2 | `exec` | `(path)` | run an MRP; nested exec returns `EBUSY` |
| 3 | `getpid` | `()` | caller's pid |
| 4 | `write` | `(fd, buf, len)` | console, file, or pipe |
| 5 | `read` | `(fd, buf, len)` | file or pipe; `0` = EOF |
| 6 | `open` | `(path)` | read-only open |
| 7 | `close` | `(fd)` | flushes dirty writable fds |
| 8 | `getkey` | `()` | keyboard, non-blocking |
| 9 | `readline` | `(buf, max)` | one line of input |
| 10 | `print` | `(str)` | |
| 11 | `printint` | `(n)` | |
| 12 | `malloc` | `(size)` | per-task arena |
| 13 | `gettick` | `()` | 100 Hz ticks since boot |
| 14 | `sleep` | `(ms)` | tick-exact |
| 15 | `getargs` | `(buf, max)` | the program's arguments |
| 16 | `mkfile` | `(path, buf, len)` | create or overwrite, binary-safe |
| 17 | `readfile` | `(path, buf, max)` | whole-file read |
| 18 | `filesize` | `(path)` | |
| 19 | `fileexists` | `(path)` | |
| 20 | `fbinfo` | `(info*)` | framebuffer address / size / bpp / pitch |
| 21 | `putpixel` | `(x, y, color)` | |
| 22 | `fillrect` | `(x\|w<<16, y\|h<<16, color)` | |
| 23 | `pollkey` | `()` | non-blocking, `0` = none |
| 24 | `mouse` | `(state*)` | absolute position + buttons |
| 25 | `speaker` | `(freq)` | PC speaker on/off |
| 26 | `sndbeep` | `(freq, ms)` | queued timed tone |
| 27 | `lseek` | `(fd, off, whence)` | |
| 28 | `printf` | `(fmt, args[3])` | kernel-side printf |
| 29 | `ringinfo` | `()` | caller CPL (0 or 3) |
| 30 | `blit` | `(src, w\|h<<16, flags)` | 8 bpp to framebuffer, scaled |
| 31 | `setpalette` | `(pal*)` | 256 x RGB |
| 32 | `keyevent` | `()` | press/release + raw scancode |
| 33 | `mousedelta` | `(int32[2])` | raw PS/2 delta + button mask |
| 34 | `netinfo` | `(u32 w[10])` | IP, MAC, counters |
| 35 | `netping` | `(ip)` | 4x ICMP echo |
| 36 | `spawn` | `(path, arena_hint)` | new task, non-blocking |
| 37 | `yield` | `()` | |
| 38 | `taskinfo` | `(u32[])` | task list (`ps`) |
| 39 | `kill` | `(pid)` | |
| 40 | `open2` | `(path, flags)` | `O_RDONLY/WRONLY/RDWR/CREAT/TRUNC/APPEND/EXCL/DIR` |
| 41 | `unlink` | `(path)` | |
| 42 | `mkdir` | `(path)` | |
| 43 | `rmdir` | `(path)` | empty directories only |
| 44 | `rename` | `(old, new)` | |
| 45 | `stat` | `(path, stat*)` | size, type, backing store, mode |
| 46 | `readdir` | `(fd, dirent*)` | one entry per call |
| 47 | `fstat` | `(fd, stat*)` | |
| 48 | `free` | `(ptr)` | |
| 49 | `wait` | `(pid, status*)` | blocking waitpid; `-1`/any child supported |
| 50 | `pipe` | `(fds[2])` | 4 KB ring |
| 51 | `meminfo` | `(u32 w[6])` | pool and per-task memory |
| 52 | `spawn2` | `(path, hint, args)` | spawn with arguments |
| 53 | `setclip` | `(x\|w<<16, y\|h<<16)` | per-task draw window |
| 54 | `drawline` | `(x0\|y0<<16, x1\|y1<<16, color)` | Bresenham, clipped |

Negative results are errnos (`SYS_ENOENT`, `SYS_EBADF`, `SYS_EFAULT`, `SYS_EBUSY`, `SYS_EIO`, `SYS_ECHILD`, ...). `syscalls` prints the live table from the running kernel.

---

## Storage & filesystems

**ATA / IDE (PIO).** Primary and secondary buses, master and slave (four slots probed at boot with `IDENTIFY`), LBA28 with automatic LBA48 for large disks, ATAPI (CD-ROM) detection, MBR parsing (four primary entries, `0x55AA` validated), and `FLUSH CACHE` after writes. A generic block layer (`kernel/library/drivers/blk.cpp`) sits in front of the drivers; AHCI sources exist alongside as work in progress.

**FAT32 (read/write).**

- Mount validates the BPB, FSInfo and cluster layout. The first FAT32 partition mounts at `/mnt` automatically (`mount` / `umount` do it by hand).
- **Lazy RAMFS mirror:** directories populate on first use and file contents load on first read. `ls`, `cd`, `cat`, `edit`, `mget`, the GUI file manager and every syscall work on disk files transparently.
- **Long file names** are read and written with 8.3 mangling (`~1..~9` numeric tails, collision checks against the real on-disk short names). Name matching on the volume is case-insensitive per the FAT spec.
- **Write-through:** create, overwrite, extend, truncate, `mkdir`, delete. Both FAT copies and the FSInfo sector stay consistent, directories grow on demand, and a failed I/O rolls the operation back.
- **Fast reads:** cluster chains are walked with contiguous-run coalescing (up to 128 sectors per ATA transfer), so the 4.2 MB `doom1.wad` streams in well under a second.
- Verified against an independent implementation (mtools) and a host-side mini-fsck up to 100 % volume usage.

**RAMFS.** The tree is populated from GRUB modules at boot. The `dist/` layout mirrors it: `dist/equinox/tools` maps to `/equinox/tools`, `dist/equinox/games` to `/equinox/games`, `dist/*.c` to `/test`, and `dist/equinox/repo` to an offline eggkg repository (`eggkg update /equinox/repo/package.list`).

**System path `/bin`.** eggkg-installed programs land in `/bin`, which the shell consults when resolving bare command names; in-OS-built tools live in `/equinox/tools`.

---

## Networking

```text
ne2k_isa (0x300 / IRQ 9) -> RX ring (drained outside the IRQ) -> lwIP 2.1.3 (NO_SYS) -> DHCP / DNS / ICMP / TCP
                                                        |
                          mget (HTTP + HTTPS client)   +   httpd (server on :80)
                          eggkg in-process fetch       +   BearSSL TLS 1.2, 9 root CAs
```

- **Kernel `nettask`** polls the stack cooperatively so the shell never blocks the network.
- **`mget <url>`** downloads over HTTP or HTTPS into the current directory (`cd /mnt` first to write to the disk), follows up to 3 redirects, and supports `-port <n>`. TLS is **fail-closed**: certificate verification uses 9 built-in root anchors; `mget -k` (`--insecure`) is an explicit opt-out.
- **`httpd`** serves a status page and RAMFS files on port 80 (`http://localhost:8080/` from the QEMU host).
- **`ping`, `tcpping`, `dns`, `ifconfig`, `netdbg`** cover diagnostics. QEMU user-mode networking does not forward ICMP, so use `tcpping` there.
- **`eggkg`** uses its own in-process transport (`net_eggkg_fetch`) with the same TLS stack and redirect handling, returning fetched archives and sources directly to the package manager.
- **NIC registry** (`kernel/net/nic.c`): one struct (probe / send / recv / mac / irq / overflow) separates lwIP from hardware. NE2000-ISA is the first driver; e1000 is also implemented, and pcnet32 / eeepro100 devices are recognized and reported honestly. The `net.driver` config key selects the driver (or disables networking with `none`).

---

## Graphics, desktop & games

- **Display:** VESA linear framebuffer at 1360x768x32 with a text-mode fallback. 24-bit RGB console colors, plus an **ANSI CSI parser** in the console (SGR 0/1, fg 30-37, bg 40-47, bright 90-97/100-107, cursor home `H`, clear `J`/`K`) so colored output works both from the shell and from programs.
- **Per-task drawing:** `set_clip` confines a task's `put_pixel`, `fill_rect` and `draw_line` to a window, enforced by the kernel. Graphics output is focus-gated so background tasks cannot paint over the active console.
- **`blit`:** scaled 8 bpp to 32 bpp with a palette, one syscall per frame, with a 4:3 letterbox option.
- **EquiX desktop (ThorVG, new in 0.4 Beta):** `desktop` runs a full-screen vector-rendered desktop (exit with ESC / menu), with `tvgdemo`, `tvgbench` and `tvginfo` as standalone demo, benchmark and info commands. ThorVG is compiled into the kernel and draws through the GUI arena.
- **LVGL 8.3 apps:** `fm` (flex-layout file manager browsing `/mnt`, PS/2 mouse) and `settings`. The `gui` command runs the LVGL demo. LVGL is compiled in when `kernel/gui/lvgl/` exists in the tree.
- **Text editor** (`edit`): arrows, PgUp/PgDn, Home/End, Tab; **Ctrl+S** saves (write-through on FAT32), **Ctrl+Q** quits. **`eqgu <file.c>`** adds compile-on-save: the file is run through `mtcc` and diagnostics appear without leaving the editor.
- **Libgame** (`Libgame/libgame.h`): header-only helpers layered on the Morph SDK: rectangles, filled circles, a scalable 5x7 bitmap font, keyboard/mouse polling, non-blocking sound effects, a fixed-timestep helper, xorshift RNG, AABB collision. Snake, Breakout and Pong ship as self-hosted C sources built by `equinoxinstall`.
- **DOOM** (doomgeneric port, `mrp_user/doom/`): runs from a 24 MB demand-paged arena and reads its WAD straight from the FAT32 disk (`doom -iwad /mnt/doom1.wad`). It uses the file, blit, palette, keyboard and sound syscalls and its own small libc shim.
- **Audio:** PC speaker with a timed-tone queue (`beep`, `song`, `snd_beep`).

---

## Drivers

| Device | Notes |
| --- | --- |
| VESA / VBE framebuffer | mode set in `start.asm`; VGA text fallback |
| PS/2 keyboard | IRQ 1, per-console scancode rings, F1/F2 console control, arrow-key history |
| PS/2 mouse | IRQ 12, AUX-filtered, absolute position + raw deltas |
| ATA / IDE PIO | LBA28/48, ATAPI detect, MBR; generic block layer on top |
| AHCI | sources present (`kernel/library/drivers/ahci.cpp`), experimental |
| PCI | config-space enumeration (`0xCF8/0xCFC`), BARs, IRQ routing, bridge recursion, `lspci` |
| NE2000 (ISA) | first driver in the NIC registry |
| e1000 | implemented; pcnet32 / eeepro100 recognized |
| PC speaker + PIT | tone queue; 100 Hz system timer |
| CMOS RTC | file timestamps, `clock` |
| Serial COM1 | mirror of the console for automation and logs |

---

## Shell command reference

The prompt is `root::users / $`. Paths can be absolute (`/mnt/x`) or relative (`./x`, `dir/x`). The shell resolves bare names against its builtins, the `/bin` system path (eggkg packages), `/equinox/tools`, `/equinox/games` and the current directory. Line features: single-star glob `*`, pipes `|`, redirection `> >> <`, arrow-key history. The full command-by-command reference with examples lives in [`docs/COMMANDS.md`](docs/COMMANDS.md).

Legend: **B** = kernel builtin, **T** = userland tool (built by `equinoxinstall` or installed by `eggkg`), **P** = bundled program (`.mrp` / `.elf`).

### Filesystem

| Command | Type | Description |
| --- | --- | --- |
| `ls [dir]` / `ls -l` / `lf [-l]` | B, T | list a directory (RAMFS and `/mnt` alike; `lf` is the new name, `ls` remains during the transition window) |
| `cd <dir>` / `pwd` | B | change / print the working directory |
| `tree [dir]` | B | recursive directory tree |
| `cat <file>` / `showf <file>` | B, T | print a file |
| `copy <src> [->] <dst>` / `cp` | B, T | copy a file or directory (recursive) |
| `mv <src> <dst>` | T | move / rename |
| `del <path>` | B | recursive delete |
| `rm <file>` / `delfile` | B, T | delete a single file (write-through on `/mnt`) |
| `mkdir <dir>` / `cdir <name>` | T, B | create a directory |
| `rmdir <dir>` / `deldir` | B, T | remove an empty directory |
| `touch <file>` / `cfile <name>` | T, B | create an empty file / update its time |
| `ccfile <name> <text>` | B | create a file with one line of content |
| `save <name> << "text"` | B | create **or overwrite** a file with the given text |
| `stat <file>` | T | size, type, backing store, timestamps |
| `pren <file.mrp>` | B | MRP binary preview |
| `edit <file>` | B | built-in text editor (Ctrl+S save, Ctrl+Q quit) |
| `eqgu <file.c>` | B | editor with compile-check on save |
| `xxd <file> [n]` | B | hex dump of the first *n* bytes (default 64) |
| `mount` / `umount` | B | attach / detach the FAT32 volume at `/mnt` |
| `diskinfo` | B | ATA drives, partitions, volume layout, free clusters, arena and cache stats |
| `fm [path]` | B | LVGL graphical file manager |
| `sys cwd\|mkdir\|touch\|rm ...` | B | direct kernel filesystem calls |

### Text tools (userland; base ISO via `equinoxinstall`, bash-class via the `bash` package)

`grep [-i -n -c -v]`, `head [-n N]`, `tail [-n N]`, `wc [-l -w -c]`, `sort [-r]`, `uniq [-c]`, `cut -d DELIM -f LIST`, `tr SET1 SET2` / `tr -d SET`, `rev`, `nl`, `more` (23-line pager), `find [-name SUBSTR]`, `which NAME`, `diff FILE1 FILE2`, `strings FILE [minlen]`, `cksum FILE...`, `basename PATH`, `dirname PATH`.

### Processes & multitasking

| Command | Type | Description |
| --- | --- | --- |
| `ps` | B | task table: pid, state, kind, console, name (`eqbash` is the shell task) |
| `run <prog>` / `./prog` | B | run a `.mrp` or `.elf` in ring 3 (foreground) |
| `spawn <prog> [args]` | B | start a program as a new background task |
| `kill <pid>` / `wait [pid]` | B | terminate a task / block for a child's status |
| `yield` / `sleep <ms>` | B | give up the CPU / tick-exact sleep |
| `switch <n>` | B | move display focus to console *n* (same as F1/F2) |
| `meminfo` | B | user page pool, faulted vs reserved pages per task, zombies |

### Build, packages & configuration

| Command | Type | Description |
| --- | --- | --- |
| `equinoxinstall` | B | interactive install wizard (build userland in-OS) |
| `equinoxinstall -compile <dir>` | B | compile userland only |
| `equinoxinstall -build <file.ruf \| name \| *.ruf>` | B | build a recipe, one tool, or every matching `.ruf` |
| `mtcc [--debug] [-c] <file.c>` | P | compile and run C, or compile to `.mrp` (`-c`) |
| `mtcc -make <file.ruf>` | P | multi-file build from a ruf recipe |
| `eggkg update\|install\|remove\|list\|search\|info\|sync` | B | package manager (see its section above) |
| `set [KEY [VALUE]] / -a / -w / -d / -b / -x` | B | view, validate and write `.ecf` config; run `.es` scripts |
| `Qfs [-list-disk \| -t hdX -format fat32 \| -install-boot]` | B | disk listing, formatting, GRUB installation |

### System information & diagnostics

`info` (version banner, feature build, RAM size), `cpu` (CPUID), `lspci`, `memmap`, `malloc` (kernel heap statistics), `tick`, `clock`, `syscalls` (live syscall table), `sctest` (syscall self-test), `ring` / `ringstats`, `testconv` / `teststr` / `testvector` (kernel libc self-tests), `random`, `math`, `mouse`, `reboot`, `panic [text]` (deliberate panic).

### Low-level & developer utilities

| Command | Description |
| --- | --- |
| `mem <hexaddr>` | read one 32-bit word from kernel memory (a bad address faults the kernel) |
| `alloc <n>` / `free <hexaddr>` | allocate from the kernel heap and dump; `free` reports the call without freeing |
| `calc <a> <op> <b>` / `hex <n>` / `dec <hex>` | integer calculator and base conversion |
| `color <fg> [bg]` / `color list` / `color reset` | console colors |
| `echo <text>` / `clear` | print text / clear the screen |

### Networking

| Command | Description |
| --- | --- |
| `ifconfig` | interface status: DHCP address, MAC, counters |
| `ping <host>` | ICMP echo (not forwarded by QEMU user-mode networking) |
| `tcpping <host> [port]` | TCP connect probe with RTT |
| `dns <hostname>` | resolve a name |
| `mget <url> [-port <n>] [-k]` | HTTP/HTTPS download to the current directory; `-k` / `--insecure` skips certificate verification |
| `httpd` / `nettask` / `netdbg` | server status / network task status / stack statistics |

### GUI, games & audio

| Command | Type | Description |
| --- | --- | --- |
| `desktop` | B | EquiX vector desktop (ThorVG); ESC or menu exits |
| `tvgdemo` / `tvgbench` / `tvginfo` | B | ThorVG demo / benchmark / renderer info |
| `gui` / `fm` / `settings` | B | LVGL demo / file manager / settings panel |
| `snake`, `breakout`, `pong` | P | games (self-hosted C sources, built in-OS) |
| `doom [args]` | B | launch DOOM (default `-iwad /doom1.wad`; e.g. `doom -iwad /mnt/doom1.wad`, `-warp 1`, `-nosound`) |
| `beep` / `song` | B | PC-speaker tone and melody tests |
| `dbgmouse` | P | mouse diagnostic (SYS_MOUSE) |

### Demo, test & fault-injection programs

`hello` (minimal MRP), `morph_demo` / `demo_api` (SDK and syscall tour), `bgcount` (background counter for `spawn`), `spin` (60-second runner for `ps`/`kill` tests), `blittest`, `fstest` (24-check file-syscall suite), `pipedemo` (child writes a pipe, parent reads), `elfdemo.elf` (static ELF32, exit status 42), and the `crash*` family — `crashde` (divide by zero), `crashptr` (NULL dereference), `crashkmem` (ring-3 write into kernel memory), `crashstk` (guard-page recursion) — all of which the shell must survive.

### C sample programs (`/test`, run with `mtcc /test/<name>.c`)

`arr`, `divzero`, `edge`, `exec_test`, `gfxclip`, `guess`, `hello`, `libc`, `libcmini`, `libtest`, `morphgfx`, `morphio`, `multitask`, `negtest`, `netinfo`, `pkgtest` (pkg API + ANSI demo), `pl1`-`pl5`, `primes`, `ringtest`, `sleeptest`, `stresstwo`, `undeftest`, `varidx`.

### Keyboard

| Key | Action |
| --- | --- |
| **F1** | open a new shell on a new virtual console |
| **F2** | focus the previous console |
| **Arrow keys** | command history |
| **Ctrl+S / Ctrl+Q** | save / quit in `edit` and `eqgu` |

---

## Building from source

Requirements (Ubuntu / Debian):

```sh
sudo apt install build-essential gcc-multilib g++-multilib nasm \
     grub-pc-bin grub-common xorriso mtools qemu-system-x86 python3
```

The makefile auto-detects the toolchain: an `i686-elf-g++` cross compiler if present, otherwise the host `g++ -m32` (needs `gcc-multilib`). The LVGL source under `kernel/gui/lvgl/` is compiled in when present (`-DHAS_LVGL`); lwIP and BearSSL come from `third_party/`.

```sh
make pack mtcc       # stage tool/game sources + libc + samples; pack mtcc.mrp
make all             # kernel.elf + equinox.iso (also stages tool sources and builds elfdemo)
make diskimg         # dist/disk.img, 64 MB FAT32 demo disk (needs mtools)
make run-disk        # boot the ISO with the disk and networking under QEMU (-m 256)
make run             # boot the ISO only (-m 256)
make test            # host-side mtcc test harness (no QEMU needed)
```

Notes:

- **Do not run `make clean` casually.** It removes the whole `dist/` directory, including prebuilt `doom.mrp` and `doom1.wad`.
- **DOOM:** `make doom` rebuilds `doom.mrp` from a `doomgeneric` checkout, which is not bundled here. The full package ships a prebuilt `dist/equinox/games/doom.mrp`. Put the shareware `doom1.wad` in `dist/` (it becomes a zero-copy boot module) and/or on the disk image.
- **Version strings** can be re-stamped across a tree with `scripts/bump_version.py` (dry-run by default; rewrites only string literals and line comments, never identifiers, numeric literals or IP addresses).

---

## Testing

The QEMU harness drives the system through the serial log and the QEMU monitor, checking screen contents and serial output. Each subsystem ships its own script under `scripts/`; adjust the developer-machine paths (ISO location, QEMU install prefix) at the top of each file before running elsewhere.

| Suite | Coverage |
| --- | --- |
| `scripts/regression_v03.py`, `regression_task2.py`, `regression_task3.py`, `regression_multitask.py` | boot, self-hosting build, tools battery, demand paging, ELF + bss, spawn / wait / kill / zombie, pipes, `meminfo`, canvas semantics, games, F1/F2, DOOM 24 MB arena |
| `scripts/fat32_test.py`, `fat32_write_test.py` | FAT32 read/write, persistence, host-side mtools oracle, stress to 100 % |
| `scripts/install_test.py`, `installer_wizard_test.py`, `base_img_test.py`, `bootdisk_test.py` | `equinoxinstall` modes, `.es` scripts, base image layout, `Qfs -install-boot` boot |
| `scripts/eggkg_test.py`, `eggkg_net_test.py`, `eggkg_build_test.py` | eggkg index/download/build/install/remove flows, network transport |
| `scripts/ecf_test.py`, `docs_verify_test.py` | `.ecf` schema/overlay behavior, documentation-vs-code checks |
| `scripts/v091_banner_test.py`, `qemu_v10*_test.py` | boot banner (RAM: 256 MB), feature regressions |
| `make test` | host x86-32 interpreter for the compiler's instruction set (no QEMU) |

---

## Known limitations

- The 1 MB user stack does not grow automatically; hitting the guard page ends the program.
- `MAX_TASKS` is 8, and zombies hold a slot until reaped (the oldest is reclaimed when the table is full).
- The paging identity map tops out at 128 MB even when more RAM is installed; the remainder is only reachable through the GUI arena.
- ELF segments must lie in `[0x800000, 0x2000000)`; ELF tasks get a fixed 2 MB heap.
- One FAT32 volume at a time (`/mnt`), MBR primary partitions only, 512-byte sectors, no FAT12/16, file names over 63 characters fall back to 8.3.
- mtcc is a C subset: no `struct`/`union`, floating point, `switch`, `sizeof`, `typedef`, 2-D arrays, variadic or function-like macros; `printf` takes at most 5 conversion arguments.
- The base ISO ships without the bash-class tools; install the `bash` package (`eggkg install bash`) or use the builtins/aliases.
- eggkg `package.list` v0 carries no versions or hashes (sha256 verification requires an `index.idx`), and archive packages are capped at 384 KB.
- FAT32 file caches in the disk arena are managed by a free-list, but heavy churn of very large files can fragment it.
- The per-task clip window is not inherited across `spawn`, and graphics-program pixels are not preserved after exit (by design).
- AHCI is experimental; the ATA PIO driver is the supported path.

---

## Repository layout

```text
boot/            start.asm (VBE, GDT, module staging) and the grub.cfg template
kernel/
  kernel.cpp     entry point and subsystem bring-up
  eggkg.cpp      package manager (ring-0 builtin)
  shell.cpp      eqbash shell, equinoxinstall, Qfs, set/ecf, .es runner, dispatch
  library/       paging, malloc, task/scheduler, syscall, stdio (consoles + ANSI + canvas),
                 libc, idt/timer, ATA/blk/ahci, RAMFS, FAT32 (+write/format), PCI,
                 ELF + MRP loaders, ecf.c, egg_sha256.c, audio, UI (editor, TUI)
  net/           lwIP glue, NE2000, e1000, NIC registry, httpd, TLS client, eggkg fetch
  gui/           LVGL 8.3, the file-manager app, and the EquiX desktop (ThorVG glue)
  thorvg/        ThorVG vector renderer (0.4 Beta)
libc/            the 13 in-OS libc modules + morph.h (spliced by mtcc <morph.h>)
mrp_user/        Morph.h SDK, MRP packer, linker scripts, DOOM shim, TCC.md, TARGETS.md
tools_user/      userland tools in C (compiled in-OS by equinoxinstall) and elfdemo
games/ Libgame/  snake/breakout/pong sources and the game framework
test/            C sample programs for mtcc
mtcc.c           the in-OS C compiler (canonical source, incl. -make/ruf v3)
scripts/         image builder, QEMU regression harnesses, bump_version.py, host mtcc tests
third_party/     lwIP 2.1.3, BearSSL (with root-CA anchors)
docs/            user documentation (Indonesian): getting started, commands,
                 architecture, eggkg, release notes
dist/            build output: equinox.iso, kernel.elf, disk.img, staged sources, repo/
```

---

## Third-party components

| Component | Use | License |
| --- | --- | --- |
| [LVGL](https://lvgl.io/) 8.3 | GUI toolkit (file manager, settings) | MIT |
| [ThorVG](https://github.com/thorvg/thorvg) | vector renderer behind the EquiX desktop | MIT |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) 2.1.3 | TCP/IP stack | BSD-3-Clause |
| [BearSSL](https://www.bearssl.org/) | TLS 1.2 client (mget, eggkg) | MIT |
| [doomgeneric](https://github.com/ozkl/doomgeneric) | DOOM engine port | GPL-2.0 (id Software's DOOM source) |
| `doom1.wad` | shareware DOOM data | id Software shareware terms |

Eggkg-l (the default package repository) is a separate repository: [github.com/amnottdevv/Eggkg-l](https://github.com/amnottdevv/Eggkg-l).

