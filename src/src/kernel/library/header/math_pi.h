#ifndef MATH_PI_H
#define MATH_PI_H

#ifdef __cplusplus
extern "C" {
#endif

#define PI 3.14159265358979323846f

// Trigonometry (angles in radians)
float sinf(float x);
float cosf(float x);
float tanf(float x);

// Konversi
float deg_to_rad(float deg);
float rad_to_deg(float rad);

#ifdef __cplusplus
}
#endif

#endif
