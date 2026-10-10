# Packages — `eggkg` and the Eggkg-l repository

`eggkg` is the Equinox package manager, built into the shell (it runs in
ring 0 but behaves like a userland tool). It is Gentoo-flavored:
packages are **C sources + a build recipe**, fetched over HTTPS and
compiled with the in-OS `mtcc` at install time.

Default package server: the **[Eggkg-l](https://github.com/amnottdevv/Eggkg-l)**
GitHub repository (`raw.githubusercontent.com/amnottdevv/Eggkg-l`),
overridable via configuration or argument (offline repos work too).

## ⚠️ The bash package is not bundled with the ISO

Starting with 0.4 Beta the base ISO **ships without the bash-class
coreutils** (`ls cat cp mv mkdir rmdir rm touch stat` — the whole
classic set). Reasons:

- keep the base ISO slim and focused on the kernel + self-hosting toolchain;
- the canonical, maintained copies of those tools live in the **Eggkg-l**
  repository as the `bash` package, so there is exactly one source of truth;
- the package manager itself demonstrates the intended software flow.

Install them right after the first boot:

```sh
root::users / $ eggkg update && eggkg install bash -y
```

Until then the shell keeps **builtins + aliases** for the essentials
(`ls`, `cat`, `cp`, `mv`, `mkdir`, `rmdir`, `del`, `touch`, `stat` work
immediately), and the text filters (`grep`, `wc`, `sort`, `tr`, …)
ship in the base and are pipe-ready out of the box.

## Commands

| Command | Function |
| --- | --- |
| `eggkg update [<repo>]` | Fetch `package.list` + `index.idx`; verify `index.idx` SHA-256 hashes. `<repo>` = URL or local path (offline), default from `eggkg.server` |
| `eggkg install <pkg> [-y]` | Fetch sources → build with `mtcc -make` → move `.mrp` products to `/bin` → record in `installed.db`; offer `[dependencies] bash=true` unless `-y` |
| `eggkg remove <pkg>` | Remove installed `.mrp` files + `installed.db` entry |
| `eggkg list` | List packages known to the repo, with installed marks |
| `eggkg search <pat>` | Search package names/descriptions |
| `eggkg info <pkg>` | Version, mirrors, size, SHA-256, dependencies |
| `eggkg sync` | Re-sync `/bin` from `/equinox/.local` (after manual meddling) |
| `eggkg help` | Usage |

Progress rendering (spinner, 20-column bar, `[ok] / [gagal]` markers via
`\r`) is serial-safe and console-safe.

## Repository format

**`package.list` (v0)** — INI-lite, one package per section:

```ini
[bash]
sources = [
  "https://github.com/amnottdevv/Eggkg-l/blob/main/bash/build.ruf",
  "https://github.com/amnottdevv/Eggkg-l/blob/main/bash/cat.c",
  "..."
]
```

- GitHub **blob** URLs are rewritten to **raw** automatically.
- Absolute local paths (`"/equinox/repo/bash/cat.c"`) read from the
  filesystem — that is how offline repos work.
- A `build.ruf` listed among the sources is used as the **build recipe**
  (see [SELF_HOSTING.md](SELF_HOSTING.md) for ruf v3/v4).

**`index.idx` (v1)** — per-file metadata: version, SHA-256, size,
mirror. `egg_sha256.c` (pure C, in-kernel) verifies downloads against
it during `update`.

## Install lifecycle

```
eggkg install bash
  │
  ├─ resolve      package.list [bash] + [dependencies]
  ├─ fetch        sources -> /equinox/.local/bash/src/*.c   (layout contract)
  ├─ build        mtcc -make /equinox/.local/bash/build.ruf
  │               (one job per tool, failures listed at the end)
  ├─ install      *.mrp -> /bin   (zero-copy relink in RAMFS)
  ├─ record       installed.db
  └─ offer        [dependencies] bash=true  -> /equinox/conf/system.ecf
                  (write now with -y; skipped with n)
```

With `dependencies.bash = true` in `system.ecf`, every subsequent boot
re-syncs `/equinox/.local` into `/bin` automatically before the shell
starts — installed tools survive and reappear after reboots.

### The build/install/record steps are config-driven

Since 0.5 the **build → install → db record** tail of an install runs
from the step list in `.config/eggkg.ecf` (see
[ECF.md](ECF.md#the-eggkg-recipe-configeggkgecf)), not from hard-coded
order. The whole list is parsed and validated *before* anything runs
(atomic), then each step is dispatched through `ecf_caller`
(`mtcc.compile_ruf_eggkg`, `pkg.install_bin`, `pkg.db_record`).

The **fetch** sub-step stays in the monolithic setup path on purpose
(careful buffer management — the download buffer is freed before the
mtcc spawn); this is stated in the recipe comments, not hidden.

Edit the recipe live — no reboot or recompile:

```sh
edit /equinox/.config/eggkg.ecf   # reorder, drop, or add steps
eggkg plan                        # dry-run: show resolved [install]
```

## Configuration keys

| Key (`system.ecf`) | Meaning |
| --- | --- |
| `eggkg.server = <url-or-path>` | Package server (default: Eggkg-l raw GitHub) |
| `eggkg.mirror = <url>` | Optional mirror tried on fetch failure |
| `eggkg.local = <path>` | Local staging root (`.local`) |
| `dependencies.bash = true\|false` | Boot-time `/bin` sync from `.local` |

Change them with the `set` builtin, e.g.:

```sh
set eggkg.server https://raw.githubusercontent.com/you/your-repo/main
set -w /equinox/conf/system.ecf     # persist
```

## Eggkg-l repository contents (0.4-era)

| Package | Contents |
| --- | --- |
| `bash` | The classic coreutils: `ls cat cp mv rm mkdir rmdir touch stat basename cksum cut diff dirname find grep head more nl rev sort tail tr uniq wc which` + `build.ruf` recipe |
| `bfc` / `bfi` | Brainfuck compiler + interpreter (mtcc showcase) |
| `emu-ch8` | CHIP-8 emulator |
| `sysmon` | System monitor |
| `eqfetch` | System info fetch tool |
| `ppmview` | PPM image viewer |
| games | `tetris`, `snake`, `space`, `flappy`, `wolf` |

## Writing your own package

1. Put the sources + a `build.ruf` (ruf v3/v4) in a Git repo (or a folder).
2. Add a section to your `package.list` with the URLs/paths.
3. Point `eggkg.server` at it (or pass the path to `eggkg update`).
4. `eggkg install <name>` — if it compiles with mtcc, it installs.

```ini
[hello]
sources = [
  "https://raw.githubusercontent.com/you/your-repo/main/hello/build.ruf",
  "https://raw.githubusercontent.com/you/your-repo/main/hello/hello.c"
]
```

Tools must be plain C in the mtcc subset and link against the shipped
libc (see [SELF_HOSTING.md](SELF_HOSTING.md)). Products named
`<name>.mrp` are picked up automatically and installed into `/bin`.
