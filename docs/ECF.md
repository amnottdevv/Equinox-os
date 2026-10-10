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

`.ecf` tidak hanya untuk store sistem; tiap tool punya berkas
konfigurasi sendiri di `.config/`:

```text
/equinox/conf/system.ecf      # store global (set/get shell, wizard)
/equinox/.config/eggkg.ecf    # perilaku eggkg (resep build/paket)
/equinox/.config/mtcc.ecf     # default mtcc (format/flags/set/spawn)
```

Resolver `ecf_tool_path(tool, for_write)` (`kernel/library/ecf.c`)
mengembalikan path absolut ke `<tool>.ecf`:

- **Baca** (`for_write = 0`): cek keberadaan berkas di kandidat
  `/mnt/equinox/.config/<tool>.ecf` lalu `/equinox/.config/<tool>.ecf`
  — **tidak membuat apa pun**. `NULL` bila tidak ada.
- **Tulis** (`for_write = 1`): kalau berkas sudah ada, pakai lokasinya;
  kalau belum, buat dir `.config` di kandidat pertama yang bisa
  (`/mnt/equinox` bila volume terpasang, selainnya RAMFS).

Karena config dibaca **tiap invocation** (tanpa cache boot), edit
langsung hidup — tidak perlu reboot atau recompile.

## Dispatch — `ecf_caller` (aksi berbasis string)

`ecf_caller.c` adalah analog string dari `syscall_table[]`: registry
statis (tanpa heap) yang memetakan nama aksi berdotted ke handler.
Dipakai oleh lapisan config eggkg supaya urutan build/paket datang dari
teks `.ecf`, bukan hardcode.

```c
typedef int (*ecf_call_fn)(const struct ecf_call_ctx* ctx);
int  ecf_call_register(const char* name, ecf_call_fn fn); // dup: last-wins
int  ecf_call(const char* name, struct ecf_call_ctx* ctx); // -1 = unknown
int  ecf_call_exists(const char* name);
int  ecf_call_count(void);
const char* ecf_call_name_at(int idx);
```

`ecf_call` mengembalikan `-1` + `[ERROR] ecf_call: unknown action
'<nama>'` untuk aksi tak dikenal. Registry terbuka: modul lain boleh
mendaftarkan handler sendiri via `ecf_call_register` (ring-0 only).

Interactive entry: shell `call <nama> [args...]` memanggil `ecf_call`
dengan `caller = "shell"`.

### Aksi terdaftar

| Aksi | Sumber | Pekerjaan |
| --- | --- | --- |
| `set.key <key> <val> [-> <ecf>]` | `config_cmd.cpp` | tulis key=val ke `.ecf` |
| `set.path_local <path>` | `config_cmd.cpp` | `eggkg.local = <path>` |
| `set.active <file>` | `config_cmd.cpp` | pivot store aktif |
| `mtcc.compile_ruf_eggkg <ruf>` | `eggkg.cpp` | spawn mtcc `-make` |
| `pkg.install_bin <pkgdir>` | `eggkg.cpp` | pindah `.mrp` ke `/bin` |
| `pkg.db_record <nama> <ver>` | `eggkg.cpp` | catat `installed.db` |
| `pkg.read_list <url>` | `eggkg.cpp` | unduh + parse `package.list` |
| `pkg.fetch_index <repobase>` | `eggkg.cpp` | unduh + parse `index.idx` (opsional) |

## Resep eggkg — `.config/eggkg.ecf`

Format = daftar langkah berurut per section; `$var` di-resolve saat
step dieksekusi (`$rufpath $pkgdir $name $version $server $repobase
$local`):

```ini
[update]
1 = pkg.read_list $server
2 = pkg.fetch_index $repobase
[install]
1 = mtcc.compile_ruf_eggkg $rufpath
2 = pkg.install_bin $pkgdir
3 = pkg.db_record $name $version
```

- `eggkg init` menulis resep default (tak menimpa yang sudah ada).
- `eggkg plan` menampilkan resolved `[install]` tanpa eksekusi.
- Resep adalah **satu-satunya jalur eksekusi**: run pertama auto-seed
  lalu baca-balik; validasi **atomik** (seluruh daftar divalidasi
  sebelum eksekusi apa pun, berhenti di step gagal pertama).
- Stage yang masih monolit (disebut jujur di komentar & resep):
  langkah unduh sumber `[2/4]` di setup install.

## `.config/mtcc.ecf` — default mtcc

```ini
[format]
default = mrp            # mrp|elf  (CLI `-format` menang)
[flags]
default =                # -q, -d/--debug, --lib (CLI menambah)
[set]
store =                  # tujuan `set` pada .ruf ("" = system.ecf)
[spawn]
name = mtcc.mrp          # tool yang di-spawn shell untuk build
args =                   # argumen ekstra saat spawn
```

`config init mtcc` me-seed template ini. `mtcc -make` juga membaca
`buildir.default` / `rufdir.default` sebagai nilai default `$buildir`
/ `$rufdir` bila resep tidak menyetelnya sendiri.

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
