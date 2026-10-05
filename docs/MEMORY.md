# Memory — map, paging, demand allocation

Equinox OS runs flat **identity-mapped** for the kernel and gives every
ring-3 program a private page directory. This page documents the layout
and the demand-paging machinery; source of truth is
`kernel/library/paging.cpp`, `kernel/library/task.cpp` and
`kernel/kernel.cpp`.

## Layout (32-bit, 256 MB guest RAM)

| Area | Address / size | Notes |
| --- | --- | --- |
| Kernel heap | 0x300000–0x500000 (2 MB) + ≈1 MB gap | two-region coalescing malloc; `realloc` merges only physically adjacent blocks |
| Per-task MRP arena | 0x500000–0x2600000 (33 MB) + 1 MB stack | reserved via `PTE_DEMAND`; sized per program by `mrp_arena_hint_for()` (8 MB default, 20 MB max) |
| Module staging | 0x2800000, 12 MB | GRUB modules (mtcc.mrp, sources, WAD) — zero-copy |
| GUI / EquiX arena | 0x3400000 … ram_top | dynamic; ThorVG caches, desktop assets |
| User page pool | bitmap-allocated 4 KB pages | backs demand paging |
| RAM top | from the GRUB memory map | page-table count = `ceil(ram_top/4 MB)` clamped 16–32 |

The region above `ram_top` is **deliberately unmapped** — wild access
faults loudly instead of corrupting silently (design invariant #5).

## Demand paging

The per-task arena is not committed up front:

1. Loader reserves the address range with `PTE_DEMAND` entries
   (`task_user_map_demand`).
2. First touch raises `#PF` → `isr_14` → `task_demand_fault` →
   allocate one 4 KB page from the user pool, zero-fill, map it, resume.
3. On task exit the whole window is torn down
   (`task_user_unmap`, `task_demand_reset`).

Consequences:

- A fresh program touching a few pages costs a few pages, not 8 MB.
- `fstest`-style large writes exercise exactly this path.
- ELF images reuse the same mechanism for their
  `ELF_HEAP_VMA` window (`task_demand_reserve`/`task_demand_fill`,
  `.bss` demand-zeroed).

## Kernel heap

Two contiguous regions are managed by a coalescing free-list malloc:
the low dynamic region and a second arena after the 1 MB guard gap.
Freeing merges adjacent free blocks; `realloc` may only merge
physically adjacent ones (a deliberate simplification). A fixed
**8 MB disk cache** for FAT32 lives outside the heap arenas.

## DMA buffers

AHCI and E1000 descriptor rings/buffers sit in **identity-mapped BSS**
so drivers can hand physical addresses straight to the hardware. Keep
it that way, or add bounce buffers consciously (design invariant #6).

## Ring-3 pointer validation

Every user pointer passed to a syscall passes through the uaccess
check; a bad pointer returns `SYS_EFAULT` rather than dereferencing
kernel memory.

## Sizing guidance

- 256 MB guest RAM (`-m 256`) is the recommended floor for a dev loop
  (`mtcc`, a couple of tasks, the desktop).
- 64 MB boots but is tight: the demand window is the same virtual size,
  so large compilations may exhaust the user page pool.
