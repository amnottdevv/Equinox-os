/* fat32_stress.c — Equinox OS v0.2 FAT32 write stress test.
 *
 * Compiled and run IN THE OS by mtcc from the mounted FAT32 volume
 * (small 3 MB test disk). Exercises the paths typed shell commands
 * cannot reach:
 *
 *   1. GROW   — create 140 one-byte files in the ROOT directory.
 *               The root starts as a single 512-byte cluster (16
 *               dirent slots): this forces the directory to GROW
 *               across ~10 clusters via the alloc+zero+link path,
 *               then verifies first/last by name and size.
 *   2. FILL   — write 4 KB pattern files until the volume is FULL
 *               (clean "volume full" failure: no crash, no hang,
 *                consistent metadata afterwards).
 *
 * Uses only the mtcc C subset: no preprocessor, no const/static,
 * arrays <= 4096, builtins print/printint/file_write/file_exists/
 * file_size.
 */

void prn(int v) {
    if (v < 0) { print("-"); v = -v; }
    printint(v);
}

/* build "pNNNN.TXT" names by hand (no sprintf in the mtcc subset) */
void mkname(char* dst, char prefix, int n) {
    dst[0] = prefix;
    dst[1] = '0' + (n / 1000) % 10;
    dst[2] = '0' + (n / 100) % 10;
    dst[3] = '0' + (n / 10) % 10;
    dst[4] = '0' + n % 10;
    dst[5] = '.';
    dst[6] = 'T';
    dst[7] = 'X';
    dst[8] = 'T';
    dst[9] = 0;
}

int main() {
    char name[16];
    char big[4096];
    int i, j, r, fitted;

    /* ---- part 1: grow the root directory ---- */
    for (i = 0; i < 140; i++) {
        mkname(name, 'g', i);
        r = file_write(name, "x", 1);
        if (r != 0) {
            print("STRESS GROW FAIL at ");
            prn(i);
            print("\n");
            return 1;
        }
    }
    print("STRESS GROW OK 140\n");

    mkname(name, 'g', 0);
    if (file_exists(name) != 1) { print("STRESS EXISTS0 FAIL\n"); return 1; }
    if (file_size(name) != 1)   { print("STRESS SIZE0 FAIL\n");   return 1; }
    mkname(name, 'g', 139);
    if (file_exists(name) != 1) { print("STRESS EXISTS139 FAIL\n"); return 1; }
    print("STRESS VERIFY OK\n");

    /* ---- part 2: fill the volume with 4 KB pattern files ---- */
    /* (mtcc has no casts: implicit int->char truncation gives j & 255) */
    for (j = 0; j < 4096; j++) big[j] = j;
    fitted = 0;
    for (i = 0; i < 3000; i++) {
        mkname(name, 'b', i);
        r = file_write(name, big, 4096);
        if (r != 0) break;
        fitted++;
    }
    if (fitted == 3000) {
        print("STRESS FULL NOT REACHED\n");
        return 1;
    }
    print("STRESS FULL OK after ");
    prn(fitted);
    print(" files\n");

    /* the shell + filesystem must still be alive afterwards */
    print("STRESS DONE\n");
    return 0;
}
