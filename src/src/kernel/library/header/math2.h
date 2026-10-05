/**
 * @file math2.h
 * @brief Math float/double presisi untuk kernel Equinox — dibutuhkan
 *        port ThorVG (TinyPiXOS) dan desktop Task 2/3.
 *
 * Implementasi x87 (fsqrt/fsin/fcos/fpatan/f2xm1/...) di math2.cpp.
 * Catatan ABI: math2.o TIDAK boleh masuk daftar FPU_SENSITIVE di makefile
 * (file itu di-compile -mgeneral-regs-only / soft-float).
 * math_pi.cpp lama (Taylor sin/cos — akurat hanya dekat 0) kini
 * memakai sinf/cosf/tanf dari sini; definisinya dihapus dari math_pi.cpp.
 */
#ifndef _MATH2_H
#define _MATH2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* konstanta (tvgStr.cpp) */
#ifndef INFINITY
#define INFINITY (__builtin_inff())
#endif
#ifndef NAN
#define NAN (__builtin_nanf(""))
#endif


/* float */
float  sqrtf(float x);
float  fabsf(float x);
float  floorf(float x);
float  ceilf(float x);
float  roundf(float x);
float  truncf(float x);
float  fmodf(float x, float y);
float  powf(float x, float y);
float  expf(float x);
float  logf(float x);
float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  atan2f(float y, float x);
float  hypotf(float x, float y);
float  copysignf(float x, float y);
float  nearbyintf(float x);

/* double */
double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double pow(double x, double y);
double exp(double x);
double log(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan2(double y, double x);
double nearbyint(double x);
double hypot(double x, double y);
double copysign(double x, double y);

#ifdef __cplusplus
} /* extern "C" */

/* Predikat float via manipulasi bit — pengganti std::isfinite / isinf /
 * isnan (tanpa <cmath>, freestanding). Overload float + double. */
static inline int __m2_fclass(float v) {
    uint32_t u;
    __builtin_memcpy(&u, &v, 4);
    uint32_t e = u & 0x7F800000u;
    uint32_t m = u & 0x007FFFFFu;
    if (e == 0x7F800000u) return m ? 1 /*NaN*/ : 2 /*Inf*/;
    return 0 /*finite*/;
}
static inline int __m2_dclass(double v) {
    uint64_t u;
    __builtin_memcpy(&u, &v, 8);
    uint64_t e = u & 0x7FF0000000000000ull;
    uint64_t m = u & 0x000FFFFFFFFFFFFFull;
    if (e == 0x7FF0000000000000ull) return m ? 1 : 2;
    return 0;
}
static inline bool isnan(float v)  { return __m2_fclass(v) == 1; }
static inline bool isnan(double v) { return __m2_dclass(v) == 1; }
static inline bool isinf(float v)  { return __m2_fclass(v) == 2; }
static inline bool isinf(double v) { return __m2_dclass(v) == 2; }
static inline bool isfinite(float v)  { return __m2_fclass(v) == 0; }
static inline bool isfinite(double v) { return __m2_dclass(v) == 0; }
#endif /* __cplusplus */

#endif /* _MATH2_H */
