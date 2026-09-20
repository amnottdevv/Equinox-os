/* cat.c — print file(s) to stdout (v0.3 FR-01 userland tool).
 *
 * Usage:  cat <file> [file ...]
 * Sequential open/read/close through fd syscalls; binary-safe
 * (writes exactly the bytes read, no trailing newline added).
 */
#include <stdio.h>
#include <fileio.h>

int cat_one(char* path) {
    int fd = f_open(path, F_RDONLY);
    if (fd < 0) {
        printf("cat: '%s' (err %d)\n", path, fd);
        return 1;
    }
    char buf[512];
    int n;
    int total = 0;
    while ((n = read(fd, buf, 512)) > 0) {
        write(1, buf, n);
        total += n;
    }
    close(fd);
    if (n < 0) {
        printf("cat: read error %d on '%s'\n", n, path);
        return 1;
    }
    return 0;
}

int main() {
    char args[128];
    int n = getargs(args, 128);
    if (n == 0) {
        printf("usage: cat <file> [file ...]\n");
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
        if (pi > 0) {
            if (cat_one(path) != 0) fails++;
        }
    }
    return fails ? 1 : 0;
}
