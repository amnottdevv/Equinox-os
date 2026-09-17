#ifndef LIBC_H
#define LIBC_H

/*
 * ============================================================================
 *  libc.h — Standard C library: conversion, sort, environment, time
 * ----------------------------------------------------------------------------
 *  Completes the kernel's libc map (v10.8). The other families live in:
 *    malloc.h    : malloc / calloc / realloc / free + heap debug
 *    libstring.h : mem* + str* + sprintf/snprintf/sscanf
 *    stdio.h     : printf / console
 *    itoa_atoi.h : atoi / atol / itoa / utoa
 *    stdlib.h    : combined declarations (includes the files above)
 *
 *  NOTE on atof(): the implementation file (libc.cpp) is compiled with
 *  -mgeneral-regs-only — the double return travels via the EAX:EDX
 *  register pair (soft-float ABI), with no x87 instructions at all.
 * ============================================================================
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Conversions with endptr + base (C89 semantics) ----
 * base 0 = autodetect (0x -> 16, leading 0 -> 8, otherwise 10).
 * endptr receives the position of the character AFTER the last
 * digit; *endptr == nptr means no digits were converted.
 * Overflow is clamped to LONG_MAX/LONG_MIN (strtol) or ULONG_MAX
 * (strtoul). */
long strtol(const char* nptr, char** endptr, int base);
unsigned long strtoul(const char* nptr, char** endptr, int base);

/* atof — decimal -> IEEE-754 double conversion, pure software
 * (bit manipulation, no FPU instructions). Supports sign, decimal
 * point, and e/E exponent. Exact precision for common cases;
 * inputs with 17+ significant digits can be off by at most 1 ulp. */
double atof(const char* nptr);

/* ---- Sort / search ----
 * qsort: iterative quicksort (explicit stack, safe from stack overflow)
 * + insertion sort for small partitions. Comparator: <0, 0, >0.
 * bsearch: binary search over a sorted array, returns a pointer to the
 * element or NULL. */
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*));

/* ---- Environment ----
 * Equinox OS has no environment block yet: getenv ALWAYS returns NULL
 * (the standard "variable does not exist" answer, not an error). */
const char* getenv(const char* name);

/* ---- Time ----
 * time() = UPTIME in seconds (get_tick()/100), NOT UNIX epoch —
 * the RTC epoch is not implemented yet. t != NULL -> *t = same value. */
long time(long* t);

#ifdef __cplusplus
}
#endif

#endif /* LIBC_H */
