# Customization — the layered configuration model

Equinox OS is built to be **edited while it runs**. Almost every
behaviour that would be hard-coded in a conventional kernel is instead
expressed as text in a `.ecf` file, read on the invocation that needs
it — so a change takes effect on the next command, with no reboot and
no recompile.

This page documents that model end to end: the layers, the dispatch
mechanism, and the per-tool configuration files. The `.ecf` format
itself is specified in [ECF.md](ECF.md); the system-wide file in
[SYSTEM_ECF.md](SYSTEM_ECF.md); the editing tools in
[SET.md](SET.md).

---

## 1. Why layers

A single flat config file scales badly. It conflates three different
concerns — the machine's identity (which NIC driver, which package
server), each tool's private defaults, and the *behavioural* wiring
that decides what runs when. Mixing them means editing one tool's
defaults risks disturbing another's, and every new tool adds keys to a
file nobody fully owns.

Equinox separates them into layers, each with a single owner and a
single job:

| Layer | Files | Owner | Contains |
| --- | --- | --- | --- |
| **1. System store** | `/equinox/conf/system.ecf` (+ overlays) | the kernel & wizard | machine identity: `net.*`, `eggkg.*`, `dependencies.*`, active-store pointer |
| **2. Per-tool config** | `/equinox/.config/<tool>.ecf` | that tool | the tool's private defaults: `eggkg.ecf`, `mtcc.ecf` |
| **3. Action registry** | in-kernel (`ecf_caller`) | each module that registers | the named actions a config may invoke |
| **4. Recipes** | the step lists *inside* `.ecf` | the user | the order and arguments of what actually runs |

Read downward: layer 1 says *what the machine is*, layer 2 says *how a
tool behaves by default*, layer 3 says *what can be invoked*, and
layer 4 says *in what order, with which arguments*. The lower layers
are the ones you edit daily; the upper ones are the ones the system
owns.

The analogy to an onion is deliberate: each layer is only reachable
through the one outside it, and peeling one back reveals the next
without the outer layers needing to know what is inside.

---

## 2. Layer 1 — the system store

`/equinox/conf/system.ecf` is the machine's identity file. It is
written by `equinoxinstall` during phase `[3/4]` and read at every
boot.

```ini
[net]
driver = e1000

[eggkg]
server = https://raw.githubusercontent.com/amnottdevv/Eggkg-l
local  = /equinox/.local

[dependencies]
bash = true
```

At boot the kernel resolves the active store through
`ecf_active_path()`, parses it, and dispatches the keys it recognises
— `net.driver` brings up the NIC, `dependencies.bash` re-syncs
`/equinox/.local` into `/bin` before the shell starts, `eggkg.*`
configures the package manager.

This layer is documented in full in [SYSTEM_ECF.md](SYSTEM_ECF.md).
It is the layer you touch when *provisioning* a machine, not the one
you touch when tuning a tool.

### Overlays and redirection

`system.ecf` may point the active store at another file:

```ini
active.conf = /equinox/conf/drivers.ecf
```

The overlay is merged on top of the base store (later keys win), so a
volume can carry its own overrides without editing the canonical file.
See [ECF.md](ECF.md#path-resolution-ecf_active_path).

---

## 3. Layer 2 — per-tool configuration

Each configurable tool owns one file in `.config/`:

```text
/equinox/.config/eggkg.ecf     # package manager: the build/install recipe
/equinox/.config/mtcc.ecf      # compiler: format, flags, set store, spawn
```

Two properties make this layer genuinely live rather than decorative:

**Read per invocation, never cached at boot.** Each tool re-reads its
file when it runs. An edit is picked up by the next command. There is
no cache to invalidate, no daemon to restart.

**A single file per tool, edited as text.** There is no drop-in
directory, no fragments to merge, no generated cache. You edit one
file, in the terminal, with any editor:

```sh
edit /equinox/.config/eggkg.ecf
```

### Resolution — `ecf_tool_path()`

The kernel resolves a tool's config path through `ecf_tool_path(tool,
for_write)` in `kernel/library/ecf.c`. Its behaviour is asymmetric by
design:

- **Reading** (`for_write = 0`) checks for the file's *existence* at
  `/mnt/equinox/.config/<tool>.ecf` and then
  `/equinox/.config/<tool>.ecf`, and returns `NULL` if absent. It
  creates nothing — a read is never allowed to mutate the filesystem.
- **Writing** (`for_write = 1`) reuses an existing file's location if
  there is one; otherwise it creates the `.config` directory at the
  first candidate that works — the persistent `/mnt/equinox` volume if
  one is mounted, RAMFS otherwise.

The `/mnt` candidate comes first so that, on an installed system, a
tool's configuration lives on the durable volume rather than in the
session-scoped RAM filesystem.

### Seeding

A tool that has never been configured does not fail — it seeds itself
and then reads back what it wrote, so the config file remains the only
execution path:

```sh
eggkg init             # writes .config/eggkg.ecf (the default recipe)
config init mtcc       # writes .config/mtcc.ecf (the default template)
```

`eggkg init` will not overwrite an existing recipe; `config init mtcc`
likewise leaves an existing file untouched. Both are idempotent
conveniences, not required steps — running the tool with no config
file produces the same seeded defaults.

---

## 4. Layer 3 — the action registry (`ecf_caller`)

This is the layer that turns configuration into *behaviour*.

`kernel/library/ecf_caller.c` is a string-keyed analogue of
`syscall_table[]`. Where the syscall table maps a number to a function
pointer, the registry maps a **dotted action name** to a handler:

```text
mtcc.compile_ruf_eggkg     pkg.install_bin      pkg.db_record
pkg.read_list              pkg.fetch_index      set.key
set.path_local             set.active
```

### The contract

```c
typedef int (*ecf_call_fn)(const struct ecf_call_ctx* ctx);

int  ecf_call_register(const char* name, ecf_call_fn fn);
int  ecf_call(const char* name, const struct ecf_call_ctx* ctx);
int  ecf_call_exists(const char* name);
int  ecf_call_count(void);
const char* ecf_call_name_at(int idx);
```

Every handler receives the same context:

```c
struct ecf_call_ctx {
    int         argc;                    /* resolved argument count   */
    const char* argv[ECF_CALL_ARGC_MAX]; /* $var already substituted  */
    const char* caller;                  /* "eggkg", "shell", ...     */
};
```

`ecf_call` performs an exact-match lookup and invokes the handler,
returning its result. An unknown name returns `-1` and prints:

```text
[ERROR] ecf_call: unknown action 'no.such.action' (try: config callers)
```

### Design properties

**The registry is open.** Any module registers its own handlers during
initialisation. Adding a capability means writing a handler and
registering it — the dispatch core is never touched. `config_cmd.cpp`
registers the `set.*` family; `eggkg.cpp` registers the `pkg.*` and
`mtcc.*` families.

**No heap.** The registry is a fixed static array (48 slots), following
the same discipline as `ecf.c`. There is nothing to allocate and
nothing to free.

**Ring-0 only.** It is an in-kernel library. The interactive entry
point is the shell's `call` builtin.

**Names are namespaced.** `<namespace>.<action>[_qualifier]`, where
each namespace owns its own name space — `mtcc.compile_ruf_eggkg` and
a hypothetical `mtcc.compile_ruf_wizard` are distinct actions, not
variants of one.

### Introspection

```sh
config callers
```

lists every registered action. This is both a discovery tool and the
mechanism the error message above points you toward — you never have
to guess what a config file is allowed to invoke.

---

## 5. Layer 4 — recipes

The top layer is the one you edit most: a step list, held *inside* a
tool's config file, that names actions in order and supplies their
arguments.

### The eggkg recipe

`.config/eggkg.ecf` holds two sections, each an ordered list:

```ini
[update]
1 = pkg.read_list $server
2 = pkg.fetch_index $repobase

[install]
1 = mtcc.compile_ruf_eggkg $rufpath
2 = pkg.install_bin $pkgdir
3 = pkg.db_record $name $version
```

Each line is `<order> = <action> <args…>`. Arguments beginning with
`$` are variables resolved **at the moment that step runs**:

| Variable | Resolves to |
| --- | --- |
| `$server` | the package server (`eggkg.server`, or an explicit `eggkg update <path>` override) |
| `$repobase` | the repository base, derived during `update` |
| `$rufpath` | the build recipe for the package being installed |
| `$pkgdir` | the staging directory the sources were fetched into |
| `$name` | the package name |
| `$version` | the package version |
| `$local` | the local staging root (`eggkg.local`) |

Because `$repobase` is derived *by* step 1 of `[update]`, step 2
resolves it correctly — variables are substituted per-step, not
pre-expanded.

### Read fully, then run

A recipe is **validated in full before anything executes**. The entire
step list is parsed, every action name is checked against the registry,
and only then does execution begin. If any action is unknown, the whole
recipe is rejected with a per-step error and *nothing runs*. Execution
also stops at the first failing step — a partial install is never left
behind.

This is what keeps configuration honest: a recipe is either entirely
valid and runs in the order you wrote, or it is rejected whole. There
is no half-applied state.

### The recipe is the only execution path

`eggkg install` does not contain a hard-coded build-and-install
sequence that the config merely *influences*. The step list **is** the
execution path. On a first run with no config file, the defaults are
seeded and then read back from the file — so even the default
behaviour travels through the same config-driven interpreter.

Handlers wrap the existing, tested monolith logic rather than
reimplementing it. `mtcc.compile_ruf_eggkg` delegates to the same
function that spawns `mtcc -make`; `pkg.install_bin` wraps the same
`.mrp`-to-`/bin` move including its zero-copy relinking. The ordering
comes from the config; the work is the proven code.

### What is honestly still monolithic

Two stages remain in the monolithic path, and this is stated in the
recipe comments rather than hidden:

- **Fetch.** The source-download stage of `install` manages its buffer
  carefully — it is freed before the `mtcc` spawn to avoid heap
  fragmentation in the 256 MB kernel. Extracting it into a handler
  would risk that discipline, so it stays inline.
- **`index.idx` verification.** Hash verification during `update`
  follows the same reasoning.

Everything after fetch — build, install to `/bin`, database record —
runs from the recipe.

### Inspecting without running

```sh
eggkg plan
```

prints the resolved `[install]` steps — after variable resolution, in
execution order — without executing any of them. It is the dry-run
counterpart to the validate-then-run guarantee: you can see exactly
what a recipe will do before it does it.

---

## 6. Triggering actions by hand

Any registered action can be invoked directly from the shell, outside
any recipe:

```sh
call set.path_local /equinox/.local/tp
call set.key foo bar
call pkg.db_record mytool 1.0
```

`call` builds a context with `caller = "shell"`, resolves no variables
(arguments are taken literally), and dispatches. It is the same
`ecf_call` the recipes use — a recipe is just a list of calls the
config supplies the order for.

This makes actions composable: a handler registered by one module can
be driven by a recipe owned by another, or by a person at a prompt,
with no glue code in between.

---

## 7. Live editing, end to end

The whole model converges on one property: **a change is live on the
next command.**

```sh
# retarget the compiler's default output format
edit /equinox/.config/mtcc.ecf        # [format] default = elf
mtcc -c prog.c                        # now emits ELF — no reboot

# reorder an install
edit /equinox/.config/eggkg.ecf       # move db_record before install_bin
eggkg plan                            # confirm the new order
eggkg install mytool -y               # runs it

# change what `set` writes to from a recipe
call set.active /equinox/conf/other.ecf
```

No step in that sequence requires a rebuild of the kernel, a restart
of a service, or a reboot. The compiler, the package manager and the
dispatch registry all re-read what they need at the point of use.

---

## 8. Tricks & recipes

Everything in this section is configuration only: no kernel rebuild, no
recompile, no service restart. Each one is a text edit plus the command
that proves it took effect.

### Make ELF the default output

```ini
# /equinox/.config/mtcc.ecf
[format]
default = elf
```

`mtcc -c prog.c` now writes a static ELF32 by default, and `mtcc -make`
produces ELF products, so you can stop passing `-format elf` on every
build. A recipe's `format` directive and the CLI `-format` still
override it, so this is a site-wide default rather than a hard lock.

One caveat worth knowing: `-format` only applies to `-c` and `-make`.
In run mode (`mtcc prog.c`, which executes in memory and never writes a
file) the setting is ignored and mtcc prints
`-o / -format only apply to -c (run mode never writes a file)`. Leave
`format.default = mrp` if run mode is the common case.

### Build quietly by default

```ini
[flags]
default = -q
```

`-q` is read at process start, so it applies to **every** subsequent
compile, including the `mtcc -make` spawned by an eggkg install. Only
`-q`, `-d`/`--debug` and `--lib` are recognised here, and the flags are
additive: mtcc has no negative flags, so a command line can add to this
default but never subtract from it.

### Inject flags into every spawned compile

```ini
[spawn]
args = -q --lib
```

The shell prepends `spawn.args` to the command line when it spawns the
compiler, deliberately *before* the arguments you typed, so config flags
are parsed first. It reaches every spawn path — the shell's `mtcc`,
`equinoxinstall -build`, and the `mtcc -make` that an eggkg install
performs, since all three funnel through the same spawn helper. Use it
for flags you never want to retype.

### Swap the compiler binary

```ini
[spawn]
name = mtcc_dbg.mrp
```

Every `mtcc` and `mtcc -make` now looks for `mtcc_dbg.mrp` instead of
`mtcc.mrp`. The search order is the request's own directory, then
`/equinox/tools`, then the alternative directory — so dropping a second
binary alongside the first is enough to A/B two compiler builds without
renaming either.

### Decide where a recipe's `set` writes

```ini
[set]
store = /equinox/conf/experiment.ecf
```

Recipe `set key = value` lines go to this file instead of the system
store. A recipe can override the same thing inline:

```text
ecf /equinox/conf/experiment.ecf
set eggkg.local = $buildir
```

Both are inert until the recipe builds cleanly: `set` lines are queued
alongside `copy`/`move` and flushed only after **every** job succeeded,
so a broken build cannot half-update a config.

### Default the build output folder

```ini
[buildir]
default = /equinox/.local/staging
```

Provides `$buildir` when a recipe does not set `out`. This is a
fallback, not an override — a recipe's own `out <dir>` always wins, and
so does an explicit `out =` line. (`rufdir.default` follows the same
rule for `$rufdir`, but the recipe's own folder is normally derived
automatically, so it is rarely reached.)

### Reorder, trim, or extend an install

```ini
# /equinox/.config/eggkg.ecf
[install]
1 = mtcc.compile_ruf_eggkg $rufpath
2 = pkg.install_bin $pkgdir
3 = pkg.db_record $name $version
4 = set.key last.built $name
```

Lines run in **file order** — the leading number is decoration, so
reordering means moving lines, not renumbering them. You can delete a
step entirely (dropping `pkg.db_record` gives a build with no database
entry), duplicate one, or append a new action from any module that has
registered a handler. Then confirm before running it:

```sh
eggkg plan          # resolved [install] steps, in order, nothing executed
```

Validation still applies to the edited file as a whole: one unknown
action name rejects the section before a single step runs.

### Run against an offline or local repository

```sh
eggkg update /equinox/repo           # overrides $server for this run
call set.path_local /equinox/.local  # or persist eggkg.local
eggkg update
```

`$server` resolves to the explicit argument when one is given, otherwise
to `eggkg.server` from the system store — so the same recipe works
against a network mirror, a local folder, or nothing at all, without a
single line changing in `eggkg.ecf`.

### Drive one action at a time

```sh
config callers                                   # what may be invoked
call set.key net.driver e1000
call set.path_local /equinox/.local/tp
call pkg.db_record mytool 1.0
```

A recipe is only a list of these calls with the order supplied by
configuration. Invoking one by hand is the same dispatch path with
`caller = "shell"`, which makes a handler registered by one module
usable by another module's recipe — or by a person at a prompt.

### Retarget the entire store

```sh
call set.active /equinox/conf/other.ecf    # pivot now
set -d /equinox/conf/other.ecf             # pivot and persist to boot
set -a /equinox/conf/other.ecf             # merge it over the store
set -w /equinox/conf/other.ecf             # save the result back
```

`call set.active` pivots the **session** immediately (it sets the
`set -d` target override and invalidates the store cache); `set -d`
writes the `active.conf = <path>` pointer into the active `.ecf`, so the
next boot sees it too. Because that pointer is also followed by
`ecf_file_get`, per-tool config files inherit the redirection when they
look up a key the main file does not carry.

### Provision a session with a script

```text
# /equinox/profile.es
[Eqshell]
Log=True
call set.key net.driver e1000
call set.path_local /equinox/.local
config init mtcc
eggkg plan
```

```sh
set -x profile.es
```

Every line runs through the ordinary shell dispatch, so a script is just
a recorded sequence of the same commands — and `Log=True` captures the
output to `/eqshell.log`. Note the honest limit: there is **no boot
autostart**; a `.es` file runs when you ask for it, not when the system
comes up.

A detail worth knowing when scripting: the shell's `set KEY VAL` edits
the **in-memory** store and is permanent only after `set -w FILE`,
whereas `call set.key KEY VAL [-> FILE]` writes the file immediately.
Pick the former when you want to stage several edits and save once,
the latter when a single call has to be durable on its own.

### Discover what is possible

| Command | Answers |
| --- | --- |
| `config callers` | every action a config file may invoke |
| `config list` | the active store and which `.config/*.ecf` files exist |
| `config show <tool>` | the full text of one tool's config |
| `config get <tool> <key>` | one value from one tool's config |
| `eggkg plan` | what `[install]` will do, in order, before it does it |
| `set` | every key in the active system store |

Nothing in the configuration layer is undocumented-by-omission: the
introspection commands are the discovery mechanism, and the error
messages point at them.

---

## 9. The layers in one view

```text
  ┌──────────────────────────────────────────────────────────┐
  │  1  system.ecf          machine identity (net, eggkg)    │
  │     └─ overlays, active.conf redirection                 │
  ├──────────────────────────────────────────────────────────┤
  │  2  .config/<tool>.ecf  per-tool defaults, live          │
  │     ├─ eggkg.ecf       the build/install recipe          │
  │     └─ mtcc.ecf        format, flags, set, spawn         │
  ├──────────────────────────────────────────────────────────┤
  │  3  ecf_caller          named actions (open registry)    │
  │     └─ pkg.*  mtcc.*  set.*      register → dispatch     │
  ├──────────────────────────────────────────────────────────┤
  │  4  recipes             ordered steps + $variables       │
  │     └─ validate fully → run in order → stop on failure   │
  └──────────────────────────────────────────────────────────┘
           ▲                    ▲                   ▲
       provisioning        tuning a tool      changing behaviour
```

Each layer is reached only through the one outside it, each has a
single owner, and the innermost ones are the ones you edit while the
system is running.

---

## See also

- [ECF.md](ECF.md) — the `.ecf` format, `ecf_tool_path`, the registry API, action table
- [CONFIGURATION.md](CONFIGURATION.md) — the `set` builtin and eqshell scripts
- [SYSTEM_ECF.md](SYSTEM_ECF.md) — every key in the system store
- [SET.md](SET.md) — editing configuration, including through `ecf_caller`
- [PACKAGES.md](PACKAGES.md) — the eggkg lifecycle this recipe drives
- [MTCC_LANGUAGE.md](MTCC_LANGUAGE.md) — what `.config/mtcc.ecf` tunes
