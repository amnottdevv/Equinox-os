/* uniq.c — collapse adjacent duplicate lines (v0.3 userland tool).
 *
 * Usage:  uniq [-c] FILE
 *   -c   prefix each group with the repeat count
 * Only ADJACENT equal lines collapse (pipe through sort first for
 * a full dedup — classic uniq semantics).
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
    char path[64];
    char line[256];
    char prev[256];
    int n;
    int i;
    int pn;
    int fd;
    int r;
    int showcount;
    int has_prev;
    int grp;

    n = getargs(args, 160);
    showcount = 0;
    path[0] = 0;
    pn = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (args[i] == '-') {
            i++;
            while (i < n && args[i] != ' ') {
                if (args[i] == 'c') showcount = 1;
                i++;
            }
        } else {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { path[pn] = args[i]; pn++; i++; }
            path[pn] = 0;
        }
    }

    if (path[0] == 0) {
        printf("usage: uniq [-c] FILE\n");
        return 2;
    }
    fd = fopen(path);
    if (fd == 0) {
        printf("uniq: '%s' (cannot open)\n", path);
        return 1;
    }

    has_prev = 0;
    grp = 0;
    while ((r = readline_fd(fd, line, 256)) >= 0) {
        if (has_prev && strcmp(prev, line) == 0) {
            grp++;
        } else {
            if (has_prev) {
                if (showcount) printf("%7d %s\n", grp, prev);
                else printf("%s\n", prev);
            }
            i = 0;
            while (line[i] && i < 255) { prev[i] = line[i]; i++; }
            prev[i] = 0;
            grp = 1;
            has_prev = 1;
        }
    }
    fclose(fd);
    if (has_prev) {
        if (showcount) printf("%7d %s\n", grp, prev);
        else printf("%s\n", prev);
    }
    return 0;
}
