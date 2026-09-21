/* touch.c — create an empty file / update nothing (v0.3 FR-01 tool).
 *
 * Usage:  touch <file> [file ...]
 * O_CREAT|O_EXCL: an existing file is reported, not truncated.
 */
#include <stdio.h>
#include <fileio.h>

int main() {
    char args[128];
    int n = getargs(args, 128);
    if (n == 0) {
        printf("usage: touch <file> [file ...]\n");
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
        int fd = f_open(path, F_WRONLY | F_CREAT | F_EXCL);
        if (fd >= 0) {
            close(fd);
            printf("created '%s'\n", path);
        } else if (fd == -12) {
            printf("'%s' already exists\n", path);
        } else {
            printf("touch: '%s' failed (err %d)\n", path, fd);
            fails++;
        }
    }
    return fails ? 1 : 0;
}
