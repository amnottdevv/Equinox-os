/* sort.c — sort the lines of a text file (v0.3 userland tool).
 *
 * Usage:  sort [-r] FILE
 *   -r   reverse (descending) order
 * Lines are stored in a 32 KB buffer (max 600 lines, 255 chars
 * each — longer lines are truncated, more lines are ignored with
 * a notice). Bubble sort with early exit; strcmp byte order.
 */
#include <stdio.h>
#include <fileio.h>

/* 32 KB line store — bigger than mtcc's 4096-element array cap,
 * so it comes from malloc() (2 MB MRP arena). */
char* sbuf;
int sbn;
int soff[600];
int scount;

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
    char path[64];
    char line[512];
    int n;
    int i;
    int pn;
    int fd;
    int r;
    int reverse;
    int k;
    int dropped;
    int swapped;
    int pass;
    int j;
    int a;
    int b;
    int tmp;

    n = getargs(args, 160);
    reverse = 0;
    path[0] = 0;
    pn = 0;
    sbn = 0;
    scount = 0;
    sbuf = malloc(32768);
    if (sbuf == 0) {
        printf("sort: out of memory\n");
        return 1;
    }

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (args[i] == '-') {
            i++;
            while (i < n && args[i] != ' ') {
                if (args[i] == 'r') reverse = 1;
                i++;
            }
        } else {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { path[pn] = args[i]; pn++; i++; }
            path[pn] = 0;
        }
    }

    if (path[0] == 0) {
        printf("usage: sort [-r] FILE\n");
        return 2;
    }
    fd = fopen(path);
    if (fd == 0) {
        printf("sort: '%s' (cannot open)\n", path);
        return 1;
    }

    dropped = 0;
    while ((r = readline_fd(fd, line, 512)) >= 0) {
        if (scount >= 600 || sbn + 256 > 32768) { dropped++; continue; }
        k = 0;
        while (line[k] && k < 255) { sbuf[sbn + k] = line[k]; k++; }
        sbuf[sbn + k] = 0;
        soff[scount] = sbn;
        scount++;
        sbn = sbn + k + 1;
    }
    fclose(fd);
    if (dropped) printf("sort: %d line(s) beyond the 600/32KB limit skipped\n", dropped);

    /* bubble sort with early exit */
    for (pass = 0; pass < scount - 1; pass++) {
        swapped = 0;
        for (j = 0; j < scount - 1 - pass; j++) {
            a = soff[j];
            b = soff[j + 1];
            if (strcmp(sbuf + a, sbuf + b) > 0) {
                tmp = soff[j];
                soff[j] = soff[j + 1];
                soff[j + 1] = tmp;
                swapped = 1;
            }
        }
        if (swapped == 0) break;
    }

    for (k = 0; k < scount; k++) {
        int idx;
        idx = reverse ? (scount - 1 - k) : k;
        printf("%s\n", sbuf + soff[idx]);
    }
    return 0;
}
