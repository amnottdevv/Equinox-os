/* mkdir.c — create director(y|ies) (v0.3 FR-01 userland tool).
 *
 * Usage:  mkdir <dir> [dir ...]
 */
#include <stdio.h>
#include <fileio.h>

int main() {
    char args[128];
    int n = getargs(args, 128);
    if (n == 0) {
        printf("usage: mkdir <dir> [dir ...]\n");
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
        int r = f_mkdir(path);
        if (r == 0)        printf("created dir '%s'\n", path);
        else if (r == -12) printf("mkdir: '%s' already exists\n", path);
        else               printf("mkdir: '%s' failed (err %d)\n", path, r);
        if (r != 0) fails++;
    }
    return fails ? 1 : 0;
}
