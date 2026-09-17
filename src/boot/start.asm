; ============================================================
; start.asm — Multiboot entry with custom GDT + VBE framebuffer request
; GRUB loads this at 0x100000 (physical), CPU in 32-bit protected
; mode already (multiboot loaders always hand off in PM), with:
;   EAX = multiboot magic (0x2BADB002)
;   EBX = pointer to multiboot_info_t
;
; IMPORTANT: We do NOT set the video mode ourselves via BIOS
; int 0x10 here (or anywhere in the kernel) - by the time this
; code runs we're already in 32-bit protected mode with no valid
; real-mode IVT, so int 0x10 does NOT call the BIOS. It instead
; hits whatever is in IDT vector 16 (once idt_init() has run) or
; triple-faults the CPU (if IDT is not yet set up) -> instant
; reboot -> bootloop. Instead we ask GRUB to set the mode for us
; via the multiboot video-mode request fields below; GRUB does
; the real-mode BIOS call *before* jumping here, then hands us
; the resulting framebuffer address/width/height/bpp inside
; multiboot_info_t (read by kernel.cpp's vesa_init_from_multiboot()).
; ============================================================

; ------------------------------------------------------------
; Staging area for relocated boot modules (.mrp files).
;
; WHY THIS EXISTS: .bss is NOBITS - it has zero bytes in the
; kernel ELF / ISO, so GRUB's free-memory bookkeeping does not
; consider it "used" by the kernel image. That means GRUB is
; free to (and in practice does) load `module /boot/*.mrp`
; entries from grub.cfg directly on top of the physical range
; our .bss ends up occupying (~0x180000-0x221140). This code
; used to zero that whole range to clear .bss BEFORE any module
; was ever read, wiping out the .mrp files before
; mrp_bootloader_load_modules() (called later from kernel_main)
; ever got to them - even though they were correctly present in
; the ISO and correctly listed in grub.cfg.
;
; FIX: right after GRUB hands off (before .bss is cleared), walk
; mb_info->mods[] while the original module bytes are still
; intact and copy each one here, then rewrite that module's
; mod_start/mod_end in place so mrp_bootloader_load_modules()
; picks up the relocated copy transparently - no C-side changes
; needed.
;
; Placement: 0x2800000 (40 MiB) — v10.9 "DOOM layout". Modules are
;   copied here BEFORE .bss clear / heap init, and BIG modules
;   (>= 128 KB, e.g. doom1.wad 4.2 MB) are kept here for the whole
;   session: the RAMFS references this memory in place (zero-copy,
;   fs_reference_binary) so a multi-megabyte WAD never has to fit in
;   the 2 MB kernel heap. The region is supervisor-only in the
;   paging map, so ring 3 can only reach it through file syscalls.
; New memory map (QEMU -m 64, all within the 64 MB identity map):
;   kernel image + .bss  ~0x100000 - 0x221140  (~1-2.1 MiB)
;   KERNEL_HEAP           0x300000 - 0x500000  (3-5 MiB, malloc.cpp)
;   MRP_USER arena        0x500000 - 0x2600000 (5-38 MiB, 33 MB!)
;   trampoline+guard+stk  0x2600000-0x2702000  (user stack 1 MB)
;   MODULE_STAGE (here)   0x2800000-0x3400000  (40-52 MiB, 12 MB)
; If the kernel .bss ever grows past 0x2800000, or the user arena
; moves again, bump MODULE_STAGE_ADDR below to match.
; ------------------------------------------------------------
MODULE_STAGE_ADDR equ 0x2800000        ; 40 MiB
MODULE_STAGE_SIZE equ 0xC00000          ; 12 MiB reserved for staged modules
MODULE_STAGE_END  equ MODULE_STAGE_ADDR + MODULE_STAGE_SIZE

section .multiboot
align 4
    dd 0x1BADB002                          ; magic
    dd 0x00000007                          ; flags: bit0 align modules, bit1 meminfo, bit2 video mode request
    dd -(0x1BADB002 + 0x00000007)          ; checksum

    ; --- present only because flags bit 2 is set ---
    ; 1366x768x32 request (user's target resolution). GRUB picks the
    ; closest available VBE mode when this exact one is unavailable
    ; (qemu std VBE has a fixed mode list); grub.cfg's gfxpayload
    ; chain also provides explicit fallbacks.
    dd 0                                   ; mode_type: 0 = linear graphics (RGB), 1 = EGA text
    dd 1366                                ; width  (preferred - GRUB falls back to closest supported)
    dd 768                                 ; height
    dd 32                                  ; depth (bits per pixel)

; ============================================================
;  Stack (reserved in .bss so it's part of the memory image
;  GRUB allocates for us, instead of a magic hardcoded address)
; ============================================================
section .bss
align 16
stack_bottom:
    resb 65536      ; 64 KB stack
stack_top:

; ============================================================
;  Entry point
; ============================================================
section .text
global _start
extern kernel_main
extern _bss_start
extern _bss_end

_start:
    cli

    ; ------------------------------------------------------------
    ; Save what GRUB gave us BEFORE we touch eax/ebx for anything
    ; else (GDT/segment reload clobbers ax, so this must come first)
    ; ------------------------------------------------------------
    mov edi, eax        ; edi = multiboot magic
    mov esi, ebx         ; esi = multiboot_info_t*

    ; ------------------------------------------------------------
    ; Stack
    ; ------------------------------------------------------------
    mov esp, stack_top

    ; ------------------------------------------------------------
    ; Load custom flat GDT
    ; ------------------------------------------------------------
    lgdt [gdt_ptr]

    ; ------------------------------------------------------------
    ; Reload CS via far jump, then reload data segments
    ; ------------------------------------------------------------
    jmp 0x08:.reload_cs
.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ------------------------------------------------------------
    ; SSE/SSE2 for userland (v10.9). The kernel itself is built with
    ; -mgeneral-regs-only where it matters, but hosted .mrp programs
    ; (g++ -m32 -O2) may emit SSE for struct copies / inline memcpy.
    ; CR4.OSFXSR + CR4.OSXMMEXCPT make FXSAVE + SSE exceptions sane;
    ; fninit + CR0.EM/MP/TS are handled in kernel_main (FPU block).
    ; ------------------------------------------------------------
    mov eax, cr4
    or eax, 0x600              ; bit9 OSFXSR | bit10 OSXMMEXCPT
    mov cr4, eax
    mov eax, cr0
    and eax, 0xFFFFFFF7        ; clear TS (bit 3) - no #NM trap
    mov cr0, eax

    ; ------------------------------------------------------------
    ; Stash magic/mb_info on the stack now that stack+segments are
    ; live. This frees up edi/esi so the module-relocation block
    ; below can use them for `rep movsb`, which hardcodes esi as
    ; source and edi as destination - we get them back afterwards.
    ; ------------------------------------------------------------
    push edi              ; [esp+4] = magic (after next push)
    push esi              ; [esp+0] = mb_info*

    ; ------------------------------------------------------------
    ; Relocate boot modules (.mrp files) OUT of low memory BEFORE
    ; .bss gets cleared - see the big comment block up top for why.
    ; Scratch registers used freely here: eax, ebx, ecx, edx, esi,
    ; edi, ebp. mb_info*/magic are safe on the stack meanwhile.
    ; ------------------------------------------------------------
    mov ebx, esi                 ; ebx = mb_info* (kept for field access)
    mov eax, [ebx + 0]           ; mb_info->flags
    test eax, 0x8                ; MULTIBOOT_INFO_MODS (bit 3)
    jz .modules_done

    mov ecx, [ebx + 20]          ; mb_info->mods_count
    test ecx, ecx
    jz .modules_done

    mov ebp, [ebx + 24]          ; ebp = mods_addr (array of multiboot_mod_entry, 16 bytes each)
    mov edx, MODULE_STAGE_ADDR   ; edx = running staging pointer (next free byte)

.mod_loop:
    push ecx                     ; save remaining module count across this iteration

    mov eax, [ebp + 0]           ; mod_start
    mov esi, eax                 ; esi = copy source
    mov eax, [ebp + 4]           ; mod_end
    sub eax, esi                 ; eax = module size in bytes
    mov ecx, eax                 ; ecx = size (rep movsb counter)

    ; bounds-check against the staging area so one oversized/bad
    ; module can't walk this off into unmapped memory
    mov eax, edx
    add eax, ecx
    cmp eax, MODULE_STAGE_END
    ja .mod_skip                 ; won't fit - leave this entry pointing at its original (risky) address

    mov edi, edx                 ; edi = copy destination = current staging pointer
    cld
    rep movsb                    ; copy ecx bytes from [esi] to [edi]; advances esi/edi

    mov [ebp + 0], edx           ; rewrite mod_start -> new location
    mov [ebp + 4], edi           ; rewrite mod_end   -> new location + size (edi already there)
    mov edx, edi                 ; advance staging pointer past this module

.mod_skip:
    add ebp, 16                  ; sizeof(multiboot_mod_entry) -> next entry
    pop ecx
    dec ecx
    jnz .mod_loop

.modules_done:

    ; ------------------------------------------------------------
    ; Restore magic/mb_info now that esi/edi are free again
    ; ------------------------------------------------------------
    pop esi               ; mb_info*
    pop edi               ; magic

    ; ------------------------------------------------------------
    ; Clear .bss - GRUB does NOT guarantee this is zeroed.
    ; Every "= 0" / default-initialized static or global in the
    ; whole kernel lives here (in_panic, kbd_head/kbd_tail,
    ; mouse_state, vesa_fb_addr, etc). Skipping this means those
    ; start out as leftover garbage from whatever was in RAM
    ; before, which causes intermittent, hard-to-reproduce hangs
    ; and crashes that look exactly like random bootloops.
    ; Does not touch edi/esi so the saved multiboot regs survive.
    ; By this point any boot module has already been copied out
    ; to MODULE_STAGE_ADDR above, so this is safe even though it
    ; may have originally overlapped a module.
    ; ------------------------------------------------------------
    mov edx, _bss_start
    mov ecx, _bss_end
    sub ecx, edx
.clear_bss:
    test ecx, ecx
    jz .bss_done
    mov byte [edx], 0
    inc edx
    dec ecx
    jmp .clear_bss
.bss_done:

    ; ------------------------------------------------------------
    ; Call kernel_main(uint32_t mb_magic, multiboot_info_t* mb_info)
    ; cdecl: push args right-to-left (mb_info first, mb_magic last)
    ; ------------------------------------------------------------
    push esi             ; arg2: mb_info
    push edi             ; arg1: mb_magic
    call kernel_main
    add esp, 8            ; caller cleans up (cdecl)

    ; Should never return, but just in case:
    cli
    hlt
    jmp $

; ============================================================
;  GDT (flat, 32-bit) — v10.7 RING 3
;  New entries compared to the old version (which only had null +
;  kernel code + kernel data, all DPL=0):
;    0x18 user code   (DPL=3, base 0, limit 4GB)  — executed by .mrp
;        programs after the loader jumps through iret (RPL=3)
;    0x20 user data   (DPL=3, base 0, limit 4GB)  — user DS/ES/FS/GS/SS
;    0x28 TSS         (system descriptor 0x89)    — base/limit ZERO
;        here because the TSS lives in the kernel .bss (the address
;        is only known at link time); tss_init() in usermode.cpp
;        patches this descriptor's base/limit bytes and THEN executes
;        `ltr 0x28`. Patching .text from CPL0 is safe: Equinox OS paging
;        keeps CR0.WP=0 (supervisors may write read-only pages), and
;        the GDTR still points at the same table — entries are re-
;        read on every use.
;
;  Selector conventions (used by usermode.cpp / the syscall path):
;    kernel code 0x08 | kernel data 0x10
;    user   code 0x1B (0x18|3) | user data 0x23 (0x20|3)
;    TSS          0x28
; ============================================================
align 8
gdt:
    dq 0                    ; 0x00 null descriptor
    ; 0x08 kernel code (DPL=0)
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00
    ; 0x10 kernel data (DPL=0)
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00
    ; 0x18 user code (DPL=3) — present, code, readable, accessed-by-CPL3
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11111010b            ; 0xFA
    db 11001111b            ; 4KB gran, 32-bit
    db 0x00
    ; 0x20 user data (DPL=3) — present, data, writable
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11110010b            ; 0xF2
    db 11001111b
    db 0x00
    ; 0x28 TSS — ditambal runtime oleh tss_init() (base = &tss di .bss)
global gdt_tss
gdt_tss:
    dw 0x0067               ; limit = sizeof(tss)-1 (104-1)
    dw 0x0000               ; base [7:0]  (ditambal)
    db 0x00                 ; base [15:8] (ditambal)
    db 10001001b            ; 0x89: present, 32-bit available TSS
    db 0x00                 ; flags/limit[19:16] - small limit, 0
    db 0x00                 ; base [31:24] (ditambal)
gdt_end:

gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt
