/* elf_link.ld — linker script for static ELF32 user programs
 * (Equinox OS v0.3, FR-07).
 *
 * The kernel ELF loader accepts PT_LOAD segments whose p_vaddr is
 * inside [0x00800000, 0x02000000) — 8..32 MB, within the task-private
 * PDEs and the syscall uaccess window. Heap arena (SYS_MALLOC) for
 * ELF tasks is reserved separately at 0x02000000..0x02200000.
 *
 * The stack is the shared 1 MB user stack at 0x02602000 (the same
 * one .mrp programs use) — a plain `ret` from _start lands on the
 * kernel-written exit trampoline, so forgetting exit() is safe.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x01000000;

    .text : {
        *(.text*)
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
