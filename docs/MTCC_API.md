# mtcc API reference — the preludes you `#include`

mtcc has no real filesystem headers; the "API surface" a `.c` program
sees is a set of small, named preludes injected by the compiler's
mini-preprocessor. Three module families matter to contributors:

| Include | Purpose |
| --- | --- |
| `#include <morph.h>` | General runtime: console I/O, files (v1), framebuffer/gfx, input, audio, net, memory helpers |
| `#include <fileio.h>` | The full file descriptor API (`f_open`..`f_fstat`, `f_free`) with `F_*` flags |
| `#include <multitasking.h>` | The task API (`task_spawn`, `task_yield`, `task_kill`, `task_list`, `task_wait`, pipes, `getargs`) |

All three are** spliced into the compilation unit** (the mini preprocessor just
textually includes the matching built-in block), so they never ship as files
on the volume. They are the single source of truth for the syscall ABI used by
in-OS programs.

> Not every C feature is available: global variables must be declared before
> use (single pass), no varargs, no function pointers, no `sizeof`, no casts,
> and `struct`/`union` may not be passed or returned **by value** (pass a
> pointer). `struct`/`union`/`enum`/`typedef`/`switch` and `{…}` initializers
> *are* supported. The full subset is specified in
> [mtcc language reference](MTCC_LANGUAGE.md); see also
> [Self Hosting](SELF_HOSTING.md) (incl. the multi-file compile/link rules).

---

## 1. `#include <morph.h>` (libc prelude alias)

Used by almost everything; pulls in the runtime declared in
`src/libc/morph.h`: memory, string, convert, ctype, env, heap, strx,
printf, sscanf, stdio, qsort, gfx and pkg in one shot.

### Console & runtime

| Function | Meaning |
| --- | --- |
| `print(char* s)` | stdout, no newline |
| `printint(int v)` | unsigned-aware decimal to stdout |
| `getkey()` | next key or -1 (non-blocking), -1 if empty |
| `readline(char* buf, int maxlen)` | blocking line read, returns the length |
| `exit(int status)` | end the current program (ring-3) |
| `getpid()` / `gettick()` / `sleep(int ms)` | id / monotonic ticks / block for *ms* |
| `getargs(char* buf, int maxlen)` | argv string from the last `run` |
| `malloc(int size)` | slab from the per-task arena; pair with `f_free` |
| `write(int fd, char* buf, int len)` | console fd write |
| `file_open/file_read/file_close/file_write/file_read_all/file_size/file_exists` | the v1 `morph.h` file API (whole-file, RAMFS simple) |

### Framebuffer & input

| Function | Meaning |
| --- | --- |
| `fb_info(struct fb* info)` | get the current VESA mode (w×h×bpp, pitch) |
| `put_pixel(int x, int y, int color)` | 1 pixel |
| `fill_rect(int x, int y, int w, int h, int color)` | rectangle |
| `draw_line(...)` | Bresenham line |
| `set_clip(int x, int y, int w, int h)` | clip rectangle for the task's window |
| `pollkey()` | edge-triggered key event like `getkey` but with the "no repeat" semantics |
| `mouse_state(...)` | current X/Y + buttons |
| `key_event()` | keyboard event struct |
| `spk_tone(int hz)` / `snd_beep(int hz, int ms)` | PC speaker |

### Networking

| Function | Meaning |
| --- | --- |
| `net_info()` / `mget(url, path)` / `tcpping` etc. | high-level network syscalls; see [NETWORKING.md](NETWORKING.md) |

### The classic libc modules that come along

`printf`, `sprintf`, `snprintf`, `sscanf`, `strcpy/strcat/strcmp/...`,
`strlen`, `ctype.*`, `qsort_int`, `qsort_str`, `rand/srand`, `atoi/strtol`,
`strdup/strtok/strspn`, `abs`, `clear_screen`, `ansi_*`, `fseek/ftell/fread/fwrite/fclose`,
`remove/rename`, `strerror`.

---

## 2. `#include <fileio.h>` (full file-descriptor API)

For programs that want real paths with create/truncate/append semantics
rather than the simple whole-file v1 morph API:

```c
#include <fileio.h>

int f = f_open("/mnt/notes.txt", F_WRONLY | F_CREAT | F_TRUNC);
if (f < 0) { print("cannot create\n"); return 1; }
fwrite?  /* use stdio if you want buffered I/O */
f_unlink("/old.txt");
f_mkdir("/tmp/work");
f_rename("/a", "/b");
int st[4];  f_stat("/mnt/notes.txt", st);
```

### Open flags (`F_*`)

| Flag | Value |
| --- | --- |
| `F_RDONLY` | 0 |
| `F_WRONLY` | 1 |
| `F_RDWR` | 2 |
| `F_CREAT` | 256 |
| `F_TRUNC` | 512 |
| `F_APPEND` | 1024 |
| `F_EXCL` | 2048 |
| `F_DIR` | 4096 |

### Functions

| Function | Description |
| --- | --- |
| `int f_open(char* path, int flags)` | open/create; fd ≥ 3, negative = errno |
| `int f_unlink(char* path)` | delete a file |
| `int f_mkdir(char* path)` | create directory |
| `int f_rmdir(char* path)` | remove empty directory |
| `int f_rename(char* old, char* new)` | move/rename |
| `int f_stat(char* path, int* st)` | `st[0]=size [1]=is_dir [2]=backing [3]=mode` |
| `int f_readdir(int fd, char* de)` | one entry per call; `de[0..63]` name, `*(int*)(de+64)` is_dir, `*(int*)(de+68)` size |
| `int f_fstat(int fd, int* st)` | same as `f_stat` but by fd |
| `int f_free(char* p)` | release a `malloc`'d block |

### Layout cheats (mtcc now has `struct`, these APIs still use arrays)

mtcc gained `struct`/`union`/`enum`/`typedef` (Stage 2), but the file API
keeps its historical `int[]`/`char[]` layouts so the *same source* also
compiles against the host SDK — don't expect a `struct stat` here.

`f_stat`: `int st[4]` — `st[0]=size`, `st[1]=is_dir`, `st[2]=backing`,
`st[3]=mode`.

`f_readdir`: `char de[72]` — `de[0]` is the first char of the name
(NUL-terminated inside 64 bytes), `int is_dir = *(int*)(de + 64)`,
`int size = *(int*)(de + 68)`. Return `1` = entry filled, `0` = end,
negative = errno.

---

## 3. `#include <multitasking.h>` (the task API)

```c
#include <multitasking.h>

int pid = task_spawn("worker.mrp");
if (pid < 0) { print("spawn failed\n"); return 1; }
task_yield();                       /* let the child get a timeslice */
int status; task_wait(pid, &status);/* block until it exits */
task_kill(pid);                     /* or kill it */
```

| Function | Description |
| --- | --- |
| `task_spawn(char* path)` | non-blocking: new task, 2 MB arena; `pid > 0` / errno |
| `task_spawn_hint(char* path, int arena_hint)` | same, explicit arena size |
| `task_spawn_args(char* path, int arena_hint, char* args)` | child starts with an args string readable via `getargs()` |
| `task_pid()` | this task's id |
| `task_yield()` | yield CPU to the round-robin scheduler |
| `task_kill(int pid)` | kill a sibling task |
| `task_list(int* out)` | snapshot table: `out[0]`=N, then N triplets `{pid,state,kind,console}` ints |
| `task_wait(int pid, int* status_out)` | blocking wait; `pid<=0` = any child; returns pid / -13 (ECHILD) |
| `pipe_create(int fds[2])` | anonymous pipe: `fds[0]=read`, `fds[1]=write`, fds 3+ |
| `mem_info(int w[6])` | pool totals/free KB, fault-in KB, live tasks, zombies |

The parent/child relationship is ordinary: spawned children inherit the
parent's pipe fds, so `parent | child` pipelines work from C just like
they do for shell builtins.

---

## Which API do I use for which data?

| Task | Choose |
| --- | --- |
| Print text, read input, one-line parsing | `#include <morph.h>` |
| Open/create/truncate files, stat/readdir | `#include <fileio.h>` |
| Spawn workers, pipes, wait | `#include <multitasking.h>` |
| Full host-side SDK with the same names (for fat `.cpp` programs) | `mrp_user/Morph.h` |
| Kernel-side programs compiled outside mtcc | the real headers in `kernel/library/header/` |

## Canonical example programs

| Program | Shows |
| --- | --- |
| `src/test/hello.c` | the absolute minimum (`morph.h` + `print`) |
| `src/test/morphio.c` | the full v1 file API, 16-stage regression |
| `src/test/libc.c` / `libcmini.c` | libc splice correctness |
| `src/test/libcmini` runner (`regression_task2.py`) | parallel mtcc threads |
| `src/test/primes.c`, `guess.c` | classic warm-up programs |
| `src/test/struct.c`, `swenum.c` | `struct`/`union` + `enum`/`switch` (Stage 2) |
| `src/test/sinit.c` | `struct`/`union` `{…}` initializers, global + local + nested |
| `src/test/mf_main.c` + `mf_helper.c` + `mf_shared.h` | **multi-file** compile/link: cross-file call, `extern`, relative `#include "x.h"` |
| MRP targets and roadmap | [Roadmap & Targets](../TARGETS.md) |
