/* compat/time.h — freestanding time shim: time() = seconds since boot
 * (100 Hz kernel tick / 100). DOOM only uses it for savegame stamps. */
#ifndef MORPH_COMPAT_TIME_H
#define MORPH_COMPAT_TIME_H

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 100

time_t time(time_t* t);
double difftime(time_t a, time_t b);
clock_t clock(void);

#endif
