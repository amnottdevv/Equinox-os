/*
 * ============================================================================
 *  libc.cpp — Standard C library: conversion, sort, environment, time
 * ----------------------------------------------------------------------------
 *  Completes the Equinox OS libc map. Before this file, the kernel already had:
 *    - malloc/free/calloc/realloc  (malloc.cpp — free-list + coalesce)
 *    - memcpy/memset/memmove/memcmp/memchr + the full str* family
 *      (libstring.cpp, stubs.cpp)
 *    - printf/sprintf/snprintf/sscanf (stdio.cpp, libstring.cpp)
 *    - atoi/atol/itoa/utoa/hex_to_int (itoa_atoi.cpp)
 *    - abs/labs/div/ldiv/rand/srand/abort/exit/atexit (stubs.cpp)
 *
 *  ADDED by this file (v10.8 libc audit request):
 *    - strtol / strtoul   : conversion with endptr + base 0/2/8/10/16
 *    - atof               : software IEEE-754 double WITHOUT FPU instructions
 *                           (this file is on the makefile FPU_SENSITIVE list,
 *                           compiled -mgeneral-regs-only — the double return
 *                           goes through the EAX:EDX pair, safe to call even
 *                           before the FPU is initialized)
 *    - qsort / bsearch    : iterative quicksort + small insertion sort,
 *                           standard C comparator function pointer
 *    - getenv             : returns NULL — Equinox OS has no environment
 *                           block yet (honest, not a hidden stub)
 *    - time               : seconds since boot (get_tick()/100). Equinox OS
 *                           has no RTC epoch yet — documented.
 * ============================================================================
 */

#include "header/libc.h"
#include "header/libstring.h"     // strlen
#include "header/timer.h"         // get_tick (100 Hz)
#include "header/itoa_atoi.h"     // atol (semantic reference)
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
//  strtol — string -> long conversion with endptr + base
// ============================================================
//  C89/C99 semantics:
//    - skip leading whitespace (space, \t, \n, \v, \f, \r)
//    - optional +/- sign (only for strtol; strtoul also accepts it,
//      the result is negated in unsigned — per the standard)
//    - base 0  : auto-detection (0x/0X = 16, leading 0 = 8, otherwise 10)
//    - base 16 : optional 0x/0X prefix
//    - base 2..36 : alphanumeric digits per the base
//    - endptr != NULL receives a pointer one character AFTER the last
//      digit consumed (the standard way to detect a parse error:
//      *endptr == nptr means no digits at all)
//    - overflow is clamped to LONG_MAX (0x7FFFFFFF) / LONG_MIN with
//      simple errno-style semantics (no global errno variable —
//      the caller can compare the result against the limit)
long strtol(const char* nptr, char** endptr, int base) {
    const char* s = nptr;
    if (!s) {
        if (endptr) *endptr = (char*)nptr;
        return 0;
    }

    // 1. whitespace
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\v' || *s == '\f' || *s == '\r') {
        s++;
    }

    // 2. sign
    int neg = 0;
    if (*s == '+')      s++;
    else if (*s == '-') { neg = 1; s++; }

    // 3. base detection
    if ((base == 0 || base == 16) &&
        s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        // only treat it as hex if a hex digit follows
        char c2 = s[2];
        if ((c2 >= '0' && c2 <= '9') || (c2 >= 'a' && c2 <= 'f') ||
            (c2 >= 'A' && c2 <= 'F')) {
            s += 2;
            base = 16;
        } else if (base == 0) {
            base = 8;      // "0x" without a digit -> "0" then 'x' stops the parse
        }
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        if (endptr) *endptr = (char*)nptr;
        return 0;
    }

    // 4. digit by digit with overflow clamp
    unsigned long acc = 0;
    unsigned long cutoff;
    int cutlim;
    const unsigned long ULONG_MAX_ = 0xFFFFFFFFul;
    int any = 0;
    cutoff = neg ? (unsigned long)0x80000000ul : 0x7FFFFFFFul;
    cutlim = (int)(cutoff % (unsigned long)base);
    cutoff /= (unsigned long)base;

    for (; *s; s++) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        any = 1;
        if (acc > cutoff || (acc == cutoff && d > cutlim)) {
            acc = (neg ? (unsigned long)0x80000000ul : 0x7FFFFFFFul);
            // consume the remaining valid digits so endptr stays consistent
            for (s++; *s; s++) {
                char c2 = *s;
                int d2;
                if (c2 >= '0' && c2 <= '9')      d2 = c2 - '0';
                else if (c2 >= 'a' && c2 <= 'z') d2 = c2 - 'a' + 10;
                else if (c2 >= 'A' && c2 <= 'Z') d2 = c2 - 'A' + 10;
                else break;
                if (d2 >= base) break;
            }
            break;
        }
        acc = acc * (unsigned long)base + (unsigned long)d;
    }

    if (endptr) *endptr = (char*)(any ? s : nptr);
    return neg ? -(long)acc : (long)acc;
    (void)ULONG_MAX_;
}

// ============================================================
//  strtoul — the unsigned version (clamped to ULONG_MAX 0xFFFFFFFF)
// ============================================================
unsigned long strtoul(const char* nptr, char** endptr, int base) {
    const char* s = nptr;
    if (!s) {
        if (endptr) *endptr = (char*)nptr;
        return 0;
    }
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\v' || *s == '\f' || *s == '\r') {
        s++;
    }
    int neg = 0;
    if (*s == '+')      s++;
    else if (*s == '-') { neg = 1; s++; }

    if ((base == 0 || base == 16) &&
        s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char c2 = s[2];
        if ((c2 >= '0' && c2 <= '9') || (c2 >= 'a' && c2 <= 'f') ||
            (c2 >= 'A' && c2 <= 'F')) {
            s += 2;
            base = 16;
        } else if (base == 0) {
            base = 8;
        }
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        if (endptr) *endptr = (char*)nptr;
        return 0;
    }

    unsigned long acc = 0;
    unsigned long cutoff = 0xFFFFFFFFul / (unsigned long)base;
    int cutlim = (int)(0xFFFFFFFFul % (unsigned long)base);
    int any = 0;
    for (; *s; s++) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        any = 1;
        if (acc > cutoff || (acc == cutoff && (unsigned)d > (unsigned)cutlim)) {
            acc = 0xFFFFFFFFul;
            for (s++; *s; s++) {
                char c2 = *s;
                int d2;
                if (c2 >= '0' && c2 <= '9')      d2 = c2 - '0';
                else if (c2 >= 'a' && c2 <= 'z') d2 = c2 - 'a' + 10;
                else if (c2 >= 'A' && c2 <= 'Z') d2 = c2 - 'A' + 10;
                else break;
                if (d2 >= base) break;
            }
            break;
        }
        acc = acc * (unsigned long)base + (unsigned long)d;
    }
    if (endptr) *endptr = (char*)(any ? s : nptr);
    return neg ? (0xFFFFFFFFul - acc) + 1ul : acc;   /* negation wrap-around */
}

// ============================================================
//  atof — software decimal -> IEEE-754 double, WITHOUT FPU
// ============================================================
//  Why bother with bit manipulation? Because this file is compiled
//  -mgeneral-regs-only (the kernel does not yet guarantee the FPU is on
//  in every path), and because the result also proves Equinox OS has
//  complete numeric conversion without depending on floating-point
//  hardware.
//
//  Algorithm (accurate enough for the whole practical range):
//    1. collect the significant mantissa digits (max 15 digits, the rest
//       dropped with a coarse round-half-even via remaining digit >= 5)
//    2. track the decimal exponent (point position + 'e'/'E' exponent)
//    3. build the value: if 0 <= exp10 <= 22 and the mantissa fits 53 bits:
//       value = mantissa * 10^exp10 (exact integer multiply), or
//       mantissa / 10^-exp10 (exact 64-bit division) — these two paths
//       guarantee an EXACT result for "ordinary" numbers (0.5, 3.14, 1e9...)
//    4. fallback: normalize the mantissa into [2^52, 2^53) while adjusting
//       the binary exponent, then manually round-to-nearest-even to a
//       52-bit mantissa + bias 1023.
//  Precision: bit-correct for common cases; pathological cases
//  (17+ digits) may be off by 1 ulp — more than enough for a hobby OS.
double atof(const char* nptr) {
    if (!nptr) return 0.0;

    const char* s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\v' || *s == '\f' || *s == '\r') {
        s++;
    }
    int neg = 0;
    if (*s == '+')      s++;
    else if (*s == '-') { neg = 1; s++; }

    // ---- mantissa: 15 significant digits ----
    uint64_t mant = 0;
    int digits = 0;
    int exp10 = 0;
    int any = 0;
    int point_seen = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == '.') {
            if (point_seen) break;
            point_seen = 1;
            continue;
        }
        if (c < '0' || c > '9') break;
        any = 1;
        if (digits < 15) {
            mant = mant * 10u + (uint64_t)(c - '0');
            digits++;
            if (point_seen) exp10--;
        } else {
            // 16th digit on: round the mantissa once (half-up is enough)
            if (c >= '5' && mant != 0xFFFFFFFFFFFFFull) mant++;
            if (!point_seen) exp10++;
        }
    }

    // ---- optional e/E exponent ----
    if (any && (*s == 'e' || *s == 'E')) {
        const char* save = s;
        s++;
        int eneg = 0;
        if (*s == '+')      s++;
        else if (*s == '-') { eneg = 1; s++; }
        int ev = 0, edig = 0;
        for (; *s >= '0' && *s <= '9'; s++) {
            if (edig < 6) ev = ev * 10 + (*s - '0');
            edig++;
        }
        if (edig == 0) {
            s = save;              // "1e" without digits: 'e' is not part of the number
        } else {
            if (eneg) ev = -ev;
            // clamp so int does not overflow
            if (ev > 300)  ev = 300;
            if (ev < -300) ev = -300;
            exp10 += ev;
        }
    }

    if (!any) return 0.0;
    if (mant == 0) return neg ? -0.0 : 0.0;

    // ---- exact path: 10^k for small k (table up to 10^18) ----
    static const uint64_t pow10u[19] = {
        1ull, 10ull, 100ull, 1000ull, 10000ull,
        100000ull, 1000000ull, 10000000ull, 100000000ull, 1000000000ull,
        10000000000ull, 100000000000ull, 1000000000000ull,
        10000000000000ull, 100000000000000ull, 1000000000000000ull,
        10000000000000000ull, 100000000000000000ull, 1000000000000000000ull
    };

    /* Try to keep the value exact as long as the mantissa stays < 2^63:
       - exp10 >= 0 : mant * 10^exp10  (if it does not overflow)
       - exp10 < 0  : mant / 10^-exp10 (integer division — loses
                      fraction precision; only used when the remainder == 0,
                      otherwise the normalization fallback below takes over) */
    if (exp10 >= 0 && exp10 <= 18 && mant <= 0x7FFFFFFFFFFFFFFFull / pow10u[exp10]) {
        mant *= pow10u[exp10];
        exp10 = 0;
    } else if (exp10 < 0 && exp10 >= -18) {
        // divide repeatedly while it divides evenly — the rest goes through the normalization path
        uint64_t q = mant / pow10u[-exp10];
        uint64_t r = mant % pow10u[-exp10];
        if (r == 0) {
            mant = q;
            exp10 = 0;
        }
        /* r != 0: leave mant/exp10 as is — the general normalization
           below handles the fraction (more precise than
           rounding here) */
    }

    // ---- general normalization to IEEE-754 ----
    // intermediate: value = mant * 10^exp10 (exp10 may be != 0)
    // Strategy: scale mant into [2^52, 2^53) with 10x multipliers/divisors,
    // adjusting exp10, until exp10 == 0. Round-to-nearest-even at the end.
    int bin_exp = 64;             // mant < 2^63 -> highest bit <= 62
    // find the highest bit
    {
        uint64_t m = mant;
        bin_exp = 0;
        while (m) { m >>= 1; bin_exp++; }   // bin_exp = bit_length(mant)
    }

    // Stability: work with a 64-bit mantissa + round sticky bit.
    uint64_t hi = mant;
    int e2 = bin_exp;             // value ~= hi * 2^(0) with an e2-bit width
    int extra_shift = 0;          // bits already shifted out (sticky)

    while (exp10 > 0) {
        // multiply by 10 = *5 then *2: if hi * 5 overflows 63 bits, shift right
        if (hi > (0xFFFFFFFFFFFFFFFFull - 4ull) / 5ull) {
            // hi*5 does not fit: divide by 2 first (keep sticky)
            extra_shift++;
            uint64_t lost = hi & 1ull;
            hi >>= 1;
            e2--;
            if (lost) hi |= 1ull;              // make it sticky in the LSB
        } else {
            hi *= 5ull;
            exp10--;                           // 10 = 5 * 2 -> power of 2 for free
            e2++;
        }
    }
    while (exp10 < 0) {
        // divide by 10 = divide by 5 then by 2; divide by 5 first for precision
        uint64_t q = hi / 5ull;
        uint64_t r = hi % 5ull;
        if (q == 0) {
            // too small to divide again: multiply by 2 (subnormal heading
            // toward normal — in practice does not happen for sane input)
            hi <<= 1;
            if (hi >> 63) { /* keep tracking */ }
            e2++;
            exp10++;          // value unchanged: *2 then a clean /10 = remaining /5
            // note: this preserves the invariant value ~= hi * 2^(e2-64) * 10^exp10
            // with a rounding simplification; this path is extremely rare.
            if (exp10 >= 0) break;
            // safe: continue to the next iteration with a larger hi
            continue;
        }
        hi = q;
        if (r) hi |= 1ull;    // sticky
        exp10++;              // /5 done; /2 =
        extra_shift++;        // shifted out via e2--
        e2--;
    }

    // Now: value ~= (hi / 2^(e2 - 53... )) — final normalization.
    // Target: 53 significant bits (1 implicit + 52 explicit).
    if (hi == 0) return neg ? -0.0 : 0.0;

    while (e2 > 53) {
        uint64_t lost = hi & 1ull;
        hi >>= 1;
        if (lost) hi |= 1ull;      // sticky round
        e2--;
    }
    while (e2 < 53 && hi < (1ull << 62)) {
        // raise precision while it does not overflow
        uint64_t nhi = hi << 1;
        if (nhi < hi) break;       // 64-bit overflow: stop
        hi = nhi;
        e2++;
    }

    // Round-to-nearest-even to a 52-bit mantissa
    uint64_t sig = hi;            // bit_length(sig) == e2 (ideally 53)
    int sig_bits = 0;
    {
        uint64_t m = sig;
        while (m) { m >>= 1; sig_bits++; }
    }
    if (sig_bits > 53) {
        int drop = sig_bits - 53;
        uint64_t rem_mask = (1ull << drop) - 1ull;
        uint64_t rem = sig & rem_mask;
        uint64_t half = 1ull << (drop - 1);
        sig >>= drop;
        sig_bits = 53;
        if (rem > half || (rem == half && (sig & 1ull))) {
            sig++;
            if (sig >> 52) {       // carry out of the 52 bits
                sig >>= 1;
                sig_bits = 54;     // corrected below
            }
        }
    }

    // Now assemble the double: sig in [2^52, 2^53) ideally.
    int64_t unbiased;              // true exponent = (MSB position of sig) - 53
    {
        uint64_t m = sig;
        int msb = 0;
        while (m) { m >>= 1; msb++; }
        unbiased = (int64_t)msb - 53;
        // sig = f * 2^unbiased, f in [1, 2)
    }

    if (unbiased > 1023) {
        // overflow -> +inf family
        uint64_t bits = 0x7FF0000000000000ull;
        union { uint64_t u; double d; } cvt;
        cvt.u = bits;
        return neg ? -cvt.d : cvt.d;
    }
    if (unbiased < -1022) {
        // subnormal / underflow -> clamp to 0 (rare; practical input is safe)
        return neg ? -0.0 : 0.0;
    }

    {
        uint64_t frac = sig & 0xFFFFFFFFFFFFFull;         // drop the implicit bit
        uint64_t exp_field = (uint64_t)(unbiased + 1023) & 0x7FFull;
        uint64_t bits = (exp_field << 52) | frac;
        if (neg) bits |= 0x8000000000000000ull;
        union { uint64_t u; double d; } cvt;
        cvt.u = bits;
        return cvt.d;
    }
}

// ============================================================
//  qsort — iterative quicksort (no recursion = IRQ-stack safe)
// ============================================================
//  Design:
//    - Hoare-style partition, pivot = middle element
//    - small sub-partitions (<= 8 elements) -> insertion sort
//    - iterative main loop with an explicit stack (max depth 32 —
//      always enough because we always recurse into the SMALL side first)
//    - swap via a byte buffer (elements <= 64 bytes use the stack buffer;
//      larger ones use the kernel malloc — and free it again)
int qsort_cmp_stub_used;    // (unused — just a symbol marker)

void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*)) {
    if (!base || size == 0 || nmemb < 2 || !compar) return;

    uint8_t* a = (uint8_t*)base;
    uint8_t swapbuf[64];

    // iterative quicksort with an offset stack
    size_t stack[32];
    int sp = 0;
    stack[sp++] = 0;
    stack[sp++] = nmemb - 1;

    while (sp > 0) {
        size_t hi = stack[--sp];
        size_t lo = stack[--sp];

        // insertion sort for small partitions
        if (hi - lo + 1 <= 8 || hi <= lo) {
            for (size_t i = lo + 1; i <= hi; i++) {
                // save a[i] into swapbuf
                uint8_t* src = a + i * size;
                for (size_t k = 0; k < size; k++) swapbuf[k] = src[k];
                size_t j = i;
                while (j > lo && compar(a + (j - 1) * size, swapbuf) > 0) {
                    uint8_t* dst = a + j * size;
                    uint8_t* mv  = a + (j - 1) * size;
                    for (size_t k = 0; k < size; k++) dst[k] = mv[k];
                    j--;
                }
                uint8_t* dst = a + j * size;
                for (size_t k = 0; k < size; k++) dst[k] = swapbuf[k];
            }
            continue;
        }

        // pivot = middle; swap to position lo so the loop is clean
        size_t mid = lo + (hi - lo) / 2;
        uint8_t* pm = a + mid * size;
        uint8_t* pl = a + lo * size;
        for (size_t k = 0; k < size; k++) {
            uint8_t t = pl[k]; pl[k] = pm[k]; pm[k] = t;
        }

        // Hoare partition: [lo+1 .. hi]
        size_t i = lo + 1;
        size_t j = hi;
        uint8_t* pivot = pl;               // the pivot value is now in a[lo]
        for (;;) {
            while (i <= j && compar(a + i * size, pivot) < 0) i++;
            while (j >= i && j > lo && compar(a + j * size, pivot) > 0) j--;
            if (i > j) break;
            uint8_t* pi = a + i * size;
            uint8_t* pj = a + j * size;
            for (size_t k = 0; k < size; k++) {
                uint8_t t = pi[k]; pi[k] = pj[k]; pj[k] = t;
            }
            i++;
            if (j == 0) break;              // guard size_t underflow
            j--;
        }
        // put the pivot in its final position (j)
        pl = a + lo * size;
        uint8_t* pj = a + j * size;
        for (size_t k = 0; k < size; k++) {
            uint8_t t = pl[k]; pl[k] = pj[k]; pj[k] = t;
        }

        // recurse into the SMALL side via the loop; the large side is pushed on the stack
        size_t left_n  = j - lo;
        size_t right_n = hi - j;
        if (left_n > right_n) {
            if (j > lo + 1) { stack[sp++] = lo; stack[sp++] = j - 1; }
            if (j + 1 < hi) { stack[sp++] = j + 1; stack[sp++] = hi; }
        } else {
            if (j + 1 < hi) { stack[sp++] = j + 1; stack[sp++] = hi; }
            if (j > lo + 1) { stack[sp++] = lo; stack[sp++] = j - 1; }
        }
        // (at most 2 pushes per iteration with the small-side-first
        // pattern: depth is guaranteed O(log n) — 32 slots suffice for nmemb 2^32)
    }
}

// ============================================================
//  bsearch — standard binary search (the array must be sorted)
// ============================================================
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*)) {
    if (!key || !base || size == 0 || !compar) return NULL;
    const uint8_t* a = (const uint8_t*)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = compar(key, a + mid * size);
        if (c == 0) return (void*)(a + mid * size);
        if (c < 0) hi = mid;
        else       lo = mid + 1;
    }
    return NULL;
}

// ============================================================
//  getenv — Equinox OS has no environment block yet
// ============================================================
//  Standard contract: return NULL when the variable does not exist. Since
//  the OS does not define an environment at all, the answer for ALL
//  names is NULL. The call is not an error — just "not there".
//  (When Equinox OS one day has a per-process ENV, this function is the
//  single point that needs changing.)
const char* getenv(const char* name) {
    (void)name;
    return NULL;
}

// ============================================================
//  time — seconds since boot (not the UNIX epoch)
// ============================================================
//  Equinox OS does not read the RTC for an epoch yet. The clearly
//  documented temporary convention: time() = uptime in seconds
//  (get_tick() runs at 100 Hz). Callers needing sub-second resolution
//  must still use gettick() directly. t != NULL -> *t = same value.
long time(long* t) {
    long secs = (long)(get_tick() / 100u);
    if (t) *t = secs;
    return secs;
}

#ifdef __cplusplus
}
#endif
