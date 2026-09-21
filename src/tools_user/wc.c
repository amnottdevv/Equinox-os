/* wc.c — count lines, words and bytes of a file (v0.3 userland tool).
 *
 * Usage:  wc [-l] [-w] [-c] FILE
 *   -l lines ('\n' chars)   -w words   -c bytes (chars)
 * Default (no flags): all three counts + the file name.
 * Reads binary-safe 512-byte chunks.
 */
#include <stdio.h>
#include <fileio.h>

int main() {
    char args[160];
    char path[64];
    char buf[512];
    int n;
    int i;
    int pn;
    int fd;
    int want_l;
    int want_w;
    int want_c;
    int nlines;
    int nwords;
    int nbytes;
    int inword;
    int rd;
    int k;

    n = getargs(args, 160);
    want_l = 0; want_w = 0; want_c = 0;
    path[0] = 0;
    pn = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (args[i] == '-') {
            i++;
            while (i < n && args[i] != ' ') {
                if (args[i] == 'l') want_l = 1;
                else if (args[i] == 'w') want_w = 1;
                else if (args[i] == 'c') want_c = 1;
                i++;
            }
        } else {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { path[pn] = args[i]; pn++; i++; }
            path[pn] = 0;
        }
    }

    if (path[0] == 0) {
        printf("usage: wc [-l] [-w] [-c] FILE\n");
        return 2;
    }
    if (!want_l && !want_w && !want_c) { want_l = 1; want_w = 1; want_c = 1; }

    fd = f_open(path, F_RDONLY);
    if (fd < 0) {
        printf("wc: '%s' (err %d)\n", path, fd);
        return 1;
    }

    nlines = 0; nwords = 0; nbytes = 0; inword = 0;
    while ((rd = read(fd, buf, 512)) > 0) {
        for (k = 0; k < rd; k++) {
            int c;
            c = buf[k] & 255;
            nbytes++;
            if (c == '\n') nlines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                inword = 0;
            } else if (inword == 0) {
                inword = 1;
                nwords++;
            }
        }
    }
    close(fd);

    if (want_l) printf("%7d", nlines);
    if (want_w) printf(" %7d", nwords);
    if (want_c) printf(" %7d", nbytes);
    printf(" %s\n", path);
    return 0;
}
