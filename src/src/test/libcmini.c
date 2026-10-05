/* libcmini.c - v10.8 in-OS smoke test (mtcc + #include <morph.h>).
   Compact on purpose: the FULL 54-stage libc.c takes many minutes to
   compile under QEMU TCG; this mini exercises every subsystem once
   (prelude splice, string, user heap, stdio FILE I/O + seek, printf
   syscall, ringinfo) and stays compile-fast. Expected output:
     ring=3 strlen=6
     mini io=ring
     RINGMINI PASS
     SUM fails=0                                    */

#include <morph.h>

int main() {
    char b[64];
    char* s;
    int f;
    int n;
    int fails;

    fails = 0;

    strcpy(b, "morph");
    if (strlen(b) != 5) fails = fails + 1;
    if (strcmp(b, "morph") != 0) fails = fails + 1;
    strcat(b, "-libc");
    if (strcmp(b, "morph-libc") != 0) fails = fails + 1;

    s = malloc(32);
    memset(s, 65, 4);
    s[4] = 0;
    if (strcmp(s, "AAAA") != 0) fails = fails + 1;
    free(s);

    f = fopen("libcmini.dat", "w");
    fwrite("hello ring3 io", 1, 14, f);
    fclose(f);
    if (file_size("libcmini.dat") != 14) fails = fails + 1;

    f = fopen("libcmini.dat", "r");
    fseek(f, 6, 0);
    n = fread(b, 1, 4, f);
    b[4] = 0;
    fclose(f);
    if (n != 4) fails = fails + 1;
    if (strcmp(b, "ring") != 0) fails = fails + 1;

    printf("ring=%d strlen=%d\n", ring(), strlen("abcdef"));
    printf("mini io=%s\n", b);

    if (fails == 0) print("RINGMINI PASS\n");
    else print("RINGMINI FAIL\n");
    printf("SUM fails=%d\n", fails);
    return fails;
}
