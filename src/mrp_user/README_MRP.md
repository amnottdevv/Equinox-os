# .mrp Loader + Free-list malloc — v1

## What changed / was added

**Kernel (`kernel/library/`)**
- `malloc.cpp` — fully replaced the bump allocator with a **free-list allocator**
  (split + coalesce + canary). Two separate arenas:
  - Kernel heap: `0x300000 - 0x500000` (2 MB) — regular `malloc/free/calloc/realloc`
  - MRP arena: `0x500000 - 0x900000` (4 MB) — dedicated to `.mrp` programs,
    completely reset every time a new program is about to run
    (`mrp_heap_init()`) and after it finishes (`mrp_free_all()`)
- `header/malloc.h` — new declarations: `heap_check_integrity()`,
  `get_heap_free_blocks()`, `get_heap_largest_free()`, `mrp_alloc()`, etc.
- `header/mrp_format.h` — the `.mrp` header format (18 bytes) + `is_valid_mrp()`
- `header/mrp_loader.h` + `mrp_loader.cpp` — the loader: validate -> copy into
  the MRP arena -> `call` the entry point -> `mrp_free_all()`
- `fs_ram.cpp` / `fs_ram.h` — added `fs_write_binary()` (byte-safe, unlike the
  old `strcpy`-based writer, so `.mrp` files whose content is machine code are
  not truncated at the first `0x00` byte)
- `kernel.cpp` — new shell command: `run <name.mrp>`

**Userland (`mrp_user/`)**
- `mrp_api.h` — the `mrp_api_t` struct (MUST match the kernel version exactly)
  + the `MRP_ENTRY` macro
- `link_mrp.ld` — the linker script that places `_start` at offset 0
- `hello.cpp` — example program: print + read input + print again
- `mrp_pack.py` — compile .cpp -> link -> objcopy -> wrap with the `.mrp` header

## How to use

```bash
# compile + pack in one step (needs i686-elf-g++ on PATH)
python3 mrp_user/mrp_pack.py mrp_user/hello.cpp hello.mrp

# or if you already have your own .bin (e.g. from another toolchain / future TCC)
python3 mrp_user/mrp_pack.py --from-bin program.bin program.mrp hello.mrp
```

Then put `hello.mrp` into the Equinox OS RAMFS and run:
```
root@equinox:/$ run hello.mrp
```

## Open items (decide / implement yourself)

0. **[FIXED — bug history]** An early version of `link_mrp.ld` linked against
   address `0`, while the code actually runs at `0x500010` (the MRP arena).
   Because the build uses `-fno-pic`, every access to static variables /
   string literals resolved to the wrong address at run time (garbage reads /
   crashes). Fixed: `link_mrp.ld` now links at `0x500010`, exactly the address
   `mrp_alloc()` hands out at run time (deterministic because
   `mrp_heap_init()` always resets the arena before loading new code).
   **If you change `sizeof(block_header)` in `malloc.cpp` or move
   `MRP_HEAP_START`, the `0x500010` in `link_mrp.ld` MUST be adjusted too** —
   see the detailed comment in that file.
1. **How `.mrp` files get into the RAMFS.** The Equinox OS RAMFS can currently
   only be filled from the shell (`ccfile`, text only) — there is no path for
   binary files from outside QEMU. The easiest option: add a **GRUB multiboot
   module** (`module /boot/hello.mrp` in `grub.cfg`), then read
   `mb_info->mods_addr` in `kernel_main()` and call `fs_write_binary()` to
   stage the content into the RAMFS at boot. Not implemented in that iteration
   because the scope was the loader + malloc, but it is the next blocker you
   will hit as soon as you test `run` for real in QEMU.
2. **`.bss` in user programs can inflate the `.mrp` file size** — `objcopy
   -O binary` writes `.bss` as literal zero bytes into the file. Fine for
   small programs, but avoid large static buffers (`static char
   buf[100000];`); use `api->alloc()` at run time when you need a lot.
3. **No global-constructor support yet** (static objects with non-trivial
   constructors). For now, stick to plain / POD static variables.
4. **Still Ring 0** — `.mrp` programs have FULL access to kernel memory, not
   a sandbox. This was always planned to be fixed in the Ring 3 step.

## Why the design looks like this (short version)

- **Why not a binary string literal as the signature?** Long text signatures
  ("0110101...") are expensive to verify and fragile in the face of different
  encodings/whitespace. A 4-byte magic (`"MRP1"`) + checksum is far cheaper
  and unambiguous.
- **Why is `entry_offset` always 0?** The linker script puts `_start` first
  via the `.start` section. That removes any need for ELF symbol-table
  parsing in the packer — assuming offset 0 is enough, and `mrp_pack.py`
  automatically verifies it with `nm` as a sanity check.
- **Why syscalls via a function-pointer struct instead of `int 0x80`?** At
  Ring 0 there is no privilege transition, so calling through a pointer is
  sufficient and much simpler. The ABI shape (one struct holding all
  "syscalls") deliberately mirrors int 0x80 so that programs written now will
  not need rewriting when the Ring 3 migration happens — only the calling
  mechanism changes.
