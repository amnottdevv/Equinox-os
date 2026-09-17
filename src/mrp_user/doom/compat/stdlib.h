/* compat/stdlib.h — freestanding stdlib shim for Equinox OS. */
#ifndef MORPH_COMPAT_STDLIB_H
#define MORPH_COMPAT_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     32767

void* malloc(size_t size);
void* calloc(size_t n, size_t size);
void* realloc(void* p, size_t size);
void  free(void* p);
void  exit(int status) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));

int   atoi(const char* s);
long  atol(const char* s);
long  strtol(const char* s, char** endp, int base);
unsigned long strtoul(const char* s, char** endp, int base);

int   abs(int x);
long  labs(long x);

int   rand(void);
void  srand(unsigned seed);

void  qsort(void* base, size_t n, size_t size,
            int (*cmp)(const void*, const void*));

char* getenv(const char* name);

#endif /* MORPH_COMPAT_STDLIB_H */

/* i_system.c I_Spawn(): launching external tools is not a thing on
 * Equinox OS — returns -1 like a failed shell call. */
int system(const char* cmd);
double atof(const char* s);
