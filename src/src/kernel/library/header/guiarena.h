/**
 * @file guiarena.h
 * @brief GUI arena — large-block allocator for the EquiX desktop
 *        (Task 1/2/3).
 *
 * 0.4 Beta FIX "10 MB RAM": the arena is NO longer capped at a fixed
 * 10 MB. The upper bound now follows the RAM actually installed
 * (multiboot mem_upper -> g_ram_top, see kernel.cpp + paging.cpp):
 *
 *     0x3400000 .. min(g_ram_top, 0x8000000)   (max 128 MB mapped)
 *
 *   RAM 64 MB  -> arena ~11.5 MB   (used to be forced to 10 MB)
 *   RAM 128 MB -> arena ~76 MB     (used to be stuck at 10 MB!)
 *
 * The region sits ABOVE MODULE_STAGE (0x2800000-0x3400000, zero-copy
 * RAMFS - wad/mtcc), supervisor pages (paging.cpp maps up to RAM top,
 * max 32 tables = 128 MB). It holds the desktop surfaces (wallpaper
 * layer, window layers, backbuffer), which are far larger than the
 * 3 MB kernel heap. Small objects (ThorVG shape/fill/canvas) still go
 * through kernel malloc().
 *
 * Allocator: first-fit free list with a 16-byte header + coalescing,
 * IRQ-safe (irq_save/irq_restore) following the same discipline as the
 * rest of the kernel.
 */
#ifndef _GUIARENA_H
#define _GUIARENA_H

#include <stddef.h>
#include <stdint.h>

#define GUI_ARENA_START   0x3400000u   /* 52 MB - tepat di atas module staging */
/* 0.4 Beta: GUI arena trimmed to 12 MB ([0x3400000, 0x4000000)) — still
 * larger than the old 10 MB GUI arena — because [0x4000000,
 * 0x8000000) (64 MB) became REGION 2 of the kernel heap (malloc.cpp). */

/* Batas atas arena - DINAMIS (g_ram_top dari multiboot, maks 128 MB
 * yang diidentity-map paging.cpp). Fungsi, bukan makro: nilainya
 * baru tahu SETELAH kernel_main membaca multiboot header. */
uint32_t ga_arena_end(void);

/* return NULL bila habis/ukuran gila */
void*  ga_malloc(size_t size);
void   ga_free(void* ptr);
size_t ga_used(void);
size_t ga_free_space(void);
void   ga_stats(void);   /* print ke console kernel */

#endif /* _GUIARENA_H */
