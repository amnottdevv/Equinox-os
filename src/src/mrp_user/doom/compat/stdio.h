/* compat/stdio.h — freestanding stdio shim for Equinox OS (doomgeneric).
 * FILE* wraps a RAMFS fd from the Equinox OS syscall layer (SYS_OPEN/READ/
 * LSEEK/CLOSE) or one of the console streams (fd 1 = stdout, 2 = stderr).
 * Write-mode files ("w"/"wb") are buffered in memory and flushed to the
 * RAMFS as a whole file on fclose() — Equinox OS files are create-whole,
 * there is no incremental write path (SYS_MKFILE). That is enough for
 * DOOM's savegames / default.cfg / statdump outputs. */
#ifndef MORPH_COMPAT_STDIO_H
#define MORPH_COMPAT_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct morph_FILE morph_FILE;
#define FILE morph_FILE

#define EOF    (-1)
#define BUFSIZ 4096

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern morph_FILE* stdout;
extern morph_FILE* stderr;

morph_FILE* fopen(const char* path, const char* mode);
morph_FILE* freopen(const char* path, const char* mode, morph_FILE* f);
int   fclose(morph_FILE* f);
size_t fread(void* buf, size_t size, size_t n, morph_FILE* f);
size_t fwrite(const void* buf, size_t size, size_t n, morph_FILE* f);
int   fseek(morph_FILE* f, long off, int whence);
long  ftell(morph_FILE* f);
int   feof(morph_FILE* f);
int   ferror(morph_FILE* f);
int   fflush(morph_FILE* f);
int   fgetc(morph_FILE* f);
int   getc(morph_FILE* f);
int   fputc(int c, morph_FILE* f);
int   putc(int c, morph_FILE* f);
int   fputs(const char* s, morph_FILE* f);
int   puts(const char* s);
int   putchar(int c);
void  perror(const char* s);

int printf(const char* fmt, ...);
int fprintf(morph_FILE* f, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t n, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(morph_FILE* f, const char* fmt, va_list ap);
int vsprintf(char* buf, const char* fmt, va_list ap);
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);

int sscanf(const char* str, const char* fmt, ...);
int fscanf(morph_FILE* f, const char* fmt, ...);

int remove(const char* path);
int rename(const char* oldpath, const char* newpath);

#endif /* MORPH_COMPAT_STDIO_H */
