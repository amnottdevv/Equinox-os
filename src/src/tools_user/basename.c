/* basename.c — strip directories from a path (v0.3 userland tool).
 *
 * Usage:  basename PATH
 *   basename /a/b/c.txt  ->  c.txt
 *   basename c.txt       ->  c.txt
 *   basename /a/b/       ->  b        (trailing '/' ignored)
 *   basename /           ->  /
 */
#include <stdio.h>

int main() {
    char args[160];
    char path[96];
    int n;
    int i;
    int pn;
    int len;
    int last;

    n = getargs(args, 160);
    path[0] = 0;
    pn = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        pn = 0;
        while (i < n && args[i] != ' ' && pn < 94) { path[pn] = args[i]; pn++; i++; }
        path[pn] = 0;
    }

    if (path[0] == 0) {
        printf("usage: basename PATH\n");
        return 2;
    }

    len = 0;
    while (path[len]) len++;

    /* drop trailing '/' (but keep a lone "/") */
    while (len > 1 && path[len - 1] == '/') { path[len - 1] = 0; len--; }

    if (path[0] == 0) {
        printf("/\n");                    /* input was all slashes */
        return 0;
    }
    if (len == 1 && path[0] == '/') {
        printf("/\n");
        return 0;
    }

    /* find the last '/' */
    last = -1;
    for (i = 0; i < len; i++) {
        if (path[i] == '/') last = i;
    }
    printf("%s\n", path + (last + 1));
    return 0;
}
