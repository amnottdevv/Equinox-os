# MRP1 — the Equinox executable format

`.mrp` is the native ring-3 program format of Equinox OS. A file is an
18-byte fixed header followed by a flat x86-32 code payload. Every
userland tool produced by `mtcc` is an MRP1 file; the kernel loads it
with demand paging (see [MEMORY.md](MEMORY.md)).

## File layout

```
offset  size  field
0       4     magic "MRP1"
4       1     version (== 1)
5       4     entry_offset  — offset of the entry point within the code
9       4     code_size     — size of the code payload (bytes after header)
13      1     flags         — MRP_FLAG_* (0x01 = NEEDS_GUI, reserved)
14      4     checksum      — rotate-xor over the code payload
18      …     code payload (flat binary, loaded at 0x500010)
```

```c
struct __attribute__((packed)) mrp_header {
    uint8_t  magic[4];
    uint8_t  version;
    uint32_t entry_offset;
    uint32_t code_size;
    uint8_t  flags;
    uint32_t checksum;
};
/* sizeof == 18 */
```

At runtime the payload is mapped at **`MRP_LOAD_BASE = 0x500010`**; the
program's entry point is `MRP_LOAD_BASE + entry_offset`.

## Checksum

```c
uint32_t sum = 0x811C9DC5u;
for (i = 0; i < len; i++)
    sum = ((sum << 5) | (sum >> 27)) ^ data[i];   /* rotate-xor */
```

Cheap, deterministic, identical on the packer (`mtcc -c`) and the
validator. It is a corruption check, **not** a security boundary.

## Validation rules (`is_valid_mrp`)

A file is loadable only if **all** hold:

1. `total_len ≥ 18`;
2. `magic == "MRP1"`;
3. `version == 1`;
4. `code_size > 0`;
5. `code_size == total_len - 18` (no truncation / no trailer);
6. `entry_offset < code_size`;
7. checksum matches.

Failure maps to a precise reason (`MRP_ERR_*`) that the loader prints
as an errno-style message (`mrp: exec rejected (EBUSY)`, corrupt file,
…).

## Loading (`mrp_loader.cpp`)

1. `fs_find_child` + `fs_ensure_content` (FAT32 files are pulled in
   lazily);
2. non-empty file, ELF check first — ELF32 images go through
   `elf.cpp` instead;
3. `is_valid_mrp` validation;
4. reserve the demand window `USER_ARENA_START` sized
   `code_size + arena_hint` (8 MB default, 20 MB max, per-name policy
   in `mrp_arena_hint_for()`);
5. `mrp_heap_init()`, copy the payload byte-wise, compute
   `entry = exec_buf + hdr->entry_offset`;
6. jump via the user3 trampoline on a fresh ring-3 stack;
7. on exit: FD flush, sched-lock release, `task_user_unmap`, console
   restore.

A nested `mrp_run` inside the same task is rejected with
`MRP_RUN_ERR_BUSY` — programs spawn new tasks instead of re-entering
loaders.

## Interfacing with the OS

Compiled programs never see the kernel's C++ API; they call the OS via
`int 0x80` with the syscall numbers from
`kernel/library/header/syscall.h` (`print`, `open`, `read`, `write`,
`spawn`, `pipe`, …). Classic `.mrp` programs written against the C SDK
receive a `struct mrp_api_t*` as their entry argument instead.

## Why not ELF everywhere?

MRP1 exists so `mtcc` can emit a program with **zero toolchain
metadata** — a flat binary plus 18 bytes. It is trivially inspectable,
trivially packable (`mrp_pack.py`), and the loader stays tiny. ELF32
static images are supported alongside for externally linked programs.
