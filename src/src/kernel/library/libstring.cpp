#include "header/libstring.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

// ======================== MEMORY ========================
void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void* memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* a = (const uint8_t*)s1;
    const uint8_t* b = (const uint8_t*)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

// ======================== STRING ========================
size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

int starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while (*src) { *d = *src; d++; src++; }
    *d = '\0';
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while (*src) { *d = *src; d++; src++; }
    *d = '\0';
    return dest;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return NULL;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!needle[0]) return (char*)haystack;
    for (const char* h = haystack; *h; h++) {
        const char* hh = h;
        const char* nn = needle;
        while (*hh && *nn && *hh == *nn) { hh++; nn++; }
        if (!*nn) return (char*)h;
    }
    return NULL;
}

static char* strtok_last = NULL;
char* strtok(char* str, const char* delim) {
    if (str) strtok_last = str;
    if (!strtok_last) return NULL;
    char* start = strtok_last;
    while (*start && strchr(delim, *start)) start++;
    if (*start == '\0') { strtok_last = NULL; return NULL; }
    char* end = start;
    while (*end && !strchr(delim, *end)) end++;
    if (*end) { *end = '\0'; strtok_last = end + 1; }
    else strtok_last = NULL;
    return start;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i = 0;
    while (src[i] && i < n) { dest[i] = src[i]; i++; }
    while (i < n) { dest[i] = '\0'; i++; }
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    size_t dest_len = strlen(dest);
    size_t i = 0;
    while (src[i] && i < n) { dest[dest_len + i] = src[i]; i++; }
    dest[dest_len + i] = '\0';
    return dest;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    return 0;
}

size_t strspn(const char* s, const char* accept) {
    size_t count = 0;
    while (s[count] && strchr(accept, s[count])) count++;
    return count;
}

size_t strcspn(const char* s, const char* reject) {
    size_t count = 0;
    while (s[count] && !strchr(reject, s[count])) count++;
    return count;
}

char* strpbrk(const char* s, const char* accept) {
    while (*s) {
        if (strchr(accept, *s)) return (char*)s;
        s++;
    }
    return NULL;
}

// ======================== CASE CONVERSION ========================
char* strlwr(char* str) {
    char* p = str;
    while (*p) {
        if (*p >= 'A' && *p <= 'Z') *p = *p - 'A' + 'a';
        p++;
    }
    return str;
}

char* strupr(char* str) {
    char* p = str;
    while (*p) {
        if (*p >= 'a' && *p <= 'z') *p = *p - 'a' + 'A';
        p++;
    }
    return str;
}

// ======================== SPLIT ========================
int split(char* str, const char* delim, char** tokens, int max_tokens) {
    int count = 0;
    char* token = strtok(str, delim);
    while (token && count < max_tokens) {
        tokens[count++] = token;
        token = strtok(NULL, delim);
    }
    return count;
}

// ======================== SPRINTF / SNPRINTF ========================
static void append_char(char** buffer, char c) {
    **buffer = c;
    (*buffer)++;
}

static void append_string(char** buffer, const char* s) {
    while (*s) { append_char(buffer, *s); s++; }
}

static void append_int(char** buffer, int num, int base, int width, char pad) {
    char temp[32];
    int i = 0;
    int is_negative = 0;
    if (num < 0 && base == 10) { is_negative = 1; num = -num; }
    if (num == 0) temp[i++] = '0';
    else {
        while (num > 0) {
            int digit = num % base;
            temp[i++] = (digit < 10) ? '0' + digit : 'A' + (digit - 10);
            num /= base;
        }
    }
    int len = i;
    int pad_len = width - len;
    if (is_negative) pad_len--;
    while (pad_len > 0) { append_char(buffer, pad); pad_len--; }
    if (is_negative) append_char(buffer, '-');
    while (i > 0) append_char(buffer, temp[--i]);
}

/* Unsigned version of append_int — needed because append_int
 * treats its argument as signed (cast (int)val for %u/%x was
 * wrong for values > INT_MAX: sign-extended to negative, then
 * the while (num > 0) loop exits immediately -> empty output). */
static void append_uint(char** buffer, unsigned int num, int base, int width, char pad) {
    char temp[32];
    int i = 0;
    if (num == 0) temp[i++] = '0';
    else {
        while (num > 0) {
            unsigned int digit = num % (unsigned int)base;
            temp[i++] = (digit < 10) ? '0' + digit : 'A' + (digit - 10);
            num /= (unsigned int)base;
        }
    }
    int len = i;
    int pad_len = width - len;
    while (pad_len > 0) { append_char(buffer, pad); pad_len--; }
    while (i > 0) append_char(buffer, temp[--i]);
}

int sprintf(char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char* buf_ptr = buffer;
    for (const char* p = format; *p; p++) {
        if (*p == '%') {
            p++;
            int width = 0; char pad = ' ';
            if (*p == '0') { pad = '0'; p++; }
            while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
            switch (*p) {
                case 'd': { int val = va_arg(args, int); append_int(&buf_ptr, val, 10, width, pad); break; }
                case 'u': { unsigned int val = va_arg(args, unsigned int); append_uint(&buf_ptr, val, 10, width, pad); break; }
                case 'x': { unsigned int val = va_arg(args, unsigned int); append_uint(&buf_ptr, val, 16, width, pad); break; }
                case 'X': { unsigned int val = va_arg(args, unsigned int); append_uint(&buf_ptr, val, 16, width, pad); break; }
                case 's': { const char* s = va_arg(args, const char*); append_string(&buf_ptr, s); break; }
                case 'c': { char c = (char)va_arg(args, int); append_char(&buf_ptr, c); break; }
                case '%': { append_char(&buf_ptr, '%'); break; }
                default: append_char(&buf_ptr, '%'); append_char(&buf_ptr, *p);
            }
        } else append_char(&buf_ptr, *p);
    }
    append_char(&buf_ptr, '\0');
    va_end(args);
    return buf_ptr - buffer - 1;
}

// ---- Bounded emit helpers specific to snprintf (stack overflow FIX) ----
static void sn_emit_char(char** out, char* end, char c) {
    if (*out < end) { **out = c; (*out)++; }
}
static void sn_emit_str(char** out, char* end, const char* s) {
    if (!s) s = "(null)";
    while (*s) { sn_emit_char(out, end, *s); s++; }
}
static void sn_emit_pad(char** out, char* end, int n, char c) {
    for (int i = 0; i < n; i++) sn_emit_char(out, end, c);
}
static void sn_emit_uint(char** out, char* end, unsigned int num, int base, int width, char pad) {
    char tmp[12];
    int i = 0;
    if (num == 0) tmp[i++] = '0';
    else while (num) {
        unsigned int d = num % (unsigned int)base;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        num /= (unsigned int)base;
    }
    if (width > i) sn_emit_pad(out, end, width - i, pad);
    while (i > 0) sn_emit_char(out, end, tmp[--i]);
}
static void sn_emit_int(char** out, char* end, int num, int base, int width, char pad) {
    if (num < 0 && base == 10) {
        sn_emit_char(out, end, '-');
        if (width > 1) width--;
        sn_emit_uint(out, end, (unsigned int)(-(unsigned int)num), base, width, pad);
    } else {
        sn_emit_uint(out, end, (unsigned int)num, base, width, pad);
    }
}

int snprintf(char* buffer, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);

    /* FIX (fatal): the old version rendered into temp[256] WITHOUT a
     * per-append bound check, then copied min(size, ...) into the buffer.
     * Two problems:
     *  (1) a caller with size > 256 (filemanager: buf[300]) still blew
     *      up the temp[256] on the stack;
     *  (2) one long %s conversion writes hundreds of bytes before the
     *      loop-level check gets a chance to run.
     * The new version writes directly into the caller's buffer with
     * bounded per-character emit — no intermediate buffer, no possible
     * overflow, always NUL-terminated (C99-ish). */
    if (!buffer || size == 0) { va_end(args); return 0; }

    char* out = buffer;
    char* end = buffer + size - 1;   // last slot that may be written

    for (const char* p = format; *p; p++) {
        if (*p == '%') {
            p++;
            int width = 0; char pad = ' ';
            if (*p == '0') { pad = '0'; p++; }
            while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
            /* consume the z/l/h length modifier — on i386 the arg width is the same */
            if (*p == 'z' || *p == 'l' || *p == 'h') {
                p++;
                if (*p == 'h') p++;
            }
            switch (*p) {
                case 'd': sn_emit_int(&out, end, va_arg(args, int), 10, width, pad); break;
                case 'u': sn_emit_uint(&out, end, va_arg(args, unsigned int), 10, width, pad); break;
                case 'x':
                case 'X': sn_emit_uint(&out, end, va_arg(args, unsigned int), 16, width, pad); break;
                case 's': sn_emit_str(&out, end, va_arg(args, const char*)); break;
                case 'c': sn_emit_char(&out, end, (char)va_arg(args, int)); break;
                case '%': sn_emit_char(&out, end, '%'); break;
                case '\0': sn_emit_char(&out, end, '%'); goto done;
                default:
                    sn_emit_char(&out, end, '%');
                    sn_emit_char(&out, end, *p);
            }
        } else {
            sn_emit_char(&out, end, *p);
        }
    }
done:
    *out = '\0';   // out <= end always -> in-bounds
    va_end(args);
    return (int)(out - buffer);
}

int sscanf(const char* str, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int count = 0;
    const char* p = format;
    const char* s = str;
    while (*p) {
        if (*p == '%') {
            p++;
            if (*p == 'd') {
                int* out = va_arg(args, int*);
                int val = 0, sign = 1;
                while (*s == ' ') s++;
                if (*s == '-') { sign = -1; s++; }
                while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
                *out = val * sign;
                count++;
            } else if (*p == 's') {
                char* out = va_arg(args, char*);
                while (*s == ' ') s++;
                while (*s && *s != ' ' && *s != '\t' && *s != '\n') { *out = *s; out++; s++; }
                *out = '\0';
                count++;
            } else if (*p == 'c') {
                char* out = va_arg(args, char*);
                while (*s == ' ') s++;
                *out = *s;
                if (*s) s++;
                count++;
            }
            p++;
        } else {
            if (*p != *s) { va_end(args); return count; }
            p++; s++;
        }
    }
    va_end(args);
    return count;
}