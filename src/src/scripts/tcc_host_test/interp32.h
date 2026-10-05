// interp32.h - x86-32 interpreter (closed mtcc instruction subset) for
// compiler testing ON THE HOST without a 32-bit CPU/mode.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Virtual memory regions addressed by the interpreter:
//   code region : base..base+len, READ-ONLY (fetch instruksi + data const)
//   data region : base..base+len, read-write (string/global)
// Interpreter sendiri menyediakan: stack virtual (0x880000 turun, 256KB)
// dan arena malloc virtual (0xA00000, 512KB).
//
// Return: 0 = normal (program selesai: ret dari entry / exit syscall),
//         1 = abort (diagnostic ke stderr), 2 = exit syscall.
// *exit_code = EAX terakhir (main return / exit arg).
int interp32_run(const uint8_t* code, uint64_t code_base, uint32_t code_len,
                 const uint8_t* data, uint64_t data_base, uint32_t data_len,
                 uint64_t entry,
                 int* exit_code, uint64_t* ninstr);

#ifdef __cplusplus
}
#endif
