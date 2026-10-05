#include "header/itoa_atoi.h"
#include "header/stdio.h"
#include <stdint.h>

int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    while (*str == ' ') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result * sign;
}

long atol(const char* str) {
    long result = 0;
    int sign = 1;
    while (*str == ' ') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result * sign;
}

int hex_to_int(const char* str) {
    int result = 0;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
    while (*str) {
        char c = *str;
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        result = (result << 4) | digit;
        str++;
    }
    return result;
}

void itoa(int num, char* buffer, int base) {
    if (base < 2 || base > 36) { buffer[0] = '\0'; return; }
    char temp[33];
    int i = 0;
    int is_negative = 0;
    if (num == 0) { buffer[0] = '0'; buffer[1] = '\0'; return; }
    /* Old bug: 'num = -num' for INT_MIN (-2147483648) caused a
     * signed integer overflow (UB) and the value stayed negative, so
     * while (num > 0) exited immediately -> empty output / just '-'.
     * Sekarang konversi ke unsigned int (well-defined two's complement
     * cast) before digitizing. */
    if (num < 0 && base == 10) {
        is_negative = 1;
        unsigned int unum = (unsigned int)(-(unsigned int)num);
        while (unum > 0) {
            unsigned int digit = unum % (unsigned int)base;
            temp[i++] = (digit < 10) ? '0' + digit : 'A' + (digit - 10);
            unum /= (unsigned int)base;
        }
    } else {
        unsigned int unum = (unsigned int)num;
        while (unum > 0) {
            unsigned int digit = unum % (unsigned int)base;
            temp[i++] = (digit < 10) ? '0' + digit : 'A' + (digit - 10);
            unum /= (unsigned int)base;
        }
    }
    if (is_negative) temp[i++] = '-';
    int j = 0;
    while (i > 0) buffer[j++] = temp[--i];
    buffer[j] = '\0';
}

void utoa(uint32_t num, char* buffer, int base) {
    if (base < 2 || base > 36) { buffer[0] = '\0'; return; }
    char temp[33];
    int i = 0;
    if (num == 0) { buffer[0] = '0'; buffer[1] = '\0'; return; }
    while (num > 0) {
        int digit = num % base;
        temp[i++] = (digit < 10) ? '0' + digit : 'A' + (digit - 10);
        num /= base;
    }
    int j = 0;
    while (i > 0) buffer[j++] = temp[--i];
    buffer[j] = '\0';
}

void itoa_pad(int num, char* buffer, int base, int width) {
    char temp[33];
    itoa(num, temp, base);
    int len = 0;
    while (temp[len]) len++;
    int pad = width - len;
    int i = 0;
    while (pad > 0) { buffer[i++] = '0'; pad--; }
    for (int j = 0; j < len; j++) buffer[i++] = temp[j];
    buffer[i] = '\0';
}

void int_to_hex(uint32_t num, char* buffer) {
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        int digit = (num >> (i * 4)) & 0xF;
        buffer[2 + (7 - i)] = (digit < 10) ? '0' + digit : 'A' + (digit - 10);
    }
    buffer[10] = '\0';
}

void test_itoa_atoi() {
    char buffer[64];
    printf("=== Testing atoi ===\n");
    printf("atoi(\"123\") = %d\n", atoi("123"));
    printf("atoi(\"-456\") = %d\n", atoi("-456"));
    printf("atoi(\"  789\") = %d\n", atoi("  789"));
    printf("\n=== Testing hex_to_int ===\n");
    printf("hex_to_int(\"0xABC\") = %d\n", hex_to_int("0xABC"));
    printf("hex_to_int(\"FF\") = %d\n", hex_to_int("FF"));
    printf("\n=== Testing itoa ===\n");
    itoa(123, buffer, 10); printf("itoa(123, 10) = %s\n", buffer);
    itoa(-456, buffer, 10); printf("itoa(-456, 10) = %s\n", buffer);
    itoa(0xABC, buffer, 16); printf("itoa(0xABC, 16) = %s\n", buffer);
}