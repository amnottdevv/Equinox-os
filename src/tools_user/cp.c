/* cp.c — copy a file (v0.3 FR-01 userland tool).
 *
 * Usage:  cp <src> <dst>
 * The destination is created/truncated through open2(O_CREAT|
 * O_TRUNC|O_WRONLY) and written at the current offset; the FAT
 * write-through happens once at close(). Cross-FS copies work
 * (RAMFS -> FAT and back).
 */
#include <stdio.h>
#include <fileio.h>

int main() {
    char args[128];
    int n = getargs(args, 128);
    char src[80];
    char dst[80];
    int i = 0;
    int si = 0;
    int di = 0;

    while (i < n && args[i] == ' ') i++;
    while (i < n && args[i] != ' ' && si < 78) src[si++] = args[i++];
    while (i < n && args[i] == ' ') i++;
    while (i < n && args[i] != ' ' && di < 78) dst[di++] = args[i++];
    src[si] = 0;
    dst[di] = 0;

    if (src[0] == 0 || dst[0] == 0) {
        printf("usage: cp <src> <dst>\n");
        return 1;
    }

    int in = f_open(src, F_RDONLY);
    if (in < 0) {
        printf("cp: cannot open '%s' (err %d)\n", src, in);
        return 1;
    }
    int out = f_open(dst, F_WRONLY | F_CREAT | F_TRUNC);
    if (out < 0) {
        printf("cp: cannot create '%s' (err %d)\n", dst, out);
        close(in);
        return 1;
    }

    char buf[512];
    int n2;
    int total = 0;
    while ((n2 = read(in, buf, 512)) > 0) {
        int w = write(out, buf, n2);
        if (w != n2) {
            printf("cp: write error %d (wrote %d of %d)\n", w, w, n2);
            close(in);
            close(out);
            return 1;
        }
        total += n2;
    }
    if (n2 < 0) {
        printf("cp: read error %d\n", n2);
        close(in);
        close(out);
        return 1;
    }
    close(in);
    close(out);   /* flush: the FAT write-through happens here */
    printf("'%s' -> '%s'  %d bytes copied\n", src, dst, total);
    return 0;
}
