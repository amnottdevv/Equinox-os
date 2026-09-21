/* tail.c — print the LAST lines of a file (v0.3 userland tool).
 *
 * Usage:  tail [-n N] FILE        (default N = 10, max 64)
 * A ring of 64 line slots keeps the last N lines; N > 64 clamps
 * to 64 (documented limit — each slot is 256 bytes).
 */
#include <stdio.h>
#include <fileio.h>

/* ring buffer: 64 slots x 256 bytes = 16384 bytes — too big for a global
 * array (mtcc caps arrays at 4096), so it comes from malloc()
 * (the 2 MB MRP arena). Indexed rbuf[slot*256 + k]. */
char* rbuf;
int rlen[64];
int rstart;
int rcount;

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
    int want;
    int pn;
    int fd;
    int r;
    int slot;
    int k;

    n = getargs(args, 160);
    want = 10;
    path[0] = 0;
    pn = 0;
    rstart = 0;
    rcount = 0;
    rbuf = malloc(64 * 256);
    if (rbuf == 0) {
        printf("tail: out of memory\n");
        return 1;
    }

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (args[i] == '-' && args[i + 1] == 'n' && args[i + 2] == ' ') {
            i = i + 3;
            while (i < n && args[i] == ' ') i++;
            want = 0;
            while (i < n && args[i] >= '0' && args[i] <= '9') {
                want = want * 10 + (args[i] - '0');
                i++;
            }
        } else if (args[i] == '-' && args[i + 1] >= '0' && args[i + 1] <= '9') {
            i++;
            want = 0;
            while (i < n && args[i] >= '0' && args[i] <= '9') {
                want = want * 10 + (args[i] - '0');
                i++;
            }
        } else {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { path[pn] = args[i]; pn++; i++; }
            path[pn] = 0;
        }
    }

    if (want < 1) want = 1;
    if (want > 64) want = 64;
    if (path[0] == 0) {
        printf("usage: tail [-n N] FILE   (default 10, max 64 lines)\n");
        return 2;
    }
    fd = fopen(path);
    if (fd == 0) {
        printf("tail: '%s' (cannot open)\n", path);
        return 1;
    }

    while ((r = readline_fd(fd, line, 512)) >= 0) {
        if (rcount == 64) {
            slot = rstart;
            rstart = (rstart + 1) % 64;
        } else {
            slot = (rstart + rcount) % 64;
            rcount++;
        }
        /* copy into the slot (truncate at 255 chars) */
        k = 0;
        while (line[k] && k < 255) { rbuf[slot * 256 + k] = line[k]; k++; }
        rbuf[slot * 256 + k] = 0;
        rlen[slot] = k;
    }
    fclose(fd);

    /* print the last `want` of the collected lines */
    if (want > rcount) want = rcount;
    for (k = rcount - want; k < rcount; k++) {
        slot = (rstart + (k % 64)) % 64;
        printf("%s\n", rbuf + slot * 256);
    }
    return 0;
}
