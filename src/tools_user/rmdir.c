/* rmdir.c — remove empty director(y|ies) (v0.3 FR-01 userland tool).
 *
 * Usage:  rmdir <dir> [dir ...]
 */
#include <stdio.h>
#include <fileio.h>

int main() {
    char args[128];
    int n = getargs(args, 128);
    if (n == 0) {
        printf("usage: rmdir <dir> [dir ...]\n");
        return 1;
    }
    char path[80];
    int i = 0;
    int fails = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        int pi = 0;
        while (i < n && args[i] != ' ' && pi < 78) path[pi++] = args[i++];
        path[pi] = 0;
        if (pi == 0) continue;
        int r = f_rmdir(path);
        if (r == 0) printf("removed dir '%s'\n", path);
        else        printf("rmdir: '%s' failed (err %d)\n", path, r);
        if (r != 0) fails++;
    }
    return fails ? 1 : 0;
}
