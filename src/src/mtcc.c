// ============================================================================
//  tcc.cpp — mtcc 0.1: "Equinox OS TinyCC" — a C compiler that runs INSIDE
//             Equinox OS as a .mrp program.
// ----------------------------------------------------------------------------
//  WHAT IS THIS?
//    The first milestone of Stage 4 (TARGETS.md): "compile C INSIDE Equinox OS,
//    not cross-compile from the host". Porting the original TinyCC (Fabrice
//    Bellard, ~80k lines, needs a full libc) is a months-long project — this
//    file is its first step: a single-file tcc-style subset-C compiler
//    (single-pass, direct x86-32 codegen, no AST) that:
//
//      1. Runs as an ordinary .mrp module:
//             mtcc hello.c               -> callable from any directory
//                                          (global tool dispatch in the
//                                          shell) — compile & run immediately
//                                          in memory (like `tcc -run`).
//             mtcc -c hello.c            -> compile to file hello.mrp in
//                                          RAMFS (like `tcc -o`), which can
//                                          then be run standalone:
//                                          run hello.mrp
//             mtcc --debug hello.c       -> same as above but also prints
//                                          compiler info (file read, function
//                                          count, code/data size, exit
//                                          code). WITHOUT --debug only the
//                                          program output is printed (clean).
//
//      2. Compiled programs call the OS THROUGH DIRECT SYSCALLS
//         (int 0x80, stable numbers from kernel/library/header/syscall.h) —
//         there is NO libc in compiled programs. This follows the
//         syscall.h design: "new programs (including future tcc output)
//         are advised to target syscall numbers directly".
//
//  ARCHITECTURE (tcc style: lexer -> recursive descent parser -> direct
//  codegen, no AST, single pass):
//
//    [source .c] -> LEXER (token) -> PARSER+CODEGEN ->
//        code buffer  (x86-32 machine code, inter-function relative via
//                      rel32 + FIXUP list)
//        data buffer  (string literals + global variables, offsets from
//                      data_base)
//    -> FIXUP patch:
//        mode RUN  : patch with the real buffer addresses -> call the
//                    entry directly (ring 0, RAM executable).
//        mode -c   : patch with base 0x500010 (MRP_LOAD_BASE, same as
//                    link_mrp.ld) -> assemble [18-byte .mrp header]
//                    [code][pad][data] -> write to RAMFS via SYS_MKFILE.
//                    The kernel loader places the image exactly at
//                    0x500010, so every absolute address is correct.
//
//  CALLING CONVENTION OF COMPILED CODE (internal, only between compiled
//  code + syscall builtins — not an external ABI):
//    - Arguments are evaluated LEFT->RIGHT, each pushed onto the stack
//      (parse order = push order, no AST needed to reverse it).
//    - Callee reads param i (0-based from the left) at [ebp + 8 + 4*(n-1-i)].
//    - Return value in EAX. Are eax/ebx/ecx/edx/esi/edi/ebp callee-saved?
//      NO — only eax returns; other registers are scratch (safe because all
//      expression values live on the STACK, not in registers — the
//      accumulator + stack machine model used throughout this codegen).
//    - Caller cleans the arguments: add esp, 4*n after the call.
//
//  EMITTED INSTRUCTIONS (a CLOSED set — the host test harness
//  (scripts/tcc_host_test/interp32.cpp) interprets exactly this set;
//  any instruction outside the set = a codegen bug the tests will catch):
//    50/53/55/58/59/5A/5B/5D  push/pop GPR
//    81 EC imm32 / 83 EC imm8 / 83 C4 imm8   sub/add esp
//    89 /r  (mov r/m32 <- r32), 8B /r (mov r32 <- r/m32), 8D /r (lea)
//    B8+rd imm32 (mov reg,imm32), A1 moffs32 (mov eax,[imm32])
//    01/09/21/31/29/39 /r (add/or/and/xor/sub/cmp)
//    F7 D8 neg eax, F7 D0 not eax, F7 FB idiv ebx
//    0F AF C3 imul eax,ebx   6B C0/DB ib imul reg,imm8
//    99 cdq   D3 E0/E8/F8 shl/shr/sar eax,cl (with 89 D9 mov ecx,ebx)
//    85 C0 test eax,eax
//    E9 rel32 jmp, 0F 8x rel32 jcc, 0F 9x C0 setcc al, 0F B6 C0 movzx eax,al
//    0F B6 00/03/01 movzx eax,byte[reg]   88 03/01 mov byte[reg],al
//    FF C0/FF C8 inc/dec eax   E8 rel32 call   C3 ret   CD 80 int 0x80
//
//  SUPPORTED LANGUAGE (a deliberate subset, see TCC.md for details + why):
//    types: int, char, void, 1-2 level pointers, 1D arrays, struct/union
//           (by sdef, nested, char & array fields; ONLY passed by pointer),
//           enum (ints), typedef (name aliases incl. struct tags)
//    statements: if/else, while, do-while, for (with a declaration in init),
//               return, break, continue, blocks, expression statements,
//               switch (case const / default / fall-through / break)
//    operators: = += -= *= /= %= <<= >>= &= |= ^=, + - * / %,
//               << >> &, |, ^, ~, &&, ||, !, ==, !=, <, >, <=, >=,
//               unary - + * &, ++/-- (pre & post, elem-scaled on pointers),
//               ternary ?:, '.' and '->' field access
//    globals: scalars + arrays + constant/list/string initializers (zero-init area)
//    functions: forward prototypes, recursion, max 8 params, default values
//  NOT supported: struct/union passed or returned BY VALUE (initializers
//    ARE supported: global, local, nested, arrays of struct), float/double,
//    unsigned semantics (int is processed as signed), 2D arrays, goto,
//    variadic functions, sizeof, casts, static locals, long/short,
//    function pointers, T** (pointer-to-pointer arrays).
//  switch note: default should be the LAST label; a matching case placed
//    AFTER default in the source does not suppress the default body.
//  ++/-- and +=/-= on pointers scale by the element size (fixed version bug).
//  The MINI PREPROCESSOR exists since v10.8 (object-like #define,
//    #include <morph.h>/<multitasking.h>/<fileio.h>/..., #ifdef/#ifndef)
//    — "no preprocessor" claims in old docs are stale.
//  v0.3.2: the libc prelude is no longer embedded — <morph.h> reads
//    /equinox/libc/morph.h (REAL FILES: 12 guarded modules with
//    explicit dependency includes; host builds resolve "/equinox/..."
//    through the source tree).
//
//  RUNTIME LIMITATIONS (stated honestly — no false expectations):
//    - exec() from compiled code is ALWAYS rejected (SYS_EBUSY -9,
//      audit fix V3 #1): the caller's code lives in the same MRP arena as
//      the exec target, so the kernel refuses nested exec to keep the
//      arena from being reset over running code (previously: a guaranteed
//      panic). The calling program stays alive after exec() returns;
//      exit() remains the cleaner way to end a program.
//    - read() only reads RAMFS files (read-only). Writing a file = mkfile()
//      (create or full overwrite, binary-safe).
//    - The stack borrows the kernel stack (ring 0, no per-process stack
//      yet) — very deep recursion (>~1000 frames) can crash.
//
//  BUILD:
//    Equinox OS : python3 mrp_user/mrp_pack.py mrp_user/tcc.cpp dist/tcc.mrp
//              (or just `make pack` — the mrp_user/*.cpp wildcard)
//    Host    : g++ -std=gnu++17 -O2 -Wall -Wextra -DMTCC_HOST_TEST   (no -I kernel — see host_main.cpp; -I to the kernel is
//              wrong: imagine glibc's <stdio.h> shadowed by the kernel's stdio.h!)
//
//  The .mrp header format & checksum are NOT copied by hand here — this
//  file #includes "mrp_format.h" from the kernel (header-only, host-safe)
//  so kernel & compiler cannot drift apart. Syscall numbers also come
//  from the kernel's "syscall.h" in non-host builds (single source of truth).
// ============================================================================

#include <stdint.h>
#include <stddef.h>

// Kernel headers are included via a PATH RELATIVE TO THIS FILE (not -I):
// - .mrp build (mrp_pack.py): relative from mrp_user/ → still resolves
// - host test build: must NOT use -I kernel/library/header because it
//   would shadow glibc's <stdio.h>/<stdlib.h> with the kernel versions
//   (it happened once: fputs/stdout "not declared" even though stdio.h
//   was included!) — host_main.cpp only uses -I mrp_user.
#include "../kernel/library/header/mrp_format.h"

#ifdef MTCC_HOST_TEST
// ============================================================================
//  HOST TEST MODE — the core compiler runs on 64-bit Linux for testing
//  (lexer/parser/codegen identical; emit stays x86-32). Compiled code is
//  executed by an INTERPRETER (interp32), not the real CPU, so no
//  32-bit binary is needed on the test machine.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Syscall numbers — MUST be identical to kernel/library/header/syscall.h.
// (In the .mrp build we include the real header; on the host we cannot
// include syscall.h because its wrappers are i386 asm `int $0x80`. This
// duplication is kept in sync by the test scripts/tcc_host_test/test_sync.py
// which compares both.)
#define SYS_EXIT      1
#define SYS_EXEC      2
#define SYS_GETPID    3
#define SYS_WRITE     4
#define SYS_READ      5
#define SYS_OPEN      6
#define SYS_CLOSE     7
#define SYS_GETKEY    8
#define SYS_READLINE  9
#define SYS_PRINT     10
#define SYS_PRINTINT  11
#define SYS_MALLOC    12
#define SYS_GETTICK   13
#define SYS_SLEEP     14
#define SYS_GETARGS   15
#define SYS_MKFILE    16
#define SYS_READFILE  17
#define SYS_FILESIZE  18
#define SYS_FILEEXISTS 19
#define SYS_FBINFO    20
#define SYS_PUTPIXEL  21
#define SYS_FILLRECT  22
#define SYS_POLLKEY   23
#define SYS_MOUSE     24
#define SYS_SPEAKER   25
#define SYS_SNDBEEP   26
#define SYS_LSEEK     27
#define SYS_PRINTF    28
#define SYS_RINGINFO  29
#define SYS_KEYEVENT  32
#define SYS_MOUSEDELTA 33
#define SYS_NETINFO   34
#define SYS_NETPING   35
/* Phase B (multitasking) — keep in sync with kernel/library/header/syscall.h */
#define SYS_SPAWN     36
#define SYS_YIELD     37
#define SYS_TASKINFO  38   /* Phase C */
#define SYS_KILL      39   /* Phase C */
/* v0.3 FR-01/FR-03 — full file + memory syscalls */
#define SYS_OPEN2     40
#define SYS_UNLINK    41
#define SYS_MKDIR     42
#define SYS_RMDIR     43
#define SYS_RENAME    44
#define SYS_STAT      45
#define SYS_READDIR   46
#define SYS_FSTAT     47
#define SYS_FREE      48
/* v0.3 (FR-02/05) - process & memory model */
#define SYS_WAIT      49
#define SYS_PIPE      50
#define SYS_MEMINFO   51
#define SYS_SPAWN2    52
/* v0.3 (FR-17/18) - per-task graphics */
#define SYS_SETCLIP   53
#define SYS_DRAWLINE  54
/* extended 0.5: memory model + fork + socket syscalls */
#define SYS_SBRK      55
#define SYS_MMAP      56
#define SYS_FORK      57
#define SYS_SOCKET    58
#define SYS_NET       59

#else // ---- build .mrp (Equinox OS) ----
#include "../kernel/library/header/syscall.h"  // syscall numbers + int $0x80 wrappers
#include "mrp_api.h"                          // MRP_ENTRY (section .start)
#endif

// ============================================================================
//  Capacity constants (all buffers are allocated at runtime via os_alloc,
//  NOT static arrays — a large .bss would bloat the .mrp file because
//  objcopy writes .bss as literal zeros, see README_MRP.md).
// ============================================================================
#define MTCC_SRC_CAP      (96 * 1024)   // max .c source read
#define MTCC_CODE_CAP     (384 * 1024)  // max generated code
#define MTCC_DATA_CAP     (128 * 1024)  // strings + globals
#define MTCC_NAME_MAX     32
#define MTCC_MAX_PARAMS   8
#define MTCC_MAX_FUNCS    128
#define MTCC_MAX_LOCALS   256
#define MTCC_MAX_GVARS    128
#define MTCC_MAX_FIXUPS   2048
#define MTCC_MAX_STRINGS  512
#define MTCC_MAX_ARGS     12
#define MTCC_LOOP_DEPTH   16
#define MTCC_SCOPE_DEPTH  24
#define MTCC_FRAME_MAX    8192

// The target is always i386 — the POINTER SIZE OF COMPILED CODE is hardcoded
// to 4 bytes; do not use sizeof(void*) (a 64-bit host build must emit 32-bit).
#define MTCC_PTR_SIZE     4
#define MTCC_INT_SIZE     4
#define MTCC_CHAR_SIZE    1

// --debug flag (used by the .mrp driver + compile diagnostic trace).
// Declared up here so mtcc_compile can see it too.
static int g_debug = 0;

// -q (quiet): suppress per-file success chatter in -c mode (errors
// still print). Used by the parallel 2-thread `equinoxinstall`.
static int g_quiet = 0;

// --lib: library mode — compile WITHOUT a main() (entry stub is a
// bare `ret`). Used to check-compile the /equinox/libc modules;
// the undefined-reference audit still runs, so a module with a
// missing dependency fails loudly.
static int g_lib_mode = 0;

// ============================================================================
//  Compiler-internal types & structures
// ============================================================================
enum MtccTypeBase { TY_VOID = 0, TY_INT = 1, TY_CHAR = 2, TY_STRUCT = 3, TY_UNION = 4 };

struct CType {
    uint8_t base;      // MtccTypeBase
    uint8_t ptr;       // number of '*'
    uint8_t is_array;  // 1 = array declaration (used at declaration only;
                       //      in expressions arrays auto-decay to pointers)
    uint32_t arr_len;  // element count (is_array only)
    uint8_t sdef;      // struct/union definition index (TY_STRUCT/TY_UNION)
    uint8_t is_ref;    // 1 = this expression's eax holds the object's
                       //      ADDRESS (struct/union lvalue/ref), not its value
};

// ----------------------------------------------------------------------------
//  Struct/union definitions. Nested structs are supported by reference
//  (their CType carries a sdef index). A struct's size is the sum of its
//  fields with every field starting on a 4-byte boundary (simple & safe —
//  wastes a few bytes; correctness-first is the mtcc tradeoff).
//  union = all field offsets 0, size = 4-aligned max field size.
// ----------------------------------------------------------------------------
struct Field {
    char name[MTCC_NAME_MAX];
    CType type;         // scalar/pointer/array/nested struct (by sdef index)
    uint32_t offset;    // byte offset inside the struct
};
struct SDef {
    char name[MTCC_NAME_MAX];   // tag name ("" for anonymous)
    uint8_t is_union;
    uint8_t field_count;
    uint32_t field_off;         // index into g_field_pool
    uint32_t size;              // sizeof() in bytes, 4-aligned
};
#define MTCC_MAX_SDEF   32
#define MTCC_MAX_FIELDS 512
static struct SDef  g_sdefs[MTCC_MAX_SDEF];
static uint32_t     g_sdef_count = 0;
static struct Field g_field_pool[MTCC_MAX_FIELDS];
static uint32_t     g_field_count = 0;

static CType ctype_make(uint8_t base, uint8_t ptr) {
    CType t; t.base = base; t.ptr = ptr; t.is_array = 0; t.arr_len = 0;
    t.sdef = 0xFF; t.is_ref = 0;
    return t;
}
// Type size — two different functions because the questions differ:
// sizeof for load/store, elem_size for pointer index scaling.
//   pointer = 4 bytes (i386 always), char = 1, int/void* = 4.
static uint32_t ctype_sizeof(const CType* t) {
    if (t->ptr > 0) return MTCC_PTR_SIZE;
    if (t->base == TY_CHAR) return MTCC_CHAR_SIZE;
    if ((t->base == TY_STRUCT || t->base == TY_UNION) && t->sdef < MTCC_MAX_SDEF)
        return g_sdefs[t->sdef].size;
    return MTCC_INT_SIZE;
}
// size of ONE ELEMENT when t is used as an array/pointer (index scaling):
static uint32_t ctype_elem_size(const CType* t) {
    if (t->base == TY_CHAR && t->ptr <= 1) return MTCC_CHAR_SIZE;
    if (t->base == TY_STRUCT || t->base == TY_UNION)
        return (t->sdef < MTCC_MAX_SDEF) ? g_sdefs[t->sdef].size : 4;
    return MTCC_INT_SIZE;
}
// Variable allocation unit size (always rounded to 4 for a tidy frame;
// char[10] rounds to 12 — 2 bytes wasted, simplicity wins).
static uint32_t ctype_alloc_size(const CType* t) {
    uint32_t n = t->is_array ? (t->arr_len * ctype_elem_size(t))
                             : ctype_sizeof(t);
    return (n + 3u) & ~3u;
}

struct Func {
    char   name[MTCC_NAME_MAX];
    CType  ret;
    uint8_t nparams;
    CType  ptypes[MTCC_MAX_PARAMS];
    char   pnames[MTCC_MAX_PARAMS][MTCC_NAME_MAX];
    // v10.8 default parameter values: `int f(int a, int b = 5)` —
    // a small language extension that lets printf(fmt, a=0, b=0, c=0)
    // be called with 1..4 arguments (mtcc has no varargs).
    int32_t pdef[MTCC_MAX_PARAMS];     // default value (parse_const)
    uint8_t pdef_has[MTCC_MAX_PARAMS]; // 1 = param has a default
    int32_t code_off;        // -1 = no body yet (prototype / forward call)
    uint8_t defined;         // 1 = body emitted
    // Fixup index list of calls to this function emitted BEFORE its body
    // (forward calls). When the body starts emitting, all are filled in.
    uint16_t pending[24];
    uint8_t  pending_count;
    // v0.3 FR-20: source line of the FIRST call (for the
    // undefined-reference error message).
    uint16_t first_call_line;
};

struct LVar {
    char  name[MTCC_NAME_MAX];
    CType type;
    int32_t ebp_off;   // local: negative; parameter: positive ([ebp+8+..])
};

struct GVar {
    char  name[MTCC_NAME_MAX];
    CType type;        // is_array=1 for arrays; ptr=1 decay result is not
                       // stored here (decay happens in expressions)
    int32_t data_off;  // offset from data_base (-1 = extern, not yet defined)
    uint8_t is_extern; // 1 = `extern` declaration, no storage yet
    uint8_t accessed;  // 1 = the program actually reads/writes this gvar
    uint16_t pendfx[24]; // fixup indices that must be patched when the
                         // definition arrives (data_off was -1 at the use)
    uint8_t pend_n;
};

// Fixup: a 4-byte location in code (or in data for kind FX_ABS_IN_DATA)
// that can only be filled once the final layout / target is known.
enum { FX_REL32 = 0,     // v = target code offset; patch: v - (at+4)
       FX_ABS_DATA = 1,  // v = data offset;      patch: data_base + v (into code)
       FX_ABS_IN_DATA = 2 }; // v = data offset;  patch: data_base + v (into data!)

struct Fixup { uint32_t at; uint8_t kind; int32_t v; };

// ============================================================
//  Global compiler state (one compile per run — the .mrp arena is
//  reset every time a program runs, so single-use state is safe).
// ============================================================
static struct {
    // buffers (os_alloc)
    uint8_t* code;  uint32_t code_len;
    uint8_t* data;  uint32_t data_len;
    Func*    funcs; uint32_t func_count;
    LVar*    locals; uint32_t local_count;
    GVar*    gvars; uint32_t gvar_count;
    Fixup*   fixups; uint32_t fixup_count;
    uint32_t str_offs[MTCC_MAX_STRINGS]; uint32_t str_count;

    // lexer
    const char* src; uint32_t src_pos; uint32_t src_len; uint32_t line;
    int   tok;
    uint32_t num;            // TK_NUM / TK_CHAR
    uint32_t str_off;        // TK_STR (offset data area)
    char  ident[MTCC_NAME_MAX];

    // error — file/line of the LAST diagnostic, so the driver can print
    // `  [ERROR] path:12: message` (English, colour, one consistent style)
    int      err;
    uint32_t err_line;
    char     err_file[64];
    char     err_msg[96];

    // function currently being compiled
    Func*    cur_func;
    int32_t  frame_off;      // more negative = deeper
    uint32_t frame_sub_at;   // location of the 4-byte `sub esp, imm32` operand

    // loop nesting (break/continue fixup pending)
    struct {
        uint16_t brk[24]; uint8_t nbrk;
        uint16_t cont[24]; uint8_t ncont;
        // switch support: an entry with is_switch=1 joins the break stack
        // (break -> after switch) but rejects continue. 'ord' counts case
        // labels seen (0 = none yet); guard0/guard1 + guard_has hold the
        // pending body-skip fixups of the CURRENT case/default — patched
        // to the body end when the next label (or '}') of the same level
        // is reached.
        int is_switch; uint8_t ord;
        uint16_t guard0, guard1; uint8_t guard_has;
    } loops[MTCC_LOOP_DEPTH];
    int loop_depth;

    // block scope marks (local_count index when the scope was opened)
    uint32_t scope_marks[MTCC_SCOPE_DEPTH];
    int scope_depth;
} S;

// typedef table: name -> CType (struct-carrying types by sdef index)
static struct { char name[MTCC_NAME_MAX]; CType type; } g_typedefs[64];
static uint32_t g_typedef_count = 0;
// enum constant table: enumerator name -> value
static struct { char name[MTCC_NAME_MAX]; int32_t val; } g_consts[128];
static uint32_t g_const_count = 0;

static void mtcc_error(const char* msg) {
    if (S.err) return;                 // the first error wins
    S.err = 1;
    S.err_line = S.line;
    uint32_t i = 0;
    while (msg && msg[i] && i < sizeof(S.err_msg) - 1) {
        S.err_msg[i] = msg[i]; i++;
    }
    S.err_msg[i] = '\0';
}

// Debug instrumentation (in-OS bug hunting): error + identifier name.
// Used by gen_primary/gen_lvalue to show WHICH identifier failed to
// resolve — distinguishing a dirty table vs broken lookup vs a junk name.
static void mtcc_error_ident(const char* pre, const char* name) {
    char msg[96];
    uint32_t i = 0;
    while (pre[i] && i < sizeof(msg) - 1) { msg[i] = pre[i]; i++; }
    uint32_t j = 0;
    while (name[j] && i < sizeof(msg) - 1) { msg[i] = name[j]; i++; j++; }
    msg[i] = '\0';
    mtcc_error(msg);
}

// ============================================================================
//  Hand-rolled string/memory helpers — an .mrp program has no libc, so
//  ALL string comparisons in this compiler go through its own helpers
//  (the host build uses the same path so behavior is identical).
// ============================================================================
static void m_memcpy(uint8_t* dst, const uint8_t* src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}
static void m_memset(uint8_t* dst, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = v;
}
static int m_streq(const char* a, const char* b) {
    uint32_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i] && a[i] == '\0';
}
static int m_memcmp(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if ((unsigned char)a[i] != (unsigned char)b[i]) return 0;
    }
    return 1;
}

static struct SDef* sdef_find(const char* tag) {
    for (uint32_t i = 0; i < g_sdef_count; i++)
        if (m_streq(g_sdefs[i].name, tag)) return &g_sdefs[i];
    return NULL;
}
static const struct Field* sdef_find_field(const struct SDef* sd, const char* name) {
    for (uint32_t i = 0; i < sd->field_count; i++)
        if (m_streq(g_field_pool[sd->field_off + i].name, name))
            return &g_field_pool[sd->field_off + i];
    return NULL;
}
static struct SDef* sdef_at(uint8_t idx) {
    return (idx < g_sdef_count) ? &g_sdefs[idx] : NULL;
}

// ============================================================================
//  Platform glue — OS services for THIS COMPILER ITSELF.
//  (Compiled code uses inline int 0x80 emitted by the codegen —
//   a different path, see the builtin section.)
// ============================================================================
#ifdef MTCC_HOST_TEST

static void* os_alloc(uint32_t size) {
    // MAP_32BIT: addresses < 2GB so they fit in uint32 — RUN mode
    // (patching real buffer addresses into code) stays valid on a 64-bit host.
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return NULL;
    // REGRESSION NET (arena reuse bug in-OS, 2nd+ run): host mmap ALWAYS
    // returns zeroed pages, so "depends on clean memory" bugs never show
    // on the host — only in the OS when the .mrp arena is reused with stale
    // contents. Fill a dirty 0xA5 pattern here so the same class of bug
    // fails immediately in the host test too.
    uint8_t* q = (uint8_t*)p;
    for (uint32_t i = 0; i < size; i++) q[i] = (uint8_t)0xA5;
    return p;
}
static void os_print(const char* s) { fputs(s, stdout); }
static void os_printint(uint32_t n) { printf("%u", n); }

static int os_read_file(const char* path, char** out_buf, uint32_t* out_len) {
    // Absolute "/equinox/..." paths are RAMFS paths in the OS. On the
    // HOST they map onto the source tree (the repo root holds libc/):
    // try "<repo-relative>", then "../../<repo-relative>" (cwd =
    // scripts/tcc_host_test), then the raw path. First hit wins.
    const char* candidates[3];
    int ncand = 1;
    candidates[0] = path;
    if (path[0] == '/' && path[1] == 'e' && path[2] == 'q' && path[3] == 'u' &&
        path[4] == 'i' && path[5] == 'n' && path[6] == 'o' && path[7] == 'x' &&
        path[8] == '/') {
        static char rel[2][128];
        static const char updir[7] = "../../";
        uint32_t k = 0;
        while (path[9 + k] && k < 120) { rel[0][k] = path[9 + k]; k++; }
        rel[0][k] = '\0';
        for (uint32_t j = 0; j < 6; j++) rel[1][j] = updir[j];
        for (uint32_t j = 0; j < k; j++) rel[1][6 + j] = rel[0][j];
        rel[1][6 + k] = '\0';
        candidates[0] = rel[0];
        candidates[1] = rel[1];
        candidates[2] = path;
        ncand = 3;
    }
    FILE* f = NULL;
    for (int c = 0; c < ncand && !f; c++) f = fopen(candidates[c], "rb");
    if (!f) return -3;                       // SYS_ENOENT
    char* buf = (char*)os_alloc(MTCC_SRC_CAP);
    if (!buf) { fclose(f); return -5; }
    size_t n = fread(buf, 1, MTCC_SRC_CAP, f);
    fclose(f);
    *out_buf = buf; *out_len = (uint32_t)n;
    return 0;
}
static int os_write_file(const char* path, const uint8_t* buf, uint32_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -3;
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -6;
}

// ---- ekstra glue untuk -make (host: POSIX dirent + munmap) ----
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
static void os_free(void* p, uint32_t size) {
    if (p) munmap(p, size);
}
// handle dir host = struct kecil {DIR*, path} — path dipakai stat()
// fallback bila d_type == DT_UNKNOWN (mis. overlayfs)
struct OsDirHost { DIR* d; char path[112]; };
static void* os_dir_open(const char* path) {
    DIR* d = opendir(path);
    if (!d) return NULL;
    struct OsDirHost* od = (struct OsDirHost*)malloc(sizeof(struct OsDirHost));
    if (!od) { closedir(d); return NULL; }
    od->d = d;
    snprintf(od->path, sizeof(od->path), "%s", path);
    return od;
}
static const char* os_dir_next(void* v, int* is_dir) {
    struct OsDirHost* od = (struct OsDirHost*)v;
    struct dirent* e = readdir(od->d);
    if (!e) return NULL;
    int isd = 0;
    if (e->d_type == DT_DIR) isd = 1;
    else if (e->d_type == DT_UNKNOWN) {
        char fp[256];
        snprintf(fp, sizeof(fp), "%s/%s", od->path, e->d_name);
        struct stat st;
        if (stat(fp, &st) == 0 && S_ISDIR(st.st_mode)) isd = 1;
    }
    *is_dir = isd;
    return e->d_name;
}
static void os_dir_close(void* v) {
    struct OsDirHost* od = (struct OsDirHost*)v;
    closedir(od->d);
    free(od);
}
static int os_mkdir(const char* path) { return mkdir(path, 0777); }
static int os_unlink(const char* path) { return unlink(path); }
static int os_rename(const char* a, const char* b) { return rename(a, b); }

// v0.6 -make: baca file ke SATU buffer persisten. Sumber job bersifat
// transien (mati setelah pp_run menyalinnya) — aman dipakai ulang;
// prelude punya cache terpisah (prelude_buf) sehingga tidak teralias.
static int os_read_file_cached(const char* path, char** out_buf, uint32_t* out_len) {
    static char* s_buf = NULL;
    if (!s_buf) s_buf = (char*)os_alloc(MTCC_SRC_CAP);
    if (!s_buf) return -5;
    const char* candidates[3];
    int ncand = 1;
    candidates[0] = path;
    if (path[0]=='/' && path[1]=='e' && path[2]=='q' && path[3]=='u' &&
        path[4]=='i' && path[5]=='n' && path[6]=='o' && path[7]=='x' &&
        path[8]=='/') {
        static char rel[2][128];
        static const char updir[7] = "../../";
        uint32_t k = 0;
        while (path[9+k] && k < 120) { rel[0][k] = path[9+k]; k++; }
        rel[0][k] = '\0';
        for (uint32_t j = 0; j < 6; j++) rel[1][j] = updir[j];
        for (uint32_t j = 0; j < k; j++) rel[1][6+j] = rel[0][j];
        rel[1][6+k] = '\0';
        candidates[0] = rel[0]; candidates[1] = rel[1]; candidates[2] = path;
        ncand = 3;
    }
    FILE* f = NULL;
    for (int c = 0; c < ncand && !f; c++) f = fopen(candidates[c], "rb");
    if (!f) return -3;
    size_t n = fread(s_buf, 1, MTCC_SRC_CAP, f);
    fclose(f);
    *out_buf = s_buf; *out_len = (uint32_t)n;
    return 0;
}

#else // ---- Equinox OS (.mrp) ----

static void* os_alloc(uint32_t size) {
    // SYS_MALLOC -> mrp_alloc from the MRP arena (0x500000-0x900000, 4MB).
    // Note: this arena also holds the tcc.mrp image itself at the start —
    // tcc's total need ~1MB is far below 4MB.
    int ret = syscall1(SYS_MALLOC, size);
    return (ret <= 0) ? NULL : (void*)(uintptr_t)ret;
}
static void os_print(const char* s) { syscall1(SYS_PRINT, (uint32_t)(uintptr_t)s); }
static void os_printint(uint32_t n) { syscall1(SYS_PRINTINT, n); }

static int os_read_file(const char* path, char** out_buf, uint32_t* out_len) {
    int fd = syscall1(SYS_OPEN, (uint32_t)(uintptr_t)path);
    if (fd < 0) return fd;                   // negative errno
    char* buf = (char*)os_alloc(MTCC_SRC_CAP);
    if (!buf) { syscall1(SYS_CLOSE, (uint32_t)fd); return -5; }
    uint32_t total = 0;
    while (total + 1024 <= MTCC_SRC_CAP) {
        int n = syscall3(SYS_READ, (uint32_t)fd,
                         (uint32_t)(uintptr_t)(buf + total), 1024);
        if (n <= 0) break;                   // 0 = EOF, negative = error
        total += (uint32_t)n;
    }
    syscall1(SYS_CLOSE, (uint32_t)fd);
    if (total >= MTCC_SRC_CAP) {
        os_print("  [ERROR]   source exceeds 96 KB (MTCC_SRC_CAP)\n");
        return -6;
    }
    *out_buf = buf; *out_len = total;
    return 0;
}
static int os_write_file(const char* path, const uint8_t* buf, uint32_t len) {
    // SYS_MKFILE (syscall #16): create-or-overwrite RAMFS file, binary-safe
    // (fs_write_binary in the kernel, not strcpy).
    return syscall3(SYS_MKFILE, (uint32_t)(uintptr_t)path,
                    (uint32_t)(uintptr_t)buf, len);
}

// ---- ekstra glue untuk -make (OS: SYS_FREE + syscall direktori) ----
#define MAKE_O_DIR 4096   // sama dengan F_DIR di <fileio.h> (prelude libc)
static void os_free(void* p, uint32_t size) {
    (void)size;
    if (p) syscall1(SYS_FREE, (uint32_t)(uintptr_t)p);   // FR-03
}
static void* os_dir_open(const char* path) {
    int fd = syscall2(SYS_OPEN2, (uint32_t)(uintptr_t)path, MAKE_O_DIR);
    if (fd < 0) return NULL;
    return (void*)(uintptr_t)fd;
}
// dirent layout (lihat tools_user/ls.c): name[64], is_dir @64 (u16),
// size @68 (u32). Hasil sah sampai panggilan os_dir_next BERIKUTNYA —
// penelepon wajib menyalin nama sebelum rekursi.
static const char* os_dir_next(void* d, int* is_dir) {
    static char de[72];
    int r = syscall2(SYS_READDIR, (uint32_t)(uintptr_t)d, (uint32_t)(uintptr_t)de);
    if (r != 1) return NULL;
    *is_dir = (de[64] & 255) | ((de[65] & 255) << 8);
    return de;
}
static void os_dir_close(void* d) {
    syscall1(SYS_CLOSE, (uint32_t)(uintptr_t)d);
}
// v0.6 -make: baca file ke SATU buffer persisten (lihat versi host)
static int os_read_file_cached(const char* path, char** out_buf, uint32_t* out_len) {
    static char* s_buf = NULL;
    if (!s_buf) s_buf = (char*)os_alloc(MTCC_SRC_CAP);
    if (!s_buf) return -5;
    int fd = syscall1(SYS_OPEN, (uint32_t)(uintptr_t)path);
    if (fd < 0) return fd;
    uint32_t total = 0;
    while (total + 1024 <= MTCC_SRC_CAP) {
        int n = syscall3(SYS_READ, (uint32_t)fd,
                         (uint32_t)(uintptr_t)(s_buf + total), 1024);
        if (n <= 0) break;
        total += (uint32_t)n;
    }
    syscall1(SYS_CLOSE, (uint32_t)fd);
    if (total >= MTCC_SRC_CAP) return -6;
    *out_buf = s_buf; *out_len = total;
    return 0;
}
static int os_mkdir(const char* path) {
    return syscall1(SYS_MKDIR, (uint32_t)(uintptr_t)path);
}
static int os_unlink(const char* path) {
    return syscall1(SYS_UNLINK, (uint32_t)(uintptr_t)path);
}
static int os_rename(const char* a, const char* b) {
    return syscall2(SYS_RENAME, (uint32_t)(uintptr_t)a, (uint32_t)(uintptr_t)b);
}
#endif // platform glue

// ============================================================================
//  0.5 — .config/mtcc.ecf : konfigurasi default mtcc (T3)
// ----------------------------------------------------------------------------
//  Dibaca SETIAP invocation (mtcc = proses segar tiap spawn — tanpa cache
//  boot, jadi edit langsung hidup). CLI SELALU menang atas config:
//    format.default   mrp|elf    default format keluaran (`-format` menang)
//    flags.default    "<flags>"  default flag: -q, -d/--debug, --lib
//                                (CLI menambah; untuk membatalkan, edit config)
//    set.store        <path>     store tujuan `set` pada .ruf
//                                ("" = MAKE_ECF_DEFAULT)
//    spawn.name       <nama>     tool yang di-spawn shell utk build
//                                ("" / absen = mtcc.mrp)
//    spawn.args       "<args>"   argumen ekstra saat spawn (mis. "-q")
//    buildir.default  <dir>      default $buildir bila resep tanpa `out`
//    rufdir.default   <dir>      default $rufdir bila resep di root
//
//  Format = INI-lite dotted key (sama dengan parser ecf.c: key ber-titik
//  dipakai apa adanya), supaya `config get mtcc <key>` di shell membaca
//  berkas yang sama.
// ============================================================================
#ifdef MTCC_HOST_TEST
#define MTCC_ECF_CFG "./.config/mtcc.ecf"
#else
#define MTCC_ECF_CFG_MNT "/mnt/equinox/.config/mtcc.ecf"
#define MTCC_ECF_CFG     "/equinox/.config/mtcc.ecf"
#endif

static char*  g_cfg_buf  = NULL;
static uint32_t g_cfg_len = 0;
static int    g_cfg_tried = 0;

static void mtcc_cfg_load(void) {
    if (g_cfg_tried) return;
    g_cfg_tried = 1;
#ifdef MTCC_HOST_TEST
    const char* paths[1]; int np = 1;
    paths[0] = MTCC_ECF_CFG;
#else
    const char* paths[2]; int np = 2;   /* volume /mnt dulu (persisten) */
    paths[0] = MTCC_ECF_CFG_MNT;
    paths[1] = MTCC_ECF_CFG;
#endif
    for (int i = 0; i < np; i++) {
        char* buf = NULL; uint32_t len = 0;
        if (os_read_file(paths[i], &buf, &len) == 0 && buf) {
            g_cfg_buf = buf; g_cfg_len = len;
            return;                       /* hidup sampai proses keluar */
        }
        if (buf) os_free(buf, MTCC_SRC_CAP);
    }
}

static int mtcc_cfg_ci(const char* a, const char* b) {  /* case-insensitive */
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

/* Cari key (dotted, ci) -> out. return 1 = ketemu & tidak kosong. */
static int mtcc_cfg_get(const char* key, char* out, uint32_t cap) {
    out[0] = '\0';
    mtcc_cfg_load();
    if (!g_cfg_buf || !g_cfg_len) return 0;

    char section[48]; section[0] = '\0';
    uint32_t pos = 0;
    while (pos < g_cfg_len) {
        uint32_t s = pos, e = pos;
        while (e < g_cfg_len && g_cfg_buf[e] != '\n') e++;
        pos = e + 1;
        uint32_t b = s;
        while (b < e && (g_cfg_buf[b] == ' ' || g_cfg_buf[b] == '\t')) b++;
        if (b >= e || g_cfg_buf[b] == '#' || g_cfg_buf[b] == ';') continue;
        if (g_cfg_buf[b] == '[') {                 /* [section] -> prefix */
            uint32_t k = b + 1, ke = b + 1;
            while (ke < e && g_cfg_buf[ke] != ']') ke++;
            uint32_t n = ke - k;
            if (n >= sizeof(section)) n = sizeof(section) - 1;
            for (uint32_t i = 0; i < n; i++) section[i] = g_cfg_buf[k + i];
            section[n] = '\0';
            continue;
        }
        uint32_t eq = b;
        while (eq < e && g_cfg_buf[eq] != '=') eq++;
        if (eq >= e) continue;                     /* bukan baris key */
        uint32_t le = eq;
        while (le > b && (g_cfg_buf[le-1] == ' ' || g_cfg_buf[le-1] == '\t' ||
                          g_cfg_buf[le-1] == '\r')) le--;
        if (le <= b) continue;
        int dotted = 0;
        for (uint32_t i = b; i < le; i++) if (g_cfg_buf[i] == '.') dotted = 1;
        char eff[80]; uint32_t n = 0;
        if (!dotted && section[0]) {               /* section.lhs */
            for (uint32_t i = 0; section[i] && n < sizeof(eff) - 1; i++)
                eff[n++] = section[i];
            if (n < sizeof(eff) - 1) eff[n++] = '.';
        }
        for (uint32_t i = b; i < le && n < sizeof(eff) - 1; i++)
            eff[n++] = g_cfg_buf[i];
        eff[n] = '\0';
        if (!mtcc_cfg_ci(eff, key)) continue;
        uint32_t v = eq + 1;
        while (v < e && (g_cfg_buf[v] == ' ' || g_cfg_buf[v] == '\t')) v++;
        uint32_t ve = e;
        while (ve > v && (g_cfg_buf[ve-1] == ' ' || g_cfg_buf[ve-1] == '\t' ||
                          g_cfg_buf[ve-1] == '\r')) ve--;
        uint32_t vn = ve - v;
        if (vn >= cap) vn = cap - 1;
        for (uint32_t i = 0; i < vn; i++) out[i] = g_cfg_buf[v + i];
        out[vn] = '\0';
        return (out[0] != '\0');
    }
    return 0;
}

// ============================================================================
//  CLI OUTPUT LAYER — English, coloured, one consistent style everywhere.
// ----------------------------------------------------------------------------
//  In-OS the console parses ANSI SGR natively (kernel/library/stdio.cpp
//  ansi_feed() + ansi_sgr_apply()), so "\033[32m" really renders GREEN on
//  screen. On the HOST the escapes go to STDERR and are only enabled for a
//  TTY, so the byte-compared test harness output (stdout) never changes.
//
//    [COMPILE] hello.c -> hello.mrp        green   a build product
//    [COMPILE] a.c + b.c -> prog.mrp       green   multi-file link
//    [LINK]    2 file(s), 116 func(s) …     cyan    program statistics
//    [MAKE]    ok: job -> out.mrp          green   .ruf recipe step
//    [ERROR]   file.c:12: message          red     diagnostics
//    [WARNING] message                     yellow
// ============================================================================
static int g_color = 0;            /* 1 = emit ANSI escapes (driver sets it) */

#define TCC_RESET   "\033[0m"
#define TCC_BOLD    "\033[1m"
#define TCC_RED     "\033[31m"
#define TCC_GREEN   "\033[32m"
#define TCC_YELLOW  "\033[33m"
#define TCC_BLUE    "\033[34m"
#define TCC_CYAN    "\033[36m"
#define TCC_WHITE   "\033[37m"

#ifdef MTCC_HOST_TEST
static void tcc_puts(const char* s) { fputs(s, stderr); }
static void tcc_putsn(const char* s, uint32_t n) { fwrite(s, 1, n, stderr); }
static void tcc_putn(uint32_t n)    { fprintf(stderr, "%u", n); }
#else
static void tcc_puts(const char* s) { os_print(s); }
static void tcc_putsn(const char* s, uint32_t n) {  /* bounded, no NUL needed */
    char b[128];
    if (n > sizeof(b) - 1) n = sizeof(b) - 1;
    for (uint32_t i = 0; i < n; i++) b[i] = s[i];
    b[n] = '\0';
    os_print(b);
}
static void tcc_putn(uint32_t n)    { os_printint(n); }
#endif

/* "  [TAG] " — the tag coloured+bold, everything after it plain. */
static void tcc_tag(const char* color, const char* tag) {
    tcc_puts("  ");
    if (g_color) { tcc_puts(color); tcc_puts(TCC_BOLD); }
    tcc_puts("[");
    tcc_puts(tag);
    tcc_puts("]");
    if (g_color) tcc_puts(TCC_RESET);
    tcc_puts(" ");
}
static void tcc_nl(void) { tcc_puts("\n"); }

/* [COMPILE] a.c (+ b.c) -> out.<fmt> — the one line every build reports. */
static void tcc_report_compile(const char* first_file, const char* more_files,
                               const char* out_name) {
    tcc_tag(TCC_GREEN, "COMPILE");
    tcc_puts(first_file);
    if (more_files && more_files[0]) { tcc_puts(" + "); tcc_puts(more_files); }
    tcc_puts(" -> ");
    tcc_puts(out_name);
    tcc_nl();
}

/* [ERROR] path:12: message — plus the offending source line underneath.
 * ONE printer for both drivers (host + in-OS), so diagnostics look and
 * read identically everywhere. */
static void tcc_report_error(void) {
    tcc_tag(TCC_RED, "ERROR");
    if (S.err_file[0]) { tcc_puts(S.err_file); tcc_puts(":"); }
    tcc_putn(S.err_line);
    tcc_puts(": ");
    tcc_puts(S.err_msg);
    tcc_nl();
    if (S.src && S.src_len) {
        uint32_t cur = 1, i = 0, start = 0;
        while (i < S.src_len && cur < S.err_line) {
            if (S.src[i] == '\n') { cur++; start = i + 1; }
            i++;
        }
        uint32_t j = start;
        while (j < S.src_len && S.src[j] != '\n') j++;
        tcc_puts("             ");      /* under the tag, aligned with text */
        tcc_putsn(S.src + start, j - start);
        tcc_nl();
    }
}

// ============================================================================
//  LEXER
// ----------------------------------------------------------------------------
//  Token kinds:
//    TK_EOF/TK_NUM/TK_STR/TK_IDENT/keyword/multi-char-punct >= 256,
//    single-char punctuator = its own ASCII code (< 256).
//  String literals are embedded into the data area during lexing (with
//  dedup) — the token only carries an offset. Dedup makes re-lexing (the
//  assignment rollback trick, see gen_assign) idempotent: no double strings.
// ============================================================================
enum {
    TK_EOF = 0, TK_NUM, TK_STR, TK_IDENT,
    TK_KW_INT = 256, TK_KW_CHAR, TK_KW_VOID, TK_KW_IF, TK_KW_ELSE,
    TK_KW_WHILE, TK_KW_FOR, TK_KW_DO, TK_KW_RETURN, TK_KW_BREAK,
    TK_KW_CONTINUE, TK_KW_STRUCT, TK_KW_UNION, TK_KW_ENUM, TK_KW_TYPEDEF,
    TK_KW_SWITCH, TK_KW_CASE, TK_KW_DEFAULT, TK_KW_EXTERN,
    TK_LE, TK_GE, TK_EQ, TK_NE, TK_AND, TK_OR, TK_SHL, TK_SHR,
    TK_ADDEQ, TK_SUBEQ, TK_MULEQ, TK_DIVEQ, TK_MODEQ,
    TK_ANDEQ, TK_OREQ, TK_XOREQ, TK_SHLEQ, TK_SHREQ,
    TK_INC, TK_DEC, TK_ARROW
};

static uint32_t data_align4(void) {
    uint32_t pad = (4 - (S.data_len & 3)) & 3;
    S.data_len += pad;
    return pad;
}

// Add a string literal (already decoded, len chars without NUL) to the
// data area with dedup. Returns the offset.
static uint32_t data_add_string(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < S.str_count; i++) {
        uint32_t off = S.str_offs[i];
        if (off + n + 1 > S.data_len) continue;
        const char* ex = (const char*)(S.data + off);
        uint32_t j = 0;
        while (j < n && ex[j] == s[j]) j++;
        if (j == n && ex[n] == '\0') return off;   // exact match
    }
    if (S.str_count >= MTCC_MAX_STRINGS ||
        S.data_len + n + 1 + 4 > MTCC_DATA_CAP) {
        mtcc_error("too many string literals / data full");
        return 0;
    }
    data_align4();
    uint32_t off = S.data_len;
    for (uint32_t j = 0; j < n; j++) S.data[S.data_len + j] = (uint8_t)s[j];
    S.data[S.data_len + n] = 0;
    S.data_len += n + 1;
    S.str_offs[S.str_count++] = off;
    return off;
}

// Escape value after '\' — returns -1 if it is not a recognized escape.
static int lex_escape(void) {
    if (S.src_pos >= S.src_len) return -1;
    char c = S.src[S.src_pos++];
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        default: return -1;
    }
}

static void lex_next(void) {
    if (S.err) { S.tok = TK_EOF; return; }

    // ---- whitespace + comments ----
    for (;;) {
        while (S.src_pos < S.src_len) {
            char c = S.src[S.src_pos];
            if (c == '\n') { S.line++; S.src_pos++; }
            else if (c == ' ' || c == '\t' || c == '\r') S.src_pos++;
            else break;
        }
        if (S.src_pos + 1 < S.src_len && S.src[S.src_pos] == '/' &&
            S.src[S.src_pos + 1] == '/') {
            while (S.src_pos < S.src_len && S.src[S.src_pos] != '\n')
                S.src_pos++;
            continue;
        }
        if (S.src_pos + 1 < S.src_len && S.src[S.src_pos] == '/' &&
            S.src[S.src_pos + 1] == '*') {
            S.src_pos += 2;
            int closed = 0;
            while (S.src_pos < S.src_len) {
                if (S.src[S.src_pos] == '\n') S.line++;
                if (S.src[S.src_pos] == '*' &&
                    S.src_pos + 1 < S.src_len &&
                    S.src[S.src_pos + 1] == '/') {
                    S.src_pos += 2; closed = 1; break;
                }
                S.src_pos++;
            }
            if (!closed) { mtcc_error("unterminated /* comment"); return; }
            continue;
        }
        break;
    }

    if (S.src_pos >= S.src_len) { S.tok = TK_EOF; return; }

    char c = S.src[S.src_pos];

    // ---- identifier / keyword ----
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        uint32_t n = 0;
        while (S.src_pos < S.src_len) {
            char d = S.src[S.src_pos];
            if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                (d >= '0' && d <= '9') || d == '_') {
                if (n < MTCC_NAME_MAX - 1) S.ident[n++] = d;
                else if (n == MTCC_NAME_MAX - 1) {
                    mtcc_error("identifier too long (max 31)");
                    return;
                }
                S.src_pos++;
            } else break;
        }
        S.ident[n] = '\0';
        // keyword table
        if      (m_streq(S.ident, "int"))      S.tok = TK_KW_INT;
        else if (m_streq(S.ident, "char"))     S.tok = TK_KW_CHAR;
        else if (m_streq(S.ident, "void"))     S.tok = TK_KW_VOID;
        else if (m_streq(S.ident, "if"))       S.tok = TK_KW_IF;
        else if (m_streq(S.ident, "else"))     S.tok = TK_KW_ELSE;
        else if (m_streq(S.ident, "while"))    S.tok = TK_KW_WHILE;
        else if (m_streq(S.ident, "for"))      S.tok = TK_KW_FOR;
        else if (m_streq(S.ident, "do"))       S.tok = TK_KW_DO;
        else if (m_streq(S.ident, "return"))   S.tok = TK_KW_RETURN;
        else if (m_streq(S.ident, "break"))    S.tok = TK_KW_BREAK;
        else if (m_streq(S.ident, "continue")) S.tok = TK_KW_CONTINUE;
        else if (m_streq(S.ident, "struct"))   S.tok = TK_KW_STRUCT;
        else if (m_streq(S.ident, "union"))    S.tok = TK_KW_UNION;
        else if (m_streq(S.ident, "enum"))     S.tok = TK_KW_ENUM;
        else if (m_streq(S.ident, "typedef"))  S.tok = TK_KW_TYPEDEF;
        else if (m_streq(S.ident, "switch"))   S.tok = TK_KW_SWITCH;
        else if (m_streq(S.ident, "case"))     S.tok = TK_KW_CASE;
        else if (m_streq(S.ident, "default"))  S.tok = TK_KW_DEFAULT;
        else if (m_streq(S.ident, "extern"))   S.tok = TK_KW_EXTERN;
        else S.tok = TK_IDENT;
        return;
    }

    // ---- number (hex 0x / decimal) ----
    if (c >= '0' && c <= '9') {
        uint32_t v = 0;
        if (c == '0' && S.src_pos + 1 < S.src_len &&
            (S.src[S.src_pos + 1] == 'x' || S.src[S.src_pos + 1] == 'X')) {
            S.src_pos += 2;
            while (S.src_pos < S.src_len) {
                char d = S.src[S.src_pos];
                int dv;
                if (d >= '0' && d <= '9') dv = d - '0';
                else if (d >= 'a' && d <= 'f') dv = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F') dv = d - 'A' + 10;
                else break;
                v = (v << 4) | (uint32_t)dv;
                S.src_pos++;
            }
        } else {
            while (S.src_pos < S.src_len &&
                   S.src[S.src_pos] >= '0' && S.src[S.src_pos] <= '9') {
                v = v * 10u + (uint32_t)(S.src[S.src_pos] - '0');
                S.src_pos++;
            }
        }
        S.num = v;
        S.tok = TK_NUM;
        return;
    }

    // ---- character literal ----
    if (c == '\'') {
        S.src_pos++;
        int v = -1;
        if (S.src_pos < S.src_len) {
            if (S.src[S.src_pos] == '\\') { S.src_pos++; v = lex_escape(); }
            else if (S.src[S.src_pos] != '\'') {
                v = (unsigned char)S.src[S.src_pos++];
            }
        }
        if (v < 0) { mtcc_error("empty/invalid character literal"); return; }
        if (S.src_pos >= S.src_len || S.src[S.src_pos] != '\'') {
            mtcc_error("unterminated character literal '\"'"); return;
        }
        S.src_pos++;
        S.num = (uint32_t)v;
        S.tok = TK_NUM;   // char literal = a number (after escape)
        return;
    }

    // ---- string literal -> data area (dedup) ----
    if (c == '"') {
        S.src_pos++;
        char buf[256];
        uint32_t n = 0;
        while (S.src_pos < S.src_len && S.src[S.src_pos] != '"') {
            int ch;
            if (S.src[S.src_pos] == '\\') {
                S.src_pos++;
                ch = lex_escape();
                if (ch < 0) { mtcc_error("unknown string escape"); return; }
            } else {
                ch = (unsigned char)S.src[S.src_pos++];
            }
            if (n >= sizeof(buf) - 1) {
                mtcc_error("string literal too long (max 255)");
                return;
            }
            buf[n++] = (char)ch;
        }
        if (S.src_pos >= S.src_len) {
            mtcc_error("unterminated string literal '\"'");
            return;
        }
        S.src_pos++;   // '"'
        S.str_off = data_add_string(buf, n);
        S.tok = TK_STR;
        return;
    }

    // ---- punctuator: longest match first ----
    {
        const char* p = S.src + S.src_pos;
        uint32_t left = S.src_len - S.src_pos;
        #define MT(s) (left >= sizeof(s)-1 && m_memcmp(p, s, sizeof(s)-1))
        if (MT("<<=")) { S.src_pos += 3; S.tok = TK_SHLEQ; return; }
        if (MT(">>=")) { S.src_pos += 3; S.tok = TK_SHREQ; return; }
        if (MT("=="))  { S.src_pos += 2; S.tok = TK_EQ;  return; }
        if (MT("!="))  { S.src_pos += 2; S.tok = TK_NE;  return; }
        if (MT("<="))  { S.src_pos += 2; S.tok = TK_LE;  return; }
        if (MT(">="))  { S.src_pos += 2; S.tok = TK_GE;  return; }
        if (MT("&&"))  { S.src_pos += 2; S.tok = TK_AND; return; }
        if (MT("||"))  { S.src_pos += 2; S.tok = TK_OR;  return; }
        if (MT("<<"))  { S.src_pos += 2; S.tok = TK_SHL; return; }
        if (MT(">>"))  { S.src_pos += 2; S.tok = TK_SHR; return; }
        if (MT("++"))  { S.src_pos += 2; S.tok = TK_INC; return; }
        if (MT("--"))  { S.src_pos += 2; S.tok = TK_DEC; return; }
        if (MT("->"))  { S.src_pos += 2; S.tok = TK_ARROW; return; }
        if (MT("+="))  { S.src_pos += 2; S.tok = TK_ADDEQ; return; }
        if (MT("-="))  { S.src_pos += 2; S.tok = TK_SUBEQ; return; }
        if (MT("*="))  { S.src_pos += 2; S.tok = TK_MULEQ; return; }
        if (MT("/="))  { S.src_pos += 2; S.tok = TK_DIVEQ; return; }
        if (MT("%="))  { S.src_pos += 2; S.tok = TK_MODEQ; return; }
        if (MT("&="))  { S.src_pos += 2; S.tok = TK_ANDEQ; return; }
        if (MT("|="))  { S.src_pos += 2; S.tok = TK_OREQ; return; }
        if (MT("^="))  { S.src_pos += 2; S.tok = TK_XOREQ; return; }
        #undef MT
    }

    // any other single char ('+' '-' '(' ';' etc.) — its ASCII code is the token.
    if ((unsigned char)c >= 33 && (unsigned char)c <= 126) {
        S.tok = (int)(unsigned char)c;
        S.src_pos++;
        return;
    }
    mtcc_error("stray character in source");
}

// [PART-1-END]
// ============================================================================
//  EMIT — byte writer into the code buffer. Every instruction emitted is
//  in the "closed set" list in the header comment above; the host test
//  harness interprets exactly that set.
// ============================================================================
static void emit8(uint8_t b) {
    if (S.code_len + 1 > MTCC_CODE_CAP) { mtcc_error("code buffer full"); return; }
    S.code[S.code_len++] = b;
}
static void emit32(uint32_t v) {
    emit8((uint8_t)(v & 0xFF));
    emit8((uint8_t)((v >> 8) & 0xFF));
    emit8((uint8_t)((v >> 16) & 0xFF));
    emit8((uint8_t)((v >> 24) & 0xFF));
}
static void emit_fix32(uint8_t kind, int32_t v) {
    // emit a 4-byte placeholder + record a fixup at that position.
    uint32_t at = S.code_len;
    emit32(0);
    if (S.err) return;
    if (S.fixup_count >= MTCC_MAX_FIXUPS) { mtcc_error("too many fixups"); return; }
    S.fixups[S.fixup_count].at = at;
    S.fixups[S.fixup_count].kind = kind;
    S.fixups[S.fixup_count].v = v;
    S.fixup_count++;
}

// Register indices (modrm encoding)
enum { R_EAX = 0, R_ECX = 1, R_EDX = 2, R_EBX = 3, R_ESP = 4, R_EBP = 5 };
// jcc/setcc conditions (low opcode nibble 0F 8x / 0F 9x)
enum { CC_O = 0, CC_NO = 1, CC_B = 2, CC_NB = 3, CC_Z = 4, CC_NZ = 5,
       CC_BE = 6, CC_NBE = 7, CC_S = 8, CC_NS = 9,
       CC_L = 12, CC_NL = 13, CC_LE = 14, CC_NLE = 15 };

static void e_push(int r) { emit8((uint8_t)(0x50 + r)); }
static void e_pop(int r)  { emit8((uint8_t)(0x58 + r)); }

// mov reg, imm32
static void e_mov_ri(int r, uint32_t v) { emit8((uint8_t)(0xB8 + r)); emit32(v); }
// mov r32 <- r32 : 89 /r (reg=src, rm=dst)
static void e_mov_rr(int dst, int src) { emit8(0x89); emit8((uint8_t)(0xC0 | (src << 3) | dst)); }
// mov r32 <- [r32] : 8B /r mod=00
static void e_mov_r_rm(int dst, int base) { emit8(0x8B); emit8((uint8_t)((dst << 3) | base)); }
// mov [r32] <- r32 : 89 /r mod=00 (reg=src, rm=base)
static void e_mov_rm_r(int base, int src) { emit8(0x89); emit8((uint8_t)((src << 3) | base)); }
// mov r32 <- [ebp+disp32] : 8B /r mod=10 rm=101
static void e_mov_r_ebp(int dst, int32_t disp) {
    emit8(0x8B); emit8((uint8_t)(0x80 | (dst << 3) | 5)); emit32((uint32_t)disp);
}
// movzx eax, byte [ebp+disp32] : 0F B6 /r mod=10 rm=101 (local char load)
static void e_movzx_eax_ebp8(int32_t disp) {
    emit8(0x0F); emit8(0xB6); emit8((uint8_t)(0x80 | (0 << 3) | 5)); emit32((uint32_t)disp);
}
// mov [ebp+disp32] <- eax : 89 /r mod=10 rm=101
static void e_mov_rm_ebp_store(int32_t disp) {
    emit8(0x89); emit8((uint8_t)(0x80 | (0 << 3) | 5)); emit32((uint32_t)disp);
}
// mov [ebp+disp32] <- al : 88 /r mod=10 rm=101 (local char store)
static void e_mov_ebp8_al(int32_t disp) {
    emit8(0x88); emit8((uint8_t)(0x80 | (0 << 3) | 5)); emit32((uint32_t)disp);
}
// movzx eax, byte [imm32] : 0F B6 05 moffs (global char load, ABS_DATA fixup)
static void e_movzx_eax_moffs8(int32_t data_off) {
    emit8(0x0F); emit8(0xB6); emit8(0x05); emit_fix32(FX_ABS_DATA, data_off);
}
// lea r32, [ebp+disp32] : 8D /r mod=10 rm=101
static void e_lea_ebp(int dst, int32_t disp) {
    emit8(0x8D); emit8((uint8_t)(0x80 | (dst << 3) | 5)); emit32((uint32_t)disp);
}
// mov eax, [imm32] (moffs — load global) : A1
static void e_mov_eax_moffs(int32_t data_off) {
    emit8(0xA1); emit_fix32(FX_ABS_DATA, data_off);
}
// mov eax, imm32 that is a data ADDRESS (ABS_DATA fixup) — used for
// global & string literal addresses.
static void e_mov_eax_data_addr(int32_t data_off) {
    emit8(0xB8); emit_fix32(FX_ABS_DATA, data_off);
}
// mov byte [r32] <- al
static void e_mov_rm8_al(int base) { emit8(0x88); emit8((uint8_t)base); }
// movzx eax, byte [r32] : 0F B6 /r mod=00
static void e_movzx_eax_rm8(int base) { emit8(0x0F); emit8(0xB6); emit8((uint8_t)base); }
// movzx edx, byte [eax]
static void e_movzx_edx_rm8_eax(void) { emit8(0x0F); emit8(0xB6); emit8(0x10); }
// movzx eax, al : 0F B6 C0
static void e_movzx_eax_al(void) { emit8(0x0F); emit8(0xB6); emit8(0xC0); }

// ALU reg-reg: add=01 or=09 and=21 xor=31 sub=29 cmp=39 (eax OP ebx)
static void e_alu_eax_ebx(uint8_t op) { emit8(op); emit8(0xD8); }
// imul eax, ebx : 0F AF C3
static void e_imul_eax_ebx(void) { emit8(0x0F); emit8(0xAF); emit8(0xC3); }
// imul eax, imm8 : 6B C0 ib   (index scaling)
static void e_imul_eax_i8(uint8_t v) { emit8(0x6B); emit8(0xC0); emit8(v); }
// imul ebx, imm8 : 6B DB ib   (rhs scaling for pointer arith)
static void e_imul_ebx_i8(uint8_t v) { emit8(0x6B); emit8(0xDB); emit8(v); }
// cdq + idiv ebx -> quotient in eax (remainder in edx)
static void e_div_ebx(void) { emit8(0x99); emit8(0xF7); emit8(0xFB); }
// mov eax, edx (remainder of %)
static void e_mov_eax_edx(void) { emit8(0x89); emit8(0xD0); }
// mov edx, eax (save the old value for post-inc/dec)
static void e_mov_edx_eax(void) { emit8(0x89); emit8(0xC2); }
// neg eax / not eax
static void e_neg_eax(void) { emit8(0xF7); emit8(0xD8); }
static void e_not_eax(void) { emit8(0xF7); emit8(0xD0); }
// mov ecx, ebx then shl/shr/sar eax, cl
static void e_shift_eax(uint8_t ext) { emit8(0x89); emit8(0xD9); emit8(0xD3); emit8(ext); }
#define E_SHL_EAX_CL  e_shift_eax(0xE0)
#define E_SHR_EAX_CL  e_shift_eax(0xE8)
#define E_SAR_EAX_CL  e_shift_eax(0xF8)
// test eax, eax
static void e_test_eax(void) { emit8(0x85); emit8(0xC0); }
// cmp eax, ebx
static void e_cmp_eax_ebx(void) { emit8(0x39); emit8(0xD8); }
// setcc al (0F 9x C0)
static void e_setcc_al(int cc) { emit8(0x0F); emit8((uint8_t)(0x90 + cc)); emit8(0xC0); }
// jmp rel32 (target = code offset; may be -1 = filled in later)
static void e_jmp(int32_t target) { emit8(0xE9); emit_fix32(FX_REL32, target); }
// jcc rel32
static void e_jcc(int cc, int32_t target) {
    emit8(0x0F); emit8((uint8_t)(0x80 + cc)); emit_fix32(FX_REL32, target);
}
// call rel32
static void e_call(int32_t target) { emit8(0xE8); emit_fix32(FX_REL32, target); }
// int 0x80 (syscall from compiled code)
static void e_int80(void) { emit8(0xCD); emit8(0x80); }
// inc/dec eax
static void e_inc_eax(void) { emit8(0xFF); emit8(0xC0); }
static void e_dec_eax(void) { emit8(0xFF); emit8(0xC8); }
// add eax, imm32 (0x05) / sub eax, imm32 (0x2D) — used to scale
// pointer ++/-- and compound assigns by the element size
static void e_add_eax_i32(uint32_t v) { emit8(0x05); emit32(v); }
static void e_sub_eax_i32(uint32_t v) { emit8(0x2D); emit32(v); }
// mov r32, [base + disp32] — used by the struct copy
static void e_mov_r_rm_disp(int dst, int base, int32_t disp) {
    emit8(0x8B); emit8((uint8_t)(0x80 | (dst << 3) | base)); emit32((uint32_t)disp);
}
// mov [base + disp32], r32
static void e_mov_rm_r_disp(int base, int src, int32_t disp) {
    emit8(0x89); emit8((uint8_t)(0x80 | (src << 3) | base)); emit32((uint32_t)disp);
}
// sub esp, imm32 (frame; operand patched later) / add esp, imm8
static void e_sub_esp_i32(uint32_t v) { emit8(0x81); emit8(0xEC); emit32(v); }
static void e_add_esp_i8(uint8_t v)   { emit8(0x83); emit8(0xC4); emit8(v); }
// epilogue: mov esp,ebp; pop ebp; ret
static void e_epilogue(void) { emit8(0x89); emit8(0xEC); emit8(0x5D); emit8(0xC3); }

// Write a 4-byte little-endian value to any buffer (used for fixup patching).
static void w32(uint8_t* buf, uint32_t off, uint32_t v) {
    buf[off]     = (uint8_t)(v & 0xFF);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    buf[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    buf[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

// ============================================================================
//  SYMBOL TABLE
// ============================================================================
static Func* find_func(const char* name) {
    for (uint32_t i = 0; i < S.func_count; i++)
        if (m_streq(S.funcs[i].name, name)) return &S.funcs[i];
    return NULL;
}
// locals: search from the TOP (innermost scope wins — shadowing)
static LVar* find_local(const char* name) {
    for (int i = (int)S.local_count - 1; i >= 0; i--)
        if (m_streq(S.locals[i].name, name)) return &S.locals[i];
    return NULL;
}
static GVar* find_gvar(const char* name) {
    for (uint32_t i = 0; i < S.gvar_count; i++)
        if (m_streq(S.gvars[i].name, name)) return &S.gvars[i];
    return NULL;
}
static void scope_push(void) {
    if (S.scope_depth >= MTCC_SCOPE_DEPTH) { mtcc_error("scopes nested too deep"); return; }
    S.scope_marks[S.scope_depth++] = S.local_count;
}
static void scope_pop(void) {
    if (S.scope_depth == 0) return;
    S.local_count = S.scope_marks[--S.scope_depth];
}

// ============================================================================
//  BUILTINS — runtime functions available TO COMPILED CODE.
//  Their implementation = emit inline `int 0x80` with the kernel syscall
//  number (see kernel/library/header/syscall.h). There is NO libc in
//  compiled programs — the OS itself is the runtime.
// ============================================================================
struct Builtin {
    const char* name;
    uint8_t sysnum;
    uint8_t nargs;    // 0..3 (the syscall ABI only has 3 argument slots)
    CType ret;
};
static const Builtin BUILTINS[] = {
    { "print",    SYS_PRINT,    1, { TY_INT, 0, 0, 0, 0, 0 } },  // print(str)
    { "printint", SYS_PRINTINT, 1, { TY_INT, 0, 0, 0, 0, 0 } },  // printint(num)
    { "getkey",   SYS_GETKEY,   0, { TY_INT, 0, 0, 0, 0, 0 } },
    { "readline", SYS_READLINE, 2, { TY_INT, 0, 0, 0, 0, 0 } },  // readline(buf,maxlen)
    { "write",    SYS_WRITE,    3, { TY_INT, 0, 0, 0, 0, 0 } },  // write(fd,buf,len)
    { "open",     SYS_OPEN,     1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "read",     SYS_READ,     3, { TY_INT, 0, 0, 0, 0, 0 } },  // read(fd,buf,len)
    { "close",    SYS_CLOSE,    1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "malloc",   SYS_MALLOC,   1, { TY_INT, 1, 0, 0, 0, 0 } },  // void* -> int*
    { "sleep",    SYS_SLEEP,    1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "gettick",  SYS_GETTICK,  0, { TY_INT, 0, 0, 0, 0, 0 } },
    { "getpid",   SYS_GETPID,   0, { TY_INT, 0, 0, 0, 0, 0 } },
    { "exit",     SYS_EXIT,     1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "exec",     SYS_EXEC,     1, { TY_INT, 0, 0, 0, 0, 0 } },  // nested -> EBUSY -9 (see above)
    { "getargs",  SYS_GETARGS,  2, { TY_INT, 0, 0, 0, 0, 0 } },
    { "mkfile",   SYS_MKFILE,   3, { TY_INT, 0, 0, 0, 0, 0 } },
    // ---- Morph.h-compatible file API (same names as the SDK header) ----
    // The mini preprocessor splices <morph.h>-family headers since v10.8,
    // but programs compiled WITHOUT includes still see these builtins —
    // they give mtcc programs the exact Morph.h file names directly.
    { "file_open",     SYS_OPEN,      1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "file_read",     SYS_READ,      3, { TY_INT, 0, 0, 0, 0, 0 } },
    { "file_close",    SYS_CLOSE,     1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "file_write",    SYS_MKFILE,    3, { TY_INT, 0, 0, 0, 0, 0 } },  // create-or-overwrite
    { "file_read_all", SYS_READFILE,  3, { TY_INT, 0, 0, 0, 0, 0 } },
    { "file_size",     SYS_FILESIZE,  1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "file_exists",   SYS_FILEEXISTS,1, { TY_INT, 0, 0, 0, 0, 0 } },
    /* v10.12 Phase C — ring-3 networking (netinfo 10-word, see syscall.h) */
    { "net_info",      SYS_NETINFO,   1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "net_ping",      SYS_NETPING,   1, { TY_INT, 0, 0, 0, 0, 0 } },
    // ---- Morph.h-compatible GAME API (same names as the SDK header) ----
    // fb_info(int fb[6]) fills {addr,width,height,bpp,pitch,avail} in
    // place (arrays decay to pointers, so the syscall writes through).
    { "fb_info",     SYS_FBINFO,     1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "put_pixel",   SYS_PUTPIXEL,   3, { TY_INT, 0, 0, 0, 0, 0 } },  // (x,y,color)
    // fill_rect packs 5 values into 3 regs: caller writes
    //   fill_rect(x | (w << 16), y | (h << 16), color);
    { "fill_rect",   SYS_FILLRECT,   3, { TY_INT, 0, 0, 0, 0, 0 } },
    { "pollkey",     SYS_POLLKEY,    0, { TY_INT, 0, 0, 0, 0, 0 } },  // 0 = no key
    // key_event (syscall 32, the DOOM input path): press AND release with
    // the raw code. Return 0 = empty, else bit16 = 1 press / 0 release and
    // bits 0-15 = the signed raw code (cast back with `(ev << 16) >> 16`).
    // pollkey() only ever sees presses, so a program that must track HELD
    // keys (game key state, EX9E/EXA1 in a CHIP-8 emulator) needs this.
    // Restored in v0.9.3: the kernel still serves syscall 32 and
    // mrp_user/Morph.h still exports key_event() for the SDK path — only
    // this builtin entry was dropped, which broke in-OS builds of the
    // wolf and emu-ch8 packages.
    { "key_event",   SYS_KEYEVENT,   0, { TY_INT, 0, 0, 0, 0, 0 } },
    { "mouse_state", SYS_MOUSE,      1, { TY_INT, 0, 0, 0, 0, 0 } },  // (int m[3])
    { "spk_tone",    SYS_SPEAKER,    1, { TY_INT, 0, 0, 0, 0, 0 } },  // freq Hz
    { "spk_silence", SYS_SPEAKER,    0, { TY_INT, 0, 0, 0, 0, 0 } },  // freq=0 path
    { "snd_beep",    SYS_SNDBEEP,    2, { TY_INT, 0, 0, 0, 0, 0 } },  // (freq,ms) queued
    // ---- v10.8 libc layer (syscalls 27-29 + internal aliases) ----
    // lseek: fd positioning (fseek/ftell user-side build on this).
    { "lseek",         SYS_LSEEK,    3, { TY_INT, 0, 0, 0, 0, 0 } },  // (fd,off,whence)
    // __arena_alloc: raw block from the kernel MRP arena — the prelude
    // heap (malloc/free/calloc/realloc in user space) carves chunks
    // out of this. Double-underscore name so user code never collides.
    { "__arena_alloc", SYS_MALLOC,   1, { TY_INT, 1, 0, 0, 0, 0 } },  // (bytes) -> int*
    // __sys_printf: legacy kernel-side printf (SYS_PRINTF) — kept for
    // compatibility. Since v0.3 FR-19 the prelude printf() renders
    // LOCALLY via __vsnprintf (up to 7 conversions) and does not use
    // this builtin; old programs calling it directly still work.
    { "__sys_printf",  SYS_PRINTF,   2, { TY_INT, 0, 0, 0, 0, 0 } },  // (fmt, int* args)
    // ring: privilege level of the CALLER (0 shell/kernel, 3 user
    // program) — lets a program prove it runs unprivileged.
    { "ring",          SYS_RINGINFO, 0, { TY_INT, 0, 0, 0, 0, 0 } },
    // ---- Phase B: multitasking (used by the multitasking.h prelude) ----
    // __task_spawn2(path, arena_hint): 2 mandatory args (mtcc has no
    // default-args) — the prelude provides the task_spawn(path) wrapper.
    { "__task_spawn2", SYS_SPAWN,    2, { TY_INT, 0, 0, 0, 0, 0 } },
    { "__task_yield0", SYS_YIELD,    0, { TY_INT, 0, 0, 0, 0, 0 } },
    // Phase C: kill(pid) + taskinfo(u32* out)
    { "__task_kill1",  SYS_KILL,     1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "__task_info1",  SYS_TASKINFO, 1, { TY_INT, 0, 0, 0, 0, 0 } },
    // ---- v0.3 FR-01: full file syscalls (used by the fileio.h prelude) ----
    // __sys_open2(path, flags) — flags = F_* constants from fileio.h
    { "__sys_open2",   SYS_OPEN2,   2, { TY_INT, 0, 0, 0, 0, 0 } },
    { "__sys_unlink1", SYS_UNLINK,  1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "__sys_mkdir1",  SYS_MKDIR,   1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "__sys_rmdir1",  SYS_RMDIR,   1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "__sys_rename2", SYS_RENAME,  2, { TY_INT, 0, 0, 0, 0, 0 } },
    { "__sys_stat2",   SYS_STAT,    2, { TY_INT, 0, 0, 0, 0, 0 } },  // (path, int* st)
    { "__sys_readdir2",SYS_READDIR, 2, { TY_INT, 0, 0, 0, 0, 0 } },  // (fd, char* de)
    { "__sys_fstat2",  SYS_FSTAT,   2, { TY_INT, 0, 0, 0, 0, 0 } },  // (fd, int* st)
    // v0.3 FR-03: free one SYS_MALLOC/__arena_alloc block
    { "__sys_free1",   SYS_FREE,    1, { TY_INT, 0, 0, 0, 0, 0 } },
    // ---- v0.3: wait/pipe/meminfo/spawn2 (multitasking.h prelude) ----
    { "__task_wait2",  SYS_WAIT,    2, { TY_INT, 0, 0, 0, 0, 0 } },  // (pid, int* status)
    { "__pipe_cre1",   SYS_PIPE,    1, { TY_INT, 0, 0, 0, 0, 0 } },  // (int fds[2])
    { "__meminfo1",    SYS_MEMINFO, 1, { TY_INT, 0, 0, 0, 0, 0 } },  // (int w[6])
    { "__task_spawn3", SYS_SPAWN2,  3, { TY_INT, 0, 0, 0, 0, 0 } },  // (path,hint,args)
    // ---- v0.3 (FR-17): per-task graphics window + line ----
    // set_clip(x | w<<16, y | h<<16) confines this task's draws; the
    // prelude wrapper takes the same packed form as fill_rect.
    { "__gfx_clip2",   SYS_SETCLIP,  2, { TY_INT, 0, 0, 0, 0, 0 } },
    // draw_line(x0|y0<<16, x1|y1<<16, color) - Bresenham, clipped.
    { "__gfx_line3",   SYS_DRAWLINE, 3, { TY_INT, 0, 0, 0, 0, 0 } },
    /* extended 0.5: COW fork + demand-backed memory + ring-3 socket client */
    { "fork",          SYS_FORK,     0, { TY_INT, 0, 0, 0, 0, 0 } },
    { "sbrk",          SYS_SBRK,     1, { TY_INT, 0, 0, 0, 0, 0 } },
    { "mmap",          SYS_MMAP,     2, { TY_INT, 0, 0, 0, 0, 0 } },
    { "socket",        SYS_SOCKET,   3, { TY_INT, 0, 0, 0, 0, 0 } },
    { "net",           SYS_NET,      3, { TY_INT, 0, 0, 0, 0, 0 } },
    { "net_connect",   SYS_NET,      3, { TY_INT, 0, 0, 0, 0, 0 } },
};
#define BUILTIN_COUNT (sizeof(BUILTINS) / sizeof(BUILTINS[0]))

static const Builtin* find_builtin(const char* name) {
    for (uint32_t i = 0; i < BUILTIN_COUNT; i++)
        if (m_streq(BUILTINS[i].name, name)) return &BUILTINS[i];
    return NULL;
}

// ============================================================================
//  MORPH PRELUDE (v10.8 -> v0.3.2) — the Equinox OS libc.
// ----------------------------------------------------------------------------
//  REMOVED from the compiler image in v0.3.2: the libc now lives as REAL
//  FILES in RAMFS /equinox/libc (staged from the source tree libc/):
//
//     morph.h    master include: macros + the 12 module includes
//     memory.c   memcpy/memset/memmove/memcmp/memchr
//     string.c   strlen/strcmp/strcpy/strcat/strchr/strstr/...
//     convert.c  atoi/strtol/itoa/utoa
//     ctype.c    isspace/isdigit/... (13 functions)
//     env.c      abort/time/getenv/abs/rand/srand
//     heap.c     user-space malloc/free/calloc/realloc
//     strx.c     strdup/strtok/strspn/strcspn/strcasecmp
//     printf.c   printf/sprintf/snprintf (local renderer)
//     sscanf.c   sscanf
//     stdio.c    fopen/fread/fwrite/fseek/fclose + extras
//     qsort.c    qsort_int/qsort_str
//     gfx.c      set_clip/draw_line (FR-17)
//
//  Every module carries an include guard + explicit dependency includes,
//  so it compiles standalone (mtcc --lib) AND composes into the full libc
//  when <morph.h> (aliases: <stdio.h> <stdlib.h> <string.h>) splices
//  morph.h — see pp_splice_prelude(). `equinoxinstall` check-compiles
//  each module in-OS with 2 parallel mtcc tasks.
// ============================================================================
// ============================================================================
//  MULTITASKING PRELUDE (Phase B) — the virtual <multitasking.h> content.
//  mtcc programs can create new .mrp tasks (non-blocking) and yield:
//      task_spawn("hello.mrp")     -> pid (>0) / negative errno
//      task_spawn_hint(f, bytes)   -> spawn with an arena hint
//      task_yield()                -> give the CPU to other tasks
//      task_pid()                  -> this task's pid
//  The wrappers use the __task_spawn2/task_yield builtins (mtcc has no
//  default arguments — 2-arg builtin + 1-arg wrapper in the prelude).
// ============================================================================
static const char* const MULTITASKING_PRELUDE[] = {
    "#ifndef MULTITASKING_H_INCLUDED\n",
    "#define MULTITASKING_H_INCLUDED\n",
    "/* multitasking.h - Equinox OS task API (Phase B, mtcc in-OS build)\n",
    "   Spliced by: #include <multitasking.h> */\n",
    "\n",
    "/* run another .mrp in a NEW task (non-blocking, 2 MB arena).\n",
    "   return: pid > 0 on success / negative = error */\n",
    "int task_spawn(char* path) {\n",
    "    return __task_spawn2(path, 0);\n",
    "}\n",
    "\n",
    "/* spawn with an arena hint (heap slack bytes, e.g. 8388608 = 8 MB) */\n",
    "int task_spawn_hint(char* path, int arena_hint) {\n",
    "    return __task_spawn2(path, arena_hint);\n",
    "}\n",
    "\n",
    "/* give the CPU to the next task (round-robin scheduler) */\n",
    "void task_yield(void) {\n",
    "    __task_yield0();\n",
    "}\n",
    "\n",
    "/* this task's pid */\n",
    "int task_pid(void) {\n",
    "    return getpid();\n",
    "}\n",
    "\n",
    "/* Phase C: kill another task by pid (0 = success). */\n",
    "int task_kill(int pid) {\n",
    "    return __task_kill1(pid);\n",
    "}\n",
    "\n",
    "/* Phase C: snapshot of the task list.\n",
    "   out[0] = count N, then N entries of 4 ints: {pid, state, kind, console}.\n",
    "   Call with an array of at least 33 ints:\n",
    "     int info[33]; int n = task_list(info); */\n",
    "int task_list(int* out) {\n",
    "    return __task_info1(out);\n",
    "}\n",
    "\n",
    "/* v0.3: blocking waitpid - pid > 0 = that child, 0/-1 = any. */\n",
    "/* Returns the reaped child's pid, or -13 (ECHILD). */\n",
    "int task_wait(int pid, int* status_out) {\n",
    "    return __task_wait2(pid, status_out);\n",
    "}\n",
    "\n",
    "/* v0.3: create an anonymous pipe: fds[0]=read end,\n",
    "   fds[1]=write end. A spawned child INHERITS the parent's pipe\n",
    "   fds - close the end you do not use so EOF/broken-pipe work. */\n",
    "int pipe_create(int* fds) {\n",
    "    return __pipe_cre1(fds);\n",
    "}\n",
    "\n",
    "/* v0.3: spawn a NEW task with an explicit args string\n",
    "   (the child reads it via getargs()). Returns pid / errno. */\n",
    "int task_spawn_args(char* path, int arena_hint, char* args) {\n",
    "    return __task_spawn3(path, arena_hint, args);\n",
    "}\n",
    "\n",
    "/* v0.3: memory statistics - w[0]=pool total KB,\n",
    "   w[1]=pool free KB, w[2]=faulted-in user KB, w[3]=live tasks,\n",
    "   w[4]=zombies, w[5]=0. Returns 0 / errno. */\n",
    "int mem_info(int* w) {\n",
    "    return __meminfo1(w);\n",
    "}\n",
    "\n",
    "#endif\n",
};

#define MULTITASKING_PRELUDE_LINES (sizeof(MULTITASKING_PRELUDE) / sizeof(MULTITASKING_PRELUDE[0]))

// ============================================================================
//  FILEIO_PRELUDE (v0.3 FR-01) — virtual contents of `#include <fileio.h>`.
// ----------------------------------------------------------------------------
//  Thin wrappers over the full file syscalls #40-#48 (open flags, unlink,
//  mkdir/rmdir, rename, stat/readdir/fstat). mtcc has no struct support,
//  so morph_stat_t = int[4] and morph_dirent_t = char[72] with the two
//  ints at offsets 64/68 — documented per function below.
// ============================================================================
static const char* const FILEIO_PRELUDE[] = {
    "#ifndef FILEIO_H_INCLUDED\n",
    "#define FILEIO_H_INCLUDED\n",
    "/* fileio.h - Equinox OS file syscalls (v0.3 FR-01)\n",
    "   Spliced by: #include <fileio.h> */\n",
    "\n",
    "/* ---- open flags (f_open) ---- */\n",
    "#define F_RDONLY 0\n",
    "#define F_WRONLY 1\n",
    "#define F_RDWR 2\n",
    "#define F_CREAT 256\n",
    "#define F_TRUNC 512\n",
    "#define F_APPEND 1024\n",
    "#define F_EXCL 2048\n",
    "#define F_DIR 4096\n",
    "\n",
    "/* open with flags -> fd (3+), negative = errno. Combine flags with |\n",
    "   e.g. f_create = f_open(path, F_WRONLY|F_CREAT|F_TRUNC); */\n",
    "int f_open(char* path, int flags) {\n",
    "    return __sys_open2(path, flags);\n",
    "}\n",
    "\n",
    "/* remove a FILE -> 0 / errno (directories: f_rmdir) */\n",
    "int f_unlink(char* path) {\n",
    "    return __sys_unlink1(path);\n",
    "}\n",
    "\n",
    "/* create a directory -> 0 / errno */\n",
    "int f_mkdir(char* path) {\n",
    "    return __sys_mkdir1(path);\n",
    "}\n",
    "\n",
    "/* remove an EMPTY directory -> 0 / errno */\n",
    "int f_rmdir(char* path) {\n",
    "    return __sys_rmdir1(path);\n",
    "}\n",
    "\n",
    "/* rename/move a file: f_rename(old, new) -> 0 / errno */\n",
    "int f_rename(char* oldpath, char* newpath) {\n",
    "    return __sys_rename2(oldpath, newpath);\n",
    "}\n",
    "\n",
    "/* stat: st is int[4] -> [0]=size [1]=is_dir [2]=backing [3]=mode */\n",
    "int f_stat(char* path, int* st) {\n",
    "    return __sys_stat2(path, st);\n",
    "}\n",
    "\n",
    "/* readdir: de is char[72] -> name at de[0..63],\n",
    "   is_dir = int at de+64, size = int at de+68.\n",
    "   fd must be opened with f_open(dir, F_DIR).\n",
    "   return 1 = entry filled, 0 = end of directory, negative = errno */\n",
    "int f_readdir(int fd, char* de) {\n",
    "    return __sys_readdir2(fd, de);\n",
    "}\n",
    "\n",
    "/* fstat by descriptor (same int[4] layout as f_stat) */\n",
    "int f_fstat(int fd, int* st) {\n",
    "    return __sys_fstat2(fd, st);\n",
    "}\n",
    "\n",
    "/* free a raw malloc()/__arena_alloc() block (v0.3 FR-03) -> 0 / -1 */\n",
    "int f_free(char* p) {\n",
    "    return __sys_free1(p);\n",
    "}\n",
    "\n",
    "#endif\n",
};

#define FILEIO_PRELUDE_LINES (sizeof(FILEIO_PRELUDE) / sizeof(FILEIO_PRELUDE[0]))

// ============================================================================
//  PREPROCESSOR MINI (v10.8)
// ----------------------------------------------------------------------------
//  Directives (must be at the start of a line, may be preceded by spaces/tabs):
//    #include <morph.h>  — splice PRELUDE (alias: <stdio.h> <stdlib.h>
//                         <string.h> <Morph.h> "morph.h" "Morph.h")
//    #include "file.h"   — read from RAMFS (relative to the process cwd) + splice
//                         recursive splice (max 8 levels, #ifndef guards work)
//    #define NAME value  — object-like macro (identifier-boundary-aware
//                         substitution; the result is not rescanned —
//                         no composite macros)
//    #undef NAME
//    #ifdef / #ifndef / #else / #endif — conditional inclusion
//    #                   — null directive, ignored
//  String literals, char literals, and comments are NOT substituted.
//  Directive lines are replaced by blank lines (error line numbers stay
//  close; included files add an offset — the internal prelude is tested).
//  Function-like macros (#define F(x) ...) are REJECTED with a clear message.
// ============================================================================
#define PP_BUF_CAP    (256 * 1024)
#define PP_MAX_MACROS 64
#define PP_MAX_DEPTH  8
#define PP_MACRO_VAL  160

struct PpMacro {
    char name[MTCC_NAME_MAX];
    char value[PP_MACRO_VAL];
};

static struct {
    struct PpMacro macros[PP_MAX_MACROS];
    uint32_t macro_count;
    char*    buf1;                 // phase 1: include + conditionals
    char*    buf2;                 // phase 2: define/undef + substitution
    uint32_t line;
    int      err;
    char     err_msg[96];
    // stack of source directories used by __include__ fallback — a
    // relative include is tried (1) as-is, then (2) joined with the
    // directory of the file currently being preprocessed (so headers next
    // to their source work even when mtcc runs from another cwd).
    char     dirstk[8][96];
    uint8_t  dirs;
} PP;

// push the dirname of 'path' (dir of main first, splice file diads later)
static void pp_push_dirname(const char* path) {
    if (PP.dirs >= 8) return;
    PP.dirstk[PP.dirs][0] = '\0';
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    if (last > 0) {
        uint32_t j = 0;
        while ((int)j < last && j < 95) { PP.dirstk[PP.dirs][j] = path[j]; j++; }
        PP.dirstk[PP.dirs][j] = '\0';
    }
    PP.dirs++;
}
static void pp_pop_dirname(void) {
    if (PP.dirs > 0) PP.dirs--;
}

static void pp_error(uint32_t line, const char* msg) {
    if (PP.err) return;
    PP.err = 1;
    PP.line = line;
    uint32_t i = 0;
    while (msg && msg[i] && i < sizeof(PP.err_msg) - 1) { PP.err_msg[i] = msg[i]; i++; }
    PP.err_msg[i] = '\0';
}

static struct PpMacro* pp_find_macro(const char* name, uint32_t* idx_out) {
    for (uint32_t i = 0; i < PP.macro_count; i++) {
        if (m_streq(PP.macros[i].name, name)) {
            if (idx_out) *idx_out = i;
            return &PP.macros[i];
        }
    }
    return NULL;
}

static void pp_undef_macro(const char* name) {
    uint32_t idx = 0;
    if (!pp_find_macro(name, &idx)) return;
    // shift the rest of the array left by one (macro_count is small, O(n) is cheap)
    for (uint32_t i = idx; i + 1 < PP.macro_count; i++)
        PP.macros[i] = PP.macros[i + 1];
    if (PP.macro_count) PP.macro_count--;
}

static int pp_idchar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static int pp_idstart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// ---- output builder with bounds check ----
struct PpOut { char* p; uint32_t len; uint32_t cap; uint32_t line; };

static int ppo_putc(struct PpOut* o, char c) {
    if (o->len + 1 >= o->cap) { pp_error(o->line, "preprocessor: buffer full"); return 0; }
    o->p[o->len++] = c;
    if (c == '\n') o->line++;
    return 1;
}
static int ppo_puts(struct PpOut* o, const char* s) {
    while (*s) { if (!ppo_putc(o, *s)) return 0; s++; }
    return 1;
}

// Splice the libc master include /equinox/libc/morph.h — THROUGH
// pp_process so the guards + nested module includes are really
// executed (a second <stdio.h> splice is a no-op).
static void pp_process(const char* src, uint32_t len, struct PpOut* o, int depth);

// Splice MULTITASKING_PRELUDE (Phase B) — bypasses pp_process so the
// guard #ifndef MULTITASKING_H_INCLUDED benar-benar dieksekusi.
// Splice FILEIO_PRELUDE (v0.3 FR-01) — same shape as the multitasking
// splicer; the guard #ifndef FILEIO_H_INCLUDED really executes.
static void pp_splice_fileio(struct PpOut* o) {
    static char* fbuf = NULL;
    static uint32_t flen = 0;
    if (!fbuf) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < FILEIO_PRELUDE_LINES; i++) {
            const char* sp = FILEIO_PRELUDE[i];
            while (*sp) { total++; sp++; }
        }
        fbuf = (char*)os_alloc(total + 1);
        if (!fbuf) { pp_error(1, "pp: OOM fileio prelude"); return; }
        uint32_t p = 0;
        for (uint32_t i = 0; i < FILEIO_PRELUDE_LINES; i++) {
            const char* sp = FILEIO_PRELUDE[i];
            while (*sp) fbuf[p++] = *sp++;
        }
        fbuf[p] = '\0';
        flen = p;
    }
    pp_process(fbuf, flen, o, 1);
}

static void pp_splice_multitasking(struct PpOut* o) {
    static char* mbuf = NULL;
    static uint32_t mlen = 0;
    if (!mbuf) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < MULTITASKING_PRELUDE_LINES; i++) {
            const char* s = MULTITASKING_PRELUDE[i];
            while (*s) { total++; s++; }
        }
        mbuf = (char*)os_alloc(total + 1);
        if (!mbuf) { pp_error(1, "pp: OOM multitasking prelude"); return; }
        uint32_t p = 0;
        for (uint32_t i = 0; i < MULTITASKING_PRELUDE_LINES; i++) {
            const char* s = MULTITASKING_PRELUDE[i];
            while (*s) mbuf[p++] = *s++;
        }
        mbuf[p] = '\0';
        mlen = p;
    }
    pp_process(mbuf, mlen, o, 1);
}


static void pp_splice_prelude(struct PpOut* o) {
    // v0.3.2: the libc prelude lives as REAL FILES at /equinox/libc/
    // (12 guarded modules; morph.h is the master include). The file is
    // read through os_read_file — in-OS from the RAMFS, on the host
    // from the source tree ("/equinox/..." translation). The content
    // is cached per run; pp_process executes the guards + the nested
    // module includes, so a second <stdio.h> splice is a no-op.
    static char* prelude_buf = NULL;
    static uint32_t prelude_len = 0;
    if (!prelude_buf) {
        char* buf = NULL;
        uint32_t len = 0;
        if (os_read_file("/equinox/libc/morph.h", &buf, &len) != 0 || !buf) {
            pp_error(1, "pp: cannot read /equinox/libc/morph.h — libc files "
                        "missing from RAMFS/source tree");
            return;
        }
        prelude_buf = buf;
        prelude_len = len;
    }
    pp_process(prelude_buf, prelude_len, o, 1);
}

// ------------------------------------------------------------
//  ONE-PASS PROCESSING — include + define + conditional + substitution
//  in a SINGLE scan pass (exactly cpp semantics: a macro is defined
//  when encountered, #ifdef sees the macro table AT THAT MOMENT,
//  includes are spliced in place and processed with the same macro
//  state). The conditional stack is PER-FILE (it does not cross
//  include boundaries), /* */ comments cross lines via in_comment.
// ------------------------------------------------------------

static int pp_cond_active(const int* active, int cd) {
    for (int k = 0; k < cd; k++) if (!active[k]) return 0;
    return 1;
}

// Copy one line of text (up to but not including '\n') with macro
// substitution. Passes string "..." char '...' and // /* */ comments
// through verbatim. *in_comment_p = the multi-line block comment state.
static void pp_copy_line(const char* src, uint32_t len, uint32_t* io,
                         struct PpOut* o, int* in_comment_p) {
    uint32_t i = *io;
    while (i < len && src[i] != '\n') {
        char c = src[i];
        if (*in_comment_p) {
            if (!ppo_putc(o, c)) return;
            if (c == '*' && i + 1 < len && src[i+1] == '/') {
                if (!ppo_putc(o, '/')) return;
                i += 2;
                *in_comment_p = 0;
                continue;
            }
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i+1] == '*') {
            if (!ppo_putc(o, '/')) return;
            if (!ppo_putc(o, '*')) return;
            i += 2;
            *in_comment_p = 1;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i+1] == '/') {
            // line comment: copy the rest of the line as-is
            while (i < len && src[i] != '\n') {
                if (!ppo_putc(o, src[i])) return;
                i++;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            if (!ppo_putc(o, src[i])) return;
            i++;
            while (i < len) {
                if (src[i] == '\\' && i + 1 < len) {
                    if (!ppo_putc(o, src[i])) return;
                    i++;
                }
                if (!ppo_putc(o, src[i])) return;
                if (src[i] == '\n') return;   // multi-line strings are unusual: stop at this line
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (pp_idstart(c)) {
            uint32_t is = i;
            while (i < len && pp_idchar(src[i])) i++;
            char name[MTCC_NAME_MAX];
            uint32_t nl = i - is;
            if (nl >= MTCC_NAME_MAX) nl = MTCC_NAME_MAX - 1;
            for (uint32_t k = 0; k < nl; k++) name[k] = src[is + k];
            name[nl] = '\0';
            struct PpMacro* mac = pp_find_macro(name, NULL);
            if (mac) {
                if (!ppo_puts(o, mac->value)) return;
            } else {
                for (uint32_t k = is; k < i; k++) {
                    if (!ppo_putc(o, src[k])) return;
                }
            }
            continue;
        }
        if (!ppo_putc(o, src[i])) return;
        i++;
    }
    *io = i;
}

static void pp_process(const char* src, uint32_t len, struct PpOut* o, int depth) {
    int active[PP_MAX_DEPTH];
    int taken [PP_MAX_DEPTH];
    int cd = 0;
    int in_comment = 0;
    uint32_t i = 0;

    while (i < len) {
        // detect a directive at the start of a line (not inside a block comment)
        uint32_t j = i;
        while (j < len && (src[j] == ' ' || src[j] == '\t')) j++;
        int is_dir = (j < len && src[j] == '#' &&
                      (i == 0 || src[i-1] == '\n') && !in_comment);

        if (!is_dir) {
            if (pp_cond_active(active, cd)) {
                pp_copy_line(src, len, &i, o, &in_comment);
                if (PP.err) return;
            } else {
                // inactive region: discard the line contents, but still
                // track block comments so a '#' inside one is not taken
                // for a directive when leaving the region... (dead region
                // + open comment: real cpp still tracks — we follow, cheap)
                while (i < len && src[i] != '\n') {
                    if (in_comment) {
                        if (src[i] == '*' && i + 1 < len && src[i+1] == '/') {
                            i += 2;
                            in_comment = 0;
                            continue;
                        }
                    } else if (src[i] == '/' && i + 1 < len && src[i+1] == '*') {
                        i += 2;
                        in_comment = 1;
                        continue;
                    }
                    i++;
                }
            }
            if (i < len) { i++; if (!ppo_putc(o, '\n')) return; }
            continue;
        }

        // ===== directive line =====
        uint32_t e = j + 1;
        while (e < len && (src[e] == ' ' || src[e] == '\t')) e++;
        uint32_t nstart = e;
        while (e < len && pp_idchar(src[e])) e++;
        char word[16];
        uint32_t wl = e - nstart;
        if (wl >= sizeof(word)) wl = sizeof(word) - 1;
        for (uint32_t k = 0; k < wl; k++) word[k] = src[nstart + k];
        word[wl] = '\0';

        uint32_t eol = e;
        while (eol < len && src[eol] != '\n') eol++;

        if (m_streq(word, "ifdef") || m_streq(word, "ifndef")) {
            if (cd >= PP_MAX_DEPTH) { pp_error(o->line, "pp: #ifdef nested too deep"); return; }
            uint32_t m = e;
            while (m < len && (src[m] == ' ' || src[m] == '\t')) m++;
            uint32_t ms = m;
            while (m < len && pp_idchar(src[m])) m++;
            char mname[MTCC_NAME_MAX];
            uint32_t ml = m - ms;
            if (ml >= MTCC_NAME_MAX) ml = MTCC_NAME_MAX - 1;
            for (uint32_t k = 0; k < ml; k++) mname[k] = src[ms + k];
            mname[ml] = '\0';
            int def = (pp_find_macro(mname, NULL) != NULL);
            int act = def;
            if (m_streq(word, "ifndef")) act = !def;
            active[cd] = act && pp_cond_active(active, cd);
            taken[cd]  = active[cd];
            cd++;
        } else if (m_streq(word, "else")) {
            if (cd == 0) { pp_error(o->line, "pp: #else without #ifdef"); return; }
            active[cd-1] = pp_cond_active(active, cd - 1) && !taken[cd-1];
            taken[cd-1]  = taken[cd-1] || active[cd-1];
        } else if (m_streq(word, "endif")) {
            if (cd == 0) { pp_error(o->line, "pp: #endif without #ifdef"); return; }
            cd--;
        } else if (!pp_cond_active(active, cd)) {
            // directive inside an inactive region: ignore
        } else if (m_streq(word, "include")) {
            uint32_t m = e;
            while (m < len && (src[m] == ' ' || src[m] == '\t')) m++;
            if (m >= eol || (src[m] != '<' && src[m] != '"')) {
                pp_error(o->line, "pp: #include without <file> or \"file\"");
                return;
            }
            char closer = (src[m] == '<') ? '>' : '"';
            uint32_t hs = m + 1;
            uint32_t he = hs;
            while (he < eol && src[he] != closer) he++;
            char hname[96];
            uint32_t hl = he - hs;
            if (hl >= sizeof(hname)) hl = sizeof(hname) - 1;
            for (uint32_t k = 0; k < hl; k++) hname[k] = src[hs + k];
            hname[hl] = '\0';

            if (m_streq(hname, "morph.h") || m_streq(hname, "Morph.h") ||
                m_streq(hname, "stdio.h") || m_streq(hname, "stdlib.h") ||
                m_streq(hname, "string.h")) {
                // splice the prelude + execute its directives (the guard
                // works: a second include hits an already-defined #ifndef)
                pp_splice_prelude(o);
                if (PP.err) return;
            } else if (m_streq(hname, "fileio.h")) {
                // v0.3 FR-01: full file API prelude (f_open/.../f_free)
                pp_splice_fileio(o);
                if (PP.err) return;
            } else if (m_streq(hname, "multitasking.h")) {
                // Phase B: task API prelude (spawn/yield)
                pp_splice_multitasking(o);
                if (PP.err) return;
            } else if (depth >= PP_MAX_DEPTH) {
                pp_error(o->line, "pp: #include nested too deep (max 8)");
                return;
            } else {
                char* ic = NULL;
                uint32_t il = 0;
                // relative include: try as-is first, then joined to the
                // directory of the file currently being preprocessed, so
                // headers next to their source work from any cwd.
                char* use_path = NULL;
                int rc = os_read_file(hname, &ic, &il);
                if (rc != 0 && PP.dirs >= 1 && PP.dirstk[PP.dirs - 1][0]) {
                    char jp[176];
                    uint32_t j = 0;
                    while (PP.dirstk[PP.dirs - 1][j] && j < sizeof(jp) - 3) {
                        jp[j] = PP.dirstk[PP.dirs - 1][j]; j++;
                    }
                    if (j && jp[j - 1] != '/') jp[j++] = '/';
                    uint32_t k = 0;
                    while (hname[k] && j < sizeof(jp) - 2) jp[j++] = hname[k++];
                    jp[j] = '\0';
                    rc = os_read_file(jp, &ic, &il);
                    if (rc == 0) {
                        // remember the joined path so nested includes of the
                        // included file resolve relative to ITS directory.
                        static char g_include_use_buf[176];
                        uint32_t u = 0;
                        while (jp[u] && u < sizeof(g_include_use_buf) - 1) {
                            g_include_use_buf[u] = jp[u]; u++;
                        }
                        g_include_use_buf[u] = '\0';
                        use_path = g_include_use_buf;
                    }
                }
                if (rc != 0 || !ic) {
                    pp_error(o->line, "pp: cannot open include file");
                    return;
                }
                if (!use_path) use_path = hname;
                pp_push_dirname(use_path);
                pp_process(ic, il, o, depth + 1);
                pp_pop_dirname();
                // v0.6 -make: buffer include WAJIB dibebaskan — morph.h
                // meng-include 12 modul libc sebagai file, jadi tanpa
                // free ini bocor 12 × 96KB ≈ 1.1MB per job dan mtcc
                // -make mati OOM di job ke-13-an (SYS_FREE = free-list
                // split+coalesce, malloc.cpp FR-03 — reclaim sungguhan).
                os_free(ic, MTCC_SRC_CAP);
                if (PP.err) return;
            }
        } else if (m_streq(word, "define")) {
            uint32_t m = e;
            while (m < len && (src[m] == ' ' || src[m] == '\t')) m++;
            uint32_t ms = m;
            while (m < len && pp_idchar(src[m])) m++;
            char mname[MTCC_NAME_MAX];
            uint32_t ml = m - ms;
            if (ml == 0 || ml >= MTCC_NAME_MAX) { pp_error(o->line, "pp: invalid #define name"); return; }
            for (uint32_t k = 0; k < ml; k++) mname[k] = src[ms + k];
            mname[ml] = '\0';
            if (m < len && src[m] == '(') {
                pp_error(o->line, "pp: function-like macros not yet supported");
                return;
            }
            uint32_t vs = m;
            while (vs < len && (src[vs] == ' ' || src[vs] == '\t')) vs++;
            uint32_t ve = vs;
            while (ve < len && src[ve] != '\n') ve++;
            while (ve > vs && (src[ve-1] == ' ' || src[ve-1] == '\t' || src[ve-1] == '\r')) ve--;
            uint32_t vl = ve - vs;
            if (vl >= PP_MACRO_VAL) { pp_error(o->line, "pp: #define value too long"); return; }

            struct PpMacro* mac = pp_find_macro(mname, NULL);
            if (!mac) {
                if (PP.macro_count >= PP_MAX_MACROS) {
                    pp_error(o->line, "pp: too many #define (max 64)");
                    return;
                }
                mac = &PP.macros[PP.macro_count++];
            }
            for (uint32_t k = 0; k < ml; k++) mac->name[k] = mname[k];
            mac->name[ml] = '\0';
            for (uint32_t k = 0; k < vl; k++) mac->value[k] = src[vs + k];
            mac->value[vl] = '\0';
        } else if (m_streq(word, "undef")) {
            uint32_t m = e;
            while (m < len && (src[m] == ' ' || src[m] == '\t')) m++;
            uint32_t ms = m;
            while (m < len && pp_idchar(src[m])) m++;
            char mname[MTCC_NAME_MAX];
            uint32_t ml = m - ms;
            if (ml >= MTCC_NAME_MAX) ml = MTCC_NAME_MAX - 1;
            for (uint32_t k = 0; k < ml; k++) mname[k] = src[ms + k];
            mname[ml] = '\0';
            pp_undef_macro(mname);
        } else if (m_streq(word, "pragma") || m_streq(word, "error") ||
                   word[0] == '\0') {
            // #pragma / #error / null directive — ignored
        } else {
            pp_error(o->line, "pp: unknown directive");
            return;
        }

        if (eol < len) eol++;
        i = eol;
        if (!ppo_putc(o, '\n')) return;
    }
}

// ------------------------------------------------------------
//  pp_run — one pass. Returns the final buffer + length, or NULL
//  on error (PP.err/err_msg/line are filled — mtcc_compile
//  forwards them to the compiler error mechanism).
// ------------------------------------------------------------
static const char* pp_run(const char* src, uint32_t src_len, uint32_t* out_len) {
    static char* g_pool_pp1 = NULL;   // v0.6 -make: buffer PP persisten
    // ---- FIX B (multi-file): do NOT wipe the whole PP here.
    // The macro table and the include-dir stack must survive between
    // mtcc_compile_add() calls — that is what makes `#ifndef` include guards
    // work ACROSS the file list (file #1's <morph.h> defines MORPH_H_INCLUDED,
    // every later file's copy is suppressed instead of re-splicing the whole
    // libc and tripping "duplicate global") and what makes a relative
    // `#include "shared.h"` resolve next to its own source. Only the
    // per-file state is reset; mtcc_compile_start() zeroes all of PP once
    // per program, so stale macros can never leak between programs.
    PP.line = 0;
    PP.err = 0;
    PP.err_msg[0] = '\0';
    PP.buf2 = NULL;
    // v0.6 -make: buf1 juga dari pool persisten — tanpa ini, alokasi
    // 256KB per job menumpuk di arena dan mtcc -make mati OOM di job ~13
    // (PP di-memset dari nol tiap pp_run, jadi isi buffer antar-job aman).
    if (!g_pool_pp1) g_pool_pp1 = (char*)os_alloc(PP_BUF_CAP);
    PP.buf1 = g_pool_pp1;
    if (!PP.buf1) {
        pp_error(1, "pp: out of memory (preprocessor buffer)");
        return NULL;
    }

    struct PpOut o = { PP.buf1, 0, PP_BUF_CAP, 1 };
    pp_process(src, src_len, &o, 0);
    if (PP.err) return NULL;

    if (o.len == 0) { pp_error(1, "pp: source empty after preprocessing"); return NULL; }
    PP.buf1[o.len] = '\0';
    *out_len = o.len;
    return PP.buf1;
}

// ============================================================================
//  PARSER — util
// ============================================================================
static void advance(void) { lex_next(); }

static const char* tok_name(int tok) {
    switch (tok) {
        case TK_EOF:   return "end of file";
        case TK_NUM:   return "number";
        case TK_STR:   return "string";
        case TK_IDENT: return "identifier";
        case TK_KW_INT: case TK_KW_CHAR: case TK_KW_VOID:
        case TK_KW_IF: case TK_KW_ELSE: case TK_KW_WHILE:
        case TK_KW_FOR: case TK_KW_DO: case TK_KW_RETURN:
        case TK_KW_BREAK: case TK_KW_CONTINUE:
        case TK_KW_STRUCT: case TK_KW_UNION: case TK_KW_ENUM:
        case TK_KW_TYPEDEF: case TK_KW_SWITCH: case TK_KW_CASE:
        case TK_KW_DEFAULT: case TK_KW_EXTERN:
            return "keyword";
        case TK_LE: return "'<='";   case TK_GE: return "'>='";
        case TK_EQ: return "'=='";   case TK_NE: return "'!='";
        case TK_AND: return "'&&'";  case TK_OR: return "'||'";
        case TK_SHL: return "'<<'";  case TK_SHR: return "'>>'";
        case TK_ADDEQ: return "'+='"; case TK_SUBEQ: return "'-='";
        case TK_MULEQ: return "'*='"; case TK_DIVEQ: return "'/='";
        case TK_MODEQ: return "'%='"; case TK_ANDEQ: return "'&='";
        case TK_OREQ: return "'|='";  case TK_XOREQ: return "'^='";
        case TK_SHLEQ: return "'<<='"; case TK_SHREQ: return "'>>='";
        case TK_INC: return "'++'";   case TK_DEC: return "'--'";
        default: return "other token";
    }
}

static void expect_tok(int tok, const char* what) {
    if (S.err) return;
    if (S.tok != tok) {
        char msg[80];
        // build the message by hand (no snprintf in .mrp)
        uint32_t i = 0;
        const char* pre = "expected ";
        while (pre[i]) { msg[i] = pre[i]; i++; }
        uint32_t j = 0;
        while (what[j] && i < sizeof(msg) - 16) msg[i++] = what[j++];
        const char* post = ", got ";
        j = 0;
        while (post[j] && i < sizeof(msg) - 16) msg[i++] = post[j++];
        const char* got = tok_name(S.tok);
        j = 0;
        while (got[j] && i < sizeof(msg) - 2) msg[i++] = got[j++];
        msg[i] = '\0';
        mtcc_error(msg);
        return;
    }
    advance();
}
#define EXPECT(c) expect_tok((c), #c)

// forward declarations (statements & expressions are mutually recursive)
static void parse_stmt(void);
static void gen_expr(CType* t);
static int32_t parse_const(void);
static void init_object(const CType* t, int32_t base, int is_local);   // struct/union {...}

// ============================================================================
//  TYPE DECLARATION (used at top level & locally)
// ----------------------------------------------------------------------------
//  base_type := int|char|void | struct [tag] [{...}] | union [tag] [{...}] |
//               enum [tag] [{...}] | <typedef-name>    {'*'}
//  The struct/union body parser registers SDefs; enum bodies register
//  constants into g_consts; typedefs map a name onto a CType.
// ============================================================================
static int find_typedef(const char* name, CType* out) {
    for (uint32_t i = 0; i < g_typedef_count; i++) {
        if (m_streq(g_typedefs[i].name, name)) { *out = g_typedefs[i].type; return 1; }
    }
    return 0;
}
static int is_typedef_name(const char* name) {
    for (uint32_t i = 0; i < g_typedef_count; i++)
        if (m_streq(g_typedefs[i].name, name)) return 1;
    return 0;
}
// accept a numeric literal OR an enum constant as an array size
static int parse_array_size(uint32_t* out) {
    if (S.tok == TK_NUM) { *out = S.num; advance(); return 1; }
    if (S.tok == TK_IDENT) {
        for (uint32_t i = 0; i < g_const_count; i++)
            if (m_streq(g_consts[i].name, S.ident)) { *out = (uint32_t)g_consts[i].val; advance(); return 1; }
    }
    return 0;
}

static CType parse_base_type(void) {
    CType t = ctype_make(TY_INT, 0);
    if (S.tok == TK_KW_INT)       { t.base = TY_INT;  advance(); }
    else if (S.tok == TK_KW_CHAR) { t.base = TY_CHAR; advance(); }
    else if (S.tok == TK_KW_VOID) { t.base = TY_VOID; advance(); }
    else if (S.tok == TK_KW_STRUCT || S.tok == TK_KW_UNION) {
        int isu = (S.tok == TK_KW_UNION);
        advance();
        char tag[MTCC_NAME_MAX]; tag[0] = '\0';
        if (S.tok == TK_IDENT) {
            uint32_t i = 0;
            while (S.ident[i] && i < MTCC_NAME_MAX - 1) { tag[i] = S.ident[i]; i++; }
            tag[i] = '\0';
            advance();
        }
        if (S.tok == '{') {
            advance();
            if (g_sdef_count >= MTCC_MAX_SDEF) { mtcc_error("too many struct/union definitions"); return t; }
            uint8_t sidx = (uint8_t)g_sdef_count;
            struct SDef* sd = &g_sdefs[g_sdef_count++];
            m_memset((uint8_t*)sd, 0, sizeof(*sd));
            uint32_t n = 0;
            while (tag[n] && n < MTCC_NAME_MAX - 1) { sd->name[n] = tag[n]; n++; }
            sd->name[n] = '\0';
            sd->is_union = (uint8_t)isu;
            sd->field_off = g_field_count;
            uint32_t off = 0;
            while (S.tok != '}' && S.tok != TK_EOF && !S.err) {
                CType ft = parse_base_type();
                if (S.err) return t;
                for (;;) {
                    if (S.tok != TK_IDENT) { mtcc_error("expected field name"); return t; }
                    CType f = ft;
                    char nm[MTCC_NAME_MAX];
                    uint32_t i = 0;
                    while (S.ident[i] && i < MTCC_NAME_MAX - 1) { nm[i] = S.ident[i]; i++; }
                    nm[i] = '\0';
                    advance();
                    if (S.tok == '[') {
                        advance();
                        uint32_t al = 0;
                        if (!parse_array_size(&al) || al == 0 || al > 4096) {
                            mtcc_error("array size must be a positive number/constant"); return t;
                        }
                        f.is_array = 1; f.arr_len = al;
                        if (f.ptr != 0) { mtcc_error("array of pointers not supported as a field"); return t; }
                        EXPECT(']');
                    }
                    if (g_field_count >= MTCC_MAX_FIELDS) { mtcc_error("too many struct fields"); return t; }
                    struct Field* fd = &g_field_pool[g_field_count++];
                    uint32_t i2 = 0;
                    while (nm[i2] && i2 < MTCC_NAME_MAX - 1) { fd->name[i2] = nm[i2]; i2++; }
                    fd->name[i2] = '\0';
                    fd->type = f;
                    if (isu) { fd->offset = 0; }
                    else { off = (off + 3u) & ~3u; fd->offset = off; }
                    uint32_t fsz = f.is_array ? f.arr_len * ctype_elem_size(&f)
                                              : ctype_sizeof(&f);
                    if (isu) { if (fsz > off) off = fsz; } else { off += fsz; }
                    sd->field_count++;
                    if (S.tok == ',') { advance(); continue; }
                    break;
                }
                EXPECT(';');
            }
            EXPECT('}');
            sd->size = (off + 3u) & ~3u;
            if (sd->size == 0) sd->size = 4;
            t = ctype_make(isu ? TY_UNION : TY_STRUCT, 0);
            t.sdef = sidx;
        } else {
            int found = 0;
            for (uint32_t i = 0; i < g_sdef_count; i++) {
                if (m_streq(g_sdefs[i].name, tag)) {
                    t = ctype_make(g_sdefs[i].is_union ? TY_UNION : TY_STRUCT, 0);
                    t.sdef = (uint8_t)i;
                    found = 1; break;
                }
            }
            if (!found) { mtcc_error("unknown struct/union tag (define it before use)"); return t; }
        }
    }
    else if (S.tok == TK_KW_ENUM) {
        advance();
        if (S.tok == TK_IDENT) advance();      // enum tag (kept for compat, unused)
        if (S.tok == '{') {
            advance();
            int32_t v = 0;
            for (;;) {
                if (S.tok != TK_IDENT) { mtcc_error("expected enumerator name"); return t; }
                if (g_const_count >= 128) { mtcc_error("too many enumerators"); return t; }
                char nm[MTCC_NAME_MAX];
                uint32_t i = 0;
                while (S.ident[i] && i < MTCC_NAME_MAX - 1) { nm[i] = S.ident[i]; i++; }
                nm[i] = '\0';
                advance();
                if (S.tok == '=') {
                    advance();
                    if (S.tok == '-') {
                        advance();
                        if (S.tok == TK_NUM) { v = -(int32_t)S.num; advance(); }
                        else { mtcc_error("bad enumerator value"); return t; }
                    } else if (S.tok == TK_NUM) { v = (int32_t)S.num; advance(); }
                    else if (S.tok == TK_IDENT) {
                        int ok = 0;
                        for (uint32_t k = 0; k < g_const_count; k++)
                            if (m_streq(g_consts[k].name, S.ident)) { v = g_consts[k].val; ok = 1; break; }
                        if (!ok) { mtcc_error("enumerator needs a constant value"); return t; }
                        advance();
                    } else { mtcc_error("bad enumerator value"); return t; }
                }
                uint32_t k = 0;
                while (nm[k] && k < MTCC_NAME_MAX - 1) { g_consts[g_const_count].name[k] = nm[k]; k++; }
                g_consts[g_const_count].name[k] = '\0';
                g_consts[g_const_count].val = v;
                g_const_count++;
                v++;
                if (S.tok == ',') { advance(); continue; }
                break;
            }
            EXPECT('}');
        }
        t.base = TY_INT;   // enum variables are int-sized in mtcc
    }
    else if (S.tok == TK_IDENT) {
        CType tt;
        if (find_typedef(S.ident, &tt)) { t = tt; advance(); }
        else { mtcc_error("expected a type (int/char/void/struct/union/enum/typedef)"); return t; }
    }
    else { mtcc_error("expected a type (int/char/void/struct/union/enum/typedef)"); return t; }
    while (S.tok == '*') { if (t.ptr < 2) t.ptr++; advance(); }
    // Note: plain void (without '*') is LEGAL as a function return type;
    // rejecting "void as a variable" happens in decl_local/gvar.
    return t;
}

static void parse_typedef(void) {
    advance();   // 'typedef'
    CType ct = parse_base_type();
    if (S.err) return;
    if (S.tok != TK_IDENT) { mtcc_error("expected typedef name"); return; }
    if (g_typedef_count >= 64) { mtcc_error("too many typedefs"); return; }
    uint32_t i = 0;
    while (S.ident[i] && i < MTCC_NAME_MAX - 1) { g_typedefs[g_typedef_count].name[i] = S.ident[i]; i++; }
    g_typedefs[g_typedef_count].name[i] = '\0';
    g_typedefs[g_typedef_count].type = ct;
    g_typedef_count++;
    advance();
    EXPECT(';');
}

// Allocate a frame slot for a local. Returns ebp_off (negative).
static int32_t frame_alloc(uint32_t size4) {
    if ((uint32_t)(-S.frame_off) + size4 > MTCC_FRAME_MAX) {
        mtcc_error("local frame too large (too many vars/arrays)");
        return -4;
    }
    S.frame_off -= (int32_t)size4;
    return S.frame_off;
}

static void decl_local(CType t) {
    // t is already base+ptr; here: ident, array, init. Called from
    // parse_stmt / for-init.
    if (t.base == TY_VOID && t.ptr == 0) {
        mtcc_error("void variable has no size (only valid for function returns)");
        return;
    }
    if (S.tok != TK_IDENT) { mtcc_error("expected a variable name"); return; }
    char name[MTCC_NAME_MAX];
    uint32_t i = 0;
    while (S.ident[i]) { name[i] = S.ident[i]; i++; }
    name[i] = '\0';
    advance();

    CType vt = t;
    if (S.tok == '[') {
        advance();
        uint32_t al = 0;
        if (!parse_array_size(&al)) { mtcc_error("array size must be a number/constant"); return; }
        if (al == 0 || al > 4096) { mtcc_error("unreasonable array size"); return; }
        vt.is_array = 1;
        vt.arr_len = al;
        if (vt.ptr != 0) { mtcc_error("array of pointers not yet supported"); return; }
        EXPECT(']');
        if (S.tok == '=') { mtcc_error("local array initializer not yet supported (use a global)"); return; }
        // declare now (before goto? no — immediately)
        if (S.local_count >= MTCC_MAX_LOCALS) { mtcc_error("too many local variables"); return; }
        LVar* lv = &S.locals[S.local_count++];
        for (i = 0; i < MTCC_NAME_MAX; i++) lv->name[i] = name[i];
        lv->type = vt;
        lv->ebp_off = frame_alloc(ctype_alloc_size(&vt));
        return;
    }

    // scalar init: generate BEFORE declaring so `int x = x;` is an error
    // (x does not exist yet on the RHS — stricter than C, safer).
    if (S.tok == '=') {
        if ((vt.base == TY_STRUCT || vt.base == TY_UNION) && vt.ptr == 0) {
            // LOCAL struct/union initializer: there is no RHS expression to
            // evaluate before the declaration (the {...} is all constants),
            // so allocate the frame slot first and emit a store per element.
            if (S.local_count >= MTCC_MAX_LOCALS) { mtcc_error("too many local variables"); return; }
            LVar* lv = &S.locals[S.local_count++];
            for (i = 0; i < MTCC_NAME_MAX; i++) lv->name[i] = name[i];
            lv->type = vt;
            lv->ebp_off = frame_alloc(ctype_alloc_size(&vt));
            advance();                              // '='
            init_object(&vt, lv->ebp_off, 1);
            return;
        }
        advance();
        CType it;
        gen_expr(&it);
        if (S.err) return;
        if (S.local_count >= MTCC_MAX_LOCALS) { mtcc_error("too many local variables"); return; }
        LVar* lv = &S.locals[S.local_count++];
        for (i = 0; i < MTCC_NAME_MAX; i++) lv->name[i] = name[i];
        lv->type = vt;
        lv->ebp_off = frame_alloc(MTCC_INT_SIZE);
        // store: mov [ebp+off], eax
        e_mov_rm_ebp_store(lv->ebp_off);
        return;
    }

    if (S.local_count >= MTCC_MAX_LOCALS) { mtcc_error("too many local variables"); return; }
    LVar* lv = &S.locals[S.local_count++];
    for (i = 0; i < MTCC_NAME_MAX; i++) lv->name[i] = name[i];
    lv->type = vt;
    lv->ebp_off = frame_alloc(ctype_alloc_size(&vt));
}
// ============================================================================
//  EXPRESSIONS — recursive descent parser + accumulator codegen model.
// ----------------------------------------------------------------------------
//  Contract: every gen_* leaves the expression VALUE in EAX and fills
//  *t with its type. Intermediate values (the left operand) live on
//  the STACK (push/pop), not in registers — simple, correct, and no
//  register allocator needed.
//
//  ASSIGNMENT WITHOUT AN AST (the rollback trick):
//    `a[i] = expr` needs the lhs ADDRESS, but the parser only moves
//    forward. Solution: first parse as a plain value (gen_ternary);
//    if the next token turns out to be an assignment operator →
//    ROLL BACK state (lexer position, code length, fixup count) to
//    before the lhs, then re-parse through the gen_lvalue path.
//    String literals are safe to re-lex because data_add_string
//    DEDUPs (re-lexing yields the same offset, no data duplication).
//    Postfix ++/-- uses the same trick.
// ============================================================================

// lexer+code state for rollback (the assignment trick — see the comment above)
// Forward declarations of the precedence chain (all mutually calling).
static void gen_logor(CType* t);
static void gen_logand(CType* t);
static void gen_bitor(CType* t);
static void gen_bitxor(CType* t);
static void gen_bitand(CType* t);
static void gen_equality(CType* t);
static void gen_relational(CType* t);
static void gen_shift(CType* t);
static void gen_additive(CType* t);
static void gen_term(CType* t);
static void gen_unary(CType* t);
static void gen_postfix(CType* t);
static void gen_primary(CType* t);

struct Snap {
    uint32_t src_pos, line, code_len, fixup_count;
    int tok; uint32_t num, str_off;
    char ident[MTCC_NAME_MAX];
};
static void snap_save(Snap* s) {
    s->src_pos = S.src_pos;  s->line = S.line;
    s->code_len = S.code_len; s->fixup_count = S.fixup_count;
    s->tok = S.tok; s->num = S.num; s->str_off = S.str_off;
    for (uint32_t i = 0; i < MTCC_NAME_MAX; i++) s->ident[i] = S.ident[i];
}
static void snap_restore(const Snap* s) {
    S.src_pos = s->src_pos;  S.line = s->line;
    S.code_len = s->code_len; S.fixup_count = s->fixup_count;
    S.tok = s->tok; S.num = s->num; S.str_off = s->str_off;
    for (uint32_t i = 0; i < MTCC_NAME_MAX; i++) S.ident[i] = s->ident[i];
}

static int tok_is_assign(int tok) {
    return tok == '=' ||
           (tok >= TK_ADDEQ && tok <= TK_XOREQ) ||   // += -= *= /= %= &= |= ^=
           tok == TK_SHLEQ || tok == TK_SHREQ;       // <<= >>=
}
static int assign_to_binop(int op) {
    switch (op) {
        case TK_ADDEQ: return '+';  case TK_SUBEQ: return '-';
        case TK_MULEQ: return '*';  case TK_DIVEQ: return '/';
        case TK_MODEQ: return '%';  case TK_ANDEQ: return '&';
        case TK_OREQ:  return '|';  case TK_XOREQ: return '^';
        case TK_SHLEQ: return TK_SHL; case TK_SHREQ: return TK_SHR;
    }
    return 0;
}
// ALU dasar: eax = eax <op> ebx  (lhs di eax, rhs di ebx)
static void emit_binop_eax_ebx(int op) {
    switch (op) {
        case '+':    e_alu_eax_ebx(0x01); break;
        case '-':    e_alu_eax_ebx(0x29); break;
        case '*':    e_imul_eax_ebx();    break;
        case '/':    e_div_ebx();         break;
        case '%':    e_div_ebx(); e_mov_eax_edx(); break;
        case '&':    e_alu_eax_ebx(0x21); break;
        case '|':    e_alu_eax_ebx(0x09); break;
        case '^':    e_alu_eax_ebx(0x31); break;
        case TK_SHL: E_SHL_EAX_CL; break;
        case TK_SHR: E_SAR_EAX_CL; break;   // signed shift (int = signed)
        default:     mtcc_error("unknown binary operator"); break;
    }
}

// primary marker: what gen_primary just parsed
static int g_prim_kind;                       // 0=plain value 1=function 2=builtin
static Func* g_prim_func;
static const Builtin* g_prim_builtin;

static void gen_lvalue(CType* t);
static void gen_ternary(CType* t);

// ----------------------------------------------------------------------------
//  gen_expr = highest level (supports assignment)
// ----------------------------------------------------------------------------
static void gen_expr(CType* t) {
    if (S.err) return;
    Snap snap;
    snap_save(&snap);
    gen_ternary(t);
    if (S.err) return;
    if (tok_is_assign(S.tok)) {
        int op = S.tok;
        snap_restore(&snap);          // discard the "value" version of the lhs code
        // ---- assignment path: parse the lhs as an LVALUE ----
        CType lt;
        gen_lvalue(&lt);              // eax = address of the target object
        if (S.err) return;
        e_push(R_EAX);                // stack: address
        advance();                    // consume the assignment operator
        CType rt;
        gen_expr(&rt);                // eax = rhs value
        if (S.err) return;
        if (op == '=') {
            e_pop(R_EBX);             // ebx = address
            if ((lt.base == TY_STRUCT || lt.base == TY_UNION) && lt.ptr == 0) {
                if (rt.base != lt.base || rt.sdef != lt.sdef) {
                    mtcc_error("struct/union assignment type mismatch"); return;
                }
                if (!rt.is_ref) {
                    mtcc_error("struct/union can only be assigned from a struct/union object"); return;
                }
                uint32_t sz = ctype_sizeof(&lt);
                for (uint32_t off = 0; off < sz; off += 4) {
                    e_mov_r_rm_disp(R_EDX, R_EAX, (int32_t)off);
                    e_mov_rm_r_disp(R_EBX, R_EDX, (int32_t)off);
                }
                e_mov_rr(R_EAX, R_EBX);
                *t = lt; t->is_ref = 1;
                return;
            }
            if (ctype_sizeof(&lt) == 1) e_mov_rm8_al(R_EBX);      // mov [ebx],al
            else                       e_mov_rm_r(R_EBX, R_EAX);  // mov [ebx],eax
        } else {
            if ((lt.base == TY_STRUCT || lt.base == TY_UNION) && lt.ptr == 0) {
                mtcc_error("compound assignment on a struct/union"); return;
            }
            e_mov_rr(R_EBX, R_EAX);   // ebx = rhs
            e_pop(R_EAX);             // eax = address
            e_mov_rr(R_ECX, R_EAX);   // ecx = address (kept for the store)
            // load the current value:
            if (ctype_sizeof(&lt) == 1) e_movzx_eax_rm8(R_ECX);
            else                       e_mov_r_rm(R_EAX, R_ECX);
            // p += n on a pointer: scale the rhs by the element size
            if ((op == TK_ADDEQ || op == TK_SUBEQ) && lt.ptr > 0 &&
                ctype_elem_size(&lt) != 1) {
                e_imul_ebx_i8((uint8_t)ctype_elem_size(&lt));
            }
            emit_binop_eax_ebx(assign_to_binop(op));   // eax = cur <op> rhs
            // store the result:
            if (ctype_sizeof(&lt) == 1) e_mov_rm8_al(R_ECX);
            else                       e_mov_rm_r(R_ECX, R_EAX);
        }
        *t = lt;                      // assignment result = the stored value
    }
}

// ----------------------------------------------------------------------------
//  ternary ?: — the condition is in eax, both branches also produce eax.
// ----------------------------------------------------------------------------
static void gen_ternary(CType* t) {
    CType a;
    gen_logor(&a);
    if (S.err) return;
    if (S.tok != '?') { *t = a; return; }
    advance();
    e_test_eax();
    e_jcc(CC_Z, -1);
    uint16_t fx_else = (uint16_t)(S.fixup_count - 1);
    CType bt;
    gen_expr(&bt);
    if (S.err) return;
    EXPECT(':');
    e_jmp(-1);
    uint16_t fx_end = (uint16_t)(S.fixup_count - 1);
    S.fixups[fx_else].v = (int32_t)S.code_len;
    CType ct;
    gen_expr(&ct);
    if (S.err) return;
    S.fixups[fx_end].v = (int32_t)S.code_len;
    *t = bt;
}

// ----------------------------------------------------------------------------
//  || and && with short-circuit. The result is always int 0/1.
// ----------------------------------------------------------------------------
static void gen_logor(CType* t) {
    CType a;
    gen_logand(&a);
    while (S.tok == TK_OR && !S.err) {
        advance();
        e_test_eax();
        e_jcc(CC_NZ, -1); uint16_t t1 = (uint16_t)(S.fixup_count - 1);
        CType b;
        gen_logand(&b);
        if (S.err) return;
        e_test_eax();
        e_jcc(CC_NZ, -1); uint16_t t2 = (uint16_t)(S.fixup_count - 1);
        e_mov_ri(R_EAX, 0);                 // false path
        e_jmp(-1); uint16_t jend = (uint16_t)(S.fixup_count - 1);
        S.fixups[t1].v = (int32_t)S.code_len;   // Ltrue:
        S.fixups[t2].v = (int32_t)S.code_len;
        e_mov_ri(R_EAX, 1);
        S.fixups[jend].v = (int32_t)S.code_len; // Lend:
        a = ctype_make(TY_INT, 0);   // || result is always int 0/1
    }
    // FIX (v10.8 audit): without a || operator the result type must NOT
    // be overwritten — an expression like `(g + 5)` must stay a pointer so
    // the suffix index `[0]` is valid. Before: always a plain int ->
    // `(p - 2)[0]` failed with "index on a non-array/pointer".
    *t = a;
}
static void gen_logand(CType* t) {
    CType a;
    gen_bitor(&a);
    while (S.tok == TK_AND && !S.err) {
        advance();
        e_test_eax();
        e_jcc(CC_Z, -1); uint16_t f1 = (uint16_t)(S.fixup_count - 1);
        CType b;
        gen_bitor(&b);
        if (S.err) return;
        e_test_eax();
        e_jcc(CC_Z, -1); uint16_t f2 = (uint16_t)(S.fixup_count - 1);
        e_mov_ri(R_EAX, 1);                 // true path
        e_jmp(-1); uint16_t jend = (uint16_t)(S.fixup_count - 1);
        S.fixups[f1].v = (int32_t)S.code_len;   // Lfalse:
        S.fixups[f2].v = (int32_t)S.code_len;
        e_mov_ri(R_EAX, 0);
        S.fixups[jend].v = (int32_t)S.code_len; // Lend:
        a = ctype_make(TY_INT, 0);   // && result is always int 0/1
    }
    // FIX (v10.8 audit): same as gen_logor — without a && operator
    // the result type is preserved (a pointer stays a pointer).
    *t = a;
}

// ----------------------------------------------------------------------------
//  bitwise | ^ & — standard C precedence order.
// ----------------------------------------------------------------------------
static void gen_bitor(CType* t) {
    CType a;
    gen_bitxor(&a);
    while (S.tok == '|' && !S.err) {
        advance();
        e_push(R_EAX);
        CType b;
        gen_bitxor(&b);
        e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
        e_alu_eax_ebx(0x09);
    }
    *t = a;
}
static void gen_bitxor(CType* t) {
    CType a;
    gen_bitand(&a);
    while (S.tok == '^' && !S.err) {
        advance();
        e_push(R_EAX);
        CType b;
        gen_bitand(&b);
        e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
        e_alu_eax_ebx(0x31);
    }
    *t = a;
}
static void gen_bitand(CType* t) {
    CType a;
    gen_equality(&a);
    while (S.tok == '&' && !S.err) {
        advance();
        e_push(R_EAX);
        CType b;
        gen_equality(&b);
        e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
        e_alu_eax_ebx(0x21);
    }
    *t = a;
}

// ----------------------------------------------------------------------------
//  == !=  and  < <= > >=  (signed — the mtcc int is always signed)
// ----------------------------------------------------------------------------
static void gen_equality(CType* t) {
    CType a;
    gen_relational(&a);
    while ((S.tok == TK_EQ || S.tok == TK_NE) && !S.err) {
        int op = S.tok;
        advance();
        e_push(R_EAX);
        CType b;
        gen_relational(&b);
        e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
        e_cmp_eax_ebx();
        e_setcc_al(op == TK_EQ ? CC_Z : CC_NZ);
        e_movzx_eax_al();
        a = ctype_make(TY_INT, 0);
    }
    *t = a;
}
static void gen_relational(CType* t) {
    CType a;
    gen_shift(&a);
    while ((S.tok == '<' || S.tok == '>' || S.tok == TK_LE || S.tok == TK_GE)
           && !S.err) {
        int op = S.tok;
        advance();
        e_push(R_EAX);
        CType b;
        gen_shift(&b);
        e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
        e_cmp_eax_ebx();
        int cc;
        switch (op) {
            case '<':    cc = CC_L;   break;   // setl  (eax <  ebx)
            case TK_LE:  cc = CC_LE;  break;   // setle (eax <= ebx)
            case '>':    cc = CC_NLE; break;   // setg  (eax >  ebx)
            case TK_GE:  cc = CC_NL;  break;   // setge (eax >= ebx)
            default:     cc = CC_Z;   break;
        }
        e_setcc_al(cc);
        e_movzx_eax_al();
        a = ctype_make(TY_INT, 0);
    }
    *t = a;
}

static void gen_shift(CType* t) {
    CType a;
    gen_additive(&a);
    while ((S.tok == TK_SHL || S.tok == TK_SHR) && !S.err) {
        int op = S.tok;
        advance();
        e_push(R_EAX);
        CType b;
        gen_additive(&b);
        e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
        if (op == TK_SHL) E_SHL_EAX_CL;
        else              E_SAR_EAX_CL;   // signed right shift
    }
    *t = a;
}

// ----------------------------------------------------------------------------
//  + and - with POINTER ARITHMETIC (p+i, i+p, p-i; p-p is the raw difference).
// ----------------------------------------------------------------------------
static void gen_additive(CType* t) {
    CType a;
    gen_term(&a);
    while ((S.tok == '+' || S.tok == '-') && !S.err) {
        int op = S.tok;
        advance();
        e_push(R_EAX);                     // lhs (maybe a pointer) on the stack
        CType b;
        gen_term(&b);
        if (S.err) return;
        if (a.ptr > 0 && b.ptr == 0) {
            // p ± i : scale i (the rhs, in eax) first
            { uint32_t esz = ctype_elem_size(&a);
              if (esz != 1) e_imul_eax_i8((uint8_t)esz); }
            e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
            e_alu_eax_ebx(op == '+' ? 0x01 : 0x29);
        } else if (a.ptr == 0 && b.ptr > 0 && op == '+') {
            // i + p : the rhs pointer goes eax → ebx; the int lhs is popped → scaled
            e_mov_rr(R_EBX, R_EAX);        // ebx = p
            e_pop(R_EAX);                  // eax = i
            { uint32_t esz = ctype_elem_size(&b);
              if (esz != 1) e_imul_eax_i8((uint8_t)esz); }
            e_alu_eax_ebx(0x01);           // eax = i*sz + p
        } else if (a.ptr > 0 && b.ptr > 0 && op == '-') {
            // p - p : the raw byte difference (not divided by the element size — v0.1)
            e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
            e_alu_eax_ebx(0x29);
        } else if (a.ptr > 0 && b.ptr > 0 && op == '+') {
            mtcc_error("pointer + pointer is not valid");
            return;
        } else if (a.ptr == 0 && b.ptr > 0 && op == '-') {
            mtcc_error("int - pointer is not valid");
            return;
        } else {
            e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
            e_alu_eax_ebx(op == '+' ? 0x01 : 0x29);
        }
        // result type: if either operand is a pointer → that pointer's type
        if (a.ptr > 0) { /* stays a */ }
        else if (b.ptr > 0) a = b;
        else a = ctype_make(TY_INT, 0);
    }
    *t = a;
}

// ----------------------------------------------------------------------------
//  * / %
// ----------------------------------------------------------------------------
static void gen_term(CType* t) {
    CType a;
    gen_unary(&a);
    while ((S.tok == '*' || S.tok == '/' || S.tok == '%') && !S.err) {
        int op = S.tok;
        advance();
        e_push(R_EAX);
        CType b;
        gen_unary(&b);
        e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
        emit_binop_eax_ebx(op);
        if (S.err) return;
        a = ctype_make(TY_INT, 0);
    }
    *t = a;
}

// ----------------------------------------------------------------------------
//  unary: - + ! ~ * & ++ --
// ----------------------------------------------------------------------------
static void gen_unary(CType* t) {
    if (S.err) return;
    switch (S.tok) {
        case '-':
            advance(); gen_unary(t);
            e_neg_eax();
            return;
        case '+':
            advance(); gen_unary(t);
            return;
        case '!':
            advance(); gen_unary(t);
            e_test_eax();
            e_setcc_al(CC_Z);
            e_movzx_eax_al();
            *t = ctype_make(TY_INT, 0);
            return;
        case '~':
            advance(); gen_unary(t);
            e_not_eax();
            return;
        case '*': {
            advance();
            CType it;
            gen_unary(&it);
            if (S.err) return;
            if (it.ptr == 0) { mtcc_error("dereferencing something that is not a pointer"); return; }
            it.ptr--;
            // eax currently = the object's address → LOAD its contents,
            // except a struct/union (its "value" IS its address here):
            if (it.base == TY_STRUCT || it.base == TY_UNION) {
                it.is_ref = 1;
            } else if (ctype_sizeof(&it) == 1) {
                e_movzx_eax_rm8(R_EAX);
            } else {
                e_mov_r_rm(R_EAX, R_EAX);
            }
            *t = it;
            return;
        }
        case '&': {
            advance();
            CType lt;
            gen_lvalue(&lt);
            if (S.err) return;
            // eax = address; type = pointer to lt
            *t = lt;
            if (t->ptr < 2) t->ptr++;
            t->is_array = 0;
            return;
        }
        case TK_INC:
        case TK_DEC: {
            int op = S.tok;
            advance();
            CType lt;
            gen_lvalue(&lt);
            if (S.err) return;
            if (lt.base == TY_STRUCT || lt.base == TY_UNION) {
                mtcc_error("++/-- on a struct/union"); return;
            }
            e_mov_rr(R_EBX, R_EAX);                    // ebx = address
            if (ctype_sizeof(&lt) == 1) e_movzx_eax_rm8(R_EBX);
            else                        e_mov_r_rm(R_EAX, R_EBX);
            if (lt.ptr > 0 && ctype_elem_size(&lt) != 1) {
                uint32_t esz = ctype_elem_size(&lt);
                if (op == TK_INC) e_add_eax_i32(esz);
                else              e_sub_eax_i32(esz);
            } else {
                if (op == TK_INC) e_inc_eax(); else e_dec_eax();
            }
            if (ctype_sizeof(&lt) == 1) e_mov_rm8_al(R_EBX);
            else                        e_mov_rm_r(R_EBX, R_EAX);
            *t = lt;                                    // result = the NEW value
            return;
        }
        default:
            gen_postfix(t);
            return;
    }
}

// ----------------------------------------------------------------------------
//  postfix: primary + suffix  '('call')'  '['index']'  ++/--
// ----------------------------------------------------------------------------
static void parse_call_args(uint32_t* argc) {
    // called with S.tok == '(' already consumed by the caller? NO —
    // convention: the caller already advance()d past '('; contents are parsed here.
    *argc = 0;
    if (S.tok == ')') { advance(); return; }
    for (;;) {
        if (*argc >= MTCC_MAX_ARGS) { mtcc_error("too many arguments (max 12)"); return; }
        CType at;
        gen_expr(&at);
        if (S.err) return;
        e_push(R_EAX);
        (*argc)++;
        if (S.tok == ',') { advance(); continue; }
        break;
    }
    if (S.tok != ')') { mtcc_error("expected ')' or ',' in the argument list"); return; }
    advance();
}

static void gen_call_user(Func* f, CType* t) {
    advance();                       // '('
    uint32_t argc = 0;
    parse_call_args(&argc);
    if (S.err) return;
    if (argc > f->nparams) {
        mtcc_error("argument count does not match the function declaration");
        return;
    }
    // v10.8: fewer arguments -> push DEFAULT values for the rest.
    // Pushes stay left-to-right, so defaults are pushed AFTER the user
    // args, for params argc..nparams-1 (the last param nearest the ret).
    for (uint32_t k = argc; k < f->nparams; k++) {
        if (!f->pdef_has[k]) {
            mtcc_error("missing argument (parameter without a default value)");
            return;
        }
        e_mov_ri(R_EAX, (uint32_t)f->pdef[k]);
        e_push(R_EAX);
    }
    // call rel32 — forward functions (code_off -1) are recorded as pending
    e_call(f->code_off);
    if (f->code_off < 0) {
        if (f->pending_count >= 24) { mtcc_error("too many forward calls"); return; }
        f->pending[f->pending_count++] = (uint16_t)(S.fixup_count - 1);
        if (f->first_call_line == 0) f->first_call_line = (uint16_t)S.line;
    }
    if (f->nparams) e_add_esp_i8((uint8_t)(f->nparams * 4));
    *t = f->ret;
}

static void gen_call_builtin(const Builtin* b, CType* t) {
    advance();                       // '('
    uint32_t argc = 0;
    parse_call_args(&argc);
    if (S.err) return;
    if (argc != b->nargs) {
        mtcc_error("wrong builtin argument count (see TCC.md)");
        return;
    }
    // pop the arguments into the syscall ABI registers: a1=EBX a2=ECX a3=EDX.
    // Stack: the last argument is on top → pop in reverse order.
    if (argc == 3)      { e_pop(R_EDX); e_pop(R_ECX); e_pop(R_EBX); }
    else if (argc == 2) { e_pop(R_ECX); e_pop(R_EBX); }
    else if (argc == 1) { e_pop(R_EBX); }
    else {
        // FIX v10.4 (game API): zero-arg syscalls leave EBX/ECX/EDX
        // untouched, so the kernel would read GARBAGE arguments.
        // Harmless for getkey/gettick, but spk_silence() must reach
        // the kernel as SYS_SPEAKER(0) — garbage freq would start a
        // random tone. Zero all three slots: 6 extra bytes, and the
        // syscall ABI becomes deterministic for every caller.
        e_mov_ri(R_EBX, 0);
        e_mov_ri(R_ECX, 0);
        e_mov_ri(R_EDX, 0);
    }
    e_mov_ri(R_EAX, b->sysnum);
    e_int80();                      // CD 80 — return value in EAX
    *t = b->ret;
}

static void gen_postfix(CType* t) {
    Snap snap;
    snap_save(&snap);
    gen_primary(t);
    while (!S.err) {
        if (S.tok == '(') {
            if (g_prim_kind == 1)      gen_call_user(g_prim_func, t);
            else if (g_prim_kind == 2) gen_call_builtin(g_prim_builtin, t);
            else { mtcc_error("calling something that is not a function"); return; }
            g_prim_kind = 0;           // the call result is no longer a function
            continue;                  // f()[2] is valid — keep processing suffixes
        }
        if (S.tok == '[') {
            advance();
            CType cur = *t;
            if (cur.ptr == 0) { mtcc_error("index on a non-array/pointer"); return; }
            e_push(R_EAX);             // base
            CType it;
            gen_expr(&it);             // idx → eax
            if (S.err) return;
            EXPECT(']');               // <-- consume the closing index bracket!
            { uint32_t esz = ctype_elem_size(&cur);
              if (esz == 4) e_imul_eax_i8(4);
              else if (esz != 1) e_imul_eax_i8((uint8_t)esz); }
            e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
            e_alu_eax_ebx(0x01);       // eax = base + idx*sz (element address)
            *t = cur; t->ptr--; t->is_array = 0;
            if (t->base == TY_STRUCT || t->base == TY_UNION) {
                t->is_ref = 1;         // element address, not a loaded value
            } else if (ctype_sizeof(t) == 1) {
                e_movzx_eax_rm8(R_EAX);
            } else {
                e_mov_r_rm(R_EAX, R_EAX);
            }
            continue;                  // a[i][?] → a pointer element can continue
        }
        if (S.tok == '.' || S.tok == TK_ARROW) {
            int arrow = (S.tok == TK_ARROW);
            advance();
            if (S.tok != TK_IDENT) { mtcc_error("expected field name"); return; }
            char fname[MTCC_NAME_MAX];
            { uint32_t i = 0;
              while (S.ident[i] && i < MTCC_NAME_MAX - 1) { fname[i] = S.ident[i]; i++; }
              fname[i] = '\0'; }
            advance();
            CType obj = *t;
            if (arrow) {
                if ((obj.base != TY_STRUCT && obj.base != TY_UNION) || obj.ptr != 1) {
                    mtcc_error("'->' requires a struct/union pointer"); return;
                }
                // eax already = the pointer value (loaded by primary)
            } else {
                if ((obj.base != TY_STRUCT && obj.base != TY_UNION) ||
                    obj.ptr != 0 || !obj.is_ref) {
                    mtcc_error("'.' requires a struct/union object"); return;
                }
            }
            const struct Field* f = sdef_find_field(&g_sdefs[obj.sdef], fname);
            if (!f) { mtcc_error_ident("no such field: ", fname); return; }
            if (f->offset != 0) { e_mov_ri(R_EBX, f->offset); e_alu_eax_ebx(0x01); }
            CType ft = f->type;
            if (ft.is_array) {
                ft.is_array = 0; ft.ptr = 1;
                *t = ft;         // address stays — that is the decayed value
            } else if (ft.base == TY_STRUCT || ft.base == TY_UNION) {
                ft.is_ref = 1; *t = ft;
            } else if (ft.ptr > 0) {
                *t = ft;
                e_mov_r_rm(R_EAX, R_EAX);   // load the field's pointer value
            } else if (ctype_sizeof(&ft) == 1) {
                *t = ft;
                e_movzx_eax_rm8(R_EAX);
            } else {
                *t = ft;
                e_mov_r_rm(R_EAX, R_EAX);
            }
            g_prim_kind = 0;   // never a function again
            continue;
        }
        if (S.tok == TK_INC || S.tok == TK_DEC) {
            int op = S.tok;
            // roll back & re-parse as an lvalue + post-inc/dec
            snap_restore(&snap);
            CType lt;
            gen_lvalue(&lt);
            if (S.err) return;
            advance();                                  // consume the ++/-- token
            if (lt.base == TY_STRUCT || lt.base == TY_UNION) {
                mtcc_error("++/-- on a struct/union"); return;
            }
            e_mov_rr(R_EBX, R_EAX);                    // ebx = address
            if (ctype_sizeof(&lt) == 1) e_movzx_eax_rm8(R_EBX);
            else                        e_mov_r_rm(R_EAX, R_EBX);
            e_mov_edx_eax();                            // edx = the OLD value
            if (lt.ptr > 0 && ctype_elem_size(&lt) != 1) {
                uint32_t esz = ctype_elem_size(&lt);
                if (op == TK_INC) e_add_eax_i32(esz);
                else              e_sub_eax_i32(esz);
            } else {
                if (op == TK_INC) e_inc_eax(); else e_dec_eax();
            }
            if (ctype_sizeof(&lt) == 1) e_mov_rm8_al(R_EBX);
            else                        e_mov_rm_r(R_EBX, R_EAX);
            e_mov_eax_edx();                            // result = the old value
            *t = lt;
            return;
        }
        break;
    }
}

// ----------------------------------------------------------------------------
//  primary: literal, ident, ( expr )
// ----------------------------------------------------------------------------
static void gen_primary(CType* t) {
    *t = ctype_make(TY_INT, 0);
    g_prim_kind = 0;
    if (S.err) return;

    switch (S.tok) {
        case TK_NUM:
            e_mov_ri(R_EAX, S.num);
            advance();
            return;
        case TK_STR:
            // literal address in the data area (ABS_DATA fixup), type char*
            e_mov_eax_data_addr((int32_t)S.str_off);
            *t = ctype_make(TY_CHAR, 1);
            advance();
            return;
        case '(': {
            advance();
            gen_expr(t);
            EXPECT(')');
            return;
        }
        case TK_IDENT: {
            char name[MTCC_NAME_MAX];
            for (uint32_t i = 0; i < MTCC_NAME_MAX; i++) name[i] = S.ident[i];
            advance();

            Func* f = find_func(name);
            if (f) {
                g_prim_kind = 1; g_prim_func = f;
                *t = f->ret;             // value not yet emitted — valid only
                return;                  // with the '(' suffix:  f(...)
            }
            const Builtin* b = find_builtin(name);
            if (b) {
                g_prim_kind = 2; g_prim_builtin = b;
                *t = b->ret;
                return;                  // likewise: print(...) etc.
            }
            LVar* lv = find_local(name);
            if (lv) {
                CType vt = lv->type;
                if (vt.is_array) {
                    vt.is_array = 0; vt.ptr = 1;        // decay: a == &a[0]
                    e_lea_ebp(R_EAX, lv->ebp_off);
                } else if (vt.ptr == 0 && (vt.base == TY_STRUCT || vt.base == TY_UNION)) {
                    e_lea_ebp(R_EAX, lv->ebp_off);      // address, not value
                    vt.is_ref = 1;
                } else if (ctype_sizeof(&vt) == 1) {
                    e_movzx_eax_ebp8(lv->ebp_off);      // movzx eax, byte [ebp+d]
                } else {
                    e_mov_r_ebp(R_EAX, lv->ebp_off);    // mov eax, [ebp+d]
                }
                *t = vt;
                return;
            }
            GVar* gv = find_gvar(name);
            if (gv) {
                gv->accessed = 1;
                CType vt = gv->type;
                if (vt.is_array) {
                    vt.is_array = 0; vt.ptr = 1;        // decay
                    e_mov_eax_data_addr(gv->data_off);  // mov eax, imm (address)
                    if (gv->data_off < 0 && gv->pend_n < 24) gv->pendfx[gv->pend_n++] = (uint16_t)(S.fixup_count - 1);
                } else if (vt.ptr == 0 && (vt.base == TY_STRUCT || vt.base == TY_UNION)) {
                    e_mov_eax_data_addr(gv->data_off);
                    vt.is_ref = 1;
                    if (gv->data_off < 0 && gv->pend_n < 24) gv->pendfx[gv->pend_n++] = (uint16_t)(S.fixup_count - 1);
                } else if (ctype_sizeof(&vt) == 1) {
                    e_movzx_eax_moffs8(gv->data_off);   // movzx eax, byte [addr]
                    if (gv->data_off < 0 && gv->pend_n < 24) gv->pendfx[gv->pend_n++] = (uint16_t)(S.fixup_count - 1);
                } else {
                    e_mov_eax_moffs(gv->data_off);      // mov eax, [addr]
                    if (gv->data_off < 0 && gv->pend_n < 24) gv->pendfx[gv->pend_n++] = (uint16_t)(S.fixup_count - 1);
                }
                *t = vt;
                return;
            }
            for (uint32_t i = 0; i < g_const_count; i++) {
                if (m_streq(g_consts[i].name, name)) {
                    e_mov_ri(R_EAX, (uint32_t)g_consts[i].val);
                    *t = ctype_make(TY_INT, 0);
                    return;
                }
            }
            mtcc_error_ident("unknown identifier: ", name);
            return;
        }
        // (unknown identifier — gen_primary)
        default:
            mtcc_error("invalid expression here");
            return;
    }
}

// ----------------------------------------------------------------------------
//  gen_lvalue — parse an assignable expression (*p, var, a[i]).
//  Result: eax = the object's ADDRESS, *t = the referenced object's type.
// ----------------------------------------------------------------------------
static void gen_lvalue(CType* t) {
    if (S.err) return;
    g_prim_kind = 0;

    // FIX (v10.8 audit): base flag for the index chain.
    // - ARRAY variable   : slot/data address = the first element's
    //                      address — used directly as the index base.
    // - POINTER variable : eax currently = the SLOT's address — the
    //                      pointer value must be LOADED before indexing.
    //                      Before, `p[i] = v` wrote to slot+idx*sz (a
    //                      stack address!) — silent corruption; the old
    //                      test suite never exercised assignment through
    //                      a pointer index. The second and later index
    //                      iterations also always load (the previous
    //                      iteration's result = the next pointer slot's
    //                      address).
    int base_needs_load = 0;

    if (S.tok == '*') {
        advance();
        CType it;
        gen_unary(&it);
        if (S.err) return;
        if (it.ptr == 0) { mtcc_error("dereferencing a non-pointer"); return; }
        it.ptr--;
        // eax = the address of the pointed-to object (not loaded — that
        // is exactly what the lvalue path is for)
        *t = it;
    } else if (S.tok == TK_IDENT) {
        char name[MTCC_NAME_MAX];
        for (uint32_t i = 0; i < MTCC_NAME_MAX; i++) name[i] = S.ident[i];
        advance();
        if (find_func(name))   { mtcc_error("a function is not an lvalue"); return; }
        if (find_builtin(name)) { mtcc_error("a builtin is not an lvalue"); return; }

        LVar* lv = find_local(name);
        if (lv) {
            CType vt = lv->type;
            int was_array = vt.is_array;
            if (vt.is_array) { vt.is_array = 0; vt.ptr = 1; }
            if (!was_array && vt.base != TY_STRUCT && vt.base != TY_UNION)
                base_needs_load = 1;   // pointer: load the value
            if (vt.base == TY_STRUCT || vt.base == TY_UNION) vt.is_ref = 1;
            e_lea_ebp(R_EAX, lv->ebp_off);
            *t = vt;
        } else {
            GVar* gv = find_gvar(name);
            if (!gv) { mtcc_error_ident("unknown identifier (lvalue): ", name); return; }
            gv->accessed = 1;
            CType vt = gv->type;
            int was_array = vt.is_array;
            if (vt.is_array) { vt.is_array = 0; vt.ptr = 1; }
            if (!was_array && vt.base != TY_STRUCT && vt.base != TY_UNION)
                base_needs_load = 1;   // global pointer: load [addr]
            if (vt.base == TY_STRUCT || vt.base == TY_UNION) vt.is_ref = 1;
            e_mov_eax_data_addr(gv->data_off);   // mov eax, <global address>
            if (gv->data_off < 0 && gv->pend_n < 24) gv->pendfx[gv->pend_n++] = (uint16_t)(S.fixup_count - 1);
            *t = vt;
        }
    } else {
        mtcc_error("not an lvalue (must be a variable / *ptr / index)");
        return;
    }

    // access chain: p[i], a[i], s.f, p->f, (*pp)[i] ...
    for (;;) {
        if (S.tok == '[') {
            advance();
            CType cur = *t;
            if (cur.ptr == 0) { mtcc_error("index on a non-array/pointer"); return; }
            if (base_needs_load) {
                e_mov_r_rm(R_EAX, R_EAX);     // eax = the pointer value (the real base)
                base_needs_load = 0;
            }
            e_push(R_EAX);                    // base
            CType it;
            gen_expr(&it);                    // idx → eax
            if (S.err) return;
            EXPECT(']');                      // <-- consume the closing bracket!
            { uint32_t esz = ctype_elem_size(&cur);
              if (esz == 4) e_imul_eax_i8(4);
              else if (esz != 1) e_imul_eax_i8((uint8_t)esz); }
            e_mov_rr(R_EBX, R_EAX); e_pop(R_EAX);
            e_alu_eax_ebx(0x01);              // eax = element address
            *t = cur; t->ptr--; t->is_array = 0;
            if (t->base == TY_STRUCT || t->base == TY_UNION) t->is_ref = 1;
            base_needs_load = 1;              // next index: load this result
            continue;
        }
        if (S.tok == '.' || S.tok == TK_ARROW) {
            int arrow = (S.tok == TK_ARROW);
            advance();
            if (S.tok != TK_IDENT) { mtcc_error("expected field name"); return; }
            char fname[MTCC_NAME_MAX];
            { uint32_t i = 0;
              while (S.ident[i] && i < MTCC_NAME_MAX - 1) { fname[i] = S.ident[i]; i++; }
              fname[i] = '\0'; }
            advance();
            CType obj = *t;
            if (arrow) {
                if ((obj.base != TY_STRUCT && obj.base != TY_UNION) || obj.ptr != 1) {
                    mtcc_error("'->' requires a struct/union pointer"); return;
                }
                e_mov_r_rm(R_EAX, R_EAX);   // load the pointer slot -> object addr
            } else {
                if ((obj.base != TY_STRUCT && obj.base != TY_UNION) || obj.ptr != 0) {
                    mtcc_error("'.' requires a struct/union object"); return;
                }
            }
            const struct Field* f = sdef_find_field(&g_sdefs[obj.sdef], fname);
            if (!f) { mtcc_error_ident("no such field: ", fname); return; }
            if (f->offset != 0) { e_mov_ri(R_EBX, f->offset); e_alu_eax_ebx(0x01); }
            CType ft = f->type;
            if (ft.is_array) { ft.is_array = 0; ft.ptr = 1; base_needs_load = 0; }
            else if (ft.base == TY_STRUCT || ft.base == TY_UNION) { base_needs_load = 0; }
            else if (ft.ptr > 0) base_needs_load = 1;
            else base_needs_load = 0;
            if (ft.base == TY_STRUCT || ft.base == TY_UNION) ft.is_ref = 1;
            *t = ft;
            continue;
        }
        break;
    }
}

// ============================================================================
//  STATEMENT
// ----------------------------------------------------------------------------
//  A loop has two pending fixup lists: break (target = after the loop)
//  and continue (target per loop: while→condition, do→condition, for→post).
//  The continue target is supplied at loop_pop() because it is only
//  certain at the end of the loop (especially do-while & for).
//
//  `for` layout (single-pass friendly — the post is emitted before the
//  body but EXECUTED after the body via two jumps):
//      [init]
//      Lcond:  <cond>; test; jz→Lend      ; an empty cond → no jz
//              jmp→Lbody                  ; the first iteration skips the post
//      Lpost:  <post>
//              jmp Lcond
//      Lbody:  <body>
//              jmp Lpost
//      Lend:
//  Cost: 2 jumps per iteration (vs 1 in a two-pass compiler) — not a
//  problem for v0.1; single-pass parse simplicity wins.
// ============================================================================
static void loop_push(void) {
    if (S.loop_depth >= MTCC_LOOP_DEPTH) { mtcc_error("loops nested too deep (max 16)"); return; }
    S.loops[S.loop_depth].nbrk = 0;
    S.loops[S.loop_depth].ncont = 0;
    S.loops[S.loop_depth].is_switch = 0;
    S.loops[S.loop_depth].ord = 0;
    S.loops[S.loop_depth].guard_has = 0;
    S.loop_depth++;
}
static void loop_pop(uint32_t cont_target) {
    if (S.loop_depth == 0) return;
    S.loop_depth--;
    for (uint8_t i = 0; i < S.loops[S.loop_depth].nbrk; i++)
        S.fixups[S.loops[S.loop_depth].brk[i]].v = (int32_t)S.code_len;
    for (uint8_t i = 0; i < S.loops[S.loop_depth].ncont; i++)
        S.fixups[S.loops[S.loop_depth].cont[i]].v = (int32_t)cont_target;
    S.loops[S.loop_depth].guard_has = 0;   // pending guard is patched first
}

// Patch the pending body-skip guard fixups of the top switch level with
// the current code position (= the end of the previous case's body).
static void sw_patch_guard(int d) {
    if (!S.loops[d].is_switch || !S.loops[d].guard_has) return;
    S.fixups[S.loops[d].guard0].v = (int32_t)S.code_len;
    if (S.loops[d].guard_has == 2)
        S.fixups[S.loops[d].guard1].v = (int32_t)S.code_len;
    S.loops[d].guard_has = 0;
}

static void parse_stmt(void) {
    if (S.err) return;

    switch (S.tok) {
        case ';':
            advance();
            return;

        case '{': {
            advance();
            scope_push();
            while (S.tok != '}' && S.tok != TK_EOF && !S.err) parse_stmt();
            scope_pop();
            EXPECT('}');
            return;
        }

        case TK_KW_IF: {
            advance();
            EXPECT('(');
            CType t;
            gen_expr(&t);
            e_test_eax();
            e_jcc(CC_Z, -1);
            uint16_t fx_else = (uint16_t)(S.fixup_count - 1);
            EXPECT(')');
            parse_stmt();
            if (S.tok == TK_KW_ELSE) {
                advance();
                e_jmp(-1);
                uint16_t fx_end = (uint16_t)(S.fixup_count - 1);
                S.fixups[fx_else].v = (int32_t)S.code_len;
                parse_stmt();
                S.fixups[fx_end].v = (int32_t)S.code_len;
            } else {
                S.fixups[fx_else].v = (int32_t)S.code_len;
            }
            return;
        }

        case TK_KW_WHILE: {
            advance();
            EXPECT('(');
            uint32_t lcond = S.code_len;
            CType t;
            gen_expr(&t);
            e_test_eax();
            e_jcc(CC_Z, -1);
            uint16_t fx_end = (uint16_t)(S.fixup_count - 1);
            EXPECT(')');
            loop_push();
            parse_stmt();
            e_jmp((int32_t)lcond);
            S.fixups[fx_end].v = (int32_t)S.code_len;
            loop_pop(lcond);
            return;
        }

        case TK_KW_DO: {
            advance();
            uint32_t lbody = S.code_len;
            loop_push();
            parse_stmt();
            if (S.tok != TK_KW_WHILE) { mtcc_error("do without while"); return; }
            advance();
            EXPECT('(');
            uint32_t lcond = S.code_len;
            CType t;
            gen_expr(&t);
            e_test_eax();
            e_jcc(CC_NZ, (int32_t)lbody);      // backward — final, patched immediately
            EXPECT(')');
            EXPECT(';');
            loop_pop(lcond);
            return;
        }

        case TK_KW_FOR: {
            advance();
            EXPECT('(');
            scope_push();
            // ---- init (a C99 declaration or an expression) ----
            if (S.tok == TK_KW_INT || S.tok == TK_KW_CHAR || S.tok == TK_KW_VOID ||
                S.tok == TK_KW_STRUCT || S.tok == TK_KW_UNION || S.tok == TK_KW_ENUM ||
                (S.tok == TK_IDENT && is_typedef_name(S.ident))) {
                CType bt = parse_base_type();
                decl_local(bt);
                while (S.tok == ',') { advance(); decl_local(bt); }
            } else if (S.tok != ';') {
                CType t;
                gen_expr(&t);
            }
            EXPECT(';');
            // ---- condition ----
            uint32_t lcond = S.code_len;
            uint16_t fx_end = 0xFFFF;
            if (S.tok != ';') {
                CType t;
                gen_expr(&t);
                e_test_eax();
                e_jcc(CC_Z, -1);
                fx_end = (uint16_t)(S.fixup_count - 1);
            }
            EXPECT(';');
            // ---- jump to the body (the first iteration skips the post) ----
            e_jmp(-1);
            uint16_t fx_body = (uint16_t)(S.fixup_count - 1);
            // ---- post (parsed now, executed after the body) ----
            uint32_t lpost = S.code_len;
            if (S.tok != ')') {
                CType t;
                gen_expr(&t);              // the value is discarded
                if (S.err) return;
            }
            EXPECT(')');
            e_jmp((int32_t)lcond);         // post → cond (backward)
            // ---- body ----
            uint32_t lbody = S.code_len;
            loop_push();
            parse_stmt();
            if (S.err) return;
            e_jmp((int32_t)lpost);         // body → post (backward)
            if (fx_end != 0xFFFF) S.fixups[fx_end].v = (int32_t)S.code_len;
            S.fixups[fx_body].v = (int32_t)lbody;
            loop_pop(lpost);
            scope_pop();
            return;
        }

        case TK_KW_RETURN: {
            advance();
            if (S.tok != ';') {
                if (S.cur_func && S.cur_func->ret.base == TY_VOID &&
                    S.cur_func->ret.ptr == 0) {
                    mtcc_error("a void function cannot return a value");
                    return;
                }
                CType t;
                gen_expr(&t);
                if (S.err) return;
            }
            EXPECT(';');
            e_epilogue();                  // mov esp,ebp; pop ebp; ret
            return;
        }

        case TK_KW_BREAK: {
            advance();
            EXPECT(';');
            if (S.loop_depth == 0) { mtcc_error("break outside a loop"); return; }
            e_jmp(-1);
            uint8_t d = (uint8_t)(S.loop_depth - 1);
            if (S.loops[d].nbrk >= 24) { mtcc_error("too many breaks"); return; }
            S.loops[d].brk[S.loops[d].nbrk++] = (uint16_t)(S.fixup_count - 1);
            return;
        }

        case TK_KW_CONTINUE: {
            advance();
            EXPECT(';');
            if (S.loop_depth == 0) { mtcc_error("continue outside a loop"); return; }
            uint8_t d = (uint8_t)(S.loop_depth - 1);
            if (S.loops[d].is_switch) { mtcc_error("continue inside switch"); return; }
            e_jmp(-1);
            if (S.loops[d].ncont >= 24) { mtcc_error("too many continues"); return; }
            S.loops[d].cont[S.loops[d].ncont++] = (uint16_t)(S.fixup_count - 1);
            return;
        }

        case TK_KW_SWITCH: {
            advance();
            EXPECT('(');
            CType tt;
            gen_expr(&tt);
            if (S.err) return;
            EXPECT(')');
            EXPECT('{');
            int32_t val_off = frame_alloc(4);   // the switch value
            int32_t mtc_off = frame_alloc(4);   // match state (-1 = none)
            e_mov_rm_ebp_store(val_off);
            e_mov_ri(R_EAX, 0xFFFFFFFFu);
            e_mov_rm_ebp_store(mtc_off);
            loop_push();
            int d = S.loop_depth - 1;
            S.loops[d].is_switch = 1;
            S.loops[d].ord = 0;
            scope_push();
            while (S.tok != '}' && S.tok != TK_EOF && !S.err) {
                if (S.tok == TK_KW_CASE) {
                    sw_patch_guard(d);          // previous case body ended
                    S.loops[d].ord++;
                    advance();
                    int32_t cv = parse_const();
                    if (S.err) return;
                    EXPECT(':');
                    // value == cv ? match_state = ord : nothing
                    e_mov_r_ebp(R_EAX, val_off);
                    e_mov_ri(R_EBX, (uint32_t)cv);
                    e_cmp_eax_ebx();
                    e_jcc(CC_NZ, -1);
                    uint16_t f1 = (uint16_t)(S.fixup_count - 1);
                    e_mov_ri(R_EAX, (uint32_t)S.loops[d].ord);
                    e_mov_rm_ebp_store(mtc_off);
                    S.fixups[f1].v = (int32_t)S.code_len;
                    // body guard: run iff 0 <= match <= ord
                    e_mov_r_ebp(R_EAX, mtc_off);
                    e_test_eax();
                    e_jcc(CC_L, -1);
                    S.loops[d].guard0 = (uint16_t)(S.fixup_count - 1);
                    e_mov_ri(R_EBX, (uint32_t)S.loops[d].ord);
                    e_cmp_eax_ebx();
                    e_jcc(CC_NLE, -1);
                    S.loops[d].guard1 = (uint16_t)(S.fixup_count - 1);
                    S.loops[d].guard_has = 2;
                } else if (S.tok == TK_KW_DEFAULT) {
                    sw_patch_guard(d);
                    S.loops[d].ord++;
                    advance();
                    EXPECT(':');
                    // body guard: run only when no case matched
                    e_mov_r_ebp(R_EAX, mtc_off);
                    e_test_eax();
                    e_jcc(CC_NL, -1);   // match >= 0 -> skip default body
                    S.loops[d].guard0 = (uint16_t)(S.fixup_count - 1);
                    S.loops[d].guard_has = 1;
                } else {
                    if (S.loops[d].ord == 0) {
                        // before the first label: only declass are legal
                        if (S.tok != TK_KW_INT && S.tok != TK_KW_CHAR &&
                            S.tok != TK_KW_VOID && S.tok != TK_KW_STRUCT &&
                            S.tok != TK_KW_UNION && S.tok != TK_KW_ENUM &&
                            !(S.tok == TK_IDENT && is_typedef_name(S.ident))) {
                            mtcc_error("statement before the first case label in switch");
                            return;
                        }
                    }
                    parse_stmt();
                }
            }
            sw_patch_guard(d);
            EXPECT('}');
            scope_pop();
            loop_pop((uint32_t)S.code_len);   // break targets = after switch
            return;
        }

        case TK_KW_INT:
        case TK_KW_CHAR:
        case TK_KW_VOID:
        case TK_KW_STRUCT:
        case TK_KW_UNION:
        case TK_KW_ENUM: {
            // local declaration mid-block (C99 style)
            CType bt = parse_base_type();
            if (S.tok == ';') { advance(); return; }   // bare `struct S;` decl
            decl_local(bt);
            while (S.tok == ',') { advance(); decl_local(bt); }
            EXPECT(';');
            return;
        }

        case TK_KW_TYPEDEF: { parse_typedef(); return; }

        case TK_KW_CASE:
        case TK_KW_DEFAULT:
            mtcc_error("case/default outside a switch");
            return;

        default: {
            // expression statement, or a declaration via a typedef name
            if (S.tok == TK_IDENT && is_typedef_name(S.ident)) {
                CType bt = parse_base_type();
                decl_local(bt);
                while (S.tok == ',') { advance(); decl_local(bt); }
                EXPECT(';');
                return;
            }
            CType t;
            gen_expr(&t);
            EXPECT(';');
            return;
        }
    }
}

// ============================================================================
//  TOP LEVEL: functions & global variables
// ============================================================================
static int32_t parse_const(void) {
    int neg = 0;
    while (S.tok == '-') { neg = !neg; advance(); }
    if (S.tok == TK_NUM) {
        int32_t v = (int32_t)S.num;
        advance();
        return neg ? -v : v;
    }
    if (S.tok == TK_IDENT) {
        for (uint32_t i = 0; i < g_const_count; i++) {
            if (m_streq(g_consts[i].name, S.ident)) {
                int32_t v = g_consts[i].val;
                advance();
                return neg ? -v : v;
            }
        }
    }
    mtcc_error("initializer must be a numeric constant");
    return 0;
}

static int32_t data_alloc(uint32_t size) {
    data_align4();
    if (S.data_len + size > MTCC_DATA_CAP) { mtcc_error("data area full"); return -1; }
    int32_t off = (int32_t)S.data_len;
    S.data_len += size;
    return off;
}

// ============================================================================
//  STRUCT / UNION INITIALIZER   = { v1, v2, ... }
// ----------------------------------------------------------------------------
// One walker serves both destinations:
//   is_local = 0  → bytes are written straight into the data area at `base`
//                   (GLOBAL variable; every value is a compile-time constant)
//   is_local = 1  → each element becomes a store into [ebp + base]
//                   (LOCAL variable; the caller already frame_alloc'd it)
// Layout follows the struct-body parser exactly: struct fields are 4-aligned,
// union fields all live at offset 0 (C: only the FIRST value is consumed).
// Limitations (documented, with clear errors): char fields are one byte,
// pointers must be constant, and nested structs/arrays need their braces.
// ============================================================================
static void init_scalar(const CType* t, int32_t base, int is_local, int32_t val) {
    if (S.err) return;
    uint32_t sz = ctype_sizeof(t);
    if (sz == 1) {
        if (is_local) { e_mov_ri(R_EAX, (uint32_t)(val & 0xFF)); e_mov_ebp8_al(base); }
        else          { S.data[(uint32_t)base] = (uint8_t)val; }
    } else {
        if (is_local) { e_mov_ri(R_EAX, (uint32_t)val); e_mov_rm_ebp_store(base); }
        else          { w32(S.data, (uint32_t)base, (uint32_t)val); }
    }
}

// `char*` field/element = "literal" → the slot holds the literal's ADDRESS,
// resolved at patch time (same mechanism as `char* s = "lit";`).
static void init_string(const CType* t, int32_t base, int is_local) {
    if (S.err) return;
    uint32_t lit = S.str_off;
    advance();                              // the TK_STR token
    if (is_local) {
        e_mov_eax_data_addr((int32_t)lit);  // mov eax, <address> (ABS_DATA fixup)
        e_mov_rm_ebp_store(base);
    } else {
        if (S.fixup_count >= MTCC_MAX_FIXUPS) { mtcc_error("fixup table full"); return; }
        S.fixups[S.fixup_count].at    = (uint32_t)base;   // offset in DATA
        S.fixups[S.fixup_count].kind  = FX_ABS_IN_DATA;
        S.fixups[S.fixup_count].v     = (int32_t)lit;
        S.fixup_count++;
    }
}

static void init_object(const CType* t, int32_t base, int is_local) {
    if (S.err) return;
    if (t->is_array) {
        // an array slot consumes one value per element (nested struct
        // elements recurse and bring their own braces)
        CType et = *t;
        et.is_array = 0;
        uint32_t esz = (et.base == TY_STRUCT || et.base == TY_UNION)
                           ? ((g_sdefs[et.sdef].size + 3u) & ~3u)
                           : ctype_sizeof(&et);
        EXPECT('{');
        for (uint32_t k = 0; k < t->arr_len && !S.err; k++) {
            if (S.tok == '}') break;                 // fewer values than slots is fine
            if ((et.base == TY_STRUCT || et.base == TY_UNION) && et.ptr == 0)
                init_object(&et, base + (int32_t)(k * esz), is_local);
            else if (S.tok == TK_STR && et.ptr == 1 && et.base == TY_CHAR)
                init_string(&et, base + (int32_t)(k * esz), is_local);
            else {
                int32_t v = parse_const();
                if (S.err) return;
                init_scalar(&et, base + (int32_t)(k * esz), is_local, v);
            }
            if (S.tok == ',') { advance(); continue; }
            break;
        }
        if (S.tok == ',') advance();
        EXPECT('}');
        return;
    }
    if (t->ptr != 0 || (t->base != TY_STRUCT && t->base != TY_UNION)) {
        mtcc_error("not a struct/union (no {...} initializer here)");
        return;
    }
    if (t->sdef >= MTCC_MAX_SDEF) { mtcc_error("incomplete struct type"); return; }
    const struct SDef* sd = &g_sdefs[t->sdef];

    EXPECT('{');
    uint32_t fi = 0;
    while (!S.err && S.tok != '}' && S.tok != TK_EOF) {
        if (fi >= sd->field_count) { mtcc_error("too many initializers for the struct"); return; }
        const struct Field* fd = &g_field_pool[sd->field_off + fi];
        int32_t fbase = base + (int32_t)fd->offset;
        if (fd->type.is_array) {
            init_object(&fd->type, fbase, is_local);   // consumes its own braces
        } else if ((fd->type.base == TY_STRUCT || fd->type.base == TY_UNION) &&
                   fd->type.ptr == 0) {
            init_object(&fd->type, fbase, is_local);   // nested struct: { {...}, ... }
        } else if (S.tok == TK_STR && fd->type.ptr == 1 && fd->type.base == TY_CHAR) {
            init_string(&fd->type, fbase, is_local);       // char* f = "lit";
        } else {
            int32_t v = parse_const();
            if (S.err) return;
            init_scalar(&fd->type, fbase, is_local, v);
        }
        fi++;
        if (sd->is_union) break;      // C: only the first member is initialized
        if (S.tok == ',') { advance(); continue; }
        break;
    }
    if (S.tok == ',') advance();      // trailing comma
    EXPECT('}');
}

static void parse_gvar_one(CType bt, const char* name) {
    if (bt.base == TY_VOID && bt.ptr == 0) { mtcc_error("void global variable"); return; }
    if (find_func(name)) { mtcc_error("the global name is taken by a function"); return; }
    // Definition over an earlier `extern` declaration? Then this is the
    // storage-defining occurrence; borrow the same GVar entry and patch its
    // deferred fixups with the real offset (see below).
    GVar* prev_ext = NULL;
    for (uint32_t i = 0; i < S.gvar_count; i++) {
        if (m_streq(S.gvars[i].name, name)) {
            if (S.gvars[i].is_extern) { prev_ext = &S.gvars[i]; break; }
            mtcc_error("duplicate global"); return;
        }
    }
    if (S.gvar_count >= MTCC_MAX_GVARS && !prev_ext) { mtcc_error("too many globals (max 128)"); return; }
    CType vt = bt;
    int32_t off = -1;

    if (S.tok == '[') {
        advance();
        uint32_t al = 0;
        if (!parse_array_size(&al)) { mtcc_error("array size must be a number/constant"); return; }
        if (al == 0 || al > 4096) { mtcc_error("unreasonable array size"); return; }
        vt.is_array = 1;
        vt.arr_len = al;
        if (vt.ptr != 0) { mtcc_error("array of pointers not yet supported"); return; }
        EXPECT(']');
        off = data_alloc(ctype_alloc_size(&vt));
        if (off < 0) return;
        if (S.tok == '=') {
            advance();
            if (vt.base == TY_STRUCT || vt.base == TY_UNION) {
                // array of struct/union — `{{...},{...}}` (or `{...}` with
                // nested braces per element), all constants.
                init_object(&vt, off, 0);
            } else if (S.tok == TK_STR) {
                if (vt.base != TY_CHAR) { mtcc_error("string initializer only for char[]"); return; }
                uint32_t lit = S.str_off;
                uint32_t n = 0;
                while (S.data[lit + n] && n < vt.arr_len) n++;
                if (n + 1 > vt.arr_len) { mtcc_error("string too long for the array"); return; }
                // the literal area is ALWAYS allocated before this slot →
                // off > lit, so a forward copy is safe without overlap.
                for (uint32_t i = 0; i < n; i++) S.data[off + i] = S.data[lit + i];
                S.data[off + n] = 0;   // the rest is already 0 (zero-init data area)
                advance();
            } else if (S.tok == '{') {
                if (vt.base == TY_CHAR) { mtcc_error("{...} initializer only for int[]"); return; }
                advance();
                uint32_t cnt = 0;
                if (S.tok != '}') {
                    for (;;) {
                        if (cnt >= vt.arr_len) { mtcc_error("initializer larger than the array"); return; }
                        int32_t v = parse_const();
                        if (S.err) return;
                        w32(S.data, (uint32_t)off + cnt * 4, (uint32_t)v);
                        cnt++;
                        if (S.tok == ',') { advance(); continue; }
                        break;
                    }
                }
                EXPECT('}');
            } else {
                mtcc_error("array initializer must be a string or {...}");
                return;
            }
        }
    } else {
        // scalar / pointer — a 4-byte slot (even a global char is 4 bytes
        // for tidy alignment; access stays 1 byte via movzx).
        // Structs/unions need their real size.
        off = data_alloc((vt.base == TY_STRUCT || vt.base == TY_UNION)
                             ? ctype_alloc_size(&vt) : 4);
        if (off < 0) return;
        if (S.tok == '=') {
            advance();
            if (S.tok == TK_STR && vt.ptr == 1 && vt.base == TY_CHAR) {
                // char* s = "literal"; → the slot gets the literal's address at patch time
                if (S.fixup_count >= MTCC_MAX_FIXUPS) { mtcc_error("fixup table full"); return; }
                S.fixups[S.fixup_count].at = (uint32_t)off;   // offset in DATA
                S.fixups[S.fixup_count].kind = FX_ABS_IN_DATA;
                S.fixups[S.fixup_count].v = (int32_t)S.str_off;
                S.fixup_count++;
                advance();
            } else if ((vt.base == TY_STRUCT || vt.base == TY_UNION) && vt.ptr == 0) {
                // GLOBAL struct/union initializer — the bytes are constants,
                // so they go straight into the data area at `off`.
                init_object(&vt, off, 0);
            } else {
                int32_t v = parse_const();
                if (S.err) return;
                w32(S.data, (uint32_t)off, (uint32_t)v);
            }
        }
    }

    GVar* g;
    if (prev_ext) {
        // The extern's declaration is replaced by the authoritative def;
        // patch all deferred references with the real data offset.
        g = prev_ext;
        g->type = vt;
        g->is_extern = 0;
        g->data_off = off;
        for (uint32_t pi = 0; pi < g->pend_n; pi++)
            S.fixups[g->pendfx[pi]].v = off;
        g->pend_n = 0;
        return;
    }
    g = &S.gvars[S.gvar_count++];
    // FIX (2nd+ in-OS run bug): write the NUL terminator EXPLICITLY.
    // The gvar table is allocated from the .mrp arena that is REUSED
    // across mtcc runs and never zeroed by the allocator. Without this
    // NUL, a new name sticks to an old name's tail ("sieve" + leftover
    // "ing" from a previous compile's "greeting" = "sieveing") -> lookup
    // fails with "unknown identifier" only on the 2nd+ run.
    uint32_t nlen = 0;
    while (nlen < MTCC_NAME_MAX - 1 && name[nlen]) {
        g->name[nlen] = name[nlen]; nlen++;
    }
    g->name[nlen] = '\0';
    g->type = vt;
    g->data_off = off;
}

// ----------------------------------------------------------------------------
//  parse_function — a prototype OR a full definition.
//  mtcc's internal call convention (see the file-top comment): arguments
//  are pushed left→right, param i at [ebp + 8 + 4*(n-1-i)].
// ----------------------------------------------------------------------------
static void parse_function(CType ret, const char* name) {
    // S.tok == '(' on entry
    Func* f = find_func(name);
    // REDECLARATION of an already-defined function (very common once a
    // program spans several .c files: the header's prototype is seen again
    // after the body). Parse the header into a scratch Func so the real
    // record is never clobbered, then accept `;` silently and reject only
    // a second BODY. (Parameter lists are not cross-checked for
    // compatibility — this compiler stays lenient there.)
    Func redecl;
    int already_defined = 0;
    if (!f) {
        if (S.func_count >= MTCC_MAX_FUNCS) { mtcc_error("too many functions (max 128)"); return; }
        f = &S.funcs[S.func_count++];
        m_memset((uint8_t*)f, 0, (uint32_t)sizeof(Func));
        for (uint32_t i = 0; i < MTCC_NAME_MAX && name[i]; i++) f->name[i] = name[i];
        f->code_off = -1;      // no body yet
    } else if (f->defined) {
        m_memset((uint8_t*)&redecl, 0, (uint32_t)sizeof(redecl));
        for (uint32_t i = 0; i < MTCC_NAME_MAX && name[i]; i++) redecl.name[i] = name[i];
        redecl.code_off = -1;
        f = &redecl;
        already_defined = 1;
    }
    f->ret = ret;
    if ((ret.base == TY_STRUCT || ret.base == TY_UNION) && ret.ptr == 0) {
        mtcc_error("struct/union return by value not supported (use a pointer)"); return;
    }

    advance();                 // '('
    f->nparams = 0;
    if (S.tok == TK_KW_VOID) {
        advance();
        if (S.tok != ')') { mtcc_error("(void) takes no parameters"); return; }
    } else if (S.tok != ')') {
        for (;;) {
            if (f->nparams >= MTCC_MAX_PARAMS) { mtcc_error("too many parameters (max 8)"); return; }
            CType pt = parse_base_type();
            if (pt.base == TY_VOID && pt.ptr == 0) { mtcc_error("void parameter is not valid"); return; }
            if ((pt.base == TY_STRUCT || pt.base == TY_UNION) && pt.ptr == 0) {
                mtcc_error("struct/union parameter by value not supported (use a pointer)"); return;
            }
            if (S.tok != TK_IDENT) { mtcc_error("expected a parameter name"); return; }
            for (uint32_t i = 0; i < MTCC_NAME_MAX && S.ident[i]; i++)
                f->pnames[f->nparams][i] = S.ident[i];
            advance();
            if (S.tok == '[') { mtcc_error("array parameter not yet supported (use a pointer)"); return; }
            // v10.8: default parameter value `= const` (must be a numeric
            // constant — a printf(fmt, a=0, b=0, c=0) C++-style extension)
            if (S.tok == '=') {
                advance();
                f->pdef_has[f->nparams] = 1;
                f->pdef[f->nparams]     = parse_const();
                if (S.err) return;
            }
            f->ptypes[f->nparams] = pt;
            f->nparams++;
            if (S.tok == ',') { advance(); continue; }
            break;
        }
    }
    if (S.tok != ')') { mtcc_error("expected ')' to close the parameter list"); return; }
    advance();

    if (S.tok == ';') { advance(); return; }      // prototype — done
    if (S.tok != '{') { mtcc_error("expected '{' or ';' after the function header"); return; }
    if (already_defined) { mtcc_error("function defined twice"); return; }   // second BODY

    // ================= DEFINITION =================
    f->code_off = (int32_t)S.code_len;
    f->defined = 1;
    // forward calls to this function → patch now (the target is known)
    for (uint32_t i = 0; i < f->pending_count; i++)
        S.fixups[f->pending[i]].v = f->code_off;

    e_push(R_EBP);                       // 55
    e_mov_rr(R_EBP, R_ESP);              // 89 E5  (mov ebp, esp)
    e_sub_esp_i32(0);                    // 81 EC imm32 — the operand is patched later
    S.frame_sub_at = S.code_len - 4;     // position of the imm32 OPERAND (not the opcode!)
    S.frame_off = 0;
    S.cur_func = f;

    // parameters = scope-0 locals with positive ebp
    S.local_count = 0;
    S.scope_depth = 0;
    for (uint32_t i = 0; i < f->nparams; i++) {
        if (S.local_count >= MTCC_MAX_LOCALS) { mtcc_error("too many locals"); return; }
        LVar* lv = &S.locals[S.local_count++];
        for (uint32_t k = 0; k < MTCC_NAME_MAX; k++) lv->name[k] = f->pnames[i][k];
        lv->type = f->ptypes[i];
        lv->ebp_off = 8 + 4 * ((int32_t)f->nparams - 1 - (int32_t)i);
    }

    parse_stmt();                        // '{' ... '}' (scope body = depth 1)

    e_epilogue();                        // fallback if control falls off the end

    uint32_t fsz = (uint32_t)(-S.frame_off);
    w32(S.code, S.frame_sub_at, fsz);    // patch `sub esp, frame_size`
    S.cur_func = NULL;
    S.local_count = 0;
    S.scope_depth = 0;
}

static void parse_program(void) {
    while (!S.err && S.tok != TK_EOF) {
        if (S.tok == TK_KW_TYPEDEF) { parse_typedef(); continue; }
        if (S.tok == TK_KW_EXTERN) {
            advance();
            CType ebt = parse_base_type();
            if (S.err) return;
            for (;;) {
                if (S.tok != TK_IDENT) { mtcc_error("expected a global name after extern"); return; }
                char en[MTCC_NAME_MAX];
                uint32_t i = 0;
                while (S.ident[i] && i < MTCC_NAME_MAX - 1) { en[i] = S.ident[i]; i++; }
                en[i] = '\0';
                advance();
                CType vt = ebt;
                if (S.tok == '[') {
                    advance();
                    uint32_t al = 0;
                    if (!parse_array_size(&al) || al == 0 || al > 4096) {
                        mtcc_error("array size must be a positive number/constant"); return;
                    }
                    vt.is_array = 1; vt.arr_len = al;
                    EXPECT(']');
                }
                if (S.tok == '=') { mtcc_error("extern must not have an initializer"); return; }
                // register: allow a prior extern decl (re-decl) and silence a
                // definition that was parsed ALREADY (extern-after-def).
                {
                    GVar* prev = find_gvar(en);
                    if (!prev) {
                        if (S.gvar_count >= MTCC_MAX_GVARS) { mtcc_error("too many globals (max 128)"); return; }
                        GVar* g = &S.gvars[S.gvar_count++];
                        m_memset((uint8_t*)g, 0, sizeof(*g));
                        uint32_t k = 0;
                        while (en[k] && k < MTCC_NAME_MAX - 1) { g->name[k] = en[k]; k++; }
                        g->name[k] = '\0';
                        g->type = vt;
                        g->data_off = -1;
                        g->is_extern = 1;
                    }
                }
                if (S.tok == ',') { advance(); continue; }
                break;
            }
            EXPECT(';');
            continue;
        }
        if (S.tok != TK_KW_INT && S.tok != TK_KW_CHAR && S.tok != TK_KW_VOID &&
            S.tok != TK_KW_STRUCT && S.tok != TK_KW_UNION && S.tok != TK_KW_ENUM &&
            !(S.tok == TK_IDENT && is_typedef_name(S.ident))) {
            mtcc_error("expected a declaration (type) at file top level");
            return;
        }
        CType bt = parse_base_type();
        if (S.tok == ';') { advance(); continue; }   // struct Tag; / enum E;
        if (S.tok != TK_IDENT) { mtcc_error("expected a function/global-variable name"); return; }
        char name[MTCC_NAME_MAX];
        for (uint32_t i = 0; i < MTCC_NAME_MAX; i++) name[i] = S.ident[i];
        advance();

        if (S.tok == '(') {
            parse_function(bt, name);
        } else {
            parse_gvar_one(bt, name);
            while (S.tok == ',' && !S.err) {
                advance();
                if (S.tok != TK_IDENT) { mtcc_error("expected a variable name"); return; }
                char n2[MTCC_NAME_MAX];
                for (uint32_t i = 0; i < MTCC_NAME_MAX; i++) n2[i] = S.ident[i];
                advance();
                parse_gvar_one(bt, n2);
            }
            EXPECT(';');
        }
    }
}

// ============================================================================
//  DRIVER COMPILE
// ============================================================================
struct MtccOut {
    uint8_t* code; uint32_t code_len;
    uint8_t* data; uint32_t data_len;
    uint32_t nfuncs; uint32_t ngvars;
};

// v0.6 -make: pool buffer persisten — diademasi SEKALI lalu dipakai ulang
// oleh semua job dalam satu proses `mtcc -make`. Ini menghindari
// fragmentasi arena 4MB (mrp_alloc tanpa free-list yang bisa dipakai
// untuk blok besar berulang). Kompiler memang sudah di-hardening untuk
// arena stale/reuse: gvars + data di-nol-kan tiap compile, NUL eksplisit
// pada nama gvar, dan host os_alloc sengaja mengisi 0xA5 agar bug
// ketergantungan memori bersih langsung ketahuan.
static uint8_t* g_pool_code   = NULL;
static uint8_t* g_pool_data   = NULL;
static Func*    g_pool_funcs  = NULL;
static LVar*    g_pool_locals = NULL;
static GVar*    g_pool_gvars  = NULL;
static Fixup*   g_pool_fixups = NULL;
static uint8_t* g_pool_image  = NULL;

// ============================================================================
//  Multi-file (single-program) compilation: a program can be made of several
//  .c files sharing ONE global symbol table (functions / globals / enums /
//  typedefs / sdefs) and one fixup list — i.e. a real link step, done by the
//  compiler itself. mtcc emits no .o files.
//  Pragmatic semantic choices (documented in TCC.md):
//   - PP macros persist across the file list, so `#include` GUARDS work the
//     usual intended way: the first file's #include <morph.h> wins; every
//     other file's <morph.h> include is suppressed by the guard instead of
//     redefining the libc bodies. Forward references across files still
//     need a prototype (standard C discipline).
//   - Cross-file call targets (function used in file A, defined in file B)
//     get forward fixups — same mechanism as forward calls within a file.
// ============================================================================
static uint16_t g_main_fx = 0;   // fixup slot of the entry stub's `call main`

static int mtcc_compile_start(void) {
    m_memset((uint8_t*)&S, 0, (uint32_t)sizeof(S));
    g_prim_kind = 0;
    g_prim_func = NULL;
    g_prim_builtin = NULL;
    // macros persist across mtcc_compile_add calls (see the comment above):
    // only the counter + table are reset here, per-program.
    m_memset((uint8_t*)&PP, 0, (uint32_t)sizeof(PP));

    // v0.6: pakai pool persisten (alokasi sekali per proses) — lihat
    // komentar di atas; pada mode normal (satu compile per run) perilaku
    // sama seperti dulu, proses kelihats selesai sebelum pool berperan.
    if (!g_pool_code)   g_pool_code   = (uint8_t*)os_alloc(MTCC_CODE_CAP);
    if (!g_pool_data)   g_pool_data   = (uint8_t*)os_alloc(MTCC_DATA_CAP);
    if (!g_pool_funcs)  g_pool_funcs  = (Func*)os_alloc((uint32_t)sizeof(Func) * MTCC_MAX_FUNCS);
    if (!g_pool_locals) g_pool_locals = (LVar*)os_alloc((uint32_t)sizeof(LVar) * MTCC_MAX_LOCALS);
    if (!g_pool_gvars)  g_pool_gvars  = (GVar*)os_alloc((uint32_t)sizeof(GVar) * MTCC_MAX_GVARS);
    if (!g_pool_fixups) g_pool_fixups = (Fixup*)os_alloc((uint32_t)sizeof(Fixup) * MTCC_MAX_FIXUPS);
    S.code   = g_pool_code;
    S.data   = g_pool_data;
    S.funcs  = g_pool_funcs;
    S.locals = g_pool_locals;
    S.gvars  = g_pool_gvars;
    S.fixups = g_pool_fixups;
    if (!S.code || !S.data || !S.funcs || !S.locals || !S.gvars || !S.fixups) {
        return 2;   // OOM — the caller prints the message
    }
    m_memset(S.data, 0, MTCC_DATA_CAP);   // global zero-init (C semantics)
    m_memset((uint8_t*)S.gvars, 0, (uint32_t)sizeof(GVar) * MTCC_MAX_GVARS);

    // The entry stub at offset 0 — this is what the .mrp loader calls:
    //   push ebp; mov ebp,esp; call main; pop ebp; ret
    // --lib mode (library check-compile): no main() exists, so the
    // stub is a bare `ret` — the .mrp is a build artifact, never run.
    e_push(R_EBP);
    e_mov_rr(R_EBP, R_ESP);
    if (!g_lib_mode) {
        e_call(-1);
        g_main_fx = (uint16_t)(S.fixup_count - 1);
    }
    e_pop(R_EBP);
    emit8(0xC3);                          // ret
    return 0;
}

static int mtcc_compile_add(const char* src, uint32_t src_len, const char* path) {
    uint32_t pp_len = 0;
    // remember WHICH file is being compiled FIRST — diagnostics from the
    // preprocessor onward quote its name (`  [ERROR] path:12: message`).
    {
        uint32_t k = 0;
        const char* pp = path ? path : "";
        while (pp[k] && k < sizeof(S.err_file) - 1) { S.err_file[k] = pp[k]; k++; }
        S.err_file[k] = '\0';
    }
    pp_push_dirname(path ? path : "");
    const char* pp_src = pp_run(src, src_len, &pp_len);
    pp_pop_dirname();
    if (!pp_src) {
        S.err = 1;
        S.err_line = PP.line ? PP.line : 1;
        uint32_t k = 0;
        while (PP.err_msg[k] && k < sizeof(S.err_msg) - 1) {
            S.err_msg[k] = PP.err_msg[k]; k++;
        }
        S.err_msg[k] = '\0';
        return 1;
    }
    S.src = pp_src;
    S.src_len = pp_len;
    S.line = 1;
    // FIX A (multi-file): the lexer offset is PER SOURCE FILE. Without this
    // reset file #2 starts tokenising at file #1's end offset — either in
    // the middle of the text ("expected a declaration") or past the end
    // (TK_EOF → parse_program never runs → the file's functions are never
    // recorded → bogus "undefined reference" at finish).
    S.src_pos = 0;
    lex_next();
    parse_program();
    return S.err ? 1 : 0;
}

static int mtcc_compile_finish(MtccOut* out) {
    if (!S.err && !g_lib_mode) {
        Func* mf = find_func("main");
        if (!mf || !mf->defined) mtcc_error("function main() not found");
        else S.fixups[g_main_fx].v = mf->code_off;
    }

    // ---- v0.3 FR-20: undefined-reference detection (linker-style) ----
    if (!S.err) {
        for (uint32_t fi = 0; fi < S.func_count && !S.err; fi++) {
            Func* f = &S.funcs[fi];
            if (!f->defined && f->pending_count > 0) {
                char msg[96];
                uint32_t k = 0;
                const char* pre = "undefined reference to '";
                while (pre[k] && k < sizeof(msg) - 32) { msg[k] = pre[k]; k++; }
                uint32_t j = 0;
                while (f->name[j] && k < sizeof(msg) - 32) {
                    msg[k] = f->name[j]; k++; j++;
                }
                msg[k++] = '\'';
                msg[k++] = ' ';
                msg[k++] = '(';
                msg[k++] = 'c';
                msg[k++] = 'a';
                msg[k++] = 'l';
                msg[k++] = 'l';
                msg[k++] = 'e';
                msg[k++] = 'd';
                msg[k++] = ' ';
                msg[k++] = 'b';
                msg[k++] = 'u';
                msg[k++] = 't';
                msg[k++] = ' ';
                msg[k++] = 'n';
                msg[k++] = 'e';
                msg[k++] = 'v';
                msg[k++] = 'e';
                msg[k++] = 'r';
                msg[k++] = ' ';
                msg[k++] = 'd';
                msg[k++] = 'e';
                msg[k++] = 'f';
                msg[k++] = 'i';
                msg[k++] = 'n';
                msg[k++] = 'e';
                msg[k++] = 'd';
                msg[k++] = ')';
                msg[k] = '\0';
                mtcc_error(msg);
                if (f->first_call_line) S.err_line = f->first_call_line;
            }
        }
    }
    if (S.err) return 1;

    // ---- multi-file LINK error: extern global touched, never defined ----
    for (uint32_t i = 0; i < S.gvar_count && !S.err; i++) {
        GVar* g = &S.gvars[i];
        if (g->is_extern && g->accessed) {
            char msg[96];
            uint32_t k = 0;
            const char* pre = "extern '";
            while (pre[k] && k < sizeof(msg) - 32) { msg[k] = pre[k]; k++; }
            uint32_t j = 0;
            while (g->name[j] && k < sizeof(msg) - 32) { msg[k] = g->name[j]; k++; j++; }
            const char* post = "' is used but never defined";
            j = 0;
            while (post[j] && k < sizeof(msg) - 1) { msg[k] = post[j]; k++; j++; }
            msg[k] = '\0';
            mtcc_error(msg);
        }
    }
    if (S.err) return 1;

    out->code = S.code; out->code_len = S.code_len;
    out->data = S.data; out->data_len = S.data_len;
    out->nfuncs = S.func_count; out->ngvars = S.gvar_count;
    return 0;
}

static int mtcc_compile(const char* src, uint32_t src_len, MtccOut* out, const char* path) {
    int rc = mtcc_compile_start();
    if (rc == 2) return 2;
    if (mtcc_compile_add(src, src_len, path) != 0) return 1;
    return mtcc_compile_finish(out);
}

// Patch all fixups with the final base. Two uses:
//   RUN mode : code_base/data_base = the real buffer addresses (MRP arena).
//   mode -c  : code_base = 0x500010 (MRP_LOAD_BASE), data_base continues
//              after the code — exactly the assembled .mrp image layout.
// CAREFUL: patching MUTATES the code buffer (one-way, one-shot).
static void mtcc_patch(const MtccOut* out, uint32_t code_base, uint32_t data_base) {
    (void)code_base;   // FX_REL32 is relative — no base needed (intentional)
    for (uint32_t i = 0; i < S.fixup_count; i++) {
        const Fixup* f = &S.fixups[i];
        if (f->kind == FX_REL32) {
            w32(out->code, f->at, (uint32_t)(f->v - (int32_t)(f->at + 4)));
        } else if (f->kind == FX_ABS_DATA) {
            w32(out->code, f->at, data_base + (uint32_t)f->v);
        } else {   // FX_ABS_IN_DATA — writes into the data area (char* global = "...")
            w32(out->data, f->at, data_base + (uint32_t)f->v);
        }
    }
}

// Assemble the .mrp image: [18-byte header][code][4-alignment pad][data].
// Returns the total size, 0 on failure. Header & checksum use the
// KERNEL's mrp_format.h (single source of truth — no drift possible).
static uint32_t mtcc_build_image(const MtccOut* out, uint8_t* image, uint32_t image_cap) {
    uint32_t pad = (4 - (out->code_len & 3)) & 3;
    uint32_t payload = out->code_len + pad + out->data_len;
    uint32_t total = MRP_HEADER_SIZE + payload;
    if (total > image_cap || payload == 0) return 0;

    uint32_t code_base = 0x500010;                       // MRP_LOAD_BASE
    uint32_t data_base = code_base + out->code_len + pad;
    mtcc_patch(out, code_base, data_base);

    m_memcpy(image + MRP_HEADER_SIZE, out->code, out->code_len);
    for (uint32_t i = 0; i < pad; i++) image[MRP_HEADER_SIZE + out->code_len + i] = 0;
    m_memcpy(image + MRP_HEADER_SIZE + out->code_len + pad, out->data, out->data_len);

    struct mrp_header h;
    h.magic[0] = MRP_MAGIC0; h.magic[1] = MRP_MAGIC1;
    h.magic[2] = MRP_MAGIC2; h.magic[3] = MRP_MAGIC3;
    h.version = MRP_VERSION;
    h.entry_offset = 0;          // the entry stub is at offset 0 by design
    h.code_size = payload;       // the loader copies code+data in one go
    h.flags = MRP_FLAG_NONE;
    h.checksum = mrp_checksum(image + MRP_HEADER_SIZE, payload);
    m_memcpy(image, (const uint8_t*)&h, MRP_HEADER_SIZE);
    return total;
}

// ============================================================================
//  ELF32 OUTPUT  —  `mtcc -c prog.c -format elf`
// ----------------------------------------------------------------------------
//  The kernel's ELF loader (kernel/library/elf.cpp) accepts exactly ONE
//  shape: ELF32, little-endian, ET_EXEC, EM_386, no relocations, every
//  p_vaddr inside [ELF_IMG_MIN=0x00800000, ELF_IMG_MAX=0x02000000), entry
//  inside the same window. It copies p_filesz bytes from the file straight
//  to p_vaddr and zero-fills ..p_memsz (.bss), then jumps to e_entry.
//
//  So mtcc emits a single PT_LOAD segment laid out as
//
//      [Ehdr 52][Phdr 32][code][pad][data]      file offset 0
//                ^ p_offset = 84                p_vaddr = ELF_BASE
//
//  and patches its fixups with data_base = ELF_BASE + code + pad — the
//  same "absolute, linked at a fixed address" model as the .mrp image
//  (link base 0x500010) and as elfdemo.elf (link base 0x01000000), which
//  is what `mrp_user/elf_link.ld` uses for host-built static ELF32 too.
//
//  p_flags = 7 (RWX): the loader ignores p_flags today (every page comes
//  in RW anyway), and signalling PF_X keeps the file honest for readelf.
// ============================================================================
#define ELF_BASE      0x01000000u   /* inside [0x00800000, 0x02000000) */
#define ELF_EHSIZE    52
#define ELF_PHENTSIZE 32
#define ELF_PHOFF     52
#define ELF_PAYLOAD   (ELF_PHOFF + ELF_PHENTSIZE)   /* 84 */
#define ELF_PHNUM     1
#define ELF_ALIGN     0x1000u

static void elf_w16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void elf_w32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Build a ready-to-run ELF32 image. Returns the file size, 0 on failure.
 * FIXUP CONTRACT: mtcc_patch() mutates the code/data buffers one-shot, so
 * this may only be called ONCE per compilation (same as mtcc_build_image). */
static uint32_t mtcc_build_image_elf(const MtccOut* out, uint8_t* image,
                                     uint32_t image_cap) {
    uint32_t pad = (4 - (out->code_len & 3)) & 3;
    uint32_t payload = out->code_len + pad + out->data_len;
    uint32_t total = ELF_PAYLOAD + payload;
    if (payload == 0 || total > image_cap) return 0;

    uint32_t code_base = ELF_BASE;                    /* entry == segment start */
    uint32_t data_base = ELF_BASE + out->code_len + pad;
    mtcc_patch(out, code_base, data_base);

    for (uint32_t i = 0; i < total; i++) image[i] = 0;

    /* ---- ELF header (52 bytes) ---- */
    image[0] = 0x7F; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1;      /* EI_CLASS   ELFCLASS32 */
    image[5] = 1;      /* EI_DATA    ELFDATA2LSB */
    image[6] = 1;      /* EI_VERSION */
    image[7] = 0;      /* EI_OSABI   SYSV */
    /* e_ident[8..15] already zero (pad + abiversion) */
    elf_w16(image + 16, 2);          /* e_type      ET_EXEC   (loader rejects others) */
    elf_w16(image + 18, 3);          /* e_machine   EM_386    */
    elf_w32(image + 20, 1);          /* e_version   */
    elf_w32(image + 24, code_base);  /* e_entry     the stub at offset 0 */
    elf_w32(image + 28, ELF_PHOFF);  /* e_phoff     */
    elf_w32(image + 32, 0);          /* e_shoff     no section headers (loader: unused) */
    elf_w32(image + 36, 0);          /* e_flags     */
    elf_w16(image + 40, ELF_EHSIZE);
    elf_w16(image + 42, ELF_PHENTSIZE);
    elf_w16(image + 44, ELF_PHNUM);
    elf_w16(image + 46, 0);          /* e_shentsize */
    elf_w16(image + 48, 0);          /* e_shnum     */
    elf_w16(image + 50, 0);          /* e_shstrndx  */

    /* ---- program header (32 bytes) ---- */
    uint8_t* ph = image + ELF_PHOFF;
    elf_w32(ph + 0, 1);              /* p_type   PT_LOAD */
    elf_w32(ph + 4, ELF_PAYLOAD);    /* p_offset payload starts after ehdr+phdr */
    elf_w32(ph + 8, code_base);      /* p_vaddr  fixed — loader does no reloc */
    elf_w32(ph + 12, code_base);     /* p_paddr  */
    elf_w32(ph + 16, payload);       /* p_filesz */
    elf_w32(ph + 20, payload);       /* p_memsz  (no .bss — mtcc pads data to 4) */
    elf_w32(ph + 24, 7);             /* p_flags  RWX */
    elf_w32(ph + 28, ELF_ALIGN);     /* p_align  */

    /* ---- payload ---- */
    m_memcpy(image + ELF_PAYLOAD, out->code, out->code_len);
    for (uint32_t i = 0; i < pad; i++) image[ELF_PAYLOAD + out->code_len + i] = 0;
    m_memcpy(image + ELF_PAYLOAD + out->code_len + pad, out->data, out->data_len);
    return total;
}

/* Mirror of the kernel's elf_check() so a bad image is caught HERE with a
 * message instead of at boot/run time inside the OS. 0 = ok. */
static int mtcc_elf_check(const uint8_t* b, uint32_t len) {
    if (len < 20 || b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F')
        return 1;                                  /* "not ELF32" */
    if (b[4] != 1 || b[5] != 1) return 2;          /* class / endianness */
    if (b[16] != 2 || b[18] != 3) return 3;        /* ET_EXEC / EM_386 */
    /* NOTE: ELF is LITTLE-endian — read every field low byte first. */
    uint32_t phoff  = (uint32_t)b[28] | ((uint32_t)b[29] << 8) |
                      ((uint32_t)b[30] << 16) | ((uint32_t)b[31] << 24);
    uint32_t entry  = (uint32_t)b[24] | ((uint32_t)b[25] << 8) |
                      ((uint32_t)b[26] << 16) | ((uint32_t)b[27] << 24);
    uint16_t phnum  = (uint16_t)(b[44] | (b[45] << 8));
    uint16_t phents = (uint16_t)(b[42] | (b[43] << 8));
    if (phnum < 1 || phnum > 16 || phents < 32) return 4;
    if (phoff + (uint32_t)phnum * phents > len) return 5;
    if (entry < 0x00800000u || entry >= 0x02000000u) return 6;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t* ph = b + phoff + i * phents;
        uint32_t t   = (uint32_t)ph[0] | ((uint32_t)ph[1] << 8) |
                       ((uint32_t)ph[2] << 16) | ((uint32_t)ph[3] << 24);
        uint32_t off = (uint32_t)ph[4] | ((uint32_t)ph[5] << 8) |
                       ((uint32_t)ph[6] << 16) | ((uint32_t)ph[7] << 24);
        uint32_t va  = (uint32_t)ph[8] | ((uint32_t)ph[9] << 8) |
                       ((uint32_t)ph[10] << 16) | ((uint32_t)ph[11] << 24);
        uint32_t fsz = (uint32_t)ph[16] | ((uint32_t)ph[17] << 8) |
                       ((uint32_t)ph[18] << 16) | ((uint32_t)ph[19] << 24);
        uint32_t msz = (uint32_t)ph[20] | ((uint32_t)ph[21] << 8) |
                       ((uint32_t)ph[22] << 16) | ((uint32_t)ph[23] << 24);
        if (t != 1) continue;
        if (fsz > msz) return 7;                       /* filesz > memsz */
        if (off > len || fsz > len - off) return 8;    /* payload past EOF */
        if ((va & ~0xFFFu) < 0x00800000u ||
            (va + msz) > 0x02000000u) return 9;        /* outside the image window */
    }
    return 0;
}

// ============================================================================
//  MAKE MINI (v0.6) — `mtcc -make <file.ruf>`
// ----------------------------------------------------------------------------
//  Build file flat untuk self-hosting TANPA equinoxinstall: SATU proses
//  mtcc mengompilasi banyak .c secara berurutan. Buffer besar (kode/data/
//  tabel/preprocessor/gambar .mrp) di-free antar-job; cache prelude &
//  include (fbuf/mbuf/prelude_buf) sengaja dipertahankan sekali per
//  proses — alokasi jadi O(1) setelah job pertama.
//
//  Sintaks .ruf — per baris, '#' = komentar, CRLF oke. Bentuk "key value",
//  "key:=value", dan "key = value" semuanya sah:
//    echo <teks>       cetak baris log saat parse (quotes di-strip)
//    src <dir>         direktori sumber, boleh berulang; baris telanjang
//                      berupa path (mis. "equinox/") dibaca sebagai src
//    exclude <nama>    lewati entri bernama ini (dir ATAU file .c, semua level)
//    out <dir>         tulis semua .mrp ke dir ini (default: di samping .c)
//    lib               semua job memakai mode --lib; file di bawah dir
//                      bernama "libc" otomatis --lib apa pun kondisinya
//  v0.9.3 — ala Makefile:
//    name := "nilai"   variabel (lookup $nama CASE-INSENSITIVE)
//    $nama             ekspansi di SEMUA nilai direktif
//    copy A [& B] ? D  salin (file/dir rekursif) ke D — dieksekusi
//    move A [& B] ? D  SETELAH semua job sukses (artefakt rusak
//                      tidak pernah ikut tersebar); '?' = "to"
//  v4 — multi-file + format + konfigurasi (SEMUA opsional, resep v3
//       tetap jalan persis seperti dulu):
//    multiple_file = True   semua sumber `src` di-link jadi SATU program
//                           (nama dari variabel `name`, folder dari `out`)
//    src a.c, b.c & d.c     daftar file eksplisit (token ber-akhir .c);
//                           selain itu tetap direktori (walk) seperti v3
//    format elf|mrp         format keluaran global (default mrp)
//    job <nama> from <sumber [& sumber…]> to <keluaran>
//                           SATU baris = N sumber -> SATU keluaran;
//                           opsional setelah <keluaran>:
//                             format mrp|elf   timpa format global
//                             flags "-q --lib" flag per job
//                             lib              mode --lib untuk job ini
//    set <key> = <nilai>    tulis key ke file .ecf — HANYA setelah build
//                           sukses (bersama copy/move), baca-modify-tulis:
//                           key lain, section dan komentar tidak tersentuh
//    ecf <path>             target `set` berikutnya (default: store aktif,
//                           /equinox/conf/system.ecf — di host: ./system.ecf)
//    $buildir / $rufdir     variabel bawaan: folder `out` (atau folder
//                           resep bila out kosong) / folder resep
//  Walker: rekursif (kedalaman maks 6), hanya file berakhiran .c yang
//  jadi job, urutan mengikuti readdir. Exit: 0 semua ok, 1 gagal, 2 usage.
// ============================================================================
#define MAKE_MAX_JOBS    128
#define MAKE_MAX_SRC     8
#define MAKE_MAX_EXCL    16
#define MAKE_PATH_MAX    112
#define MAKE_NAME_MAX    64
#define MAKE_DEPTH_MAX   6
// ---- v4 (ruf v4) ----
#define MAKE_MAX_FILES   16      /* token `src` yang ber-akhir .c (mode A) */
#define MAKE_MAX_EXJOB   32      /* baris `job … from … to …`             */
#define MAKE_EXJOB_SRC   6       /* sumber per job eksplisit              */
#define MAKE_MAX_SET     8       /* baris `set key = value`               */
#define MAKE_SET_KEY     48
#define MAKE_LINE_MAX    512     /* baris resep terpanjang                */
#define MAKE_KEY_MAX     40
#define MAKE_VAL_MAX     512
#define MAKE_EXPAND_MAX  512     /* buffer ekspansi $var                  */
// Image buffer: ELF butuh 84 B header + 64 B slack > MRP 18 B, jadi satu
// ukuran menampung KEDUA format (pool dialokasi sekali, dipakai ulang).
#define MAKE_IMAGE_CAP   (ELF_PAYLOAD + MTCC_CODE_CAP + 4 + MTCC_DATA_CAP + 64)

struct MakeJob { char path[MAKE_PATH_MAX]; int is_lib; };
static struct MakeJob g_make_jobs[MAKE_MAX_JOBS];
static int            g_make_njobs = 0;
static char           g_make_srcs[MAKE_MAX_SRC][MAKE_PATH_MAX];
static int            g_make_nsrc = 0;
static char           g_make_excl[MAKE_MAX_EXCL][MAKE_NAME_MAX];
static int            g_make_nexcl = 0;
static char           g_make_out[MAKE_PATH_MAX];
static int            g_make_lib_all = 0;
static int            g_make_walk_err = 0;
// ---- state v4 ----
// fmt: 0 = mrp, 1 = elf, 2 = "ikuti format global" (belum ditentukan)
struct MakeExJob {
    int  nsrc;
    char src[MAKE_EXJOB_SRC][MAKE_PATH_MAX];
    char out[MAKE_PATH_MAX];            /* `to` (wajib terisi)            */
    char name[MAKE_NAME_MAX];           /* nama job (untuk pesan error)   */
    char flags[48];                     /* salinan teks `flags "…"`       */
    int  fmt;
    int  is_lib;
    int  quiet;                         /* flags "-q": sembunyikan [COMPILE] */
};
static struct MakeExJob g_make_exjob[MAKE_MAX_EXJOB];
static int             g_make_nexjob = 0;
struct MakeSet { char key[MAKE_SET_KEY]; char val[MAKE_PATH_MAX]; };
static struct MakeSet  g_make_sets[MAKE_MAX_SET];
static int             g_make_nset = 0;
static char            g_make_ecf[MAKE_PATH_MAX];   /* "" = store aktif   */
static char            g_make_files[MAKE_MAX_FILES][MAKE_PATH_MAX];
static int             g_make_nfile = 0;
static int             g_make_multi = 0;            /* multiple_file=True */
static int             g_make_fmt_elf = 0;          /* format global      */
static char            g_make_rufdir[MAKE_PATH_MAX];/* folder resep ($rufdir/$buildir) */
static uint32_t make_slen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}
static void make_scpy(char* d, const char* s, uint32_t cap) {
    uint32_t i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static void make_trim(char* s) {
    uint32_t n = make_slen(s);
    while (n && (s[n-1]==' ' || s[n-1]=='\t' || s[n-1]=='\r')) { s[n-1] = 0; n--; }
    uint32_t st = 0;
    while (s[st]==' ' || s[st]=='\t') st++;
    if (st) { uint32_t i = 0; while (s[st]) { s[i] = s[st]; i++; st++; } s[i] = 0; }
}
static void make_join(char* dst, uint32_t cap, const char* a, const char* b) {
    make_scpy(dst, a, cap);
    uint32_t l = make_slen(dst);
    if (l && dst[l-1] != '/' && l < cap - 1) dst[l++] = '/';
    for (uint32_t i = 0; b[i] && l < cap - 1; i++) dst[l++] = b[i];
    dst[l] = '\0';
}
// ---- v0.9.3: variabel ala Makefile + copy/move post-build --------
#define MAKE_MAX_VARS   16
#define MAKE_VAR_NAME   32
#define MAKE_VAR_VAL    MAKE_PATH_MAX
#define MAKE_MAX_ACT    16
#define MAKE_ACT_SRC    4
static char g_var_name[MAKE_MAX_VARS][MAKE_VAR_NAME];
static char g_var_val[MAKE_MAX_VARS][MAKE_VAR_VAL];
static int  g_make_nvar = 0;

struct MakeAct {
    int  is_move;                              /* 0 copy, 1 move  */
    int  nsrc;
    char src[MAKE_ACT_SRC][MAKE_PATH_MAX];
    char dst[MAKE_PATH_MAX];
};
static struct MakeAct g_make_acts[MAKE_MAX_ACT];
static int  g_make_nact = 0;

static int make_streq_i(const char* a, const char* b) {
    while (*a || *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
        a++; b++;
    }
    return 1;
}
static int make_var_find(const char* name) {
    for (int i = 0; i < g_make_nvar; i++)
        if (make_streq_i(g_var_name[i], name)) return i;
    return -1;
}
static int make_var_store(char* name, char* val) {
    int v = make_var_find(name);
    if (v >= 0) { make_scpy(g_var_val[v], val, MAKE_VAR_VAL); return 0; }
    if (g_make_nvar >= MAKE_MAX_VARS) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR too many variables (max ");
        tcc_putn(MAKE_MAX_VARS); tcc_puts(")\n");
        return -1;
    }
    if (make_slen(name) >= MAKE_VAR_NAME) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR variable name too long"); tcc_nl();
        return -1;
    }
    make_scpy(g_var_name[g_make_nvar], name, MAKE_VAR_NAME);
    make_scpy(g_var_val[g_make_nvar], val, MAKE_VAR_VAL);
    g_make_nvar++;
    return 0;
}
/* nilai variabel: tabel `name := value` dulu, lalu variabel bawaan v4
   ($rufdir = folder resep, $buildir = `out` bila ada — resolve SAAT
   dipakai, jadi urutan deklarasi di resep tidak berpengaruh). */
static const char* make_var_value(const char* nm) {
    int v = make_var_find(nm);
    if (v >= 0) return g_var_val[v];
    if (make_streq_i(nm, "rufdir"))  return g_make_rufdir;
    if (make_streq_i(nm, "buildir")) return g_make_out[0] ? g_make_out : g_make_rufdir;
    return NULL;
}
/* $name -> nilai variabel (lookup case-insensitive, kaya Target/$target).
   Variabel tak dikenal dibiarkan apa adanya. */
static void make_expand(char* s, uint32_t cap) {
    char out[MAKE_EXPAND_MAX];
    uint32_t o = 0;
    uint32_t i = 0;
    while (s[i] && o < cap - 1) {
        if (s[i] != '$') { out[o++] = s[i++]; continue; }
        char nm[MAKE_VAR_NAME];
        uint32_t n = 0;
        i++;
        while (s[i] && ((s[i] >= 'A' && s[i] <= 'Z') ||
                        (s[i] >= 'a' && s[i] <= 'z') ||
                        (s[i] >= '0' && s[i] <= '9') || s[i] == '_') &&
               n < sizeof(nm) - 1) nm[n++] = s[i++];
        nm[n] = '\0';
        if (n == 0) { if (o < cap - 1) out[o++] = '$'; continue; }
        const char* val = make_var_value(nm);
        if (!val) {
            if (o < cap - 1) out[o++] = '$';
            for (uint32_t k = 0; k < n && o < cap - 1; k++) out[o++] = nm[k];
            continue;
        }
        for (uint32_t k = 0; val[k] && o < cap - 1; k++)
            out[o++] = val[k];
    }
    out[o] = '\0';
    make_scpy(s, out, cap);
}
static void make_mkdir_p(const char* path) {
    char tmp[MAKE_PATH_MAX];
    make_scpy(tmp, path, sizeof(tmp));
    int n = make_slen(tmp);
    int i = 0;
    if (tmp[0] == '/') i = 1;
    while (i < n) {
        while (i < n && tmp[i] != '/') i++;
        char save = tmp[i];
        tmp[i] = 0;
        if (i > 0) os_mkdir(tmp);        /* EEXIST diabaikan */
        tmp[i] = save;
        if (save == 0) break;
        i++;
    }
    os_mkdir(tmp);
}
static int make_isdir(const char* p) {
    void* d = os_dir_open(p);
    if (!d) return 0;
    os_dir_close(d);
    return 1;
}
static int make_copy_file(const char* src, const char* dst) {
    char* buf = 0;
    uint32_t total = 0;
    if (os_read_file(src, &buf, &total) != 0) return -1;
    int w = os_write_file(dst, (const uint8_t*)buf, total);
    os_free(buf, MTCC_SRC_CAP);
    return w;
}
static int make_copy_path(const char* src, const char* dst) {
    char target[MAKE_PATH_MAX];
    char child[MAKE_PATH_MAX];
    if (make_isdir(src)) {
        const char* base = src;
        for (const char* q = src; *q; q++) if (*q == '/') base = q + 1;
        make_join(target, sizeof(target), dst, (const char*)base);
        make_mkdir_p(target);                /* v0.9.3a: parents juga */
        void* d = os_dir_open(src);
        if (!d) return -1;
        for (;;) {
            int isd = 0;
            const char* nm = os_dir_next(d, &isd);
            if (!nm) break;
            if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
                continue;
            char name[MAKE_NAME_MAX];
            make_scpy(name, nm, sizeof(name));
            make_join(child, sizeof(child), src, name);
            if (isd) {
                make_copy_path(child, target);
            } else {
                char tf[MAKE_PATH_MAX];
                make_join(tf, sizeof(tf), target, name);
                make_copy_file(child, tf);
            }
        }
        os_dir_close(d);
        return 0;
    }
    if (make_isdir(dst)) {
        const char* base = src;
        for (const char* q = src; *q; q++) if (*q == '/') base = q + 1;
        make_join(target, sizeof(target), dst, (const char*)base);
        return make_copy_file(src, target);
    }
    return make_copy_file(src, dst);
}
static int make_move_path(const char* src, const char* dst) {
    char target[MAKE_PATH_MAX];
    if (os_rename(src, dst) == 0) return 0;
    if (make_isdir(dst)) {
        const char* base = src;
        for (const char* q = src; *q; q++) if (*q == '/') base = q + 1;
        make_join(target, sizeof(target), dst, (const char*)base);
        if (os_rename(src, target) == 0) return 0;
        return -1;              /* dir tanpa rename lintas lokasi: nope */
    }
    if (make_copy_path(src, dst) != 0) return -1;
    os_unlink(src);
    return 0;
}
/* eksekusi antrean copy/move SETELAH build sukses */
static int make_acts_run(void) {
    int rc = 0;
    for (int a = 0; a < g_make_nact; a++) {
        struct MakeAct* act = &g_make_acts[a];
        for (int s2 = 0; s2 < act->nsrc; s2++) {
            int r = act->is_move
                        ? make_move_path(act->src[s2], act->dst)
                        : make_copy_path(act->src[s2], act->dst);
            tcc_tag(TCC_CYAN, "MAKE");
            tcc_puts(act->is_move ? "move" : "copy");
            tcc_puts(": ");
            tcc_puts(act->src[s2]);
            tcc_puts(" -> ");
            tcc_puts(act->dst);
            if (r == 0) tcc_puts(" ok\n");
            else        { tcc_puts(" FAILED\n"); rc = -1; }
        }
    }
    return rc;
}
/* baris "copy|move A [& B...] ? dst" -> antrean (dieksekusi post-build) */
static int make_add_act(int is_move, char* val, int lineno) {
    char left[160];
    int qpos = -1;
    for (int i = 0; val[i]; i++)
        if (val[i] == '?') { qpos = i; break; }
    if (qpos < 0) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": expected '?' before the destination — ");
        tcc_puts(is_move ? "move <src> ? <dst>\n"
                         : "copy <src> ? <dst>\n");
        return -1;
    }
    uint32_t ll = 0;
    while (val[ll] && (int)ll < qpos && ll < sizeof(left) - 1) {
        left[ll] = val[ll]; ll++;
    }
    left[ll] = '\0';
    char dst[MAKE_PATH_MAX];
    make_scpy(dst, val + qpos + 1, sizeof(dst));
    make_trim(dst);
    uint32_t dl = make_slen(dst);
    if (dl >= 2 && ((dst[0] == '"' && dst[dl-1] == '"') ||
                    (dst[0] == '\'' && dst[dl-1] == '\''))) {
        for (uint32_t i = 0; i < dl - 2; i++) dst[i] = dst[i+1];
        dst[dl-2] = 0;
    }
    if (!dst[0]) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": empty destination\n");
        return -1;
    }
    if (g_make_nact >= MAKE_MAX_ACT) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR too many copy/move entries (max ");
        tcc_putn(MAKE_MAX_ACT); tcc_puts(")\n");
        return -1;
    }
    struct MakeAct* act = &g_make_acts[g_make_nact];
    act->is_move = is_move;
    act->nsrc = 0;
    act->dst[0] = 0;
    make_scpy(act->dst, dst, MAKE_PATH_MAX);
    /* sumber: pisah di '&' */
    uint32_t i2 = 0;
    while (i2 < ll) {
        char one[MAKE_PATH_MAX];
        uint32_t k = 0;
        while (i2 < ll && left[i2] != '&' && k < sizeof(one) - 1) {
            one[k++] = left[i2++];
        }
        i2++;
        one[k] = '\0';
        make_trim(one);
        uint32_t ol = make_slen(one);
        if (ol >= 2 && ((one[0] == '"' && one[ol-1] == '"') ||
                        (one[0] == '\'' && one[ol-1] == '\''))) {
            for (uint32_t j = 0; j < ol - 2; j++) one[j] = one[j+1];
            one[ol-2] = 0;
            ol -= 2;
        }
        if (!one[0]) continue;
        if (act->nsrc >= MAKE_ACT_SRC) {
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
            tcc_puts(": too many sources (max ");
            tcc_putn(MAKE_ACT_SRC); tcc_puts(")\n");
            return -1;
        }
        make_scpy(act->src[act->nsrc++], one, MAKE_PATH_MAX);
    }
    if (act->nsrc == 0) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": empty source list\n");
        return -1;
    }
    g_make_nact++;
    return 0;
}


// buang '/' ekor lalu daftarkan direktori sumber
static int make_add_src(char* v) {
    uint32_t n = make_slen(v);
    while (n && v[n-1] == '/') { v[n-1] = 0; n--; }
    if (!v[0]) return 0;
    if (g_make_nsrc >= MAKE_MAX_SRC) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR too many src entries (max ");
        tcc_putn(MAKE_MAX_SRC); tcc_puts(")\n");
        return -1;
    }
    make_scpy(g_make_srcs[g_make_nsrc++], v, MAKE_PATH_MAX);
    return 0;
}
static int make_add_excl(const char* v) {
    if (make_slen(v) >= MAKE_NAME_MAX) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR exclude name too long"); tcc_nl(); return -1;
    }
    for (int i = 0; i < g_make_nexcl; i++)
        if (m_streq(g_make_excl[i], v)) return 0;   // duplikat — abaikan
    if (g_make_nexcl >= MAKE_MAX_EXCL) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR too many exclude entries (max ");
        tcc_putn(MAKE_MAX_EXCL); tcc_puts(")\n");
        return -1;
    }
    make_scpy(g_make_excl[g_make_nexcl++], v, MAKE_NAME_MAX);
    return 0;
}

// ---- v4 helpers -------------------------------------------------------------
/* token berikutnya (dipisah spasi/tab) — kata di-NUL-kan di tempat, jadi
   pemanggil boleh memodifikasinya; NULL bila sudah habis. */
static char* make_tok(char** cur) {
    char* p = *cur;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *cur = p; return NULL; }
    char* s = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *p = 0; p++; }
    *cur = p;
    return s;
}
/* buang sepasang quote pembungkus (di tempat) */
static void make_unquote(char* s) {
    uint32_t n = make_slen(s);
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') ||
                   (s[0] == '\'' && s[n-1] == '\''))) {
        for (uint32_t i = 0; i < n - 2; i++) s[i] = s[i+1];
        s[n-2] = '\0';
    }
}
static int make_add_file(const char* v) {
    if (g_make_nfile >= MAKE_MAX_FILES) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR too many src files (max ");
        tcc_putn(MAKE_MAX_FILES); tcc_puts(")\n");
        return -1;
    }
    make_scpy(g_make_files[g_make_nfile++], v, MAKE_PATH_MAX);
    return 0;
}
/* nilai `src`: daftar token (pisah spasi / ',' / '&'). Token BERAKHIR ".c"
   = file sumber mode A (multiple_file), sisanya direktori (walk, v3).
   Dengan begitu `src $pathsrc`, `src a.c, b.c` dan `src /dir` semuanya sah
   dan urutan baris di resep tidak berpengaruh. */
static int make_add_src_val(char* v, int lineno) {
    char* p = v;
    int rc = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '&') p++;
        if (!*p) break;
        char* s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '&') p++;
        if (*p) { *p = 0; p++; }
        make_unquote(s);
        if (!s[0]) continue;
        uint32_t n = make_slen(s);
        int is_c = (n >= 3 && s[n-2] == '.' && s[n-1] == 'c');
        if (is_c) {
            if (make_add_file(s) != 0) rc = -1;
        } else {
            if (make_add_src(s) != 0) rc = -1;
        }
    }
    (void)lineno;
    return rc;
}
/* `job <nama> from <sumber [& sumber…]> to <keluaran>
      [format mrp|elf] [flags "-q --lib"] [lib]`
   Satu baris = N sumber -> SATU keluaran (memetakan ke compile_start /
   compile_add* / compile_finish = jalur multi-file yang sudah ada). */
static int make_add_job(char* v, int lineno) {
    char* cur = v;
    char* nm = make_tok(&cur);
    if (!nm) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": job requires <name> from <src> to <out>\n");
        return -1;
    }
    char* kw = make_tok(&cur);
    if (!kw || !m_streq(kw, "from")) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": expected 'from' after the job name\n");
        return -1;
    }
    if (g_make_nexjob >= MAKE_MAX_EXJOB) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR too many job lines (max ");
        tcc_putn(MAKE_MAX_EXJOB); tcc_puts(")\n");
        return -1;
    }
    struct MakeExJob* ej = &g_make_exjob[g_make_nexjob];
    ej->nsrc = 0; ej->out[0] = 0; ej->flags[0] = 0;
    ej->fmt = 2; ej->is_lib = 0; ej->quiet = 0;
    make_scpy(ej->name, nm, MAKE_NAME_MAX);
    // ---- sumber: token sampai kata `to` ----
    for (;;) {
        char* t = make_tok(&cur);
        if (!t) {
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
            tcc_puts(": job '"); tcc_puts(ej->name);
            tcc_puts("' is missing 'to <output>'\n");
            return -1;
        }
        if (m_streq(t, "to")) break;
        if (m_streq(t, "&") || m_streq(t, ",")) continue;   /* pemisah lepas */
        char* p = t;
        while (*p) {
            char one[MAKE_PATH_MAX];
            uint32_t k = 0;
            while (*p && *p != '&' && *p != ',' && k < sizeof(one) - 1)
                one[k++] = *p++;
            while (*p == '&' || *p == ',') p++;
            one[k] = '\0';
            make_unquote(one);
            if (!one[0]) continue;
            if (ej->nsrc >= MAKE_EXJOB_SRC) {
                tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
                tcc_puts(": job '"); tcc_puts(ej->name);
                tcc_puts("' has too many sources (max ");
                tcc_putn(MAKE_EXJOB_SRC); tcc_puts(")\n");
                return -1;
            }
            make_scpy(ej->src[ej->nsrc++], one, MAKE_PATH_MAX);
        }
    }
    // ---- keluaran ----
    char* op = make_tok(&cur);
    if (!op) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": job '"); tcc_puts(ej->name);
        tcc_puts("' is missing <output> after 'to'\n");
        return -1;
    }
    make_unquote(op);
    make_expand(op, MAKE_PATH_MAX);
    if (!op[0]) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": job '"); tcc_puts(ej->name); tcc_puts("' has an empty output\n");
        return -1;
    }
    make_scpy(ej->out, op, MAKE_PATH_MAX);
    {   /* folder keluaran dibuat sekarang (identik dengan perilaku `out`) */
        char dir[MAKE_PATH_MAX];
        make_scpy(dir, ej->out, sizeof(dir));
        int cut = -1;
        for (int i = 0; dir[i]; i++) if (dir[i] == '/') cut = i;
        if (cut > 0) { dir[cut] = '\0'; make_mkdir_p(dir); }
    }
    // ---- opsi positional SETELAH `to` ----
    char* t2;
    while ((t2 = make_tok(&cur)) != NULL) {
        if (m_streq(t2, "format")) {
            char* f = make_tok(&cur);
            if (!f) {
                tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
                tcc_puts(": job: format expects mrp or elf\n");
                return -1;
            }
            make_unquote(f);
            if (m_streq(f, "elf"))       ej->fmt = 1;
            else if (m_streq(f, "mrp"))  ej->fmt = 0;
            else {
                tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
                tcc_puts(": job: format expects mrp or elf, got '");
                tcc_puts(f); tcc_puts("'\n");
                return -1;
            }
        } else if (m_streq(t2, "flags")) {
            char fb[48]; uint32_t fi = 0;
            while (*cur == ' ' || *cur == '\t') cur++;
            if (*cur == '"') {
                cur++;
                while (*cur && *cur != '"' && fi < sizeof(fb) - 1) fb[fi++] = *cur++;
                if (*cur == '"') cur++;
            } else {
                while (*cur && *cur != ' ' && *cur != '\t' && fi < sizeof(fb) - 1)
                    fb[fi++] = *cur++;
            }
            fb[fi] = '\0';
            make_scpy(ej->flags, fb, sizeof(ej->flags));
            /* hanya flag yang berarti untuk build per job yang diizinkan */
            char* fp = fb;
            while (*fp) {
                while (*fp == ' ') fp++;
                if (!*fp) break;
                char ft[16]; uint32_t k = 0;
                while (*fp && *fp != ' ' && k < sizeof(ft) - 1) ft[k++] = *fp++;
                ft[k] = '\0';
                if (m_streq(ft, "-q")) ej->quiet = 1;
                else if (m_streq(ft, "--lib")) ej->is_lib = 1;
                else {
                    tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
                    tcc_puts(": job: unsupported flag '");
                    tcc_puts(ft); tcc_puts("' (only -q and --lib)\n");
                    return -1;
                }
            }
        } else if (m_streq(t2, "lib")) {
            ej->is_lib = 1;
        } else {
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
            tcc_puts(": job: unknown option '"); tcc_puts(t2);
            tcc_puts("' (expected format / flags / lib)\n");
            return -1;
        }
    }
    if (ej->nsrc == 0) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": job '"); tcc_puts(ej->name); tcc_puts("' has no source\n");
        return -1;
    }
    g_make_nexjob++;
    return 0;
}
/* `set <key> = <value>` — antrian tulis-.ecf, dieksekusi SETELAH build
   sukses. Bentuk tanpa '=' juga diterima (`set key value`). */
static int make_add_set(char* v, int lineno) {
    while (*v == ' ' || *v == '\t') v++;
    char key[MAKE_SET_KEY];
    uint32_t k = 0;
    while (*v && *v != '=' && *v != ' ' && *v != '\t' && k < sizeof(key) - 1)
        key[k++] = *v++;
    key[k] = '\0';
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '=') { v++; while (*v == ' ' || *v == '\t') v++; }
    char val[MAKE_PATH_MAX];
    make_scpy(val, v, sizeof(val));
    make_trim(val);
    make_unquote(val);
    make_expand(val, sizeof(val));
    if (!key[0] || !val[0]) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": set requires <key> = <value>\n");
        return -1;
    }
    if (g_make_nset >= MAKE_MAX_SET) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR too many set entries (max ");
        tcc_putn(MAKE_MAX_SET); tcc_puts(")\n");
        return -1;
    }
    make_scpy(g_make_sets[g_make_nset].key, key, MAKE_SET_KEY);
    make_scpy(g_make_sets[g_make_nset].val, val, MAKE_PATH_MAX);
    g_make_nset++;
    return 0;
}
// satu baris .ruf -> 0 ok / -1 error
static int make_line(char* line, int lineno) {
    make_trim(line);
    if (!line[0] || line[0] == '#') return 0;

    // v4: `set <key> = <value>` ditangkap SEBELUM pemecahan key/value umum
    // (kunci boleh berupa token berspasi-tempel, mis. `set eggkg.local = x`)
    if (line[0] == 's' && line[1] == 'e' && line[2] == 't' &&
        (line[3] == ' ' || line[3] == '\t')) return make_add_set(line + 3, lineno);
    if (line[0] == 's' && line[1] == 'e' && line[2] == 't' && !line[3]) {
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": set requires <key> = <value>\n");
        return -1;
    }

    char key[MAKE_KEY_MAX]; char val[MAKE_VAL_MAX];
    key[0] = 0; val[0] = 0;
    const char* eq = 0;
    for (const char* q = line; *q; q++) if (*q == '=') { eq = q; break; }
    int is_assign = (eq && eq > line && eq[-1] == ':') ? 1 : 0;
    if (eq) {
        uint32_t kl = (uint32_t)(eq - line);
        if (kl >= sizeof(key)) { key[0] = 0; }
        else {
            uint32_t i = 0;
            for (; i < kl && i < sizeof(key) - 1; i++) key[i] = line[i];
            key[i] = 0; make_trim(key);
            uint32_t kln = make_slen(key);
            if (kln && key[kln-1] == ':') key[kln-1] = 0;   // "exclude:" -> "exclude"
            make_trim(key);   // v0.9.3: "source := x" -> "source " -> "source"
        }
        make_scpy(val, eq + 1, sizeof(val)); make_trim(val);
    } else {
        // "key value" — token pertama jadi key, sisanya val
        uint32_t i = 0;
        while (line[i] && line[i] != ' ' && line[i] != '\t' && i < sizeof(key) - 1) {
            key[i] = line[i]; i++;
        }
        key[i] = 0;
        const char* rest = line + i;
        while (*rest == ' ' || *rest == '\t') rest++;
        make_scpy(val, rest, sizeof(val)); make_trim(val);
    }
    // strip sepasang quotes pembungkus (untuk echo)
    uint32_t vl = make_slen(val);
    if (vl >= 2 && ((val[0]=='"' && val[vl-1]=='"') ||
                    (val[0]=='\'' && val[vl-1]=='\''))) {
        for (uint32_t i = 0; i < vl - 2; i++) val[i] = val[i+1];
        val[vl-2] = 0;
    }

    // v0.9.3: $variabel diekspansi untuk SEMUA direktif
    make_expand(val, sizeof(val));

    if (m_streq(key, "echo")) {
        tcc_tag(TCC_CYAN, "ECHO"); tcc_puts(val); tcc_nl();
        return 0;
    }
    if (m_streq(key, "src")) {
        if (!val[0]) {
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
            tcc_puts(": src requires <dir|file…>\n"); return -1;
        }
        return make_add_src_val(val, lineno);   /* v4: dir MAUPUN daftar .c */
    }
    if (m_streq(key, "exclude")) {
        if (!val[0]) {
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
            tcc_puts(": exclude requires <name>\n"); return -1;
        }
        return make_add_excl(val);
    }
    if (m_streq(key, "out")) {
        if (!val[0]) {
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
            tcc_puts(": out requires <dir>\n"); return -1;
        }
        make_scpy(g_make_out, val, MAKE_PATH_MAX);
        make_mkdir_p(val);   /* v0.9.3a: target out dibuat otomatis */
        return 0;
    }
    if (m_streq(key, "lib")) {
        if (!val[0] || m_streq(val, "1") || m_streq(val, "true") ||
            m_streq(val, "yes") || m_streq(val, "on")) {
            g_make_lib_all = 1;
            return 0;
        }
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": unknown value for lib\n"); return -1;
    }
    if (m_streq(key, "format")) {              // v4: format keluaran global
        if (m_streq(val, "elf"))      g_make_fmt_elf = 1;
        else if (m_streq(val, "mrp")) g_make_fmt_elf = 0;
        else {
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
            tcc_puts(": format expects mrp or elf, got '"); tcc_puts(val);
            tcc_puts("'\n"); return -1;
        }
        return 0;
    }
    if (m_streq(key, "multiple_file")) {       // v4: semua sumber -> SATU link
        if (!val[0] || m_streq(val, "1") || make_streq_i(val, "true") ||
            make_streq_i(val, "yes") || make_streq_i(val, "on")) {
            g_make_multi = 1; return 0;
        }
        if (m_streq(val, "0") || make_streq_i(val, "false") ||
            make_streq_i(val, "no") || make_streq_i(val, "off")) {
            g_make_multi = 0; return 0;
        }
        tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
        tcc_puts(": multiple_file expects True or False, got '"); tcc_puts(val);
        tcc_puts("'\n"); return -1;
    }
    if (m_streq(key, "ecf")) {                 // v4: target `set` berikutnya
        if (!val[0]) {
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
            tcc_puts(": ecf requires <path>\n"); return -1;
        }
        make_scpy(g_make_ecf, val, MAKE_PATH_MAX);
        return 0;
    }
    if (m_streq(key, "job")) {                 // v4: N sumber -> SATU keluaran
        if (!val[0]) {
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
            tcc_puts(": job requires <name> from <src> to <out>\n"); return -1;
        }
        return make_add_job(val, lineno);
    }
    if (m_streq(key, "copy") || m_streq(key, "move")) {
        // v0.9.3: "copy A [& B] ? dst" — dieksekusi SETELAH build sukses
        return make_add_act(m_streq(key, "move"), val, lineno);
    }
    // baris telanjang berupa path -> src ("equinox/" ala sketch);
    // v4: token ber-akhir .c jadi daftar file mode A (lihat make_add_src_val)
    if (!val[0] && key[0] && !eq) {
        return make_add_src_val(key, lineno);
    }
    // v0.9.3: "name := value" -> variabel (kunci selain direktif)
    if (is_assign) {
        return make_var_store(key, val);
    }
    tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)lineno);
    tcc_puts(": unknown directive '"); tcc_puts(key); tcc_puts("'\n");
    return -1;
}

// walker rekursif — .c jadi job; exclude cocok nama entri (semua level);
// file di bawah dir "libc" (di level mana pun) otomatis --lib
static void make_walk(const char* dir, int depth, int in_libc) {
    void* d = os_dir_open(dir);
    if (!d) {
        tcc_tag(TCC_YELLOW, "MAKE"); tcc_puts("WARNING cannot open directory '"); tcc_puts(dir);
        tcc_puts("' — skipped\n");
        g_make_walk_err = 1;
        return;
    }
    for (;;) {
        int is_dir = 0;
        const char* nm = os_dir_next(d, &is_dir);
        if (!nm) break;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
        // SALIN nama dulu — buffer os_dir_next tertimpa saat rekursi
        char name[MAKE_NAME_MAX];
        make_scpy(name, nm, sizeof(name));
        int excl = 0;
        for (int i = 0; i < g_make_nexcl; i++)
            if (m_streq(g_make_excl[i], name)) { excl = 1; break; }
        if (excl) continue;
        char child[MAKE_PATH_MAX];
        make_join(child, sizeof(child), dir, name);
        if (is_dir) {
            if (depth < MAKE_DEPTH_MAX)
                make_walk(child, depth + 1, in_libc || m_streq(name, "libc"));
        } else {
            uint32_t l = make_slen(name);
            if (l >= 2 && name[l-2] == '.' && name[l-1] == 'c' &&
                g_make_njobs < MAKE_MAX_JOBS) {
                make_scpy(g_make_jobs[g_make_njobs].path, child, MAKE_PATH_MAX);
                g_make_jobs[g_make_njobs].is_lib = in_libc || g_make_lib_all;
                g_make_njobs++;
            }
        }
    }
    os_dir_close(d);
}

// buffer pool bekerja bersama mtcc_compile (g_pool_*) — tidak perlu
// free antar-job: semua buffer diademasi sekali dan dipakai ulang.

/* ".mrp"/".elf" sudah menempel di ekor `s`? */
static int make_has_ext(const char* s, const char* ext4) {
    uint32_t l = make_slen(s);
    return (l >= 4 && s[l-4] == '.' && s[l-3] == ext4[1] &&
            s[l-2] == ext4[2] && s[l-1] == ext4[3]);
}
// <dir>/<base>.<ext> di samping sumber — atau <out>/<base>.<ext> bila out
// aktif. v4: ekstensi mengikuti format (mrp|elf) dan `use_name` (mode A)
// memakai variabel `name` bila ada, persis seperti -o CLI: ekstensi
// ditambahkan bila belum ada dan tidak pernah diduplikasi.
static void make_output_name(const char* cpath, char* oname, uint32_t cap,
                             int fmt_elf, int use_name) {
    const char* ext = fmt_elf ? ".elf" : ".mrp";
    const char* base = cpath;
    for (const char* q = cpath; *q; q++) if (*q == '/') base = q + 1;
    uint32_t dlen = (uint32_t)(base - cpath);        /* prefix "dir/" */

    char name[MAKE_NAME_MAX];
    name[0] = '\0';
    if (use_name) {
        int v = make_var_find("name");
        if (v >= 0 && g_var_val[v][0]) make_scpy(name, g_var_val[v], MAKE_NAME_MAX);
        if (make_has_ext(name, ".mrp") || make_has_ext(name, ".elf")) {
            uint32_t n = make_slen(name);
            name[n - 4] = '\0';                     /* `name := prog.mrp` */
        }
    }
    if (!name[0]) {
        uint32_t k = 0;
        while (base[k] && base[k] != '.' && k < sizeof(name) - 1) { name[k] = base[k]; k++; }
        name[k] = '\0';
    }
    if (g_make_out[0]) {
        make_join(oname, cap, g_make_out, name);
    } else if (dlen > 0) {
        uint32_t l = 0;
        for (; l < dlen && l < cap - 1; l++) oname[l] = cpath[l];
        for (uint32_t i = 0; name[i] && l < cap - 1; i++) oname[l++] = name[i];
        oname[l] = '\0';
    } else {
        make_scpy(oname, name, cap);
    }
    if (make_has_ext(oname, ext)) return;
    uint32_t l = make_slen(oname);
    for (uint32_t i = 0; ext[i] && l < cap - 1; i++) oname[l++] = ext[i];
    oname[l] = '\0';
}
/* `to` eksplisit: ekstensi yang sudah ditulis dipakai apa adanya; ekstensi
   yang BERTENTANGAN dengan format = error (jangan diam-diam menulis .mrp
   saat format elf diminta). */
static int make_job_out_name(const struct MakeExJob* ej, int fmt_elf,
                             char* oname, uint32_t cap) {
    const char* ext = fmt_elf ? ".elf" : ".mrp";
    make_scpy(oname, ej->out, cap);
    if (make_has_ext(oname, ext)) return 0;
    if (make_has_ext(oname, ".mrp") || make_has_ext(oname, ".elf")) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR job '"); tcc_puts(ej->name);
        tcc_puts("': output '"); tcc_puts(oname);
        tcc_puts("' conflicts with format "); tcc_puts(fmt_elf ? "elf" : "mrp");
        tcc_nl();
        return -1;
    }
    uint32_t l = make_slen(oname);
    if (l + 4 >= cap) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR job '"); tcc_puts(ej->name);
        tcc_puts("': output name too long"); tcc_nl();
        return -1;
    }
    for (uint32_t i = 0; ext[i]; i++) oname[l++] = ext[i];
    oname[l] = '\0';
    return 0;
}

// ---- v4: satu unit build (N sumber -> SATU output, dua sisi mata uang) -----
/* [COMPILE] a.c + b.c -> out.mrp — maksimal 3 nama tampil, sisanya
   "+N more" supaya baris tetap terbaca walau mode A menautkan 40 file. */
static void make_report_compile(char* const* srcs, int nsrc, const char* oname) {
    tcc_tag(TCC_GREEN, "COMPILE");
    int shown = (nsrc <= 3) ? nsrc : 2;
    for (int i = 0; i < shown; i++) {
        if (i) tcc_puts(" + ");
        tcc_puts(srcs[i]);
    }
    if (nsrc > shown) {
        tcc_puts(" + ");
        tcc_putn((uint32_t)(nsrc - shown));
        tcc_puts(" more");
    }
    tcc_puts(" -> ");
    tcc_puts(oname);
    tcc_nl();
}
/* Compile N sumber dengan SATU symbol table + SATU fixup list (jalur
   multi-file yang sama dengan CLI), bangun gambar sesuai format, validasi,
   tulis. return 0 sukses / 1 gagal (pesan lengkap sudah dicetak). */
static int make_run_one(char* const* srcs, int nsrc, const char* oname,
                        int fmt_elf, int is_lib, int show, const char* jname) {
    if (nsrc <= 0) return 1;
    g_lib_mode = is_lib;
    MtccOut out;
    int cr = 0;
    if (nsrc == 1) {
        // jalur v3 (satu .c = satu job) tetap lewat wrapper mtcc_compile
        // (start+add+finish) — perilaku identik dengan sebelum v4.
        char* s = NULL; uint32_t sl = 0;
        if (os_read_file_cached(srcs[0], &s, &sl) != 0 || !s) {
            tcc_tag(TCC_RED, "MAKE");
            tcc_puts("ERROR cannot read ");
            tcc_puts(srcs[0]);
            tcc_nl();
            return 1;
        }
        cr = mtcc_compile(s, sl, &out, srcs[0]);
    } else {
        cr = mtcc_compile_start();
        for (int i = 0; i < nsrc && cr == 0; i++) {
            char* s = NULL; uint32_t sl = 0;
            /* Satu buffer cache per bacaan — aman karena pp_run MENYALIN
               sumber ke buffer PP-nya sendiri sebelum diurai, jadi bacaan
               berikutnya boleh menimpanya. */
            if (os_read_file_cached(srcs[i], &s, &sl) != 0 || !s) {
                tcc_tag(TCC_RED, "MAKE");
                tcc_puts("ERROR cannot read ");
                tcc_puts(srcs[i]);
                tcc_nl();
                return 1;
            }
            if (mtcc_compile_add(s, sl, srcs[i]) != 0) cr = 1;
        }
        if (cr == 0) cr = mtcc_compile_finish(&out);
    }
    if (cr != 0) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR ");
        if (jname) { tcc_puts("job '"); tcc_puts(jname); tcc_puts("': "); }
        tcc_puts(srcs[0]);
        if (cr == 2) tcc_puts(": 4 MB compiler arena exhausted");
        else { tcc_nl(); tcc_report_error(); }
        return 1;
    }
    if (!g_pool_image) g_pool_image = (uint8_t*)os_alloc(MAKE_IMAGE_CAP);
    uint8_t* image = g_pool_image;
    if (!image) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR out of memory building the image for ");
        tcc_puts(srcs[0]);
        tcc_nl();
        return 1;
    }
    uint32_t total = 0;
    if (fmt_elf) {
        total = mtcc_build_image_elf(&out, image, MAKE_IMAGE_CAP);
        int why = total ? mtcc_elf_check(image, total) : 1;
        if (why != 0) {
            tcc_tag(TCC_RED, "MAKE");
            tcc_puts("ERROR invalid ELF image for ");
            tcc_puts(srcs[0]);
            if (total) {
                tcc_puts(" (check #");
                tcc_putn((uint32_t)why);
                tcc_puts(")");
            }
            tcc_nl();
            return 1;
        }
    } else {
        enum mrp_validate_reason vr;
        total = mtcc_build_image(&out, image, MAKE_IMAGE_CAP);
        int bad = (total == 0);
        if (!bad && !is_valid_mrp(image, total, &vr)) bad = 1;
        if (bad) {
            tcc_tag(TCC_RED, "MAKE");
            tcc_puts("ERROR invalid image for ");
            tcc_puts(srcs[0]);
            tcc_nl();
            return 1;
        }
    }
    if (os_write_file(oname, image, total) != 0) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR cannot write ");
        tcc_puts(oname);
        tcc_nl();
        return 1;
    }
    if (show) make_report_compile(srcs, nsrc, oname);
    return 0;
}

// ---- v4: `set key = value` -> file .ecf (baca-modify-tulis) ---------------
#ifdef MTCC_HOST_TEST
#define MAKE_ECF_DEFAULT "system.ecf"     /* host: store aktif = ./system.ecf */
#else
#define MAKE_ECF_DEFAULT "/equinox/conf/system.ecf"
#endif
/* Satu key di-merge ke file .ecf (INI-lite, aturan parser kernel ecf.c):
   - baris yang cocok diganti HANYA nilainya. Key efektif sebuah baris =
     `section.lhs` bila lhs tak ber-titik, lhs apa adanya bila ber-titik —
     jadi `[net] driver = x` terbaca sebagai net.driver, dan menimpa
     `net.driver` berarti menyentuh baris itu. Lhs, section, komentar dan
     key lain tidak pernah berubah.
   - key absen / file absen -> dibuat. Key ber-titik di-append di EOF
     (dotted = verbatim); key polos disisipkan SEBELUM section pertama
     supaya tetap terbaca sebagai key global.
   return 0 sukses / -1 gagal (pesan sudah dicetak). */
static int make_ecf_set(const char* path, const char* key, const char* val) {
    char* in = NULL;
    uint32_t inlen = 0;
    if (os_read_file(path, &in, &inlen) != 0 || !in) { in = NULL; inlen = 0; }
    if (inlen >= MTCC_SRC_CAP - 512) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR "); tcc_puts(path); tcc_puts(" is too big to edit");
        tcc_nl();
        if (in) os_free(in, MTCC_SRC_CAP);
        return -1;
    }
    int key_dotted = 0;
    for (const char* c = key; *c; c++) if (*c == '.') { key_dotted = 1; break; }

    // ---- phase 1: lokasi baris yang cocok + titik sisip ----------------
    uint32_t m_start = 0, m_end = 0;      /* m_end = 0 -> tak ada yang cocok */
    uint32_t insert_at = inlen;
    int seen_section = 0;
    char lhs[MAKE_SET_KEY * 2];
    char section[64];
    lhs[0] = 0; section[0] = 0;
    uint32_t pos = 0;
    while (pos < inlen) {
        uint32_t s = pos, e = pos;
        while (e < inlen && in[e] != '\n') e++;
        pos = e + 1;
        uint32_t b = s;
        while (b < e && (in[b] == ' ' || in[b] == '\t')) b++;
        if (b >= e || in[b] == '#') continue;
        if (in[b] == '[') {
            if (!seen_section) {
                seen_section = 1;
                if (!key_dotted) insert_at = s;   /* polos -> sebelum section */
            }
            uint32_t k = b + 1, ke = b + 1;
            while (ke < e && in[ke] != ']') ke++;
            uint32_t n = ke - k;
            if (n >= sizeof(section)) n = sizeof(section) - 1;
            for (uint32_t i = 0; i < n; i++) section[i] = in[k + i];
            section[n] = '\0';
            continue;
        }
        uint32_t eq = b;
        while (eq < e && in[eq] != '=') eq++;
        if (eq >= e) continue;                    /* bukan baris key        */
        uint32_t le = eq;
        while (le > b && (in[le-1] == ' ' || in[le-1] == '\t' || in[le-1] == '\r'))
            le--;
        if (le <= b) continue;                    /* "= value" tanpa key    */
        int dotted = 0;
        for (uint32_t i = b; i < le; i++) if (in[i] == '.') { dotted = 1; break; }
        char eff[MAKE_SET_KEY * 2];
        uint32_t n = 0;
        if (!dotted && section[0]) {
            for (uint32_t i = 0; section[i] && n < sizeof(eff) - 1; i++)
                eff[n++] = section[i];
            if (n < sizeof(eff) - 1) eff[n++] = '.';
        }
        for (uint32_t i = b; i < le && n < sizeof(eff) - 1; i++) eff[n++] = in[i];
        eff[n] = '\0';
        if (!m_end && make_streq_i(eff, key)) {
            m_start = s;
            m_end = e;                            /* tanpa '\n' (ikut sisa) */
            uint32_t ln = le - b;
            if (ln >= sizeof(lhs)) ln = sizeof(lhs) - 1;
            for (uint32_t i = 0; i < ln; i++) lhs[i] = in[b + i];
            lhs[ln] = '\0';
        }
    }

    // ---- phase 2: susun konten baru ----
    uint32_t need = inlen + make_slen(key) + make_slen(val) + 8;
    if (m_end) need = (m_end - m_start) > (make_slen(lhs) + make_slen(val) + 4)
                        ? inlen : inlen + make_slen(lhs) + make_slen(val) + 8;
    if (need >= MTCC_SRC_CAP) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR merging '"); tcc_puts(key);
        tcc_puts("' into "); tcc_puts(path); tcc_puts(" would overflow");
        tcc_nl();
        if (in) os_free(in, MTCC_SRC_CAP);
        return -1;
    }
    char* out = (char*)os_alloc(MTCC_SRC_CAP);
    if (!out) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR out of memory editing "); tcc_puts(path); tcc_nl();
        if (in) os_free(in, MTCC_SRC_CAP);
        return -1;
    }
    uint32_t o = 0;
    if (m_end) {                                  /* ganti baris yang cocok */
        for (uint32_t i = 0; i < m_start; i++) out[o++] = in[i];
        for (uint32_t i = 0; lhs[i]; i++) out[o++] = lhs[i];
        out[o++] = ' '; out[o++] = '='; out[o++] = ' ';
        for (uint32_t i = 0; val[i]; i++) out[o++] = val[i];
        for (uint32_t i = m_end; i < inlen; i++) out[o++] = in[i];
    } else {                                      /* sisipkan key baru      */
        for (uint32_t i = 0; i < insert_at; i++) out[o++] = in[i];
        if (insert_at > 0 && in[insert_at - 1] != '\n') out[o++] = '\n';
        for (uint32_t i = 0; key[i]; i++) out[o++] = key[i];
        out[o++] = ' '; out[o++] = '='; out[o++] = ' ';
        for (uint32_t i = 0; val[i]; i++) out[o++] = val[i];
        out[o++] = '\n';
        for (uint32_t i = insert_at; i < inlen; i++) out[o++] = in[i];
    }
    int wr = os_write_file(path, (const uint8_t*)out, o);
    if (in) os_free(in, MTCC_SRC_CAP);
    os_free(out, MTCC_SRC_CAP);
    if (wr != 0) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR cannot write "); tcc_puts(path); tcc_nl();
        return -1;
    }
    return 0;
}
/* jalankan antrean `set` SETELAH build sukses (bersama copy/move) */
static int make_sets_run(void) {
    int rc = 0;
    for (int i = 0; i < g_make_nset; i++) {
        const char* target = g_make_ecf[0] ? g_make_ecf : MAKE_ECF_DEFAULT;
        int r = make_ecf_set(target, g_make_sets[i].key, g_make_sets[i].val);
        tcc_tag(TCC_CYAN, "MAKE");
        tcc_puts("set ");
        tcc_puts(g_make_sets[i].key);
        tcc_puts(" = ");
        tcc_puts(g_make_sets[i].val);
        tcc_puts(" -> ");
        tcc_puts(target);
        if (r == 0) tcc_puts(" ok");
        else        { tcc_puts(" FAILED"); rc = -1; }
        tcc_nl();
    }
    return rc;
}

// jalankan mtcc -make — return 0 semua ok / 1 gagal
static int mtcc_make_run(const char* ruf_path) {
    g_make_njobs = 0; g_make_nsrc = 0; g_make_nexcl = 0;
    g_make_out[0] = 0; g_make_lib_all = 0; g_make_walk_err = 0;
    g_make_nvar = 0; g_make_nact = 0;   /* v0.9.3 */
    g_make_nfile = 0; g_make_multi = 0; g_make_fmt_elf = 0;   /* v4 */
    g_make_nexjob = 0; g_make_nset = 0; g_make_ecf[0] = 0;
    g_make_rufdir[0] = 0;
    {   /* $rufdir (dan default $buildir) = folder tempat resep berada */
        const char* slash = 0;
        for (const char* q = ruf_path; *q; q++) if (*q == '/') slash = q;
        if (slash && slash != ruf_path) {
            uint32_t n = (uint32_t)(slash - ruf_path);
            if (n >= MAKE_PATH_MAX) n = MAKE_PATH_MAX - 1;
            for (uint32_t i = 0; i < n; i++) g_make_rufdir[i] = ruf_path[i];
            g_make_rufdir[n] = '\0';
        }
    }
    {   /* 0.5: default .config/mtcc.ecf — resep & CLI selalu menang
         * (variabel ini di-reset di atas, jadi dicatat SEBELUM parse). */
        char v[MAKE_PATH_MAX];
        if (mtcc_cfg_get("format.default", v, sizeof(v)) &&
            v[0] == 'e' && v[1] == 'l' && v[2] == 'f' && v[3] == '\0')
            g_make_fmt_elf = 1;
        if (mtcc_cfg_get("set.store", v, sizeof(v)) && v[0])
            make_scpy(g_make_ecf, v, MAKE_PATH_MAX);   /* `ecf =` menang */
        if (mtcc_cfg_get("rufdir.default", v, sizeof(v)) && v[0] &&
            !g_make_rufdir[0])
            make_scpy(g_make_rufdir, v, MAKE_PATH_MAX);
        if (mtcc_cfg_get("buildir.default", v, sizeof(v)) && v[0])
            make_scpy(g_make_out, v, MAKE_PATH_MAX);   /* `out =` menang */
    }

    char* buf = NULL; uint32_t blen = 0;
    if (os_read_file_cached(ruf_path, &buf, &blen) != 0 || !buf) {
        tcc_tag(TCC_RED, "ERROR"); tcc_puts("cannot read '"); tcc_puts(ruf_path);
        tcc_puts("' (is the .ruf file missing?)"); tcc_nl();
        return 1;
    }
    uint32_t pos = 0; int lineno = 0; int perr = 0;
    char line[MAKE_LINE_MAX];
    while (pos < blen && !perr) {
        uint32_t ll = 0;
        while (pos + ll < blen && buf[pos+ll] != '\n') ll++;
        if (ll >= sizeof(line)) {
            // diam-diam memotong baris = resep yang salah baca; error saja.
            tcc_tag(TCC_RED, "MAKE"); tcc_putn((uint32_t)(lineno + 1));
            tcc_puts(": line too long (max ");
            tcc_putn(MAKE_LINE_MAX - 1);
            tcc_puts(")\n");
            perr = 1; break;
        }
        for (uint32_t i = 0; i < ll; i++) line[i] = buf[pos+i];
        line[ll] = 0;
        lineno++;
        pos += ll + 1;
        if (make_line(line, lineno) != 0) perr = 1;
    }
    if (perr) return 1;
    if (g_make_nfile && !g_make_multi) {
        tcc_tag(TCC_RED, "MAKE");
        tcc_puts("ERROR an explicit .c file list in 'src' needs 'multiple_file = True'");
        tcc_nl();
        return 1;
    }
    if (g_make_nsrc == 0 && g_make_nfile == 0 && g_make_nexjob == 0) {
        tcc_tag(TCC_RED, "MAKE"); tcc_puts("ERROR no 'src <dir>' in "); tcc_puts(ruf_path);
        tcc_puts("\n");
        return 1;
    }
    for (int s = 0; s < g_make_nsrc; s++) {
        // src yang MENUNJUK dir libc langsung (mis. "src /equinox/libc")
        // juga harus --lib — cek komponen terakhir dari path src
        const char* last = g_make_srcs[s];
        for (const char* q = last; *q; q++)
            if (*q == '/') last = q + 1;
        make_walk(g_make_srcs[s], 0, m_streq(last, "libc"));
    }

    // ---- susun daftar kerja -------------------------------------------
    // mode A: file `src` + semua hasil walk jadi SATU program.
    // selain itu: v3 lama, satu .c = satu job. Baris `job` di atas keduanya.
    int link_jobs = (g_make_multi && (g_make_nfile + g_make_njobs) > 0) ? 1 : 0;
    int ntotal = g_make_nexjob + link_jobs + (g_make_multi ? 0 : g_make_njobs);
    if (ntotal == 0) {
        tcc_tag(TCC_YELLOW, "MAKE"); tcc_puts("no .c file found — 0 job(s)"); tcc_nl();
        return g_make_walk_err ? 1 : 0;
    }
    tcc_tag(TCC_CYAN, "MAKE"); tcc_putn((uint32_t)ntotal); tcc_puts(" job(s)"); tcc_nl();

    int ok = 0, fail = 0;
    // Compiler chatter is silenced for EVERY job (the per-file line belongs
    // to [MAKE]), but an explicit `mtcc -q -make …` still suppresses even
    // that one line — remember what the caller asked for.
    int quiet_save = g_quiet;
    g_quiet = 1;
    int show = !quiet_save;

    // (1) mode A — semua sumber -> SATU link
    if (link_jobs) {
        char* srcs[MAKE_MAX_JOBS + MAKE_MAX_FILES];
        int n = 0;
        int lib = g_make_lib_all;
        for (int i = 0; i < g_make_nfile; i++) srcs[n++] = g_make_files[i];
        for (int i = 0; i < g_make_njobs; i++) {
            srcs[n++] = g_make_jobs[i].path;
            if (g_make_jobs[i].is_lib) lib = 1;
        }
        char oname[MAKE_PATH_MAX + 8];
        make_output_name(srcs[0], oname, sizeof(oname), g_make_fmt_elf, 1);
        if (make_run_one(srcs, n, oname, g_make_fmt_elf, lib, show, 0)) fail++;
        else ok++;
    }
    // (2) v3 — auto-walk, satu file = satu job
    if (!g_make_multi) {
        for (int j = 0; j < g_make_njobs; j++) {
            char oname[MAKE_PATH_MAX + 8];
            make_output_name(g_make_jobs[j].path, oname, sizeof(oname),
                             g_make_fmt_elf, 0);
            char* one[1];
            one[0] = g_make_jobs[j].path;
            if (make_run_one(one, 1, oname, g_make_fmt_elf,
                             g_make_jobs[j].is_lib, show, 0)) fail++;
            else ok++;
        }
    }
    // (3) v4 — baris `job`
    for (int j = 0; j < g_make_nexjob; j++) {
        struct MakeExJob* ej = &g_make_exjob[j];
        int fmt = (ej->fmt == 2) ? g_make_fmt_elf : ej->fmt;
        char oname[MAKE_PATH_MAX + 8];
        if (make_job_out_name(ej, fmt, oname, sizeof(oname)) != 0) { fail++; continue; }
        char* srcs[MAKE_EXJOB_SRC];
        for (int i = 0; i < ej->nsrc; i++) srcs[i] = ej->src[i];
        int lib = ej->is_lib || g_make_lib_all;
        if (make_run_one(srcs, ej->nsrc, oname, fmt, lib,
                         show && !ej->quiet, ej->name)) fail++;
        else ok++;
    }
    g_quiet = quiet_save;
    // v0.9.3: copy/move dieksekusi SETELAH semua job sukses — artefak rusak
    // tidak pernah ikut tersebar. v4: `set` (tulis .ecf) mengikuti aturan
    // yang sama — konfigurasi tidak boleh berubah karena build gagal.
    if (fail == 0) {
        if (g_make_nact > 0 && make_acts_run() != 0) fail = 1;
        if (fail == 0 && g_make_nset > 0 && make_sets_run() != 0) fail = 1;
    }
    tcc_tag(fail ? TCC_RED : TCC_GREEN, "MAKE");
    tcc_puts("done: ");
    tcc_putn((uint32_t)ok);
    tcc_puts(" ok, ");
    tcc_putn((uint32_t)fail);
    tcc_puts(" failed (");
    tcc_putn((uint32_t)ntotal);
    tcc_puts(" job(s))");
    tcc_nl();
    return fail ? 1 : 0;
}

// ============================================================================
//  .mrp PROGRAM DRIVER — runs INSIDE Equinox OS.
// ============================================================================
#ifndef MTCC_HOST_TEST

static void tcc_usage(void) {
    os_print("mtcc 0.3 - Equinox OS TinyCC (C subset -> x86-32, int 0x80)\n");
    os_print("usage : mtcc [--debug] [-c] [-o out] [-format mrp|elf]\n");
    os_print("            [-multiple-files] <file.c> [file2.c ...]\n");
    os_print("  mtcc program.c       compile & run immediately (like tcc -run)\n");
    os_print("  mtcc -c program.c    compile to program.mrp, then: run program.mrp\n");
    os_print("  mtcc a.c b.c         MULTI-FILE: compile+LINK all files into ONE\n");
    os_print("                       program (shared symbols + one fixup list).\n");
    os_print("                       main() may live in any file; cross-file calls\n");
    os_print("                       need a prototype (put it in a shared .h).\n");
    os_print("                       File names may also be comma-separated.\n");
    os_print("  -multiple-files     explicit \"link these files\" marker (optional —\n");
    os_print("                       several files are detected automatically)\n");
    os_print("  -o name             output name (with -c; the extension of the\n");
    os_print("                       chosen format is appended when missing).\n");
    os_print("                       Without -o the name comes from the FIRST file.\n");
    os_print("  -format mrp|elf     output format: MRP1 (default) or a static\n");
    os_print("                       ELF32 (ET_EXEC, one PT_LOAD at 0x01000000,\n");
    os_print("                       runnable with: run program.elf)\n");
    os_print("  mtcc -make b.ruf     run a .ruf build recipe (self-hosting\n");
    os_print("                       without equinoxinstall):\n");
    os_print("                       v3: echo/src/exclude/out/lib + copy/move\n");
    os_print("                       v4: multiple_file = True  every `src` source\n");
    os_print("                           links into ONE program;\n");
    os_print("                           job <n> from a.c & b.c to out.mrp\n");
    os_print("                           [format mrp|elf] [flags \"-q --lib\"] [lib];\n");
    os_print("                           format mrp|elf  global output format;\n");
    os_print("                           set key = value  merged into an .ecf\n");
    os_print("                           (ecf <path>) AFTER a fully green build\n");
    os_print("  mtcc --debug ...     verbose compiler info (per-file reads, code/\n");
    os_print("                       data size, exit code). Without --debug only\n");
    os_print("                       the status lines and the program output print.\n");
    os_print("  mtcc -q ...          quiet: no status lines, program output only\n");
    os_print("  mtcc --lib ...       library mode: main() is not required\n");
    os_print("preproc: #include <morph.h> (also stdio/stdlib/string.h) splices\n");
    os_print("         the libc prelude; #include \"file.h\" reads RAMFS;\n");
    os_print("         \"file.c\" is spliced too (single-TU include style);\n");
    os_print("         a relative include also resolves NEXT TO ITS SOURCE file,\n");
    os_print("         so headers live beside their .c regardless of cwd;\n");
    os_print("         #define NAME val / #undef / #ifdef #ifndef #else #endif\n");
    os_print("         macros persist across a file list => include guards work\n");
    os_print("         exactly once for the WHOLE program (like spliced TUs)\n");
    os_print("types   : int char void  *pointers  arrays  struct union enum\n");
    os_print("          typedef  (struct/union BY VALUE as param/return is NOT\n");
    os_print("          supported — pass a pointer; fields are 4-byte aligned)\n");
    os_print("          struct/union/enum init: x = {v1, v2, ...} (global & local,\n");
    os_print("          nested, arrays of struct; union takes the first value)\n");
    os_print("stmts   : if else while for do break continue return\n");
    os_print("          switch case default (default must be LAST)\n");
    os_print("builtins: print printint getkey readline write open read close\n");
    os_print("          malloc sleep gettick getpid exit exec getargs mkfile\n");
    os_print("          file_open file_read file_close file_write\n");
    os_print("          file_read_all file_size file_exists  (Morph.h API)\n");
    os_print("          lseek(fd,off,whence) ring() -> CPL caller (v10.8)\n");
    os_print("net API : net_info(w10) fills network status; net_ping(ip) 0..4\n");
    os_print("libc     : 12 modules in /equinox/libc (morph.h master include)\n");
    os_print("          compile-time splice via #include <morph.h>\n");
    os_print("flags    : -c compile-only   -o <out.mrp> output name\n");
    os_print("           -q quiet    --lib no-main check    --debug verbose\n");
    os_print("           -make <file.ruf>  run a build recipe (v3 + v4, lihat atas)\n");
    os_print("libc prelude: strlen strcmp strncmp strcpy strncpy strcat\n");
    os_print("          strncat strchr strrchr strstr strdup strtok strspn\n");
    os_print("          strcspn strcasecmp memcpy memset memmove memcmp\n");
    os_print("          memchr atoi strtol itoa utoa sscanf ctype(13 fn)\n");
    os_print("          malloc free calloc realloc printf sprintf snprintf\n");
    os_print("          (7 conv args: %d %i %u %x %X %o %p %c %s)\n");
    os_print("          fopen fread fwrite fseek ftell fclose fgetc fputc\n");
    os_print("          puts fputs remove rename strerror abs rand srand\n");
    os_print("          qsort_int qsort_str time getenv abort (via <morph.h>)\n");
    os_print("game API : fb_info(fb) put_pixel(x,y,c) fill_rect(x|w<<16,y|h<<16,c)\n");
    os_print("          draw_line(x0|y0<<16,x1|y1<<16,c) set_clip(x|w<<16,y|h<<16)\n");
    os_print("          pollkey() mouse_state(m) spk_tone(hz) spk_silence()\n");
    os_print("          key_event() 0 / press<<16|rawcode (release+press,\n");
    os_print("          code = (ev<<16)>>16) — the held-key / DOOM path\n");
    os_print("          snd_beep(hz, ms)  queue a timed note (non-blocking)\n");
}

// --debug flag: 1 = show compiler info (read/code/data/exit code),
// 0 = clean mode — program output only ("same as run").
// (redeclared here for documentation only; its definition is above)

MRP_ENTRY {
    (void)api;   // mtcc is pure syscalls (int 0x80) — the mrp_api table is not used

    // The in-OS console parses ANSI SGR natively (stdio.cpp ansi_feed()),
    // so every status line below is genuinely coloured on screen.
    g_color = 1;

    // Exit codes for the 2-thread equinoxinstall pool (SYS_WAIT reads
    // them): 0 = success, 1 = compile/read/write error, 2 = no input.
    #define TCC_EXIT_OK    0
    #define TCC_EXIT_ERR   1
    #define TCC_EXIT_USAGE 2

    // Arguments from the shell: `mtcc -c hello.c` / `mtcc --debug hello.c` /
    // `run mtcc.mrp hello.c` → "…hello.c"
    char args[128];
    int n = syscall2(SYS_GETARGS, (uint32_t)(uintptr_t)args, (uint32_t)sizeof(args));
    if (n < 0 || n > (int)sizeof(args) - 1) n = 0;
    args[n] = '\0';

    // Parse flags (any combination, any order):
    //   -c        compile to <base>.mrp (without running)
    //   --debug   show compiler info
    //   -d        shorthand for --debug
    //   -q        quiet: no success line in -c mode (errors still print)
    //   --lib     library mode: no main() required (check-compile the
    //             /equinox/libc modules; entry stub is a bare ret)
    char* p = args;
    while (*p == ' ') p++;
    int compile_only = 0;
    int make_mode = 0;
    g_debug = 0;
    g_quiet = 0;
    g_lib_mode = 0;
    {   /* 0.5: default flags dari .config/mtcc.ecf — CLI menambah di
         * atas ini (mtcc tak punya flag "negatif", jadi default + CLI). */
        char v[64];
        if (mtcc_cfg_get("flags.default", v, sizeof(v))) {
            char* fp = v;
            while (*fp) {
                while (*fp == ' ') fp++;
                if (!*fp) break;
                char ft[16]; uint32_t k = 0;
                while (*fp && *fp != ' ' && k < sizeof(ft) - 1) ft[k++] = *fp++;
                ft[k] = '\0';
                if (m_streq(ft, "-q")) g_quiet = 1;
                else if (m_streq(ft, "-d") || m_streq(ft, "--debug")) g_debug = 1;
                else if (m_streq(ft, "--lib")) g_lib_mode = 1;
            }
        }
    }
    while (p[0] == '-') {
        if (p[1] == 'c' && (p[2] == ' ' || p[2] == '\0')) {
            compile_only = 1;
            p += 2;
        } else if (p[1] == '-' && p[2] == 'd' && p[3] == 'e' && p[4] == 'b'
                   && p[5] == 'u' && p[6] == 'g'
                   && (p[7] == ' ' || p[7] == '\0')) {
            g_debug = 1;
            p += 7;
        } else if (p[1] == '-' && p[2] == 'l' && p[3] == 'i' && p[4] == 'b'
                   && (p[5] == ' ' || p[5] == '\0')) {
            g_lib_mode = 1;
            p += 5;
        } else if (p[1] == 'm' && p[2] == 'a' && p[3] == 'k' && p[4] == 'e'
                   && (p[5] == ' ' || p[5] == '\0')) {
            make_mode = 1;        // v0.6: mtcc -make <file.ruf>
            p += 5;
        } else if (p[1] == 'q' && (p[2] == ' ' || p[2] == '\0')) {
            g_quiet = 1;
            p += 2;
        } else if (p[1] == 'd' && (p[2] == ' ' || p[2] == '\0')) {
            g_debug = 1;          // shorthand
            p += 2;
        } else if ((p[1] == 'h' || p[1] == '?') && (p[2] == ' ' || p[2] == '\0')) {
            tcc_usage();          // `mtcc -h` / `mtcc -?`
            syscall1(SYS_EXIT, TCC_EXIT_OK);
            return;
        } else if (p[1] == '-' && p[2] == 'h' && (p[3] == ' ' || p[3] == '\0')) {
            tcc_usage();          // `mtcc -help`
            syscall1(SYS_EXIT, TCC_EXIT_OK);
            return;
        } else if (p[1] == '-' && p[2] == 'h' && p[3] == 'e' && p[4] == 'l'
                   && p[5] == 'p' && (p[6] == ' ' || p[6] == '\0')) {
            tcc_usage();          // `mtcc --help`
            syscall1(SYS_EXIT, TCC_EXIT_OK);
            return;
        } else {
            break;                // unknown flag — decided by the scan below
        }
        while (*p == ' ') p++;
    }
    if (*p == '\0') { tcc_usage(); syscall1(SYS_EXIT, TCC_EXIT_USAGE); return; }

    // ---- -make <file.ruf>: multi-file build (v0.6) ----
    if (make_mode) {
        while (*p == ' ') p++;
        char ruf[96];
        uint32_t r = 0;
        while (p[r] && p[r] != ' ' && r < sizeof(ruf) - 1) { ruf[r] = p[r]; r++; }
        ruf[r] = '\0';
        if (!ruf[0]) {
            tcc_tag(TCC_RED, "ERROR"); tcc_puts("-make requires <file.ruf>"); tcc_nl();
            syscall1(SYS_EXIT, TCC_EXIT_USAGE);
            return;
        }
        int rc = mtcc_make_run(ruf);
        syscall1(SYS_EXIT, rc ? TCC_EXIT_ERR : TCC_EXIT_OK);
        return;
    }

    if (g_debug) {
        tcc_puts("\n  ");
        tcc_puts(TCC_CYAN);
        tcc_puts("mtcc");
        tcc_puts(TCC_RESET);
        tcc_puts(" — Equinox OS TinyCC (C subset -> x86-32, int 0x80)\n");
    }

    // ---- file list (multi-file) + -o / -format / -multiple-files ----
    // Usage: mtcc [flags] [-o out] [-format mrp|elf] [-multiple-files]
    //                 <a.c> [b.c ...]   (commas also separate file names)
    static char g_files[12][64];
    int g_nfiles = 0;
    char g_out[64];
    g_out[0] = '\0';
    int g_fmt_elf = 0;
    {   /* 0.5: format.default dari .config/mtcc.ecf (`-format` menang —
         * variabel ini hanya nilai awal sebelum parsing CLI). */
        char v[8];
        if (mtcc_cfg_get("format.default", v, sizeof(v)) &&
            v[0] == 'e' && v[1] == 'l' && v[2] == 'f' && v[3] == '\0')
            g_fmt_elf = 1;
    }
    int g_multi = 0;
    const char* q2 = p;
    while (*q2) {
        while (*q2 == ' ' || *q2 == ',') q2++;
        if (!*q2) break;
        if (q2[0] == '-' && q2[1] == 'o' && (q2[2] == ' ' || q2[2] == '\0')) {
            q2 += 2;
            while (*q2 == ' ') q2++;
            uint32_t k = 0;
            while (q2[k] && q2[k] != ' ' && k < sizeof(g_out) - 1) { g_out[k] = q2[k]; k++; }
            g_out[k] = '\0';
            q2 += k;
        } else if (q2[0] == '-' && q2[1] == 'f' && q2[2] == 'o'
                   && q2[3] == 'r' && q2[4] == 'm' && q2[5] == 'a'
                   && q2[6] == 't' && (q2[7] == ' ' || q2[7] == '\0')) {
            q2 += 7;
            while (*q2 == ' ') q2++;
            char f[8];
            uint32_t k = 0;
            while (q2[k] && q2[k] != ' ' && k < sizeof(f) - 1) { f[k] = q2[k]; k++; }
            f[k] = '\0';
            q2 += k;
            if (f[0] == 'e' && f[1] == 'l' && f[2] == 'f' && f[3] == '\0') g_fmt_elf = 1;
            else if (f[0] == 'm' && f[1] == 'r' && f[2] == 'p' && f[3] == '\0') g_fmt_elf = 0;
            else {
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("unknown format '");
                tcc_puts(f);
                tcc_puts("' (expected: mrp | elf)");
                tcc_nl();
                syscall1(SYS_EXIT, TCC_EXIT_USAGE);
                return;
            }
        } else if (q2[0] == '-' && q2[1] == 'm'
                   && q2[2] == 'u' && q2[3] == 'l' && q2[4] == 't'
                   && q2[5] == 'i' && q2[6] == 'p' && q2[7] == 'l'
                   && q2[8] == 'e' && q2[9] == '-'
                   && q2[10] == 'f' && q2[11] == 'i' && q2[12] == 'l'
                   && q2[13] == 'e' && q2[14] == 's'
                   && (q2[15] == ' ' || q2[15] == '\0')) {
            g_multi = 1;
            q2 += 15;
        } else if (q2[0] == '-' && q2[1] != '\0') {
            /* Flags are accepted in front of the files AND behind them;
             * anything left is reported instead of being silently treated
             * as a file name (professional CLIs never guess). */
            if (q2[1] == 'c' && (q2[2] == ' ' || q2[2] == '\0')) {
                compile_only = 1; q2 += 2;
            } else if (q2[1] == 'q' && (q2[2] == ' ' || q2[2] == '\0')) {
                g_quiet = 1; q2 += 2;
            } else if (q2[1] == 'd' && (q2[2] == ' ' || q2[2] == '\0')) {
                g_debug = 1; q2 += 2;
            } else if (q2[1] == '-' && q2[2] == 'd' && q2[3] == 'e'
                       && q2[4] == 'b' && q2[5] == 'u' && q2[6] == 'g'
                       && (q2[7] == ' ' || q2[7] == '\0')) {
                g_debug = 1; q2 += 7;
            } else if (q2[1] == '-' && q2[2] == 'l' && q2[3] == 'i'
                       && q2[4] == 'b' && (q2[5] == ' ' || q2[5] == '\0')) {
                g_lib_mode = 1; g_quiet = 1; q2 += 5;
            } else {
                char bad[26];
                uint32_t k = 0;
                while (q2[k] && q2[k] != ' ' && k < sizeof(bad) - 2) { bad[k] = q2[k]; k++; }
                bad[k] = '\0';
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("unknown option '");
                tcc_puts(bad);
                tcc_puts("'");
                tcc_nl();
                syscall1(SYS_EXIT, TCC_EXIT_USAGE);
                return;
            }
        } else {
            /* source file — commas are accepted as separators too */
            if (g_nfiles >= 12) {
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("too many source files (max 12)");
                tcc_nl();
                syscall1(SYS_EXIT, TCC_EXIT_USAGE);
                return;
            }
            uint32_t raw = 0;
            while (q2[raw] && q2[raw] != ' ' && q2[raw] != ',') raw++;
            uint32_t k = raw;
            if (k > sizeof(g_files[0]) - 1) k = sizeof(g_files[0]) - 1;
            for (uint32_t i = 0; i < k; i++) g_files[g_nfiles][i] = q2[i];
            g_files[g_nfiles][k] = '\0';
            q2 += raw;
            g_nfiles++;
        }
    }
    if (g_nfiles == 0) { tcc_usage(); syscall1(SYS_EXIT, TCC_EXIT_USAGE); return; }
    if (g_multi && g_nfiles < 2 && !g_quiet) {
        tcc_tag(TCC_YELLOW, "WARNING");
        tcc_puts("-multiple-files was given but only one source file was listed");
        tcc_nl();
    }

    char* srcs[12];
    uint32_t src_lens[12];
    for (int i = 0; i < g_nfiles; i++) {
        if (os_read_file(g_files[i], &srcs[i], &src_lens[i]) != 0 || !srcs[i]) {
            tcc_tag(TCC_RED, "ERROR");
            tcc_puts("cannot read '");
            tcc_puts(g_files[i]);
            tcc_puts("' (file in RAMFS? try `ls /test`)");
            tcc_nl();
            syscall1(SYS_EXIT, TCC_EXIT_ERR);
            return;
        }
        if (g_debug) {
            tcc_tag(TCC_CYAN, "READ");
            tcc_puts(g_files[i]);
            tcc_puts(" (");
            tcc_putn(src_lens[i]);
            tcc_puts(" bytes)");
            tcc_nl();
        }
    }

    MtccOut out;
    int cr = mtcc_compile_start();
    if (cr == 0) {
        for (int i = 0; i < g_nfiles && cr == 0; i++)
            if (mtcc_compile_add(srcs[i], src_lens[i], g_files[i]) != 0) cr = 1;
        if (cr == 0) cr = mtcc_compile_finish(&out);
    }
    if (cr != 0) {
        if (cr == 2) {
            tcc_tag(TCC_RED, "ERROR");
            tcc_puts("out of memory (4 MB compiler arena full)");
            tcc_nl();
        } else {
            tcc_report_error();
            if (g_debug) {
                // ---- state dump (only when --debug asked for it) ----
                os_print("[dbg] fn=");
                os_printint(S.func_count);
                os_print(" gv=");
                os_printint(S.gvar_count);
                os_print(" lc=");
                os_printint(S.local_count);
                os_print(" sd=");
                os_printint((uint32_t)S.scope_depth);
                os_print(" fx=");
                os_printint(S.fixup_count);
                os_print(" pos=");
                os_printint(S.src_pos);
                os_print(" tok=");
                os_printint((uint32_t)S.tok);
                os_print("\n");
                for (uint32_t g = 0; g < S.gvar_count && g < 8; g++) {
                    char nb[MTCC_NAME_MAX + 2];
                    uint32_t k = 0;
                    while (k < MTCC_NAME_MAX && S.gvars[g].name[k]) {
                        nb[k] = S.gvars[g].name[k]; k++;
                    }
                    nb[k] = '\0';
                    os_print("[dbg] gvar");
                    os_printint(g);
                    os_print(" name='");
                    os_print(nb);
                    os_print("'\n");
                }
                for (uint32_t f = 0; f < S.func_count && f < 8; f++) {
                    char nb[MTCC_NAME_MAX + 2];
                    uint32_t k = 0;
                    while (k < MTCC_NAME_MAX && S.funcs[f].name[k]) {
                        nb[k] = S.funcs[f].name[k]; k++;
                    }
                    nb[k] = '\0';
                    os_print("[dbg] func");
                    os_printint(f);
                    os_print(" name='");
                    os_print(nb);
                    os_print("'\n");
                }
            }
        }
        syscall1(SYS_EXIT, TCC_EXIT_ERR);
        return;
    }
    // ---- the output name (reported by both -c and run) -----------------
    // v0.3: the output lands NEXT TO THE SOURCE (the input's directory
    // prefix is kept), not in the task cwd — so `mtcc -c /equinox/tools/ls.c`
    // writes /equinox/tools/ls.mrp (SYS_MKFILE resolves the dir part via
    // syscall_resolve).  The name comes from -o if given, else from the
    // FIRST source, and always ends in the extension of the chosen format.
    const char* ext = g_fmt_elf ? ".elf" : ".mrp";
    char oname[64];
    oname[0] = '\0';
    if (g_out[0]) {
        uint32_t j = 0;
        while (g_out[j] && j < sizeof(oname) - 6) { oname[j] = g_out[j]; j++; }
        oname[j] = '\0';
        uint32_t el = 0, sl = j;
        int has = 1;
        while (ext[el]) el++;
        if (sl < el) has = 0;
        for (uint32_t k = 0; has && k < el; k++)
            if (oname[sl - el + k] != ext[k]) has = 0;
        if (!has) {
            for (uint32_t k = 0; k < el; k++) oname[sl + k] = ext[k];
            oname[sl + el] = '\0';
        }
    } else {
        const char* av0 = g_files[0];
        const char* base = av0;
        uint32_t dlen = 0;
        for (const char* q = av0; *q; q++) {
            if (*q == '/') { base = q + 1; dlen = (uint32_t)(q + 1 - av0); }
        }
        uint32_t j = 0;
        while (j < dlen && j < sizeof(oname) - 8) { oname[j] = av0[j]; j++; }
        uint32_t k = 0;
        while (base[k] && base[k] != '.' && j < sizeof(oname) - 5) {
            oname[j] = base[k]; j++; k++;
        }
        uint32_t el = 0;
        while (ext[el] && j < sizeof(oname) - 1) { oname[j] = ext[el]; j++; el++; }
        oname[j] = '\0';
    }

    // ---- the status lines every build prints (suppressed by -q) --------
    //   [COMPILE] hello.c (+ b.c) -> hello.mrp      what we are building
    //   [LINK]    1 file, 3 function(s), 1139 B code how big it came out
    if (!g_quiet) {
        tcc_report_compile(g_files[0], g_nfiles > 1 ? g_files[1] : "",
                           compile_only ? oname : "<in-memory>");
        tcc_tag(TCC_CYAN, "LINK");
        tcc_putn((uint32_t)g_nfiles);
        tcc_puts(g_nfiles == 1 ? " file, " : " files, ");
        tcc_putn(out.nfuncs);
        tcc_puts(" function(s), ");
        tcc_putn(out.code_len);
        tcc_puts(" bytes code, ");
        tcc_putn(out.data_len);
        tcc_puts(" bytes data");
        tcc_nl();
    }
    if (!compile_only && (g_out[0] || g_fmt_elf) && !g_quiet) {
        tcc_tag(TCC_YELLOW, "WARNING");
        tcc_puts("-o / -format only apply to -c (run mode never writes a file)");
        tcc_nl();
    }

    if (compile_only) {
        // ---- -c mode: build the image (mrp | elf) and write it ---------
        uint32_t cap;
        if (g_fmt_elf) cap = ELF_PAYLOAD + out.code_len + 4 + out.data_len + 64;
        else           cap = MRP_HEADER_SIZE + out.code_len + 4 + out.data_len;
        uint8_t* image = (uint8_t*)os_alloc(cap);
        if (!image) {
            tcc_tag(TCC_RED, "ERROR");
            tcc_puts("not enough memory for the image");
            tcc_nl();
            syscall1(SYS_EXIT, TCC_EXIT_ERR);
            return;
        }
        uint32_t total;
        if (g_fmt_elf) {
            total = mtcc_build_image_elf(&out, image, cap);
            if (total == 0) {
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("ELF image is 0 bytes");
                tcc_nl();
                syscall1(SYS_EXIT, TCC_EXIT_ERR);
                return;
            }
            int why = mtcc_elf_check(image, total);
            if (why != 0) {
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("ELF image failed validation (check #");
                tcc_putn((uint32_t)why);
                tcc_puts(")");
                tcc_nl();
                syscall1(SYS_EXIT, TCC_EXIT_ERR);
                return;
            }
        } else {
            total = mtcc_build_image(&out, image, cap);
            if (total == 0) {
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("MRP image is 0 bytes");
                tcc_nl();
                syscall1(SYS_EXIT, TCC_EXIT_ERR);
                return;
            }
            // Defensive: validate it ourselves before writing (mrp_format.h)
            enum mrp_validate_reason vr;
            if (!is_valid_mrp(image, total, &vr)) {
                tcc_tag(TCC_RED, "ERROR");
                tcc_puts("MRP image failed validation: ");
                tcc_puts(mrp_reason_str(vr));
                tcc_nl();
                syscall1(SYS_EXIT, TCC_EXIT_ERR);
                return;
            }
        }
        if (os_write_file(oname, image, total) != 0) {
            tcc_tag(TCC_RED, "ERROR");
            tcc_puts("cannot write ");
            tcc_puts(oname);
            tcc_puts(" to RAMFS");
            tcc_nl();
            syscall1(SYS_EXIT, TCC_EXIT_ERR);
            return;
        }
        if (!g_quiet) {
            tcc_tag(TCC_CYAN, "OUTPUT");
            tcc_puts(oname);
            tcc_puts(" (");
            tcc_putn(total);
            tcc_puts(" bytes) — run ");
            tcc_puts(oname);
            tcc_nl();
        }
        syscall1(SYS_EXIT, TCC_EXIT_OK);
    } else {
        // ---- run mode: patch with the real addresses & call directly ----
        mtcc_patch(&out, (uint32_t)(uintptr_t)out.code, (uint32_t)(uintptr_t)out.data);
        if (g_debug) {
            tcc_tag(TCC_CYAN, "RUN");
            tcc_puts("calling main() directly (like tcc -run)");
            tcc_nl();
        }
        typedef int (*entry_fn)(void);
        int rc = (int)((entry_fn)(uintptr_t)out.code)();

        if (g_debug) {
            Func* mf = find_func("main");
            tcc_tag(TCC_CYAN, "EXIT");
            if (mf && !(mf->ret.base == TY_VOID && mf->ret.ptr == 0)) {
                if (rc < 0) { tcc_puts("-"); tcc_putn((uint32_t)(-rc)); }
                else        { tcc_putn((uint32_t)rc); }
            } else {
                tcc_puts("done");
            }
            tcc_nl();
        }
    }
}

#endif // !MTCC_HOST_TEST
