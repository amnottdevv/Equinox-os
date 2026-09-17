/*
 * Linker script for .mrp programs (userland side).
 *
 * IMPORTANT FIX (revision after a bug was found): this script PREVIOUSLY
 * linked at address 0 (`. = 0;`). That was WRONG and made programs crash
 * or read garbage whenever they touched a static variable or string
 * literal. The explanation:
 *
 *   Compilation uses -fno-pic -fno-pie (see mrp_pack.py) so the code is
 *   simple & fast without PIC overhead. BUT the consequence: the compiler
 *   embeds ABSOLUTE addresses directly in instructions to access static
 *   data / string literals (not addresses relative to the instruction
 *   pointer). Those absolute addresses are computed from the LINK-TIME
 *   address (set by `. = ...` in this script).
 *
 *   If we link at address 0 while the loader actually COPIES & RUNS the
 *   program somewhere around 0x500000 (see mrp_loader.cpp / the MRP arena
 *   in malloc.cpp), an instruction that meant "fetch the string at
 *   0x00000042" is now completely wrong -- the string really lives at
 *   0x50000042 when the load base differs, not at 0x00000042.
 *
 * FIX: link at EXACTLY the runtime address where the loader will place
 * the code, because:
 *   1. mrp_heap_init() ALWAYS resets the MRP arena to one big whole
 *      block before loading new code (see mrp_loader.cpp)
 *   2. mrp_alloc(code_size) is the FIRST allocation in the freshly
 *      reset arena -> first-fit always returns the address right after
 *      the first block_header
 *   3. sizeof(block_header) in malloc.cpp = 16 bytes (4 magic + 4 size +
 *      1 free + 3 alignment padding + 4 next pointer, on i686 32-bit)
 *
 * So the runtime address is ALWAYS deterministic: MRP_HEAP_START
 * (0x500000) + 16 = 0x500010. Hence we link there too.
 *
 * THIS IS FRAGILE -- if any of the three facts above changes
 * (block_header size changes because a field is added/removed,
 * MRP_HEAP_START moves, or the loader stops resetting the arena before
 * each load), the 0x500010 number here MUST be adjusted too, or every
 * old .mrp program will crash again from wrong absolute addresses.
 * Medium-term TODO: generate this constant automatically (e.g. via a
 * shared header also included by mrp_pack.py) instead of the hardcoded
 * value -- or, better, move to real -fpic code once that is feasible
 * (usually together with Ring 3 work, since PIC + a variable relocation
 * base only pays off fully when the loader supports multiple load
 * addresses, not the single fixed one it has today).
 */

ENTRY(_start)

/* Linker-script constant (not a C preprocessor -- GNU ld has its own
 * assignment syntax at the SECTIONS top level; valid & portable without
 * extra preprocessing). */
MRP_LOAD_BASE = 0x500010;

SECTIONS {
    . = MRP_LOAD_BASE;

    /* v10.9 (DOOM): .start is now its OWN output section, not part of
     * .text. Reason: an output section's alignment = the max alignment
     * of all its inputs -- doomgeneric links against a 32-bit libgcc
     * whose .text has alignment 64, so if *(.start) stayed inside
     * .text, ALL of .text (including _start) would shift to 0x500040
     * and entry_offset=0 would point at the wrong address. With a
     * separate .start, the .start input (alignment 16; the base
     * 0x500010 is already 16-aligned) ALWAYS lands exactly at
     * MRP_LOAD_BASE, and .text is free to align 64 after it. Old
     * program layouts do not change (.start code stays the first
     * bytes of the image). */
    .start : {
        KEEP(*(.start))   /* _start MUST sit at offset 0 of the image */
    }

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4) {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(4) {
        _mrp_bss_start = .;
        *(.bss)
        *(COMMON)
        _mrp_bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.gnu.build-id)
        *(.note.gnu.property)
        *(.note*)
    }
}
