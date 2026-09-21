/*
 * ============================================================================
 *  paging.cpp — Identity map 0-64MB + LFB, split supervisor/user (v10.7)
 * ----------------------------------------------------------------------------
 *  The data structures live in the kernel .bss (NOBITS - zeroed by start.asm):
 *    page_dir[1024]        4 KB   directory
 *    low_pts[16][1024]    64 KB   16 tables for 0-64 MB (4 KB per entry)
 *    fb_pts[4][1024]      16 KB   4 spare tables for the LFB region
 *
 *  The .bss grows by ~85 KB - still far below KERNEL_HEAP (0x300000);
 *  .bss now ends around ~0x240000.
 *
 *  Entry flags: P=1, R/W=1 (every region writable by the supervisor;
 *  protection depends on U/S), U/S=1 only for user blocks.
 *  CR0.WP stays 0 (default): the supervisor may write read-only pages -
 *  required by tss_init(), which patches the GDT descriptor in .text
 *  after paging is on.
 * ============================================================================
 */

#include "header/paging.h"
#include "header/vesa.h"        /* LFB address/size */
#include "header/usermode.h"    /* user region constants */
#include <stdint.h>
#include <stddef.h>

#define PTE_PRESENT  0x001u
#define PTE_RW       0x002u
#define PTE_USER     0x004u

static uint32_t page_dir[1024]  __attribute__((aligned(4096)));
static uint32_t low_pts[16][1024] __attribute__((aligned(4096)));
static uint32_t fb_pts[4][1024]   __attribute__((aligned(4096)));

static int paging_active = 0;

int paging_is_active(void) {
    return paging_active;
}

/* Is the 4 KB PAGE containing this address entirely user-owned?
 * (4 KB granularity: a page sticking out of the region = supervisor.) */
static int page_is_user(uint32_t addr) {
    uint32_t pstart = addr & ~0xFFFu;
    uint32_t pend   = pstart + 0xFFFu;

    if (pstart >= USER_ARENA_START && pend < USER_GUARD_START)
        return 1;                       /* arena + trampoline */
    if (pstart >= USER_STACK_START && pend < USER_STACK_END)
        return 1;                       /* stack user */
    return 0;                           /* termasuk halaman guard */
}

void paging_init(void) {
    if (paging_active) return;

        /* ---- 1. Zero the directory + all tables ---- */
    for (int i = 0; i < 1024; i++) page_dir[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 1024; j++) low_pts[i][j] = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 1024; j++) fb_pts[i][j] = 0;

    /* ---- 2. Low 64 MB: 16 page directory entry + 16 tabel ---- */
    for (int i = 0; i < 16; i++) {
        page_dir[i] = ((uint32_t)(uintptr_t)low_pts[i])
                      | PTE_PRESENT | PTE_RW | PTE_USER;
        /* The PDE gets U/S=1 so user access is only limited by the PTE
         * (hardware rule: user mode needs U/S=1 at BOTH levels). */
        for (int j = 0; j < 1024; j++) {
            uint32_t addr = ((uint32_t)i << 22) | ((uint32_t)j << 12);
            low_pts[i][j] = addr | PTE_PRESENT | PTE_RW
                            | (page_is_user(addr) ? PTE_USER : 0);
        }
    }

    /* ---- 3. Framebuffer VESA ----
     * The LFB region is usually 0xFD000000 (QEMU std) - OUTSIDE the first
     * 64 MB, so without this mapping every console/game pixel would #PF
     * right after paging turns on. Size = pitch * height, rounded up per
     * page & per 4 MB table. */
    if (vesa_is_available() && vesa_get_framebuffer()) {
        uint32_t fb    = vesa_get_framebuffer();
        uint32_t bytes = (uint32_t)vesa_get_pitch()
                       * (uint32_t)vesa_get_height();
        if (bytes) {
            uint32_t start = fb & ~((4u << 20) - 1);      /* align down 4MB */
            uint32_t end   = fb + bytes;
            int tbl = 0;
            for (uint32_t a = start; a < end && tbl < 4; a += (4u << 20)) {
                int pdi = (int)(a >> 22);
                if (pdi < 16) continue;    /* already covered by the low map
                                             * (odd, but defensive - no duplicates) */
                page_dir[pdi] = ((uint32_t)(uintptr_t)fb_pts[tbl])
                                | PTE_PRESENT | PTE_RW;   /* supervisor */
                for (int j = 0; j < 1024; j++) {
                    fb_pts[tbl][j] = ((uint32_t)pdi << 22)
                                     | ((uint32_t)j << 12)
                                     | PTE_PRESENT | PTE_RW;
                }
                tbl++;
            }
        }
    }

    /* ---- 4. Aktifkan ---- */
    asm volatile("mov %0, %%cr3" : : "r"(page_dir) : "memory");

    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;                 /* CR0.PG */
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    paging_active = 1;
}
