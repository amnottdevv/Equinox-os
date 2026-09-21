/* strings.c — print printable runs from a file (v0.3 userland tool).
 *
 * Usage:  strings FILE [minlen]
 * Prints every run of printable chars (32..126) of length >=
 * minlen (default 4), one per line. Binary-safe chunked reads.
 */
#include <stdio.h>
#include <fileio.h>

char runbuf[256];

int main() {
    char args[160];
    char path[64];
    char buf[512];
    int n;
    int i;
    int pn;
    int fd;
    int rd;
    int minlen;
    int k;
    int rl;

    n = getargs(args, 160);
    path[0] = 0;
    pn = 0;
    minlen = 4;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (path[0] == 0) {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { path[pn] = args[i]; pn++; i++; }
            path[pn] = 0;
        } else {
            minlen = 0;
            while (i < n && args[i] >= '0' && args[i] <= '9') {
                minlen = minlen * 10 + (args[i] - '0');
                i++;
            }
            if (minlen < 1) minlen = 4;
            if (minlen > 255) minlen = 255;
        }
    }

    if (path[0] == 0) {
        printf("usage: strings FILE [minlen]\n");
        return 2;
    }
    fd = f_open(path, F_RDONLY);
    if (fd < 0) {
        printf("strings: '%s' (err %d)\n", path, fd);
        return 1;
    }

    rl = 0;
    while ((rd = read(fd, buf, 512)) > 0) {
        for (k = 0; k < rd; k++) {
            int c;
            c = buf[k] & 255;
            if (c >= 32 && c <= 126) {
                if (rl < 255) { runbuf[rl] = c; rl++; }
            } else {
                if (rl >= minlen) {
                    runbuf[rl] = 0;
                    printf("%s\n", runbuf);
                }
                rl = 0;
            }
        }
    }
    close(fd);
    if (rl >= minlen) {
        runbuf[rl] = 0;
        printf("%s\n", runbuf);
    }
    return 0;
}
