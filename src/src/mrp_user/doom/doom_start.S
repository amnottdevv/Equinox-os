/* ============================================================================
 *  doom_start.S — .mrp entry stub (v10.9)
 * ----------------------------------------------------------------------------
 *  The MRP loader jumps to image offset 0 with the user stack set up by
 *  user3_launch as [stub_exit][legacy_arg] — i.e. a plain C-callable frame.
 *  This stub must be the FIRST bytes of the image at MRP_LOAD_BASE.
 *
 *  Why assembly: gcc gives the .start input section an alignment of 32
 *  (even with aligned(16) on the function), and MRP_LOAD_BASE 0x500010
 *  is only 16-aligned — the linker would shift _start to 0x500020 and
 *  entry_offset 0 would land on padding. Here we force `.align 4`, so
 *  the .start output section always lands exactly on the base.
 * ============================================================================ */

    .section .start, "ax", @progbits
    .align 4
    .globl _start
    .type  _start, @function
_start:
    /* Tail-jump into the C entry: morph_start sees the same frame
     * (return address = exit stub, first argument = legacy api). */
    jmp morph_start
    .size _start, . - _start
