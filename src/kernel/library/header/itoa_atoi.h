#ifndef ITOA_ATOI_H
#define ITOA_ATOI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int atoi(const char* str);
long atol(const char* str);
int hex_to_int(const char* str);
void itoa(int num, char* buffer, int base);
void utoa(uint32_t num, char* buffer, int base);
void itoa_pad(int num, char* buffer, int base, int width);
void int_to_hex(uint32_t num, char* buffer);
void test_itoa_atoi(void);

#ifdef __cplusplus
}
#endif

#endif