/* libc/printf.c - printf family (printf/sprintf/snprintf local renderer).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: none. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_PRINTF_C
#define LIBC_PRINTF_C

/* ===================== printf family ===================== */
/* v0.3 FR-19: printf renders LOCALLY through the snprintf engine
   (__vsnprintf) with up to 7 conversion args — the old 3-arg
   kernel SYS_PRINTF ABI limit is gone. */
int __vsnprintf(char* b, int cap, char* fmt, int* ai);
int printf(char* fmt, int a = 0, int b = 0, int c = 0, int d = 0, int e = 0) {
    char buf[512];
    int ai[5];
    int n;
    if (!fmt) fmt = "(null)";
    ai[0] = a; ai[1] = b; ai[2] = c; ai[3] = d; ai[4] = e;
    n = __vsnprintf(buf, 512, fmt, ai);
    if (n > 511) n = 511;
    buf[n] = 0;
    print(buf);
    return n;
}
int __sn_emit(char* b, int cap, int* len, char ch) {
    if (*len < cap - 1) b[*len] = ch;
    *len = *len + 1;
    return 0;
}
int __sn_str(char* b, int cap, int* len, char* s, int width, char pad, int la) {
    int n; int i;
    n = 0;
    while (s[n]) n = n + 1;
    if (!la) { i = n; while (i < width) { __sn_emit(b, cap, len, pad); i = i + 1; } }
    i = 0;
    while (i < n) { __sn_emit(b, cap, len, s[i]); i = i + 1; }
    if (la) { i = n; while (i < width) { __sn_emit(b, cap, len, ' '); i = i + 1; } }
    return 0;
}
int __sn_int(char* b, int cap, int* len, int v, int base, int width, char pad, int la) {
    char t[16];
    int n; int i; int neg; int d;
    n = 0; neg = 0;
    if (v < 0) neg = 1;
    if (v == 0) { t[0] = '0'; n = 1; }
    while (v != 0) {
        d = v % base;
        if (d < 0) d = -d;
        t[n] = '0' + d;
        n = n + 1;
        v = v / base;
    }
    if (neg) { t[n] = '-'; n = n + 1; }
    if (!la) { i = n; while (i < width) { __sn_emit(b, cap, len, pad); i = i + 1; } }
    i = n - 1;
    while (i >= 0) { __sn_emit(b, cap, len, t[i]); i = i - 1; }
    if (la) { i = n; while (i < width) { __sn_emit(b, cap, len, ' '); i = i + 1; } }
    return 0;
}
int __sn_hex(char* b, int cap, int* len, int v, int up, int width, char pad, int la) {
    char t[8];
    int i; int d; int n;
    i = 0;
    while (i < 8) {
        d = (v >> ((7 - i) * 4)) & 15;
        if (d < 10) t[i] = '0' + d;
        else if (up) t[i] = 'A' + d - 10;
        else t[i] = 'a' + d - 10;
        i = i + 1;
    }
    n = 0;
    while (n < 7 && t[n] == '0') n = n + 1;
    if (!la) { i = 8 - n; while (i < width) { __sn_emit(b, cap, len, pad); i = i + 1; } }
    i = n;
    while (i < 8) { __sn_emit(b, cap, len, t[i]); i = i + 1; }
    if (la) { i = 8 - n; while (i < width) { __sn_emit(b, cap, len, ' '); i = i + 1; } }
    return 0;
}
/* v0.3 FR-19: unsigned decimal + octal rendering — binary long
   division by 10: (n >> i) & 1 extracts each bit even when n is
   negative (arithmetic shift + mask), so the whole 0..4294967295
   range works without an unsigned type. */
int __udiv10(int n, int* rem) {
    int q; int r; int b; int i;
    q = 0; r = 0;
    i = 31;
    while (i >= 0) {
        r = r * 2 + ((n >> i) & 1);
        b = 0;
        if (r >= 10) { b = 1; r = r - 10; }
        q = q * 2 + b;
        i = i - 1;
    }
    *rem = r;
    return q;
}
int __sn_uint(char* b, int cap, int* len, int v, int width, char pad, int la) {
    char t[12];
    int n; int i; int r;
    n = 0;
    if (v == 0) { t[0] = '0'; n = 1; }
    while (v != 0) {
        v = __udiv10(v, &r);
        t[n] = '0' + r;
        n = n + 1;
    }
    if (!la) { i = n; while (i < width) { __sn_emit(b, cap, len, pad); i = i + 1; } }
    i = n - 1;
    while (i >= 0) { __sn_emit(b, cap, len, t[i]); i = i - 1; }
    if (la) { i = n; while (i < width) { __sn_emit(b, cap, len, ' '); i = i + 1; } }
    return 0;
}
int __sn_oct(char* b, int cap, int* len, int v, int width, char pad, int la) {
    char t[13];
    int n; int i; int r;
    n = 0;
    if (v == 0) { t[0] = '0'; n = 1; }
    while (v != 0) {
        r = v & 7;
        v = (v >> 3) & 536870911;
        t[n] = '0' + r;
        n = n + 1;
    }
    if (!la) { i = n; while (i < width) { __sn_emit(b, cap, len, pad); i = i + 1; } }
    i = n - 1;
    while (i >= 0) { __sn_emit(b, cap, len, t[i]); i = i - 1; }
    if (la) { i = n; while (i < width) { __sn_emit(b, cap, len, ' '); i = i + 1; } }
    return 0;
}
int __vsnprintf(char* buf, int size, char* fmt, int* ai) {
    int i; int len; int specn; char ch; char pad; int width; int la; int av; char* sv;
    i = 0; len = 0; specn = 0;
    while (fmt[i]) {
        ch = fmt[i];
        if (ch != '%') { __sn_emit(buf, size, &len, ch); i = i + 1; continue; }
        i = i + 1;
        if (fmt[i] == '%') { __sn_emit(buf, size, &len, '%'); i = i + 1; continue; }
        la = 0; pad = ' '; width = 0;
        while (fmt[i] == '-') { la = 1; i = i + 1; }
        if (fmt[i] == '0') { pad = '0'; i = i + 1; }
        while (fmt[i] >= '0' && fmt[i] <= '9') { width = width * 10 + (fmt[i] - '0'); i = i + 1; }
        if (specn < 5) av = ai[specn];
        else av = 0;
        specn = specn + 1;
        ch = fmt[i];
        if (ch == 'd' || ch == 'i') {
            __sn_int(buf, size, &len, av, 10, width, pad, la);
        } else if (ch == 'u') {
            __sn_uint(buf, size, &len, av, width, pad, la);
        } else if (ch == 'o') {
            __sn_oct(buf, size, &len, av, width, pad, la);
        } else if (ch == 'x') {
            __sn_hex(buf, size, &len, av, 0, width, pad, la);
        } else if (ch == 'X') {
            __sn_hex(buf, size, &len, av, 1, width, pad, la);
        } else if (ch == 'c') {
            __sn_emit(buf, size, &len, av);
        } else if (ch == 's') {
            sv = av;
            if (!sv) sv = "(null)";
            __sn_str(buf, size, &len, sv, width, pad, la);
        } else if (ch == 'p') {
            __sn_emit(buf, size, &len, '0');
            __sn_emit(buf, size, &len, 'x');
            __sn_hex(buf, size, &len, av, 0, width, pad, la);
        }
        i = i + 1;
    }
    if (len < size) buf[len] = 0;
    else buf[size - 1] = 0;
    return len;
}
int snprintf(char* buf, int size, char* fmt, int a = 0, int b = 0, int c = 0, int d = 0, int e = 0) {
    int ai[5];
    ai[0] = a; ai[1] = b; ai[2] = c; ai[3] = d; ai[4] = e;
    return __vsnprintf(buf, size, fmt, ai);
}
int sprintf(char* buf, char* fmt, int a = 0, int b = 0, int c = 0, int d = 0, int e = 0) {
    int ai[5];
    ai[0] = a; ai[1] = b; ai[2] = c; ai[3] = d; ai[4] = e;
    return __vsnprintf(buf, 1073741824, fmt, ai);
}
#endif
