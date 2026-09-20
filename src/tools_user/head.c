/* head.c — print the FIRST lines of a file (v0.3 userland tool).
 *
 * Usage:  head [-n N] FILE        (default N = 10)
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
    char line[512];
    int n;
    int i;
    int want;
    int pn;
    int fd;
    int r;
    int printed;

    n = getargs(args, 160);
    want = 10;
    path[0] = 0;
    pn = 0;

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
            if (want < 1) want = 1;
            if (want > 100000) want = 100000;
        } else if (args[i] == '-' && args[i + 1] >= '0' && args[i + 1] <= '9') {
            i++;
            want = 0;
            while (i < n && args[i] >= '0' && args[i] <= '9') {
                want = want * 10 + (args[i] - '0');
                i++;
            }
            if (want < 1) want = 1;
        } else {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { path[pn] = args[i]; pn++; i++; }
            path[pn] = 0;
        }
    }

    if (path[0] == 0) {
        printf("usage: head [-n N] FILE   (default 10 lines)\n");
        return 2;
    }
    fd = fopen(path);
    if (fd == 0) {
        printf("head: '%s' (cannot open)\n", path);
        return 1;
    }
    printed = 0;
    while (printed < want && (r = readline_fd(fd, line, 512)) >= 0) {
        printf("%s\n", line);
        printed++;
    }
    fclose(fd);
    return 0;
}
