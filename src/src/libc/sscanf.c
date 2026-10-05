/* libc/sscanf.c - sscanf (FR-19).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: none. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_SSCANF_C
#define LIBC_SSCANF_C

/* ===================== sscanf (FR-19) =====================
   %d %u %x %s %c %%, whitespace, literal chars. Pointers are
   passed straight through (mtcc does not type-check arguments). */
void __ss_putc(char* dst, int n, char c) {
    if (dst) dst[n] = c;
}
int sscanf(char* s, char* fmt, int* a = 0, int* b = 0, int* c = 0, int* d = 0, int* e = 0) {
    int ai[5];
    int conv; int si; int fi; int n; int neg; int v; int dg; int base; int* ip; char ch;
    ai[0] = a; ai[1] = b; ai[2] = c; ai[3] = d; ai[4] = e;
    si = 0; fi = 0; conv = 0;
    while (fmt[fi]) {
        ch = fmt[fi];
        if (ch == ' ') {
            while (fmt[fi] == ' ') fi = fi + 1;
            while (s[si] == ' ' || s[si] == '\t' || s[si] == '\n') si = si + 1;
        } else if (ch != '%') {
            if (s[si] != ch) return conv;
            si = si + 1;
            fi = fi + 1;
        } else {
            fi = fi + 1;
            ch = fmt[fi];
            if (ch == '%') {
                if (s[si] != '%') return conv;
                si = si + 1;
                fi = fi + 1;
            } else if (ch == 'd' || ch == 'u' || ch == 'x') {
                base = 10;
                if (ch == 'x') base = 16;
                while (s[si] == ' ' || s[si] == '\t' || s[si] == '\n') si = si + 1;
                neg = 0; v = 0; dg = 0;
                if (s[si] == '-') { neg = 1; si = si + 1; }
                else if (s[si] == '+') si = si + 1;
                if (base == 16 && s[si] == '0' && (s[si + 1] == 'x' || s[si + 1] == 'X')) si = si + 2;
                while (1) {
                    ch = s[si];
                    if (ch >= '0' && ch <= '9') v = v * base + (ch - '0');
                    else if (base == 16 && ch >= 'a' && ch <= 'f') v = v * base + (ch - 'a' + 10);
                    else if (base == 16 && ch >= 'A' && ch <= 'F') v = v * base + (ch - 'A' + 10);
                    else break;
                    dg = dg + 1;
                    si = si + 1;
                }
                if (dg == 0) return conv;
                if (neg) v = -v;
                ip = 0;
                if (conv < 5) ip = ai[conv];
                if (ip) *ip = v;
                conv = conv + 1;
                fi = fi + 1;
            } else if (ch == 'c') {
                if (!s[si]) return conv;
                ip = 0;
                if (conv < 5) ip = ai[conv];
                if (ip) *ip = s[si] & 255;
                conv = conv + 1;
                si = si + 1;
                fi = fi + 1;
            } else if (ch == 's') {
                while (s[si] == ' ' || s[si] == '\t' || s[si] == '\n') si = si + 1;
                if (!s[si]) return conv;
                ip = 0;
                if (conv < 5) ip = ai[conv];
                n = 0;
                while (s[si] && s[si] != ' ' && s[si] != '\t' && s[si] != '\n') {
                    __ss_putc(ip, n, s[si]);
                    n = n + 1;
                    si = si + 1;
                }
                __ss_putc(ip, n, 0);
                conv = conv + 1;
                fi = fi + 1;
            } else {
                fi = fi + 1;
            }
        }
    }
    return conv;
}
#endif
