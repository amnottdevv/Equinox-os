/* libc/stdio.c - stdio FILE I/O on RAMFS (fopen/fread/fwrite/fseek/fclose + extras).
   Part of the Equinox OS libc (spliced via <morph.h>).
   Dependencies: memory, string, heap. Include guard keeps the module
   safe under double inclusion. */
#ifndef LIBC_STDIO_C
#define LIBC_STDIO_C
#include "/equinox/libc/memory.c"                /* dependency */
#include "/equinox/libc/string.c"                /* dependency */
#include "/equinox/libc/heap.c"                  /* dependency */

/* ============ stdio FILE I/O (RAMFS) ============
   fopen mode "r" = direct fd (fseek via the lseek syscall).
   mode "w"/"a" = dynamic write-buffer; fclose writes the whole
   file via file_write. Handle = 1..8 (0 = error/NULL). */
int __fio_fd[8];
int __fio_mode[8];
int __fio_len[8];
int __fio_cap[8];
int __fio_buf[8];
char __fio_path[512];
int __fio_alloc() {
    int i;
    i = 0;
    while (i < 8) {
        if (__fio_mode[i] == 0) return i;
        i = i + 1;
    }
    return -1;
}
int fopen(char* path, char* mode = 0) {
    int s; int fd; int n; char* b; char m;
    s = __fio_alloc();
    if (s < 0) { print("fopen: too many open files\n"); return 0; }
    m = 'r';
    if (mode) m = mode[0];
    if (m == 'w' || m == 'a') {
        b = malloc(4096);
        if (!b) return 0;
        __fio_buf[s] = b;
        __fio_cap[s] = 4096;
        __fio_len[s] = 0;
        strcpy(__fio_path + s * 64, path);
        if (m == 'a') {
            n = file_size(path);
            if (n > 0) {
                if (n + 16 > 4096) {
                    free(b);
                    b = malloc(n + 16);
                    if (!b) return 0;
                    __fio_buf[s] = b;
                    __fio_cap[s] = n + 16;
                }
                n = file_read_all(path, b, n);
                if (n < 0) n = 0;
                __fio_len[s] = n;
            }
        }
        __fio_mode[s] = 2;
        __fio_fd[s] = -1;
        return s + 1;
    }
    fd = file_open(path);
    if (fd < 0) return 0;
    __fio_fd[s] = fd;
    __fio_mode[s] = 1;
    __fio_len[s] = 0;
    return s + 1;
}
int fread(char* buf, int sz, int n, int f) {
    int s; int total; int got;
    if (f <= 0) return 0;
    s = f - 1;
    if (__fio_mode[s] != 1) return 0;
    total = sz * n;
    if (total <= 0) return 0;
    got = file_read(__fio_fd[s], buf, total);
    if (got < 0) return 0;
    return got;
}
int fwrite(char* buf, int sz, int n, int f) {
    int s; int total; int ncap; char* nb;
    if (f <= 0) return 0;
    s = f - 1;
    if (__fio_mode[s] != 2) return 0;
    total = sz * n;
    if (total <= 0) return 0;
    while (__fio_len[s] + total > __fio_cap[s]) {
        ncap = __fio_cap[s] * 2;
        nb = malloc(ncap);
        if (!nb) return 0;
        memcpy(nb, __fio_buf[s], __fio_len[s]);
        free(__fio_buf[s]);
        __fio_buf[s] = nb;
        __fio_cap[s] = ncap;
    }
    memcpy(__fio_buf[s] + __fio_len[s], buf, total);
    __fio_len[s] = __fio_len[s] + total;
    return total;
}
int fseek(int f, int off, int whence) {
    int s; int npos;
    if (f <= 0) return -1;
    s = f - 1;
    if (__fio_mode[s] == 1) {
        lseek(__fio_fd[s], off, whence);
        return 0;
    }
    if (__fio_mode[s] == 2) {
        npos = off;
        if (whence == 1) npos = __fio_len[s] + off;
        else if (whence == 2) npos = __fio_len[s] + off;
        if (npos < 0) npos = 0;
        if (npos > __fio_len[s]) npos = __fio_len[s];
        __fio_len[s] = npos;
        return 0;
    }
    return -1;
}
int ftell(int f) {
    int s;
    if (f <= 0) return -1;
    s = f - 1;
    if (__fio_mode[s] == 1) return lseek(__fio_fd[s], 0, 1);
    return __fio_len[s];
}
int fclose(int f) {
    int s; int r;
    if (f <= 0) return -1;
    s = f - 1;
    r = 0;
    if (__fio_mode[s] == 1) {
        r = file_close(__fio_fd[s]);
    } else if (__fio_mode[s] == 2) {
        r = file_write(__fio_path + s * 64, __fio_buf[s], __fio_len[s]);
        free(__fio_buf[s]);
    }
    __fio_mode[s] = 0;
    __fio_fd[s] = 0;
    __fio_len[s] = 0;
    __fio_cap[s] = 0;
    __fio_buf[s] = 0;
    return r;
}
/* ================= stdio extras (FR-19) ================= */
int puts(char* s) {
    print(s);
    print("\n");
    return 0;
}
int fputs(char* s, int f) {
    return fwrite(s, 1, strlen(s), f);
}
int fputc(int c, int f) {
    char b[2];
    b[0] = c;
    b[1] = 0;
    if (fwrite(b, 1, 1, f) != 1) return -1;
    return c;
}
int fgetc(int f) {
    char b[1];
    int n;
    n = fread(b, 1, 1, f);
    if (n < 1) return -1;
    return b[0] & 255;
}
int remove(char* path) {
    return __sys_unlink1(path);
}
int rename(char* oldp, char* newp) {
    return __sys_rename2(oldp, newp);
}
char* strerror(int e) {
    if (e == -1) return "EPERM: operation not permitted";
    if (e == -3) return "ENOENT: no such file or directory";
    if (e == -4) return "EISDIR: path is a directory";
    if (e == -5) return "ENOMEM: out of memory";
    if (e == -6) return "EINVAL: invalid argument";
    if (e == -7) return "ENOTSUP: operation not supported";
    if (e == -8) return "EMFILE: too many open files";
    if (e == -9) return "EBUSY: resource busy";
    if (e == -10) return "EFAULT: bad user pointer";
    if (e == -11) return "EIO: I/O error";
    if (e == -12) return "EEXIST: file already exists";
    if (e == -13) return "ECHILD: no such child";
    return "unknown error";
}

/* ============ term: ANSI escape (v0.9.3) ============
   Konsol kernel mem-parse ESC[... (SGR warna, J clear, H kursor,
   K hapus baris); byte mentah tetap diteruskan ke serial. Helper
   ini membuat program .mrp bisa berwarna kaya bahasa lain. */
#define ANSI_BLACK    0
#define ANSI_RED      1
#define ANSI_GREEN    2
#define ANSI_YELLOW   3
#define ANSI_BLUE     4
#define ANSI_MAGENTA  5
#define ANSI_CYAN     6
#define ANSI_WHITE    7

void clear_screen() {
    char b[8];
    b[0] = 27; b[1] = '['; b[2] = '2'; b[3] = 'J'; b[4] = 0;
    print(b);
    b[0] = 27; b[1] = '['; b[2] = 'H'; b[3] = 0;
    print(b);
}
int __ansi_putint(char* b, int i, int v) {
    char tmp[8];
    int n = 0;
    if (v < 1) v = 1;
    if (v > 999) v = 999;
    while (v > 0) { tmp[n] = 48 + v % 10; n = n + 1; v = v / 10; }
    while (n > 0) { n = n - 1; b[i] = tmp[n]; i = i + 1; }
    return i;
}
void ansi_reset() {
    char b[6];
    b[0] = 27; b[1] = '['; b[2] = '0'; b[3] = 'm'; b[4] = 0;
    print(b);
}
void ansi_bold() {
    char b[6];
    b[0] = 27; b[1] = '['; b[2] = '1'; b[3] = 'm'; b[4] = 0;
    print(b);
}
void ansi_fg(int c) {
    char b[6];
    b[0] = 27; b[1] = '['; b[2] = '3'; b[3] = 48 + (c & 7); b[4] = 'm'; b[5] = 0;
    print(b);
}
void ansi_fg_bright(int c) {
    char b[6];
    b[0] = 27; b[1] = '['; b[2] = '9'; b[3] = 48 + (c & 7); b[4] = 'm'; b[5] = 0;
    print(b);
}
void ansi_bg(int c) {
    char b[6];
    b[0] = 27; b[1] = '['; b[2] = '4'; b[3] = 48 + (c & 7); b[4] = 'm'; b[5] = 0;
    print(b);
}
void ansi_default() {
    char b[8];
    b[0] = 27; b[1] = '['; b[2] = '3'; b[3] = '9'; b[4] = ';'; b[5] = '4'; b[6] = '9'; b[7] = 'm';
    b[8] = 0;
    print(b);
}
void ansi_goto(int row, int col) {
    char b[24];
    int i = 0;
    b[i] = 27; i = i + 1;
    b[i] = '['; i = i + 1;
    i = __ansi_putint(b, i, row);
    b[i] = ';'; i = i + 1;
    i = __ansi_putint(b, i, col);
    b[i] = 'H'; i = i + 1;
    b[i] = 0;
    print(b);
}
void ansi_clear_line() {
    char b[6];
    b[0] = 27; b[1] = '['; b[2] = '2'; b[3] = 'K'; b[4] = 0;
    print(b);
}
#endif
