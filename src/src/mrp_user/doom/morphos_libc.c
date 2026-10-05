/* ============================================================================
 *  morphos_libc.c — freestanding libc shim for doomgeneric on Equinox OS
 * ----------------------------------------------------------------------------
 *  Implements exactly the libc surface doomgeneric's core actually uses
 *  (audited: printf family, stdio FILE over the RAMFS fd syscalls, string,
 *  ctype, stdlib, minimal sscanf) on top of the raw int 0x80 syscall ABI.
 *
 *  This file deliberately does NOT include Morph.h: Morph.h declares its
 *  libc helpers as `static inline` under their mtcc names (malloc, memcpy,
 *  ...) which would collide with the real libc symbols defined here.
 *
 *  Syscall ABI (syscall.h): EAX = nr, EBX/ECX/EDX = args, EAX = return.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <assert.h>
#include <sys/stat.h>
#include <stdint.h>

/* ---------------------------------------------------------------
 *  Raw syscall wrappers (int 0x80) — local copies, ABI-stable.
 * --------------------------------------------------------------- */
static inline int sc0(int num) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory", "cc");
    return ret;
}
static inline int sc1(int num, uint32_t a1) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret) : "a"(num), "b"(a1) : "memory", "cc");
    return ret;
}
static inline int sc2(int num, uint32_t a1, uint32_t a2) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret) : "a"(num), "b"(a1), "c"(a2) : "memory", "cc");
    return ret;
}
static inline int sc3(int num, uint32_t a1, uint32_t a2, uint32_t a3) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                     : "memory", "cc");
    return ret;
}

#define SYS_EXIT      1
#define SYS_WRITE     4
#define SYS_READ      5
#define SYS_OPEN      6
#define SYS_CLOSE     7
#define SYS_MALLOC   12
#define SYS_GETTICK  13
#define SYS_SLEEP    14
#define SYS_GETARGS  15
#define SYS_MKFILE   16
#define SYS_LSEEK    27

int errno;

/* ---------------------------------------------------------------
 *  string.h
 * --------------------------------------------------------------- */
size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

size_t strnlen(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

char* strcpy(char* d, const char* s) {
    char* r = d;
    while ((*d++ = *s++) != 0) {}
    return r;
}

char* strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char* strcat(char* d, const char* s) {
    char* r = d;
    while (*d) d++;
    while ((*d++ = *s++) != 0) {}
    return r;
}

char* strncat(char* d, const char* s, size_t n) {
    char* r = d;
    while (*d) d++;
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    return r;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0)
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
    }
    return 0;
}

int strcasecmp(const char* a, const char* b) {
    while (*a && toupper((unsigned char)*a) == toupper((unsigned char)*b)) {
        a++; b++;
    }
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = toupper((unsigned char)a[i]);
        int cb = toupper((unsigned char)b[i]);
        if (ca != cb || a[i] == 0) return ca - cb;
    }
    return 0;
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (*s == 0) return NULL;
    }
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    for (; *s; s++)
        if (*s == (char)c) last = s;
    return (char*)last;
}

char* strstr(const char* h, const char* n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char* a = h;
        const char* b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char*)h;
    }
    return NULL;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char* strtok(char* s, const char* sep) {
    static char* next = NULL;
    if (s) next = s;
    if (!next || !*next) return NULL;
    char* start = next;
    while (*start && strchr(sep, *start)) start++;
    if (!*start) { next = start; return NULL; }
    char* end = start;
    while (*end && !strchr(sep, *end)) end++;
    if (*end) { *end = 0; next = end + 1; }
    else next = end;
    return start;
}

/* ---------------------------------------------------------------
 *  mem* — gcc emits calls to memcpy/memset for struct copies.
 * --------------------------------------------------------------- */
void* memcpy(void* d, const void* s, size_t n) {
    unsigned char* dd = (unsigned char*)d;
    const unsigned char* ss = (const unsigned char*)s;
    /* 4-byte strided copy for the aligned interior (I_FinishUpdate
     * pushes 320 bytes/row through here at 35 fps). */
    while (n >= 4 && (((uintptr_t)dd | (uintptr_t)ss) & 3u)) {
        *dd++ = *ss++; n--;
    }
    uint32_t* dd4 = (uint32_t*)dd;
    const uint32_t* ss4 = (const uint32_t*)ss;
    while (n >= 4) { *dd4++ = *ss4++; n -= 4; }
    dd = (unsigned char*)dd4;
    ss = (const unsigned char*)ss4;
    while (n--) *dd++ = *ss++;
    return d;
}

void* memmove(void* d, const void* s, size_t n) {
    unsigned char* dd = (unsigned char*)d;
    const unsigned char* ss = (const unsigned char*)s;
    if (dd < ss) return memcpy(d, s, n);
    dd += n; ss += n;
    while (n--) *--dd = *--ss;
    return d;
}

void* memset(void* d, int c, size_t n) {
    unsigned char* dd = (unsigned char*)d;
    while (n >= 4 && ((uintptr_t)dd & 3u)) { *dd++ = (unsigned char)c; n--; }
    uint32_t v = (uint32_t)(unsigned char)c;
    v |= v << 8; v |= v << 16;
    uint32_t* dd4 = (uint32_t*)dd;
    while (n >= 4) { *dd4++ = v; n -= 4; }
    dd = (unsigned char*)dd4;
    while (n--) *dd++ = (unsigned char)c;
    return d;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* aa = (const unsigned char*)a;
    const unsigned char* bb = (const unsigned char*)b;
    for (; n--; aa++, bb++)
        if (*aa != *bb) return (int)*aa - (int)*bb;
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* ss = (const unsigned char*)s;
    for (; n--; ss++)
        if (*ss == (unsigned char)c) return (void*)ss;
    return NULL;
}

/* ---------------------------------------------------------------
 *  ctype.h — ASCII table.
 * --------------------------------------------------------------- */
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isgraph(int c) { return c > 32 && c < 127; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isspace(int c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }

/* ---------------------------------------------------------------
 *  stdlib.h — arena allocator with an 8-byte size prefix so
 *  realloc() knows the old size (the kernel MRP arena is a bump
 *  allocator; free() is a no-op, which is fine for a single-shot
 *  program in a 33 MB arena that is reset by the next run).
 * --------------------------------------------------------------- */
void* malloc(size_t size) {
    if (size == 0) size = 1;
    /* 8-byte prefix keeps the payload 4-aligned like the raw arena;
     * round the prefix request so the NEXT raw block stays aligned. */
    uint32_t raw = (uint32_t)(size + 8u);
    uint8_t* p = (uint8_t*)(uintptr_t)(uint32_t)sc1(SYS_MALLOC, raw);
    if (!p) return NULL;
    *(uint32_t*)(void*)p = (uint32_t)size;
    return p + 8;
}

void free(void* p) {
    (void)p;   /* bump arena: memory reclaims on program exit */
}

void* calloc(size_t n, size_t size) {
    void* p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void* realloc(void* p, size_t size) {
    if (!p) return malloc(size);
    uint32_t old = *(const uint32_t*)(const void*)((const uint8_t*)p - 8);
    void* q = malloc(size);
    if (!q) return NULL;
    memcpy(q, p, old < size ? old : size);
    return q;
}

void exit(int status) {
    sc1(SYS_EXIT, (uint32_t)status);
    for (;;) {}   /* noreturn */
}

void abort(void) {
    static const char msg[] = "abort() called\n";
    sc3(SYS_WRITE, 2, (uint32_t)(uintptr_t)msg, (uint32_t)sizeof(msg) - 1);
    sc1(SYS_EXIT, (uint32_t)-1);
    for (;;) {}
}

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static unsigned long rand_state = 1;

void srand(unsigned seed) { rand_state = seed ? seed : 1; }

int rand(void) {          /* the classic MSVCRT-style LCG */
    rand_state = rand_state * 214013u + 2531011u;
    return (int)((rand_state >> 16) & 0x7FFF);
}

long strtol(const char* s, char** endp, int base) {
    const char* nptr = s;
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = (*s++ == '-');
    if ((base == 0 || base == 16) &&
        s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (*s == '0') ? 8 : 10;
    }
    long v = 0;
    int any = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        any = 1;
    }
    if (endp) *endp = (char*)(any ? s : nptr);
    return neg ? -v : v;
}

unsigned long strtoul(const char* s, char** endp, int base) {
    return (unsigned long)strtol(s, endp, base);
}

int atoi(const char* s) { return (int)strtol(s, NULL, 10); }
long atol(const char* s) { return strtol(s, NULL, 10); }

/* Insertion sort is enough for the tiny lists DOOM sorts (none in the
 * core, actually — this only satisfies the symbol if a source file
 * changes upstream). */
void qsort(void* base, size_t n, size_t size,
           int (*cmp)(const void*, const void*)) {
    unsigned char* b = (unsigned char*)base;
    unsigned char* tmp = (unsigned char*)malloc(size);
    if (!tmp) return;
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
    free(tmp);
}

char* getenv(const char* name) {
    (void)name;
    return NULL;   /* no environment on Equinox OS */
}

/* ---------------------------------------------------------------
 *  math.h — x87 FPU (kernel ran fninit; CR0.TS/EM clear since v10.9).
 * --------------------------------------------------------------- */
double fabs(double x) {
    __asm__ volatile("fabs" : "+t"(x));
    return x;
}

double sqrt(double x) {
    __asm__ volatile("fsqrt" : "+t"(x));
    return x;
}

double floor(double x) {
    if (x >= 2147483647.0 || x <= -2147483648.0) return x;  /* integral */
    long i = (long)x;
    if (x < 0 && (double)i != x) i--;
    return (double)i;
}

double ceil(double x)  { return -floor(-x); }
double sin(double x)   { (void)x; return 0.0; }  /* unused by core   */
double cos(double x)   { (void)x; return 1.0; }  /* unused by core   */
double tan(double x)   { (void)x; return 0.0; }  /* unused by core   */
double atan2(double y, double x) { (void)x; (void)y; return 0.0; }
double pow(double b, double e)   { (void)b; (void)e; return 0.0; }
double fmod(double a, double b)  { (void)a; (void)b; return 0.0; }

/* ---------------------------------------------------------------
 *  time.h — 100 Hz kernel tick.
 * --------------------------------------------------------------- */
time_t time(time_t* t) {
    time_t v = (time_t)(sc0(SYS_GETTICK) / 100);
    if (t) *t = v;
    return v;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

clock_t clock(void) { return (clock_t)sc0(SYS_GETTICK); }

int mkdir(const char* path, ...) {
    (void)path;   /* flat RAMFS — directories exist on demand */
    return -1;
}

/* ---------------------------------------------------------------
 *  printf engine — the subset doomgeneric formats with:
 *  %d %i %u %x %X %o %c %s %p %f %% with flags '-'/'0'/'+',
 *  width, precision, and h/l/ll/z length prefixes (all 32-bit).
 * --------------------------------------------------------------- */
static void prn_char(char* out, size_t cap, size_t* n, char c) {
    if (*n + 1 < cap) out[*n] = c;
    (*n)++;
}

static void prn_str(char* out, size_t cap, size_t* n,
                    const char* s, int width, int prec, char pad, int left) {
    size_t len = s ? strlen(s) : 0;
    if (prec >= 0 && (size_t)prec < len) len = (size_t)prec;
    int padn = (int)width - (int)len;
    if (padn < 0) padn = 0;
    if (!left)
        for (int i = 0; i < padn; i++) prn_char(out, cap, n, pad);
    for (size_t i = 0; i < len; i++) prn_char(out, cap, n, s[i]);
    if (left)
        for (int i = 0; i < padn; i++) prn_char(out, cap, n, ' ');
}

static void prn_num(char* out, size_t cap, size_t* n,
                    unsigned long v, int neg, unsigned base,
                    int upper, int width, int prec, char pad, int left,
                    int plus) {
    char tmp[24];
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int len = 0;
    do { tmp[len++] = digits[v % base]; v /= base; } while (v);

    if (prec >= 0 && prec > len) {          /* zero-pad via precision  */
        int zp = prec - len;
        while (zp--) tmp[len++] = '0';
    }
    int signlen = (neg || plus) ? 1 : 0;
    int total = len + signlen;
    int padn = width - total;
    if (padn < 0) padn = 0;

    if (left || pad == ' ') {               /* space pad: before sign  */
        if (!left)
            for (int i = 0; i < padn; i++) prn_char(out, cap, n, ' ');
        if (neg) prn_char(out, cap, n, '-');
        else if (plus) prn_char(out, cap, n, '+');
        while (len--) prn_char(out, cap, n, tmp[len]);
        if (left)
            for (int i = 0; i < padn; i++) prn_char(out, cap, n, ' ');
    } else {                                /* '0' pad: after sign     */
        if (neg) prn_char(out, cap, n, '-');
        else if (plus) prn_char(out, cap, n, '+');
        for (int i = 0; i < padn; i++) prn_char(out, cap, n, '0');
        while (len--) prn_char(out, cap, n, tmp[len]);
    }
}

static void prn_double(char* out, size_t cap, size_t* n,
                       double v, int width, int prec, char pad, int left) {
    if (prec < 0) prec = 6;
    if (prec > 9) prec = 9;
    if (v < 0) { prn_char(out, cap, n, '-'); v = -v; }
    unsigned long ip = (unsigned long)v;
    double frac = v - (double)ip;
    double scale = 1.0;
    for (int i = 0; i < prec; i++) scale *= 10.0;
    unsigned long fp = (unsigned long)(frac * scale + 0.5);
    if (fp >= (unsigned long)scale) { ip += 1; fp = 0; }
    /* integer part (no padding rules beyond simple width support) */
    char tmp[24];
    int len = 0;
    do { tmp[len++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
    while (len--) prn_char(out, cap, n, tmp[len]);
    if (prec > 0) {
        prn_char(out, cap, n, '.');
        for (int i = prec - 1; i >= 0; i--) {
            unsigned long d = fp;
            for (int j = 0; j < i; j++) d /= 10;
            prn_char(out, cap, n, (char)('0' + d % 10));
        }
    }
    (void)width; (void)pad; (void)left;
}

int vsnprintf(char* out, size_t cap, const char* fmt, va_list ap) {
    size_t n = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { prn_char(out, cap, &n, *fmt); continue; }
        fmt++;
        /* flags */
        char pad = ' ';
        int left = 0, plus = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') pad = '0';
            else if (*fmt == '+') plus = 1;
            else break;
        }
        /* width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        /* precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9')
                prec = prec * 10 + (*fmt++ - '0');
        }
        /* length modifiers: consume, everything is 32-bit here */
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'L' ||
               *fmt == 'z' || *fmt == 'j' || *fmt == 't')
            fmt++;
        /* specifier */
        switch (*fmt) {
            case 'd':
            case 'i': {
                long v = va_arg(ap, int);
                prn_num(out, cap, &n, v < 0 ? -(unsigned long)v
                                            : (unsigned long)v,
                        v < 0, 10, 0, width, prec, pad, left, plus);
                break;
            }
            case 'u':
                prn_num(out, cap, &n, (unsigned long)va_arg(ap, unsigned),
                        0, 10, 0, width, prec, pad, left, plus);
                break;
            case 'x':
                prn_num(out, cap, &n, (unsigned long)va_arg(ap, unsigned),
                        0, 16, 0, width, prec, pad, left, plus);
                break;
            case 'X':
                prn_num(out, cap, &n, (unsigned long)va_arg(ap, unsigned),
                        0, 16, 1, width, prec, pad, left, plus);
                break;
            case 'o':
                prn_num(out, cap, &n, (unsigned long)va_arg(ap, unsigned),
                        0, 8, 0, width, prec, pad, left, plus);
                break;
            case 'c': {
                char c = (char)va_arg(ap, int);
                prn_str(out, cap, &n, &c, width, -1, pad, left);
                break;
            }
            case 's':
                prn_str(out, cap, &n, va_arg(ap, const char*),
                        width, prec, pad, left);
                break;
            case 'p': {
                void* p = va_arg(ap, void*);
                prn_char(out, cap, &n, '0');
                prn_char(out, cap, &n, 'x');
                prn_num(out, cap, &n, (unsigned long)(uintptr_t)p,
                        0, 16, 0, 8, -1, '0', 0, 0);
                break;
            }
            case 'f':
                prn_double(out, cap, &n, va_arg(ap, double),
                           width, prec, pad, left);
                break;
            case '%':
                prn_char(out, cap, &n, '%');
                break;
            case '\0':
                goto done;
            default:
                prn_char(out, cap, &n, '%');
                prn_char(out, cap, &n, *fmt);
                break;
        }
    }
done:
    if (cap) {
        size_t term = (n < cap - 1) ? n : cap - 1;
        out[term] = 0;
    }
    return (int)n;
}

int snprintf(char* out, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char* out, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char* out, const char* fmt, va_list ap) {
    return vsnprintf(out, (size_t)-1, fmt, ap);
}

/* ---------------------------------------------------------------
 *  stdio — FILE* over RAMFS fds + console streams.
 *  Read path:  SYS_OPEN/READ/LSEEK/CLOSE (fd 3+).
 *  Write path: whole-file buffering; SYS_MKFILE on fclose() (the
 *  RAMFS has no incremental write — files are created atomically).
 * --------------------------------------------------------------- */
struct morph_FILE {
    int fd;                 /* 1 = stdout, 2 = stderr, 3+ = RAMFS file */
    int is_console;
    int eof;
    int err;
    uint8_t* wbuf;          /* write-mode accumulation buffer          */
    uint32_t wlen;
    uint32_t wcap;
    char* wpath;            /* target path for fclose()               */
};

static morph_FILE _stdout_file = { 1, 1, 0, 0, NULL, 0, 0, NULL };
static morph_FILE _stderr_file = { 2, 1, 0, 0, NULL, 0, 0, NULL };

morph_FILE* stdout = &_stdout_file;
morph_FILE* stderr = &_stderr_file;

morph_FILE* fopen(const char* path, const char* mode) {
    if (!path || !mode) { errno = EINVAL; return NULL; }
    int want_write = (strchr(mode, 'w') != NULL);
    if (want_write) {
        morph_FILE* f = (morph_FILE*)calloc(1, sizeof(morph_FILE));
        if (!f) { errno = ENOMEM; return NULL; }
        f->fd = -1;
        f->wpath = strdup(path);
        if (!f->wpath) { free(f); errno = ENOMEM; return NULL; }
        return f;
    }
    int fd = sc1(SYS_OPEN, (uint32_t)(uintptr_t)path);
    if (fd < 0) { errno = ENOENT; return NULL; }
    morph_FILE* f = (morph_FILE*)calloc(1, sizeof(morph_FILE));
    if (!f) { sc1(SYS_CLOSE, (uint32_t)fd); errno = ENOMEM; return NULL; }
    f->fd = fd;
    return f;
}

morph_FILE* freopen(const char* path, const char* mode, morph_FILE* f) {
    if (f) fclose(f);
    return fopen(path, mode);
}

int fclose(morph_FILE* f) {
    if (!f) return 0;
    int rc = 0;
    if (f->wpath) {                       /* buffered write file      */
        if (f->wbuf && f->wlen > 0)
            rc = sc3(SYS_MKFILE, (uint32_t)(uintptr_t)f->wpath,
                     (uint32_t)(uintptr_t)f->wbuf, f->wlen);
        free(f->wbuf);
        free(f->wpath);
    } else if (f->fd >= 3) {
        sc1(SYS_CLOSE, (uint32_t)f->fd);
    }
    if (f != &_stdout_file && f != &_stderr_file)
        free(f);
    return rc;
}

size_t fread(void* buf, size_t size, size_t n, morph_FILE* f) {
    if (!f || !buf || size == 0 || n == 0) return 0;
    if (f->wpath || f->fd < 0) { f->err = 1; return 0; }
    size_t total = size * n;
    if (total == 0) return 0;
    int got = sc3(SYS_READ, (uint32_t)f->fd,
                  (uint32_t)(uintptr_t)buf, (uint32_t)total);
    if (got <= 0) {
        f->eof = (got == 0);
        return 0;
    }
    return (size_t)got / size;
}

size_t fwrite(const void* buf, size_t size, size_t n, morph_FILE* f) {
    if (!f || !buf || size == 0 || n == 0) return 0;
    size_t total = size * n;
    if (f->is_console) {
        int put = sc3(SYS_WRITE, (uint32_t)f->fd,
                      (uint32_t)(uintptr_t)buf, (uint32_t)total);
        return (put > 0) ? (size_t)put / size : 0;
    }
    if (!f->wpath) { f->err = 1; return 0; }       /* opened read-only */
    if (f->wlen + (uint32_t)total > f->wcap) {
        uint32_t ncap = f->wcap ? f->wcap * 2u : 4096u;
        while (ncap < f->wlen + (uint32_t)total) ncap *= 2u;
        uint8_t* nb = (uint8_t*)realloc(f->wbuf, ncap);
        if (!nb) { f->err = 1; return 0; }
        f->wbuf = nb;
        f->wcap = ncap;
    }
    memcpy(f->wbuf + f->wlen, buf, total);
    f->wlen += (uint32_t)total;
    return n;
}

int fseek(morph_FILE* f, long off, int whence) {
    if (!f || f->wpath || f->fd < 3) return -1;
    if (whence < 0 || whence > 2) { errno = EINVAL; return -1; }
    int pos = sc3(SYS_LSEEK, (uint32_t)f->fd, (uint32_t)off,
                  (uint32_t)whence);
    if (pos < 0) { f->err = 1; return -1; }
    f->eof = 0;
    return 0;
}

long ftell(morph_FILE* f) {
    if (!f || f->wpath || f->fd < 3) return -1;
    return (long)sc3(SYS_LSEEK, (uint32_t)f->fd, 0, 1 /* SEEK_CUR */);
}

int feof(morph_FILE* f)   { return f ? f->eof : 0; }
int ferror(morph_FILE* f) { return f ? f->err : 0; }
int fflush(morph_FILE* f) {
    /* console writes are unbuffered at the syscall level; buffered
     * files commit on fclose. Nothing to do here. */
    (void)f;
    return 0;
}

int fgetc(morph_FILE* f) {
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) return EOF;
    return c;
}

int getc(morph_FILE* f) { return fgetc(f); }

int fputc(int c, morph_FILE* f) {
    unsigned char b = (unsigned char)c;
    if (fwrite(&b, 1, 1, f) != 1) return EOF;
    return b;
}

int putc(int c, morph_FILE* f) { return fputc(c, f); }

int fputs(const char* s, morph_FILE* f) {
    if (!s) return EOF;
    size_t n = strlen(s);
    return (fwrite(s, 1, n, f) == n) ? 0 : EOF;
}

int puts(const char* s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return (fputc('\n', stdout) == EOF) ? EOF : 0;
}

int putchar(int c) { return fputc(c, stdout); }

void perror(const char* s) {
    if (s && *s) fprintf(stderr, "%s: ", s);
    fprintf(stderr, "errno %d\n", errno);
}

/* ---- printf family ------------------------------------------- */
static int vfprintf_core(morph_FILE* f, const char* fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t len = (n < 0) ? 0 : ((size_t)n >= sizeof(buf) ? sizeof(buf) - 1
                                                         : (size_t)n);
    fwrite(buf, 1, len, f);
    return n;
}

int vfprintf(morph_FILE* f, const char* fmt, va_list ap) {
    return vfprintf_core(f, fmt, ap);
}

int vprintf(const char* fmt, va_list ap) {
    return vfprintf_core(stdout, fmt, ap);
}

int fprintf(morph_FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf_core(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf_core(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int remove(const char* path) { (void)path; errno = EPERM; return -1; }
int rename(const char* a, const char* b) { (void)a; (void)b; errno = EPERM; return -1; }

/* ---- sscanf (subset: %x %i %d %u %s %c, width, literals) ------ */
static int scan_int(const char** sp, int base, int* out) {
    const char* s = *sp;
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = (*s++ == '-');
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0) base = (*s == '0') ? 8 : 10;
    long v = 0;
    int any = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        any = 1;
    }
    if (!any) return 0;
    *out = (int)(neg ? -v : v);
    *sp = s;
    return 1;
}

int vsscanf(const char* str, const char* fmt, va_list ap) {
    int matched = 0;
    const char* s = str;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*fmt != '%') {
            if (*s++ != *fmt) return matched;
            continue;
        }
        fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'z') fmt++;
        int* ip;
        char* cp;
        switch (*fmt) {
            case 'x':
            case 'X':
                ip = va_arg(ap, int*);
                if (scan_int(&s, 16, ip)) matched++;
                else return matched;
                break;
            case 'i':
            case 'd':
                ip = va_arg(ap, int*);
                if (scan_int(&s, *fmt == 'i' ? 0 : 10, ip)) matched++;
                else return matched;
                break;
            case 'u':
                ip = va_arg(ap, int*);
                if (scan_int(&s, 10, ip)) matched++;
                else return matched;
                break;
            case 's': {
                cp = va_arg(ap, char*);
                while (isspace((unsigned char)*s)) s++;
                if (!*s) return matched;
                int w = width ? width : 2147483647;
                while (*s && !isspace((unsigned char)*s) && w--) *cp++ = *s++;
                *cp = 0;
                matched++;
                break;
            }
            case 'c': {
                cp = va_arg(ap, char*);
                int w = width ? width : 1;
                while (w-- && *s) *cp++ = *s++;
                matched++;
                break;
            }
            case '%':
                if (*s++ != '%') return matched;
                break;
            case '\0':
                return matched;
            default:               /* %[ and friends: unsupported      */
                return matched;
        }
    }
    return matched;
}

int sscanf(const char* str, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

int fscanf(morph_FILE* f, const char* fmt, ...) {
    /* Only reached if a config file exists in the RAMFS; DOOM's config
     * loader tolerates a failed parse by using defaults. */
    (void)f; (void)fmt;
    return EOF;
}

void __assert_fail(const char* expr, const char* file, int line) {
    fprintf(stderr, "assert failed: %s (%s:%d)\n", expr, file, line);
    abort();
}

int system(const char* cmd) {
    (void)cmd;
    return -1;   /* no process launching on Equinox OS */
}

double atof(const char* s) {
    /* simple decimal parse — config floats like mouse_acceleration */
    double v = 0.0, frac = 0.0, scale = 0.1;
    int neg = 0, seen = 0;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '+' || *s == '-') neg = (*s++ == '-');
    while (isdigit((unsigned char)*s)) { v = v * 10.0 + (*s++ - '0'); seen = 1; }
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            frac += (double)(*s++ - '0') * scale;
            scale *= 0.1;
            seen = 1;
        }
    }
    if (!seen) return 0.0;
    v += frac;
    return neg ? -v : v;
}
