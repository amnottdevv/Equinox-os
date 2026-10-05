/* which.c — locate a command on the system path (v0.3 userland tool).
 *
 * Usage:  which NAME
 * Searches (in order): the CURRENT directory, /, /bin,
 * /equinox/tools, /equinox/games — for NAME and NAME.mrp.
 * Prints the first hit (like the shell's own dispatcher order:
 * `.` first, then the system path) and exits 0; exit 1 = not found.
 */
#include <stdio.h>
#include <fileio.h>

int try_dir(char* dir, char* name) {
    char p[96];
    int k;
    int i;
    int st[4];

    k = 0;
    if (dir[0]) {
        while (dir[k] && k < 80) { p[k] = dir[k]; k++; }
        if (k > 0 && p[k - 1] != '/' && k < 92) { p[k] = '/'; k++; }
    }
    i = 0;
    while (name[i] && k < 94) { p[k] = name[i]; k++; i++; }
    p[k] = 0;

    if (f_stat(p, st) == 0 && st[1] == 0) {
        printf("%s\n", p);
        return 1;
    }
    return 0;
}

int main() {
    char args[160];
    char name[64];
    int n;
    int i;
    int pn;

    n = getargs(args, 160);
    name[0] = 0;
    pn = 0;

    i = 0;
    while (i < n) {
        while (i < n && args[i] == ' ') i++;
        if (i >= n) break;
        pn = 0;
        while (i < n && args[i] != ' ' && pn < 62) { name[pn] = args[i]; pn++; i++; }
        name[pn] = 0;
    }

    if (name[0] == 0) {
        printf("usage: which NAME\n");
        return 2;
    }

    /* current directory first (that is how the shell resolves too) */
    if (try_dir("", name)) return 0;
    if (try_dir("/", name)) return 0;
    if (try_dir("/bin", name)) return 0;
    if (try_dir("/equinox/tools", name)) return 0;
    if (try_dir("/equinox/games", name)) return 0;

    /* then the auto .mrp append */
    {
        char mname[70];
        int k;
        k = 0;
        while (name[k] && k < 64) { mname[k] = name[k]; k++; }
        mname[k] = '.'; k++;
        mname[k] = 'm'; k++;
        mname[k] = 'r'; k++;
        mname[k] = 'p'; k++;
        mname[k] = 0;
        if (try_dir("", mname)) return 0;
        if (try_dir("/", mname)) return 0;
        if (try_dir("/bin", mname)) return 0;
        if (try_dir("/equinox/tools", mname)) return 0;
        if (try_dir("/equinox/games", mname)) return 0;
    }
    printf("which: '%s' not found on the system path\n", name);
    return 1;
}
