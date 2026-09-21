/* tr.c — translate or delete characters (v0.3 userland tool).
 *
 * Usage:  tr SET1 SET2 FILE        translate
 *         tr -d SET FILE           delete chars in SET
 * SETs support ranges (a-z, A-Z, 0-9, a-e etc.) and escapes:
 * \n \t \r \0 \\ (plus any literal char).
 * If SET2 is shorter than SET1, its LAST char repeats (classic
 * tr behavior). Binary-safe chunk I/O.
 */
#include <stdio.h>
#include <fileio.h>

/* parse a SET argument into raw bytes; returns the length.
 * Ranges: X-Y expands to X..Y when both are printable and Y > X
 * (span <= 63); a leading/dangling '-' is a literal dash. */
int parse_set(char* s, char* out) {
    int i;
    int o;
    i = 0;
    o = 0;
    while (s[i]) {
        if (s[i] == '\\' && s[i + 1]) {
            i++;
            if (s[i] == 'n') out[o] = '\n';
            else if (s[i] == 't') out[o] = '\t';
            else if (s[i] == 'r') out[o] = '\r';
            else if (s[i] == '0') out[o] = 0;
            else if (s[i] == '\\') out[o] = '\\';
            else out[o] = s[i];
            i++;
            o++;
        } else if (s[i + 1] == '-' && s[i + 2] &&
                   s[i + 2] > s[i] && s[i + 2] - s[i] <= 63) {
            char c;
            c = s[i];
            while (c <= s[i + 2] && o < 63) {
                out[o] = c;
                o++;
                c = c + 1;
            }
            i = i + 3;
        } else {
            out[o] = s[i];
            i++;
            o++;
        }
        if (o >= 63) break;
    }
    return o;
}

int main() {
    char args[160];
    char path[64];
    char tok1[64];
    char tok2[64];
    char set1[64];
    char set2[64];
    char buf[512];
    char out[512];
    int n;
    int i;
    int pn;
    int fd;
    int rd;
    int delmode;
    int len1;
    int len2;
    int k;
    int j;

    n = getargs(args, 160);
    delmode = 0;
    tok1[0] = 0; tok2[0] = 0; path[0] = 0;
    pn = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (args[i] == '-' && args[i + 1] == 'd' && (args[i + 2] == ' ' || args[i + 2] == 0)) {
            delmode = 1;
            i = i + 2;
        } else {
            char tok[64];
            int ti;
            ti = 0;
            while (i < n && args[i] != ' ' && ti < 63) { tok[ti] = args[i]; ti++; i++; }
            tok[ti] = 0;
            if (tok1[0] == 0) {
                k = 0;
                while (tok[k]) { tok1[k] = tok[k]; k++; }
                tok1[k] = 0;
            } else if (tok2[0] == 0) {
                k = 0;
                while (tok[k]) { tok2[k] = tok[k]; k++; }
                tok2[k] = 0;
            } else if (path[0] == 0) {
                k = 0;
                while (tok[k] && k < 62) { path[k] = tok[k]; k++; }
                path[k] = 0;
            }
        }
    }

    len1 = 0;
    len2 = 0;
    if (delmode) {
        len1 = parse_set(tok1, set1);
        if (len1 == 0 || path[0] == 0) {
            printf("usage: tr -d SET FILE\n");
            return 2;
        }
    } else {
        len1 = parse_set(tok1, set1);
        len2 = parse_set(tok2, set2);
        if (len1 == 0 || len2 == 0 || path[0] == 0) {
            printf("usage: tr SET1 SET2 FILE\n");
            printf("  SETs support \\n \\t \\r \\0 \\\\ escapes\n");
            return 2;
        }
    }

    fd = f_open(path, F_RDONLY);
    if (fd < 0) {
        printf("tr: '%s' (err %d)\n", path, fd);
        return 1;
    }

    while ((rd = read(fd, buf, 512)) > 0) {
        int on;
        on = 0;
        for (k = 0; k < rd; k++) {
            int c;
            int pos;
            c = buf[k] & 255;
            pos = -1;
            for (j = 0; j < len1; j++) {
                if ((set1[j] & 255) == c) { pos = j; break; }
            }
            if (delmode) {
                if (pos >= 0) continue;         /* drop it */
                out[on] = c; on++;
            } else if (pos >= 0) {
                int m;
                m = pos < len2 ? pos : len2 - 1; /* last char repeats */
                out[on] = set2[m] & 255; on++;
            } else {
                out[on] = c; on++;
            }
            if (on >= 510) { write(1, out, on); on = 0; }
        }
        if (on > 0) write(1, out, on);
    }
    close(fd);
    return 0;
}
