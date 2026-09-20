#ifndef MALLOC_H
#define MALLOC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t new_size);
void free(void* ptr);

// Debug
void malloc_stats(void);

// --- Extra heap info accessors ---
uint32_t get_heap_used(void);
uint32_t get_heap_total(void);

// --- Extra diagnostics (free-list allocator) ---
// Number of blocks currently free (an indication of external fragmentation)
uint32_t get_heap_free_blocks(void);
// Size of the largest free block available (allocations larger than this will fail)
uint32_t get_heap_largest_free(void);
// Validate heap integrity (checks the magic canary of every block). 1 = OK, 0 = corrupt.
int heap_check_integrity(void);

// ==================== Separate region for .mrp programs ====================
// The .mrp loader does NOT use the kernel heap above (so that user
// programs cannot corrupt/exhaust the kernel's heap & vice versa).
// This region has its own free-list with a separate API.
void  mrp_heap_init(void);
void* mrp_alloc(size_t size);
int   mrp_free(void* ptr);    // v0.3 FR-03: free ONE block (0 = ok / -1 = not an arena pointer)
void  mrp_free_all(void); // release ALL program allocations (called when the program exits)
uint32_t get_mrp_heap_used(void);
uint32_t get_mrp_heap_total(void);

#ifdef __cplusplus
}
#endif

#endif