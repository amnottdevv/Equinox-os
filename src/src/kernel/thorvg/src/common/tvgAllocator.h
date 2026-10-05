#ifndef _TVG_ALLOCATOR_H_
#define _TVG_ALLOCATOR_H_

/* tvgPort — tvgAllocator.h versi kernel Equinox.
 *
 * Kernel hanya punya malloc()/free() (malloc.cpp, tanpa realloc/calloc).
 * ThorVG butuh realloc (tvgArray/tvgStr) dan calloc.
 * Solusi: wrapper dengan size-header 8 byte di depan blok kernel:
 *   [size_t len][ data ... ]
 * realloc = malloc baru + copy min(lama, baru) + free lama.
 * Semua alokasi ThorVG kecil-kecil (objek shape/fill/rle) — heap kernel
 * 3 MB cukup; buffer SURFACE BESAR tetap lewat ga_malloc (guiarena).
 */
#include <stdlib.h>   /* malloc/free kernel */
#include <string.h>   /* memcpy/memset kernel */

namespace tvg
{
    struct TvgAllocHdr {
        size_t len;   /* panjang data (tanpa header) */
    };

    inline void* kalloc_raw(size_t size) {
        if (size == 0) size = 1;
        TvgAllocHdr* h = (TvgAllocHdr*)malloc(size + sizeof(TvgAllocHdr));
        if (!h) return nullptr;
        h->len = size;
        return (void*)(h + 1);
    }

    inline void kfree_raw(void* p) {
        if (!p) return;
        free((void*)((TvgAllocHdr*)p - 1));
    }

    inline size_t ksize_raw(const void* p) {
        if (!p) return 0;
        return ((const TvgAllocHdr*)p - 1)->len;
    }

    template<typename T = void>
    static inline T* malloc(size_t size)
    {
        return static_cast<T*>(kalloc_raw(size));
    }

    template<typename T = void>
    static inline T* calloc(size_t nmem, size_t size)
    {
        size_t total = nmem * size;
        T* p = static_cast<T*>(kalloc_raw(total));
        if (p) memset(p, 0, total);
        return p;
    }

    template<typename T = void>
    static inline T* realloc(T* ptr, size_t size)
    {
        if (!ptr) return static_cast<T*>(kalloc_raw(size));
        if (size == 0) { kfree_raw(ptr); return nullptr; }
        size_t old = ksize_raw(ptr);
        T* p = static_cast<T*>(kalloc_raw(size));
        if (!p) return nullptr;         /* mirip glibc: blok lama utuh */
        memcpy(p, ptr, old < size ? old : size);
        kfree_raw(ptr);
        return p;
    }

    template<typename T = void>
    static inline void free(T* ptr)
    {
        kfree_raw((void*)ptr);
    }
}

#endif //_TVG_ALLOCATOR_H_
