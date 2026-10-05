/**
 * @file math2.cpp
 * @brief Implementasi math float/double presisi via x87 inline assembly.
 *
 * Dipakai oleh port ThorVG (kernel/thorvg) dan desktop EquiX.
 * - fsqrt/fpatan: akurat penuh (hardware).
 * - fsin/fcos: akurat dalam |x| < 2^63 (cukup untuk UI/gradient).
 * - exp/log/pow: dekomposisi 2^z = 2^n * 2^f via f2xm1 + fscale.
 * - floor/ceil/round/trunc: cast int64 + koreksi arah.
 *
 * file ini TIDAK boleh masuk FPU_SENSITIVE (butuh x87).
 */
#include <math2.h>

extern "C" {

/* ================= double: inti x87 ================= */

double sqrt(double x) {
    double r;
    __asm__ ("fsqrt" : "=t" (r) : "0" (x));
    return r;
}

double sin(double x) {
    double r;
    __asm__ ("fsin" : "=t" (r) : "0" (x));
    return r;
}

double cos(double x) {
    double r;
    __asm__ ("fcos" : "=t" (r) : "0" (x));
    return r;
}

double tan(double x) {
    double r;
    /* fptan: st0 <- 1.0, st1 <- tan(x); buang 1.0-nya */
    __asm__ ("fptan\n\tfstp %%st(0)" : "=t" (r) : "0" (x));
    return r;
}

double atan2(double y, double x) {
    double r;
    /* fpatan: st1 <- atan(st1/st0), pop. in: st0=x, st1=y */
    __asm__ ("fpatan" : "=t" (r) : "u" (y), "0" (x));
    return r;
}

double fmod(double x, double y) {
    double r;
    /* fprem berulang sampai C2=0 (reduksi parsial x87) */
    __asm__ (
        "1: fprem\n\t"
        "fstsw %%ax\n\t"
        "sahf\n\t"
        "jp 1b\n"
        : "=t" (r)
        : "0" (x), "u" (y)
        : "ax"
    );
    return r;
}

double exp(double x) {
    double r;
    /* z = x * log2(e); n = rndint(z); f = z - n; exp = 2^f * 2^n */
    __asm__ (
        "fldl2e\n\t"
        "fmulp\n\t"          /* st0 = z                    */
        "fld %%st(0)\n\t"
        "frndint\n\t"        /* st0 = n, st1 = z           */
        "fxch\n\t"           /* st0 = z, st1 = n           */
        "fsub %%st(1)\n\t"   /* st0 = f = z - n, st1 = n   */
        "f2xm1\n\t"          /* st0 = 2^f - 1              */
        "fld1\n\t"
        "faddp\n\t"          /* st0 = 2^f, st1 = n         */
        "fscale\n\t"         /* st0 = 2^f * 2^n            */
        "fstp %%st(1)\n"     /* buang n                    */
        : "=t" (r)
        : "0" (x)
    );
    return r;
}

double log(double x) {
    double r;
    /* fyl2x: st1 = st1 * log2(st0), pop. in: st0 = x, st1 = ln(2) */
    __asm__ ("fyl2x" : "=t" (r) : "u" (0.69314718055994530942), "0" (x));
    return r;
}

double pow(double x, double y) {
    double r;
    /* x^y = 2^(y * log2(x)); hasil fyl2x = z, lalu 2^z seperti exp() */
    __asm__ (
        "fyl2x\n\t"          /* st0 = z = y*log2(x)        */
        "fld %%st(0)\n\t"
        "frndint\n\t"        /* st0 = n, st1 = z           */
        "fxch\n\t"
        "fsub %%st(1)\n\t"   /* st0 = f, st1 = n           */
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fscale\n\t"
        "fstp %%st(1)\n"
        : "=t" (r)
        : "u" (y), "0" (x)
    );
    return r;
}

double nearbyint(double x) {
    /* default x87 rounding mode = nearest-even, persis semantic nearbyint */
    double r;
    __asm__ ("frndint" : "=t" (r) : "0" (x));
    return r;
}
double fabs(double x)   { return __builtin_fabs(x); }
double copysign(double x, double y) { return __builtin_copysign(x, y); }
double hypot(double x, double y) { return sqrt(x * x + y * y); }

double trunc(double x)  { return (double)(int64_t)x; }
double floor(double x) {
    double t = (double)(int64_t)x;      /* trunc menuju nol */
    if (x < 0.0 && x != t) t -= 1.0;
    return t;
}
double ceil(double x) {
    double t = (double)(int64_t)x;
    if (x > 0.0 && x != t) t += 1.0;
    return t;
}
double round(double x) {
    /* half away from zero, sesuai C round() */
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

/* ================= float: wrapper ke double ================= */

float  sqrtf(float x)     { return (float)sqrt((double)x); }
float  sinf(float x)      { return (float)sin((double)x); }
float  cosf(float x)      { return (float)cos((double)x); }
float  tanf(float x)      { return (float)tan((double)x); }
float  atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float  fmodf(float x, float y)  { return (float)fmod((double)x, (double)y); }
float  expf(float x)      { return (float)exp((double)x); }
float  logf(float x)      { return (float)log((double)x); }
float  powf(float x, float y)   { return (float)pow((double)x, (double)y); }
float  nearbyintf(float x) { return (float)nearbyint((double)x); }
float  fabsf(float x)     { return __builtin_fabsf(x); }
float  copysignf(float x, float y) { return __builtin_copysignf(x, y); }
float  hypotf(float x, float y)   { return (float)hypot((double)x, (double)y); }
float  floorf(float x)    { return (float)floor((double)x); }
float  ceilf(float x)     { return (float)ceil((double)x); }
float  roundf(float x)    { return (float)round((double)x); }
float  truncf(float x)    { return (float)trunc((double)x); }

} /* extern "C" */
