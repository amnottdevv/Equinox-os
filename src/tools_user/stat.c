/* stat.c — show file metadata (v0.3 FR-01 userland tool).
 *
 * Usage:  stat <path>
 * Prints the morph_stat_t fields: size, type, filesystem.
 */
#include <stdio.h>
#include <fileio.h>

int main() {
    char args[128];
    int n = getargs(args, 128);
    char path[80];
    int i = 0;
    int pi = 0;
    while (i < n && args[i] == ' ') i++;
    while (i < n && args[i] != ' ' && pi < 78) path[pi++] = args[i++];
    path[pi] = 0;

    if (path[0] == 0) {
        printf("usage: stat <path>\n");
        return 1;
    }
    int st[4];
    int r = f_stat(path, st);
    if (r != 0) {
        printf("stat: '%s' (err %d)\n", path, r);
        return 1;
    }
    printf("  path   : %s\n", path);
    printf("  type   : %s\n", st[1] ? "directory" : "file");
    printf("  size   : %d bytes\n", st[0]);
    printf("  fs     : %s\n", st[2] ? "FAT32 (/mnt)" : "RAMFS");
    return 0;
}
