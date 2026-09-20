/* pipedemo.c — v0.3 (FR-02) pipe + wait demo (mtcc subset C).
 *
 * Run:   pipedemo
 *
 * One program, two roles (the args decide):
 *   parent: creates a pipe, spawns itself again with args "w",
 *           closes its write end, reads lines until EOF, then
 *           wait()s and reports the child's pid + exit status.
 *   child : (args start with 'w') inherits the pipe fds, closes the
 *           read end, writes 3 messages, exits with status 7.
 *
 * The whole thing proves: SYS_PIPE, fd inheritance across spawn,
 * blocking read (the parent parks until data arrives), EOF on last
 * write-end close, and waitpid zombie collection — the classic
 * `prog | prog` plumbing in miniature.
 */
#include <stdio.h>
#include <multitasking.h>

int main() {
    char args[128];
    getargs(args, 128);

    /* ---- child branch: args "w <n>" = writer ---- */
    if (args[0] == 'w') {
        int wfd = 3;    /* fd 3 = inherited READ end; fd 4 = WRITE end */
        close(3);       /* the child never reads                        */
        write(4, "pipe: hello from the child\n", 27);
        write(4, "pipe: second message\n", 21);
        write(4, "pipe: bye!\n", 11);
        close(4);
        exit(7);        /* wait() reports this status                   */
    }

    /* ---- parent branch ---- */
    printf("== pipe + wait demo ==\n");
    int fds[2];
    int r = pipe_create(fds);
    if (r != 0) {
        printf("pipe_create FAILED (err %d)\n", r);
        return 1;
    }
    printf("pipe created: read=%d write=%d\n", fds[0], fds[1]);

    int pid = task_spawn_args("pipedemo.mrp", 0, "w");
    if (pid <= 0) {
        printf("spawn FAILED (err %d)\n", pid);
        return 1;
    }
    printf("child spawned: pid %d (it inherits the pipe)\n", pid);

    close(fds[1]);     /* parent never writes: EOF becomes detectable */

    printf("---- pipe output ----\n");
    int total = 0;
    char buf[64];
    int n = read(fds[0], buf, 63);
    while (n > 0) {
        buf[n] = 0;
        printf("%s", buf);
        total = total + n;
        n = read(fds[0], buf, 63);
    }
    printf("---- EOF (%d bytes) ----\n", total);
    close(fds[0]);

    int status = 0;
    int got = task_wait(0, &status);
    if (got > 0) {
        printf("wait: child %d exited with status %d\n", got, status);
        if (status == 7 && total > 0) {
            printf("PIPEDEMO RESULT: PASS\n");
        } else {
            printf("PIPEDEMO RESULT: FAIL (status %d, %d bytes)\n",
                   status, total);
        }
    } else {
        printf("wait FAILED (err %d)\n", got);
    }
    return 0;
}
