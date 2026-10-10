# `set` — the configuration builtin

`set` is the shell builtin that reads, edits and persists the Equinox
configuration store (parsed `.ecf` files — see [ECF.md](ECF.md)) and
that runs eqshell scripts (`.es` — see [EQSHELL.md](EQSHELL.md)).

```text
set                # show the active store
set list           # list all keys (canonical, one per line)
set <key> <value>  # set one key in memory
set -a FILE        # apply every valid entry from FILE
set -w FILE        # write the whole active store to FILE (the save)
set -d FILE [-path DIR] [-base SRC]   # register FILE as system config
set -b PATH        # record a base-directory pivot (next boot)
set -x FILE        # run FILE as an eqshell script
```

## Showing the store

`set` with no arguments prints the active store exactly as parsed:

```text
root::users / $ set
[net] driver = e1000
[dependencies] bash = true
[eggkg] server = https://raw.githubusercontent.com/amnottdevv/Eggkg-l
```

`set list` prints the flattened keys (`net.driver = e1000`, …). Keys
are case-insensitive on write; the parser preserves the written form.

## Editing keys

```sh
set net.driver e1000     # change in memory — nothing is persisted yet
set eggkg.mirror https://example/mirror
```

The edit becomes permanent only after `set -w`:

```sh
set -w /equinox/conf/system.ecf
```

Notes:

- Setting a key that is not in the schema (`ecf_schema[]` in
  `kernel/library/ecf.c`) is allowed — unknown keys round-trip through
  `set -w`, consumers ignore them.
- Values are trimmed, free-form strings. There is no quoting or
  escaping; `#` starts a comment only on its own line or after
  whitespace.
- Keys marked `reboot = 1` in the schema (currently only
  `net.driver`) are flagged: `ecf_needs_reboot()` reports that a
  restart is required before the change takes effect.

## Applying and writing files

| Form | Effect |
| --- | --- |
| `set -a FILE` | Parse `FILE` and apply **every schema-valid** entry to the in-memory store. Unknown keys are kept. |
| `set -w FILE` | Serialize the whole in-memory store to `FILE`, preserving section layout and comments from the last load where possible (`ecf_set_file` patches in place for single keys). |

A typical provisioning session:

```sh
set -a /equinox/conf/drivers.ecf   # pull extras in
set eggkg.server https://example
set -w /equinox/conf/system.ecf    # persist
```

## Registering the system config

```sh
set -d FILE [-path DIR] [-base SRC]
```

Registers `FILE` as the active `.ecf` store for subsequent boots. The
pair `(-path, -base)` answers "where is the install root?":

- `-path DIR` — directory prefix searched when resolving `FILE`;
- `-base SRC` — a pivot base, the same value `set -b` records.

`set -d` writes the registration into the current `system.ecf`
(`active.conf = FILE`); run `set -w` afterwards to persist it.

## Base-directory pivot

```sh
set -b /mnt/equinox
```

Records `base.path = /mnt/equinox` in the store. The kernel re-applies
the pivot **at the next boot** (end of `fat32_boot_init()`), shifting
all relative paths (`conf/system.ecf`, `equinox/…`) under that root.
This is what lets a second, hand-built install on `/mnt` become the
active system after reboot.

## Running eqshell scripts

```sh
set -x setup.es
```

`set -x` executes a `.es` file: lines are run as shell commands in
order (builtins included), nesting is capped at 3 levels, and
`Log=True` in the script turns on transcription to `/eqshell.log`.
See [EQSHELL.md](EQSHELL.md).

## Where the store lives

The canonical store is **`/equinox/conf/system.ecf`** (resolved by
`ecf_active_path()`), with the legacy `boot/system.ecf` on a volume as
fallback, and an `active.conf` overlay redirect. Full details in
[SYSTEM_ECF.md](SYSTEM_ECF.md).

## Programmatic equivalent

The shell builtin is a thin wrapper over `ecf.c`:

| Shell | C API |
| --- | --- |
| `set` / `set list` | `ecf_get` over `ecf_store()` |
| `set k v` | `ecf_put(ecf_store(), k, v)` |
| `set -a F` | `ecf_load_merge(F, ecf_store())` |
| `set -w F` | `ecf_write_store(F, ecf_store())` |
| `set -d/-b` | writes `active.conf` / `base.path` then `ecf_store_invalidate()` |

After any edit outside `set` itself, call `ecf_store_invalidate()` so
the next `ecf_store()` reloads from disk.

## Writing through `ecf_caller`

`call set.key <key> <val> [-> <ecf>]` performs the same patch as
`set k v` but names its target file explicitly and works from any
caller (shell, a `.ecf` recipe step, another handler). When the target
is the active store the handler also syncs the in-memory store, so a
following `set KEY` reads back the fresh value — same coherence rule as
the `set` builtin.

`call set.path_local <path>` and `call set.active <file>` cover the
`eggkg.local` and store-pivot cases. See [ECF.md](ECF.md) for the
registry and the full action table.
