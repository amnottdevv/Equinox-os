/* diff.c — line-by-line file comparison (v0.3 userland tool).
 *
 * Usage:  diff FILE1 FILE2
 * A streaming line comparator (no LCS/hunk merging — the first
 * N differing lines are shown '<' / '>' style, plus a summary).
 * Exit 0 = identical, 1 = differ, 2 = usage/open error.
 */
#include <stdio.h>
#include <fileio.h>

int readline_fd(int fd, char* line, int maxlen) {
    int n;
    int c;
    n = 0;
    c = -1;
    while (n < maxlen - 1) {
        c = fgetc(fd);
        if (c == -1) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        line[n] = c;
        n = n + 1;
    }
    line[n] = 0;
    if (n == 0 && c == -1) return -1;
    return n;
}

int main() {
    char args[160];
    char p1[64];
    char p2[64];
    char l1[512];
    char l2[512];
    int n;
    int i;
    int pn;
    int f1;
    int f2;
    int r1;
    int r2;
    int ln;
    int diffs;
    int shown;
    int extra;

    n = getargs(args, 160);
    p1[0] = 0; p2[0] = 0;
    pn = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (p1[0] == 0) {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { p1[pn] = args[i]; pn++; i++; }
            p1[pn] = 0;
        } else {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { p2[pn] = args[i]; pn++; i++; }
            p2[pn] = 0;
        }
    }

    if (p2[0] == 0) {
        printf("usage: diff FILE1 FILE2\n");
        return 2;
    }
    f1 = fopen(p1);
    if (f1 == 0) {
        printf("diff: '%s' (cannot open)\n", p1);
        return 2;
    }
    f2 = fopen(p2);
    if (f2 == 0) {
        printf("diff: '%s' (cannot open)\n", p2);
        fclose(f1);
        return 2;
    }

    ln = 0;
    diffs = 0;
    shown = 0;
    while (1) {
        r1 = readline_fd(f1, l1, 512);
        r2 = readline_fd(f2, l2, 512);
        if (r1 < 0 && r2 < 0) break;              /* both at EOF */
        ln++;
        if (r1 < 0 || r2 < 0 || strcmp(l1, l2) != 0) {
            diffs++;
            if (shown < 20) {
                if (r1 < 0) printf("%d: < (no line)\n", ln);
                else printf("%d: < %s\n", ln, l1);
                if (r2 < 0) printf("%d: > (no line)\n", ln);
                else printf("%d: > %s\n", ln, l2);
                shown++;
            }
        }
        if (r1 < 0 || r2 < 0) {
            /* one file ended: count the remaining lines of the other */
            extra = 0;
            if (r1 < 0) {
                while (readline_fd(f2, l2, 512) >= 0) extra++;
            } else {
                while (readline_fd(f1, l1, 512) >= 0) extra++;
            }
            diffs = diffs + extra;
            printf("(one file ended — %d extra line(s) not shown)\n", extra);
            break;
        }
    }
    fclose(f1);
    fclose(f2);

    if (diffs == 0) {
        printf("Files are identical\n");
        return 0;
    }
    printf("%d differing line(s) (first %d shown)\n", diffs, shown);
    return 1;
}
