#include "header/math_pi.h"

// ============================================================
//  Taylor series for sin and cos
//  Accuracy is good for 7-8 digits with 6 iterations
// ============================================================

static float factorial(int n) {
    float result = 1.0f;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
}

static float power(float x, int n) {
    float result = 1.0f;
    for (int i = 0; i < n; i++) {
        result *= x;
    }
    return result;
}

float sinf(float x) {
    // Normalisasi x ke range [-PI, PI]
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;

    float result = 0.0f;
    int sign = 1;
    for (int n = 1; n <= 11; n += 2) {
        float term = power(x, n) / factorial(n);
        result += sign * term;
        sign = -sign;
    }
    return result;
}

float cosf(float x) {
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;

    float result = 0.0f;
    int sign = 1;
    for (int n = 0; n <= 10; n += 2) {
        float term = power(x, n) / factorial(n);
        result += sign * term;
        sign = -sign;
    }
    return result;
}

float tanf(float x) {
    float s = sinf(x);
    float c = cosf(x);
    if (c == 0.0f) return 0.0f; // to avoid infinity
    return s / c;
}

float deg_to_rad(float deg) {
    return deg * (PI / 180.0f);
}

float rad_to_deg(float rad) {
    return rad * (180.0f / PI);
}
