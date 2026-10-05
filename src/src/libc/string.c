/* libc/string.c - core string functions (strlen/strcmp/strcpy/strcat/strchr/strstr).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: none. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_STRING_C
#define LIBC_STRING_C

/* ===================== string ===================== */
int strlen(char* s) {
    int n;
    n = 0;
    while (s[n]) n = n + 1;
    return n;
}
int strcmp(char* a, char* b) {
    int i;
    i = 0;
    while (a[i] && a[i] == b[i]) i = i + 1;
    return a[i] - b[i];
}
int strncmp(char* a, char* b, int n) {
    int i;
    i = 0;
    while (i < n && a[i] && a[i] == b[i]) i = i + 1;
    if (i == n) return 0;
    return a[i] - b[i];
}
char* strcpy(char* d, char* s) {
    int i;
    i = 0;
    while (s[i]) { d[i] = s[i]; i = i + 1; }
    d[i] = 0;
    return d;
}
char* strncpy(char* d, char* s, int n) {
    int i;
    i = 0;
    while (i < n && s[i]) { d[i] = s[i]; i = i + 1; }
    while (i < n) { d[i] = 0; i = i + 1; }
    return d;
}
char* strcat(char* d, char* s) {
    int i;
    int j;
    i = 0;
    while (d[i]) i = i + 1;
    j = 0;
    while (s[j]) { d[i] = s[j]; i = i + 1; j = j + 1; }
    d[i] = 0;
    return d;
}
char* strncat(char* d, char* s, int n) {
    int i;
    int j;
    i = 0;
    while (d[i]) i = i + 1;
    j = 0;
    while (j < n && s[j]) { d[i] = s[j]; i = i + 1; j = j + 1; }
    d[i] = 0;
    return d;
}
char* strchr(char* s, int c) {
    int i;
    i = 0;
    while (s[i]) {
        if (s[i] == c) return s + i;
        i = i + 1;
    }
    if (c == 0) return s + i;
    return 0;
}
char* strrchr(char* s, int c) {
    int i;
    int last;
    last = -1;
    i = 0;
    while (s[i]) {
        if (s[i] == c) last = i;
        i = i + 1;
    }
    if (c == 0) return s + i;
    if (last < 0) return 0;
    return s + last;
}
char* strstr(char* h, char* n) {
    int i;
    int j;
    if (!n[0]) return h;
    i = 0;
    while (h[i]) {
        if (h[i] == n[0]) {
            j = 0;
            while (n[j] && h[i + j] && h[i + j] == n[j]) j = j + 1;
            if (!n[j]) return h + i;
        }
        i = i + 1;
    }
    return 0;
}
#endif
