# `system.ecf` — the system configuration file

`/equinox/conf/system.ecf` is **the** persistent system configuration
of Equinox OS. It is written by the `equinoxinstall` wizard (phase
`[3/4]`), read at every boot, and edited with the `set` builtin (or
the GUI settings editor). Everything below describes its schema and
resolution rules as implemented in `kernel/library/ecf.c`.

## Format

`.ecf` is an INI-lite format (see [ECF.md](ECF.md) for the exact
grammar):

```ini
# Equinox system configuration
[net]
driver = e1000          # ne2000 | e1000 | none

[dependencies]
bash = true             # sync /equinox/.local into /bin at boot

[eggkg]
server = https://raw.githubusercontent.com/amnottdevv/Eggkg-l
mirror =
local  = /equinox/.local

[base]
path =                  # recorded by `set -b`; applied at next boot

[active]
conf =                  # overlay pointer set by `set -d`
```

Section keys flatten with a dot: `[net] driver` → `net.driver`.

## The known keys

| Key | Consumer | Values | Notes |
| --- | --- | --- | --- |
| `net.driver` | `net_nic_init()` (`kernel/net/nic.c`) | `ne2000`, `e1000`, `none` | Schema-flagged as **reboot-required**. `none` disables networking. |
| `dependencies.bash` | `eggkg_boot_check()` | `true` / `false` | Boot-time: copy `/equinox/.local/<pkg>/*.mrp` into `/bin` when the package is enabled. |
| `eggkg.server` | `eggkg update` | URL or local path | Primary package index source. |
| `eggkg.mirror` | `eggkg update` | URL or local path | Fallback when the server fails. |
| `eggkg.local` | `eggkg` build/install | path | Local staging root for sources and builds. |
| `base.path` | boot (`fat32_boot_init` tail) | path | Pivot root applied at **next boot** (`set -b`). |
| `active.conf` | `ecf_active_path()` / boot | filename | Overlay: redirect the *active store* to another `.ecf`. |

Unknown keys are legal and must round-trip unchanged through
`set -w` — the schema is a hint for validators, not a hard gate.

## Resolution at boot

1. `ecf_active_path(0)` picks the canonical file:
   `/equinox/conf/system.ecf` first, then the legacy
   `boot/system.ecf` on the volume.
2. `ecf_store()` parses that file into the global store and follows
   the `active.conf` overlay (one redirect) — that file is parsed with
   `ecf_parse_merge`, so overlapping keys are **overridden** by the
   overlay.
3. Consumers run: `net_nic_init()` reads `net.driver`,
   `eggkg_boot_check()` reads `dependencies.bash`, eggkg reads
   `eggkg.*`.
4. The `base.path` pivot, if present, is applied at the end of
   `fat32_boot_init()`.

Neither `base.path` nor the `active.conf` target is consulted until a
reboot — `set -b` / `set -d` therefore always say "applies at next
boot".

## Managing it

```sh
set                                        # show the active store
set net.driver e1000                       # edit in memory
set -w /equinox/conf/system.ecf            # persist
set -a /equinox/conf/drivers.ecf           # merge another file in
set -d drivers.ecf -path /equinox/conf     # make it the active store
set -b /mnt/equinox                        # pivot base (next boot)
```

The GUI settings editor (`gui` → settings) edits the same store; when
an eqshell script is the launcher, the GUI runs an mtcc compile-check
before saving.

## Failure modes

- File missing → `ecf_store()` serves an empty store; consumers fall
  back to defaults (`ne2000`, no bash package, no mirror).
- Bad section header / stray `=` on its own line → the line is skipped;
  parsing continues (INI-lite is deliberately forgiving).
- Invalid value for a schema key (`net.driver = ixgbe`) → rejected by
  `ecf_check` at `set -a`/GUI validation time with an error naming the
  allowed values.
