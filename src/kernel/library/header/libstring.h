#ifndef LIBSTRING_H
#define LIBSTRING_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ======================== MEMORY ========================
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
void* memmove(void* dest, const void* src, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);

// ======================== STRING ========================
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int starts_with(const char* str, const char* prefix);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
char* strtok(char* str, const char* delim);
char* strncpy(char* dest, const char* src, size_t n);
char* strncat(char* dest, const char* src, size_t n);
int strncmp(const char* s1, const char* s2, size_t n);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strpbrk(const char* s, const char* accept);

// ======================== FORMATTED OUTPUT ========================
int sprintf(char* buffer, const char* format, ...);
int snprintf(char* buffer, size_t size, const char* format, ...);
int sscanf(const char* str, const char* format, ...);

// ======================== CASE CONVERSION ========================
char* strlwr(char* str);
char* strupr(char* str);

// ======================== TOKEN / SPLIT ========================
int split(char* str, const char* delim, char** tokens, int max_tokens);

#ifdef __cplusplus
}
#endif

#endif