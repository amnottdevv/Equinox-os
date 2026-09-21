/* ls.c — list directory contents (v0.3 FR-01 userland tool).
 *
 * Usage:  ls [-l] [path]
 * The path may be relative (resolved against the shell cwd) or
 * absolute. Without a path the current directory is listed.
 * The listing comes from the SYS_READDIR syscall (#46) through an
 * O_DIR fd — this program holds NO ring-0 privileges.
 */
#include <stdio.h>
#include <fileio.h>

/* dirent layout (see <fileio.h>): char name[64], is_dir at +64,
 * size at +68. mtcc has no struct/cast, so the two ints are read
 * byte-wise (little-endian). */
int main() {
    char args[128];
    char path[80];
    int n = getargs(args, 128);
    int detail = 0;
    int i = 0;
    int pi = 0;

    path[0] = '.';
    path[1] = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (args[i] == '-') {
            i++;
            while (i < n && args[i] != ' ') {
                if (args[i] == 'l') detail = 1;
                i++;
            }
        } else {
            pi = 0;
            while (i < n && args[i] != ' ' && pi < 78) {
                path[pi++] = args[i];
                i++;
            }
            path[pi] = 0;
        }
    }

    int fd = f_open(path, F_DIR);
    if (fd < 0) {
        printf("ls: cannot open '%s' (err %d)\n", path, fd);
        return 1;
    }

    char de[72];
    int r;
    int count = 0;
    int dirs = 0;
    while ((r = f_readdir(fd, de)) == 1) {
        int is_dir = (de[64] & 255) | ((de[65] & 255) << 8);
        int sz = (de[68] & 255) | ((de[69] & 255) << 8)
               | ((de[70] & 255) << 16) | ((de[71] & 255) << 24);
        if (detail) {
            printf("%7d  %s%s\n", sz, de, is_dir ? "  <DIR>" : "");
        } else {
            printf("%s%s\n", de, is_dir ? "/" : "");
        }
        count++;
        if (is_dir) dirs++;
    }
    close(fd);
    if (r < 0) {
        printf("ls: readdir error %d\n", r);
        return 1;
    }
    printf("%d entries (%d dirs)\n", count, dirs);
    return 0;
}
