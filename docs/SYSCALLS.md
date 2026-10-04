# System calls — `int 0x80`

Ring-3 programs talk to the kernel through software interrupt `0x80`.
The syscall number goes in **EAX**; arguments in EBX/ECX/EDX/ESI (usage
per entry below); the return value replaces EAX. Numbers are
**append-only**: new syscalls are only ever added at the end (table in
`kernel/library/syscall.cpp`, constants in
`kernel/library/header/syscall.h`). Number **#0 is deliberately
unused** — it catches "forgot to set EAX".

From ring 3, pointers must live inside the task's user region or the
call fails with `SYS_EFAULT` (uaccess check). Most calls return a
negative `SYS_E*` errno on failure; the convention per call is noted.

## Console & input (#1–#10, #23–#24, #32–#33)

| # | Name | Signature | Returns |
| --- | --- | --- | --- |
| 1 | `SYS_EXIT` | `(status)` | does not return |
| 8 | `SYS_GETKEY` | `()` | blocking key / -1 |
| 9 | `SYS_READLINE` | `(buf, maxlen)` | line length |
| 23 | `SYS_POLLKEY` | `()` | non-blocking key / 0 |
| 24 | `SYS_MOUSE` | `(state*)` | absolute position + buttons |
| 32 | `SYS_KEYEVENT` | `()` | press/release event + raw code |
| 33 | `SYS_MOUSEDELTA` | `(int32 dxdy[2])` | PS/2 delta, buttons |

## Output & text (#4, #10, #11, #28)

| # | Name | Signature | Returns |
| --- | --- | --- | --- |
| 4 | `SYS_WRITE` | `(fd, buf, len)` | bytes written |
| 10 | `SYS_PRINT` | `(str)` | 0 |
| 11 | `SYS_PRINTINT` | `(num)` | 0 |
| 28 | `SYS_PRINTF` | `(fmt, int args[3])` | chars printed |

## Legacy whole-file I/O (#6, #7, #16–#19)

| # | Name | Signature | Returns |
| --- | --- | --- | --- |
| 6 | `SYS_OPEN` | `(path)` | fd (3+), read-only |
| 7 | `SYS_CLOSE` | `(fd)` | 0 |
| 16 | `SYS_MKFILE` | `(path, buf, len)` | 0 / errno |
| 17 | `SYS_READFILE` | `(path, buf, maxlen)` | bytes / errno |
| 18 | `SYS_FILESIZE` | `(path)` | size / errno |
| 19 | `SYS_FILEEXISTS` | `(path)` | 0/1 |

## Positional file I/O (#5, #27, #40–#48)

| # | Name | Signature | Returns |
| --- | --- | --- | --- |
| 5 | `SYS_READ` | `(fd, buf, len)` | bytes / 0 = EOF |
| 27 | `SYS_LSEEK` | `(fd, off, whence)` | new position |
| 40 | `SYS_OPEN2` | `(path, flags)` | fd — flags: `O_CREAT/TRUNC/APPEND/EXCL/DIR` |
| 41 | `SYS_UNLINK` | `(path)` | 0 |
| 42 | `SYS_MKDIR` | `(path)` | 0 |
| 43 | `SYS_RMDIR` | `(path)` | 0 (empty dir) |
| 44 | `SYS_RENAME` | `(old, new)` | 0 |
| 45 | `SYS_STAT` | `(path, morph_stat_t*)` | 0 |
| 46 | `SYS_READDIR` | `(fd, morph_dirent_t*)` | 1 = entry / 0 = end |
| 47 | `SYS_FSTAT` | `(fd, morph_stat_t*)` | 0 |

Per-task fd tables (cwd + args live in `struct Task`); writes go
write-through to FAT32 on close.

## Memory (#12, #48, #51)

| # | Name | Signature | Returns |
| --- | --- | --- | --- |
| 12 | `SYS_MALLOC` | `(size)` | pointer / 0 (per-task MRP arena) |
| 48 | `SYS_FREE` | `(ptr)` | 0 |
| 51 | `SYS_MEMINFO` | `(u32 w[6])` | pool stats (total/free, faulted, reserved, zombies) |

## Time, misc, introspection (#13–#15, #29, #34)

| # | Name | Signature | Returns |
| --- | --- | --- | --- |
| 13 | `SYS_GETTICK` | `()` | timer ticks since boot (100 Hz) |
| 14 | `SYS_SLEEP` | `(ms)` | 0 — tick-precise |
| 15 | `SYS_GETARGS` | `(buf, maxlen)` | spawn args length |
| 29 | `SYS_RINGINFO` | `()` | caller CPL: 0 = kernel, 3 = user |
| 34 | `SYS_NETINFO` | `(u32 w[10])` | NIC/stack info |

## Graphics & audio (#20–#22, #25–#26, #30–#31, #53–#54)

| # | Name | Signature | Returns |
| --- | --- | --- | --- |
| 20 | `SYS_FBINFO` | `(info*)` | framebuffer geometry |
| 21 | `SYS_PUTPIXEL` | `(x, y, color)` | 0 |
| 22 | `SYS_FILLRECT` | `(x\|w<<16, y\|h<<16, color)` | 0 |
| 25 | `SYS_SPEAKER` | `(freq)` | 0 — 0 = off |
| 26 | `SYS_SNDBEEP` | `(freq, ms)` | queued beep |
| 30 | `SYS_BLIT` | `(src, w\|h<<16, flags)` | 8-bpp blit; flags: stretch / 4:3 letterbox (DOOM) |
| 31 | `SYS_SETPAL` | `(pal*)` | 256×RGB palette |
| 53 | `SYS_SETCLIP` | `(x\|w<<16, y\|h<<16)` | per-task draw window |
| 54 | `SYS_DRAWLINE` | `(x0\|y0<<16, x1\|y1<<16, color)` | Bresenham, focus-gated |

## Processes (#2, #3, #36–#39, #49, #52)

| # | Name | Signature | Returns |
| --- | --- | --- | --- |
| 2 | `SYS_EXEC` | `(path)` | run `.mrp` (legacy, single-arena) |
| 3 | `SYS_GETPID` | `()` | pid |
| 36 | `SYS_SPAWN` | `(path, arena_hint)` | new user task |
| 37 | `SYS_YIELD` | `()` | 0 |
| 38 | `SYS_TASKINFO` | `(u32 info[])` | task count (feeds `ps`) |
| 39 | `SYS_KILL` | `(pid)` | 0 |
| 49 | `SYS_WAIT` | `(pid, u32* status)` | child pid — blocks like waitpid |
| 52 | `SYS_SPAWN2` | `(path, hint, args)` | pid — spawn with argv |

Lifecycle: spawn → run → exit → child becomes **zombie** (slot held for
the parent) → `wait` reaps it; orphans are reaped when the parent dies.
`MAX_TASKS` = 8.

## Pipes & network (#35, #50)

| # | Name | Signature | Returns |
| --- | --- | --- | --- |
| 35 | `SYS_NETPING` | `(ip)` | ICMP echo, 0–4 replies |
| 50 | `SYS_PIPE` | `(int fds[2])` | 4 KB kernel ring, ref-counted, inherited across spawn; blocking read/write, EOF + broken-pipe detection |

## Errno values

| Value | Constant | Meaning |
| --- | --- | --- |
| -1 | `SYS_ENOSYS` | unknown syscall / not implemented |
| -2 | `SYS_EBADF` | invalid fd / not open |
| -3 | `SYS_ENOENT` | path does not exist |
| -4 | `SYS_EISDIR` | path is a directory |
| -5 | `SYS_ENOMEM` | allocation failed / arena full |
| -6 | `SYS_EINVAL` | invalid argument |
| -7 | `SYS_ENOTSUP` | not supported (yet) |
| -8 | `SYS_EMFILE` | fd table full |
| -9 | `SYS_EBUSY` | nested exec rejected (arena busy) |
| -10 | `SYS_EFAULT` | argument pointer outside the user region |
| -11 | `SYS_EIO` | disk I/O error (FAT32 write-through) |
| -12 | `SYS_EEXIST` | `O_EXCL` / mkdir / rename target exists |
| -13 | `SYS_ECHILD` | `wait()` without a matching live child |

Inspect the live table from the shell with `syscalls`; `sctest` runs a
self-test of the dispatch path.
