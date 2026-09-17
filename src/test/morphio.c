/* morphio.c — Morph.h file API regression.
 * Runs both in the host interpreter (make test) and in Equinox OS
 * (mtcc test/morphio.c). Exercises the whole file story:
 *   exists -> write (create) -> size -> read_all -> edit in memory
 *   -> write (overwrite / "timpa") -> verify -> open/read/close fd path
 *   -> error paths on a missing file.
 *
 * Uses ONLY mtcc-supported constructs (no preprocessor, no static,
 * no typedef): built-ins file_exists / file_write / file_size /
 * file_read_all / file_open / file_read / file_close — the exact
 * Morph.h names.
 */

/* print a signed int (printint() itself is unsigned-only) */
void prn(int v) {
    if (v < 0) {
        print("-");
        v = -v;
    }
    printint(v);
}

/* tiny strcmp so we can verify file content without a libc */
int streq_(char* a, char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

int main() {
    char buf[64];
    int r;

    print("1 exists  : "); prn(file_exists("morphio.dat")); print("\n");   /* 0  */

    r = file_write("morphio.dat", "hello world", 11);                      /* create */
    print("2 write   : "); prn(r); print("\n");                            /* 0  */

    print("3 exists  : "); prn(file_exists("morphio.dat")); print("\n");   /* 1  */
    print("4 size    : "); prn(file_size("morphio.dat")); print("\n");     /* 11 */

    r = file_read_all("morphio.dat", buf, 64);
    print("5 readall : "); prn(r); print("\n");                            /* 11 */
    buf[r] = 0;                                                             /* NUL-terminate */
    print("6 content : "); print(buf); print("\n");                         /* hello world */

    /* ---- edit pattern: load -> modify in memory -> overwrite ---- */
    buf[11] = ' ';
    buf[12] = 'v';
    buf[13] = '2';
    buf[14] = 0;
    r = file_write("morphio.dat", buf, 14);                                 /* timpa! */
    print("7 edit    : "); prn(r); print("\n");                             /* 0  */
    print("8 size2   : "); prn(file_size("morphio.dat")); print("\n");     /* 14 */

    r = file_read_all("morphio.dat", buf, 64);
    print("9 read2   : "); prn(r); print("\n");                             /* 14 */
    buf[r] = 0;
    print("10 streq  : "); prn(streq_(buf, "hello world v2")); print("\n"); /* 1  */

    /* ---- classic fd path: open -> sequential read -> close ---- */
    r = file_open("morphio.dat");
    print("11 open   : "); prn(r); print("\n");                             /* 3  */
    r = file_read(r, buf, 5);
    print("12 read5  : "); prn(r); print("\n");                             /* 5  */
    buf[5] = 0;
    print("13 head   : "); print(buf); print("\n");                         /* hello */
    print("14 close  : "); prn(file_close(3)); print("\n");                 /* 0  */

    /* ---- error paths: missing file ---- */
    print("15 miss   : "); prn(file_size("nope.dat")); print("\n");         /* -3 */
    print("16 rdmiss : "); prn(file_read_all("nope.dat", buf, 64)); print("\n"); /* -3 */

    return 0;
}
