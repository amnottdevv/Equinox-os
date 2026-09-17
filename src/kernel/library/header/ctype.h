// ============================================================
//  ctype.h — ASCII ctype for the Equinox OS kernel (v10.11)
// ------------------------------------------------------------
//  Created because lwIP (gcc, third_party) includes <ctype.h>
//  (used by ip4addr_aton). The kernel include path (this folder)
//  precedes inc32/glibc, so this file shadows the glibc version
//  and avoids the __ctype_b_loc dependency.
//  All functions are static inline — no tables, no linkage.
// ============================================================
#ifndef _EQUINOX_CTYPE_H
#define _EQUINOX_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

static inline int morph_isdigit_(int c) { return (c >= '0' && c <= '9'); }
static inline int morph_islower_(int c) { return (c >= 'a' && c <= 'z'); }
static inline int morph_isupper_(int c) { return (c >= 'A' && c <= 'Z'); }
static inline int morph_isalpha_(int c) { return morph_islower_(c) || morph_isupper_(c); }
static inline int morph_isalnum_(int c) { return morph_isalpha_(c) || morph_isdigit_(c); }
static inline int morph_isspace_(int c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
            c == '\f' || c == '\r');
}
static inline int morph_isxdigit_(int c) {
    return morph_isdigit_(c) ||
           (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int morph_iscntrl_(int c) { return (c < 0x20 || c == 0x7F); }
static inline int morph_isprint_(int c) { return (c >= 0x20 && c < 0x7F); }
static inline int morph_isgraph_(int c) { return (c > 0x20 && c < 0x7F); }
static inline int morph_ispunct_(int c) {
    return morph_isgraph_(c) && !morph_isalnum_(c);
}
static inline int morph_tolower_(int c) {
    return morph_isupper_(c) ? (c - 'A' + 'a') : c;
}
static inline int morph_toupper_(int c) {
    return morph_islower_(c) ? (c - 'a' + 'A') : c;
}

/* Standard names (macros — safe for both C and C++) */
#define isdigit(c)  morph_isdigit_(c)
#define islower(c)  morph_islower_(c)
#define isupper(c)  morph_isupper_(c)
#define isalpha(c)  morph_isalpha_(c)
#define isalnum(c)  morph_isalnum_(c)
#define isspace(c)  morph_isspace_(c)
#define isxdigit(c) morph_isxdigit_(c)
#define iscntrl(c)  morph_iscntrl_(c)
#define isprint(c)  morph_isprint_(c)
#define isgraph(c)  morph_isgraph_(c)
#define ispunct(c)  morph_ispunct_(c)
#define tolower(c)  morph_tolower_(c)
#define toupper(c)  morph_toupper_(c)

#ifdef __cplusplus
}
#endif

#endif /* _EQUINOX_CTYPE_H */
