#ifndef ELF_H
#define ELF_H

/*
 * ============================================================================
 *  elf.h — ELF32 (i386, ET_EXEC) loader (v0.3, FR-07)
 * ----------------------------------------------------------------------------
 *  Load path: the shell `run`/`./prog` dispatcher and SYS_SPAWN both
 *  detect the \x7FELF magic BEFORE the .mrp validation, so a static
 *  ELF32 executable built by ANY i386 toolchain runs next to .mrp
 *  programs — same ring-3 launch path (user3_launch4), same syscalls
 *  (int 0x80, ABI in syscall.h), same exit trampoline.
 *
 *  IMAGE LAYOUT CONTRACT (uaccess-compatible):
 *    - every PT_LOAD p_vaddr must land inside [ELF_IMG_MIN, ELF_IMG_MAX)
 *      (0x00800000-0x02000000) — inside the task-private PDEs 2-7 and
 *      inside the uaccess arena range check, so syscall pointers from
 *      ELF .data/.bss pass validation exactly like .mrp pointers.
 *    - the .mrp malloc arena is reserved separately at ELF_HEAP_VMA
 *      (SYS_MALLOC works for ELF programs too).
 *    - entry = e_entry; a plain `ret` lands on the standard user
 *      trampoline (exit stub) exactly like .mrp programs.
 *
 *  Pages are DEMAND pages (task_demand_fill): p_filesz bytes are
 *  copied in, the p_memsz tail (bss) faults in zero-filled later.
 * ============================================================================
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Image window (see the contract above). */
#define ELF_IMG_MIN     0x00800000u    /* 8 MB  — above the .mrp arena start */
#define ELF_IMG_MAX     0x02000000u    /* 32 MB — below the ELF heap          */
/* Heap arena for ELF programs (SYS_MALLOC / mrp_alloc). */
#define ELF_HEAP_VMA    0x02000000u    /* 32 MB — PDE 8, task-private        */
#define ELF_HEAP_BYTES  0x00200000u    /* 2 MB default heap slack            */

/* Magic + structural check only (no task needed — used to sniff the
 * format before a slot is allocated). Returns 1/0. */
int  elf_is_elf(const uint8_t* data, uint32_t size);

/* Full validation + span computation (no task needed). On success
 * returns 0 and fills *entry (e_entry), *lo (page-aligned lowest
 * PT_LOAD vaddr), *span (page-aligned total image span). Non-zero =
 * reject reason (message via printf by the caller). */
int  elf_check(const uint8_t* data, uint32_t size, uint32_t* entry,
               uint32_t* lo, uint32_t* span);

/* Load into the TASK's demand window: reserves every PT_LOAD span and
 * fills the file-backed bytes (task_demand_fill: CR3 switched, IRQ
 * off). Call task_user_map_demand(ELF_HEAP_VMA, ELF_HEAP_BYTES)
 * FIRST. Returns 0 / -1. */
int  elf_load(struct Task* t, const uint8_t* data, uint32_t size);

/* Human-readable reason for the last elf_check rejection (static
 * buffer — print immediately). */
const char* elf_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* ELF_H */
