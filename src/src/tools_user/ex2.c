/* ex2.c — extended 0.5 syscall self-test: COW fork, sbrk/mmap, socket.
 *
 * Run:   mtcc /equinox/tools/ex2.c      (compile + run in one step)
 *   or:  equinoxinstall ; ex2
 *
 * What it proves
 *   1. mmap()  hands out a demand-backed window that is writable
 *      (the first store faults the page in).
 *   2. sbrk()  moves the program break and reports the old one.
 *   3. fork()  copies the address space lazily: the child sees the
 *      parent's mmap value, then writes its OWN copy — the parent's
 *      page must stay untouched (COW isolation), and wait() must reap
 *      the child with its exit status.
 *   4. socket()/net() open a ring-3 TCP client. The connect check is
 *      a SKIP when no host HTTP server is reachable, so the test also
 *      passes on a bare `make run`.
 *
 * NOTE: mtcc is a deliberate C subset — no `const`, no casts, no
 * sizeof — hence `int` addresses and one helper function.
 */
#include <stdio.h>
#include <multitasking.h>

int fails = 0;

void ck(int cond, char* msg) {
    if (cond) {
        printf("[PASS] %s\n", msg);
    } else {
        printf("[FAIL] %s\n", msg);
        fails = fails + 1;
    }
}

int main() {
    int mp;         /* mmap window address (kept in an int: no casts) */
    int* p;
    int b0;         /* program break before the grow */
    int b1;         /* sbrk(8192) -> old break */
    int heap;       /* a page inside the grown break */
    int* hp;
    int child;
    int status;
    int reaped;
    int sock;
    int r;
    int n;
    char buf[256];

    printf("== ex2: fork / sbrk / mmap / socket ==\n");

    /* ---- 1. mmap: demand-backed, zero-filled on first touch ---- */
    mp = mmap(0, 4096);
    p = mp;
    ck(mp != 0, "mmap returned a window");
    *p = 0xC0DE1234;
    ck(*p == 0xC0DE1234, "mmap window writable (page faulted in)");

    /* ---- 2. sbrk: the break grows, the old break is returned ---- */
    b0 = sbrk(0);
    ck(b0 != 0, "sbrk(0) reports the program break");
    b1 = sbrk(8192);
    ck(b1 == b0, "sbrk(8192) returns the OLD break");
    ck(sbrk(0) == b0 + 8192, "break advanced by 8192");
    heap = b0 + 4096;
    hp = heap;
    *hp = 0x5A5A5A5A;
    ck(*hp == 0x5A5A5A5A, "heap page inside the break is writable");

    /* ---- 3. fork: lazy copy-on-write + wait() ---- */
    child = fork();
    if (child == 0) {
        /* child: same value as the parent BEFORE its first write ... */
        printf("child: pid=%d mmap=%x\n", getpid(), *p);
        *p = 0x87654321;                 /* ... own copy AFTER it */
        printf("child: wrote %x\n", *p);
        exit(0);
    } else if (child > 0) {
        reaped = task_wait(child, &status);
        printf("parent: child=%d reaped=%d status=%d mmap=%x\n",
               child, reaped, status, *p);
        ck(reaped == child, "wait() returned the forked child");
        ck(status == 0, "child exit status is 0");
        ck(*p == 0xC0DE1234, "parent page unchanged (COW isolation)");
    } else {
        ck(0, "fork() returned a child pid");
    }

    /* ---- 4. ring-3 TCP client ---- */
    sock = socket(2, 1, 0);              /* AF_INET, SOCK_STREAM */
    ck(sock >= 0, "socket(AF_INET, SOCK_STREAM) opened");
    if (sock >= 0) {
        r = net(sock, "10.0.2.2", 8081); /* QEMU user-net gateway */
        if (r == 0) {
            n = write(sock, "GET /ex2.txt HTTP/1.0\r\n\r\n", 25);
            printf("socket: wrote %d bytes\n", n);
            n = read(sock, buf, 255);
            printf("socket: read %d bytes\n", n);
            ck(n > 0, "socket round-trip from the HTTP server");
        } else {
            printf("[SKIP] net() connect refused (%d) - no host server\n", r);
        }
        close(sock);
    }

    printf("ex2 RESULT fails=%d\n", fails);
    exit(0);
}
