/* cut.c — extract selected FIELDS from each line (v0.3 userland tool).
 *
 * Usage:  cut [-d DELIM] -f LIST FILE
 *   -d DELIM   single-char field delimiter (default: tab)
 *   -f LIST    comma-separated fields: N, N-M, N- (open ended)
 * Example:  cut -d : -f 1,3-4 /etc-style-list
 * A line with no delimiter is ONE field (the whole line), so higher
 * fields print empty (there is no -s flag in this port).
 */
#include <stdio.h>
#include <fileio.h>

int readline_fd(int fd, char* line, int maxlen) {
    int n;
    int c;
    n = 0;
    c = -1;
    while (n < maxlen - 1) {
        c = fgetc(fd);
        if (c == -1) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        line[n] = c;
        n = n + 1;
    }
    line[n] = 0;
    if (n == 0 && c == -1) return -1;
    return n;
}

int main() {
    char args[160];
    char path[64];
    char list[64];
    char line[512];
    int lo[16];
    int hi[16];
    int nranges;
    int n;
    int i;
    int pn;
    int fd;
    int r;
    int delim;
    int k;
    int j;
    int has_f;
    int num;
    int state;   /* 0 = none, 1 = have a number, 2 = after '-' */
    int a;
    int b;

    n = getargs(args, 160);
    delim = '\t';
    list[0] = 0;
    path[0] = 0;
    pn = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (args[i] == '-' && args[i + 1] == 'd' && args[i + 2] == ' ') {
            i = i + 3;
            while (i < n && args[i] == ' ') i++;
            if (args[i] == '\\') {
                i++;
                if (args[i] == 't') delim = '\t';
                else if (args[i] == 'n') delim = '\n';
                else if (args[i] == '0') delim = 0;
                else delim = args[i];
                i++;
            } else {
                delim = args[i];
                i++;
            }
        } else if (args[i] == '-' && args[i + 1] == 'f' && args[i + 2] == ' ') {
            i = i + 3;
            while (i < n && args[i] == ' ') i++;
            k = 0;
            while (i < n && args[i] != ' ' && k < 63) { list[k] = args[i]; k++; i++; }
            list[k] = 0;
        } else {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { path[pn] = args[i]; pn++; i++; }
            path[pn] = 0;
        }
    }

    if (list[0] == 0 || path[0] == 0) {
        printf("usage: cut [-d DELIM] -f LIST FILE\n");
        printf("  LIST: N, N-M, N-  (comma separated)\n");
        return 2;
    }

    /* parse LIST -> lo[]/hi[] ranges */
    nranges = 0;
    a = 0; b = 0; num = 0; state = 0;
    k = 0;
    while (list[k]) {
        char ch;
        ch = list[k];
        if (ch >= '0' && ch <= '9') {
            num = num * 10 + (ch - '0');
            if (state == 0) state = 1;
            else if (state == 2) state = 3;
        } else if (ch == '-') {
            if (state == 1) { a = num; state = 2; }
            num = 0;
        } else if (ch == ',') {
            if (state == 1) { a = num; b = num; }
            else if (state == 2) { b = 999; }
            else if (state == 3) { b = num; }
            if (state != 0 && nranges < 16) {
                lo[nranges] = a;
                hi[nranges] = b;
                nranges++;
            }
            a = 0; b = 0; num = 0; state = 0;
        }
        k++;
    }
    if (state == 1) { a = num; b = num; }
    else if (state == 2) { b = 999; }
    else if (state == 3) { b = num; }
    if (state != 0 && nranges < 16) {
        lo[nranges] = a;
        hi[nranges] = b;
        nranges++;
    }
    if (nranges == 0) {
        printf("cut: bad field list '%s'\n", list);
        return 2;
    }

    fd = fopen(path);
    if (fd == 0) {
        printf("cut: '%s' (cannot open)\n", path);
        return 1;
    }

    while ((r = readline_fd(fd, line, 512)) >= 0) {
        int fs[64];
        int fe[64];
        int nf;
        int start;
        int p;
        int any;

        /* split by delim: fields as [start,end) in line */
        start = 0;
        nf = 0;
        p = 0;
        while (p <= 512) {
            int at_end;
            int hit;
            at_end = (line[p] == 0);
            hit = (line[p] == delim && delim != 0);
            if (at_end || hit) {
                if (nf < 64) {
                    fs[nf] = start;
                    fe[nf] = p;
                    nf++;
                }
                if (at_end) break;
                p++;
                start = p;
            } else {
                p++;
            }
        }

        any = 0;
        for (j = 0; j < nranges; j++) {
            int f;
            for (f = lo[j]; f <= hi[j]; f++) {
                if (f >= 1 && f <= nf) {
                    char saved;
                    if (any) {
                        char d[2];
                        d[0] = delim;
                        d[1] = 0;
                        printf("%s", d);
                    }
                    saved = line[fe[f - 1]];
                    line[fe[f - 1]] = 0;
                    printf("%s", line + fs[f - 1]);
                    line[fe[f - 1]] = saved;
                    any = 1;
                }
                if (f >= 999) break;
            }
        }
        printf("\n");
    }
    fclose(fd);
    return 0;
}
