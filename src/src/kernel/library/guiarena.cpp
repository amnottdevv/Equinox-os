/**
 * @file guiarena.cpp
 * @brief Implementasi GUI arena (lihat guiarena.h — 0.4 Beta: arena dinamis).
 */
#include "header/guiarena.h"
#include "header/stdio.h"
#include "header/paging.h"     /* g_ram_top (di-set kernel.cpp dari multiboot) */

namespace {

struct ga_hdr {
    uint32_t magic;      /* GA_MAGIC = blok hidup */
    uint32_t size;       /* ukuran blok TERMASUK header, 8-byte aligned */
    uint32_t used;       /* 1 = terpakai, 0 = free */
    uint32_t pad;
};

const uint32_t GA_MAGIC = 0x47414955u;  /* "GAIU" */
const size_t   GA_MIN_SPLIT = sizeof(ga_hdr) + 16;

/* 0.4 Beta: maksimum tabel paging yang di-map = 32 -> 128 MB. Arena
 * tidak boleh melewati itu (akses di luar map = page fault). */
const uint32_t GA_HARD_END = 0x4000000u;   /* 0.4 Beta: 64 MB — di atasnya jadi kernel heap region 2 */

static inline uint32_t irq_save(void) {
    uint32_t flags;
    asm volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint32_t flags) {
    asm volatile("pushl %0\n\tpopfl" :: "r"(flags) : "memory");
}

static inline size_t ga_align(size_t s) {
    return (s + 7u) & ~(size_t)7u;
}

/* Walk seluruh arena: gabung free bersebelahan. */
void coalesce(uint8_t* end) {
    uint8_t* p = (uint8_t*)GUI_ARENA_START;
    while (p + sizeof(ga_hdr) <= end) {
        ga_hdr* h = (ga_hdr*)p;
        if (h->magic != GA_MAGIC) return;  /* korupsi: berhenti diam */
        if (!h->used) {
            while (p + h->size + sizeof(ga_hdr) <= end) {
                ga_hdr* n = (ga_hdr*)(p + h->size);
                if (n->magic != GA_MAGIC || n->used) break;
                h->size += n->size;
            }
        }
        p += h->size;
    }
}

} /* namespace */

/* ---------- 0.4 Beta: batas arena dinamis ----------
 * RAM kecil (< ~53 MB): arena mengecil mengikuti RAM (min 1 MB agar
 * tvgdemo kecil tetap jalan); RAM besar: mentok di 128 MB (batas
 * identity-map paging saat ini). g_ram_top di-set SEBELUM paging_init
 * oleh kernel.cpp dari multiboot mem_upper. */
extern "C" uint32_t ga_arena_end_impl(void) {
    uint32_t top = g_ram_top();                 /* di-set kernel_main */
    if (top < GUI_ARENA_START + (1u << 20))     /* butuh min 1 MB */
        top = GUI_ARENA_START + (1u << 20);
    if (top > GA_HARD_END) top = GA_HARD_END;
    return top & ~0xFFFu;                       /* page aligned */
}
uint32_t ga_arena_end(void) { return ga_arena_end_impl(); }

void* ga_malloc(size_t size) {
    uint8_t* end = (uint8_t*)ga_arena_end();
    if (size == 0 || size > (size_t)(end - (uint8_t*)GUI_ARENA_START))
        return nullptr;

    size_t need = ga_align(size + sizeof(ga_hdr));
    if (need < sizeof(ga_hdr)) return nullptr;  /* overflow wrap */

    uint32_t f = irq_save();

    /* first init: satu blok free raksasa */
    static bool inited = false;
    if (!inited) {
        ga_hdr* h = (ga_hdr*)GUI_ARENA_START;
        h->magic = GA_MAGIC;
        h->size  = (uint32_t)(end - (uint8_t*)GUI_ARENA_START);
        h->used  = 0;
        h->pad   = 0;
        inited   = true;
    }

    uint8_t* p = (uint8_t*)GUI_ARENA_START;
    void* result = nullptr;
    while (p + sizeof(ga_hdr) <= end) {
        ga_hdr* h = (ga_hdr*)p;
        if (h->magic != GA_MAGIC) break;             /* korupsi: stop */
        if (!h->used && h->size >= need) {
            /* split bila sisa cukup besar */
            if (h->size >= need + GA_MIN_SPLIT) {
                ga_hdr* n = (ga_hdr*)(p + need);
                n->magic = GA_MAGIC;
                n->size  = h->size - (uint32_t)need;
                n->used  = 0;
                n->pad   = 0;
                h->size  = (uint32_t)need;
            }
            h->used = 1;
            result = p + sizeof(ga_hdr);
            break;
        }
        p += h->size;
    }

    irq_restore(f);
    return result;
}

void ga_free(void* ptr) {
    if (!ptr) return;
    uint8_t* p = (uint8_t*)ptr - sizeof(ga_hdr);
    if (p < (uint8_t*)GUI_ARENA_START || p >= (uint8_t*)ga_arena_end()) return;

    uint32_t f = irq_save();
    ga_hdr* h = (ga_hdr*)p;
    if (h->magic == GA_MAGIC && h->used) {
        h->used = 0;
        coalesce((uint8_t*)ga_arena_end());
    }
    irq_restore(f);
}

size_t ga_used(void) {
    uint8_t* p = (uint8_t*)GUI_ARENA_START;
    uint8_t* end = (uint8_t*)ga_arena_end();
    size_t used = 0;
    while (p + sizeof(ga_hdr) <= end) {
        ga_hdr* h = (ga_hdr*)p;
        if (h->magic != GA_MAGIC) break;
        if (h->used) used += h->size;
        p += h->size;
    }
    return used;
}

size_t ga_free_space(void) {
    return (size_t)(ga_arena_end() - GUI_ARENA_START) - ga_used();
}

void ga_stats(void) {
    uint32_t end = ga_arena_end();
    printf("gui arena : 0x%x-0x%x (%u MB, DINAMIS ikut RAM terpasang)\n",
           GUI_ARENA_START, end, (end - GUI_ARENA_START) >> 20);
    printf("  used    : %u KB\n", (unsigned)(ga_used() >> 10));
    printf("  free    : %u KB\n", (unsigned)(ga_free_space() >> 10));
}
