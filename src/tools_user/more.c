/* more.c — paginated file viewer (v0.3 userland tool).
 *
 * Usage:  more FILE
 * Prints 23 lines per page, then "--More--" waits for a key:
 *   q     quit (stop printing)
 *   other any other key: next page. Arrow keys count as "other".
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

/* wait for a key (getkey is non-blocking; poll + sleep) */
int wait_key(void) {
    int c;
    c = getkey();
    while (c == -1) {
        sleep(40);
        c = getkey();
    }
    return c;
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
    int shown;
    int quit;

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
        printf("usage: more FILE\n");
        return 2;
    }
    fd = fopen(path);
    if (fd == 0) {
        printf("more: '%s' (cannot open)\n", path);
        return 1;
    }

    shown = 0;
    quit = 0;
    while ((r = readline_fd(fd, line, 512)) >= 0) {
        if (shown == 23) {
            int c;
            printf("--More-- (q = quit, any key = next page)\n");
            c = wait_key();
            printf("\n");
            if (c == 'q' || c == 'Q') { quit = 1; break; }
            shown = 0;
        }
        printf("%s\n", line);
        shown++;
    }
    fclose(fd);
    if (!quit) printf("--EOF--\n");
    return 0;
}
