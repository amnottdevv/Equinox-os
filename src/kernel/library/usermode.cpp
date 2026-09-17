/*
 * ============================================================================
 *  usermode.cpp — Ring 3 support: TSS, trampoline, launch & kill path
 * ----------------------------------------------------------------------------
 *  This file is the heart of v10.7. Four parts:
 *
 *   1. TSS + the user interrupt stack (tss_init / user_int_stack)
 *   2. The exit trampoline on the user page (user_trampoline_init)
 *   3. user3_launch - the only path into CPL 3 (iret), plus the
 *      user3_killed landing pad where kern_longjmp touches down
 *   4. user3_terminate / user3_report_fault - the kill path that
 *      returns control to the shell WITHOUT a panic
 *
 *  This file is compiled with -mgeneral-regs-only (FPU_SENSITIVE in
 *  the makefile) because report_fault runs in exception-handler context.
 * ============================================================================
 */

#include "header/usermode.h"
#include "header/stdio.h"       /* printf for the kill report */
#include "header/libstring.h"   /* snprintf (bounded) */
#include "header/paging.h"      /* paging_is_active() utk memmap */
#include <stdint.h>
#include <stddef.h>

/* Defined in the top-level asm block below - extern "C" so the
 * C++ call is not mangled. */
extern "C" void kern_longjmp(uint32_t esp, uint32_t eip);

// ============================================================
//  1. TSS + USER INTERRUPT STACK
// ------------------------------------------------------------
//  A standard 32-bit TSS (104 bytes). We only use SS0/ESP0
//  (the privilege stack for CPL3->CPL0) and iomap_base. iomap_base =
//  sizeof(tss) means there is NO port-IO bitmap -> every in/out
//  from CPL 3 = #GP -> user programs cannot touch hardware
//  directly; the only door is a syscall.
// ============================================================

struct tss_entry {
    uint16_t prev_task;
    uint16_t reserved0;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint16_t es;
    uint16_t cs;
    uint16_t ss;
    uint16_t ds;
    uint16_t fs;
    uint16_t gs;
    uint16_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

static struct tss_entry tss __attribute__((aligned(16)));

/* Dedicated stack for EVERY CPL3->CPL0 transition (syscall 0x80,
 * exceptions, timer/keyboard/mouse IRQs while a user program runs).
 * 16 KB is enough: CPU frame (~50 bytes) + pushal (~32) + a C handler
 * frame + one IRQ stacked on top of a syscall (trap gate 0x80 does
 * not clear IF). Kept separate from the shell/mrp_run stack so the
 * kernel stack frozen while waiting for a program is NEVER touched
 * from an interrupt path. */
static uint8_t user_int_stack[16384] __attribute__((aligned(16)));

/* The GDT TSS entry label from start.asm (6th descriptor, selector 0x28). */
extern "C" uint8_t gdt_tss[8];

void tss_init(void) {
    /* Zero the whole TSS (esp1/ss1/esp2/ss2 = 0, unused - we never
     * use rings 1/2). */
    for (uint32_t i = 0; i < sizeof(tss); i++) {
        ((uint8_t*)&tss)[i] = 0;
    }
    tss.ss0 = KERNEL_DATA_SEL;                 /* 0x10 */
    tss.esp0 = (uint32_t)(uintptr_t)&user_int_stack[sizeof(user_int_stack)];
    tss.iomap_base = (uint16_t)sizeof(tss);    /* no IO bitmap */

    /* Patch the TSS descriptor in the start.asm GDT (base = &tss,
     * limit = sizeof-1). Descriptor format: limit[15:0], base[15:0],
     * base[23:16], access, flags+limit[19:16], base[31:24]. */
    uint32_t base = (uint32_t)(uintptr_t)&tss;
    uint32_t limit = (uint32_t)sizeof(tss) - 1;
    gdt_tss[0] = (uint8_t)(limit & 0xFF);
    gdt_tss[1] = (uint8_t)((limit >> 8) & 0xFF);
    gdt_tss[2] = (uint8_t)(base & 0xFF);
    gdt_tss[3] = (uint8_t)((base >> 8) & 0xFF);
    gdt_tss[4] = (uint8_t)((base >> 16) & 0xFF);
    gdt_tss[7] = (uint8_t)((base >> 24) & 0xFF);
    /* (the access byte 0x89 & flags are already correct from start.asm) */

    /* Load the task register. Once at boot; never task-switches. */
    asm volatile("ltr %w0" : : "rm"((uint16_t)0x28));
}

// ============================================================
//  2. TRAMPOLINE EXIT (halaman user 0x2600000 — v10.9)
// ------------------------------------------------------------
//  An .mrp entry is a plain C function: void _start(void*). If the
//  program just does `return;`, the CPU rets to the address we
//  smuggled onto the user stack. That address = this 11-byte stub
//  (code IN USER MEMORY, so CPL 3 may execute it - kernel code is on
//  supervisor pages):
//      b8 01 00 00 00     mov eax, 1     ; SYS_EXIT
//      bb 00 00 00 00     mov ebx, 0     ; status 0
//      cd 80              int 0x80
//
//  Meaning: a program that forgets exit() still ends cleanly through
//  the official syscall path instead of jumping to a random address.
// ============================================================

static const uint8_t exit_stub[12] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,     /* mov eax, SYS_EXIT */
    0xBB, 0x00, 0x00, 0x00, 0x00,     /* mov ebx, 0        */
    0xCD, 0x80,                        /* int 0x80          */
};

void user_trampoline_init(void) {
    volatile uint8_t* dst = (volatile uint8_t*)(uintptr_t)USER_TRAMPOLINE;
    for (uint32_t i = 0; i < sizeof(exit_stub); i++) {
        dst[i] = exit_stub[i];
    }
}

// ============================================================
//  3. STATE LAUNCH/KILL + ASM CORE
// ------------------------------------------------------------
//  user3_saved_esp/eip = the return point on the mrp_run() kernel stack.
//  Written by user3_launch BEFORE the iret; read by kern_longjmp on the
//  kill path. user3_exit_normal/status = the final result for mrp_run().
//
//  These FOUR variables are deliberately NON-static + extern "C": the
//  top-level asm block below refers to them by name - GCC cannot see
//  those references, so the symbols must be exact and must never be
//  renamed/merged by the optimizer (static could be).
// ============================================================

extern "C" {
uint32_t user3_saved_esp      = 0;
uint32_t user3_saved_eip      = 0;
uint32_t user3_exit_status_val   = 0;
uint32_t user3_exit_normal_val   = 0;
}

int user3_exit_normal(void) {
    return user3_exit_normal_val ? 1 : 0;
}

/*
 *  ALL instruction-level asm lives in ONE top-level block:
 *
 *  kern_longjmp(esp, eip) - a freestanding mini longjmp: set ESP to the
 *  saved value, jmp to the resume label. Never touches EBP
 *  (landing pad memulihkannya dari slot stack). `cld` menjaga
 *  string direction (kernel ABI convention).
 *
 *  user3_launch(entry, user_esp, arg) -> uint32_t (exit status)
 *
 *  The prologue saves ebx/esi/edi/ebp (callee-saved cdecl ABI) so the
 *  caller's C frame (mrp_run_inner) stays intact when execution
 *  "comes back" through the landing pad - not via a normal ret.
 *
 *  The user3_killed landing pad is reached ONLY via kern_longjmp, with
 *  ESP == the ebp value at launch. Slots: [esp]=caller's ebp,
 *  [esp-4]=edi, [esp-8]=esi, [esp-12]=ebx, [esp+4]=the return address.
 */
asm(
    ".global user3_launch\n"
    ".global kern_longjmp\n"
    ".text\n"
    "kern_longjmp:\n"
    "    movl 8(%esp), %eax\n"       /* target eip               */
    "    movl 4(%esp), %esp\n"       /* new esp                  */
    "    cld\n"
    "    jmp *%eax\n"
    "user3_launch:\n"
    /* STANDARD PROLOGUE: push ebp FIRST so the cdecl arguments sit at
     * at [ebp+8/12/16] (first-prologue bug fix: ebx/esi/edi used to be
     * pushed BEFORE ebp, shifting the arguments to [ebp+20+] -
     * the iret EIP was read from the caller's ESI (=0) and the user
     * from EBX (=the entry address) -> the program jumped to 0x0 + the .mrp code
     * overwritten by the stub at entry-8. Thanks, QEMU frame dump.) */
    "    pushl %ebp\n"
    "    movl %esp, %ebp\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    /* kernel resume point - the longjmp lands here (esp == ebp) */
    "    movl %ebp, user3_saved_esp\n"
    "    movl $user3_killed, %eax\n"
    "    movl %eax, user3_saved_eip\n"
    /* cdecl arguments: [ebp+8]=entry, [ebp+12]=user_esp, [ebp+16]=arg */
    "    movl 8(%ebp), %eax\n"          /* eax = entry EIP        */
    "    movl 16(%ebp), %ecx\n"         /* ecx = arg (_start api) */
    "    movl 12(%ebp), %edx\n"         /* edx = user stack top   */
    /* v10.9: 20 bytes of headroom (not 8) so ESP at _start entry
     * == 12 (mod 16) - exactly like a function called with `call`
     * from a 16-aligned stack (i386 System V ABI). Hosted g++ -O2
     * code may use movaps/SSE and assumes this alignment.
     * Layout: [esp]=trampoline ret, [esp+4]=arg, [esp+8..+19]=pad. */
    "    subl $20, %edx\n"
    "    movl $0x2600000, %ebx\n"       /* USER_TRAMPOLINE (v10.9)*/
    "    movl %ebx, (%edx)\n"           /* [esp+0] = ret addr stub*/
    "    movl %ecx, 4(%edx)\n"          /* [esp+4] = arg          */
    /* build the iret frame (push order is the reverse of iret's pops).
     * NOTE: these pushes descend BELOW the freshly pushed ebx/esi/edi
     * slots - those stay intact for the landing pad. */
    "    pushl $0x23\n"                 /* SS  user data | 3      */
    "    pushl %edx\n"                  /* ESP user               */
    "    pushl $0x202\n"                /* EFLAGS: bit1 + IF=1    */
    "    pushl $0x1B\n"                 /* CS  user code | 3      */
    "    pushl %eax\n"                  /* EIP entry              */
    /* the data segment registers must be user BEFORE the iret */
    "    movw $0x23, %ax\n"
    "    mov %ax, %ds\n"
    "    mov %ax, %es\n"
    "    mov %ax, %fs\n"
    "    mov %ax, %gs\n"
    "    iretl\n"                       /* === enter CPL 3 === */
    "user3_killed:\n"
    /* the longjmp lands here: ESP == launch ebp; IF was already set
     * by the terminate path. Slots relative to esp(=ebp) MATCH the
     * prologue above: [esp-4]=ebx, [esp-8]=esi, [esp-12]=edi,
     * [esp]=old ebp, [esp+4]=return address. (FIX: the old version
     * read edi/esi/ebx from the old prologue offsets - EBX and EDI
     * were swapped, corrupting the caller's registers after a kill:
     * the program-name printf turned to garbage.)
     *
     * FIX(v0.1): the "movw $0x10, %ax" load was lost in the v10.14
     * English rewrite -> every user-program exit landed here with
     * EAX = the kern_longjmp target address (0x112dcf) and the bare
     * "mov %ax, %ds" loaded garbage into DS = instant #GP kernel
     * panic. Restored: load the kernel data selector FIRST. */
    "    movw $0x10, %ax\n"
    "    mov %ax, %ds\n"
    "    mov %ax, %es\n"
    "    mov %ax, %fs\n"
    "    mov %ax, %gs\n"
    "    movl -4(%esp), %ebx\n"
    "    movl -8(%esp), %esi\n"
    "    movl -12(%esp), %edi\n"
    "    movl user3_exit_status_val, %eax\n"
    "    movl (%esp), %ebp\n"
    "    addl $4, %esp\n"               /* drop the ebp slot -> ret */
    "    ret\n"                         /* esp = arg1; caller cleans */
);

// ============================================================
//  4. THE KILL PATH
// ------------------------------------------------------------
//  Called from:
//    - syscall_dispatch  : SYS_EXIT from CPL 3 (normal exit)
//    - exception_panic   : a CPL 3 fault (#DE/#PF/#GP/... -> kill)
//  Context: CPL 0 on user_int_stack, IF=0 (interrupt gate) or IF=1
//  (trap gate 0x80). printf is safe here - the console needs no IRQs.
// ============================================================

__attribute__((noreturn))
void user3_terminate(int normal, uint32_t status, const char* msg) {
    user3_exit_normal_val = normal ? 1u : 0u;
    user3_exit_status_val = status;

    if (msg) {
        printf("%s", msg);
    }

    /* The shell/mrp_run runs with IF=1; exception entry clears IF, so
     * re-enable it explicitly before the jump. */
    asm volatile("sti");

    /* Abandon user_int_stack (TSS.ESP0 is constant - the next
     * transition starts from the top again) and land in user3_killed. */
    kern_longjmp(user3_saved_esp, user3_saved_eip);
    __builtin_unreachable();
}

/* Exception names - mirrors the idt.cpp table (lean, no dependency). */
static const char* um_exception_name(uint8_t vec) {
    switch (vec) {
        case 0:  return "Division By Zero";
        case 1:  return "Debug";
        case 3:  return "Breakpoint";
        case 4:  return "Overflow";
        case 5:  return "Bound Range Exceeded";
        case 6:  return "Invalid Opcode";
        case 7:  return "Device Not Available";
        case 8:  return "Double Fault";
        case 10: return "Invalid TSS";
        case 11: return "Segment Not Present";
        case 12: return "Stack-Segment Fault";
        case 13: return "General Protection Fault";
        case 14: return "Page Fault";
        case 16: return "x87 FPU Error";
        case 17: return "Alignment Check";
        default: return "CPU Exception";
    }
}

__attribute__((noreturn))
void user3_report_fault(uint8_t vec, uint32_t err_code, uint32_t eip) {
    char line[128];

    snprintf(line, sizeof(line),
             "[user] program faulted: %s at eip=0x%08x\n",
             um_exception_name(vec), eip);
    printf("%s", line);

    if (vec == 14) {
        uint32_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        snprintf(line, sizeof(line),
                 "[user]   CR2=0x%08x %s/%s/%s%s\n",
                 cr2,
                 (err_code & 2) ? "write" : "read",
                 (err_code & 4) ? "user" : "supervisor",
                 (err_code & 1) ? "present" : "not-present",
                 (cr2 < 0x1000u) ? " (NULL pointer?)" : "");
        printf("%s", line);
    } else if (err_code != 0) {
        snprintf(line, sizeof(line),
                 "[user]   error code 0x%08x\n", err_code);
        printf("%s", line);
    }

    /* Specific hint: OLD .mrp programs (mrp_api_t function-pointer
     * table) die right here by walking into supervisor pages. */
    printf("[user] program terminated - control returned to shell\n");

    user3_terminate(0, 0x80000000u | (uint32_t)vec, NULL);
}

// ============================================================
//  `memmap` - memory-map diagnostics + ring-3 status
// ============================================================
void usermode_print_memmap(void) {
    uint32_t cr0, cr3;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    printf("Equinox OS memory map (identity, QEMU -m 64, v10.9 DOOM layout):\n");
    printf("  0x00000000-0x002FFFFF  kernel  image + .bss + kstack\n");
    printf("  0x00300000-0x004FFFFF  kernel  KERNEL_HEAP (malloc)\n");
    printf("  0x00500000-0x025FFFFF  USER    MRP arena 33 MB (code + heap)\n");
    printf("  0x02600000-0x02600FFF  USER    trampoline (exit stub)\n");
    printf("  0x02601000-0x02601FFF  kernel  GUARD page (stack ovf)\n");
    printf("  0x02602000-0x02701FFF  USER    user stack 1 MB\n");
    printf("  0x02800000-0x033FFFFF  kernel  module staging 12 MB (WAD zero-copy)\n");
    printf("  0x03400000-0x03FFFFFF  kernel  free RAM\n");
    printf("paging: %s  CR0=0x%08x CR3=0x%08x\n",
           paging_is_active() ? "ON (supervisor/user split aktif)" : "OFF",
           cr0, cr3);
    printf("ring 3: TSS ESP0=0x%08x  user CS=0x1B DS=0x23\n",
           (uint32_t)(uintptr_t)&user_int_stack[sizeof(user_int_stack)]);
}
