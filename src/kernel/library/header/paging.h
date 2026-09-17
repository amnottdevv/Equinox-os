#ifndef PAGING_H
#define PAGING_H

/*
 * ============================================================================
 *  paging.h — Identity-mapped paging with a supervisor/user split (v10.7)
 * ----------------------------------------------------------------------------
 *  Paging is the prerequisite for ring 3 protection: the U/S bit on page
 *  directory/page table entries is what makes kernel memory untouchable
 *  from CPL 3 (segment limits alone are not enough — our user DS is a
 *  flat 4GB).
 *
 *  Map (identity — virtual == physical, no relocation):
 *    0x00000000-0x04FFFFFF  supervisor  (16 tables for 64 MB QEMU RAM)
 *        except two USER blocks:
 *          0x00500000-0x00900FFF  user (MRP arena + trampoline page)
 *          0x00902000-0x00911FFF  user (64 KB user stack)
 *        0x00901000-0x00901FFF deliberately SUPERVISOR = guard page
 *        (user stack overflow -> #PF -> program killed, instead of
 *        silently trampling the trampoline/arena).
 *    The VESA LFB (e.g. 0xFD000000 on QEMU std) is mapped supervisor for
 *    pitch*height — without this, the first printf after paging is enabled
 *    would #PF immediately because the framebuffer is not in the page
 *    directory yet.
 *
 *  4 KB granularity (not PSE 4 MB) so the user boundary is precise.
 *
 *  paging_init() is called by kernel_main AFTER vesa_init/force_mode
 *  (needs the fb address) and BEFORE init_display() (first drawing).
 * ============================================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Build the page directory + tables, then enable them (CR3 + CR0.PG).
 * Idempotent-guard: a second call is ignored. */
void paging_init(void);

/* 1 = paging is active (used by the `memmap` diagnostics). */
int paging_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* PAGING_H */
