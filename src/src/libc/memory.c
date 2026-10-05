/* libc/memory.c - core memory functions (memcpy/memset/memmove/memcmp/memchr).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: none. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_MEMORY_C
#define LIBC_MEMORY_C

/* ===================== memory ===================== */
char* memcpy(char* d, char* s, int n) {
    int i;
    i = 0;
    while (i < n) { d[i] = s[i]; i = i + 1; }
    return d;
}
char* memset(char* d, int c, int n) {
    int i;
    i = 0;
    while (i < n) { d[i] = c; i = i + 1; }
    return d;
}
char* memmove(char* d, char* s, int n) {
    int i;
    if (d < s) {
        i = 0;
        while (i < n) { d[i] = s[i]; i = i + 1; }
    } else {
        i = n - 1;
        while (i >= 0) { d[i] = s[i]; i = i - 1; }
    }
    return d;
}
int memcmp(char* a, char* b, int n) {
    int i;
    i = 0;
    while (i < n) {
        if ((a[i] & 255) != (b[i] & 255)) return (a[i] & 255) - (b[i] & 255);
        i = i + 1;
    }
    return 0;
}
char* memchr(char* s, int c, int n) {
    int i;
    i = 0;
    while (i < n) {
        if (s[i] == c) return s + i;
        i = i + 1;
    }
    return 0;
}
#endif
