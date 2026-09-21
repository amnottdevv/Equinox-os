/* dirname.c — print the directory part of a path (v0.3 userland tool).
 *
 * Usage:  dirname PATH
 *   dirname /a/b/c.txt  ->  /a/b
 *   dirname c.txt       ->  .
 *   dirname /a/b/       ->  /a
 *   dirname /a          ->  /
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
        printf("usage: dirname PATH\n");
        return 2;
    }

    len = 0;
    while (path[len]) len++;

    /* drop trailing '/' (but keep a lone "/") */
    while (len > 1 && path[len - 1] == '/') { path[len - 1] = 0; len--; }

    /* find the last remaining '/' */
    last = -1;
    for (i = 0; i < len; i++) {
        if (path[i] == '/') last = i;
    }
    if (last < 0) {
        printf(".\n");                    /* no slash: current dir */
    } else if (last == 0) {
        printf("/\n");                    /* slash at the root */
    } else {
        path[last] = 0;
        printf("%s\n", path);
    }
    return 0;
}
