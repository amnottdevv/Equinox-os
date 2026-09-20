/* multitask.c — demo FASE B: multitasking dari dalam program C (mtcc).
 *
 * Compile & jalankan di-OS:
 *     mtcc /test/multitask.c
 *
 * Program ini:
 *   1. melaporkan pid-nya sendiri,
 *   2. SPAWN hello.mrp sebagai task BARU (non-blocking),
 *   3. tetap berputar dengan task_yield() sambil task anak berjalan,
 *   4. melaporkan hasilnya — dua task .mrp berjalan BERSAMAAN.
 */
#include <stdio.h>
#include <multitasking.h>

int main() {
    printf("== demo multitasking ==\n");
    printf("saya pid %d\n", task_pid());

    int pid = task_spawn("hello.mrp");
    if (pid > 0) {
        printf("spawn sukses: hello.mrp jadi pid %d\n", pid);
        printf("berputar sambil task anak jalan...\n");
        int i;
        for (i = 0; i < 10; i++) {
            task_yield();
            sleep(100);
            printf("parent hidup (putaran %d)\n", i);
        }
        printf("parent selesai\n");
    } else {
        printf("spawn GAGAL (err %d) - coba 'ps'\n", pid);
    }
    return 0;
}
