/* libc.c - v10.8 libc prelude regression (mtcc + #include <morph.h>)
   Covers: string, memory, conversion, user heap (malloc/free/calloc/
   realloc + coalesce), printf family, stdio FILE I/O (r/w/a + seek),
   lseek, qsort_int/qsort_str, time, getenv, ring().
   Each stage prints "ok <n> <name>"; failures print "FAIL ..." and
   bump the counter. Final line: "SUM <stages> <fails>" for OCR. */

#include <morph.h>

#define MAGIC_ANSWER 42

int fails;
int stages;

void ck(int cond, char* name) {
    stages = stages + 1;
    if (cond) {
        printf("ok %d %s\n", stages, name);
    } else {
        printf("FAIL %d %s\n", stages, name);
        fails = fails + 1;
    }
}

int main() {
    char buf[128];
    char b2[64];
    char* s;
    char* p;
    int i;
    int n;
    int* ip;
    int* jp;
    int f;
    int r;
    int arr[32];

    fails = 0;
    stages = 0;

    /* ---- preprocessor: #define substitution ---- */
    ck(MAGIC_ANSWER == 42, "define");

    /* ---- strlen / strcmp / strncmp ---- */
    ck(strlen("") == 0 && strlen("morph") == 5, "strlen");
    ck(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0,
       "strcmp");
    ck(strncmp("abcdef", "abcxyz", 3) == 0 && strncmp("abc", "abd", 3) < 0,
       "strncmp");

    /* ---- strcpy / strncpy / strcat / strncat ---- */
    strcpy(buf, "hello");
    ck(strlen(buf) == 5 && buf[0] == 'h' && buf[4] == 'o', "strcpy");
    strncpy(b2, "abcdef", 3);
    ck(b2[0] == 'a' && b2[1] == 'b' && b2[2] == 'c' && b2[3] == 0, "strncpy-term");
    strcat(buf, " world");
    ck(strcmp(buf, "hello world") == 0, "strcat");
    strcpy(buf, "ab");
    strncat(buf, "cdefg", 3);
    ck(strcmp(buf, "abcde") == 0, "strncat");

    /* ---- strchr / strrchr / strstr ---- */
    strcpy(buf, "morph/os/mr");
    p = strchr(buf, '/');
    ck(p == buf + 5, "strchr");
    p = strrchr(buf, '/');
    ck(p == buf + 8, "strrchr");
    p = strstr(buf, "os");
    ck(p == buf + 6, "strstr");
    ck(strstr(buf, "zzz") == 0, "strstr-miss");

    /* ---- memcpy / memmove / memcmp / memchr ---- */
    strcpy(buf, "0123456789");
    memcpy(b2, buf, 6);
    ck(b2[0] == '0' && b2[5] == '5', "memcpy");
    strcpy(buf, "abcdef");
    memmove(buf + 2, buf, 4);      /* forward overlap */
    ck(buf[2] == 'a' && buf[5] == 'd', "memmove");
    ck(memcmp("abc", "abc", 3) == 0 && memcmp("abc", "abd", 3) < 0, "memcmp");
    strcpy(buf, "xyz");
    p = memchr(buf, 'y', 3);
    ck(p == buf + 1 && memchr(buf, 'q', 3) == 0, "memchr");

    /* ---- atoi / strtol ---- */
    ck(atoi("  -42") == -42 && atoi("1337x") == 1337, "atoi");
    ck(strtol("0x1F", 0, 16) == 31, "strtol-hex");
    ck(strtol("0x1F", 0, 0) == 31, "strtol-autodetect");
    ck(strtol("0755", 0, 0) == 493, "strtol-octal");
    ck(strtol("  -12abc", &p, 10) == -12 && *p == 'a', "strtol-endp");
    ck(strtol("zz", &p, 10) == 0 && p[0] == 'z' && p[1] == 'z', "strtol-nodigit");

    /* ---- itoa ---- */
    itoa(-1234, buf, 10);
    ck(strcmp(buf, "-1234") == 0, "itoa");
    itoa(255, buf, 16);
    ck(strcmp(buf, "ff") == 0, "itoa-hex");

    /* ---- user heap: malloc / free / calloc / realloc ---- */
    ip = malloc(100);
    ck(ip != 0, "malloc");
    for (i = 0; i < 25; i = i + 1) ip[i] = i * i;
    ck(ip[24] == 576, "malloc-write");
    free(ip);

    /* many small allocations: 60x200-byte blocks, all freed, then one
       big block — without coalescing it fails at the first 64KB chunk */
    for (i = 0; i < 60; i = i + 1) {
        jp = malloc(200);
        jp[0] = i;
        free(jp);
    }
    ip = malloc(60000);
    ck(ip != 0, "heap-coalesce");
    if (ip) {
        ip[14999] = 77;
        ck(ip[14999] == 77, "heap-bigwrite");
        free(ip);
    }

    jp = calloc(16, 4);
    n = 0;
    for (i = 0; i < 16; i = i + 1) n = n + jp[i];
    ck(n == 0, "calloc-zero");
    jp[0] = 11;
    jp[1] = 22;
    jp = realloc(jp, 400);
    ck(jp != 0 && jp[0] == 11 && jp[1] == 22, "realloc-copy");
    free(jp);

    /* ---- printf (syscall 28, max 3 conversion args) ---- */
    printf("printf-check d=%d s=%s x=%x\n", -42, "str", 48879);
    printf("printf-c c=%c pct=%% neg=%d\n", 'Q', -7);
    printf("printf-width %05d|%-6d|\n", 42, 7);
    printf("printf-one-arg works\n");

    /* ---- snprintf / sprintf ---- */
    n = snprintf(buf, 8, "cut-%d", 123456);
    ck(n == 10 && strlen(buf) == 7, "snprintf-trunc");
    n = snprintf(buf, 100, "%d %s %x", -7, "ok", 255);
    ck(strcmp(buf, "-7 ok ff") == 0 && n == 8, "snprintf-fmt");
    sprintf(buf, "%04d-%s", 9, "end");
    ck(strcmp(buf, "0009-end") == 0, "sprintf");

    /* ---- stdio FILE I/O ---- */
    f = fopen("libcio.dat", "w");
    ck(f != 0, "fopen-w");
    r = fwrite("hello file io", 1, 13, f);
    ck(r == 13, "fwrite");
    r = fclose(f);
    ck(r == 0, "fclose-w");

    ck(file_exists("libcio.dat") == 1 && file_size("libcio.dat") == 13,
       "file-created");

    f = fopen("libcio.dat", "r");
    ck(f != 0, "fopen-r");
    for (i = 0; i < 64; i = i + 1) buf[i] = 0;
    r = fread(buf, 1, 64, f);
    ck(r == 13 && strcmp(buf, "hello file io") == 0, "fread-all");
    r = fseek(f, 6, 0);
    ck(r == 0, "fseek-set");
    ck(ftell(f) == 6, "ftell");
    for (i = 0; i < 64; i = i + 1) buf[i] = 0;
    r = fread(buf, 1, 4, f);
    ck(r == 4 && strcmp(buf, "file") == 0, "fread-after-seek");
    r = fseek(f, -2, 1);
    ck(r == 0 && ftell(f) == 8, "fseek-cur-neg");
    fclose(f);

    /* append mode: old contents + addition */
    f = fopen("libcio.dat", "a");
    fwrite("!", 1, 1, f);
    fclose(f);
    ck(file_size("libcio.dat") == 14, "append-mode");

    /* fseek truncation on the write buffer */
    f = fopen("libcio.dat", "w");
    fwrite("abcdefgh", 1, 8, f);
    fseek(f, 3, 0);
    fwrite("XY", 1, 2, f);
    fclose(f);
    n = file_read_all("libcio.dat", buf, 32);
    ck(n == 5 && buf[0] == 'a' && buf[3] == 'X' && buf[4] == 'Y', "w-seek-truncate");

    /* ---- lseek langsung (POSIX style) ---- */
    n = open("libcio.dat");
    ck(n >= 3, "open-fd");
    r = lseek(n, 1, 0);
    ck(r == 1, "lseek-set");
    r = lseek(n, 0, 2);
    ck(r == 5, "lseek-end");
    r = read(n, buf, 4);
    ck(r == 0, "read-eof");
    close(n);

    /* ---- qsort_int / qsort_str ---- */
    for (i = 0; i < 32; i = i + 1) arr[i] = (i * 37) % 101;
    qsort_int(arr, 32);
    n = 1;
    for (i = 1; i < 32; i = i + 1) if (arr[i - 1] > arr[i]) n = 0;
    ck(n == 1, "qsort_int");

    /* mtcc subset: array-of-pointer not yet supported — use an int[]
       holding the string addresses (loose int<->pointer assignment). */
    for (i = 0; i < 5; i = i + 1) arr[i] = 0;
    arr[0] = "pear"; arr[1] = "apple"; arr[2] = "fig";
    arr[3] = "date"; arr[4] = "banana";
    qsort_str(arr, 5);
    ck(strcmp(arr[0], "apple") == 0 && strcmp(arr[4], "pear") == 0, "qsort_str");

    /* ---- time / getenv / ring ---- */
    ck(time() >= 0, "time");
    ck(getenv("PATH") == 0, "getenv-null");
    ck(ring() == 3, "ring3-selfcheck");

    /* ---- ring 3 cannot mess with selectors:
       (report only; the kernel shell must say ring 0) ---- */

    printf("SUM stages=%d fails=%d\n", stages, fails);
    if (fails == 0) print("LIBC ALL PASS\n");
    return fails;
}
