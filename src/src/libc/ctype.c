/* libc/ctype.c - character classification (isspace/isdigit/...).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: none. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_CTYPE_C
#define LIBC_CTYPE_C

/* ===================== ctype (FR-19) ===================== */
int isspace(int c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 11 || c == 12) return 1;
    return 0;
}
int isdigit(int c) {
    if (c >= '0' && c <= '9') return 1;
    return 0;
}
int isalpha(int c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return 0;
}
int isalnum(int c) {
    if (isalpha(c) || isdigit(c)) return 1;
    return 0;
}
int isupper(int c) {
    if (c >= 'A' && c <= 'Z') return 1;
    return 0;
}
int islower(int c) {
    if (c >= 'a' && c <= 'z') return 1;
    return 0;
}
int isxdigit(int c) {
    if (isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) return 1;
    return 0;
}
int isprint(int c) {
    if (c >= 32 && c < 127) return 1;
    return 0;
}
int isgraph(int c) {
    if (c > 32 && c < 127) return 1;
    return 0;
}
int iscntrl(int c) {
    if (c < 32 || c == 127) return 1;
    return 0;
}
int ispunct(int c) {
    if (isgraph(c) && !isalnum(c)) return 1;
    return 0;
}
int toupper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}
int tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}
#endif
