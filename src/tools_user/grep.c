/* grep.c — print lines matching a PATTERN (v0.3 userland tool).
 *
 * Usage:  grep [-i] [-n] [-c] [-v] PATTERN FILE [FILE ...]
 *   -i   ignore case
 *   -n   prefix each line with its line number
 *   -c   print only the COUNT of matching lines
 *   -v   invert: print lines that do NOT match
 *
 * PATTERN is a mini regular expression: literal chars plus
 *   .      any single char
 *   X*     zero or more of X
 *   ^PAT   PAT must start at the beginning of the line
 *   PAT$   PAT must end at the end of the line
 * A pattern with no metachars is a plain substring search
 * (like grep -F). Multiple FILEs get a "name:" prefix.
 */
#include <stdio.h>
#include <fileio.h>

/* ---- recursive mini-regex: full-match r against a PREFIX of t.
 * Returns 1 + sets *ep = number of chars consumed from t. */
int m(char* r, char* t, int* ep) {
    int e;
    if (r[0] == 0) { *ep = 0; return 1; }
    if (r[1] == '*') {
        int i;
        i = 0;
        while (1) {
            if (m(r + 2, t + i, &e)) { *ep = i + e; return 1; }
            if (t[i] == 0) return 0;
            if (r[0] != '.' && t[i] != r[0]) return 0;
            i = i + 1;
        }
    }
    if (t[0] == 0) return 0;
    if (r[0] == '.' || r[0] == t[0]) {
        if (m(r + 1, t + 1, &e)) { *ep = 1 + e; return 1; }
    }
    return 0;
}

/* does `line` match `pat`? (fold = 1 -> compare lowercased) */
int line_match(char* pat, char* line, int fold) {
    char p[128];
    char l[512];
    int plen;
    int anchored;
    int endanchor;
    int i;
    int e;

    plen = 0;
    while (pat[plen] && plen < 127) { p[plen] = pat[plen]; plen++; }
    p[plen] = 0;
    i = 0;
    while (line[i] && i < 511) { l[i] = line[i]; i++; }
    l[i] = 0;

    anchored = 0;
    if (p[0] == '^') {
        anchored = 1;
        i = 0;
        while (p[i + 1]) { p[i] = p[i + 1]; i++; }
        p[i] = 0;
    }
    endanchor = 0;
    i = 0;
    while (p[i]) i++;
    if (i > 0 && p[i - 1] == '$') {
        endanchor = 1;
        p[i - 1] = 0;
    }
    if (fold) {
        i = 0;
        while (p[i]) { if (p[i] >= 'A' && p[i] <= 'Z') p[i] = p[i] + 32; i++; }
        i = 0;
        while (l[i]) { if (l[i] >= 'A' && l[i] <= 'Z') l[i] = l[i] + 32; i++; }
    }

    i = 0;
    while (1) {
        if (m(p, l + i, &e)) {
            if (!endanchor) return 1;
            if (l[i + e] == 0) return 1;
        }
        if (anchored || l[i] == 0) return 0;
        i = i + 1;
    }
}

/* read one line from an fopen() slot (fgetc based, '\r' skipped).
 * returns length, 0 = empty line, -1 = EOF (nothing read). */
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
    char pat[128];
    char fbuf[512];
    int foff[9];
    int nfiles;
    int fold;
    int lineno;
    int countonly;
    int invert;
    int anymatch;
    int fails;
    int n;
    int i;
    int f;

    n = getargs(args, 160);
    fold = 0; lineno = 0; countonly = 0; invert = 0;
    pat[0] = 0; nfiles = 0;
    foff[0] = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (args[i] == '-' && args[i + 1] != 0 && args[i + 1] != ' ') {
            i++;
            while (i < n && args[i] != ' ') {
                if (args[i] == 'i') fold = 1;
                else if (args[i] == 'n') lineno = 1;
                else if (args[i] == 'c') countonly = 1;
                else if (args[i] == 'v') invert = 1;
                i++;
            }
        } else {
            int pi;
            char tok[64];
            pi = 0;
            while (i < n && args[i] != ' ' && pi < 63) { tok[pi] = args[i]; pi++; i++; }
            tok[pi] = 0;
            if (pat[0] == 0) {
                int k;
                k = 0;
                while (tok[k] && k < 127) { pat[k] = tok[k]; k++; }
                pat[k] = 0;
            } else if (nfiles < 8) {
                int k;
                int base;
                base = foff[nfiles];
                k = 0;
                while (tok[k] && k < 62) { fbuf[base + k] = tok[k]; k++; }
                fbuf[base + k] = 0;
                nfiles++;
                foff[nfiles] = base + k + 1;
            }
        }
    }

    if (pat[0] == 0 || nfiles == 0) {
        printf("usage: grep [-i] [-n] [-c] [-v] PATTERN FILE [FILE ...]\n");
        printf("  pattern: text + . (any) X* (repeat) ^start end$\n");
        return 2;
    }

    anymatch = 0;
    fails = 0;
    for (f = 0; f < nfiles; f++) {
        char path[64];
        char line[512];
        int fd;
        int ln;
        int hits;
        int r;
        int k;
        k = 0;
        while (fbuf[foff[f] + k]) { path[k] = fbuf[foff[f] + k]; k++; }
        path[k] = 0;

        fd = fopen(path);
        if (fd == 0) {
            printf("grep: '%s' (cannot open)\n", path);
            fails++;
            continue;
        }
        ln = 0;
        hits = 0;
        while ((r = readline_fd(fd, line, 512)) >= 0) {
            ln++;
            if (line_match(pat, line, fold) != invert) {
                hits++;
                anymatch = 1;
                if (!countonly) {
                    if (nfiles > 1 && lineno)
                        printf("%s:%d:%s\n", path, ln, line);
                    else if (nfiles > 1)
                        printf("%s:%s\n", path, line);
                    else if (lineno)
                        printf("%d:%s\n", ln, line);
                    else
                        printf("%s\n", line);
                }
            }
        }
        fclose(fd);
        if (countonly) {
            if (nfiles > 1) printf("%s: %d\n", path, hits);
            else printf("%d\n", hits);
            if (hits > 0) anymatch = 1;
        }
    }
    if (fails) return 2;
    return anymatch ? 0 : 1;
}
