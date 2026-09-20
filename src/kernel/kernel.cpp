// kernel do you love donut's ? 
#include "library/header/stdio.h"
#include "library/header/color.h"
#include "library/header/libstring.h"
#include "library/header/idt.h"
#include "library/header/timer.h"
#include "library/header/malloc.h"
#include "library/header/fs_ram.h"
#include "library/header/mrp_loader.h"
#include "library/header/codeEditor.h"
#include "library/header/tui.h"
#include "library/header/settings.h"
#include "library/header/libaudio.h"
#include "library/header/filemanager.h"
#include "library/header/ps2_mouse.h"
#include "library/header/itoa_atoi.h"
#include "library/header/clock.h"
#include "library/header/sys.h"
#include "library/header/vector.h"
#include "library/header/random.h"
#include "library/header/math_pi.h"
#include "library/header/multiboot.h"
#include "library/header/vesa.h"
#include "net/net.h"                    // v10.11: TCP/IP (lwIP + NE2000)
#include "net/ne2000.h"               // v10.11: netdbg forensik
#include "library/header/pci.h"        // v0.3: PCI enumeration (FR-12)
#include "library/header/panic.h"
#include "library/header/syscall.h"
#include "library/header/paging.h"     // paging_init (v10.7 ring 3)
#include "library/header/usermode.h"   // tss_init / memmap (v10.7 ring 3)
#include "library/header/ata.h"        // v0.2: ATA/IDE PIO driver
#include "library/header/fs_fat32.h"   // v0.2: FAT32 read/write mount
#include "library/header/task.h"       // Phase A: the multitasking scheduler
#include "library/header/serial.h"     // Phase A: QEMU serial debug
#include <stdint.h>

// Phase A: the shell now lives in kernel/shell.cpp (one task per console)
extern "C" void shell_entry(void* arg);

// Forward declaration: mrp_bootloader.cpp (loads GRUB multiboot modules into the RAMFS)
extern "C" int mrp_bootloader_load_modules(const multiboot_info_t* mb_info);

// LVGL (compiled when LVGL source tree is present)
#ifdef HAS_LVGL
  #include "lvgl.h"
  #include "lv_port/lv_port_disp.h"
  #include "lv_port/lv_port_indev.h"
#endif

// ============================================================
//  VESA/VBE state now lives entirely in vesa.cpp - see vesa.h.
//  kernel_main() just calls vesa_init_from_multiboot(mb_magic,
//  mb_info) once at boot; everything else (graphics_test(), and
//  later the GUI/LVGL work) reads through vesa_get_*()/
//  vesa_draw_pixel()/vesa_fill_rect() instead of kernel.cpp
//  keeping its own separate copy of the framebuffer state.
// ============================================================

static void __attribute__((unused)) graphics_test() {
    if (!vesa_is_available()) {
        printf("VESA: Not available (GRUB VBE failed).\n");
        return;
    }

    printf("VESA: Framebuffer at 0x%x, %dx%d, %d bpp\n",
           vesa_get_framebuffer(), vesa_get_width(), vesa_get_height(), vesa_get_bpp());

    // Draw test pattern
    vesa_fill_rect(0, 0, vesa_get_width(), vesa_get_height(), 0x000000);  // black
    vesa_fill_rect(100, 100, 200, 150, 0xFF0000);                        // red
    vesa_fill_rect(350, 100, 200, 150, 0x00FF00);                        // green
    vesa_fill_rect(600, 100, 200, 150, 0x0000FF);                        // blue

    printf("VESA: Test pattern drawn.\n");
}

//  BOOT SPLASH
// ============================================================
// ============================================================
//  BOOT LOG — replaces the percentage-loading splash (fake
//  progress). Every REAL init step is now logged as it happens:
//    Detecting VESA framebuffer (VBE)
//    [ OK ] /equinox/tools/mtcc.mrp  (151.3 KB)  <- mrp_bootloader
//  Lines are also kept in a static ring; after init_display()
//  switches VESA on (which wipes the old text view),
//  boot_log_replay() redraws the whole log on the framebuffer.
//  In the VGA text fallback there is no replay — the screen is
//  still intact.
// ============================================================
#define BOOTLOG_MAX 24
#define BOOTLOG_LEN 71
static char bootlog_lines[BOOTLOG_MAX][BOOTLOG_LEN + 1];
static int  bootlog_count = 0;
static int  bootlog_timer_ready = 0;

/* One formatted boot line: green "[ OK ]" bullet + grey message.
 * Shared by boot_log() and boot_log_replay() so the live boot view
 * and the post-VESA replay look identical. */
static void boot_log_line(const char* msg) {
    set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    printf("[ OK ] ");
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printf("%s\n", msg);
}

static void boot_log(const char* msg) {
    if (bootlog_count < BOOTLOG_MAX) {
        strncpy(bootlog_lines[bootlog_count], msg, BOOTLOG_LEN);
        bootlog_lines[bootlog_count][BOOTLOG_LEN] = '\0';
        bootlog_count++;
    }
    boot_log_line(msg);
    /* Small pacing so it stays readable. ONLY when the timer is
     * alive — sleep_ms() busy-waits on ticks and would hang if
     * called before timer_init(). */
    if (bootlog_timer_ready) sleep_ms(120);
}

static void boot_log_replay(void) {
    if (!vesa_is_available()) return;   /* VGA text: the log is still visible */
    clear_screen();
    /* Purple banner — matches the Equinox emblem palette.
     * set_fg_rgb() snaps to light magenta in the VGA fallback. */
    set_fg_rgb(0xB794FFu);
    printf("Equinox OS v0.2 Beta - booting\n\n");
    for (int i = 0; i < bootlog_count; i++) {
        boot_log_line(bootlog_lines[i]);
    }
}

// ============================================================
//  INTRO ASCII — Equinox emblem (purple / white)
// ============================================================
/* Equinox emblem — 45 chars wide, 21 lines. Rendered with a
 * symmetric white -> purple -> white vertical gradient (the
 * "balance" theme of an equinox: day and night mirroring each
 * other). Colors come from set_fg_rgb() so the VESA console can
 * use exact 24-bit purples; the VGA text fallback snaps to
 * white / light magenta automatically. */
extern "C" void kernel_main(uint32_t mb_magic, multiboot_info_t* mb_info) {
    /* Boot banner. The console is still VGA text at this point; if
     * VESA comes up later, init_display() switches the view and
     * boot_log_replay() redraws the whole boot log. */
    clear_screen();
    /* Purple boot banner — matches the Equinox emblem palette
     * (set_fg_rgb snaps to light magenta in VGA text mode). */
    set_fg_rgb(0xB794FFu);
    printf("Equinox OS v0.2 Beta - booting\n\n");
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    // ============================================================
    // 1. FPU INIT (before anything touches floating point)
    //    fninit = reset the FPU to its default state
    //    CR0.EM = 0 -> don't trap to #NM on FPU instructions
    //    CR0.MP = 1 -> monitor coprocessor
    //    v10.9: + CR4.OSFXSR | CR4.OSXMMEXCPT so user programs built
    //    with g++ -O2 (doomgeneric) may use SSE (movaps etc) without
    //    #UD, and CR0.TS = 0 explicitly (no #NM). The kernel itself
    //    stays safe: files touching interrupt context are built with
    //    -mgeneral-regs-only (FPU_SENSITIVE in the makefile).
    // ============================================================
    boot_log("Initializing FPU + SSE (fninit, CR4.OSFXSR)");
    asm volatile("fninit");
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2);   // clear EM (bit 2)
    cr0 &= ~(1 << 3);   // clear TS (bit 3) — v10.9: no #NM trap
    cr0 |= (1 << 1);    // set MP (bit 1)
    asm volatile("mov %0, %%cr0" : : "r"(cr0));
    uint32_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9)     // OSFXSR: FXSAVE/FXRESTOR + valid SSE state
        |  (1 << 10);   // OSXMMEXCPT: SSE exceptions go through #XM
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    // ============================================================
    // 2. DISABLE INTERRUPTS (not ready yet)
    // ============================================================
    asm volatile("cli");

    // ============================================================
    // 3. LOAD IDT & PIC
    //    idt_init() calls sti at the very end.
    //    Make sure every handler is registered before sti.
    // ============================================================
    boot_log("Loading IDT + remapping PIC (IRQ 32-47)");
    idt_init();

    // ============================================================
    // 4. TIMER INIT (after the IDT; interrupts are ON)
    // ============================================================
    boot_log("Starting PIT timer (100 Hz)");
    timer_init(100);
    asm volatile("sti");
    bootlog_timer_ready = 1;   /* from here on sleep_ms() is safe to use */

    // ============================================================
    // 4b. PS/2 MOUSE INIT
    //     MUST be called manually — pic_remap() inside idt_init()
    //     deliberately masks every slave IRQ (including IRQ12) until
    //     mouse_init() unmasks it and sends command 0xF4 (enable data
    //     reporting) to the device. Without this call the mouse:
    //       - the physical device/QEMU never sends a packet (0xF4 is
    //         never sent)
    //       - IRQ12 stays masked forever even if the device sends data
    //     lv_port_indev_init() only registers the LVGL driver, it
    //     never touches the hardware — which is why the mouse used to
    //     look "connected" but never move.
    // ============================================================
    boot_log("Detecting PS/2 mouse (IRQ12)");
    mouse_init();

    // ============================================================
    // 5. VESA — grab the info from multiboot (if available)
    //    vesa_init_from_multiboot() validates the magic and the
    //    pointers itself, so it is safe to call directly here.
    // ============================================================
    boot_log("Detecting VESA framebuffer (multiboot VBE)");
    vesa_init_from_multiboot(mb_magic, mb_info);

    /* ============================================================
     *  FIX(K9): ENFORCE THE 1366x768x32 MODE (16:9 RATIO)
     *  ------------------------------------------------------------
     *  start.asm + grub.cfg already request 1366x768x32, but the
     *  firmware VBE mode list usually LACKS 1366x768 (QEMU std-VBE
     *  tops out at 1280x1024/1600x1200) -> GRUB falls back to
     *  1024x768 (4:3). vesa_force_mode() programs 1366x768x32
     *  directly through the Bochs VBE DISPI registers when the card
     *  supports it (QEMU/VBox: yes; real hardware without DISPI: the
     *  GRUB mode is kept, nothing breaks).
     *
     *  MUST run BEFORE init_display() so the console size
     *  (term_cols/term_rows) is computed from the final mode.
     * ============================================================ */
    if (vesa_is_available()) {
        int fmode = vesa_force_mode(1366, 768, 32);
        if (fmode == 0) {
            /* The active mode may be rounded by the hardware (QEMU
             * rounds XRES down to a multiple of 8: 1366 -> 1360, still
             * ~16:9). Print what is REALLY active. */
            char vbuf[BOOTLOG_LEN + 1];
            snprintf(vbuf, sizeof(vbuf), "VESA: DISPI override active %ux%u @ %u bpp (16:9)",
                     (unsigned)vesa_get_width(), (unsigned)vesa_get_height(),
                     (unsigned)vesa_get_bpp());
            boot_log(vbuf);
        } else if (fmode == -2) {
            boot_log("VESA: DISPI not available - keeping the GRUB mode");
        } else if (fmode == -3) {
            boot_log("VESA: DISPI override rejected - GRUB mode restored");
        }
        // fmode == -1 is impossible here (vesa_is_available was checked)
    }

    // ============================================================
    //  5b. PAGING + TSS — v10.7 RING 3
    //      MUST run before init_display(): the first console glyph
    //      writes to the LFB (0xFD000000), which is not yet mapped
    //      in the page directory = instant #PF. Identity map
    //      0-64MB + the LFB, split U/S: the MRP arena + trampoline
    //      + the user stack = USER, everything else supervisor.
    //      tss_init() installs SS0:ESP0 (user_int_stack) + ltr — a
    //      prerequisite for EVERY CPL3->CPL0 transition (syscalls,
    //      exceptions, IRQs while a user program runs).
    // ============================================================
    boot_log("Enabling paging (identity map + user/supervisor split)");
    paging_init();

    boot_log("Loading TSS + ring 3 segments (user code 0x18, data 0x20)");
    tss_init();

    // ============================================================
    // 6. DISPLAY INIT — switch printf/put_char to VESA framebuffer
    //    (falls back to VGA text mode automatically if VESA failed)
    // ============================================================
    init_display();
    if (vesa_is_available()) {
        char fbl[BOOTLOG_LEN + 1];
        snprintf(fbl, sizeof(fbl), "Framebuffer: %ux%u @ %u bpp (pitch %u)",
                 (unsigned)vesa_get_width(), (unsigned)vesa_get_height(),
                 (unsigned)vesa_get_bpp(), (unsigned)vesa_get_pitch());
        boot_log(fbl);
    } else {
        boot_log("VESA not available - console fallback VGA text 80x25");
    }
    boot_log_replay();

    // ============================================================
    // 7. THE REST
    // ============================================================
    boot_log("Initializing RAMFS (root, /bin, /dev)");
    fs_init();
    /* The root fs must exist — without it the shell, modules and
     * the loader are all dead. This is the real software panic
     * path. */
    if (!fs_get_root()) {
        kernel_panic("can't init RAMFS", "fs_get_root() == NULL after fs_init()");
    }

    // ============================================================
    // 7a. LOAD GRUB MULTIBOOT MODULES -> RAMFS
    //     This is what lets .mrp files into the RAMFS at boot, so
    //     `./hello.mrp` / `run hello.mrp` work straight from the
    //     shell.
    //
    //     MUST run AFTER fs_init() (needs the root fs) and BEFORE
    //     shell() (the files must exist by the time the user types
    //     ./...).
    // ============================================================
    /* mrp_bootloader prints its own boot-log lines:
     *   [ OK ] /equinox/tools/mtcc.mrp  (151.3 KB)
     *   [ OK ] N file(s) included from M boot module(s)
     * A module named libc* that fails to load triggers
     * kernel_panic("can't load libc") -> reboot in 30 s. */
    boot_log("Loading boot modules (GRUB multiboot)");
    mrp_bootloader_load_modules(mb_info);

    // ============================================================
    //  7b. DISK SUBSYSTEM (v0.2): ATA + FAT32
    //      Scan the IDE buses, parse the MBR of every hard disk
    //      and mount the first FAT32 partition at /mnt (write-
    //      through). Safe no-op without a disk attached.
    // ============================================================
    boot_log("Detecting ATA drives + FAT32 volume (v0.2)");
    fat32_set_boot_log(boot_log);
    {
        /* Arena placement needs the real top of RAM: clamp the
         * FAT cache arena against the multiboot upper-memory figure.
         * (60 MB fallback for non-multiboot debug boots.) */
        uint32_t mem_upper =
            (mb_magic == 0x2BADB002u && mb_info) ? mb_info->mem_upper * 1024u
                                                 : (60u * 1024u * 1024u);
        fat32_boot_init(mem_upper);
    }

    boot_log("Initializing syscall layer (int 0x80)");
    syscall_init();   /* reset the fd table (RAMFS file descriptors) */
    sys_init();

    // ============================================================
    //  v0.3 (FR-12): PCI BUS ENUMERATION
    //  Discover the hardware QEMU actually presents (host bridge,
    //  PIIX3, IDE, USB, VGA, ...) BEFORE net_init: the NIC layer
    //  (kernel/net/nic.c) reports recognized PCI NICs from this
    //  table. `lspci` prints it from the shell.
    // ============================================================
    boot_log("Enumerating PCI bus (v0.3 FR-12)");
    {
        int n = pci_init();
        printf("pci: %d device(s) on the bus (lspci for the table)\n", n);
    }

    boot_log("Kernel loaded successfully");

    /* v10.11: TCP/IP stack (lwIP NO_SYS + NE2000 ISA). Called
     * before the 5-second loading screen so the "net: ne0 up ..."
     * log lines are still readable. Without a NIC -> polite no-op
     * (network commands stay off). */
    net_init();

    /* v10.12: httpd :80 started automatically — the host just
     * sets up a hostfwd (the make run default:
     * hostfwd=tcp::8080-:80) and browses http://localhost:8080/.
     * Without a NIC -> skipped. */
    if (net_is_up() && net_httpd_start()) {
        printf("net: httpd listening on :80 (host: browse :8080 via hostfwd)\n");
    }

    /* Pause so the boot log + file list can be read before the
     * screen switches to the shell intro. Press any key to skip
     * (the keyboard buffer is flushed so nothing leaks into the
     * shell as a command). */
    printf("\n");
    set_color(VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK);
    printf("Boot complete. Starting the shell in 5 seconds...\n");
    set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    printf("(press any key to skip)\n");
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    for (int i = 0; i < 50; i++) {          /* 50 x 100 ms = 5 s */
        if (keyboard_has_data()) {
            while (keyboard_has_data()) {
                (void)keyboard_read_byte();  /* flush input */
            }
            break;
        }
        sleep_ms(100);
    }

    /* Phase A: wrap the boot execution as task 0 (console 0), then
     * run the shell — new tasks/consoles are created via F1. */
    serial_init();       /* mirror console ke COM1 (QEMU -serial) */
    task_init0();
    /* v0.3 FR-09: the "net" kernel task services lwIP ~100 Hz (the
     * ISRs only copy frames into the RX ring). Falls back to a no-op
     * when there is no NIC. */
    net_start_task();
    shell_entry(NULL);   /* never returns */
}
