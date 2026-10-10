// host_main.cpp — host-side (Linux 64-bit) mtcc driver + test harness.
// ----------------------------------------------------------------------------
//  Includes the compiler source DIRECTLY (MTCC_HOST_TEST mode) so there
//  is no API boundary between TUs — the core compiler is tested as-is.
//  Compiled code runs through the x86-32 interpreter (interp32.cpp).
//
//  Usage (the same flag set as the in-OS driver):
//    mtcc_host run  [opts] <file.c> [file2.c ...]   compile + interpret in-memory
//    mtcc_host c    [opts] <file.c> [file2.c ...]   compile -> image file -> run it
//    mtcc_host lib  <file.c>                        check-compile (no main)
//    mtcc_host make <file.ruf>                      run a .ruf recipe
//
//    opts: -o <out>            output name (default: first source, .mrp/.elf)
//          -format mrp|elf     image format (default mrp)
//          -multiple-files     explicit "link these files" marker (optional —
//                              several files are detected automatically)
//          -q                  quiet: no compiler chatter
//          --debug             verbose (per-file reads, code dump)
//          --lib               library mode: main() not required
//
//  Output protocol for the test runner:
//    - stdout  = the compiled program's output, ending with the line "EXIT=<n>"
//    - stderr  = compiler chatter ([COMPILE]/[LINK]/[ERROR] tags, coloured
//                when stderr is a TTY) + diagnostic interpreter messages
//    - exit 1  = compile error / abort interpreter
//
//  Colour: ANSI escapes only when stderr is a TTY (so run_tests.py, which
//  byte-compares stdout, is unaffected); NO_COLOR=1 turns them off too.
#define MTCC_HOST_TEST 1
#include "../../mtcc.c"   /* canonical source (v10.14; was mrp_user/mtcc.cpp) */
#include "interp32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void) {
    fprintf(stderr,
        "mtcc host driver — Equinox OS TinyCC\n"
        "usage: mtcc_host <run|c|lib|make> [options] <file.c> [file2.c ...]\n"
        "\n"
        "  run    compile + link the file list, then interpret in-memory\n"
        "  c      compile + write the image (next to the first source), then\n"
        "         load that file back and interpret it — exactly like the OS\n"
        "  lib    check-compile a library module (no main required)\n"
        "  make   run a .ruf build recipe (same engine as `mtcc -make`)\n"
        "\n"
        "  -o <name>          output name (default: first source, ext per format)\n"
        "  -format mrp|elf    image format: MRP1 (default) or static ELF32\n"
        "  -multiple-files    explicit multi-file marker (optional; several files\n"
        "                     on the command line already link into ONE program)\n"
        "  -q                 quiet — no compiler chatter\n"
        "  --debug, -d        verbose output (per-file reads, code dump)\n"
        "  --lib              library mode: main() not required\n"
        "  -h, --help         show this summary\n");
}

/* split "a.c,b.c" style arguments — commas are accepted as separators */
static void add_file(const char** files, int* n, int max, const char* arg) {
    const char* p = arg;
    while (*p) {
        const char* start = p;
        while (*p && *p != ',') p++;
        /* trim surrounding blanks */
        const char* e = p;
        while (e > start && (e[-1] == ' ' || e[-1] == '\t')) e--;
        const char* s = start;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        if (e > s) {
            if (*n >= max) { fprintf(stderr, "  [ERROR]   too many source files (max %d)\n", max); exit(2); }
            int len = (int)(e - s);
            if (len > 240) len = 240;
            char* buf = (char*)malloc((size_t)len + 1);
            memcpy(buf, s, (size_t)len);
            buf[len] = '\0';
            files[(*n)++] = buf;
        }
        if (*p == ',') p++;
    }
}

/* default output name: first source without its extension */
static void default_out_name(const char* first, int elf, char* dst, int cap) {
    const char* base = first;
    for (const char* q = first; *q; q++) if (*q == '/') base = q + 1;
    int j = 0;
    while (base[j] && base[j] != '.' && j < cap - 6) { dst[j] = base[j]; j++; }
    dst[j++] = '.';
    if (elf) { dst[j++] = 'e'; dst[j++] = 'l'; dst[j++] = 'f'; }
    else     { dst[j++] = 'm'; dst[j++] = 'r'; dst[j++] = 'p'; }
    dst[j] = '\0';
}

/* -o value -> final file name (extension appended unless already right) */
static void out_name_from(const char* user, int elf, char* dst, int cap) {
    const char* want = elf ? ".elf" : ".mrp";
    size_t ul = strlen(user), wl = strlen(want);
    int has = (ul > wl && strcmp(user + ul - wl, want) == 0);
    snprintf(dst, (size_t)cap, "%s%s", user, has ? "" : want);
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    const char* mode = argv[1];

    /* colour: ANSI only when stderr is a TTY (and NO_COLOR is unset) */
    g_color = isatty(2) && !getenv("NO_COLOR");

    // "make" mode: the SAME .ruf engine as the in-OS `mtcc -make`
    // (mtcc_make_run — called directly because MRP_ENTRY/main() only
    // exists in the .mrp build).
    if (strcmp(mode, "make") == 0) {
        int rc = mtcc_make_run(argv[2]);
        printf("EXIT=%d\n", rc);
        return rc ? 1 : 0;
    }
    if (strcmp(mode, "run") && strcmp(mode, "c") && strcmp(mode, "lib")) {
        fprintf(stderr, "  [ERROR]   unknown mode '%s'\n", mode);
        usage();
        return 2;
    }

    // ---- options -------------------------------------------------------
    const char* files[64];
    int nfiles = 0;
    char out_user[256];
    out_user[0] = '\0';
    int fmt_elf = 0, quiet = 0, multi = 0, debug = 0;
    g_lib_mode = (strcmp(mode, "lib") == 0);
    g_quiet = g_lib_mode;

    for (int i = 2; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) {
            snprintf(out_user, sizeof(out_user), "%s", argv[++i]);
        } else if (!strcmp(a, "-format") && i + 1 < argc) {
            const char* f = argv[++i];
            if (!strcmp(f, "elf")) fmt_elf = 1;
            else if (!strcmp(f, "mrp")) fmt_elf = 0;
            else {
                fprintf(stderr, "  [ERROR]   unknown format '%s' (mrp | elf)\n", f);
                return 2;
            }
        } else if (!strcmp(a, "-multiple-files") || !strcmp(a, "-multiple")) {
            multi = 1;
        } else if (!strcmp(a, "-q")) {
            quiet = 1; g_quiet = 1;
        } else if (!strcmp(a, "--debug") || !strcmp(a, "-d")) {
            debug = 1;
        } else if (!strcmp(a, "--lib")) {
            g_lib_mode = 1; g_quiet = 1;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help") ||
                   !strcmp(a, "-help") || !strcmp(a, "-?")) {
            usage();
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "  [ERROR]   unknown option '%s'\n", a);
            usage();
            return 2;
        } else {
            add_file(files, &nfiles, 64, a);
        }
    }
    if (nfiles < 1) { usage(); return 2; }
    if (debug) g_quiet = 0;
    if (multi && nfiles < 2 && !quiet) {
        tcc_tag(TCC_YELLOW, "WARNING");
        tcc_puts("-multiple-files was given but only one source file was listed");
        tcc_nl();
    }

    // ---- compile + link: ONE symbol table + ONE fixup list for all files
    MtccOut out;
    int cr = mtcc_compile_start();
    if (cr == 0) {
        for (int i = 0; i < nfiles; i++) {
            char* src = NULL;
            uint32_t slen = 0;
            if (os_read_file(files[i], &src, &slen) != 0 || !src) {
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("cannot read ");
                tcc_puts(files[i]);
                tcc_nl();
                return 1;
            }
            if (debug) {
                tcc_tag(TCC_CYAN, "READ");
                tcc_puts(files[i]);
                tcc_puts(" (");
                tcc_putn(slen);
                tcc_puts(" bytes)");
                tcc_nl();
            }
            if (mtcc_compile_add(src, slen, files[i]) != 0) { cr = 1; break; }
        }
        if (cr == 0) cr = mtcc_compile_finish(&out);
    }
    if (cr != 0) {
        if (cr == 2) {
            tcc_tag(TCC_RED, "ERROR");
            tcc_puts("out of memory");
            tcc_nl();
        } else {
            /* [ERROR] <file>:<line>: <message> + the offending source line
             * — printed by the shared tcc_report_error() in mtcc.c. */
            tcc_report_error();
        }
        return 1;
    }

    // ---- name the product --------------------------------------------
    char oname[300];
    if (out_user[0]) out_name_from(out_user, fmt_elf, oname, sizeof(oname));
    else             default_out_name(files[0], fmt_elf, oname, sizeof(oname));

    // ---- statistics ----------------------------------------------------
    /* Order on screen: [COMPILE] (what) -> [LINK] (how big) -> program
     * output -> [RUN] (cost). `run` has no file, so its product line goes
     * right here; the file-producing modes report after the write. */
    const char* product = (strcmp(mode, "run") == 0) ? "<in-memory>" : oname;
    const char* extra = (nfiles > 1) ? files[1] : "";
    if (!quiet && !g_quiet && strcmp(mode, "run") == 0)
        tcc_report_compile(files[0], extra, product);
    if (!quiet && !g_quiet) {
        tcc_tag(TCC_CYAN, "LINK");
        tcc_putn((uint32_t)nfiles);
        tcc_puts(nfiles == 1 ? " file, " : " files, ");
        tcc_putn(out.nfuncs);
        tcc_puts(" function(s), ");
        tcc_putn(out.code_len);
        tcc_puts(" bytes code, ");
        tcc_putn(out.data_len);
        tcc_puts(" bytes data");
        tcc_nl();
    }

    // Dump code for codegen debugging (MTCC_DUMP=1)
    if (getenv("MTCC_DUMP")) {
        fprintf(stderr, "  [DUMP]   code (%u byte):", out.code_len);
        for (uint32_t i = 0; i < out.code_len; i++) {
            if ((i & 15) == 0) fprintf(stderr, "\n%04x: ", i);
            fprintf(stderr, "%02x ", out.code[i]);
        }
        fprintf(stderr, "\n");
        if (out.data_len) {
            fprintf(stderr, "  [DUMP]   data (%u byte):", out.data_len);
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

    if (strcmp(mode, "lib") == 0) {
        // ---- lib mode: check-compile + validate the image, NO run
        // (library modules have no main; the bare-ret entry stub is fine
        // to build but must not be interpreted). ----
        uint32_t need = MRP_HEADER_SIZE + out.code_len + 4 + out.data_len;
        uint8_t* image = (uint8_t*)malloc(need);
        if (!image) { tcc_tag(TCC_RED, "ERROR"); tcc_puts("out of memory\n"); return 1; }
        uint32_t total = mtcc_build_image(&out, image, need);
        if (total == 0) { tcc_tag(TCC_RED, "ERROR"); tcc_puts("image build failed\n"); return 1; }
        enum mrp_validate_reason vr;
        if (!is_valid_mrp(image, total, &vr)) {
            tcc_tag(TCC_RED, "ERROR");
            tcc_puts("invalid .mrp: ");
            tcc_puts(mrp_reason_str(vr));
            tcc_nl();
            return 1;
        }
        if (!quiet && !g_quiet) {
            tcc_tag(TCC_GREEN, "COMPILE");
            tcc_puts(files[0]);
            tcc_puts(" -> ");
            tcc_puts(oname);
            tcc_puts(" (lib check, ");
            tcc_putn(out.nfuncs);
            tcc_puts(" function(s))");
            tcc_nl();
        }
        printf("EXIT=0\n");
        return 0;
    }

    if (strcmp(mode, "run") == 0 && !fmt_elf) {
        // ---- mode A: patch with the REAL buffer addresses (MAP_32BIT)
        mtcc_patch(&out, (uint32_t)(uintptr_t)out.code,
                          (uint32_t)(uintptr_t)out.data);
        r = interp32_run(out.code, (uint64_t)(uintptr_t)out.code, out.code_len,
                         out.data, (uint64_t)(uintptr_t)out.data, out.data_len,
                         (uint64_t)(uintptr_t)out.code, &ec, &ni);
    } else {
        // ---- image modes: MRP (c / run) or ELF (c / run) ---------------
        uint32_t cap;
        if (fmt_elf) cap = ELF_PAYLOAD + out.code_len + 4 + out.data_len + 64;
        else         cap = MRP_HEADER_SIZE + out.code_len + 4 + out.data_len;
        uint8_t* image = (uint8_t*)malloc(cap);
        if (!image) { tcc_tag(TCC_RED, "ERROR"); tcc_puts("out of memory\n"); return 1; }
        uint32_t total;
        if (fmt_elf) {
            total = mtcc_build_image_elf(&out, image, cap);
            if (total == 0) { tcc_tag(TCC_RED, "ERROR"); tcc_puts("ELF build failed\n"); return 1; }
            int why = mtcc_elf_check(image, total);
            if (why != 0) {
                tcc_tag(TCC_RED, "ERROR");
                fprintf(stderr, "invalid ELF32 image (check #%d)\n", why);
                return 1;
            }
        } else {
            total = mtcc_build_image(&out, image, cap);
            if (total == 0) { tcc_tag(TCC_RED, "ERROR"); tcc_puts("image build failed\n"); return 1; }
            enum mrp_validate_reason vr;
            if (!is_valid_mrp(image, total, &vr)) {
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("invalid .mrp: ");
                tcc_puts(mrp_reason_str(vr));
                tcc_nl();
                return 1;
            }
        }

        if (strcmp(mode, "c") == 0) {
            if (os_write_file(oname, image, total) != 0) {
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("cannot write ");
                tcc_puts(oname);
                tcc_nl();
                return 1;
            }
            if (!quiet && !g_quiet) {
                tcc_report_compile(files[0], extra, product);
                tcc_tag(TCC_CYAN, "OUTPUT");
                tcc_puts(oname);
                tcc_puts(" (");
                tcc_putn(total);
                tcc_puts(" bytes)");
                tcc_nl();
            }
        }

        // ---- interpret exactly like the OS loader would ----------------
        uint32_t pad = (4 - (out.code_len & 3)) & 3;
        uint64_t code_base, data_base;
        if (fmt_elf) { code_base = ELF_BASE; data_base = ELF_BASE + out.code_len + pad; }
        else         { code_base = 0x500010; data_base = 0x500010 + out.code_len + pad; }
        const uint8_t* cptr = image + (fmt_elf ? ELF_PAYLOAD : MRP_HEADER_SIZE);
        const uint8_t* dptr = cptr + out.code_len + pad;
        r = interp32_run(cptr, code_base, out.code_len,
                         dptr, data_base, out.data_len,
                         code_base, &ec, &ni);
        free(image);
    }

    if (r != 0) return 1;
    fflush(stdout);          /* program output first, then the [RUN] tag */
    if (!quiet && !g_quiet) {
        tcc_tag(TCC_CYAN, "RUN");
        tcc_putn((uint32_t)ni);
        tcc_puts(" instruction(s) interpreted");
        tcc_nl();
    }
    printf("EXIT=%d\n", ec);
    return 0;
}
