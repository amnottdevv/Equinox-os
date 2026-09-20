/* cksum.c — byte checksum + size of a file (v0.3 userland tool).
 *
 * Usage:  cksum FILE [FILE ...]
 * Prints "<sum> <size> <name>" per file — sum is the 32-bit
 * wrap-around sum of all bytes (rendered unsigned via %u).
 * Binary-safe chunked reads. Multiple files each get a line.
 */
#include <stdio.h>
#include <fileio.h>

char fbuf[512];
int foff[9];

int ck_one(char* path) {
    char buf[512];
    int fd;
    int rd;
    int k;
    int sum;
    int size;

    fd = f_open(path, F_RDONLY);
    if (fd < 0) {
        printf("cksum: '%s' (err %d)\n", path, fd);
        return 1;
    }
    sum = 0;
    size = 0;
    while ((rd = read(fd, buf, 512)) > 0) {
        for (k = 0; k < rd; k++) {
            sum = sum + (buf[k] & 255);      /* wraps mod 2^32 */
            size++;
        }
    }
    close(fd);
    printf("%u %d %s\n", sum, size, path);
    return 0;
}

int main() {
    char args[160];
    int n;
    int i;
    int nfiles;
    int fails;
    int k;

    n = getargs(args, 160);
    nfiles = 0;
    foff[0] = 0;
    fails = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (nfiles < 8) {
            char tok[64];
            int ti;
            int base;
            ti = 0;
            while (i < n && args[i] != ' ' && ti < 62) { tok[ti] = args[i]; ti++; i++; }
            tok[ti] = 0;
            base = foff[nfiles];
            k = 0;
            while (tok[k]) { fbuf[base + k] = tok[k]; k++; }
            fbuf[base + k] = 0;
            nfiles++;
            foff[nfiles] = base + k + 1;
        } else {
            while (i < n && args[i] != ' ') i++;
        }
    }

    if (nfiles == 0) {
        printf("usage: cksum FILE [FILE ...]\n");
        return 2;
    }
    for (k = 0; k < nfiles; k++) {
        if (ck_one(fbuf + foff[k]) != 0) fails++;
    }
    return fails ? 1 : 0;
}
