#include "header/malloc.h"
#include "header/stdio.h"   // printf for malloc_stats
#include "header/task.h"   // Phase A: per-task MRP arena
#include <stdint.h>
#include <stddef.h>

// ============================================================================
//  Equinox OS Heap Allocator v2 -- free-list with split + coalescing
// ----------------------------------------------------------------------------
//  The old version was a bump allocator (heap_top += size) with a no-op
//  free(). Problem: once the heap ran out there was no way back even if
//  every allocation had been logically "freed". For an .mrp loader that
//  loads/unloads programs repeatedly, that means quick OOM.
//
//  New design: every heap block carries a small header and lives in a
//  singly-linked list ordered by physical address. free() marks the
//  block free and coalesces with its physical neighbors (prev/next)
//  when they are free too -> prevents external fragmentation from
//  piling up over time.
//
//  There are TWO separate arenas:
//    - Kernel heap   : 0x300000 - 0x500000 (2 MB)  -> the normal malloc()
//    - MRP program   : 0x500000 - 0x2600000 (33 MB) -> .mrp loader only
//                       (v10.9 "DOOM layout": doomgeneric needs a ~16-24 MB
//                       zone; it used to be 4 MB). So user programs can
//                       neither corrupt nor exhaust the kernel heap - or vice versa.
//
//  QEMU runs with -m 64 (64 MB), so this region is clear of the kernel
//  .bss (~0x100000+) and the stack (in .bss). GRUB module staging moved
//  to 0x2800000 (start.asm) - OUTSIDE these arenas.
//
//  v10.12 FIX (Fase C bug hunt): BSS kernel tumbuh (pool lwIP + httpd)
//  past the old static 0x300000 -> arena_init() overwrote the .bss TAIL
//  (lwIP globals like next_timeout/dhcp_pcb turned to garbage -> page fault
//  in sys_timeout_abs). The heap start is now DYNAMIC: 64 KB above _bss_end
//  (linker symbol), page-aligned, minimal 0x300000.
// ============================================================================

#define KERNEL_HEAP_START 0x300000u
#define KERNEL_HEAP_MAX   0x500000u

/* v0.3 TOOLS RELEASE (29 eqbuild tools): the main 2 MB heap is not
 * enough for the RAMFS at build time — eqbuild writes ~34 KB .mrp
 * products while the boot modules + sources already fill ~1.2 MB,
 * and the freed .c blocks are too fragmented for contiguous 34 KB
 * allocations (OOM at tool #13). FIX: extend the kernel heap with
 * the FREE physical gap between USER_STACK_END (0x2702000) and the
 * GRUB module staging area (0x2800000) — ~1 MB, identity-mapped
 * supervisor pages (paging.cpp maps all of low 64 MB), claimed by
 * nothing else (see usermode.h). The kernel arena becomes TWO
 * physical regions on ONE address-ordered free list; coalesce()
 * and realloc() only merge PHYSICALLY ADJACENT blocks so the two
 * regions can never be fused into a phantom block spanning the
 * gap. */
#define KERNEL_HEAP_EXT_START 0x2702000u
#define KERNEL_HEAP_EXT_END   0x2800000u


#define MRP_HEAP_START    0x500000u
#define MRP_HEAP_MAX      0x2600000u   /* == USER_ARENA_END (usermode.h) */

/* linker symbol: end of .bss (linker.ld) */
extern "C" uint8_t _bss_end[];

/* Effective heap start: 64 KB above _bss_end, page-aligned. */
static uint32_t kernel_heap_start(void) {
    uint32_t s = ((uint32_t)(uintptr_t)_bss_end + 0x10000u + 0xFFFu) & ~0xFFFu;
    if (s < KERNEL_HEAP_START) s = KERNEL_HEAP_START;
    return s;
}

#define BLOCK_MAGIC       0x4D42u   // "MB" - canary for heap corruption detection
#define MIN_SPLIT_PAYLOAD 16u       // don't split when the remainder < this

struct block_header {
    uint32_t        magic;
    size_t          size;    // payload size (NOT counting this header)
    uint8_t         free;
    block_header*   next;    // next physical block (address-ordered)
};

struct heap_arena {
    uintptr_t       start;
    uintptr_t       max;
    block_header*   head;
    uint8_t         inited;
};

static heap_arena kernel_arena = { 0, KERNEL_HEAP_EXT_END, nullptr, 0 };

/* Phase A — the MRP arena is now PER-TASK (field Task::mrp_arena).
 * The task_mrp_arena layout (task.h) MUST be identical to heap_arena
 * — guarded by the static_assert below. Before tasks exist (boot),
 * a static fallback is used (the legacy behavior).
 * Note: a task's arena VMA = the 0x500000+ window mapped onto its
 * physical chunk (task.cpp) — the first allocation is still
 * 0x500010, exactly matching the .mrp link convention. */
static heap_arena mrp_fallback = { MRP_HEAP_START, MRP_HEAP_MAX, nullptr, 0 };

static_assert(sizeof(heap_arena) == sizeof(struct task_mrp_arena),
              "task_mrp_arena layout != heap_arena (see task.h)");

static inline heap_arena* mrp_a(void) {
    struct Task* t = task_current();
    if (t) return (heap_arena*)&t->mrp_arena;
    return &mrp_fallback;
}

static inline size_t align_up(size_t size) {
    return (size + 7u) & ~((size_t)7u);
}

// FIX(M2): save the IF status (EFLAGS bit 9) BEFORE cli, then restore it at
// the end of the critical section. Before: cli ... sti unconditionally ->
// when called from inside an IRQ handler (IF already 0), the sti() at the
// end enabled interrupts mid-handler -> IRQ races/data corruption.
static inline uint32_t irq_save(void) {
    uint32_t flags;
    asm volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint32_t flags) {
    asm volatile("pushl %0\n\tpopfl" :: "r"(flags) : "memory");
}

// FIX(M1): allocation sizes must be sane -- not 0 and not larger than
// the arena itself (also automatically rejects align_up overflow,
// e.g. malloc((size_t)-1), which used to wrap to 0).
static inline int size_sane(heap_arena* a, size_t size) {
    return size != 0 && size <= (size_t)(a->max - a->start);
}

static inline void arena_init(heap_arena* a) {
    if (a->inited) return;
    if (a->start == 0) a->start = kernel_heap_start();   /* dynamic kernel arena */
    block_header* first = (block_header*)a->start;
    first->magic = BLOCK_MAGIC;
    first->size  = (a->max - a->start) - sizeof(block_header);
    first->free  = 1;
    first->next  = nullptr;
    a->head = first;
    a->inited = 1;

    /* v0.3 TOOLS: the KERNEL arena spans TWO physical regions (main
     * 2 MB below 0x500000 + the ~1 MB gap above the user stack, see
     * KERNEL_HEAP_EXT_*). Trim the first block to the main region and
     * chain a second free block for the extension on the same
     * address-ordered free list. MRP/task arenas stay single-region. */
    if (a == &kernel_arena) {
        first->size = (KERNEL_HEAP_MAX - a->start) - sizeof(block_header);
        block_header* second = (block_header*)KERNEL_HEAP_EXT_START;
        second->magic = BLOCK_MAGIC;
        second->size  = (KERNEL_HEAP_EXT_END - KERNEL_HEAP_EXT_START)
                        - sizeof(block_header);
        second->free  = 1;
        second->next  = nullptr;
        first->next   = second;
    }
}

static block_header* arena_find_fit(heap_arena* a, size_t size) {
    block_header* cur = a->head;
    while (cur) {
        if (cur->free && cur->size >= size) return cur;
        cur = cur->next;
    }
    return nullptr;
}

static void arena_split(block_header* blk, size_t size) {
    size_t remaining = blk->size - size;
    if (remaining < sizeof(block_header) + MIN_SPLIT_PAYLOAD) {
        return;
    }

    uintptr_t new_addr = (uintptr_t)blk + sizeof(block_header) + size;
    block_header* new_blk = (block_header*)new_addr;
    new_blk->magic = BLOCK_MAGIC;
    new_blk->size  = remaining - sizeof(block_header);
    new_blk->free  = 1;
    new_blk->next  = blk->next;

    blk->size = size;
    blk->next = new_blk;
}

static void arena_coalesce(heap_arena* a, block_header* blk) {
    /* v0.3 TOOLS: merge only PHYSICALLY ADJACENT neighbors — the
     * kernel arena is two regions; list-adjacent blocks across the
     * 0x500000..0x2702000 gap must NOT be fused (a phantom block
     * spanning the gap would hand out memory owned by the user
     * pool/stacks). For single-region arenas nothing changes: their
     * list-adjacent free blocks are always physically adjacent. */
    while (blk->next && blk->next->free &&
           (uintptr_t)blk + sizeof(block_header) + blk->size
               == (uintptr_t)blk->next) {
        block_header* nxt = blk->next;
        blk->size += sizeof(block_header) + nxt->size;
        blk->next = nxt->next;
    }

    block_header* cur = a->head;
    while (cur && cur->next != blk) cur = cur->next;
    if (cur && cur->free &&
        (uintptr_t)cur + sizeof(block_header) + cur->size
            == (uintptr_t)blk) {
        cur->size += sizeof(block_header) + blk->size;
        cur->next = blk->next;
        while (cur->next && cur->next->free &&
               (uintptr_t)cur + sizeof(block_header) + cur->size
                   == (uintptr_t)cur->next) {
            block_header* nxt = cur->next;
            cur->size += sizeof(block_header) + nxt->size;
            cur->next = nxt->next;
        }
    }
}

static void* arena_alloc(heap_arena* a, size_t size) {
        // FIX(M1): malloc(0xFFFFFFFF) used to pass the size==0 check; align_up wrapped
        // to 0, yielded a 0-byte block, the caller assumed a big buffer -> corruption.
    if (!size_sane(a, size)) return nullptr;
    arena_init(a);

    size_t aligned = align_up(size);
    if (aligned < size) return nullptr; // guard overflow tambahan
    block_header* blk = arena_find_fit(a, aligned);
    if (!blk) return nullptr;

    arena_split(blk, aligned);
    blk->free = 0;

    return (void*)((uintptr_t)blk + sizeof(block_header));
}

static void arena_free(heap_arena* a, void* ptr) {
    if (!ptr) return;
    if ((uintptr_t)ptr < a->start || (uintptr_t)ptr >= a->max) return;

    block_header* blk = (block_header*)((uintptr_t)ptr - sizeof(block_header));

    if (blk->magic != BLOCK_MAGIC) {
        printf("[malloc] PANIC: heap corruption detected at free(0x%x)\n", (uint32_t)(uintptr_t)ptr);
        return;
    }
    if (blk->free) {
        printf("[malloc] WARNING: double-free detected at 0x%x, ignored\n", (uint32_t)(uintptr_t)ptr);
        return;
    }

    blk->free = 1;
    arena_coalesce(a, blk);
}

static size_t arena_used(heap_arena* a) {
    if (!a->inited) return 0;
    size_t used = 0;
    block_header* cur = a->head;
    while (cur) {
        if (!cur->free) used += cur->size + sizeof(block_header);
        cur = cur->next;
    }
    return used;
}

static uint32_t arena_free_block_count(heap_arena* a) {
    if (!a->inited) return 0;
    uint32_t count = 0;
    block_header* cur = a->head;
    while (cur) {
        if (cur->free) count++;
        cur = cur->next;
    }
    return count;
}

static size_t arena_largest_free(heap_arena* a) {
    if (!a->inited) return 0;
    size_t largest = 0;
    block_header* cur = a->head;
    while (cur) {
        if (cur->free && cur->size > largest) largest = cur->size;
        cur = cur->next;
    }
    return largest;
}

static int arena_check_integrity(heap_arena* a) {
    if (!a->inited) return 1;
    block_header* cur = a->head;
    while (cur) {
        if (cur->magic != BLOCK_MAGIC) return 0;
        cur = cur->next;
    }
    return 1;
}

// ==================== Kernel heap public API ====================

extern "C" void* malloc(size_t size) {
    uint32_t f = irq_save();
    void* p = arena_alloc(&kernel_arena, size);
    irq_restore(f);
    return p;
}

extern "C" void* calloc(size_t num, size_t size) {
    if (num != 0 && size > ((size_t)-1) / num) return nullptr;
    size_t total = num * size;
    void* ptr = malloc(total);
    if (ptr) {
        uint8_t* p = (uint8_t*)ptr;
        for (size_t i = 0; i < total; i++)
            p[i] = 0;
    }
    return ptr;
}

extern "C" void* realloc(void* ptr, size_t new_size) {
    if (ptr == nullptr) return malloc(new_size);
    if (new_size == 0) {
        free(ptr);
        return nullptr;
    }

        // FIX(M3): a pointer outside the kernel heap range is invalid. The old code
        // read blk->magic straight from a wild address -> wild read / kernel page fault.
    // v0.3 TOOLS: the kernel heap is TWO regions (main + extension).
    if (!((uintptr_t)ptr >= (uintptr_t)KERNEL_HEAP_START &&
          (uintptr_t)ptr <  (uintptr_t)KERNEL_HEAP_MAX) &&
        !((uintptr_t)ptr >= (uintptr_t)KERNEL_HEAP_EXT_START &&
          (uintptr_t)ptr <  (uintptr_t)KERNEL_HEAP_EXT_END)) {
        printf("[malloc] PANIC: realloc() on a pointer outside the heap 0x%x\n", (uint32_t)(uintptr_t)ptr);
        return nullptr;
    }

    // FIX(M1): reject insane sizes / align_up overflow.
    if (!size_sane(&kernel_arena, new_size)) return nullptr;

    uint32_t f = irq_save();
    block_header* blk = (block_header*)((uintptr_t)ptr - sizeof(block_header));
    if (blk->magic != BLOCK_MAGIC) {
        irq_restore(f);
        printf("[malloc] PANIC: realloc() on an invalid pointer 0x%x\n", (uint32_t)(uintptr_t)ptr);
        return nullptr;
    }

    size_t aligned = align_up(new_size);

    if (aligned <= blk->size) {
        arena_split(blk, aligned);
        irq_restore(f);
        return ptr;
    }

    if (blk->next && blk->next->free &&
        /* v0.3 TOOLS: adjacency guard — never fuse blocks across the
         * two-region gap (see arena_coalesce). */
        (uintptr_t)blk + sizeof(block_header) + blk->size
            == (uintptr_t)blk->next &&
        blk->size + sizeof(block_header) + blk->next->size >= aligned) {
        block_header* nxt = blk->next;
        blk->size += sizeof(block_header) + nxt->size;
        blk->next = nxt->next;
        arena_split(blk, aligned);
        irq_restore(f);
        return ptr;
    }
    irq_restore(f);

    void* new_ptr = malloc(new_size);
    if (!new_ptr) return nullptr;

    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    size_t copy_size = blk->size < new_size ? blk->size : new_size;
    for (size_t i = 0; i < copy_size; i++) dst[i] = src[i];

    free(ptr);
    return new_ptr;
}

extern "C" void free(void* ptr) {
    uint32_t f = irq_save();
    arena_free(&kernel_arena, ptr);
    irq_restore(f);
}

// ==================== Debug ====================
extern "C" void malloc_stats(void) {
    size_t used  = arena_used(&kernel_arena);
    size_t total = (KERNEL_HEAP_MAX - (kernel_arena.inited ? (size_t)kernel_arena.start
                                                           : (size_t)KERNEL_HEAP_START))
                   + (KERNEL_HEAP_EXT_END - KERNEL_HEAP_EXT_START);
    uint32_t pct = total ? (uint32_t)((used * 100) / total) : 0;
    printf("Heap: %u / %u bytes used (%u%%)\n", (uint32_t)used, (uint32_t)total, pct);
    printf("Free blocks: %u, largest free: %u bytes\n",
           arena_free_block_count(&kernel_arena), (uint32_t)arena_largest_free(&kernel_arena));
    if (!arena_check_integrity(&kernel_arena)) {
        printf("[malloc] WARNING: heap integrity check FAILED!\n");
    }
}

extern "C" uint32_t get_heap_used(void) {
    return (uint32_t)arena_used(&kernel_arena);
}

extern "C" uint32_t get_heap_total(void) {
    /* v0.3 TOOLS: main region + the extension region. */
    uint32_t main_sz = KERNEL_HEAP_MAX - (kernel_arena.inited
                        ? (uint32_t)kernel_arena.start
                        : (uint32_t)KERNEL_HEAP_START);
    return main_sz + (KERNEL_HEAP_EXT_END - KERNEL_HEAP_EXT_START);
}

extern "C" uint32_t get_heap_free_blocks(void) {
    return arena_free_block_count(&kernel_arena);
}

extern "C" uint32_t get_heap_largest_free(void) {
    return (uint32_t)arena_largest_free(&kernel_arena);
}

extern "C" int heap_check_integrity(void) {
    return arena_check_integrity(&kernel_arena);
}

// ==================== MRP program heap (used by the .mrp loader) ====================

extern "C" void mrp_heap_init(void) {
    heap_arena* a = mrp_a();
    a->inited = 0;
    a->head = nullptr;
    arena_init(a);
}

extern "C" void* mrp_alloc(size_t size) {
    uint32_t f = irq_save();
    void* p = arena_alloc(mrp_a(), size);
    irq_restore(f);
    return p;
}

/* v0.3 FR-03: free a single MRP-arena block. The arena has been a
 * split+coalesce free-list since v2 — only the syscall wrapper was
 * missing, so SYS_MALLOC could never be paired with a free(). Range
 * + magic checks live in arena_free (wild/double frees are reported
 * and ignored there, not fatal). */
extern "C" int mrp_free(void* ptr) {
    if (!ptr) return 0;
    uint32_t f = irq_save();
    heap_arena* a = mrp_a();
    if ((uintptr_t)ptr < a->start || (uintptr_t)ptr >= a->max) {
        irq_restore(f);
        return -1;                  /* not an arena pointer */
    }
    arena_free(a, ptr);
    irq_restore(f);
    return 0;
}

extern "C" void mrp_free_all(void) {
    uint32_t f = irq_save();
    heap_arena* a = mrp_a();
    a->inited = 0;
    a->head = nullptr;
    arena_init(a);
    irq_restore(f);
}

extern "C" uint32_t get_mrp_heap_used(void) {
    return (uint32_t)arena_used(mrp_a());
}

extern "C" uint32_t get_mrp_heap_total(void) {
    heap_arena* a = mrp_a();
    return (uint32_t)(a->inited || a->max ? (a->max - a->start) : 0);
}
