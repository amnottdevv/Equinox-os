#ifndef RANDOM_H
#define RANDOM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Set the seed for the PRNG
void random_seed(uint32_t seed);

// Generate random 32-bit unsigned integer
uint32_t random_uint32(void);

// Generate a random integer in the range [min, max] (inclusive)
int random_int(int min, int max);

// Generate a random float in the range [0.0, 1.0)
float random_float(void);

#ifdef __cplusplus
}
#endif

#endif
