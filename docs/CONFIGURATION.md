# Configuration — `.ecf`, the `set` builtin and eqshell scripts

Equinox keeps almost all persistent configuration in **`.ecf` files**
(*Equinox Config File*) — a tiny INI-lite format — manipulated by the
**`set`** builtin and automated by **eqshell scripts** (`.es`).

## The `.ecf` format

```ini
# comment
[net]                     # section -> keys become "net.key"
driver = e1000

net.driver = ne2000       # dotted keys are taken as-is (equivalent)

[dependencies]
bash = true

[eggkg]
server = https://raw.githubusercontent.com/amnottdevv/Eggkg-l
```

Rules (parsed by `kernel/library/ecf.c`):

- `#` starts a comment; blank lines are ignored.
- `[section]` prefixes subsequent keys until the next section — the
  store key becomes `section.key`.
- A key containing a dot (`net.driver`) is stored verbatim.
- Values are trimmed strings; no quoting or escaping.
- Free-form keys are allowed — unknown keys are preserved by `set -w`
  and ignored by consumers.

## Where the files live

| File | Role |
| --- | --- |
| `/equinox/conf/system.ecf` | **The system configuration.** Written by `equinoxinstall` (phase [3/4]), read at every boot |
| `boot/system.ecf` (on a volume) | Legacy location — still honoured as a fallback |
| `<name>.ecf` pointed to by `active.conf = <file>` | Overlay: `system.ecf` may redirect the active store to another file (e.g. `drivers.ecf`) |

At boot the kernel resolves the active store through `ecf_active_path()`
(`conf/` first, legacy `boot/` fallback), parses it and dispatches the
known keys (`net.driver` → `net_nic_init()`,
`dependencies.bash` → `/bin` sync, `eggkg.*` → package manager).

## The `set` builtin

| Form | Function |
| --- | --- |
| `set` | Show the active store |
| `set <key> <value>` | Set a key in memory (`set net.driver e1000`) |
| `set list` | List all keys |
| `set -a FILE` | Parse FILE and apply every valid entry |
| `set -w FILE` | Write the **entire** active store to FILE (the save) |
| `set -d FILE [-path DIR] [-base SRC]` | Register FILE as the system config (optionally with base dir) — written into `system.ecf` so boots pick it up |
| `set -b PATH` | Record a base-directory pivot (applied at next boot) |
| `set -x FILE` | Execute an eqshell script (`.es`) — see below |

Typical flow — change the NIC driver persistently:

```sh
set net.driver e1000
set -w /equinox/conf/system.ecf
```

The GUI side: the settings editor (`gui` → settings) edits the same
store and runs an mtcc compile-check on save-time scripts.

## System keys used by 0.4

| Key | Consumer | Meaning |
| --- | --- | --- |
| `net.driver` | `net_nic_init()` | `ne2000` (default) / `e1000` / `none` — see [DRIVERS.md](DRIVERS.md) |
| `dependencies.bash` | boot sync | `true` → sync `/equinox/.local` into `/bin` before the shell starts |
| `eggkg.server` | `eggkg update` | Package server URL or local path |
| `eggkg.mirror` | `eggkg update` | Fallback mirror |
| `eggkg.local` | eggkg | Local staging root |

Unknown keys are allowed and round-trip through `set -w`.

## eqshell scripts (`.es`)

An eqshell script is a plain text file whose **first meaningful line
must be `[Eqshell]`**. Run it with `set -x script.es`.

```text
[Eqshell]
# comments start with '#'
echo provisioning the base install
Qfs -list-disk
set net.driver e1000
set -w /equinox/conf/system.ecf
Log=True
```

- Lines are shell commands executed in order (builtins included).
- Nesting is limited (3 levels) to keep failure modes simple.
- `Log=True` enables the transcript: every executed line + output is
  journaled to **`/eqshell.log`** (8 KB in-memory buffer; on overflow
  the transcript is truncated with a notice printed).

This is the standard hook for post-install provisioning: the manual
install path ([INSTALL.md](INSTALL.md)) can be captured as one `.es`
file and replayed with `set -x`.

## Quick reference card

```sh
set                          # show
set net.driver e1000         # edit one key
set list                     # list keys
set -a drivers.ecf           # apply a file
set -w /equinox/conf/system.ecf   # persist the store
set -d drivers.ecf -path /equinox/conf   # register system config
set -b /mnt/equinox          # pivot base dir (next boot)
set -x setup.es              # run an eqshell script
```
