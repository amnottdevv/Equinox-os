# Command reference — the eqshell (0.4 Beta)

Prompt: `root::users / $`. Commands marked **✓ userland** are ring-3
`.mrp` programs compiled **inside the OS** (by `equinoxinstall`, or by
`eggkg install bash` for the coreutils). Everything else is a builtin
in the kernel shell.

The shell supports **pipes** (`|`), **output/input redirection**
(`>`, `<`), **globbing** (`*`) and history — the text toolset is
pipe-ready out of the box:

```sh
grep -n printf /test/hello.c | tr a-z A-Z > /mnt/out.txt
```

> **Thin base ISO**: the classic coreutils (`ls cat cp mv rm mkdir
> rmdir touch stat` as full tools) are **not bundled** — they arrive
> with `eggkg install bash` ([PACKAGES.md](PACKAGES.md)). The shell
> ships *eqbash builtins* (`lf`, `showf`, `cdir`, `cfile`, `copy`,
> `del`, …) plus **bash-name aliases** (`mkdir`, `touch`, `rm`, `cp`,
> …) so old reflexes keep working until the package lands; after the
> install, the package tools take over via the system path.

## 1. Filesystem — navigation & files

| Command | Type | Function / example |
| --- | --- | --- |
| `ls [dir]`, `lf [dir]` | builtin | list directory (RAMFS and `/mnt` alike); `ls -l` for details |
| `cd <dir>`, `pwd` | builtin | change / print working directory |
| `tree [dir]` | builtin | recursive directory tree |
| `cat <file>`, `showf <file>` | builtin | print file (absolute paths OK: `cat /eqshell.log`) |
| `cfile <name>`, `ccfile <name> <text>` | builtin | create file / create with one line of content |
| `touch <name>` | builtin | alias of `cfile` (empty file) |
| `save <name> << "text"` | builtin | create/overwrite a file with multi-line text (ends on an empty line) |
| `edit <file>` | builtin | full-screen editor (arrows/PgUp/PgDn/Home/End/Tab; **Ctrl+S** save, **Ctrl+Q** quit) |
| `copy SRC [->] DST` | builtin | copy files **and trees**; glob (`*`); destination dirs auto-created |
| `cp SRC DST` | builtin | alias of `copy` |
| `move SRC DST` | builtin | move / rename |
| `delfile <name>`, `rm <name>` | builtin | delete one file (not recursive) |
| `deldir <name>`, `rmdir <name>` | builtin | delete an empty directory |
| `del PATH` | builtin | delete a file **or tree** recursively |
| `cdir <name>`, `mkdir <name>` | builtin | create a directory |
| `pren <old> <new>` | builtin | rename |
| `xxd <file> [n]` | builtin | hex dump of the first n bytes (default 64) |
| `mount hdX` / `umount hdX` | builtin | attach / detach a FAT32 volume at `/mnt` |
| `diskinfo` | builtin | drive + volume details |
| `fm`, `eqgu` | builtin | GUI file manager / settings+editor (LVGL) |

## 2. Text tools (✓ userland, after `equinoxinstall`)

| Command | Function | Example |
| --- | --- | --- |
| `grep [-i] [-n] [-c] [-v] PAT FILE…` | search lines; mini-regex `.` `X*` `^` `$` | `grep -n printf /test/hello.c` |
| `head [-n N] FILE` / `tail [-n N] FILE` | first/last N lines (default 10) | `tail -n 3 serial.log` |
| `wc [-l -w -c] FILE` | count lines/words/bytes | `wc -l /test/hello.c` |
| `sort [-r] FILE` | sort lines asc/desc | `sort -r names.txt` |
| `uniq [-c] FILE` | drop adjacent duplicates | `sort x \| uniq -c x` |
| `cut -d D -f LIST FILE` | extract fields (`N`, `N-M`, `N-`, commas) | `cut -d : -f 1 users.txt` |
| `tr SET1 SET2 FILE` / `tr -d SET FILE` | translate / delete chars; ranges `a-z`; escapes `\n \t \r \0` | `tr a-z A-Z data.txt` |
| `rev FILE` | reverse each line | `rev data.txt` |
| `nl FILE` | number lines | `nl /test/hello.c` |
| `more FILE` | pager, 23 lines/page, `q` quits | `more /test/libc.c` |
| `find [dir] [-name SUB]` | recursive search (depth 8) | `find / -name mrp` |
| `which NAME` | resolve through the system path | `which grep` |
| `diff F1 F2` | line compare (first 20, `</>` style) | `diff a.txt b.txt` |
| `strings FILE [minlen]` | printable runs | `strings doom.mrp 8` |
| `cksum FILE…` | 32-bit checksum + size | `cksum cat.mrp` |
| `basename PATH` / `dirname PATH` | path splitting | `dirname /mnt/doom1.wad` |
| `fstest` | 24-check syscall/file suite | `fstest` |
| `pipedemo` | pipe + wait demo (child writes, parent reads) | `pipedemo` |

## 3. Processes

| Command | Function |
| --- | --- |
| `ps` | task table: pid, name, state, CPU, memory |
| `kill <pid>` | terminate a task |
| `wait [pid]` | block until a child exits; prints its status |
| `spawn <x.mrp> [args]` | start a new ring-3 task with args |
| `run <x.mrp>` / `./x.mrp` / `./x.elf` | run an MRP / ELF32 program |
| `yield` | give up the CPU slice |
| `sleep <ms>` | tick-precise sleep |
| `switch <n>` | switch virtual console (same as F1/F2) |
| `meminfo` | user pool: total/free, per-task faulted/reserved, zombies |

## 4. Compiler, self-hosting, packages

| Command | Function |
| --- | --- |
| `equinoxinstall` | **the installer wizard** — disk → layout → NIC → build userland in-OS (see [INSTALL.md](INSTALL.md)) |
| `equinoxinstall -compile <dir>` | compile userland from `<dir>/libc` + `<dir>/tools` only |
| `equinoxinstall -build <ruf\|name\|*.ruf>` | build via ruf v3 recipe / one tool / glob |
| `mtcc <file.c>` | compile **and run** C in the OS — `mtcc /test/hello.c` |
| `mtcc -c <file.c>` / `mtcc -make <file.ruf>` | compile only / run a build recipe ([SELF_HOSTING.md](SELF_HOSTING.md)) |
| `eggkg update\|install\|remove\|list\|search\|info\|sync` | package manager ([PACKAGES.md](PACKAGES.md)) — `eggkg install bash` brings the coreutils |
| `set …` | configuration store + eqshell runner ([CONFIGURATION.md](CONFIGURATION.md)) |
| `Qfs -list-disk\|-format\|-install-boot` | disk tool ([QFS.md](QFS.md)) |

## 5. Hardware & introspection

| Command | Function |
| --- | --- |
| `lspci` | PCI table (bus/dev/fn, vendor:device, class, IRQ, BARs) |
| `cpu` | CPUID info (vendor, features) |
| `info` | banner: version, RAM, uptime |
| `memmap` | physical memory map / kernel regions |
| `malloc` / `alloc <n>` / `free <addr>` | kernel heap status & probe |
| `syscalls` | live syscall table #0–#54 |
| `sctest` | syscall path self-test |
| `ring` / `ringstats` | ring-buffer test & statistics |
| `tick` | timer value & rate (100 Hz) |
| `mouse` | PS/2 mouse test |
| `random` / `math <expr>` / `calc <expr>` | RNG, expression evaluator |
| `hex <n>` / `dec <n>` | hex⇄dec conversion |
| `testconv` `teststr` `testvector` | libc self-tests |
| `clock` | RTC time/date |

## 6. Networking

| Command | Function |
| --- | --- |
| `ifconfig` | driver, MAC, DHCP lease, counters |
| `ping <host>` | ICMP — not forwarded by QEMU slirp; use `tcpping` |
| `tcpping <host> [port]` | TCP probe + RTT |
| `dns <host>` | resolve via DNS |
| `mget <url> [-port n]` | HTTP/**HTTPS** download (redirects ≤3) |
| `httpd` | web server :80 (status + RAMFS) — host: `http://localhost:8080/` |
| `nettask` / `netdbg` | network task / stack diagnostics |

## 7. Graphics, desktop, games & audio

| Command | Function |
| --- | --- |
| `gui` | LVGL desktop (settings, editor, file manager) |
| `desktop` | **EquiX** — the ThorVG vector desktop |
| `tvgdemo` / `tvginfo` / `tvgbench` | ThorVG renderer demos/benchmark |
| `settings` | settings panel |
| `color <n>` | text color |
| `doom [args]` | DOOM (`doom -iwad /mnt/doom1.wad`) |
| `beep` / `song` | PC speaker tests |

## 8. Misc

| Command | Function |
| --- | --- |
| `echo <text>` | print |
| `clear` | clear screen |
| `reboot` | reboot the machine |
| `panic [text]` | deliberate panic-handler test |

## Example session

```sh
root::users / $ equinoxinstall                 # 1. build the userland in-OS
root::users / $ eggkg update && eggkg install bash -y   # 2. coreutils
root::users / $ mtcc /test/hello.c             # 3. compile & run C live
root::users / $ grep -n printf /test/hello.c | tr a-z A-Z
root::users / $ Qfs -list-disk                 # 4. disks (PATA + SATA)
root::users / $ mount hde && ls /mnt           #    SATA disk via AHCI
root::users / $ cd /mnt && save note.txt << "persisted across reboots"
root::users / $ mget https://github.com/octocat/Hello-World
root::users / $ ps                             # 5. tasks; F1/F2 consoles
root::users / $ set net.driver e1000 && set -w /equinox/conf/system.ecf
root::users / $ doom -iwad /mnt/doom1.wad
```
