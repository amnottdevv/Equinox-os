/* libc/qsort.c - sorting (qsort_int/qsort_str).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: string. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_QSORT_C
#define LIBC_QSORT_C
#include "/equinox/libc/string.c"                /* dependency */

/* ===================== sort ===================== */
int __qs_ip(int* a, int lo, int hi) {
    int p; int i; int j; int t;
    p = a[(lo + hi) / 2];
    i = lo - 1;
    j = hi + 1;
    while (1) {
        i = i + 1;
        while (a[i] < p) i = i + 1;
        j = j - 1;
        while (a[j] > p) j = j - 1;
        if (i >= j) return j;
        t = a[i]; a[i] = a[j]; a[j] = t;
    }
}
void __qs_i(int* a, int lo, int hi) {
    int p;
    if (lo >= hi) return;
    p = __qs_ip(a, lo, hi);
    __qs_i(a, lo, p);
    __qs_i(a, p + 1, hi);
}
void qsort_int(int* a, int n) {
    if (n > 1) __qs_i(a, 0, n - 1);
}
int __qs_sp(char** a, int lo, int hi) {
    char* p; int i; int j; char* t;
    p = a[(lo + hi) / 2];
    i = lo - 1;
    j = hi + 1;
    while (1) {
        i = i + 1;
        while (strcmp(a[i], p) < 0) i = i + 1;
        j = j - 1;
        while (strcmp(a[j], p) > 0) j = j - 1;
        if (i >= j) return j;
        t = a[i]; a[i] = a[j]; a[j] = t;
    }
}
void __qs_s(char** a, int lo, int hi) {
    int p;
    if (lo >= hi) return;
    p = __qs_sp(a, lo, hi);
    __qs_s(a, lo, p);
    __qs_s(a, p + 1, hi);
}
void qsort_str(char** a, int n) {
    if (n > 1) __qs_s(a, 0, n - 1);
}
#endif
