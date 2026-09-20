/* multitask.c — Phase B demo: multitasking from inside a C program (mtcc).
 *
 * Compile & run in-OS:
 *     mtcc /test/multitask.c
 *
 * This program:
 *   1. reports its own pid,
 *   2. SPAWNS hello.mrp as a NEW task (non-blocking),
 *   3. keeps looping with task_yield() while the child runs,
 *   4. reports the result — two .mrp tasks running CONCURRENTLY.
 */
#include <stdio.h>
#include <multitasking.h>

int main() {
    printf("== multitasking demo ==\n");
    printf("my pid is %d\n", task_pid());

    int pid = task_spawn("hello.mrp");
    if (pid > 0) {
        printf("spawn ok: hello.mrp is now pid %d\n", pid);
        printf("looping while the child task runs...\n");
        int i;
        for (i = 0; i < 10; i++) {
            task_yield();
            sleep(100);
            printf("parent alive (lap %d)\n", i);
        }
        printf("parent done\n");
    } else {
        printf("spawn FAILED (err %d) - try 'ps'\n", pid);
    }
    return 0;
}
