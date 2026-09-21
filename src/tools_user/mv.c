/* mv.c — rename/move a file (v0.3 FR-01 userland tool).
 *
 * Usage:  mv <old> <new>
 * RAMFS: an O(1) relink. FAT: create-copy-delete. Renaming over an
 * existing FILE replaces it (POSIX semantics); over a directory is
 * refused. Renaming FAT directories is not supported yet (-7).
 */
#include <stdio.h>
#include <fileio.h>

int main() {
    char args[128];
    int n = getargs(args, 128);
    char oldp[80];
    char newp[80];
    int i = 0;
    int oi = 0;
    int ni = 0;

    while (i < n && args[i] == ' ') i++;
    while (i < n && args[i] != ' ' && oi < 78) oldp[oi++] = args[i++];
    while (i < n && args[i] == ' ') i++;
    while (i < n && args[i] != ' ' && ni < 78) newp[ni++] = args[i++];
    oldp[oi] = 0;
    newp[ni] = 0;

    if (oldp[0] == 0 || newp[0] == 0) {
        printf("usage: mv <old> <new>\n");
        return 1;
    }
    int r = f_rename(oldp, newp);
    if (r == 0) {
        printf("'%s' -> '%s'\n", oldp, newp);
        return 0;
    }
    if (r == -12) printf("mv: '%s' already exists (dir?)\n", newp);
    else if (r == -7) printf("mv: renaming a FAT directory is not supported\n");
    else printf("mv: failed (err %d)\n", r);
    return 1;
}
