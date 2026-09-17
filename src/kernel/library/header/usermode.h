#ifndef USERMODE_H
#define USERMODE_H

/*
 * ============================================================================
 *  usermode.h — Ring 3 (user mode) support, v10.7
 * ----------------------------------------------------------------------------
 *  This is the module that makes .mrp programs run at CPL 3 with real
 *  memory protection (U/S paging), no longer a "confident ring 0".
 *
 *  Life cycle of one .mrp program (single-tasking, no scheduler):
 *
 *    mrp_run()  ── user3_launch(entry, esp, arg)
 *                    │  save kernel context (resume esp/eip) + ebx/esi/edi
 *                    │  push iret frame {SS=0x23, ESP, EFLAGS(IF=1), CS=0x1B, EIP}
 *                    │  DS/ES/FS/GS ← 0x23  then  iretl  ──► CPL 3
 *                    ▼
 *              [ user program runs ]
 *                    │  syscall  : int 0x80  (trap gate DPL=3) → CPL 0 on
 *                    │             user_int_stack (TSS.ESP0), iret back
 *                    │  exit()   : SYS_EXIT from ring 3 → user3_terminate()
 *                    │  crash    : #DE/#PF/#GP/... → exception handler sees
 *                    │             CS&3==3 → user3_report_fault() → kill
 *                    ▼
 *              user3_terminate() / user3_report_fault()
 *                    │  sti + kern_longjmp(saved_esp, saved_eip)
 *                    ▼
 *              mrp_run() continues cleanup (arena reset) → back to the shell.
 *
 *  THE SHELL NEVER DIES from a user program fault. A full panic (red
 *  screen + 30-second reboot) still happens only for CPL 0 faults =
 *  kernel bugs, as it should be.
 * ============================================================================
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------
 *  USER MEMORY MAP (identity-mapped, see paging.cpp)
 * ---------------------------------------------------------------
 *  0x00500000 - 0x025FFFFF  USER   MRP arena (.mrp code + program heap, 33 MB)
 *  0x02600000 - 0x02600FFF  USER   trampoline page (exit stub at 0x2600000)
 *  0x02601000 - 0x02601FFF  SUPER  GUARD page (stack overflow detection)
 *  0x02602000 - 0x02701FFF  USER   user stack 1 MB (initial ESP = 0x2702000)
 *  0x02800000 - 0x033FFFFF  SUPER  GRUB module staging (WAD zero-copy, v10.9)
 *  Every other address (kernel, kernel heap, below 64 MB
 *  and the framebuffer) = supervisor (U/S=0) → access from CPL 3 = #PF.
 * --------------------------------------------------------------- */
#define USER_ARENA_START   0x500000u   /* MRP arena (malloc.cpp MRP_HEAP_START) */
#define USER_ARENA_END     0x2600000u  /* v10.9 "DOOM layout": 33 MB (was 4 MB) */
#define USER_TRAMPOLINE    0x2600000u  /* `exit` stub for programs that just ret */
#define USER_GUARD_START   0x2601000u  /* supervisor: stack overflow detector    */
#define USER_GUARD_END     0x2602000u
#define USER_STACK_START   0x2602000u
#define USER_STACK_END     0x2702000u  /* v10.9: 1 MB (was 64 KB) */
#define USER_STACK_TOP     USER_STACK_END

/* Ring 3 selectors (must match the GDT in start.asm). */
#define USER_CS_SEL       0x1Bu        /* 0x18 | 3 */
#define USER_DS_SEL       0x23u        /* 0x20 | 3 */
#define KERNEL_DATA_SEL   0x10u

/* ---------------------------------------------------------------
 *  tss_init() — patches the TSS descriptor in the GDT + `ltr`.
 *  Called by kernel_main AFTER paging_init(), BEFORE the first user
 *  program. TSS.SS0 = kernel data, TSS.ESP0 = top of user_int_stack:
 *  EVERY CPL3→CPL0 transition (int 0x80, exception, IRQ while a user
 *  program runs) automatically gets a clean kernel stack, separate
 *  from the shell/mrp_run stack that is frozen waiting for the program.
 * --------------------------------------------------------------- */
void tss_init(void);

/* Make sure the exit stub exists in the trampoline page (idempotent,
 * cheap). Done on every user3_launch: the stub is only 11 bytes in the
 * first user page. */
void user_trampoline_init(void);

/* Jump to ring 3. NEVER returns via the normal path — only via the
 * kill/exit path (longjmp to the landing pad). Return value = the
 * program's exit status (from SYS_EXIT); normal/kill is distinguished
 * via user3_exit_normal(). */
uint32_t user3_launch(uint32_t entry, uint32_t user_esp, uint32_t arg);

/* 1 = the last program finished via a normal exit(); 0 = killed
 * because of a fault. Read by mrp_run() after user3_launch() returns. */
int user3_exit_normal(void);

/* Terminate the user program immediately from a CPL 0 context (called
 * by the ring 3 SYS_EXIT path / exception handler). msg != NULL is
 * printed first (e.g. a fault report), then sti + longjmp back to
 * mrp_run(). NORETURN — the user_int_stack is simply abandoned
 * (TSS.ESP0 is constant at its top, so the next transition starts
 * clean). */
__attribute__((noreturn))
void user3_terminate(int normal, uint32_t status, const char* msg);

/* Exception-handler variant: builds a "program faulted" report
 * (exception name, EIP, CR2/error code for #PF) then terminates.
 * vec < 32. NORETURN. */
__attribute__((noreturn))
void user3_report_fault(uint8_t vec, uint32_t err_code, uint32_t eip);

/* Shell `memmap` command helper: prints the region map + paging status. */
void usermode_print_memmap(void);

#ifdef __cplusplus
}
#endif

#endif /* USERMODE_H */
