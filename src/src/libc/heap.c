/* libc/heap.c - user-space heap (malloc/free/calloc/realloc over __arena_alloc).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: memory. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_HEAP_C
#define LIBC_HEAP_C
#include "/equinox/libc/memory.c"                /* dependency */

/* ============ user-space heap (malloc/free/calloc/realloc) ============
   The kernel provides raw blocks via __arena_alloc (MRP arena).
   The prelude manages its own free-list: block = [size ints][flag],
   flag 0 = free, 0x55555555 = in use. First-fit + split + coalesce
   on free. Maximum 8 chunks x 64KB (the arena is reset on exit). */
int __hchunk_addr[8];
int __hchunk_ints[8];
int __hchunk_count;
int* __hraw(int units) {
    int ci; int i; int sz; int* p;
    ci = 0;
    while (ci < __hchunk_count) {
        p = __hchunk_addr[ci];
        i = 0;
        while (i < __hchunk_ints[ci] - 1) {
            sz = p[i];
            if (p[i + 1] == 0 && sz >= units) {
                if (sz >= units + 4) {
                    p[i + units + 2] = sz - units - 2;
                    p[i + units + 3] = 0;
                } else {
                    units = sz;
                }
                p[i + 1] = 1431655765;
                return p + i + 2;
            }
            i = i + sz + 2;
        }
        ci = ci + 1;
    }
    return 0;
}
int* malloc(int n) {
    int units; int* c; int k;
    if (n <= 0) n = 1;
    units = (n + 3) / 4;
    c = __hraw(units);
    if (c) return c;
    k = 16384;
    if (units + 16 > k) k = units + 16;
    if (__hchunk_count >= 8) return 0;
    c = __arena_alloc(k * 4);
    if (!c) return 0;
    c[0] = k - 2;
    c[1] = 0;
    __hchunk_addr[__hchunk_count] = c;
    __hchunk_ints[__hchunk_count] = k;
    __hchunk_count = __hchunk_count + 1;
    return __hraw(units);
}
void free(int* p) {
    int* h; int ci; int* c; int i; int sz;
    if (!p) return;
    h = p - 2;
    if (h[1] != 1431655765) {
        print("free(): invalid pointer, ignored\n");
        return;
    }
    h[1] = 0;
    ci = 0;
    while (ci < __hchunk_count) {
        c = __hchunk_addr[ci];
        if (h >= c && h < c + __hchunk_ints[ci]) {
            i = 0;
            while (i < __hchunk_ints[ci] - 1) {
                sz = c[i];
                if (c[i + 1] == 0) {
                    while (i + sz + 2 < __hchunk_ints[ci] && c[i + sz + 3] == 0) {
                        sz = sz + c[i + sz + 2] + 2;
                        c[i] = sz;
                    }
                }
                i = i + sz + 2;
            }
            return;
        }
        ci = ci + 1;
    }
}
int* calloc(int num, int sz) {
    int* p;
    p = malloc(num * sz);
    if (p) memset(p, 0, num * sz);
    return p;
}
int* realloc(int* p, int nsz) {
    int* np; int osz;
    if (!p) return malloc(nsz);
    if (nsz <= 0) { free(p); return 0; }
    np = malloc(nsz);
    if (!np) return 0;
    osz = (p - 2)[0] * 4;
    if (osz > nsz) osz = nsz;
    memcpy(np, p, osz);
    free(p);
    return np;
}
#endif
