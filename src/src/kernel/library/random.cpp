#include "header/random.h"
#include "header/timer.h"  // for get_tick()
#include <stdint.h>

// ============================================================
//  Linear Congruential Generator (LCG)
//  Parameters from glibc: a=1103515245, c=12345, m=2^31
// ============================================================
static uint32_t random_state = 1;

void random_seed(uint32_t seed) {
    random_state = seed ? seed : 1;
}

uint32_t random_uint32(void) {
    random_state = random_state * 1103515245 + 12345;
    return random_state & 0x7FFFFFFF; // limit to 31-bit to keep it positive
}

int random_int(int min, int max) {
    if (min >= max) return min;
    uint32_t range = max - min + 1;
    return min + (random_uint32() % range);
}

float random_float(void) {
    return (float)random_uint32() / (float)0x7FFFFFFF;
}
