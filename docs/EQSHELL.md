# eqshell scripting — `.es` files

An *eqshell script* is a plain-text file executed by the
kernel shell's `set -x` builtin. It is the standard hook for
post-install provisioning and declarative configuration.

## The contract

- The **first meaningful line must be `[Eqshell]`** — anything else is
  rejected (this keeps ordinary text files from being executed by
  accident).
- Every subsequent line is run as a shell command, in order, with the
  builtins available: `set`, `Qfs`, `mount`, `copy`, `mkdir`, …
- `#` starts a comment; blank lines are ignored.
- Nesting (`set -x` inside a `.es`) is capped at **3 levels**.
- `Log=True` on its own line enables transcript journaling.

## Example

```text
[Eqshell]
# provision a fresh install
echo step 1: disk inventory
Qfs -list-disk
set net.driver e1000
set -w /equinox/conf/system.ecf
echo step 2: packages
eggkg update
Log=True
```

## The transcript — `/eqshell.log`

When `Log=True` is seen, every executed command line and its output is
appended to `/eqshell.log` (RAMFS). The buffer is an 8 KB in-memory
ring; on overflow the transcript is **truncated with a notice** rather
than growing without bound — copy it out before rebooting if you need
the full history (RAMFS is volatile).

## Execution model

`set -x FILE` (implemented in `kernel/shell.cpp`, the same dispatcher
as an interactive line):

1. Read the file; the RAMFS-backed path resolves directly.
2. Verify the `[Eqshell]` header.
3. For each command line: run through the normal builtin / `.mrp`
   dispatcher, capture output when logging.
4. Exit status of the script = status of the last command.

Because scripts share the interactive dispatcher, every builtin
(including `equinoxinstall`, `eggkg`, and nested `set -x` up to depth
3) is available — there is no separate script language.

## Use cases

- Reproduce the manual install path: the whole procedure in
  [INSTALL.md](INSTALL.md) can be captured as one `.es` and replayed.
- Declarative config: a repo can ship `drivers.ecf` + `setup.es`
  applying NIC/driver choices on first boot.
- CI hooks: `ecf_test.py` and the installer harness use scripted
  command files with `Log=True` to assert on the transcript.
