/* libtest.c — v0.3 FR-19 libc regression (mtcc subset).
   Exercises every FR-19 addition; prints LIBTEST-<n> markers the
   harness greps for. */
#include <morph.h>

int main() {
    char b[128];

    /* 1. printf with 5 conversion args (was max 3) */
    printf("LIBTEST-1 %d %d %d %d %d\n", 1, 2, 3, 4, 5);

    /* 2. true unsigned %u (binary long division) */
    printf("LIBTEST-2 %u\n", -1);
    printf("LIBTEST-2b %u\n", 4000000000);

    /* 3. %p and %o */
    printf("LIBTEST-3 0x%08x %o\n", 48879, 8);

    /* 4. snprintf with width/pad/left-align */
    snprintf(b, 128, "[%5d][%-5d][%05d]", 42, 42, 42);
    printf("LIBTEST-4 %s\n", b);

    /* 5. sprintf with more than 3 args */
    sprintf(b, "%s+%d+%s", "a", 7, "z");
    printf("LIBTEST-5 %s\n", b);

    /* 6. sscanf %d %x %s */
    int d = 0; int x = 0; char w[32];
    int n = sscanf("  -17 0x2a word", "%d %x %s", &d, &x, w);
    printf("LIBTEST-6 %d %d %s %d\n", d, x, w, n);

    /* 7. ctype */
    printf("LIBTEST-7 %d%d%d%d%d\n",
           isdigit('7'), isalpha('q'), isspace('\t'),
           toupper('a'), tolower('Z'));

    /* 8. strdup + strcasecmp */
    char* s = strdup("Equinox");
    printf("LIBTEST-8 %d %d\n", strcasecmp(s, "EQUINOX"),
           strcasecmp(s, "equinoz"));

    /* 9. strtok */
    char line[32];
    strcpy(line, "alpha,beta,,gamma");
    char* t1 = strtok(line, ",");
    char* t2 = strtok(0, ",");
    char* t3 = strtok(0, ",");
    char* t4 = strtok(0, ",");
    printf("LIBTEST-9 %s %s %s %s\n", t1, t2, t3, t4);

    /* 10. strspn/strcspn */
    printf("LIBTEST-10 %d %d\n",
           strspn("12345abc", "0123456789"),
           strcspn("abc123", "0123456789"));

    /* 11. abs/rand determinism */
    srand(42);
    int r1 = rand();
    srand(42);
    int r2 = rand();
    printf("LIBTEST-11 %d %d %d\n", abs(-19), r1 == r2, r1 >= 0);

    /* 12. strerror + puts */
    printf("LIBTEST-12 %s\n", strerror(-3));
    puts("LIBTEST-12b puts works");

    /* 13. fgetc/fputc round-trip through fopen w/r */
    int f = fopen("/tmp_libtest.txt", "w");
    fputc('O', f);
    fputc('K', f);
    fclose(f);
    int g = fopen("/tmp_libtest.txt", "r");
    char c1 = fgetc(g);
    char c2 = fgetc(g);
    fclose(g);
    remove("/tmp_libtest.txt");
    printf("LIBTEST-13 %c%c\n", c1, c2);

    /* 14. memchr + strrchr (regression: older libc) */
    char hay[16];
    strcpy(hay, "ab.c.def");
    printf("LIBTEST-14 %d %d\n", memchr(hay, '.', 8) - hay,
           strrchr(hay, '.') - hay);
    return 0;
}
