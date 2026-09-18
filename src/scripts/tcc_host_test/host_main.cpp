// host_main.cpp — host-side (Linux 64-bit) mtcc test harness.
// ----------------------------------------------------------------------------
//  Includes the compiler source DIRECTLY (MTCC_HOST_TEST mode) so there
//  is no API boundary between TUs — the core compiler is tested as-is.
//  Compiled code runs through the x86-32 interpreter (interp32.cpp).
//
//  Usage:
//    mtcc_host run <file.c>   — compile + interpret in-memory (mode A /
//                               equivalent to `run tcc.mrp file.c` in Equinox OS)
//    mtcc_host c <file.c>     — compile + assemble .mrp + validate + write
//    <base>.mrp + interpret exactly like the kernel's mrp_run loader
//                               (mode B / equivalent to `run tcc.mrp -c file.c`
//                               followed by `run <base>.mrp`)
//
//  Output protocol for the test runner:
//    - stdout  = the compiled program's output, ending with the line "EXIT=<n>"
//    - stderr  = chatter compiler + diagnostic interpreter
//    - exit 1  = compile error / abort interpreter
#define MTCC_HOST_TEST 1
#include "../../mtcc.c"   /* canonical source (v10.14; was mrp_user/mtcc.cpp) */
#include "interp32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mtcc_host run|c <file.c>\n");
        return 2;
    }
    const char* mode = argv[1];
    const char* path = argv[2];

    char* src = NULL;
    uint32_t src_len = 0;
    if (os_read_file(path, &src, &src_len) != 0 || !src) {
        fprintf(stderr, "[tcc] cannot read %s\n", path);
        return 1;
    }

    MtccOut out;
    int cr = mtcc_compile(src, src_len, &out);
    if (cr != 0) {
        if (cr == 2) fprintf(stderr, "[tcc] error: out of memory\n");
        else fprintf(stderr, "[tcc] error line %u: %s\n", S.err_line, S.err_msg);
        return 1;
    }
    fprintf(stderr, "[tcc] compile OK: %u fungsi, %u byte kode, %u byte data\n",
            out.nfuncs, out.code_len, out.data_len);

    // Dump hex kode utk debug codegen (MTCC_DUMP=1)
    if (getenv("MTCC_DUMP")) {
        fprintf(stderr, "[tcc] code dump (%u byte):", out.code_len);
        for (uint32_t i = 0; i < out.code_len; i++) {
            if ((i & 15) == 0) fprintf(stderr, "\n%04x: ", i);
            fprintf(stderr, "%02x ", out.code[i]);
        }
        fprintf(stderr, "\n");
        if (out.data_len) {
            fprintf(stderr, "[tcc] data dump (%u byte):", out.data_len);
            for (uint32_t i = 0; i < out.data_len; i++) {
                if ((i & 15) == 0) fprintf(stderr, "\n%04x: ", i);
                fprintf(stderr, "%02x ", out.data[i]);
            }
            fprintf(stderr, "\n");
        }
    }

    int ec = 0;
    uint64_t ni = 0;
    int r = 1;

    if (strcmp(mode, "run") == 0) {
                // ---- mode A: patch the real buffer address (MAP_32BIT -> load uint32) ----
        mtcc_patch(&out, (uint32_t)(uintptr_t)out.code,
                          (uint32_t)(uintptr_t)out.data);
        r = interp32_run(out.code, (uint64_t)(uintptr_t)out.code, out.code_len,
                         out.data, (uint64_t)(uintptr_t)out.data, out.data_len,
                         (uint64_t)(uintptr_t)out.code, &ec, &ni);
    } else if (strcmp(mode, "c") == 0) {
        // ---- mode B: assemble the .mrp image + validate + run at base 0x500010 ----
        uint32_t need = MRP_HEADER_SIZE + out.code_len + 4 + out.data_len;
        uint8_t* image = (uint8_t*)malloc(need);
        if (!image) { fprintf(stderr, "[tcc] OOM image\n"); return 1; }
        uint32_t total = mtcc_build_image(&out, image, need);
        if (total == 0) { fprintf(stderr, "[tcc] image build failed\n"); return 1; }

        enum mrp_validate_reason vr;
        if (!is_valid_mrp(image, total, &vr)) {
            fprintf(stderr, "[tcc] image invalid: %s\n", mrp_reason_str(vr));
            return 1;
        }

        const char* base = path;
        for (const char* q = path; *q; q++) if (*q == '/') base = q + 1;
        char oname[256];
        int j = 0;
        while (base[j] && base[j] != '.' && j < 248) { oname[j] = base[j]; j++; }
        oname[j++] = '.'; oname[j++] = 'm'; oname[j++] = 'r'; oname[j++] = 'p';
        oname[j] = '\0';
        if (os_write_file(oname, image, total) != 0) {
            fprintf(stderr, "[tcc] failed to write %s\n", oname);
            return 1;
        }
        fprintf(stderr, "[tcc] tulis %s (%u byte)\n", oname, total);

        uint32_t pad = (4 - (out.code_len & 3)) & 3;
        uint64_t code_base = 0x500010;                     // MRP_LOAD_BASE
        uint64_t data_base = code_base + out.code_len + pad;
        r = interp32_run(image + MRP_HEADER_SIZE, code_base, out.code_len,
                         image + MRP_HEADER_SIZE + out.code_len + pad,
                         data_base, out.data_len,
                         code_base, &ec, &ni);
    } else {
        fprintf(stderr, "mode asing: %s\n", mode);
        return 2;
    }

    if (r != 0) return 1;
    printf("EXIT=%d\n", ec);
    fprintf(stderr, "[tcc] %llu instruksi di-interpretasi\n",
            (unsigned long long)ni);
    return 0;
}
