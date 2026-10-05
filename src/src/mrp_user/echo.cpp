// echo.cpp — v0.8: echo ala Unix untuk pipe/redirect eqshell.
//
// print() -> SYS_PRINT -> kernel merutekan ke fd1 bila ter-redirect
// (shell: echo "teks" > file). Tanpa argumen: cetak baris kosong.
//
// Run:  echo hello world
//       echo data penting > /user/catatan.txt
//       cat /user/catatan.txt

#include "Morph.h"

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;

    char args[128];
    int n = getargs(args, (int)sizeof(args));
    if (n > 0) {
        print(args);
    }
    print("\n");
    exit(0);
}
