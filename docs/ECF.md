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
