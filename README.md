# Equinox OS

<p align="center">
  <img src="https://img.shields.io/badge/architecture-i686%20%2F%2032--bit-6f42c1?style=for-the-badge" alt="i686 32-bit" />
  <img src="https://img.shields.io/badge/kernel-monolithic-8A4FFF?style=for-the-badge" alt="Monolithic kernel" />
  <img src="https://img.shields.io/badge/boot-GRUB%20Multiboot-2D2D2D?style=for-the-badge" alt="GRUB Multiboot" />
  <img src="https://img.shields.io/badge/language-C%2FC%2B%2B%2FASM-00599C?style=for-the-badge" alt="C C++ Assembly" />
</p>

<p align="center">
  <img src="https://img.shields.io/github/license/equinoxosproject/Equinox-os?style=flat-square" alt="License" />
  <img src="https://img.shields.io/github/repo-size/equinoxosproject/Equinox-os?style=flat-square" alt="Repository size" />
  <img src="https://img.shields.io/github/commit-activity/y/equinoxosproject/Equinox-os?style=flat-square" alt="Commit activity" />
  <img src="https://img.shields.io/github/last-commit/equinoxosproject/Equinox-os?style=flat-square" alt="Last commit" />
  <a href="https://github.com/equinoxosproject/Equinox-os/actions/workflows/sync-release-source.yaml"><img src="https://github.com/equinoxosproject/Equinox-os/actions/workflows/sync-release-source.yaml/badge.svg" alt="Release source sync" /></a>
</p>

**A 32-bit x86 hobby operating system that compiles its own userland.**
Preemptive multitasking, ring-3 processes with demand paging, two executable
formats (MRP and ELF32), a built-in C compiler, FAT32 read/write, a TCP/IP
stack with HTTPS, an LVGL desktop, and DOOM.

**Release:** v0.3 Beta &nbsp;·&nbsp; **Target:** i686 (QEMU, `-m 64`) &nbsp;·&nbsp;
**Boot:** GRUB Multiboot &nbsp;·&nbsp; **Language:** C++17 / C / NASM

```text
root::users / $ eqbuild                      # the OS builds its own 29 userland tools
root::users / $ mtcc /test/hello.c           # compile + run C, inside the OS
root::users / $ grep -n printf /test/hello.c
root::users / $ cd /mnt && mget https://github.com/octocat/Hello-World   # HTTPS -> FAT32 disk
root::users / $ doom -iwad /mnt/doom1.wad    # DOOM straight off the disk
```

---

## Table of contents

1. [Highlights](#highlights)
2. [Quick start](#quick-start)
3. [Architecture](#architecture)
4. [Multitasking & process model](#multitasking--process-model)
5. [Memory management](#memory-management)
6. [Executable formats: MRP and ELF32](#executable-formats-mrp-and-elf32)
7. [mtcc — the in-OS C compiler](#mtcc--the-in-os-c-compiler)
8. [Self-hosting with `eqbuild`](#self-hosting-with-eqbuild)
9. [System call interface (54 syscalls)](#system-call-interface-54-syscalls)
10. [Storage & filesystems](#storage--filesystems)
11. [Networking](#networking)
12. [Graphics, GUI, games & DOOM](#graphics-gui-games--doom)
13. [Drivers](#drivers)
14. [Command reference (110 commands)](#command-reference-110-commands)
15. [Building from source](#building-from-source)
16. [Testing](#testing)
17. [Known limitations](#known-limitations)
18. [Repository layout](#repository-layout)
19. [Third-party components](#third-party-components)

---

## Highlights

| Area | What you get |
| --- | --- |
| **Processes** | Ring-3 user programs via TSS-based privilege transitions and `int 0x80`; a private page directory per task; kernel memory is unreachable from user code. |
| **Multitasking** | Preemptive round-robin scheduler (100 Hz), up to 8 tasks, per-task FPU state, virtual consoles (F1 / F2), `spawn` / `wait` / `kill`, zombies, and kernel pipes. |
| **Demand paging** | Task windows are *reserved*, not allocated. A page fault hands out one zero-filled page on first touch: a 24 MB DOOM arena costs only what DOOM actually uses. |
| **Executables** | **MRP** (18-byte header, checksummed flat binary) and **static ELF32/i386**, both loaded into the demand-paged window. |
| **Compiler** | **mtcc**: a single-pass C compiler with direct x86-32 code generation that runs *inside* the OS, with a libc prelude of 68 functions plus task and file-I/O headers. |
| **Self-hosting** | `eqbuild` compiles the 29 shipped tools from C source in-OS. The result is byte-identical to the host compiler's output. |
| **Syscalls** | 54 append-only syscalls (#1-#54): console, files, processes, pipes, memory, graphics, audio, input, networking. |
| **Storage** | ATA PIO driver (LBA28/LBA48, ATAPI detection, MBR), full **FAT32 read/write** with long file names, write-through consistency, and a RAM filesystem. |
| **Networking** | lwIP 2.1.3: DHCP, DNS, ICMP, TCP. HTTP **and HTTPS** client (`mget`, BearSSL, fail-closed TLS) and an HTTP server on port 80. |
| **Graphics** | 1360x768x32 VESA framebuffer, LVGL 8.3 desktop (file manager, editor, settings), per-task clip window, Bresenham lines, 8 bpp blit path. |
| **Games** | Snake, Breakout, Pong (Libgame framework) and **DOOM** loaded from the FAT32 disk. |
| **Tooling** | 110 shell commands, 29 self-built userland tools, 24 C sample programs, and QEMU regression suites. |

Scale: about 38,000 lines of first-party C / C++ / assembly (excluding LVGL,
lwIP and BearSSL) plus about 10,000 lines of Python build and test tooling.

### What is new in v0.3

- **Process & memory model:** demand paging, per-task page directories, `wait`, pipes, zombie handling, `meminfo`.
- **ELF32 loader** next to the MRP loader.
- **Full file syscalls** (#40-#48): `open2` with `O_CREAT / O_TRUNC / O_APPEND / O_EXCL / O_DIR`, `unlink`, `mkdir`, `rmdir`, `rename`, `stat`, `readdir`, `fstat`, writable file descriptors.
- **Self-hosting:** the OS compiles its own userland (`eqbuild`, 29 tools).
- **mtcc libc:** locally rendered `printf` family with true unsigned `%u`, `sscanf`, ctype, string extras, `qsort`, `rand`; undefined references are reported with the call-site line number.
- **Drivers:** PCI bus enumeration (`lspci`) and a NIC driver registry; NIC receive moved out of the IRQ into a ring drained by a kernel `nettask`.
- **Graphics:** per-task clip window (#53) and a Bresenham `draw_line` (#54).
- **Hardening:** fail-closed TLS (`mget -k` is an explicit opt-out), scheduler-lock leak safety net, an idle path for the scheduler (exact `sleep(ms)`), and header dependency tracking in the makefile.

---

## Quick start

### Run the prebuilt images

You need only [QEMU](https://www.qemu.org/) and the two images (`equinox.iso`, `disk.img`):

```sh
qemu-system-i386 -m 64 -boot order=d -cdrom equinox.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

- `-boot order=d` boots the ISO. The demo disk carries an MBR without boot code.
- Without `disk.img`, drop the `-drive` lines: the OS runs entirely from the RAM filesystem.
- The guest gets `10.0.2.15` by DHCP; the QEMU host is `10.0.2.2`, DNS is `10.0.2.3`.
  The guest web server is reachable from the host at `http://localhost:8080/`.

### First five minutes

```text
root::users / $ ls /mnt                          # FAT32 disk, auto-mounted
root::users / $ eqbuild                          # build all 29 tools from source (in-OS)
root::users / $ mtcc /test/hello.c               # compile + run a C program
root::users / $ ps                               # task table
root::users / $ spawn /equinox/tools/bgcount.mrp # run a task in the background
root::users / $ doom -iwad /mnt/doom1.wad        # DOOM from the disk
```

Press **F1** for a new virtual console/shell and **F2** to return to the previous one.

### Disk image contents

`disk.img` is a 64 MB IDE image: MBR plus one FAT32 partition (type `0x0C`, label `EQDISK`)
holding `README.TXT`, `hello from equinox.txt` (long file name), `docs/`, `bin.dat` and `doom1.wad`.
Everything you write to `/mnt` is written through to the image.

---

## Architecture

```text
GRUB (Multiboot)  ->  kernel.elf + boot modules (mtcc.mrp, games, tool sources, WAD)
      |
boot/start.asm       VBE 1360x768x32 (VGA text fallback), GDT, module staging @ 0x2800000
      |
kernel/kernel.cpp    heap arenas -> paging -> IDT/PIC/PIT -> console -> PS/2 -> PCI
      |              -> scheduler -> RAMFS from modules -> ATA + FAT32 (/mnt)
      |              -> NIC + lwIP + nettask (DHCP)
      v
kernel/shell.cpp     prompt "root::users / $"   (builtins + global tool dispatch)
      |
      +-- ring-3 programs: .mrp / .elf  <--- int 0x80 --->  syscall layer (54 calls)
      +-- eqbuild -> mtcc (in-OS) -> 29 self-built tools
```

The kernel is monolithic. Every subsystem mirrors its log to serial COM1, which makes
the whole system scriptable from the host (all regression suites drive QEMU this way).
The boot log lists each subsystem as `[ OK ]` together with every file loaded into the RAM filesystem.

More detail lives in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (Indonesian).

---

## Multitasking & process model

**Scheduler.** Preemptive round-robin driven by the 100 Hz timer (quantum: 1 tick = 10 ms).
Each task owns a 16 KB kernel stack and a separate 16 KB interrupt stack (the `TSS.ESP0` target),
plus its own `fxsave` FPU state. When nothing is runnable the scheduler halts with interrupts on
until an IRQ makes a task ready, so `sleep(ms)` is exact to the tick and the network task never busy-spins.

**Limits.** `MAX_TASKS = 8`, 16 file descriptors per task.

**Per-task state.** Every task carries its own file-descriptor table, current directory,
argument string, private user arena and page directory, virtual console, and graphics clip window.
Syscalls always act on the calling task's context.

**Lifecycle.**

```text
spawn / spawn2 --> READY <--> RUNNING --> exit --> ZOMBIE --wait--> slot freed
                     ^  |                            (status held for the parent)
                     |  +--> SLEEP / BLOCKED (pipe, wait, console focus)
```

- A USER child becomes a **zombie** on exit: its slot and exit status are kept until the parent calls `wait`.
- Killed children are also waitable. Orphans are reaped when the parent dies. If the task table
  is full, the oldest zombie is reclaimed.
- Errors: `SYS_ECHILD` (-13) for `wait` with no children, `SYS_EPERM` (-14).

**Pipes.** `SYS_PIPE` creates a 4 KB kernel ring buffer with reference-counted ends. Pipe descriptors
are inherited across `spawn`. Reads block until data arrives and return EOF when the last writer
closes. Writes block on a full buffer, and writing with no readers returns a broken-pipe error (`-EIO`).
`pipedemo` (shipped, built by `eqbuild`) demonstrates a child writing to its parent.

**Virtual consoles.** Each task has its own cell mirror and cursor. Background tasks print to their
own console instead of overwriting the screen. **F1** opens a new shell on a new console, **F2** focuses the
previous one, and `switch <n>` selects a console explicitly. Graphics programs use a per-task
*canvas*: the first draw snapshots the text screen and clears to black; the console is redrawn on exit.

**Hardening in the scheduler path.** A lock-leak safety net force-releases the scheduler lock after every
syscall and shell command and reports it. Header dependency tracking prevents ABI drift between objects.

---

## Memory management

| Region | Address / size | Purpose |
| --- | --- | --- |
| Kernel heap | 2 MB below `0x500000` **plus** a 1 MB region `0x2702000-0x2800000` (about 3.1 MB total) | Free-list allocator with split, coalesce and canaries. Only *physically adjacent* blocks are ever merged. |
| User page pool | `0x500000-0x2600000` (33 MB) | Bitmap of 4 KB physical pages handed to tasks on demand. |
| Module staging | `0x2800000`, 12 MB | GRUB modules (`mtcc.mrp`, `doom.mrp`, tool sources, WAD). Big files are referenced zero-copy. |
| FAT32 disk arena | 8 MB at `0x3400000` | File caches for FAT32 files, outside the kernel heap (free-list allocator). |
| Task window | arena at `0x500000+` (2 MB default, 8 MB mtcc, 24 MB DOOM) plus a 1 MB stack | Reserved with `PTE_DEMAND`; faulted in per page. |
| ELF window | segments in `[0x800000, 0x2000000)`, 2 MB heap at `0x2000000` | Static ELF32 images. |

**Demand paging.** Reserving a window marks its PTEs non-present with a `PTE_DEMAND` marker.
The first access raises `#PF`; `isr_14` calls `task_demand_fault()`, which allocates one physical page,
maps it, zero-fills it through the faulting address, and resumes the instruction. Fresh `.bss` therefore
reads as zero, and physical memory is consumed only for pages actually touched. Loader copies use a
two-phase fill (map and zero under the kernel directory, then copy under the task's directory).
`meminfo` (shell) and syscall #51 report pool totals, faulted KB per task, reserved versus faulted,
stack pages and zombie count.

**Guard pages.** A supervisor-only guard page sits below each user stack. Overflow kills the offending
program and the shell keeps running. The `crash*` programs exercise this: division by zero, NULL
dereference, a write into kernel memory from ring 3, and runaway recursion.

---

## Executable formats: MRP and ELF32

### MRP (Equinox Runnable Program)

A flat binary behind an 18-byte header. A 4-byte magic and an additive checksum let the loader reject
truncated or corrupt files before jumping into them.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `M` `R` `P` `1` |
| 4 | 1 | format version (1) |
| 5 | 4 | entry offset (relative to the code start) |
| 9 | 4 | code size |
| 13 | 1 | flags (`MRP_FLAG_NEEDS_GUI` reserved) |
| 14 | 4 | checksum (rotate-xor over the payload, non-zero seed) |

Programs link at `0x500010` (`mrp_user/link_mrp.ld`). The loader validates, maps the per-task demand
window, copies the image, and enters ring 3. Programs use their own per-task arena for `malloc` / `free`;
the arena is discarded on exit.
`mrp_user/mrp_pack.py` compiles, links and wraps a hosted `.cpp` into a `.mrp`;
`mtcc -c` produces one natively.

### ELF32

Static `ET_EXEC` / `EM_386` / little-endian executables run beside MRP.
The loader validates the program headers (at most 16), requires every `PT_LOAD` segment and the entry
point to fall in `[0x800000, 0x2000000)`, maps the span into the demand window, and zero-fills `.bss` on
fault. A plain `ret` from `_start` lands on the standard exit trampoline. Build one on the host with:

```sh
gcc -m32 -ffreestanding -fno-pie -static -no-pie -nostdlib -O2 \
    -T mrp_user/elf_link.ld tools_user/elfdemo.c -o elfdemo.elf     # or: make elfdemo
```

Run it with `elfdemo.elf`, `./elfdemo.elf`, or `spawn` + `wait` (exit status 42).

Both formats are launched the same way (`run <file>`, `./file`, or by bare name from any directory),
and both support `spawn`, `wait` and pipes.

---

## mtcc — the in-OS C compiler

`mtcc` is a tcc-style, **single-pass** compiler: lexer, recursive-descent parser, and direct x86-32
code generation with no AST. It ships as an ordinary `.mrp` module and runs inside the OS.

```text
mtcc prog.c          compile and run immediately (like tcc -run)
mtcc -c prog.c       compile to prog.mrp, then: run prog.mrp
mtcc --debug prog.c  verbose compiler info (sizes, exit code)
```

**Language subset:** `int`, `char`, `void`, one- and two-level pointers, 1-D arrays; `if/else`, `while`,
`do-while`, `for` (with a declaration in the initializer), `return`, `break`, `continue`; every assignment
and arithmetic/bitwise/logical/relational operator, pre/post `++ --`, and the ternary operator; global
scalars and arrays with constant, list and string initializers; recursion and forward prototypes (up to
8 parameters). Not supported: `struct` / `union`, floating point, `switch`, `sizeof`, `typedef`, 2-D arrays,
variadic functions, unsigned semantics.

**Preprocessor:** `#include <morph.h>` (aliases: `<stdio.h>`, `<stdlib.h>`, `<string.h>`),
`<multitasking.h>`, `<fileio.h>`, `"file.h"` from RAMFS, object-like `#define`, `#undef`,
`#ifdef` / `#ifndef` / `#else` / `#endif`.

**Diagnostics:** calling a declared-but-undefined function fails the build and reports the line number of
the first call site.

### Library surface

| Header | Contents |
| --- | --- |
| `<morph.h>` (68 functions) | **string/memory:** `strlen strcmp strncmp strcpy strncpy strcat strncat strchr strrchr strstr strdup strtok strspn strcspn strcasecmp memcpy memset memmove memcmp memchr` · **convert/parse:** `atoi strtol itoa utoa sscanf abs` · **ctype (13):** `isalnum isalpha iscntrl isdigit isgraph islower isprint ispunct isspace isupper isxdigit tolower toupper` · **heap:** `malloc free calloc realloc` · **stdio:** `printf sprintf snprintf puts fputs fopen fread fwrite fseek ftell fclose fgetc fputc remove rename` · **misc:** `strerror rand srand qsort_int qsort_str time getenv abort` · **graphics:** `draw_line set_clip` |
| `<multitasking.h>` (10) | `task_spawn task_spawn_hint task_spawn_args task_yield task_wait task_kill task_pid task_list pipe_create mem_info` |
| `<fileio.h>` (9) | `f_open f_stat f_fstat f_readdir f_mkdir f_rmdir f_unlink f_rename f_free` with `F_*` open flags |
| **Built-ins** | thin syscall wrappers available without any include: `print printint getkey readline write open read close malloc sleep gettick getpid exit exec getargs mkfile lseek ring`, `file_open file_read file_close file_write file_read_all file_size file_exists`, `net_info net_ping`, and the game API `fb_info put_pixel fill_rect draw_line set_clip pollkey mouse_state spk_tone spk_silence snd_beep` |

`printf` / `sprintf` / `snprintf` render **locally** inside the program (`%d %i %u %x %X %o %p %c %s`, with
width and flags; true unsigned `%u`), taking up to 5 conversion arguments per call.
A 54-stage libc self-test (`test/libtest.c`) passes in-OS.

### Quality bar

- The compiler emits a **closed instruction set**. A host-side x86-32 interpreter
  (`scripts/tcc_host_test/`, run with `make test`) executes exactly that set, so any codegen bug
  outside it is caught immediately.
- **Parity:** the in-OS compiler produces byte-identical `.mrp` images to the host build of the same source.
- Documentation: [`mrp_user/TCC.md`](mrp_user/TCC.md), [`mrp_user/workflow_mrp.md`](mrp_user/workflow_mrp.md).

---

## Self-hosting with `eqbuild`

The ISO ships the userland tools as **C source** in `/equinox/tools`, not as binaries.
Running `eqbuild` compiles each file with the in-OS `mtcc`, installs the resulting `.mrp`, and deletes the
source, with one log line per file:

```text
[eqbuild] 3/29  cat.c
[eqbuild] 3/29  OK  cat.mrp built, source removed
```

A failed compile keeps its source, so the command can simply be re-run. The RAM filesystem is rebuilt
from the ISO on every boot, so each fresh boot carries the sources again. After `eqbuild`, the shell
prefers the built tools over its kernel builtins (`ls cat cp mv rm mkdir rmdir touch stat`) and every tool
can be called by name from any directory.

---

## System call interface (54 syscalls)

`int 0x80`, **EAX** = number, **EBX / ECX / EDX** = arguments, **EAX** = result (negative = error).
The numbering is append-only, so old binaries keep working. The table below is generated from
`kernel/library/header/syscall.h`.

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

Negative results are errnos (`SYS_ENOENT`, `SYS_EBADF`, `SYS_EFAULT`, `SYS_EBUSY`, `SYS_EIO`, `SYS_ECHILD`, ...).
`syscalls` prints the live table from the running kernel.

---

## Storage & filesystems

**ATA / IDE (PIO).** Primary and secondary buses, master and slave (four slots probed at boot with
`IDENTIFY`), LBA28 with automatic LBA48 for large disks, ATAPI (CD-ROM) detection, MBR parsing (four
primary entries, `0x55AA` validated), and `FLUSH CACHE` after writes.

**FAT32 (read/write).**

- Mount validates the BPB, FSInfo and cluster layout. The first FAT32 partition mounts at `/mnt` automatically
  (`mount` / `umount` do it by hand).
- **Lazy RAMFS mirror:** directories populate on first use and file contents load on first read.
  `ls`, `cd`, `cat`, `edit`, `mget`, the GUI file manager and every syscall work on disk files transparently.
- **Long file names** are read and written with 8.3 mangling (`~1..~9` numeric tails, collision checks against the
  real on-disk short names). Name matching on the volume is case-insensitive per the FAT spec.
- **Write-through:** create, overwrite, extend, truncate, `mkdir`, delete. Both FAT copies and the FSInfo sector
  stay consistent, directories grow on demand, and a failed I/O rolls the operation back.
- **Fast reads:** cluster chains are walked with contiguous-run coalescing (up to 128 sectors per ATA transfer),
  so the 4.2 MB `doom1.wad` streams in well under a second.
- Verified against an independent implementation (mtools) and a host-side mini-fsck up to 100 % volume usage.

**RAMFS.** The tree is populated from GRUB modules at boot. The `dist/` layout mirrors it:
`dist/equinox/tools` maps to `/equinox/tools`, `dist/equinox/games` to `/equinox/games`, `dist/*.c` to `/test`.

---

## Networking

```text
ne2k_isa (0x300 / IRQ 9) -> RX ring (drained outside the IRQ) -> lwIP 2.1.3 (NO_SYS) -> DHCP / DNS / ICMP / TCP
                                                        |
                          mget (HTTP + HTTPS client)   +   httpd (server on :80)
                          BearSSL TLS 1.2, 9 root CAs
```

- **Kernel `nettask`** polls the stack cooperatively so the shell never blocks the network.
- **`mget <url>`** downloads over HTTP or HTTPS into the current directory (`cd /mnt` first to write to the disk),
  follows up to 3 redirects, and supports `-port <n>`.
  TLS is **fail-closed**: certificate verification uses 9 built-in root anchors;
  `mget -k` (`--insecure`) is an explicit opt-out.
- **`httpd`** serves a status page and RAMFS files on port 80 (`http://localhost:8080/` from the QEMU host).
- **`ping`, `tcpping`, `dns`, `ifconfig`, `netdbg`** cover diagnostics. QEMU user-mode networking does not forward ICMP, so use `tcpping` there.
- **NIC registry** (`kernel/net/nic.c`): one struct (probe / send / recv / mac / irq / overflow) separates lwIP from
  hardware. NE2000-ISA is the first driver; e1000, pcnet32 and eepro100 are recognized and reported honestly. Adding a driver
  is one struct and one registry line.

---

## Graphics, GUI, games & DOOM

- **Display:** VESA linear framebuffer at 1360x768x32 with a text-mode fallback. 24-bit RGB console colors.
- **Per-task drawing:** `set_clip` confines a task's `put_pixel`, `fill_rect` and `draw_line` to a window, enforced by the kernel.
  Graphics output is focus-gated so background tasks cannot paint over the active console.
- **`blit`:** scaled 8 bpp to 32 bpp with a palette, one syscall per frame, with a 4:3 letterbox option.
- **LVGL 8.3.11 desktop** (`gui`, `fm`, `settings`) with PS/2 mouse support: a flex-layout file manager
  (browses `/mnt`), a code editor, and a settings panel.
- **Text editor** (`edit`): arrows, PgUp/PgDn, Home/End, Tab; **Ctrl+S** saves (write-through on FAT32), **Ctrl+Q** quits.
- **Libgame** (`Libgame/libgame.h`): header-only helpers layered on the Morph SDK: rectangles, filled circles, a scalable 5x7 bitmap font,
  keyboard/mouse polling, non-blocking sound effects, a fixed-timestep helper, xorshift RNG, AABB collision.
  Snake, Breakout and Pong are built on it.
- **DOOM** (doomgeneric port, `mrp_user/doom/`): runs from a 24 MB demand-paged arena and reads its WAD straight from the FAT32 disk
  (`doom -iwad /mnt/doom1.wad`). It uses the file, blit, palette, keyboard and sound syscalls and its own small libc shim.
- **Audio:** PC speaker with a timed-tone queue (`beep`, `song`, `snd_beep`).

---

## Drivers

| Device | Notes |
| --- | --- |
| VESA / VBE framebuffer | mode set in `start.asm`; VGA text fallback |
| PS/2 keyboard | IRQ 1, per-console scancode rings, F1/F2 console control |
| PS/2 mouse | IRQ 12, AUX-filtered, absolute position + raw deltas |
| ATA / IDE PIO | LBA28/48, ATAPI detect, MBR |
| PCI | config-space enumeration (`0xCF8/0xCFC`), BARs, IRQ routing, bridge recursion, `lspci` |
| NE2000 (ISA) | via the NIC registry |
| PC speaker + PIT | tone queue; 100 Hz system timer |
| CMOS RTC | file timestamps, `clock` |
| Serial COM1 | mirror of the console for automation and logs |

---

## Command reference (110 commands)

The prompt is `root::users / $`. There are **110 distinct commands**: 69 kernel builtins, 25 further userland tools
(29 tools in total; four of them override a builtin of the same name), and 16 bundled programs. 24 more C sample programs
live in `/test`. Paths can be absolute (`/mnt/x`) or relative (`./x`, `dir/x`). Globbing and shell-level pipes are not
implemented (pipes exist at the syscall level).

Legend: **B** = kernel builtin, **T** = userland tool (available after `eqbuild`), **P** = program (`.mrp` / `.elf`).

### Filesystem

| Command | Type | Description |
| --- | --- | --- |
| `ls [dir]` / `ls -l` | B, T | list a directory (RAMFS and `/mnt` alike) |
| `cd <dir>` | B | change directory |
| `pwd` | B | print the working directory |
| `tree [dir]` | B | recursive directory tree |
| `cat <file>` | B, T | print a file |
| `cp <src> <dst>` | T | copy a file (RAMFS or disk) |
| `mv <src> <dst>` | T | move / rename |
| `rm <file>` | B, T | delete a file (write-through on `/mnt`) |
| `mkdir <dir>` | T | create a directory |
| `rmdir <dir>` | B, T | remove an empty directory |
| `touch <file>` | T | create an empty file / update its time |
| `stat <file>` | T | size, type, backing store, timestamps |
| `cfile <name>` | B | create an empty file |
| `ccfile <name> <text>` | B | create a file with one line of content |
| `cdir <name>` | B | create a directory |
| `save <name> << "text"` | B | create **or overwrite** a file with the given text |
| `edit <file>` | B | built-in text editor (Ctrl+S save, Ctrl+Q quit) |
| `xxd <file> [n]` | B | hex dump of the first *n* bytes (default 64) |
| `mount` / `umount` | B | attach / detach the FAT32 volume at `/mnt` |
| `diskinfo` | B | ATA drives, partitions, volume layout, free clusters, arena and cache stats |
| `fm [path]` | B | LVGL graphical file manager |
| `sys cwd` / `sys mkdir <n>` / `sys touch <n>` / `sys rm <n>` | B | direct kernel filesystem calls |

### Text tools (all userland, built by `eqbuild`)

| Command | Description |
| --- | --- |
| `grep [-i -n -c -v] PATTERN FILE...` | search lines; mini-regex `.` `X*` `^` `$`; multi-file output |
| `head [-n N] FILE` | first *N* lines (default 10) |
| `tail [-n N] FILE` | last *N* lines (default 10, max 64) |
| `wc [-l -w -c] FILE` | count lines / words / bytes |
| `sort [-r] FILE` | sort lines ascending or descending |
| `uniq [-c] FILE` | collapse adjacent duplicates (optionally count) |
| `cut -d DELIM -f LIST FILE` | extract fields: `N`, `N-M`, `N-`, comma lists |
| `tr SET1 SET2 FILE` / `tr -d SET FILE` | translate or delete characters; ranges (`a-z`) and escapes |
| `rev FILE` | reverse each line |
| `nl FILE` | number lines |
| `more FILE` | 23-line pager (any key: next page, `q`: quit) |
| `find [dir] [-name SUBSTR]` | recursive search (depth 8) |
| `which NAME` | locate a command on the system path (`.` `/` `/bin` `/equinox/tools` `/equinox/games`) |
| `diff FILE1 FILE2` | line diff (first 20 differences) |
| `strings FILE [minlen]` | printable character runs |
| `cksum FILE...` | 32-bit checksum and size |
| `basename PATH` / `dirname PATH` | split a path |

### Processes & multitasking

| Command | Type | Description |
| --- | --- | --- |
| `ps` | B | task table: pid, state, kind, console, name |
| `spawn <prog> [args]` | B | start a program as a new background task |
| `kill <pid>` | B | terminate a task |
| `wait [pid]` | B | block until a child exits and print its status |
| `yield` | B | give up the CPU to the next task |
| `sleep <ms>` | B | sleep (tick-exact) |
| `switch <n>` | B | move display focus to console *n* (same as F1/F2) |
| `meminfo` | B | user page pool, faulted vs reserved pages per task, zombies |
| `run <prog>` / `./prog` | B | run a `.mrp` or `.elf` in ring 3 (foreground) |

### Compilers & program launchers

| Command | Type | Description |
| --- | --- | --- |
| `mtcc [--debug] [-c] <file.c>` | P | compile and run C, or compile to `.mrp` (`-c`) |
| `eqbuild` | B | compile all `/equinox/tools/*.c` in-OS, install the `.mrp`, remove the sources |
| `elfdemo.elf` | P | static ELF32 demo (exit status 42) |
| `doom [args]` | B | launch DOOM (default `-iwad /doom1.wad`; e.g. `doom -iwad /mnt/doom1.wad`, `-warp 1`, `-nosound`) |

### System information & diagnostics

| Command | Description |
| --- | --- |
| `info` | banner: version, architecture |
| `cpu` | CPU vendor and features (CPUID) |
| `lspci` | PCI device table: IDs, class, BARs, IRQ |
| `memmap` | physical memory / kernel areas |
| `malloc` | kernel heap statistics (two regions) |
| `tick` | timer tick counter and rate |
| `clock` | RTC clock display |
| `syscalls` | list the syscall interface |
| `sctest` | syscall-layer self-test from the shell |
| `ring` / `ringstats` | ring-buffer test and statistics |
| `testconv` / `teststr` / `testvector` | kernel libc self-tests (conversion, strings, vector) |
| `random` | random-number demo |
| `math` | trigonometry demo (sin/cos/tan of 45°); takes no arguments |
| `mouse` | PS/2 mouse driver test |
| `reboot` | reboot the machine |
| `panic [text]` | deliberately trigger the panic handler |

### Low-level & developer utilities

| Command | Description |
| --- | --- |
| `mem <hexaddr>` | read one 32-bit word from kernel memory (a bad address faults the kernel) |
| `alloc <n>` | allocate *n* bytes from the kernel heap and dump the first 16 |
| `free <hexaddr>` | placeholder that reports the call; it does not free |
| `calc <a> <op> <b>` | integer calculator: `+ - * /` (e.g. `calc 12 * 3`) |
| `hex <n>` / `dec <hex>` | decimal to hex and back |
| `color <fg> [bg]` / `color list` / `color reset` | console colors |
| `echo <text>` | print text |
| `clear` | clear the screen |

### Networking

| Command | Description |
| --- | --- |
| `ifconfig` | interface status: DHCP address, MAC, counters |
| `ping <host>` | ICMP echo (not forwarded by QEMU user-mode networking) |
| `tcpping <host> [port]` | TCP connect probe with RTT |
| `dns <hostname>` | resolve a name |
| `mget <url> [-port <n>] [-k]` | HTTP/HTTPS download to the current directory; `-k` / `--insecure` skips certificate verification |
| `httpd` | HTTP server status (port 80) |
| `nettask` | kernel network-task status |
| `netdbg` | network stack statistics and diagnostics |

### GUI, games & audio

| Command | Type | Description |
| --- | --- | --- |
| `gui` | B | LVGL desktop |
| `settings` | B | GUI settings panel |
| `snake`, `breakout`, `pong` | P | Libgame games |
| `beep` / `song` | B | PC-speaker tone and melody tests |
| `dbgmouse` | P | mouse diagnostic (SYS_MOUSE) |

### Demo, test & fault-injection programs

| Command | Type | Description |
| --- | --- | --- |
| `hello` | P | minimal MRP example (print, read a line, print) |
| `morph_demo`, `demo_api` | P | tour of the Morph SDK / every syscall from ring 3 |
| `bgcount` | P | non-interactive background counter (`spawn bgcount`) |
| `spin` | P | 60-second long-runner for testing `ps` and `kill` |
| `blittest` | P | blit-path regression program |
| `fstest` | T | 24-check file-syscall suite (cleans up after itself) |
| `pipedemo` | T | pipe + `wait` demo: child writes, parent reads |
| `crashde` | P | ring-3 division by zero; the shell must survive |
| `crashptr` | P | ring-3 NULL dereference (`#PF`, CR2 = 0) |
| `crashkmem` | P | ring-3 write into kernel memory; must be blocked |
| `crashstk` | P | runaway recursion into the stack guard page |

### C sample programs (`/test`, run with `mtcc /test/<name>.c`)

`arr`, `divzero`, `edge`, `exec_test`, `gfxclip`, `guess`, `hello`, `libc`, `libcmini`, `libtest`, `morphgfx`,
`morphio`, `multitask`, `negtest`, `netinfo`, `pl1`-`pl5`, `primes`, `ringtest`, `undeftest`, `varidx`.

### Keyboard

| Key | Action |
| --- | --- |
| **F1** | open a new shell on a new virtual console |
| **F2** | focus the previous console |
| **Ctrl+S / Ctrl+Q** | save / quit in `edit` |

---

## Building from source

Requirements (Ubuntu / Debian):

```sh
sudo apt install build-essential gcc-multilib g++-multilib nasm \
     grub-pc-bin grub-common xorriso mtools qemu-system-x86 python3
```

The makefile auto-detects the toolchain: an `i686-elf-g++` cross compiler if present, otherwise the host
`g++ -m32` (needs `gcc-multilib`).

```sh
make pack mtcc       # user programs -> dist/equinox/tools, /games; mtcc.mrp; /test samples
make all             # kernel.elf + equinox.iso (also stages tool sources and builds elfdemo)
make diskimg         # dist/disk.img, 64 MB FAT32 demo disk (needs mtools)
make run-disk        # boot the ISO with the disk and networking under QEMU
make run             # boot the ISO only
make test            # host-side mtcc test harness (no QEMU needed)
```

Notes:

- **Do not run `make clean` casually.** It removes the whole `dist/` directory, including prebuilt `doom.mrp` and `doom1.wad`.
- **DOOM:** `make doom` rebuilds `doom.mrp` from a `doomgeneric` checkout, which is not bundled here. The full package ships a prebuilt
  `dist/equinox/games/doom.mrp`. Put the shareware `doom1.wad` in `dist/` (it becomes a zero-copy boot module) and/or on the disk image.
- **Version strings** can be re-stamped across a tree with `scripts/bump_version.py` (dry-run by default; rewrites only string
  literals and line comments, never identifiers, numeric literals or IP addresses).

---

## Testing

The QEMU harness drives the system through the serial log and the QEMU monitor, checking screen contents and serial output.

| Suite | Coverage |
| --- | --- |
| `scripts/regression_v03.py` | boot, `eqbuild`, `fstest` 24/24, tools, FAT32 tools, DHCP + httpd, in-OS `mtcc`, 60 s soak |
| `scripts/regression_task3.py` | self-hosting (29/29 tools built in-OS), tools battery, clip-window pixel proof, libc 54/54, soak |
| `scripts/regression_task2.py` | demand paging, ELF + bss, spawn / wait / kill / zombie / ECHILD, pipes, `meminfo` |
| `scripts/regression_multitask.py` | canvas semantics, games, F1/F2, DOOM 24 MB arena, `ps` / `kill` / `switch` |
| `scripts/fat32_test.py`, `fat32_write_test.py` | FAT32 read/write, persistence, host-side mtools oracle, stress to 100 % |
| `make test` | host x86-32 interpreter for the compiler's instruction set (no QEMU) |

The suites total **101 checks** at release (see [`RELEASE_v0.3.md`](RELEASE_v0.3.md)). The harness scripts contain developer-machine
paths (ISO location, QEMU install prefix) at the top of each file; adjust them before running elsewhere.

---

## Known limitations

- The 1 MB user stack does not grow automatically; hitting the guard page ends the program.
- `MAX_TASKS` is 8, and zombies hold a slot until reaped (the oldest is reclaimed when the table is full).
- ELF segments must lie in `[0x800000, 0x2000000)`; ELF tasks get a fixed 2 MB heap.
- One FAT32 volume at a time (`/mnt`), MBR primary partitions only, 512-byte sectors, no FAT12/16, file names over 63 characters fall back to 8.3.
- mtcc is a C subset: no `struct`, floating point, `switch`, `sizeof`, `typedef`; `printf` takes at most 5 conversion arguments.
- FAT32 file caches in the disk arena are managed by a free-list, but heavy churn of very large files can fragment it.
- The per-task clip window is not inherited across `spawn`, and graphics-program pixels are not preserved after exit (by design).
- Shell-level pipes and globbing are not implemented.

---

## Repository layout

```text
boot/            start.asm (VBE, GDT, module staging) and the grub.cfg template
kernel/
  kernel.cpp     entry point and subsystem bring-up
  shell.cpp      shell, eqbuild, global tool dispatcher
  library/       paging, malloc, task/scheduler, syscall, stdio (consoles + canvas), libc,
                 idt/timer, ATA, RAMFS, FAT32 (+write), PCI, ELF + MRP loaders, audio, UI (editor, TUI)
  net/           lwIP glue, NE2000, NIC registry, httpd, TLS client
  gui/           LVGL 8.3 and the file-manager app
mrp_user/        Morph.h SDK, MRP packer, linker scripts, DOOM shim, TCC.md, TARGETS.md
tools_user/      29 userland tools in C (compiled by eqbuild) and elfdemo
games/ Libgame/  Snake, Breakout, Pong and the game framework
test/            C sample programs for mtcc
mtcc.c           the in-OS C compiler (canonical source)
scripts/         image builder, QEMU regression harness, bump_version.py, host mtcc tests
third_party/     lwIP 2.1.3, BearSSL (with root-CA anchors)
docs/            user documentation (Indonesian): getting started, commands, architecture, release notes
```

---

## Third-party components

| Component | Use | License |
| --- | --- | --- |
| [LVGL](https://lvgl.io/) 8.3.11 | GUI toolkit | MIT |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) 2.1.3 | TCP/IP stack | BSD-3-Clause |
| [BearSSL](https://www.bearssl.org/) | TLS 1.2 client | MIT |
| [doomgeneric](https://github.com/ozkl/doomgeneric) | DOOM engine port | GPL-2.0 (id Software's DOOM source) |
| `doom1.wad` | shareware DOOM data | id Software shareware terms |

<!-- Add the project's own LICENSE file and a License section here before publishing. -->

---

<p align="center">
  Built to understand the machine one subsystem at a time.<br />
  <strong>Equinox OS</strong> — kernel, shell, disk, network, and games in one small experiment.
</p>
