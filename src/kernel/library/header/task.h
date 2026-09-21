#ifndef TASK_H
#define TASK_H

/*
 * ============================================================================
 *  task.h — Multitasking: round-robin scheduler + virtual consoles (Phase A)
 * ----------------------------------------------------------------------------
 *  Model: kernel tasks, cooperative + preemptive at controlled points.
 *
 *  - Every task owns a KERNEL STACK (16 KB) + an FPU save area (fxsave).
 *  - Context switches happen only at CPL 0:
 *      (a) timer IRQ0 (100 Hz) when the quantum expires
 *      (b) task_yield() / task_sleep() called by the task itself
 *  - While a task runs a .mrp program at CPL 3, IRQ0 enters via
 *    TSS.ESP0 = the CURRENT task's kernel stack -> the frame is saved
 *    on that task's stack -> context switches stay valid. TSS.ESP0 is
 *    reloaded on every switch.
 *  - .mrp programs are still linked at the fixed VMA 0x500010, but EACH
 *    task maps VMA 0x500000+ to its OWN physical chunk (per-task page
 *    directory) -> two programs can run side by side without clobbering
 *    each other.
 * ============================================================================
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_TASKS     8
#define TASK_KSTACK   16384u       /* 16 KB kernel stack per task */
#define TASK_MAX_FDS  16           /* = SYS_MAX_FDS (syscall.cpp) */

/* Task states. */
enum {
    TASK_FREE = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,                  /* sleeping until sleep_until */
    TASK_DEAD
};

/* Task kinds. */
enum {
    TASK_KIND_SHELL = 0,
    TASK_KIND_USER  = 1,           /* spawned .mrp program (Phase B) */
    TASK_KIND_KERNEL = 2           /* v0.3 FR-09: console-less worker (nettask) */
};

struct fs_node;

/* fd entry (layout MUST match the legacy definition in syscall.cpp).
 * v0.3 FR-01: extended with open flags, a write-back dirty mark and
 * a readdir cursor. v0.3 (FR-02): extended with the pipe object
 * + end marker (an fd is EITHER a file: node != NULL, OR a pipe end:
 * node == NULL && pipe != NULL — fd 0/1/2 have both NULL = console).
 * Existing offsets (node, pos) are unchanged so the asm/FPU offsets in
 * Task stay valid — new fields are appended per-entry only. */
struct kpipe;                     /* defined in syscall.cpp (owner) */

struct sys_fd_entry {
    struct fs_node* node;          /* NULL = console / pipe / free slot */
    uint32_t pos;                  /* next read position */
    uint32_t mode;                 /* v0.3: SYS_O_* flags the fd was opened with */
    uint32_t dirty;                /* v0.3: 1 = in-memory writes pending disk flush */
    struct fs_node* dcur;          /* v0.3: SYS_READDIR cursor (O_DIR fds) */
    struct kpipe* pipe;            /* v0.3: pipe object (node == NULL) */
    uint32_t pipe_end;             /* v0.3: 0 = read end, 1 = write end */
};

/* u3_save — written by the asm user3_launch4 (offsets 0 and 4 MUST stay). */
struct u3_save {
    uint32_t esp;                  /* resume esp  (frame of mrp_run) */
    uint32_t eip;                  /* resume eip  (label user3_killed) */
};

/* Per-task MRP arena state (layout compatible with heap_arena in
 * malloc.cpp — cast there; a static_assert guards it). VMA-based. */
struct task_mrp_arena {
    uintptr_t start;               /* always USER_ARENA_START (VMA) */
    uintptr_t max;                 /* start + chunk size */
    void*     head;                /* first block_header* (VMA) */
    uint8_t   inited;
};

#define TASK_NAME_MAX  16
#define TASK_ARGS_MAX  128

/*
 * IMPORTANT OFFSETS (used by the asm __task_switch):
 *   offsetof(Task, ksp)       == 0
 *   offsetof(Task, fpu_state) == 16
 * Task must be aligned(16).
 */
struct Task {
    uint32_t     ksp;                       /* +0   saved esp at the switch point */
    uint8_t      fpu_state[512] __attribute__((aligned(16)));  /* +16 fxsave */

    int          used;
    int          pid;
    int          state;
    int          kind;
    char         name[TASK_NAME_MAX];

    uint32_t     kstack_top;                /* top of the main kernel stack */
    uint8_t      kstack[TASK_KSTACK];       /* main kernel stack (task frames) */
    uint32_t     istack_top;                /* top of the INTERRUPT stack (TSS.ESP0) */
    uint8_t      istack[TASK_KSTACK];       /* CPL3->CPL0 stack (syscalls / IRQs) */

    int          console;                   /* virtual console id (-1 = none) */
    int          owns_console;              /* 1 = this task OWNS the console (may free it) */
    int          wait_console;              /* >= 0: blocked waiting for that console's
                                                focus (SIGTTOU-style graphics gating) */

    /* --- v0.3 FR-17: per-task draw window (clip rect) ---
     * set_clip() confines put_pixel / fill_rect / draw_line of THIS
     * task to [x, x+w) x [y, y+h) — the kernel enforces it, a rogue
     * program cannot paint outside its window even at ring 3.
     * clip_on == 0 = no clip (full screen, the default). */
    int          clip_on;
    int          clip_x, clip_y, clip_w, clip_h;

    /* --- per-task .mrp program state --- */
    int          user_running;              /* 1 = mrp_run active in this task */
    uint32_t     user_phys;                 /* physical chunk base (0 = none) */
    uint32_t     user_size;
    uint32_t     stack_phys;                /* physical base of the 1 MB user stack */
    uint32_t*    page_dir;                  /* page directory of the task (VMA) */
    uint32_t     user_entry;                /* program entry VMA (Phase B) */
    struct u3_save u3;                      /* resume point of user3_launch */
    uint32_t     u3_normal;                 /* normal exit? (0/1) */
    uint32_t     u3_status;                 /* exit status / fault code */
    struct task_mrp_arena mrp_arena;        /* per-task arena allocator */

    /* --- per-task syscall state --- */
    struct sys_fd_entry fds[TASK_MAX_FDS];
    struct fs_node* cwd;
    char         args[TASK_ARGS_MAX];

    /* --- v0.3 (FR-02): parentage + wait/zombie --- */
    int          parent_pid;       /* 0 = no parent / orphaned (pid 0 never exists) */
    int          zombie;           /* 1 = exited, holding the status for wait() */
    int          exit_normal;      /* 1 = clean exit(), 0 = faulted/killed */
    uint32_t     exit_status;      /* u3_status snapshot at exit time */
    int          wait_pid;         /* -1 none; -2 = any child; >0 = specific pid */
    struct kpipe* wait_pipe;       /* non-NULL while blocked on a pipe end */

    /* --- v0.3 (FR-05/06): demand-paging accounting --- */
    uint32_t     pages_user;       /* user pages actually faulted in (4 KB each) */
    uint32_t     arena_reserve;    /* reserved arena bytes (VMA window size) */

    /* --- scheduling --- */
    uint32_t     sleep_until;
    int          quantum;
    int          killed;
} __attribute__((aligned(16)));

/* ==================== API ==================== */

struct Task* task_current(void);
int  task_getpid(void);
int  task_sched_active(void);

void task_init0(void);
struct Task* task_create_shell(void);

/* v0.3 FR-09: console-less kernel worker task. `fn` MUST call
 * task_kernel_start() first (first-activation TSS/CR3 setup). */
struct Task* task_create_kernel(const char* name, void (*fn)(void*), void* arg);
extern "C" void task_kernel_start(void);

struct Task* task_create_user(const char* name, struct fs_node* parent,
                              const char* fname, const char* args,
                              uint32_t arena_hint);

void schedule(void);
void task_yield(void);
void task_sleep(uint32_t ms);

/* Phase C: task info & control for `ps` / `kill` / `switch`. */
#define TASK_INFO_COLS 4   /* {pid, state, kind, console} per entry */
uint32_t task_fill_info(uint32_t* out, uint32_t max_entries);
int      task_kill(int pid);        /* 0 ok / -1 EINVAL / -2 self / -3 ENOENT */
void     task_ps_dump(void);       /* `ps`: dump the task table to the active console */

/* v0.3 FR-08: count fd references to a node across all tasks
 * (skip_fd: fd of the CURRENT task to skip, -1 = none). */
int      task_fd_count_node(struct fs_node* node, int skip_fd);

/* SIGTTOU-style graphics focus gating (fixes the cross-console pixel
 * leak: a background game painting over another terminal's screen).
 * Called by the graphics syscalls (fillrect / putpixel / blit) before
 * they touch the framebuffer. If the calling task's console is NOT the
 * active one, the task is SUSPENDED until its console regains focus
 * (console_activate -> task_wake_console), exactly like a Unix
 * SIGTTOU'd background process. Returns 1 when the draw may proceed,
 * 0 when it must be skipped (the task was killed while waiting). */
int  task_console_draw_gate(void);
void task_wake_console(int console_id);

__attribute__((noreturn))
void task_exit_final(void);
void task_reap(struct Task* t);

/* ==================== v0.3: demand paging (FR-06) ====================
 * PTE software bit 9 (OS-available, ignored by the hardware when
 * P=0): a non-present PTE carrying PTE_DEMAND marks a RESERVED user
 * page. The first CPL 3 (or loader CPL 0) access faults; isr_14 calls
 * task_demand_fault() which allocates + zero-fills + maps one physical
 * page and resumes the faulting instruction. Physical memory is only
 * consumed by pages that are actually touched. */
#define PTE_DEMAND 0x200u

/* Reserve the per-task VMA window: [arena_vma, +arena_bytes) plus the
 * 1 MB user stack — all DEMAND (non-present, U/S=1). Replaces the old
 * eager task_user_map() chunk allocation. 0 / -1. */
int  task_user_map_demand(struct Task* t, uint32_t arena_vma,
                          uint32_t arena_bytes);

/* Reserve an EXTRA demand range (ELF image segments, FR-07). */
void task_demand_reserve(struct Task* t, uint32_t vma, uint32_t len);

/* #PF hook (isr_14): 1 = fault satisfied (page mapped, resume),
 * 0 = not ours (fall through to the kill/panic path). */
int  task_demand_fault(uint32_t cr2, uint32_t err_code);

/* Copy `len` bytes into the task's demand window (loader context,
 * CPL 0): CR3 is switched to the task's directory for the copy and
 * every page is faulted in explicitly (the generic #PF path must NOT
 * be used cross-task). Safe against preemption (irq_save). */
void task_demand_fill(struct Task* t, uint32_t vma,
                      const uint8_t* src, uint32_t len);

/* FR-05 accounting helpers + `meminfo` dump. */
uint32_t task_read_cr3(void);
uint32_t task_user_phys_total_bytes(void);
uint32_t task_user_phys_free_bytes(void);
uint32_t task_pages_user_total(void);
void     task_mem_dump(void);

/* ==================== v0.3: wait/zombie (FR-02) ====================
 * Blocking waitpid: pid > 0 = that child, pid <= 0 = any child.
 * Returns the reaped child's pid (> 0) and writes its status to
 * *status_out (32-bit exit code, 0x80000000|vec = faulted), or a
 * negative errno (SYS_ECHILD = no matching child). */
int  task_wait_pid(int pid, uint32_t* status_out);

/* Wake tasks blocked in wait() whose request matches child_pid
 * (called when a task becomes a zombie). */
void task_wake_waitpid(int child_pid);

/* ==================== v0.3: pipes (FR-02) ====================
 * Wake every task blocked on the pipe (data arrived / end closed).
 * The pipe object itself lives in syscall.cpp. */
void task_wake_pipe(struct kpipe* p);

/* Refcount a pipe end +1/-1 (spawn inheritance / teardown). Defined
 * in syscall.cpp next to the pipe object. */
void kpipe_end_ref(struct kpipe* p, uint32_t end, int delta);

/* v0.3: release every fd of a task (pipes + node refs) WITHOUT
 * a disk flush — called from task_reap (IRQ context). */
void syscall_task_close_pipes(struct Task* t);

void task_sched_lock(void);
void task_sched_unlock(void);
/* v0.3: leak detection / safety net (see syscall_dispatch). */
extern "C" int  task_sched_lock_depth(void);
extern "C" void task_sched_force_unlock(void);

void tss_set_esp0(uint32_t esp0);
void task_load_cr3(uint32_t* pd);

uint32_t task_user_phys_alloc(uint32_t bytes);
void     task_user_phys_free(uint32_t phys, uint32_t bytes);

void task_user_unmap(struct Task* t);

/* Fallback arena for malloc.cpp when g_cur is NULL. */
struct task_mrp_arena* task_fallback_arena(void);

/* Called from timer IRQ0: F1/F2 + quantum + schedule. */
void task_irq_dispatch(void);

/* F1/F2 requests from the IRQ1 keyboard handler (served by task_irq_dispatch). */
extern int g_req_new_console;
extern int g_req_prev_console;

#ifdef __cplusplus
}
#endif

#endif /* TASK_H */
