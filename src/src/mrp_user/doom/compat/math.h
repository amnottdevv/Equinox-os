/* compat/math.h — freestanding math shim. DOOM's renderer is pure
 * fixed-point; the core only *includes* <math.h> (i_input.c). We
 * provide the handful of double helpers a C89 program might reach
 * for, implemented with the FPU (initialized by the kernel since
 * v10.9: fninit + CR4.OSFXSR). */
#ifndef MORPH_COMPAT_MATH_H
#define MORPH_COMPAT_MATH_H

#define M_PI     3.14159265358979323846
#define M_PI_2   1.57079632679489661923

#ifndef __cplusplus
#define isnan(x) ((x) != (x))
#define isinf(x) ((x) != 0 && ((x) + (x)) == (x))
#endif

double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan2(double y, double x);
double pow(double b, double e);
double fmod(double a, double b);

#endif /* MORPH_COMPAT_MATH_H */
