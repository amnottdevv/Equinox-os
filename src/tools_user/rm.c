/* rm.c — remove file(s) (v0.3 FR-01 userland tool).
 *
 * Usage:  rm <file> [file ...]
 * Directories are refused (EISDIR) — use rmdir.
 */
#include <stdio.h>
#include <fileio.h>

int main() {
    char args[128];
    int n = getargs(args, 128);
    if (n == 0) {
        printf("usage: rm <file> [file ...]\n");
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
        int r = f_unlink(path);
        if (r == 0) {
            printf("removed '%s'\n", path);
        } else if (r == -4) {
            printf("rm: '%s' is a directory (use rmdir)\n", path);
            fails++;
        } else {
            printf("rm: '%s' failed (err %d)\n", path, r);
            fails++;
        }
    }
    return fails ? 1 : 0;
}
