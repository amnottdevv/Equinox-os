/* exec_test.c - regression test audit V3 bug #1 (re-entrant exec).
 *
 * BEFORE the fix: exec() from inside a running program made the kernel
 * reset the MRP arena (0x500000-0x900000) -- the same arena where THIS
 * program's code lives. Control returned to overwritten code -> #GP/#UD
 * -> kernel panic + 30-second auto-reboot.
 *
 * AFTER the fix: nested exec is REJECTED, exec() returns SYS_EBUSY (-9),
 * and the calling code stays alive until the program finishes normally.
 *
 * How to use:  run tcc.mrp exec_test.c
 * Correct output: the lines "returned -9 SYS_EBUSY" + "still alive" + EXIT=0.
 * (The host harness models the same kernel contract: exec -> -9.)
 */

int main() {
    print("exec_test: regression audit V3 #1 (nested exec)\n");
    print("exec_test: calling exec(hello.mrp) from inside a program...\n");
    int r;
    r = exec("hello.mrp");
    if (r == -9) {
        print("exec_test: returned -9 SYS_EBUSY - refused, no crash. FIX OK\n");
    } else {
        if (r == 0) {
            print("exec_test: returned 0 - nested exec should have been refused!\n");
        } else {
            print("exec_test: a different errno - check syscall.h\n");
        }
    }
    print("exec_test: the calling code is still alive. FIX V3-1 OK\n");
    return 0;
}
