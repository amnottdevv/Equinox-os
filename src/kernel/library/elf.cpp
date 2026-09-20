/*
 * ============================================================================
 *  elf.cpp — ELF32 (i386, ET_EXEC) loader (v0.3, FR-07)
 * ----------------------------------------------------------------------------
 *  See header/elf.h for the layout contract. Everything here runs in
 *  the SPAWNER's context (CPL 0); the actual byte copies go through
 *  task_demand_fill() which switches CR3 to the target task's
 *  directory with IRQs off (the generic #PF demand path must never
 *  run against a directory that is not the current task's).
 *
 *  Only what a freestanding static executable needs is parsed:
 *    e_ident   : class == 1 (ELF32), data == 1 (little-endian)
 *    e_type    : 2 (ET_EXEC — no relocations, fixed vaddrs)
 *    e_machine : 3 (EM_386)
 *    e_entry   : the entry VMA (pushed to user3_launch4)
 *    program headers: PT_LOAD (1) segments only — notes/stack entries
 *    are ignored.
 *  Segments are copied page-wise into demand pages; the p_memsz tail
 *  beyond p_filesz (.bss) is NOT written — it faults in zero-filled.
 * ============================================================================
 */

#include "header/elf.h"
#include "header/task.h"
#include "header/stdio.h"
#include <stdint.h>
#include <stddef.h>

/* ---- minimal ELF32 structures (no external headers) ---- */
typedef struct {
    uint8_t  e_ident[16];   /* 0x00 magic + class/data/version/osabi  */
    uint16_t e_type;        /* 0x10 2 = ET_EXEC                        */
    uint16_t e_machine;     /* 0x12 3 = EM_386                         */
    uint32_t e_version;     /* 0x14                                    */
    uint32_t e_entry;       /* 0x18                                    */
    uint32_t e_phoff;       /* 0x1c                                    */
    uint32_t e_shoff;       /* 0x20 (unused)                           */
    uint32_t e_flags;       /* 0x24 (unused)                           */
    uint16_t e_ehsize;      /* 0x28                                    */
    uint16_t e_phentsize;   /* 0x2a                                    */
    uint16_t e_phnum;       /* 0x2c                                    */
    uint16_t e_shentsize;   /* 0x2e (unused)                           */
    uint16_t e_shnum;       /* 0x30 (unused)                           */
    uint16_t e_shstrndx;    /* 0x32 (unused)                           */
} elf32_ehdr_t;

typedef struct {
    uint32_t p_type;        /* 0x00 1 = PT_LOAD                        */
    uint32_t p_offset;      /* 0x04 file offset                        */
    uint32_t p_vaddr;       /* 0x08 target VMA                         */
    uint32_t p_paddr;       /* 0x0c (unused)                           */
    uint32_t p_filesz;      /* 0x10 bytes copied from the file          */
    uint32_t p_memsz;       /* 0x14 total image bytes (bss incl.)      */
    uint32_t p_flags;       /* 0x18 (unused — pages are RW)            */
    uint32_t p_align;       /* 0x1c (unused)                           */
} elf32_phdr_t;

#define PT_LOAD   1u

static char g_elf_err[64] = "ok";

const char* elf_last_error(void) { return g_elf_err; }

static void elf_fail(const char* msg) {
    /* bounded copy — every message below is well under 64 bytes */
    uint32_t i = 0;
    while (msg[i] && i < sizeof(g_elf_err) - 1) {
        g_elf_err[i] = msg[i];
        i++;
    }
    g_elf_err[i] = '\0';
}

int elf_is_elf(const uint8_t* data, uint32_t size) {
    if (!data || size < 20) return 0;
    return data[0] == 0x7F && data[1] == 'E' && data[2] == 'L'
        && data[3] == 'F' ? 1 : 0;
}

/* Parse + validate WITHOUT touching a task. Returns 0 on success. */
int elf_check(const uint8_t* data, uint32_t size, uint32_t* entry,
              uint32_t* lo, uint32_t* span) {
    if (!elf_is_elf(data, size)) { elf_fail("not an ELF file"); return -1; }

    const elf32_ehdr_t* eh = (const elf32_ehdr_t*)(const void*)data;
    if (eh->e_ident[4] != 1) { elf_fail("not ELF32 (class != 1)"); return -1; }
    if (eh->e_ident[5] != 1) { elf_fail("not little-endian"); return -1; }
    if (eh->e_type != 2)     { elf_fail("not ET_EXEC (PIE/reloc)"); return -1; }
    if (eh->e_machine != 3)  { elf_fail("not EM_386"); return -1; }
    if (eh->e_phnum == 0 || eh->e_phnum > 16) {
        elf_fail("bad program header count"); return -1;
    }
    if (eh->e_phentsize < sizeof(elf32_phdr_t)) {
        elf_fail("phentsize too small"); return -1;
    }
    uint32_t ph_end = eh->e_phoff + (uint32_t)eh->e_phnum * eh->e_phentsize;
    if (eh->e_phoff >= size || ph_end > size || ph_end < eh->e_phoff) {
        elf_fail("phdr table out of file"); return -1;
    }

    /* Walk the PT_LOADs: validate ranges + compute the image span. */
    uint32_t img_lo = 0xFFFFFFFFu, img_hi = 0;
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const elf32_phdr_t* ph = (const elf32_phdr_t*)
            (const void*)(data + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        if (ph->p_offset >= size || ph->p_filesz > size - ph->p_offset) {
            elf_fail("segment data out of file"); return -1;
        }
        if (ph->p_filesz > ph->p_memsz) {
            elf_fail("filesz > memsz"); return -1;
        }
        uint32_t seg_lo = ph->p_vaddr & ~0xFFFu;
        uint32_t seg_hi = (ph->p_vaddr + ph->p_memsz + 0xFFFu) & ~0xFFFu;
        if (seg_lo < ELF_IMG_MIN || seg_hi > ELF_IMG_MAX) {
            elf_fail("segment outside the image window"); return -1;
        }
        if (seg_lo < img_lo) img_lo = seg_lo;
        if (seg_hi > img_hi) img_hi = seg_hi;
    }
    if (img_lo == 0xFFFFFFFFu) { elf_fail("no PT_LOAD segments"); return -1; }

    if (eh->e_entry < ELF_IMG_MIN || eh->e_entry >= ELF_IMG_MAX) {
        elf_fail("entry outside the image window"); return -1;
    }

    if (entry) *entry = eh->e_entry;
    if (lo)    *lo    = img_lo;
    if (span)  *span = img_hi - img_lo;
    elf_fail("ok");
    return 0;
}

/* Load into the task (map_demand must already be done). */
int elf_load(struct Task* t, const uint8_t* data, uint32_t size) {
    if (!t || !data) return -1;

    uint32_t entry, lo, span;
    if (elf_check(data, size, &entry, &lo, &span) != 0) {
        printf("elf: %s\n", elf_last_error());
        return -1;
    }

    /* Reserve the whole image span as demand pages (one reservation
     * covers overlapping/adjacent segments — reserve skips present
     * PTEs, so calling it per-segment would also be fine). */
    task_demand_reserve(t, lo, span);

    const elf32_ehdr_t* eh = (const elf32_ehdr_t*)(const void*)data;
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const elf32_phdr_t* ph = (const elf32_phdr_t*)
            (const void*)(data + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_filesz == 0) continue;      /* pure bss: zero on fault */

        /* Copy the file-backed bytes. The LAST partial page's tail
         * stays zero (demand pages are zero-filled at map time), so
         * filesz..memsz within the same page is correct .bss. */
        task_demand_fill(t, ph->p_vaddr, data + ph->p_offset, ph->p_filesz);
    }
    return 0;
}
