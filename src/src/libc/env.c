/* libc/env.c - runtime environment (abort/time/getenv/abs/rand/srand).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: none. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_ENV_C
#define LIBC_ENV_C

/* ===================== misc ===================== */
void abort() {
    print("abort() called\n");
    exit(134);
}
int time() {
    return gettick() / 100;
}
char* getenv(char* name) {
    if (!name) return 0;
    return 0;
}
int abs(int v) {
    if (v < 0) return -v;
    return v;
}
int __rand_seed;
int srand(int s) {
    __rand_seed = s;
    return 0;
}
int rand() {
    __rand_seed = 1664525 * __rand_seed + 1013904223;
    return (__rand_seed >> 8) & 32767;
}
#endif
