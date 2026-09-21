/* find.c — walk a directory tree and print paths (v0.3 userland tool).
 *
 * Usage:  find DIR [-name SUBSTR]
 *   -name SUBSTR   only names containing SUBSTR
 * Prints every FILE path under DIR (recursively, depth <= 8),
 * and a summary line. Directories are walked but not listed
 * (they show as "N dirs" in the summary).
 */
#include <stdio.h>
#include <fileio.h>

int g_files;
int g_dirs;
char g_pat[64];
int g_have_pat;

/* dirent layout: name[0..63], is_dir int at +64, size at +68 */
int de_isdir(char* de) {
    return (de[64] & 255) | ((de[65] & 255) << 8);
}

void walk(char* path, int depth) {
    char de[72];
    char child[192];
    int fd;
    int r;
    int i;
    int k;

    if (depth > 8) return;
    fd = f_open(path, F_DIR);
    if (fd < 0) return;

    while ((r = f_readdir(fd, de)) == 1) {
        char name[65];
        i = 0;
        while (i < 63 && de[i]) { name[i] = de[i]; i++; }
        name[i] = 0;
        if (name[0] == 0) continue;
        if (name[0] == '.' && name[1] == 0) continue;
        if (name[0] == '.' && name[1] == '.' && name[2] == 0) continue;

        /* child = path + "/" + name */
        k = 0;
        while (path[k] && k < 150) { child[k] = path[k]; k++; }
        if (k > 0 && k < 190 && child[k - 1] != '/') { child[k] = '/'; k++; }
        i = 0;
        while (name[i] && k < 191) { child[k] = name[i]; k++; i++; }
        child[k] = 0;

        if (de_isdir(de)) {
            g_dirs++;
            walk(child, depth + 1);
        } else {
            int match;
            match = 1;
            if (g_have_pat && !strstr(name, g_pat)) match = 0;
            if (match) {
                printf("%s\n", child);
                g_files++;
            }
        }
    }
    close(fd);
}

int main() {
    char args[160];
    char path[96];
    int n;
    int i;
    int pn;

    n = getargs(args, 160);
    path[0] = 0;
    pn = 0;
    g_files = 0;
    g_dirs = 0;
    g_pat[0] = 0;
    g_have_pat = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        if (args[i] == '-' && args[i + 1] == 'n' && args[i + 2] == 'a' &&
            args[i + 3] == 'm' && args[i + 4] == 'e' && args[i + 5] == ' ') {
            i = i + 6;
            while (i < n && args[i] == ' ') i++;
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 62) { g_pat[pn] = args[i]; pn++; i++; }
            g_pat[pn] = 0;
            if (g_pat[0]) g_have_pat = 1;
        } else {
            pn = 0;
            while (i < n && args[i] != ' ' && pn < 94) { path[pn] = args[i]; pn++; i++; }
            path[pn] = 0;
        }
    }

    if (path[0] == 0) {
        printf("usage: find DIR [-name SUBSTR]\n");
        return 2;
    }
    /* the root itself must be a directory */
    {
        int st[4];
        if (f_stat(path, st) < 0 || st[1] == 0) {
            printf("find: '%s' is not a directory\n", path);
            return 1;
        }
    }
    walk(path, 0);
    printf("find: %d file(s), %d dir(s) under '%s' (depth <= 8)\n",
           g_files, g_dirs, path);
    return 0;
}
