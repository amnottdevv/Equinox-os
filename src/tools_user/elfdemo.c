/* elfdemo.c — v0.3 (FR-07): a STATIC ELF32 executable for Equinox OS.
 *
 * Built on the HOST by the normal -m32 toolchain (gcc -nostdlib,
 * mrp_user/elf_link.ld, base 0x01000000) — no .mrp packing, no
 * relocation: the kernel ELF loader maps the PT_LOADs into the
 * task's demand window and jumps to e_entry.
 *
 * Run in-OS:   elfdemo.elf          (or `run elfdemo.elf`)
 *              spawn elfdemo.elf    + `wait` collects status 42
 *
 * Pure int 0x80 ABI (syscall.h): EAX = number, EBX/ECX/EDX = args.
 */

#define SYS_PRINT     10
#define SYS_PRINTINT  11
#define SYS_EXIT      1
#define SYS_GETPID    3
#define SYS_MEMINFO   51

static inline int syscall1(int num, int a1) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(a1)
                     : "memory", "cc");
    return ret;
}

static inline int syscall2(int num, int a1, int a2) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(a1), "c"(a2)
                     : "memory", "cc");
    return ret;
}

static void msg(const char* s)  { (void)syscall1(SYS_PRINT,  (int)s); }
static void num(int n)          { (void)syscall1(SYS_PRINTINT, n); }

/* .bss proof: demand paging hands out ZERO-FILLED pages, so an
 * untouched .bss buffer must read back as all zeros. Global (not
 * static) + 8 KB so it REALLY spans fresh pages beyond .text. */
int bss_buf[2048];

__attribute__((section(".text")))
void _start(void) {
    msg("== ELF demo (static ELF32, demand paging) ==\n");
    msg("hello from an ELF program at ring 3\n");
    msg("my pid: ");  num(syscall1(SYS_GETPID, 0)); msg("\n");

    /* bss zero-fill check */
    int zeros = 1;
    for (int i = 0; i < 2048; i++) {
        if (bss_buf[i] != 0) { zeros = 0; break; }
    }
    msg(zeros ? "bss zero-fill: OK\n" : "bss zero-fill: FAIL\n");

    /* FR-05: meminfo syscall from an ELF program */
    unsigned int w[6];
    if (syscall2(SYS_MEMINFO, (int)w, 0) == 0) {
        msg("meminfo: pool "); num((int)w[0]); msg(" KB, free ");
        num((int)w[1]); msg(" KB, faulted "); num((int)w[2]);
        msg(" KB\n");
    }

    msg("ELFDEMO RESULT: ");
    msg(zeros ? "PASS" : "FAIL");
    msg("\n");
    syscall1(SYS_EXIT, 42);   /* wait() reports 42 */
}
