/* rev.c — reverse each line of a file (v0.3 userland tool).
 *
 * Usage:  rev FILE
 * "hello world"  ->  "dlrow olleh". Text-line tool: '\n'/'\r'
 * are separators (not reversed), a missing final newline is kept
 * missing.
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
    int pn;
    int fd;
    int r;
    int a;
    int b;
    char tmp;

    n = getargs(args, 160);
    path[0] = 0;
    pn = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        pn = 0;
        while (i < n && args[i] != ' ' && pn < 62) { path[pn] = args[i]; pn++; i++; }
        path[pn] = 0;
    }

    if (path[0] == 0) {
        printf("usage: rev FILE\n");
        return 2;
    }
    fd = fopen(path);
    if (fd == 0) {
        printf("rev: '%s' (cannot open)\n", path);
        return 1;
    }

    while ((r = readline_fd(fd, line, 512)) >= 0) {
        a = 0;
        b = r - 1;
        while (a < b) {
            tmp = line[a];
            line[a] = line[b];
            line[b] = tmp;
            a++;
            b--;
        }
        printf("%s\n", line);
    }
    fclose(fd);
    return 0;
}
