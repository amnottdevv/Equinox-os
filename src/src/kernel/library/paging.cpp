/*
 * ============================================================================
 *  paging.cpp — Identity map RAM DINAMIS + LFB, split supervisor/user
 * ----------------------------------------------------------------------------
 *  The data structures live in the kernel .bss (NOBITS - zeroed by start.asm):
 *    page_dir[1024]        4 KB   directory
 *    low_pts[32][1024]   128 KB   up to 32 tables = 128 MB (0.4 Beta)
 *    fb_pts[4][1024]      16 KB   4 spare tables for the LFB region
 *
 *  0.4 Beta "RAM NOT FULLY USED" FIX: the table count is no longer
 *  hardcoded at 16 (64 MB). kernel.cpp reads the multiboot mem_upper ->
 *  paging_set_ram_top() BEFORE paging_init(); mapped tables =
 *  ceil(ram_top/4MB) clamped to 16..32. RAM 96 MB -> 24 tables; RAM
 *  128 MB -> 32 tables; RAM 64 MB -> 16 tables (same as the old
 *  behaviour).
 *  The region ABOVE ram_top is deliberately left UNMAPPED (access = a
 *  clean #PF instead of a silent write into RAM that does not exist).
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
static uint32_t low_pts[32][1024] __attribute__((aligned(4096)));
static uint32_t fb_pts[4][1024]   __attribute__((aligned(4096)));

static int paging_active = 0;

/* ---- 0.4 Beta: jendela MMIO PCI (lihat paging_map_mmio) ----
 * Tabel cadangan: sisa fb_pts (yang tidak dipakai LFB) dulu, lalu
 * sisa low_pts di atas RAM nyata (RAM 64 MB -> tabel 16..31 bebas).
 * Dipakai supaya menambah BAR PCI (mis. e1000 BAR0 @ 0xFEBC0000)
 * TIDAK menggusur tabel mana pun dan tidak menambah .bss. */
static int g_ram_tables = 16;    /* tabel yang dipakai peta RAM rendah */
static int g_fb_used    = 0;     /* fb_pts yang dipakai peta LFB       */

/* ---- 0.4 Beta: dynamic RAM top ----
 * Default 64 MB when kernel.cpp has no chance to set it (non-multiboot
 * debug boot). Min 16 MB so the legacy kernel structures are always
 * mapped; max 32 tables = 128 MB (low_pts capacity).
 * g_ram_top() is also used by guiarena (dynamic GUI arena, not 10 MB). */
static uint32_t ram_top_bytes = 64u * 1024u * 1024u;

void paging_set_ram_top(uint32_t top_bytes) {
    if (top_bytes < 16u * 1024u * 1024u)
        top_bytes = 16u * 1024u * 1024u;
    if (top_bytes > 32u * (4u << 20))          /* 128 MB: kapasitas low_pts */
        top_bytes = 32u * (4u << 20);
    ram_top_bytes = top_bytes & ~0xFFFu;       /* page aligned */
}

uint32_t g_ram_top(void) {
    return ram_top_bytes;
}

int paging_is_active(void) {
    return paging_active;
}

/* Phase A: the kernel page-directory template — used by shell tasks.
 * User tasks get their own directory (task.cpp) with a U/S window. */
uint32_t* paging_kernel_dir(void) {
    return page_dir;
}

/* Phase A: the template is now ALL supervisor — the user arena is
 * mapped U/S through the PER-TASK page directories (own physical
 * chunk window, see task.cpp). The template no longer uses
 * page_is_user. */

void paging_init(void) {
    if (paging_active) return;

        /* ---- 0. 0.4 Beta: jumlah tabel = ikut RAM nyata ----
         * 16 tabel (64 MB) = batas bawah supaya semua wilayah kernel
         * lama selalu ter-map (heap 3 MB, arena MRP, staging 12 MB).
         * Tabel tambahan (17..32) hanya di-map bila RAM-nya ada. */
    uint32_t ram_tables = (ram_top_bytes + (4u << 20) - 1) >> 22;
    if (ram_tables < 16) ram_tables = 16;
    if (ram_tables > 32) ram_tables = 32;
    g_ram_tables = (int)ram_tables;   /* paging_map_mmio: tabel sisa */

        /* ---- 1. Zero the directory + all tables ---- */
    for (int i = 0; i < 1024; i++) page_dir[i] = 0;
    for (uint32_t i = 0; i < ram_tables; i++)
        for (int j = 0; j < 1024; j++) low_pts[i][j] = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 1024; j++) fb_pts[i][j] = 0;

    /* ---- 2. Low RAM: ram_tables directory entries + tables ----
     * Semua supervisor (U/S=0). PDE 1..9 nanti di-overwrite per-task
     * oleh task_build_dir (user window). PDE 13+ (0x3400000+, GUI
     * arena) di-share ke task sebagai SUPERVISOR: task user tidak
     * bisa menyentuh surface desktop. */
    for (uint32_t i = 0; i < ram_tables; i++) {
        page_dir[i] = ((uint32_t)(uintptr_t)low_pts[i])
                      | PTE_PRESENT | PTE_RW;
        for (int j = 0; j < 1024; j++) {
            uint32_t addr = (i << 22) | ((uint32_t)j << 12);
            low_pts[i][j] = addr | PTE_PRESENT | PTE_RW;
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
                if (pdi >= 0 && pdi < (int)ram_tables)
                    continue;    /* already covered by the low RAM map
                                  * (defensif - jangan duplikat PDE) */
                page_dir[pdi] = ((uint32_t)(uintptr_t)fb_pts[tbl])
                                | PTE_PRESENT | PTE_RW;   /* supervisor */
                for (int j = 0; j < 1024; j++) {
                    fb_pts[tbl][j] = ((uint32_t)pdi << 22)
                                     | ((uint32_t)j << 12)
                                     | PTE_PRESENT | PTE_RW;
                }
                tbl++;
            }
            g_fb_used = tbl;           /* sisanya bebas untuk MMIO */
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

/* ============================================================
 *  0.4 Beta — paging_map_mmio(): identity-map jendela MMIO PCI
 * ------------------------------------------------------------
 *  paging_init hanya memetakan RAM rendah (atas mem_upper) dan
 *  jendela LFB. PCI hole ada DI LUAS peta itu — mis. e1000 BAR0
 *  = 0xFEBC0000 (QEMU) — jadi driver mana pun yang menyentuh
 *  BAR-nya langsung #PF. Dipanggil pci_init() untuk tiap MEM BAR:
 *  satu tabel cadangan per jendela 4 MB, alamat identik +
 *  supervisor, persis seperti peta RAM rendah.
 * ============================================================ */
static uint32_t* paging_spare_table(void) {
    if (g_fb_used < 4)  return fb_pts[g_fb_used++];        /* sisa LFB */
    if (g_ram_tables < 32) return low_pts[g_ram_tables++]; /* sisa RAM  */
    return 0;
}

int paging_map_mmio(uint32_t base, uint32_t len) {
    if (!len) return -1;
    if (!paging_active) return -1;      /* page_dir belum berlaku */
    uint32_t start = base & ~((4u << 20) - 1);
    uint32_t end   = base + len;
    if (end < base) return -1;          /* overflow */
    for (uint32_t a = start; a < end; a += (4u << 20)) {
        int pdi = (int)(a >> 22);
        if (pdi < 0 || pdi > 1023) continue;
        if (page_dir[pdi] & PTE_PRESENT) continue;   /* sudah ter-map */
        uint32_t* t = paging_spare_table();
        if (!t) return -1;              /* pool habis */
        for (int j = 0; j < 1024; j++)
            t[j] = ((uint32_t)pdi << 22) | ((uint32_t)j << 12)
                   | PTE_PRESENT | PTE_RW;
        page_dir[pdi] = ((uint32_t)(uintptr_t)t) | PTE_PRESENT | PTE_RW;
    }
    asm volatile("mov %0, %%cr3" : : "r"(page_dir) : "memory");
    return 0;
}
