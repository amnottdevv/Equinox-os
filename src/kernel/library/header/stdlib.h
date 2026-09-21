#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>
#include <stdint.h>

/* Full libc pieces live in their own headers (v10.8):
 *   malloc.h  - heap,  libstring.h - memory, string, snprintf,
 *   libc.h    - strtol, strtoul, atof, qsort, bsearch, getenv, time,
 *   itoa_atoi.h - atoi, atol, itoa, utoa,  stdio.h - printf, console.
 * This header gathers the C-standard NAMES in one place. */
#include "libc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Memory — declared in malloc.h */
void* malloc(size_t size);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t new_size);
void  free(void* ptr);

/* Conversion — declared in itoa_atoi.h */
int   atoi(const char* str);
long  atol(const char* str);

/* Numeric */
int   abs(int n);
long  labs(long n);

/* Random */
int   rand(void);
void  srand(unsigned int seed);

/* Div */
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
div_t  div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);

/* Misc */
void  abort(void);
void  exit(int status);
int   atexit(void (*func)(void));

#ifdef __cplusplus
}
#endif

#endif /* _STDLIB_H */
