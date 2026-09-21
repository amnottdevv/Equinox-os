/* fstest.c — v0.3 FR-01 / FR-03 syscall regression suite (runs in-OS).
 *
 * Run:   fstest          (from any directory; cleans up after itself)
 * Output: one "PASS/FAIL <name>" line per check, then
 *         "FSTEST SUMMARY: <n>/<total> PASS" — the QEMU regression
 *         harness greps the serial mirror for that line.
 */
#include <stdio.h>
#include <fileio.h>

int g_pass = 0;
int g_fail = 0;

void check(char* name, int ok) {
    if (ok) {
        printf("PASS %s\n", name);
        g_pass++;
    } else {
        printf("FAIL %s\n", name);
        g_fail++;
    }
}

int main() {
    char* msg = "Hello fileio v0.3";
    int len = strlen(msg);
    char buf[64];

    /* ---- 1. open2 O_CREAT|O_RDWR|O_TRUNC ---------------------- */
    int fd = f_open("fstest.tmp", F_RDWR | F_CREAT | F_TRUNC);
    check("open2 create rw", fd >= 3);

    /* ---- 2. write() to the fd --------------------------------- */
    int n = write(fd, msg, len);
    check("write to fd", n == len);

    /* ---- 3. lseek back + read-back verify --------------------- */
    int sk = lseek(fd, 0, 0);
    int rn = read(fd, buf, len);
    buf[rn > 0 ? rn : 0] = 0;
    check("lseek rewind", sk == 0);
    check("read back", rn == len && strcmp(buf, msg) == 0);

    /* ---- 4. fstat by descriptor ------------------------------- */
    int st[4];
    int fs = f_fstat(fd, st);
    check("fstat size", fs == 0 && st[0] == len && st[1] == 0);
    close(fd);

    /* ---- 5. stat by path -------------------------------------- */
    fs = f_stat("fstest.tmp", st);
    check("stat size/type", fs == 0 && st[0] == len && st[1] == 0);

    /* ---- 6. O_APPEND ------------------------------------------- */
    fd = f_open("fstest.tmp", F_WRONLY | F_APPEND);
    n = write(fd, "!", 1);
    close(fd);                       /* flush (FAT write-through here) */
    check("append write", n == 1);
    fd = f_open("fstest.tmp", F_RDONLY);
    lseek(fd, len, 0);
    n = read(fd, buf, 8);
    check("append visible", n == 1 && buf[0] == '!');
    close(fd);

    /* ---- 7. O_EXCL on an existing file ------------------------ */
    fd = f_open("fstest.tmp", F_WRONLY | F_CREAT | F_EXCL);
    check("O_EXCL refuses existing", fd == -12);

    /* ---- 8. mkdir + readdir + rmdir ---------------------------- */
    int md = f_mkdir("fstest.d");
    check("mkdir", md == 0);
    check("mkdir EEXIST", f_mkdir("fstest.d") == -12);
    fs = f_stat("fstest.d", st);
    check("stat dir", fs == 0 && st[1] == 1);
    int dfd = f_open(".", F_DIR);
    int seen = 0;
    char de[72];
    while (f_readdir(dfd, de) == 1) {
        if (strcmp(de, "fstest.d") == 0) seen = 1;
    }
    close(dfd);
    check("readdir sees new dir", seen == 1);
    check("rmdir", f_rmdir("fstest.d") == 0);
    check("rmdir leaves no stat", f_stat("fstest.d", st) == -3);

    /* ---- 9. rename --------------------------------------------- */
    int rn2 = f_rename("fstest.tmp", "fstest2.tmp");
    check("rename", rn2 == 0);
    check("rename old gone", file_exists("fstest.tmp") == 0);
    check("rename new exists", file_exists("fstest2.tmp") == 1);
    check("rename back", f_rename("fstest2.tmp", "fstest.tmp") == 0);

    /* ---- 10. unlink -------------------------------------------- */
    check("unlink", f_unlink("fstest.tmp") == 0);
    check("unlink ENOENT", f_unlink("fstest.tmp") == -3);
    check("stat after unlink", f_stat("fstest.tmp", st) == -3);

    /* ---- 11. FR-03: raw arena malloc/free loop ----------------- */
    /* __arena_alloc = SYS_MALLOC (raw kernel arena block);
     * f_free = SYS_FREE. 100 alloc/free rounds of 4 KB must ALL
     * succeed — the arena is a split+coalesce free-list now. */
    int ok = 1;
    int i;
    for (i = 0; i < 100; i++) {
        int* p = __arena_alloc(4096);
        if (p == 0) { ok = 0; break; }
        p[0] = i;
        p[1023] = i;
        if (f_free(p) != 0) { ok = 0; break; }
    }
    check("arena malloc/free x100", ok);

    /* ---- 12. prelude heap malloc/free loop ---------------------- */
    ok = 1;
    for (i = 0; i < 50; i++) {
        char* q = malloc(256);
        if (q == 0) { ok = 0; break; }
        q[0] = 'x';
        q[255] = 'y';
        free(q);
    }
    check("user heap malloc/free x50", ok);

    printf("FSTEST SUMMARY: %d/%d PASS\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
