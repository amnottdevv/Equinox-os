/* libc/strx.c - allocating string helpers (strdup/strtok/strspn/strcspn/strcasecmp).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: memory, string, ctype. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_STRX_C
#define LIBC_STRX_C
#include "/equinox/libc/memory.c"                /* dependency */
#include "/equinox/libc/string.c"                /* dependency */
#include "/equinox/libc/ctype.c"                 /* dependency */

/* ================= string extras (FR-19) ================= */
char* strdup(char* s) {
    int n; char* d;
    n = strlen(s) + 1;
    d = malloc(n);
    if (!d) return 0;
    memcpy(d, s, n);
    return d;
}
int strspn(char* s, char* set) {
    int i; int j; int ok;
    i = 0;
    while (s[i]) {
        ok = 0;
        j = 0;
        while (set[j]) {
            if (s[i] == set[j]) { ok = 1; break; }
            j = j + 1;
        }
        if (!ok) break;
        i = i + 1;
    }
    return i;
}
int strcspn(char* s, char* set) {
    int i; int j; int hit;
    i = 0;
    while (s[i]) {
        hit = 0;
        j = 0;
        while (set[j]) {
            if (s[i] == set[j]) { hit = 1; break; }
            j = j + 1;
        }
        if (hit) break;
        i = i + 1;
    }
    return i;
}
int strcasecmp(char* a, char* b) {
    int i; int ca; int cb;
    i = 0;
    while (1) {
        ca = tolower(a[i]);
        cb = tolower(b[i]);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
        i = i + 1;
    }
}
char* __sttok_cur;
char* strtok(char* s, char* sep) {
    char* p; char* q;
    if (s) __sttok_cur = s;
    if (!__sttok_cur) return 0;
    p = __sttok_cur + strspn(__sttok_cur, sep);
    if (!p[0]) { __sttok_cur = 0; return 0; }
    q = p + strcspn(p, sep);
    if (q[0]) { q[0] = 0; __sttok_cur = q + 1; }
    else __sttok_cur = 0;
    return p;
}
#endif
