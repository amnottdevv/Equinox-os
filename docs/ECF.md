# `.ecf` — Equinox Config File format and store model

`.ecf` is the configuration container used across the system
(`system.ecf`, package `drivers.ecf`, overlays). This page is the
reference for the format itself; the system-wide file is documented in
[SYSTEM_ECF.md](SYSTEM_ECF.md), the editing tool in [SET.md](SET.md).

## Grammar

```text
# comment
[section]
key = value
section.key = value        # dotted key stored verbatim
key                        # presence flag, value is empty
```

Rules (enforced by `ecf_parse_into()` in `kernel/library/ecf.c`):

- `#` begins a comment; blank lines are ignored.
- `[section]` switches the current section until the next section;
  keys inside it flatten to `section.key`.
- A key already containing a dot is stored verbatim regardless of the
  current section.
- Values are trimmed; there is no quoting, escaping, or multi-line
  support.
- Free-form keys pass through — the schema
  (`ecf_schema[]`) validates *known* keys but never drops unknown ones.

## The store

`struct ecf_store` holds up to 64 entries of `(key ≤ 64, value ≤ 128)`
in a single file-scope buffer (~12.5 KB, deliberately *not* on the
stack — see the header comment in `ecf.c`).

Two parse modes:

| Function | Behaviour |
| --- | --- |
| `ecf_parse(buf, len, st)` | Reset the store, then parse. |
| `ecf_parse_merge(buf, len, st)` | Parse on top, later keys override. Used for the `active.conf` overlay. |

## Public API

```c
int          ecf_load(const char* path, struct ecf_store* st);
int          ecf_load_merge(const char* path, struct ecf_store* st);
const char*  ecf_get(const struct ecf_store* st, const char* key);
int          ecf_put(struct ecf_store* st, const char* key, const char* val);
struct ecf_store* ecf_store(void);        // global, lazy-loaded once
void         ecf_store_invalidate(void);  // force reload next call
const char*  ecf_active_path(int for_write);
int          ecf_set_file(const char* path, const char* key, const char* val);
int          ecf_write_store(const char* path, const struct ecf_store* st);
int          ecf_check(struct ecf_store* st);  // schema validate
```

`ecf_set_file` patches a single key **in place** in the file on disk;
`ecf_write_store` re-serializes the whole store.

## Per-tool config — `.config/<tool>.ecf`

`.ecf` is not only the system store; every configurable tool owns a
configuration file of its own under `.config/`:

```text
/equinox/conf/system.ecf      # global store (shell set/get, wizard)
/equinox/.config/eggkg.ecf    # package manager: the build/install recipe
/equinox/.config/mtcc.ecf     # compiler: format, flags, set, spawn
```

The resolver `ecf_tool_path(tool, for_write)` (`kernel/library/ecf.c`)
returns the absolute path to `<tool>.ecf`, and is deliberately
asymmetric:

- **Reading** (`for_write = 0`) checks for the file's existence at
  `/mnt/equinox/.config/<tool>.ecf` and then
  `/equinox/.config/<tool>.ecf`, creating nothing. It returns `NULL`
  if the file is absent — a read never mutates the filesystem.
- **Writing** (`for_write = 1`) reuses an existing file's location;
  otherwise it creates the `.config` directory at the first candidate
  that works (`/mnt/equinox` when the volume is mounted, RAMFS
  otherwise).

Tool names are restricted to letters, digits, `_` and `-` — a tool
name is a file name, never a dotted key.

Because a tool's configuration is read **on every invocation** and
never cached at boot, an edit is live on the next command: no reboot,
no daemon restart, no recompile.

## Dispatch — `ecf_caller` (string-keyed actions)

`kernel/library/ecf_caller.c` is the string analogue of
`syscall_table[]`: a fixed, heap-free registry that maps a dotted
action name to a handler. The eggkg recipe layer uses it so that build
and package ordering comes from `.ecf` text rather than from compiled
code.

```c
typedef int (*ecf_call_fn)(const struct ecf_call_ctx* ctx);
int  ecf_call_register(const char* name, ecf_call_fn fn); // duplicate: last wins
int  ecf_call(const char* name, struct ecf_call_ctx* ctx); // -1 = unknown
int  ecf_call_exists(const char* name);
int  ecf_call_count(void);
const char* ecf_call_name_at(int idx);
```

`ecf_call` returns `-1` and prints
`[ERROR] ecf_call: unknown action '<name>' (try: config callers)` for
an unregistered name. The registry is open: any other module may
register its own handlers through `ecf_call_register` (ring-0 only).

The interactive entry point is the shell's `call <name> [args...]`
builtin, which invokes `ecf_call` with `caller = "shell"`.

### Registered actions

| Action | Source | Effect |
| --- | --- | --- |
| `set.key <key> <val> [-> <ecf>]` | `config_cmd.cpp` | write `key = val` into an `.ecf` (defaults to the active store) |
| `set.path_local <path>` | `config_cmd.cpp` | set `eggkg.local = <path>` in the active store |
| `set.active <file>` | `config_cmd.cpp` | pivot the active store to `<file>` |
| `mtcc.compile_ruf_eggkg <ruf>` | `eggkg.cpp` | spawn `mtcc -make` on the given recipe |
| `pkg.install_bin <pkgdir>` | `eggkg.cpp` | move the built `.mrp` products into `/bin` |
| `pkg.db_record <name> <ver>` | `eggkg.cpp` | record the package in `installed.db` |
| `pkg.read_list <url>` | `eggkg.cpp` | fetch and parse `package.list` |
| `pkg.fetch_index <repobase>` | `eggkg.cpp` | fetch and parse `index.idx` (optional) |

`config callers` prints this list at runtime — you never have to guess
which names a config file may invoke.

## The eggkg recipe — `.config/eggkg.ecf`

The recipe is an ordered step list held per section. It **is** the
execution path: `eggkg install` contains no hard-coded build-and-install
sequence that the config merely influences, and `eggkg update` runs
`[update]` the same way.

### File layout

```ini
# eggkg.ecf — build/package recipe (edit freely; every line is a `call`)
# the number is cosmetic ordering; $var is resolved when the step runs
[update]
1 = pkg.read_list $server
2 = pkg.fetch_index $repobase
[install]
1 = mtcc.compile_ruf_eggkg $rufpath
2 = pkg.install_bin $pkgdir
3 = pkg.db_record $name $version
```

### Grammar and parsing rules

- Line form is `<order> = <action> <args…>` — an `=` must be present;
  a line without one is skipped. Both `1 = pkg…` and `1 := pkg…` parse,
  because everything up to the first `=` is discarded.
- **The leading number is cosmetic.** It is stripped and *not* used for
  ordering: steps execute in **file order**. Renumbering the file does
  not reorder execution; moving a line does.
- `#` or `;` at the start of a line (after indentation) is a comment;
  blank lines are skipped. CRLF is tolerated.
- Only two sections are recognised, `[update]` and `[install]`. Any
  other section header is parsed and ignored.
- The first whitespace-delimited token is the action name (≤ 47
  characters); the rest are its arguments.

### Limits

| Constant | Value |
| --- | --- |
| Steps per section (`EGG_STEP_MAX`) | 16 |
| Arguments per step (`ECF_CALL_ARGC_MAX`) | 8 |
| Argument length | 63 characters |
| Action name (`ECF_CALL_NAME_MAX`) | 48 (47 usable characters) |

Lines beyond the per-section limit are silently skipped during load,
so an oversized recipe is truncated rather than rejected. The recipe
loader reads the file node directly, so it is not bound by the 8 KB
`ECF_FILE_MAX` that the single-key readers (`ecf_file_get`, used by
`config show`/`config get`) enforce.

### Variables

Arguments beginning with `$` are substituted **at the moment that step
runs**, not pre-expanded for the whole file. The names are
case-insensitive; an unknown `$name` is passed through literally.

| Variable | Resolves to | Source |
| --- | --- | --- |
| `$rufpath` | the build recipe for the package being installed | install context |
| `$pkgdir` | the staging directory the sources were fetched into | install context |
| `$name` | the package name | `package.list` entry |
| `$version` | the package version | `package.list` entry |
| `$server` | the package server | explicit `eggkg update <source>`, else `eggkg.server` |
| `$repobase` | the repository base, derived during `update` | derived by step 1 |
| `$local` | the local staging root | `eggkg.local` |

Per-step substitution is what lets `[update]` step 2 use `$repobase`
even though step 1 is what computes it.

### Validation and execution

- **Atomic validation.** The whole step list is parsed and every action
  name is checked against the registry *before* any step runs. One
  unknown action rejects the entire section:
  `eggkg: resep [install] INVALID — tidak ada yang dijalankan`.
- **Stop on first failure.** Execution halts at the first step that
  returns non-zero; a partial install is never left behind.
- **Self-seeding.** If the file does not exist, the default recipe is
  written and then read back, so even default behaviour travels through
  the config interpreter.
- **Honest residuals.** The source-download stage of `install` and the
  `index.idx` hash verification remain inline: both manage their buffers
  tightly (freed before the `mtcc` spawn to avoid heap fragmentation in
  the 256 MB kernel) and this is stated in the recipe's own comments
  rather than hidden.

### Commands

| Command | Behaviour |
| --- | --- |
| `eggkg init` | write the default recipe; never overwrites an existing file |
| `eggkg plan` | print the resolved `[install]` steps, in order, without executing them |
| `eggkg update [source]` | run `[update]`; `<source>` overrides `$server` for that run |
| `eggkg install <name> [-y]` | run `[install]` for a package |
| `config show eggkg` / `config get eggkg <key>` | inspect the file from the shell |

## `.config/mtcc.ecf` — compiler defaults

Seeded by `config init mtcc`:

```ini
# mtcc.ecf — defaults for mtcc (a recipe or the CLI always wins)
[format]
default = mrp
[flags]
default =
[set]
store =
[spawn]
name = mtcc.mrp
args =
```

Keys are read case-insensitively after section flattening, so
`[format] default` is looked up as `format.default`.

| Key | Default | Effect | Precedence |
| --- | --- | --- | --- |
| `format.default` | `mrp` | Only the exact value `elf` switches the default output to ELF; any other value means `mrp`. Applies to `-c` and `-make` — run mode never writes a file, ignores it, and warns | Seed only — a recipe's `format` directive and the CLI `-format` both win |
| `flags.default` | *(empty)* | Space-separated default flags; only `-q`, `-d`/`--debug` and `--lib` are recognised | **Additive** — mtcc has no negative flags, so the CLI adds on top of it |
| `set.store` | *(empty → `/equinox/conf/system.ecf`)* | Where a recipe's `set key = value` lines write | A recipe's `ecf <path>` wins |
| `rufdir.default` | *(empty)* | Fallback value for `$rufdir` | Rarely reached — `$rufdir` is derived from the recipe's own path first, so this applies only when the recipe path contains no directory |
| `buildir.default` | *(empty → falls back to `$rufdir`)* | Default for `$buildir` / the `out` folder | A recipe's `out <dir>` wins |
| `spawn.name` | `mtcc.mrp` | The binary the shell looks for when compiling or building | Searched in the request's directory, `/equinox/tools`, then the alternative |
| `spawn.args` | *(empty)* | Extra arguments inserted **before** the command line, so config flags such as `-q` are parsed first | Always combined with the command's own arguments |

Two readers consume the same file: mtcc itself calls `mtcc_cfg_get()`
for `format.default`, `flags.default`, `set.store`, `rufdir.default`
and `buildir.default`; the shell calls `ecf_file_get()` for
`spawn.name` and `spawn.args` when it spawns the compiler. Because
`flags.default` is applied at process start, setting it once changes
every subsequent compile.

`mtcc -make` derives `$rufdir` from the recipe's own path and `$buildir`
from `out`, so the two `*.default` keys are genuine fallbacks: they fill
in what the recipe left empty and never override what it declared.

## Path resolution — `ecf_active_path()`

Resolution order for the active store:

1. The `set -d` target override (`ecf_tgt_on`), if registered this
   session;
2. `/equinox/conf/system.ecf` (canonical);
3. `boot/system.ecf` legacy fallback on the volume;
4. The `active.conf` overlay — candidates are tried in order and the
   first that parses wins.

`for_write = 1` returns the path a save should target even when no
file exists yet (so `set -w new.ecf` works).

## Limits

| Constant | Value |
| --- | --- |
| Keys per store | 64 |
| Key length | 64 |
| Value length | 128 |
| Whole-file buffer | 8 KB |
| Parse line length | 160 |
| Overlay redirects | 1 |

## Validation

`ecf_check()` walks the store and reports the first key that fails the
schema (unknown keys are OK; wrong *value* for a known key — e.g.
`net.driver = ixgbe` — fails with the allowed set in the message). The
shell `set` builtin and the eqgui settings editor both call it before
persisting.

## Testing

`src/scripts/ecf_test.py` drives a 25-case QEMU regression covering
parse edge cases, merge precedence, `active.conf` redirect, and
`set` round-trips. Run: `make test-ecf`.
