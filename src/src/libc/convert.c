/* libc/convert.c - numeric conversion (atoi/strtol/itoa/utoa).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: none. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_CONVERT_C
#define LIBC_CONVERT_C

/* ===================== conversion ===================== */
int strtol(char* s, char** endp = 0, int base = 10) {
    int i; int neg; int v; int d; int any; char c;
    i = 0; neg = 0; v = 0; any = 0;
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') i = i + 1;
    if (s[i] == '+') i = i + 1;
    else if (s[i] == '-') { neg = 1; i = i + 1; }
    if ((base == 0 || base == 16) && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        c = s[i + 2];
        d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d >= 0 && d < 16) { i = i + 2; base = 16; any = 1; }
        else if (base == 0) base = 8;
    } else if (base == 0) {
        if (s[i] == '0') base = 8;
        else base = 10;
    }
    while (1) {
        c = s[i];
        d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        if (d < 0 || d >= base) break;
        v = v * base + d;
        i = i + 1;
        any = 1;
    }
    if (endp) {
        if (any) *endp = s + i;
        else *endp = s;
    }
    if (neg) return -v;
    return v;
}
int atoi(char* s) {
    return strtol(s, 0, 10);
}
void itoa(int v, char* b, int base) {
    char t[16];
    int n; int i; int neg; int d;
    n = 0; neg = 0;
    if (v < 0) neg = 1;
    if (v == 0) { t[0] = '0'; n = 1; }
    while (v != 0) {
        d = v % base;
        if (d < 0) d = -d;
        if (d < 10) t[n] = '0' + d;
        else t[n] = 'a' + d - 10;
        n = n + 1;
        v = v / base;
    }
    if (neg) { t[n] = '-'; n = n + 1; }
    i = n - 1;
    while (i >= 0) { b[n - 1 - i] = t[i]; i = i - 1; }
    b[n] = 0;
}
void utoa(int v, char* b, int base) {
    itoa(v, b, base);
}
#endif
