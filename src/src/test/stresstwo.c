/* stresstwo.c - 2-thread mtcc compile stress (v0.3.2 pool debugging).
 *
 * Spawns TWO mtcc.mrp tasks per round (exactly what the equinoxinstall
 * pool does), waits for both, and checks the exit codes:
 *   0 = compiled OK, 1 = compile error, 0x800000xx = task faulted.
 * The libc module sources stay in RAMFS, so the loop can run forever.
 *
 * Compile & run in-OS:  mtcc /test/stresstwo.c
 * Expected (healthy):   STRESS2 rounds=15 fails=0
 */
#include <stdio.h>
#include <multitasking.h>

int main() {
    int round;
    int fails;
    int pid1;
    int pid2;
    int st1;
    int st2;

    fails = 0;
    printf("== 2-thread compile stress: 15 rounds ==\n");

    for (round = 0; round < 15; round++) {
        st1 = -1;
        st2 = -1;
        pid1 = task_spawn_args("mtcc.mrp", 8388608,
                               "-q --lib -c /equinox/libc/stdio.c");
        pid2 = task_spawn_args("mtcc.mrp", 8388608,
                               "-q --lib -c /equinox/libc/printf.c");
        if (pid1 > 0) task_wait(pid1, &st1);
        else          st1 = pid1;
        if (pid2 > 0) task_wait(pid2, &st2);
        else          st2 = pid2;
        if (st1 != 0) fails = fails + 1;
        if (st2 != 0) fails = fails + 1;
        printf("round %2d: pid %d st 0x%x | pid %d st 0x%x\n",
               round, pid1, st1, pid2, st2);
    }

    printf("STRESS2 rounds=15 fails=%d\n", fails);
    return fails;
}
