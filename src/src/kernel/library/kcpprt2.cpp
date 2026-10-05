/**
 * @file kcpprt2.cpp
 * @brief Runtime C++ minimal tambahan untuk port ThorVG (0.4 Beta, Task 1).
 *
 * Kernel Equinox: -fno-exceptions -fno-rtti -nostdlib. GCC tetap
 * mereferensikan sebagian simbol runtime C++ saat ada objek statis
 * dengan destructor (registrasi via __cxa_atexit) atau function-local
 * static (guard __cxa_*). Semuanya di-stub aman untuk single-thread:
 *  - __cxa_atexit   : no-op (kernel tidak pernah "exit")
 *  - __cxa_guard_*  : semantik guard byte single-thread
 *  - __cxa_pure_virtual: safety net — hentikan dengan pesan
 *
 * Catatan: .init_array kini DIJALANKAN oleh start.asm (loop
 * __init_array_start..__init_array_end sebelum kernel_main).
 */
#include "header/stdio.h"

extern "C" {

/* satu identitas DSO apa pun — hanya perlu ada & unik per binary */
void* __dso_handle = (void*)&__dso_handle;

int __cxa_atexit(void (*func)(void*), void* arg, void* dso) {
    (void)func; (void)arg; (void)dso;
    return 0;   /* no-op: kernel never exits, dtor tak perlu jalan */
}

/* guard function-local static (single-thread, tanpa reentransi) */
int __cxa_guard_acquire(unsigned char* guard) {
    return (*guard == 0) ? 1 : 0;
}
void __cxa_guard_release(unsigned char* guard) {
    *guard = 1;
}
void __cxa_guard_abort(unsigned char* guard) {
    *guard = 0;
}

void __cxa_pure_virtual(void) {
    printf("[cpp] FATAL: pure virtual call — system halted\n");
    for (;;) {
        __asm__ __volatile__("cli\n\thlt");
    }
}

} /* extern "C" */
