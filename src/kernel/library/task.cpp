#include "header/task.h"
#include "header/stdio.h"
#include "header/serial.h"
#include "header/paging.h"
#include "header/usermode.h"
#include "header/malloc.h"
#include "header/timer.h"
#include "header/syscall.h"
#include "header/libstring.h"
#include "header/mrp_format.h"   /* Phase B: .mrp header validation for spawn */
#include "header/elf.h"         /* v0.3 (FR-07): ELF32 loader */
#include "header/fs_ram.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================
 *  task.cpp — Round-robin scheduler + virtual consoles + per-task
 *  user arenas (Equinox OS multitasking, Phases A/B/C). See task.h.
 * ============================================================ */

#define TASK_QUANTUM      1          /* tick (10 ms) per task */
#define USER_STACK_BYTES  0x100000   /* 1 MB user stack per task (as before) */

/* ==================== static storage ==================== */
struct Task tasks[MAX_TASKS] __attribute__((aligned(16)));

/* Per-task page directory (PDE 0, 10-15 and the LFB share the kernel
 * template; PDE 1-9 are task-private). */
static uint32_t task_pds[MAX_TASKS][1024] __attribute__((aligned(4096)));
static uint32_t task_pts[MAX_TASKS][9][1024] __attribute__((aligned(4096)));

static struct task_mrp_arena fallback_arena;

/* ==================== global state ==================== */
static Task* g_cur_task = NULL;
static int   g_next_pid = 1;
static int   g_sched_ready = 0;
static int   g_sched_lock_depth = 0;

int g_req_new_console = 0;
int g_req_prev_console = 0;

extern "C" void shell_entry(void* arg);     /* shell.cpp */
extern "C" void task_first_entry(void* arg); /* debug shim (below) */

/* ==================== asm core ==================== */

asm(
    ".global __task_switch\n"
    ".text\n"
    "__task_switch:\n"
    "    pushl %ebp\n"
    "    pushl %edi\n"
    "    pushl %esi\n"
    "    pushl %ebx\n"
    /* [esp]=ebp [4]=edi [8]=esi [12]=ebx [16]=ret [20]=arg1 [24]=arg2 */
    "    movl 20(%esp), %eax\n"         /* eax = Task* old */
    "    fxsave 16(%eax)\n"             /* save old FPU/SSE state */
    "    movl %esp, 0(%eax)\n"          /* old->ksp = current esp */
    "    movl 24(%esp), %ebx\n"         /* ebx = Task* new */
    "    fxrstor 16(%ebx)\n"            /* restore new FPU/SSE state */
    "    movl 0(%ebx), %esp\n"          /* === STACK SWITCH === */
    "    popl %ebx\n"
    "    popl %esi\n"
    "    popl %edi\n"
    "    popl %ebp\n"
    "    ret\n"
);

asm(
    ".global task_entry_trampoline\n"
    ".text\n"
    /* entered with ebx=fn, esi=arg (from the crafted initial stack) */
    "task_entry_trampoline:\n"
    "    sti\n"                          /* new task: interrupts on */
    "    subl $12, %esp\n"
    "    pushl %esi\n"                   /* arg (cdecl) */
    "    call *%ebx\n"
    "    addl $16, %esp\n"
    "    call task_exit_final\n"         /* fn finished -> kill the task */
);

extern "C" void __task_switch(struct Task* old, struct Task* newt);
extern "C" void task_entry_trampoline(void);

/* Offsets used by the asm — guarded by static_asserts. */
static_assert(__builtin_offsetof(struct Task, ksp) == 0,
              "asm: ksp must be at offset 0");
static_assert(__builtin_offsetof(struct Task, fpu_state) == 16,
              "asm: fpu_state must be at offset 16");
static_assert(sizeof(struct task_mrp_arena) == 16,
              "per-task arena layout changed — check the malloc.cpp cast");

/* ==================== helper ==================== */
static inline uint32_t irq_save(void) {
    uint32_t flags;
    asm volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint32_t flags) {
    asm volatile("pushl %0\n\tpopfl" :: "r"(flags) : "memory");
}

static inline int task_slot(const Task* t) { return (int)(t - tasks); }

/* v0.3 FR-08: count how many fd-table entries across ALL tasks
 * reference `node` — the caller passes the fd of the CURRENT task it
 * is about to release (skipped); -1 = count everything. Used by
 * sys_close() to decide whether dropping the FAT content cache is
 * safe (no other reader/writer holds the node). */
int task_fd_count_node(struct fs_node* node, int skip_fd) {
    if (!node) return 0;
    struct Task* cur = task_current();
    int cnt = 0;
    uint32_t f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (!t->used) continue;
        for (int fd = 3; fd < TASK_MAX_FDS; fd++) {
            if (t == cur && fd == skip_fd) continue;
            if (t->fds[fd].node == node) cnt++;
        }
    }
    irq_restore(f);
    return cnt;
}

static void task_fpu_init_state(Task* t) {
    /* a valid default state -> the first fxrstor on switch is always safe */
    asm volatile("fninit");
    asm volatile("fxsave %0" : "=m"(t->fpu_state[0]) :: "memory");
}

/* ==================== user physical pool ==================== */
#define POOL_START   0x500000u
#define POOL_END     0x2600000u
#define POOL_PAGES   ((POOL_END - POOL_START) / 0x1000)
static uint32_t pool_bitmap[(POOL_PAGES + 31) / 32];

uint32_t task_user_phys_alloc(uint32_t bytes) {
    if (bytes == 0) return 0;
    uint32_t pages = (bytes + 0xFFF) / 0x1000;
    if (pages == 0 || pages > POOL_PAGES) return 0;

    uint32_t f = irq_save();
    uint32_t run = 0;
    for (uint32_t i = 0; i < POOL_PAGES; i++) {
        if (pool_bitmap[i >> 5] & (1u << (i & 31))) {
            run = 0;
        } else {
            run++;
            if (run >= pages) {
                uint32_t first = i - pages + 1;
                for (uint32_t k = first; k <= i; k++)
                    pool_bitmap[k >> 5] |= (1u << (k & 31));
                irq_restore(f);
                return POOL_START + first * 0x1000;
            }
        }
    }
    irq_restore(f);
    return 0;
}

void task_user_phys_free(uint32_t phys, uint32_t bytes) {
    if (phys < POOL_START || phys >= POOL_END || bytes == 0) return;
    uint32_t first = (phys - POOL_START) / 0x1000;
    uint32_t pages = (bytes + 0xFFF) / 0x1000;
    if (first + pages > POOL_PAGES) return;
    uint32_t f = irq_save();
    for (uint32_t k = first; k < first + pages; k++)
        pool_bitmap[k >> 5] &= ~(1u << (k & 31));
    irq_restore(f);
}

/* ==================== page-table per task ==================== */

#define PTE_PRESENT 0x001u
#define PTE_RW      0x002u
#define PTE_USER    0x004u
/* PTE_DEMAND (bit 9, OS-available) comes from task.h: a non-present
 * PTE carrying it = a RESERVED user page, faulted in on first touch. */

static void task_build_dir(Task* t) {
    uint32_t slot = (uint32_t)task_slot(t);
    uint32_t* tmpl = paging_kernel_dir();

    /* SPONTANEOUS-REBOOT BUG FIX: build the whole directory
     * non-preemptibly. The new page_dir is only published at the END
     * of the build — during the loop page_dir == NULL and the kernel
     * PDEs are partial. If IRQ0 preempts in the middle and the task is
     * then resumed, task_post_switch sees page_dir == NULL ->
     * task_load_cr3(NULL) -> CR3 = 0 -> the page walk reads physical
     * page 0 (the IVT) -> triple fault -> spontaneous reboot.
     * irq_save closes this window. */
    uint32_t irqf = irq_save();

    for (int i = 0; i < 1024; i++) task_pds[slot][i] = 0;

    /* Kernel PDEs: share the template, supervisor (strip U/S). */
    for (int pde = 0; pde < 1024; pde++) {
        if (pde >= 1 && pde <= 9) continue;          /* task-private */
        uint32_t e = tmpl[pde];
        if (e & PTE_PRESENT) task_pds[slot][pde] = e & ~PTE_USER;
    }

    /* PDE 1..9 -> task-private page tables, identity + supervisor. */
    for (int pde = 1; pde <= 9; pde++) {
        task_pds[slot][pde] = ((uint32_t)(uintptr_t)task_pts[slot][pde - 1])
                              | PTE_PRESENT | PTE_RW | PTE_USER;
        for (int j = 0; j < 1024; j++) {
            uint32_t vma = ((uint32_t)pde << 22) | ((uint32_t)j << 12);
            uint32_t pte = vma | PTE_PRESENT | PTE_RW;   /* supervisor */
            if (vma == USER_TRAMPOLINE) pte |= PTE_USER; /* exit stub */
            task_pts[slot][pde - 1][j] = pte;
        }
    }
    t->page_dir = task_pds[slot];
    irq_restore(irqf);
}

static inline uint32_t* task_pte_slot(Task* t, uint32_t vma) {
    if (!t || t->page_dir != task_pds[task_slot(t)]) return NULL;
    int pde = (int)(vma >> 22);
    if (pde < 1 || pde > 9) return NULL;
    return &task_pts[task_slot(t)][pde - 1][(vma >> 12) & 0x3FF];
}

/* ============================================================
 *  v0.3 (FR-06): DEMAND PAGING
 * ------------------------------------------------------------
 *  The old model allocated one CONTIGUOUS physical chunk up front
 *  (arena bytes + 1 MB stack) — a 24 MB DOOM reservation starved
 *  every other task even though DOOM only touches a fraction of it
 *  early on. The new model RESERVES the VMA window with non-present
 *  PTEs (PTE_DEMAND|RW|USER) and hands out one zero-filled physical
 *  page per first touch:
 *
 *    ring 3 access  -> #PF -> isr_14 -> task_demand_fault() -> resume
 *    loader copies  -> task_demand_fill() (explicit per-page map,
 *                      CR3 switched, IRQ-safe — the #PF path must
 *                      never run against ANOTHER task's directory)
 *
 *  Physical pool (33 MB) is only consumed by pages actually touched.
 *  The 4 KB zero-fill gives fresh .bss pages read-as-zero semantics.
 *  The guard page (supervisor, present) and every other supervisor
 *  mapping keep their kill-on-user-access behavior — demand paging
 *  only softens NON-present, DEMAND-marked user pages.
 * ============================================================ */

/* Mark [vma, vma+len) as reserved-demand in the task's own tables.
 * NOTE: build_dir leaves every PDE 1-9 PTE as PRESENT-supervisor
 * identity — the reservation OVERWRITES that default (the only
 * present-user page in the range set, the trampoline at 0x2600000,
 * is never inside a reserved range). A stale TLB entry for an
 * overwritten VMA is flushed per page (the loaders switch CR3
 * around the copy anyway, which flushes everything). */
void task_demand_reserve(struct Task* t, uint32_t vma, uint32_t len) {
    if (!t) return;
    uint32_t f = irq_save();
    uint32_t lo = vma & ~0xFFFu;
    uint32_t hi = (vma + len + 0xFFFu) & ~0xFFFu;
    for (uint32_t a = lo; a < hi; a += 0x1000) {
        uint32_t* pte = task_pte_slot(t, a);
        if (!pte) continue;                    /* outside PDE 1-9: skip */
        *pte = a | PTE_RW | PTE_USER | PTE_DEMAND;   /* reserved (P=0) */
        asm volatile("invlpg (%0)" :: "r"(a) : "memory");
    }
    irq_restore(f);
}

/* The #PF hook (isr_14). Returns 1 when the fault was a demand
 * reservation and a zero page has been mapped (resume the faulting
 * instruction); 0 = not ours (guard page / unmapped hole / foreign
 * context -> the normal kill-or-panic path).
 *
 * ZERO-FILL DISCIPLINE: the fresh page is zeroed through the
 * FAULTING VMA (never through the phys identity!) — the PTE is
 * installed FIRST, then the zero loop runs on a present mapping.
 * Zeroing via the identity would #PF again whenever the freshly
 * allocated physical page happens to lie inside the task's own
 * demand window (P=0 there) — a nested-fault loop that eats the
 * interrupt stack. */
int task_demand_fault(uint32_t cr2, uint32_t err_code) {
    (void)err_code;                       /* P=0 is implied by DEMAND */
    struct Task* t = g_cur_task;
    if (!t) return 0;

    uint32_t page = cr2 & ~0xFFFu;
    uint32_t* pte = task_pte_slot(t, page);
    if (!pte) return 0;
    if (*pte & PTE_PRESENT) return 0;     /* present (guard page, ...) */
    if (!(*pte & PTE_DEMAND)) return 0;   /* unmapped hole, not reserved */

    uint32_t phys = task_user_phys_alloc(0x1000);
    if (!phys) return 0;                  /* pool exhausted -> kill path */

    /* install the mapping FIRST, then zero via the faulting VMA */
    *pte = phys | PTE_PRESENT | PTE_RW | PTE_USER;
    t->pages_user++;
    asm volatile("invlpg (%0)" :: "r"(page) : "memory");

    uint32_t* z = (uint32_t*)(uintptr_t)page;   /* present now */
    for (uint32_t i = 0; i < 0x400; i++) z[i] = 0;
    return 1;
}

/* Loader-context copy into the demand window. TWO PHASES, because
 * the fresh pages must be ZEROED before the task's mapping goes
 * live, but the identity mapping of a pool address is only reliable
 * under the KERNEL directory (under the task's directory that VMA
 * may itself be a reserved demand page — zeroing through it would
 * re-fault, cf. task_demand_fault):
 *
 *   PHASE 1 (kernel CR3): walk the range, map + zero every page via
 *            the identity (kernel dir maps 0-64 MB supervisor).
 *   PHASE 2 (task CR3):   copy the bytes through the VMA window —
 *            every page is present by then. IRQs stay off across
 *            both phases (a preempt with a foreign CR3 loaded is
 *            not survivable). */
void task_demand_fill(struct Task* t, uint32_t vma,
                      const uint8_t* src, uint32_t len) {
    if (!t || !src || !len) return;
    uint32_t f = irq_save();
    uint32_t old_cr3 = task_read_cr3();

    /* ---- PHASE 1: map + zero (kernel directory) ---- */
    task_load_cr3(paging_kernel_dir());
    {
        uint32_t lo = vma & ~0xFFFu;
        uint32_t hi = (vma + len + 0xFFFu) & ~0xFFFu;
        for (uint32_t a = lo; a < hi; a += 0x1000) {
            uint32_t* pte = task_pte_slot(t, a);
            if (!pte) continue;
            if (*pte & PTE_PRESENT) continue;
            uint32_t phys = task_user_phys_alloc(0x1000);
            if (!phys) continue;
            uint32_t* z = (uint32_t*)(uintptr_t)phys;   /* identity: OK
                                                          * under the
                                                          * KERNEL dir */
            for (uint32_t i = 0; i < 0x400; i++) z[i] = 0;
            *pte = phys | PTE_PRESENT | PTE_RW | PTE_USER;
            t->pages_user++;
        }
    }

    /* ---- PHASE 2: copy the bytes (task directory) ---- */
    task_load_cr3(t->page_dir);
    {
        uint32_t off = 0;
        while (off < len) {
            uint32_t page = (vma + off) & ~0xFFFu;
            uint32_t* pte = task_pte_slot(t, page);
            uint32_t in_off = (vma + off) & 0xFFFu;
            uint32_t chunk = 0x1000 - in_off;
            if (chunk > len - off) chunk = len - off;
            if (pte && (*pte & PTE_PRESENT)) {
                uint8_t* dst = (uint8_t*)(uintptr_t)(page + in_off);
                for (uint32_t i = 0; i < chunk; i++) dst[i] = src[off + i];
            }
            off += chunk;
        }
    }

    /* ---- restore the caller's directory ---- */
    task_load_cr3((uint32_t*)(uintptr_t)old_cr3);
    irq_restore(f);
}

/* Reserve the per-task window: [arena_vma, +arena_bytes) DEMAND +
 * the 1 MB user stack DEMAND. Replaces the eager chunk mapping. */
int task_user_map_demand(struct Task* t, uint32_t arena_vma,
                         uint32_t arena_bytes) {
    if (!t || !arena_bytes) return -1;
    if (t->page_dir != task_pds[task_slot(t)]) task_build_dir(t);

    task_demand_reserve(t, arena_vma, arena_bytes);
    task_demand_reserve(t, USER_STACK_START,
                        USER_STACK_END - USER_STACK_START);

    t->mrp_arena.start  = arena_vma;
    t->mrp_arena.max    = arena_vma + arena_bytes;
    t->mrp_arena.head   = NULL;
    t->mrp_arena.inited = 0;
    t->user_phys  = 0;                  /* no upfront chunk anymore */
    t->user_size  = arena_bytes;        /* VMA reservation size       */
    t->stack_phys = 0;
    t->arena_reserve = arena_bytes;
    return 0;
}

void task_user_unmap(Task* t) {
    if (!t) return;
    if (t->page_dir != task_pds[task_slot(t)]) return;

    uint32_t f = irq_save();
    /* Walk EVERY task-private PTE (PDE 1-9): free the physical page
     * of every PRESENT user mapping back to the pool, clear DEMAND
     * reservations, and restore the identity-supervisor default.
     * The trampoline page (VMA==phys 0x2600000, outside the pool) is
     * left untouched — phys >= POOL_END is skipped. */
    for (int pde = 1; pde <= 9; pde++) {
        for (int j = 0; j < 1024; j++) {
            uint32_t* pte = &task_pts[task_slot(t)][pde - 1][j];
            uint32_t e = *pte;
            if (e & PTE_USER) {
                uint32_t phys = e & ~0xFFFu;
                if (phys >= POOL_START && phys < POOL_END)
                    task_user_phys_free(phys, 0x1000);
                uint32_t vma = ((uint32_t)pde << 22) | ((uint32_t)j << 12);
                *pte = vma | PTE_PRESENT | PTE_RW;      /* supervisor */
                asm volatile("invlpg (%0)" :: "r"(vma) : "memory");
            } else if (e & PTE_DEMAND) {
                uint32_t vma = ((uint32_t)pde << 22) | ((uint32_t)j << 12);
                *pte = vma | PTE_PRESENT | PTE_RW;      /* supervisor */
            }
        }
    }
    irq_restore(f);
    t->pages_user = 0;
    t->arena_reserve = 0;
}

void task_load_cr3(uint32_t* pd) {
    /* v0.3: every directory in the system carries a
     * present PDE 0 (kernel low identity, shared from the template).
     * A pd without it is corrupted state — loading it triple-faults
     * on the next fetch. Catch it, name the CALLER, fall back. */
    if (!(pd && (pd[0] & PTE_PRESENT))) {
        printf("[CR3] task_load_cr3(bad pd=%08x pde0=%08x) caller=%08x"
               " — loading kernel dir instead\n",
               (uint32_t)(uintptr_t)pd, pd ? pd[0] : 0,
               (uint32_t)(uintptr_t)__builtin_return_address(0));
        pd = paging_kernel_dir();
    }
    asm volatile("mov %0, %%cr3" : : "r"(pd) : "memory");
}

/* v0.3: public CR3 read (stdio.cpp canvas switch, demand fill). */
uint32_t task_read_cr3(void) {
    uint32_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* ==================== API dasar ==================== */

struct Task* task_current(void) { return g_cur_task; }
int  task_getpid(void)          { return g_cur_task ? g_cur_task->pid : 0; }
int  task_sched_active(void)    { return g_sched_ready; }
struct task_mrp_arena* task_fallback_arena(void) { return &fallback_arena; }

/* ==================== task 0 (boot) ==================== */

void task_init0(void) {
    Task* t = &tasks[0];
    memset((void*)t, 0, sizeof(Task));
    t->used  = 1;
    t->pid   = g_next_pid++;
    t->state = TASK_RUNNING;
    t->kind  = TASK_KIND_SHELL;
    strncpy(t->name, "shell0", TASK_NAME_MAX - 1);
    t->console = 0;
    t->wait_console = -1;
    t->page_dir = paging_kernel_dir();
    t->kstack_top = usermode_int_stack_top();  /* boot stack of task 0 (CPL 0) */
    t->istack_top = usermode_int_stack_top();  /* CPL3->CPL0 of task 0: user_int_stack */
    t->cwd = NULL;
    t->args[0] = 0;
    task_fpu_init_state(t);
    g_cur_task = t;
    g_sched_ready = 1;
}

/* ==================== create ==================== */

static Task* task_alloc_slot(void) {
    uint32_t f = irq_save();
    /* v0.3 (FR-02): a zombie holds its slot until wait() — if
     * the table is full, steal the OLDEST zombie (its parent lost
     * the right to collect the status, reported via `ps`). */
    int full = 1;
    for (int i = 0; i < MAX_TASKS; i++) if (!tasks[i].used) { full = 0; break; }
    if (full) {
        int victim = -1;
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].used && tasks[i].zombie) { victim = i; break; }
        if (victim >= 0) {
            tasks[victim].used  = 0;
            tasks[victim].state = TASK_FREE;
            tasks[victim].zombie = 0;
        }
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].used) {
            Task* t = &tasks[i];
            memset((void*)t, 0, sizeof(Task));
            t->used = 1;
            t->wait_console = -1;   /* 0 is a VALID console id */
            irq_restore(f);
            return t;
        }
    }
    irq_restore(f);
    return NULL;
}

/* Craft the initial stack — following the __task_switch discipline:
 *   pops: ebx, esi, edi, ebp, then ret.
 *   [ksp+0]=ebx=fn  [ksp+4]=esi=arg  [ksp+8]=edi=0  [ksp+12]=ebp=0
 *   [ksp+16]=retaddr=trampoline  (the INNERMOST slot — popped by `ret` last)
 * FIX: an early version put the retaddr at ksp+0 -> ebx=trampoline and
 * ret->0 (crash #UD). */
static void task_craft_stack(Task* t, void (*fn)(void*), void* arg) {
    uint32_t* top = (uint32_t*)(void*)t->kstack_top;
    *(--top) = (uint32_t)(uintptr_t)task_entry_trampoline;  /* ret — ksp+16 */
    *(--top) = 0;                                    /* ebp — ksp+12 */
    *(--top) = 0;                                    /* edi — ksp+8  */
    *(--top) = (uint32_t)(uintptr_t)arg;             /* esi — ksp+4  */
    *(--top) = (uint32_t)(uintptr_t)fn;              /* ebx — ksp+0  */
    t->ksp = (uint32_t)(uintptr_t)top;
}

struct Task* task_create_shell(void) {
    Task* t = task_alloc_slot();
    if (!t) return NULL;
    t->pid   = g_next_pid++;
    t->state = TASK_BLOCKED;   /* FIX: READY only AFTER it is fully set up (below) */
    t->kind  = TASK_KIND_SHELL;
    strncpy(t->name, "shell", TASK_NAME_MAX - 1);
    t->kstack_top = (uint32_t)(uintptr_t)&t->kstack[TASK_KSTACK];
    t->istack_top = (uint32_t)(uintptr_t)&t->istack[TASK_KSTACK];
    t->page_dir   = paging_kernel_dir();
    t->console    = console_create();
    if (t->console < 0) {
        t->used = 0;
        return NULL;
    }
    t->owns_console = 1;      /* this console BELONGS to this shell task */
    t->cwd = NULL;
    t->args[0] = 0;
    task_fpu_init_state(t);
    task_craft_stack(t, task_first_entry, (void*)(uintptr_t)1);
    t->state = TASK_READY;     /* FIX: only now may it be scheduled */
    return t;
}

/* ==================== v0.3 FR-09: kernel background tasks ====================
 * A console-less worker task (the "net" nettask). `fn` is entered via
 * the standard trampoline (interrupts on) and MUST call
 * task_kernel_start() first — the manual first-activation setup that
 * a shell gets for free through task_first_entry/task_post_switch
 * (TSS.ESP0, CR3). When fn returns the task exits like any other.
 * NOTE: consumes one of the MAX_TASKS slots for its whole lifetime. */
static void task_post_switch(void);   /* defined below (scheduler section) */

extern "C" void task_kernel_start(void) {
    task_post_switch();
}

struct Task* task_create_kernel(const char* name, void (*fn)(void*), void* arg) {
    Task* t = task_alloc_slot();
    if (!t) return NULL;
    t->pid   = g_next_pid++;
    t->state = TASK_BLOCKED;           /* READY only after the craft  */
    t->kind  = TASK_KIND_KERNEL;
    strncpy(t->name, name, TASK_NAME_MAX - 1);
    t->kstack_top = (uint32_t)(uintptr_t)&t->kstack[TASK_KSTACK];
    t->istack_top = (uint32_t)(uintptr_t)&t->istack[TASK_KSTACK];
    t->page_dir   = paging_kernel_dir();
    t->console    = -1;                /* console-less                 */
    t->owns_console = 0;
    t->wait_console = -1;
    t->cwd = NULL;
    t->args[0] = 0;
    /* v0.3 FR-01: start with a clean fd table (never inherit). */
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        t->fds[fd].node  = NULL;
        t->fds[fd].pos   = 0;
        t->fds[fd].mode  = 0;
        t->fds[fd].dirty = 0;
        t->fds[fd].dcur  = NULL;
    }
    task_fpu_init_state(t);
    task_craft_stack(t, fn, arg);
    t->state = TASK_READY;             /* fully set up now            */
    return t;
}

/* ==================== scheduler ==================== */

static void wake_scan(void) {
    uint32_t now = get_tick();
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (t->used && t->state == TASK_BLOCKED && t->wait_console < 0
            && now >= t->sleep_until)
            t->state = TASK_READY;
    }
}

/* v0.3: defined later (exit/reap section) — needed by the
 * killed-task zombie conversion in pick_next. */
static Task* task_find_live(int pid);
static void  task_release_resources(Task* t);
static void  task_orphan_children(int dead_pid);
void task_wake_waitpid(int child_pid);

static Task* pick_next(Task* cur) {
    int start = cur ? (task_slot(cur) + 1) % MAX_TASKS : 0;
    for (int k = 0; k < MAX_TASKS; k++) {
        Task* t = &tasks[(start + k) % MAX_TASKS];
        if (!t->used || t == cur) continue;
        if (t->state != TASK_READY) continue;
        /* GUARD: a READY task whose stack has not been crafted yet
         * (embryo) must NEVER be scheduled: ksp==0 -> switch to ESP=0
         * = crash. */
        if (!t->ksp || !t->page_dir) continue;
        if (t->killed) {
            /* v0.3 (FR-02): a killed USER task with a LIVE
             * parent becomes a ZOMBIE first (POSIX: killed children
             * stay waitable — `kill` + `wait` must pair). Everything
             * else (or an orphan) is reaped + the slot released. */
            Task* kp = task_find_live(t->parent_pid);
            if (t->kind == TASK_KIND_USER && !t->zombie && kp && kp != t) {
                t->exit_normal = 0;
                t->exit_status = 0x80000000u;   /* killed marker */
                t->zombie = 1;
                t->state  = TASK_DEAD;
                task_release_resources(t);
                task_wake_waitpid(t->pid);
                continue;
            }
            /* Phase C: a killed task (not cur) — reap + release the slot
             * HERE so round-robin never picks it again. */
            task_orphan_children(t->pid);
            task_reap(t);
            t->state = TASK_FREE;
            t->used  = 0;
            continue;
        }
        return t;
    }
    return NULL;
}

/* Called AFTER a switch — runs on the resumed task's stack.
 * May only touch GLOBALS + fields of g_cur_task. */
static void task_post_switch(void) {
    Task* t = g_cur_task;
    /* TSS.ESP0 = the task's INTERRUPT stack (CPL3->CPL0 syscall and
     * exception frames land here — SEPARATE from the main kstack where
     * the mrp_run/user_task_entry frames that longjmp targets live). */
    tss_set_esp0(t->istack_top);
    /* Defensive GUARD: NEVER load a NULL CR3 (a fresh task whose
     * directory has not been built yet) — CR3=0 = triple fault. */
    if (!t->page_dir) t->page_dir = paging_kernel_dir();
    /* v0.3: a task directory must always map the
     * kernel's low identity — PDE 0 is shared from the template by
     * every directory in the system (task_build_dir) or IS the
     * kernel template (paging_kernel_dir). A page_dir that does not
     * carry a present PDE 0 is CORRUPTED state: loading it would
     * triple-fault on the next instruction fetch. Catch it, NAME the
     * task, print the corruption pattern, and fall back to the
     * kernel directory so the system stays alive for debugging. */
    if (!(t->page_dir[0] & PTE_PRESENT)) {
        printf("[SCHED] pid %d slot %d '%s': CORRUPT page_dir=%08x "
               "PDE0=%08x istack=%08x clip=%d,%d,%d,%d,%d — kernel dir\n",
               t->pid, (int)(t - tasks), t->name,
               (uint32_t)(uintptr_t)t->page_dir, t->page_dir[0],
               t->istack_top, t->clip_on, t->clip_x, t->clip_y,
               t->clip_w, t->clip_h);
        t->page_dir = paging_kernel_dir();
    }
    if ((uint32_t)(uintptr_t)t->page_dir != task_read_cr3())
        task_load_cr3(t->page_dir);
    /* v0.3: console-less kernel tasks (nettask) keep the console that
     * the interrupted task was using — no output redirection. */
    if (t->console >= 0) term_set_output(t->console);
}

/* v0.3 fix (the "instant sleep" bug): while schedule() idles in
 * the hlt loop below, the timer ISR itself calls schedule() (quantum
 * path) and would re-enter the loop — every tick would nest another
 * idle frame on the interrupt stack and overflow it within seconds.
 * The guard makes nested calls return immediately: the OUTER loop
 * owns the scanning. */
static int g_sched_idling;

void schedule(void) {
    if (!g_sched_ready || !g_cur_task) return;
    if (g_sched_lock_depth) return;
    if (g_sched_idling) return;     /* nested call from inside the idle loop */

    uint32_t f = irq_save();

    wake_scan();
    Task* cur = g_cur_task;
    Task* next = pick_next(cur);
    if (!next) {
        if (cur->state != TASK_BLOCKED) {
            /* nothing runnable, but cur itself is runnable: keep it */
            cur->quantum = TASK_QUANTUM;
            irq_restore(f);
            return;
        }
        /* v0.3 fix: cur has blocked itself (sleep / waitpid /
         * pipe / console gate) and NO other task is runnable. The old
         * code returned to cur anyway — and the BLOCKED->RUNNING
         * fix-up in task_sleep then cancelled the sleep, so every
         * sleep() returned INSTANTLY whenever it happened to land in
         * a tick window where nettask (the usual other task) was
         * itself mid-sleep. Instead: HALT with interrupts ON until
         * an interrupt changes the picture, re-scanning after each
         * wake:
         *   - the timer tick makes wake_scan flip sleepers READY
         *   - task_wake_* (console / pipe / waitpid) flip us directly
         *   - F1 creates a new READY shell from the timer ISR
         * hlt (not a busy spin) — the nettask heartbeat no longer
         * burns 100% CPU while the shell sleeps. */
        g_sched_idling = 1;
        __asm__ volatile("sti" ::: "memory");
        for (;;) {
            wake_scan();
            next = pick_next(cur);
            if (next || cur->state != TASK_BLOCKED) break;
            __asm__ volatile("hlt" ::: "memory");   /* wait for an IRQ */
        }
        __asm__ volatile("cli" ::: "memory");
        g_sched_idling = 0;
        if (!next) {
            /* cur itself was woken (READY): resume as RUNNING */
            cur->state = TASK_RUNNING;
            cur->quantum = TASK_QUANTUM;
            irq_restore(f);
            return;
        }
        /* fall through: switch from the (still-blocked) cur to next */
    }
    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;
    next->state = TASK_RUNNING;
    next->quantum = TASK_QUANTUM;
    g_cur_task = next;

    __task_switch(cur, next);
    /* --- resume point: we are on task `next`'s stack/frame --- */
    task_post_switch();
    irq_restore(f);
}

/* Phase A->B debug shim: first entry of a shell task — a MANUAL
 * task_post_switch (first activation goes through the trampoline,
 * not through a return from schedule()). */
extern "C" void task_first_entry(void* arg) {
    task_post_switch();   /* Phase B FIX: CR3 + TSS + console for this task */
    shell_entry(arg);
}

void task_yield(void) {
    if (!g_sched_ready || !g_cur_task) return;
    schedule();
}

void task_sleep(uint32_t ms) {
    if (!g_sched_ready || !g_cur_task) {
        sleep_ms(ms);
        return;
    }
    uint32_t ticks = ms / 10;
    if (ticks == 0) ticks = 1;
    g_cur_task->sleep_until = get_tick() + ticks;
    g_cur_task->state = TASK_BLOCKED;
    schedule();
    /* v0.3: no BLOCKED->RUNNING fix-up here — that used to
     * silently CANCEL the sleep whenever no other task was READY
     * (schedule() bailed and returned to us still blocked). Now
     * schedule() idles in the hlt loop until we are due: on return
     * we are RUNNING again — either resumed via a real context
     * switch, or woken in place by the idle loop. */
}

/* ==================== console focus gating (SIGTTOU-style) ====================
 * Fix for the cross-console pixel leak: a background game used to keep
 * painting the GLOBAL framebuffer while the user worked on another
 * terminal (snake visibly crawling over shell 2). The Unix answer is
 * SIGTTOU — a background process that writes to the terminal is
 * stopped until it regains the foreground. We do the same for the
 * graphics syscalls: fillrect / putpixel / blit call this gate first;
 * an unfocused task is parked in TASK_BLOCKED (wait_console) and is
 * resumed by console_activate() -> task_wake_console() when its
 * console is focused again. The task then continues INSIDE the
 * syscall and performs the draw it was suspended on — no frames are
 * dropped, no ghosting, and the game resumes exactly where it froze. */
int task_console_draw_gate(void) {
    Task* t = g_cur_task;
    if (!g_sched_ready || !t) return 1;      /* boot context: allow */
    if (t->console < 0) return 1;            /* console-less task: allow */
    while (t->console != console_active_id()) {
        if (t->killed) return 0;             /* killed while parked: skip */
        t->wait_console = t->console;
        t->state = TASK_BLOCKED;
        t->sleep_until = 0xFFFFFFFFu;        /* never wake from the timer */
        schedule();                          /* park until refocused */
        t->wait_console = -1;
        /* schedule() may return without switching away (no other
         * runnable task): loop and re-check instead of drawing on a
         * foreign console — interrupts stay enabled, so the timer
         * can still process F1/F2 and focus our console. */
    }
    return 1;
}

/* Wake every task parked on `console_id` (called from
 * console_activate after the switch is complete). */
void task_wake_console(int console_id) {
    if (console_id < 0) return;
    uint32_t f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (t->used && t->wait_console == console_id
            && t->state == TASK_BLOCKED)
            t->state = TASK_READY;
    }
    irq_restore(f);
}

/* v0.3 (FR-02): find a live (non-zombie) task by pid. */
static Task* task_find_live(int pid) {
    if (pid <= 0) return NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (t->used && t->pid == pid) return t;
    }
    return NULL;
}

/* v0.3 (FR-02): release the RESOURCES of a task (pages, fds,
 * pipes, console) but keep the slot + exit status when it becomes a
 * zombie. Split out of task_reap so the exit path can choose
 * "zombie (status pending)" vs "full reap (slot free)". */
static void task_release_resources(Task* t) {
    if (!t || !t->used) return;
    task_user_unmap(t);
    t->user_phys = 0;
    t->user_size = 0;
    t->stack_phys = 0;
    t->user_running = 0;
    t->page_dir = paging_kernel_dir();
    /* v0.3: pipes belong to syscall.cpp — release the ends
     * WITHOUT a disk flush (kill semantics, like the fd clear below).
     * File fds: the task slot is recycled, phantom descriptors must
     * not leak into the next program (normal exit flushed already). */
    syscall_task_close_pipes(t);
    for (int fd = 3; fd < TASK_MAX_FDS; fd++) {
        t->fds[fd].node  = NULL;
        t->fds[fd].pos   = 0;
        t->fds[fd].mode  = 0;
        t->fds[fd].dirty = 0;
        t->fds[fd].dcur  = NULL;
        t->fds[fd].pipe  = NULL;
        t->fds[fd].pipe_end = 0;
    }
    if (t->console >= 0) {
        /* memory-cleanup fix: a finished .mrp program no longer needs
         * the pixel canvas snapshot of the console it shared — release
         * the ~4 MB chunk back to the user physical pool. */
        if (t->kind == TASK_KIND_USER)
            console_canvas_invalidate(t->console);
        if (t->owns_console) {
            if (t->console == console_active_id()) {
                int nextc = console_next_used(t->console, -1);
                if (nextc >= 0) console_activate(nextc);
            }
            console_free(t->console);
        }
        t->console = -1;      /* shared: just drop the reference */
    }
}

/* v0.3 (FR-02): a parent fully died — its children must not
 * wait forever nor hold zombie slots: zombies are freed, live
 * children are orphaned (parent_pid = 0 -> their exit goes straight
 * to a full reap). */
static void task_orphan_children(int dead_pid) {
    if (dead_pid <= 0) return;
    uint32_t f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (!t->used || t->parent_pid != dead_pid) continue;
        if (t->zombie) {
            t->used  = 0;               /* nobody will wait: free it */
            t->state = TASK_FREE;
            t->zombie = 0;
        } else {
            t->parent_pid = 0;          /* orphaned */
        }
    }
    irq_restore(f);
}

/* ==================== exit / reap ==================== */

__attribute__((noreturn))
void task_exit_final(void) {
    uint32_t f = irq_save();
    Task* dead = g_cur_task;
    if (dead) {
        /* v0.3 (FR-02): USER tasks with a LIVE parent become a
         * ZOMBIE — the slot holds {pid, exit status} until the parent
         * calls wait() (or dies / the table overflows: alloc_slot
         * steals the oldest zombie). Everything else reaps fully. */
        int zombified = 0;
        if (dead->kind == TASK_KIND_USER && !dead->zombie) {
            Task* parent = task_find_live(dead->parent_pid);
            if (parent && parent != dead) {
                dead->exit_normal = dead->u3_normal ? 1 : 0;
                dead->exit_status = dead->u3_status;
                dead->zombie = 1;
                dead->state  = TASK_DEAD;
                task_release_resources(dead);
                task_wake_waitpid(dead->pid);
                zombified = 1;
            }
        }
        if (!zombified) {
            task_orphan_children(dead->pid);
            task_reap(dead);
            dead->state = TASK_DEAD;
            dead->used  = 0;      /* the slot may be reused */
        }
    }
    g_cur_task = NULL;

    /* pick another task manually (schedule() needs g_cur_task) */
    Task* next = NULL;
    for (int k = 0; k < MAX_TASKS && !next; k++) {
        Task* t = &tasks[k];
        if (t->used && t->state == TASK_READY && !t->killed) next = t;
    }

    if (next && dead) {
        next->state = TASK_RUNNING;
        next->quantum = TASK_QUANTUM;
        g_cur_task = next;
        /* NOTE: IRQs stay OFF across the switch — the dying stack is
         * abandoned; enabling them here would let IRQ0 hit g_cur_task
         * (= next) while still running on dead's stack and corrupt
         * next->ksp through a spurious schedule(). The resumed task
         * restores its own IF state in its schedule() frame. */
        __task_switch(dead, next);
        /* never returns for the dying task */
    }

    /* no other task: halt forever */
    irq_restore(f);
    printf("[task] no runnable task - halting\n");
    serial_puts("[task] no runnable task - halting\n");
    asm volatile("cli");
    for (;;) asm volatile("hlt");
    __builtin_unreachable();
}

void task_reap(Task* t) {
    if (!t || !t->used) return;
    /* v0.3: the eager-chunk frees are gone — task_user_unmap
     * walks the page tables and frees every FAULTED-IN page (demand
     * paging keeps no contiguous chunk). */
    task_release_resources(t);
}

/* ==================== anti-preemption lock ==================== */

void task_sched_lock(void)   { g_sched_lock_depth++; }
void task_sched_unlock(void) { if (g_sched_lock_depth) g_sched_lock_depth--; }

/* v0.3 debug: current sched-lock depth (leak detector for the
 * `nettask` hunt — see net_dbg_dump). */
extern "C" int task_sched_lock_depth(void) { return g_sched_lock_depth; }

/* v0.3 SAFETY NET: a leaked sched lock would starve EVERY sleeping
 * task (wake_scan only runs inside schedule(), which bails while
 * locked). syscall_dispatch calls this after every handler so a
 * storage-path leak can never wedge the system again — it is
 * reported loudly instead. */
extern "C" void task_sched_force_unlock(void) {
    if (g_sched_lock_depth) {
        printf("[task] WARNING: sched-lock leak (depth %d) - force-released\n",
               g_sched_lock_depth);
        g_sched_lock_depth = 0;
    }
}

/* ==================== IRQ dispatch (timer) ==================== */

void task_irq_dispatch(void) {
    if (!g_sched_ready) return;

    if (g_req_new_console) {
        g_req_new_console = 0;
        Task* t = task_create_shell();
        if (t) {
            console_activate(t->console);
            printf("[task] new shell pid %d on console %d (F1)\n",
                   t->pid, t->console);
        } else {
            printf("[task] task table full - cannot create a new shell\n");
        }
    }
    if (g_req_prev_console) {
        g_req_prev_console = 0;
        int prev = console_next_used(console_active_id(), -1);
        if (prev >= 0 && prev != console_active_id())
            console_activate(prev);
    }

    if (g_cur_task && g_cur_task->killed) {
        task_exit_final();
    }

    Task* cur = g_cur_task;
    if (!cur) return;
    if (--cur->quantum <= 0) {
        cur->quantum = TASK_QUANTUM;
        schedule();
    }
}

/* ==================== Phase C: ps / kill / info ==================== */

/* Snapshot of the task table for `ps` (shell) and SYS_TASKINFO (CPL 3
 * programs). out: u32 array, entries = {pid, state, kind, console}
 * (TASK_INFO_COLS). Returns the number of entries written. */
uint32_t task_fill_info(uint32_t* out, uint32_t max_entries) {
    if (!out || max_entries == 0) return 0;
    uint32_t f = irq_save();
    uint32_t n = 0;
    for (int i = 0; i < MAX_TASKS && n < max_entries; i++) {
        Task* t = &tasks[i];
        if (!t->used) continue;
        out[n * TASK_INFO_COLS + 0] = (uint32_t)t->pid;
        out[n * TASK_INFO_COLS + 1] = (uint32_t)t->state;
        out[n * TASK_INFO_COLS + 2] = (uint32_t)t->kind;
        out[n * TASK_INFO_COLS + 3] = (uint32_t)t->console;
        n++;
    }
    irq_restore(f);
    return n;
}

/* Mark a task dead. Non-blocking: the task is actually cleaned up by
 * the scheduler (pick_next reaps it / the task_irq_dispatch exit path).
 * Return: 0 ok, -1 pid invalid, -2 = this task itself, -3 = not found. */
int task_kill(int pid) {
    if (pid <= 0) return -1;
    uint32_t f = irq_save();
    int found = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (!t->used || t->pid != pid) continue;
        if (t == g_cur_task) {
            irq_restore(f);
            return -2;   /* killing yourself = exit(), not kill */
        }
        if (!t->killed) {
            t->killed = 1;
            /* if it is parked (asleep OR waiting for console focus):
             * wake it so the scheduler sees the killed flag and reaps
             * it (the draw gate exits without painting). */
            if (t->state == TASK_BLOCKED) t->state = TASK_READY;
        }
        found = 1;
    }
    irq_restore(f);
    return found ? 0 : -3;
}

/* Phase C: shell `ps` — dump the task table to the active console. */
void task_ps_dump(void) {
    static const char* st_name[] = {
        "FREE", "READY", "RUNNING", "SLEEP", "DEAD"
    };
    static const char* kd_name[] = { "shell", "user " };
    printf("  PID  STATE    KIND   CONS  NAME\n");
    printf("  ---- -------- -----  ----  --------\n");
    uint32_t f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (!t->used) continue;
        const char* st = (t->state <= 4) ? st_name[t->state] : "?";
        const char* kd = (t->kind <= 1) ? kd_name[t->kind] : "?";
        char nm[TASK_NAME_MAX + 1];
        for (int j = 0; j < TASK_NAME_MAX; j++) nm[j] = t->name[j];
        nm[TASK_NAME_MAX] = '\0';
        printf("  %4d  %-8s %-6s %4d  %s%s\n",
               t->pid, st, kd, t->console, nm,
               t->killed ? "  [killed]" : "");
    }
    irq_restore(f);
}

/* ==================== v0.3 (FR-02): wait/zombie + pipes ==================== */

/* Wake tasks blocked in task_wait_pid() whose request matches
 * child_pid (called when a task becomes a zombie). */
void task_wake_waitpid(int child_pid) {
    if (child_pid <= 0) return;
    uint32_t f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (!t->used || t->state != TASK_BLOCKED) continue;
        if (t->wait_pid == -2 || t->wait_pid == child_pid)
            t->state = TASK_READY;
    }
    irq_restore(f);
}

/* Wake every task blocked on a pipe end (data arrived / end closed /
 * broken pipe). The pipe object itself is owned by syscall.cpp. */
void task_wake_pipe(struct kpipe* p) {
    if (!p) return;
    uint32_t f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (t->used && t->state == TASK_BLOCKED && t->wait_pipe == p)
            t->state = TASK_READY;
    }
    irq_restore(f);
}

/* Blocking waitpid (SYS_WAIT / shell `wait`). pid > 0 = that child,
 * pid <= 0 = any child. Return: the reaped child's pid (> 0), or a
 * negative errno (-ECHILD). Runs in the CALLER's (parent's) context. */
int task_wait_pid(int pid, uint32_t* status_out) {
    struct Task* me = task_current();
    if (!me) return -1;

    for (;;) {
        /* 1. collect an already-zombie child */
        for (int i = 0; i < MAX_TASKS; i++) {
            Task* t = &tasks[i];
            if (!t->used || !t->zombie) continue;
            if (t->parent_pid != me->pid) continue;
            if (pid > 0 && t->pid != pid) continue;
            if (status_out) *status_out = t->exit_status;
            int got = t->pid;
            t->zombie = 0;
            t->used   = 0;      /* the wait() reaps: slot free */
            t->state  = TASK_FREE;
            return got;
        }

        /* 2. any live child left to wait for? */
        int live = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            Task* t = &tasks[i];
            if (!t->used || t->zombie) continue;
            if (t->parent_pid != me->pid) continue;
            if (pid > 0 && t->pid != pid) continue;
            live = 1;
            break;
        }
        if (!live) return -13;            /* SYS_ECHILD */

        /* 3. block until a child exits (task_wake_waitpid wakes us) */
        me->wait_pid   = (pid > 0) ? pid : -2;
        me->state      = TASK_BLOCKED;
        me->sleep_until = 0xFFFFFFFFu;    /* no timer wake */
        schedule();
        me->wait_pid = -1;

        if (me->killed) return -13;       /* killed while waiting */
        /* loop: maybe another child matched, or a spurious wake */
    }
}

/* ==================== FR-05: pool stats + meminfo ==================== */

uint32_t task_user_phys_total_bytes(void) {
    return POOL_END - POOL_START;
}

uint32_t task_user_phys_free_bytes(void) {
    uint32_t f = irq_save();
    uint32_t free_kb = 0;
    for (uint32_t i = 0; i < POOL_PAGES; i++)
        if (!(pool_bitmap[i >> 5] & (1u << (i & 31)))) free_kb++;
    irq_restore(f);
    return free_kb * 0x1000u;
}

uint32_t task_pages_user_total(void) {
    uint32_t f = irq_save();
    uint32_t n = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].used) n += tasks[i].pages_user;
    irq_restore(f);
    return n;
}

/* Shell `meminfo` / SYS_MEMINFO view: physical pool + per-task
 * demand-paging footprint (FR-05 accounting). */
void task_mem_dump(void) {
    uint32_t total = task_user_phys_total_bytes();
    uint32_t freeb = task_user_phys_free_bytes();
    uint32_t faulted = task_pages_user_total() * 0x1000u;
    printf("Equinox OS user physical pool (demand paging, v0.3):\n");
    printf("  pool:      %u KB total, %u KB free, %u KB faulted by tasks\n",
           total / 1024u, freeb / 1024u, faulted / 1024u);
    printf("  (reserved-but-untouched pages cost nothing — pages are\n"
           "   handed out zero-filled on the first #PF)\n");
    printf("  PID  KIND   NAME        RESV-KB  FAULT-KB  STK-PG\n");
    printf("  ---- -----  ----------  -------  --------  ------\n");
    uint32_t f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        if (!t->used) continue;
        static const char* kd[] = { "shell", "user ", "krnl " };
        const char* kd_s = (t->kind <= 2) ? kd[t->kind] : "?";
        char nm[TASK_NAME_MAX + 1];
        for (int j = 0; j < TASK_NAME_MAX; j++) nm[j] = t->name[j];
        nm[TASK_NAME_MAX] = '\0';
        uint32_t stack_pages = 0;
        if (t->page_dir == task_pds[i]) {
            for (uint32_t a = USER_STACK_START; a < USER_STACK_END; a += 0x1000) {
                uint32_t* pte = task_pte_slot(t, a);
                if (pte && (*pte & PTE_PRESENT) && (*pte & PTE_USER)) stack_pages++;
            }
        }
        printf("  %4d  %-5s  %-10s  %7u  %8u  %6u%s\n",
               t->pid, kd_s, nm, t->arena_reserve / 1024u,
               (t->pages_user * 0x1000u) / 1024u, stack_pages,
               t->zombie ? "  [zombie]" : "");
    }
    irq_restore(f);
}

/* ==================== Phase B: user tasks (.mrp) ==================== */

/* Kernel-side entry of a user task: CR3 + TSS + console output MUST be
 * set MANUALLY here — the FIRST activation enters through the
 * trampoline (not through a return from schedule()), so
 * task_post_switch() does not run automatically. Without this the
 * program runs on the spawner's CR3 -> #PF. */
static void user_task_entry(void* arg) {
    (void)arg;
    task_post_switch();   /* Phase B FIX: load this task's page dir + TSS + console */
    struct Task* t = task_current();
    uint32_t st = user3_launch4(t->user_entry, USER_STACK_TOP, 0, &t->u3);
    (void)st;
    /* v0.3 (FR-02): program finished — flush every dirty fd NOW
     * (task context) so an unflushed write never becomes a zombie's
     * lost data; pipes are released by the exit path below. */
    syscall_fd_flush_all();
    /* program finished / killed -> terminate this task */
    task_exit_final();
}

/* block_header layout (malloc.cpp) — MUST stay in sync:
 * i386: {u32 magic; size; u8 free; pad[3]; block_header* next} = 16 bytes. */
#define BLK_MAGIC 0x4D42u
struct spawn_blk { uint32_t magic; uint32_t size; uint8_t free; uint8_t pad[3];
                   struct spawn_blk* next; };

/* v0.3: unwind a failed create AFTER the slot was taken (the
 * pipe ends inherited from the spawner must be dropped again and the
 * task-private pages returned to the pool). */
static void task_create_user_fail(Task* t, const char* why) {
    if (!t) return;
    if (why) printf("mrp: task create failed: %s\n", why);
    task_user_unmap(t);          /* frees faulted-in pages           */
    syscall_task_close_pipes(t); /* drop inherited pipe ends          */
    t->used = 0;
}

/* Create a user task from a .mrp OR ELF32 program file. Runs in the
 * SPAWNER's context (CPL 0). v0.3: the image is copied through
 * the NEW task's demand window (task_demand_fill: CR3 switched, IRQs
 * off) — no upfront physical chunk, only the touched pages exist.
 * The new task is then scheduled round-robin. */
struct Task* task_create_user(const char* name, struct fs_node* parent,
                              const char* fname, const char* args,
                              uint32_t arena_hint) {
    if (!parent || !fname) return NULL;

    /* 1. find + validate the file (.mrp OR ELF — FR-07) */
    struct fs_node* file = fs_find_child(parent, fname);
    if (!file || file->is_dir || !file->content || file->size == 0) return NULL;
    const uint8_t* data = (const uint8_t*)file->content;

    int is_elf = elf_is_elf(data, file->size);
    uint32_t elf_entry = 0, elf_lo = 0, elf_span = 0;
    enum mrp_validate_reason reason;
    if (is_elf) {
        if (elf_check(data, file->size, &elf_entry, &elf_lo, &elf_span) != 0)
            return NULL;
    } else if (!is_valid_mrp(data, file->size, &reason)) {
        return NULL;
    }
    const struct mrp_header* hdr = (const struct mrp_header*)data;
    const uint8_t* code = data + MRP_HEADER_SIZE;
    uint32_t code_pad = is_elf ? 0u : ((hdr->code_size + 7u) & ~7u);

    /* 2. task slot */
    Task* t = task_alloc_slot();
    if (!t) return NULL;
    t->pid   = g_next_pid++;
    /* EMBRYO BUG FIX: BLOCKED for the whole create (the 2-8 MB
     * alloc + map + code-copy window can span several ticks — IRQ0
     * WILL fire). Premature READY = the scheduler may pick a task
     * with ksp=0/page_dir=0 -> crash. */
    t->state = TASK_BLOCKED;
    t->kind  = TASK_KIND_USER;
    if (name) { strncpy(t->name, name, TASK_NAME_MAX - 1); }
    else      { strncpy(t->name, fname, TASK_NAME_MAX - 1); }
    t->kstack_top = (uint32_t)(uintptr_t)&t->kstack[TASK_KSTACK];
    t->istack_top = (uint32_t)(uintptr_t)&t->istack[TASK_KSTACK];

    /* console: SHARE the spawner's console (program output goes to
     * the same console — like a UNIX child writing to the same
     * terminal) */
    struct Task* spawner = task_current();
    t->console = spawner ? spawner->console : -1;
    t->owns_console = 0;      /* SHARED console — reap must not free it */
    t->cwd     = spawner ? spawner->cwd : NULL;
    /* v0.3 (FR-02): parentage for wait()/zombie + PIPE ends are
     * inherited (file fds are NOT — FR-01 "never inherit" stands). */
    t->parent_pid = spawner ? spawner->pid : 0;
    if (spawner) {
        for (int fd = 3; fd < TASK_MAX_FDS; fd++) {
            if (spawner->fds[fd].node == NULL && spawner->fds[fd].pipe) {
                t->fds[fd].pipe     = spawner->fds[fd].pipe;
                t->fds[fd].pipe_end = spawner->fds[fd].pipe_end;
                kpipe_end_ref(spawner->fds[fd].pipe, spawner->fds[fd].pipe_end, +1);
            }
        }
    }
    if (args) { strncpy(t->args, args, TASK_ARGS_MAX - 1); }

    /* 3. reserve the demand window (no physical chunk — FR-06) */
    if (is_elf) {
        if (task_user_map_demand(t, ELF_HEAP_VMA, ELF_HEAP_BYTES) != 0) {
            task_create_user_fail(t, NULL);
            return NULL;
        }
    } else {
        if (arena_hint == 0) arena_hint = 2u * 1024u * 1024u;
        uint32_t need = (hdr->code_size + arena_hint + 0xFFFu) & ~0xFFFu;
        if (16u + code_pad + 16u > need) { t->used = 0; return NULL; }
        if (task_user_map_demand(t, USER_ARENA_START, need) != 0) {
            task_create_user_fail(t, NULL);
            return NULL;
        }
    }

    if (is_elf) {
        /* 4a. ELF image: reserve + fill every PT_LOAD (FR-07) */
        if (elf_load(t, data, file->size) != 0) {
            task_create_user_fail(t, "ELF segment out of range");
            return NULL;
        }
        t->user_entry = elf_entry;
    } else {
        /* 4b. .mrp image — build the arena blocks + copy the code
         *   [Block0 16B: busy, size=code][code bytes][Block1 16B: free, rest]
         *   VMA 0x500000 = Block0, VMA 0x500010 = code — exactly the .mrp
         *   link convention (link_mrp.ld) — programs run without
         *   relocation. The bytes go in through the NEW task's demand
         *   window (task_demand_fill). */
        uint32_t need = (uint32_t)(t->mrp_arena.max - t->mrp_arena.start);
        task_demand_fill(t, USER_ARENA_START + 16u, code, hdr->code_size);

        struct spawn_blk b0, b1;
        b0.magic = BLK_MAGIC; b0.size = hdr->code_size; b0.free = 0;
        b0.next  = (struct spawn_blk*)(uintptr_t)(USER_ARENA_START + 16u + code_pad);
        b1.magic = BLK_MAGIC; b1.size = need - 16u - code_pad - 16u;
        b1.free  = 1; b1.next = NULL;
        task_demand_fill(t, USER_ARENA_START, (const uint8_t*)&b0, sizeof b0);
        task_demand_fill(t, USER_ARENA_START + 16u + code_pad,
                         (const uint8_t*)&b1, sizeof b1);

        t->mrp_arena.head   = (void*)(uintptr_t)USER_ARENA_START;  /* Block0 VMA */
        t->mrp_arena.inited = 1;
        t->user_entry  = USER_ARENA_START + 16u + hdr->entry_offset;
    }

    /* 5. trampoline exit stub + craft the stack */
    user_trampoline_init();
    task_fpu_init_state(t);
    task_craft_stack(t, user_task_entry, NULL);
    t->state = TASK_READY;     /* FIX: stack + page dir + arena are all ready */
    return t;
}
