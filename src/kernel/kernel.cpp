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
#include "library/header/panic.h"
#include "library/header/syscall.h"
#include "library/header/paging.h"     // paging_init (v10.7 ring 3)
#include "library/header/usermode.h"   // tss_init / memmap (v10.7 ring 3)
#include <stdint.h>

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

// ============================================================
//  CPUID
// ============================================================
static void print_cpu_vendor() {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    char vendor[13] = {0};
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    printf("CPU Vendor: %s\n", vendor);
}

// ============================================================
//  COLOR HANDLING
// ============================================================
static const char* color_names[] = {
    "black", "blue", "green", "cyan", "red", "magenta", "brown", "lightgrey",
    "darkgrey", "lightblue", "lightgreen", "lightcyan", "lightred",
    "lightmagenta", "lightbrown", "white"
};

static int get_color_by_name(const char* name) {
    for (int i = 0; i < 16; i++) {
        if (strcmp(color_names[i], name) == 0) return i;
    }
    return -1;
}

static void color_list() {
    printf("Available colors:\n");
    for (int i = 0; i < 16; i++) {
        set_color(i, 0);
        printf("  %-12s", color_names[i]);
        if ((i + 1) % 4 == 0) printf("\n");
    }
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    printf("\nUsage: color <fg> [bg]  or  color list  or  color reset\n");
}

// ============================================================
//  SHELL COMMANDS
// ============================================================
static void cmd_calc(const char* args) {
    int a = atoi(args);
    while (*args >= '0' && *args <= '9') args++;
    while (*args == ' ') args++;
    char op = *args;
    args++;
    while (*args == ' ') args++;
    int b = atoi(args);
    int result = 0, valid = 1;
    switch(op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case '/':
            if (b == 0) { printf("Error: Division by zero!\n"); return; }
            result = a / b; break;
        default:
            printf("Unknown operator: '%c'. Use +, -, *, or /\n", op);
            valid = 0;
    }
    if (valid) printf("%d %c %d = %d\n", a, op, b, result);
}

static void cmd_hex(const char* args) {
    int num = atoi(args);
    char buffer[32];
    itoa(num, buffer, 16);
    printf("%d (decimal) = 0x%s (hex)\n", num, buffer);
}

static void cmd_dec(const char* args) {
    int num = hex_to_int(args);
    printf("0x%s = %d (decimal)\n", args, num);
}

static void cmd_mem(const char* args) {
    uint32_t addr = hex_to_int(args);
    uint32_t value = *(volatile uint32_t*)addr;
    printf("Memory at 0x%x = 0x%x (%d)\n", addr, value, value);
}

static void cmd_alloc(const char* args) {
    size_t size = atoi(args);
    void* ptr = malloc(size);
    if (ptr) {
        printf("Allocated %u bytes at 0x%x\n", size, (uint32_t)ptr);
        uint8_t* p = (uint8_t*)ptr;
        for (size_t i = 0; i < size && i < 16; i++) p[i] = 0xAA + i;
        printf("First 16 bytes: ");
        for (size_t i = 0; i < size && i < 16; i++) printf("%02x ", p[i]);
        printf("\n");
    } else {
        printf("Allocation failed!\n");
    }
}

static void cmd_free(const char* args) {
    uint32_t addr = hex_to_int(args);
    free((void*)addr);
    printf("Free called for address 0x%x (dummy, no actual free)\n", addr);
}

// ============================================================
//  SCTEST — int 0x80 syscall layer self-test from the shell.
//  Every call through a syscallN() wrapper is a REAL "int
//  $0x80" instruction (the same path ring-3 programs and the
//  tcc runtime take) — if everything PASSes, the syscall
//  infrastructure is proven alive, not just dead code.
// ============================================================
static void cmd_sctest(void) {
    int pass = 0, fail = 0, skip = 0;

    printf("== sctest: int 0x80 syscall layer test ==\n");

    /* 1. getpid */
    int pid = syscall0(SYS_GETPID);
    if (pid == 1) { printf("  PASS  getpid() = %d\n", pid); pass++; }
    else { printf("  FAIL  getpid() = %d (expected 1)\n", pid); fail++; }

    /* 2. write to the console via int 0x80 */
    static const char sc_msg[] = "  (this line was printed via the write syscall)\n";
    int wn = syscall3(SYS_WRITE, SYS_FD_STDOUT,
                      (uint32_t)(uintptr_t)sc_msg,
                      (uint32_t)(sizeof(sc_msg) - 1));
    if (wn == (int)(sizeof(sc_msg) - 1)) {
        printf("  PASS  write(1,...) returned %d bytes\n", wn); pass++;
    } else {
        printf("  FAIL  write(1,...) returned %d (expected %u)\n",
               wn, (unsigned)(sizeof(sc_msg) - 1)); fail++;
    }

    /* 3. gettick + sleep */
    uint32_t t0 = (uint32_t)syscall0(SYS_GETTICK);
    syscall1(SYS_SLEEP, 200);
    uint32_t t1 = (uint32_t)syscall0(SYS_GETTICK);
    if (t1 > t0) {
        printf("  PASS  sleep(200): tick %u -> %u\n", t0, t1); pass++;
    } else {
        printf("  FAIL  sleep(200): tick did not advance (%u -> %u)\n", t0, t1); fail++;
    }

    /* 4. malloc from the MRP arena */
    uint32_t mp = (uint32_t)syscall1(SYS_MALLOC, 64);
    if (mp != 0) { printf("  PASS  malloc(64) -> 0x%08x\n", mp); pass++; }
    else         { printf("  FAIL  malloc(64) return 0\n"); fail++; }

    /* 5. open/read/close a RAMFS file */
    int fd = syscall1(SYS_OPEN, (uint32_t)(uintptr_t)"hello.mrp");
    if (fd >= 3) {
        printf("  PASS  open(\"hello.mrp\") -> fd %d\n", fd); pass++;
        char head[8];
        int rn = syscall3(SYS_READ, (uint32_t)fd,
                          (uint32_t)(uintptr_t)head, (uint32_t)sizeof(head));
        if (rn > 0) {
            printf("  PASS  read() %d byte, header: ", rn);
            for (int i = 0; i < rn && i < 8; i++)
                printf("%02x ", (uint8_t)head[i]);
            printf("\n");
            pass++;
        } else {
            printf("  FAIL  read() returned %d\n", rn); fail++;
        }
        int cn = syscall1(SYS_CLOSE, (uint32_t)fd);
        if (cn == 0) { printf("  PASS  close(fd)\n"); pass++; }
        else         { printf("  FAIL  close() returned %d\n", cn); fail++; }
    } else if (fd == SYS_ENOENT) {
        printf("  SKIP  open: hello.mrp not in RAMFS root\n"); skip++;
    } else {
        printf("  FAIL  open() returned err %d\n", fd); fail++;
    }

    /* 6. exec an .mrp via the syscall (the program's output is printed too) */
    int er = syscall1(SYS_EXEC, (uint32_t)(uintptr_t)"hello.mrp");
    if (er == 0) {
        printf("  PASS  exec(\"hello.mrp\") succeeded\n"); pass++;
    } else if (er == SYS_ENOENT) {
        printf("  SKIP  exec: hello.mrp not present (try booting from ISO)\n"); skip++;
    } else {
        printf("  FAIL  exec() returned err %d\n", er); fail++;
    }

    /* 7. an unknown syscall number */
    int e99 = syscall0(99);
    if (e99 == SYS_ENOSYS) {
        printf("  PASS  syscall(99) -> ENOSYS (%d)\n", e99); pass++;
    } else {
        printf("  FAIL  syscall(99) returned %d (expected %d)\n", e99, SYS_ENOSYS);
        fail++;
    }

    /* 8. exit status echo (single-task: status balik ke caller) */
    int ex = syscall1(SYS_EXIT, 42);
    if (ex == 42) { printf("  PASS  exit(42) -> status echoed back %d\n", ex); pass++; }
    else          { printf("  FAIL  exit(42) returned %d\n", ex); fail++; }

    printf("== sctest finished: %d PASS, %d FAIL, %d SKIP ==\n", pass, fail, skip);
}

// ============================================================
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
    printf("Equinox OS v0.1 Beta - booting\n\n");
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
static const char* const equinox_ascii[] = {
    "               ,,ggddY888Ybbgg,,",
    "          ,agd8\"\"'   .d8888888888bga,",
    "       ,gdP\"\"'     .d88888888888888888g,",
    "     ,dP\"        ,d888888888888888888888b,",
    "   ,dP\"         ,8888888888888888888888888b,",
    "  ,8\"          ,8888888P\"\"\"88888888888888888,",
    " ,8'           I888888I    )88888888888888888,",
    ",8'            `8888888booo8888888888888888888,",
    "d'              `88888888888888888888888888888b",
    "8                `\"8888888888888888888888888888",
    "8                  `\"88888888888888888888888888",
    "8                      `\"8888888888888888888888",
    "Y,                        `8888888888888888888P",
    "`8,                         `88888888888888888'",
    " `8,              .oo.       `888888888888888'",
    "  `8a             8888        88888888888888'",
    "   `Yba           `\"\"'       ,888888888888P'",
    "     \"Yba                   ,88888888888'",
    "       `\"Yba,             ,8888888888P\"'",
    "          `\"Y8baa,      ,d88888888P\"'",
    "               ``\"\"YYba8888P888\"'",
    NULL
};

/* Purple endpoints of the intro gradient (0xRRGGBB). */
#define EQ_PURPLE 0x8A4FFFu   /* vivid violet (center lines)   */
#define EQ_LILAC  0xB794FFu   /* soft lavender (version line)  */
#define EQ_WHITE  0xFFFFFFu   /* pure white (top/bottom edges) */

static uint32_t eq_gradient(int line, int total) {
    /* t = 1 at the outer edges, 0 at the center line. The emblem
     * therefore fades white -> purple -> white, mirroring the
     * day/night balance of an equinox. */
    if (total <= 1) return EQ_WHITE;
    int half = (total - 1) / 2;
    int d = line > half ? line - half : half - line;
    int t = (d * 256) / half;            /* 0..256 fixed point */
    if (t > 256) t = 256;
    uint32_t pr = (EQ_PURPLE >> 16) & 0xFF, pg = (EQ_PURPLE >> 8) & 0xFF,
             pb = EQ_PURPLE & 0xFF;
    uint32_t wr = (EQ_WHITE >> 16) & 0xFF, wg = (EQ_WHITE >> 8) & 0xFF,
             wb = EQ_WHITE & 0xFF;
    uint32_t r = pr + ((wr - pr) * (uint32_t)t) / 256;
    uint32_t g = pg + ((wg - pg) * (uint32_t)t) / 256;
    uint32_t b = pb + ((wb - pb) * (uint32_t)t) / 256;
    return (r << 16) | (g << 8) | b;
}

static void print_intro() {
    clear_screen();
    int cols = term_get_cols();

    /* Equinox emblem, centered, one gradient color per line. */
    int lines = 0;
    while (equinox_ascii[lines] != NULL) lines++;
    int wmax = 0;
    for (int i = 0; i < lines; i++) {
        int l = (int)strlen(equinox_ascii[i]);
        if (l > wmax) wmax = l;
    }
    int pad = (cols - wmax) / 2;
    if (pad < 0) pad = 0;
    for (int i = 0; i < lines; i++) {
        set_fg_rgb(eq_gradient(i, lines));
        for (int p = 0; p < pad; p++) put_char(' ');
        printf("%s\n", equinox_ascii[i]);
    }

    /* Minimal wordmark — the OS identity directly under the emblem.
     * Everything else (RAM, heap, uptime, CPU vendor, tool tips)
     * was removed on purpose: after the banner the user lands
     * straight in the shell. */
    const char* wordmark = "E Q U I N O X   O S";
    const char* version  = "v 0 . 1   B E T A";
    printf("\n");
    int wpad = (cols - (int)strlen(wordmark)) / 2;
    if (wpad < 0) wpad = 0;
    set_fg_rgb(EQ_WHITE);
    for (int p = 0; p < wpad; p++) put_char(' ');
    printf("%s\n", wordmark);
    int vpad = (cols - (int)strlen(version)) / 2;
    if (vpad < 0) vpad = 0;
    set_fg_rgb(EQ_LILAC);
    for (int p = 0; p < vpad; p++) put_char(' ');
    printf("%s\n\n", version);

    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

#ifdef HAS_LVGL
// ============================================================
//  LVGL SHARED INIT — called once before any LVGL screen
// ============================================================
static int lvgl_initialized = 0;

/* FIX(V2): return int (0 = ready, -1 = no VESA) so every GUI
 * command (`gui`, `fm`, `fm <path>`) can bail out with a clear
 * message instead of continuing into LVGL with a 0x0 driver and
 * a null framebuffer. */
static int lvgl_ensure_init(void) {
    if (lvgl_initialized) return 0;
    if (!vesa_is_available()) {
        printf("GUI: VESA framebuffer not available.\n");
        printf("GUI: Boot via the ISO/GRUB image so graphics mode gets enabled:\n");
        printf("GUI:   qemu-system-i386 -m 64 -cdrom dist/equinox.iso\n");
        printf("GUI: (booting with -kernel does not set a VBE mode -> VGA text only)\n");
        return -1;
    }
    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();
    timer_set_tick_cb(lv_tick_inc);
    lvgl_initialized = 1;
    return 0;
}

// ============================================================
//  LVGL DEMO — type "gui" in the shell to launch it
// ============================================================
static void lvgl_demo() {
    if (lvgl_ensure_init() != 0) return;

    /* --- Clear LVGL screen --- */
    lv_obj_clean(lv_scr_act());

    /* --- Title --- */
    lv_obj_t *title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "Equinox OS + LVGL v8.3");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00AAFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    /* --- Subtitle --- */
    lv_obj_t *sub = lv_label_create(lv_scr_act());
    lv_label_set_text(sub, "VESA Framebuffer  |  PS/2 Mouse  |  Keyboard");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 65);

    /* --- Counter Button --- */
    static int counter = 0;
    lv_obj_t *btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 160, 50);
    lv_obj_align(btn, LV_ALIGN_CENTER, -120, -60);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Click me!");
    lv_obj_center(btn_label);

    lv_obj_t *counter_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(counter_label, &lv_font_montserrat_20, 0);
    char counter_buf[32];
    sprintf(counter_buf, "Clicks: %d", counter);
    lv_label_set_text(counter_label, counter_buf);
    lv_obj_align(counter_label, LV_ALIGN_CENTER, -120, 10);

    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        counter++;
        lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
        char buf[32];
        sprintf(buf, "Clicks: %d", counter);
        lv_label_set_text(lbl, buf);
    }, LV_EVENT_CLICKED, counter_label);

    /* --- Slider --- */
    lv_obj_t *slider_label = lv_label_create(lv_scr_act());
    lv_label_set_text(slider_label, "Slider: 50");
    lv_obj_align(slider_label, LV_ALIGN_CENTER, 120, -30);

    lv_obj_t *slider = lv_slider_create(lv_scr_act());
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_set_width(slider, 200);
    lv_obj_align(slider, LV_ALIGN_CENTER, 120, 10);

    lv_obj_add_event_cb(slider, [](lv_event_t *e) {
        lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
        int val = lv_slider_get_value(lv_event_get_target(e));
        char buf[32];
        sprintf(buf, "Slider: %d", val);
        lv_label_set_text(lbl, buf);
    }, LV_EVENT_VALUE_CHANGED, slider_label);

    /* --- Switch --- */
    lv_obj_t *sw_label = lv_label_create(lv_scr_act());
    lv_label_set_text(sw_label, "Dark Mode");
    lv_obj_align(sw_label, LV_ALIGN_CENTER, 120, 60);

    lv_obj_t *sw = lv_switch_create(lv_scr_act());
    lv_obj_align(sw, LV_ALIGN_CENTER, 120, 90);
    lv_obj_add_event_cb(sw, [](lv_event_t *e) {
        lv_obj_t *scr = lv_scr_act();
        bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(scr,
            on ? lv_color_hex(0x1A1A2E) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(scr,
            on ? lv_color_white() : lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
        /* Re-color title and subtitle */
        lv_obj_t *title = lv_obj_get_child(scr, 0);
        lv_obj_t *sub   = lv_obj_get_child(scr, 1);
        if (title) lv_obj_set_style_text_color(title,
            on ? lv_color_hex(0x00CCFF) : lv_color_hex(0x0055AA), 0);
        if (sub) lv_obj_set_style_text_color(sub,
            on ? lv_color_hex(0xAAAAAA) : lv_color_hex(0x888888), 0);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Exit hint --- */
    lv_obj_t *hint = lv_label_create(lv_scr_act());
    lv_label_set_text(hint, "Press ESC to return to shell");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    /* --- ESC to exit --- */
    static volatile int exit_gui = 0;
    exit_gui = 0;
    lv_obj_add_event_cb(lv_scr_act(), [](lv_event_t *e) {
        uint32_t key = *(uint32_t *)lv_event_get_param(e);
        if (key == LV_KEY_ESC) exit_gui = 1;
    }, LV_EVENT_KEY, NULL);

    /* --- Event loop --- */
    while (!exit_gui) {
        lv_timer_handler();
        sleep_ms(5);
    }
}
#endif

// ============================================================
//  GLOBAL TOOL DISPATCH — "system" .mrp tools callable from any
//  directory: `mtcc main.c`, `mtcc -c test/hello.c`, `hello.mrp`,
//  etc — no `run`, no cd to the tool's location.
// ------------------------------------------------------------
//  SYSTEM SEARCH PATH (v10.6): directories scanned by this
//  dispatcher AND by the `run` command fallback. Order matters:
//  root first (user files win), then classic /bin, then the new
//  global directories /equinox/tools (mtcc, morph_demo) and
//  /equinox/games (snake, breakout, pong). Nodes are re-fetched on
//  every call — the user may have rm'd a directory from the
//  shell. Whatever is NOT on the system path is NOT dispatched —
//  it still goes through `run` / `./`.
//
//  Dispatch mode: QUIET — loader messages ("mrp: running...")
//  are hidden so tool output stays clean, only the program's own
//  output shows (the `run`/`./` commands stay verbose as usual).
// ============================================================
#define SHELL_SYS_PATH_DIRS 4

static struct fs_node* shell_sys_path_dir(int i) {
    switch (i) {
        case 0: return fs_get_root();
        case 1: return fs_find_child(fs_get_root(), "bin");
        case 2: return fs_get_node_from_path(fs_get_root(), "/equinox/tools");
        case 3: return fs_get_node_from_path(fs_get_root(), "/equinox/games");
        default: return NULL;
    }
}

static int run_tool_quiet(struct fs_node* dir, const char* fname, const char* args) {
    /* v10.6: `dir` = the directory where the tool was FOUND (root,
     * /bin, /equinox/tools, /equinox/games) — not always root anymore. */
    syscall_set_args(args);
    mrp_set_quiet(1);
    mrp_run(dir, fname);
    mrp_set_quiet(0);
    syscall_set_args("");   // don't leak into the next program
    return 1;
}

static int try_run_tool(const char* input) {
    // First token = the tool name (the rest = arguments).
    char tok[80];
    int i = 0;
    while (input[i] && input[i] != ' ' && i < (int)sizeof(tok) - 1) {
        tok[i] = input[i];
        i++;
    }
    tok[i] = '\0';
    if (tok[0] == '\0') return 0;

    // A name containing '/' is a path, not a command — outside the
    // global dispatcher's jurisdiction (don't punch through the
    // system path).
    if (strstr(tok, "/")) return 0;

    const char* args = input + i;
    while (*args == ' ') args++;

    // v10.6: system path = /, /bin, /equinox/tools, /equinox/games.
    for (int d = 0; d < SHELL_SYS_PATH_DIRS; d++) {
        struct fs_node* dir = shell_sys_path_dir(d);
        if (!dir) continue;

        // Candidate 1: the exact name (e.g. "hello.mrp")
        struct fs_node* n = fs_find_child(dir, tok);
        if (n && !n->is_dir) {
            return run_tool_quiet(dir, tok, args);
        }

        // Candidate 2: auto-append ".mrp" (e.g. "mtcc" -> mtcc.mrp)
        char mrp_name[86];
        snprintf(mrp_name, sizeof(mrp_name), "%s.mrp", tok);
        n = fs_find_child(dir, mrp_name);
        if (n && !n->is_dir) {
            return run_tool_quiet(dir, mrp_name, args);
        }
    }
    return 0;
}

// ============================================================
//  SHELL PATH RESOLUTION (v10.4: editor round-trip, fix K4)
// ------------------------------------------------------------
//  The old FIX(K4) built absolute paths with a plain strcat —
//  fine for simple names, but "."/".." components slipped through
//  raw: `edit ../x.txt` from /sub produced "/sub/../x.txt", which
//  fs_get_node_from_path REJECTS (".." is not a node name) -> the
//  editor loaded an empty buffer, then save fell back to ROOT with
//  the same mangled name -> stray data + the old file "lost".
//
//  shell_resolve_path() canonicalizes a shell argument into a
//  CLEAN absolute path:
//    - "/a/b"  starts from root (absolute argument)
//    - "a/b"   starts from cwd
//    - "."      is skipped, ".." pops one component (never past root)
//    - double slashes are collapsed, no trailing '/' (except root)
//  It does not check whether the path exists — used for files that
//  already exist (edit/cat) as well as new ones (editor save).
// ============================================================
static void shell_resolve_path(struct fs_node* cwd, const char* arg,
                               char* out, uint32_t outsz) {
    if (!out || outsz < 2) return;

    uint32_t olen = 0;
    if (arg[0] == '/') {
        out[olen++] = '/';                 // absolute: ignore cwd
    } else {
        // Relative: start from the cwd's absolute path ("/" or "/a/b").
        fs_get_path(cwd, out, outsz);
        while (out[olen] && olen < outsz - 1) olen++;
        if (olen == 0) { out[0] = '/'; olen = 1; }
    }

    // Append the argument components, resolving . and .. on the fly.
    const char* p = arg;
    while (*p) {
        while (*p == '/') p++;             // collapse double slashes
        if (!*p) break;
        const char* start = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);

        if (len == 1 && start[0] == '.') continue;          // "."  -> stay
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            // ".." -> drop the last component ("/" root is never passed)
            while (olen > 1 && out[olen - 1] != '/') olen--;
            if (olen > 1) olen--;                           // drop the '/'
            continue;
        }

        // Append "/component" (bounds-checked).
        if (olen > 0 && out[olen - 1] != '/') {
            if (olen < outsz - 1) out[olen++] = '/';
        }
        for (uint32_t i = 0; i < len && olen < outsz - 1; i++) {
            out[olen++] = start[i];
        }
    }

    if (olen == 0) { out[0] = '/'; olen = 1; }             // "" -> "/"
    out[olen] = '\0';
}

// ============================================================
//  SHELL MAIN LOOP
// ============================================================
static void shell() {
    char input[128];
    struct fs_node* cwd = fs_get_root();

    /* v10.6: HOME DIRECTORY = /user ("user space"). The shell now
     * starts there — files the user creates (ccfile, editor, mtcc
     * output) land in /user by default instead of polluting the
     * root. Falls back to root should /user somehow not exist
     * (e.g. a corrupted fs). */
    struct fs_node* home = fs_get_node_from_path(fs_get_root(), "/user");
    if (home && home->is_dir) {
        cwd = home;
    }

    print_intro();

    while (1) {
        // Sync the "process" cwd (used by the open/mkfile syscalls)
        // so global tools (mtcc) resolve relative paths against the
        // user's directory — they work from anywhere.
        syscall_set_cwd(cwd);

        char path_buf[256];
        fs_get_path(cwd, path_buf, sizeof(path_buf));

        /* Prompt — root::users <path> $
         *   root   : light green
         *   ::     : dark grey
         *   users  : light cyan
         *   <path> : yellow
         *   $      : white
         * Looks like a zsh/powerline prompt, but uses only the 16
         * VGA colors so it still lives in the text-mode fallback. */
        set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        printf("root");
        set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        printf("::");
        set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        printf("users");
        set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        printf(" ");
        set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
        printf("%s", path_buf);
        set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        printf(" $ ");
        // Arrow-aware line editor: Up/Down walk the command history.
        // The entered line is stored after execution below (history_add).
        gets_history(input, sizeof(input));
        history_add(input);

        if (strcmp(input, "help") == 0) {
            printf("Available commands:\n");
            printf("  (up/down arrows recall previous commands)\n");
            printf("  help          - show this help\n");
            printf("  clear         - clear screen\n");
            printf("  echo          - print a message\n");
            printf("  info          - show system info\n");
            printf("  cpu           - show CPU vendor\n");
            printf("  color         - change colors (see 'color list')\n");
            printf("  calc <a> <op> <b> - calculator\n");
            printf("  hex <num>     - convert decimal to hex\n");
            printf("  dec <hex>     - convert hex to decimal\n");
            printf("  mem <addr>    - read memory at address\n");
            printf("  testconv      - test itoa/atoi functions\n");
            printf("  tick          - show timer tick count\n");
            printf("  sleep <ms>    - delay in milliseconds\n");
            printf("  malloc        - show heap statistics\n");
            printf("  alloc <bytes> - allocate memory (test)\n");
            printf("  free <addr>   - free memory (dummy)\n");
            printf("  ls            - list directory contents\n");
            printf("  ls -l         - list with details\n");
            printf("  pwd           - print working directory\n");
            printf("  cd <path>     - change directory\n");
            printf("  cdir <name>   - create directory\n");
            printf("  cfile <name>  - create empty file\n");
            printf("  ccfile <name> << \"content\" - create file with content\n");
            printf("  cat <name>    - display file content\n");
            printf("  run <path.mrp> [args] - run an .mrp (subpaths ok, falls back to system path)\n");
            printf("  ./name.mrp [args]     - shortcut for 'run name.mrp' (subpaths ok)\n");
            printf("  mtcc <file.c>         - compile & run C from ANY directory\n");
            printf("  mtcc --debug <file.c> - same, plus verbose compiler info\n");
            printf("  mtcc -c <file.c>      - compile to <file>.mrp, then run it\n");
            printf("  (any .mrp in /, /bin, /equinox/tools or /equinox/games can be\n");
            printf("   called by name, e.g. mtcc, snake, breakout, pong)\n");
            printf("  tree          - display directory tree\n");
            printf("  rm <name>     - delete file or empty directory\n");
            printf("  rmdir <name>  - delete empty directory\n");
            printf("  edit <file>   - open text editor\n");
            printf("  settings      - system settings UI\n");
            printf("  fm [path]     - file manager (optional path)\n");
            printf("  clock         - show real-time clock (uptime)\n");
            printf("  sys <cmd>     - system utilities (sys help for more)\n");
            printf("  testvector    - test dynamic array (vector)\n");
            printf("  random        - test random number generator\n");
            printf("  math          - test math functions (sin/cos/tan)\n");
            printf("  beep          - play a beep (440Hz, 500ms)\n");
            printf("  song          - queue Twinkle Twinkle (non-blocking)\n");
            printf("  teststr       - test libstring functions\n");
            printf("  mouse         - debug mouse driver\n");
            printf("  ifconfig      - network interface info\n");
            printf("  ping <host>   - ICMP ping 4x; host = IP/hostname\n");
            printf("  tcpping <host> [port] - TCP probe (works under QEMU user-net)\n");
            printf("  dns <hostname>   - resolve a domain name via 10.0.2.3\n");
            printf("  mget <url> [-port <n>] - HTTP download to RAMFS\n");
            printf("  httpd         - mini web server :80 / status\n");
            printf("  ringstats     - ring buffer stats (kbd/mouse/audio)\n");
            printf("  memmap        - memory map + ring 3 status\n");
            printf("  ring          - show current privilege level (ring 0 vs 3)\n");
            printf("  doom [args]   - run DOOM (doomgeneric port, needs doom1.wad)\n");
            printf("  panic [msg]   - trigger a kernel panic (auto-reboot in 30s)\n");
            printf("  syscalls      - list int 0x80 syscalls\n");
            printf("  sctest        - test the syscall layer from the shell\n");
#ifdef HAS_LVGL
            printf("  gui           - launch LVGL GUI demo\n");
#endif
            printf("  reboot        - restart the system\n");
        }
        else if (strcmp(input, "clear") == 0) {
            clear_screen();
        }
        else if (strcmp(input, "echo") == 0) {
            printf("Echo mode activated! (type anything)\n");
            char buf[64];
            gets(buf, sizeof(buf));
            printf("You said: %s\n", buf);
        }
        else if (strcmp(input, "info") == 0) {
            printf("Equinox OS v0.1 Beta (32-bit, i686)\n");
            printf("Build: %s %s\n", __DATE__, __TIME__);
            printf("RAM: 16 MB (simulated)\n");
            /* FIX(K1/M8): heap size comes from malloc.cpp (2 MB),
             * not a wrong hardcoded magic number (960 KB). */
            printf("Heap: %u bytes total, %u used\n",
                   get_heap_total(), get_heap_used());
        }
        else if (strcmp(input, "cpu") == 0) {
            print_cpu_vendor();
        }
        else if (strcmp(input, "panic") == 0) {
            /* Kernel-panic screen demo + 30 s auto-reboot. */
            kernel_panic("can't load libc", "triggered from shell: command 'panic'");
        }
        else if (starts_with(input, "panic ")) {
            kernel_panic(input + 6, "triggered from shell: command 'panic <msg>'");
        }
        else if (strcmp(input, "syscalls") == 0) {
            /* Syscall numbers + ABI list (living documentation). */
            syscall_list();
        }
        else if (strcmp(input, "sctest") == 0) {
            /* Self-test of int 0x80 from ring 0 — the same path
             * ring-3 programs take. */
            cmd_sctest();
        }
        else if (strcmp(input, "reboot") == 0) {
            printf("Rebooting...\n");
            reboot();
        }
        else if (strcmp(input, "testconv") == 0) {
            test_itoa_atoi();
        }
        else if (strcmp(input, "tick") == 0) {
            printf("Timer ticks: %u\n", get_tick());
        }
        else if (starts_with(input, "sleep ")) {
            uint32_t ms = atoi(input + 6);
            printf("Sleeping for %u ms...\n", ms);
            sleep_ms(ms);
            printf("Done.\n");
        }
        else if (strcmp(input, "malloc") == 0) {
            malloc_stats();
        }
        else if (starts_with(input, "alloc ")) {
            cmd_alloc(input + 6);
        }
        else if (starts_with(input, "free ")) {
            cmd_free(input + 5);
        }
        else if (strcmp(input, "ls") == 0) {
            fs_ls(cwd, 0);
        }
        else if (strcmp(input, "ls -l") == 0) {
            fs_ls(cwd, 1);
        }
        else if (strcmp(input, "pwd") == 0) {
            char buf[256];
            fs_get_path(cwd, buf, sizeof(buf));
            printf("%s\n", buf);
        }
        else if (starts_with(input, "cd ")) {
            const char* path = input + 3;
            if (fs_change_dir(&cwd, path) != 0) {
                printf("cd: no such directory\n");
            }
        }
        else if (starts_with(input, "cdir ")) {
            const char* name = input + 5;
            int ret = fs_create_dir(cwd, name);
            if (ret == -2) printf("cdir: '%s' already exists\n", name);
            else if (ret == -7) printf("cdir: name too long (max 63 chars)\n");
            else if (ret != 0) printf("cdir: failed\n");
        }
        else if (starts_with(input, "cfile ")) {
            const char* name = input + 6;
            int ret = fs_create_file(cwd, name, NULL);
            if (ret == -2) printf("cfile: '%s' already exists\n", name);
            else if (ret == -7) printf("cfile: name too long (max 63 chars)\n");
            else if (ret != 0) printf("cfile: failed\n");
        }
        else if (starts_with(input, "ccfile ")) {
            const char* rest = input + 7;
            char* delim = strstr(rest, " << ");
            if (delim) {
                *delim = '\0';
                char* name = (char*)rest;
                char* content = delim + 4;
                if (*content == '"') content++;
                size_t len = strlen(content);
                if (len > 0 && content[len-1] == '"') content[len-1] = '\0';
                int ret = fs_create_file(cwd, name, content);
                if (ret == -2) printf("ccfile: '%s' already exists\n", name);
                else if (ret == -7) printf("ccfile: name too long (max 63 chars)\n");
                else if (ret != 0) printf("ccfile: failed\n");
            } else {
                printf("ccfile: missing content (use << \"text\")\n");
            }
        }
        else if (starts_with(input, "cat ")) {
            const char* name = input + 4;
            struct fs_node* node = fs_find_child(cwd, name);
            if (!node) {
                printf("cat: '%s' not found\n", name);
            } else if (node->is_dir) {
                printf("cat: '%s' is a directory\n", name);
            } else {
                if (node->content && node->size > 0) {
                    /* FIX(K3): print exactly node->size bytes. Binary
                     * files (.mrp) are filled by fs_write_binary with
                     * NO NUL terminator -> printf("%s") would read wild
                     * bytes until the first 0 outside the buffer. */
                    for (uint32_t i = 0; i < node->size; i++)
                        put_char(node->content[i]);
                    put_char('\n');
                } else {
                    printf("(empty file)\n");
                }
            }
        }
        else if (starts_with(input, "run ")) {
            /* run <name.mrp> [args] — arguments are stored in the
             * syscall layer (SYS_GETARGS #15) so .mrp programs can
             * read them. Example: run tcc.mrp -c hello.c (mtcc).
             *
             * v10.6: <name> may be a SUBPATH ("equinox/games/snake.mrp",
             * "../x.mrp", "/equinox/tools/mtcc.mrp") — it is canonicalized
             * and then run from its parent directory. A plain name is
             * searched in cwd first, then falls back to the system path
             * (/, /bin, /equinox/tools, /equinox/games) so `run mtcc.mrp`
             * still works from /user. */
            const char* rest = input + 4;
            while (*rest == ' ') rest++;
            char name[80];
            int i = 0;
            while (rest[i] && rest[i] != ' ' && i < (int)sizeof(name) - 1) {
                name[i] = rest[i];
                i++;
            }
            name[i] = '\0';
            const char* args = rest + i;
            while (*args == ' ') args++;
            if (name[0] == '\0') {
                printf("run: empty file name (try: run snake.mrp)\n");
                continue;
            }
            syscall_set_args(args);

            int found = 0;
            if (strstr(name, "/")) {
                /* subpath: canonicalize relative to cwd -> clean
                 * absolute path -> fetch the node -> run from its
                 * parent. */
                char full[300];
                shell_resolve_path(cwd, name, full, sizeof(full));
                struct fs_node* n = fs_get_node_from_path(fs_get_root(), full);
                if (n && !n->is_dir && n->parent) {
                    mrp_run(n->parent, n->name);
                    found = 1;
                }
            } else {
                struct fs_node* n = fs_find_child(cwd, name);
                if (n && !n->is_dir) {
                    mrp_run(cwd, name);
                    found = 1;
                } else {
                    /* fallback: system path (mrp_run verbose, not
                     * quiet — the user explicitly asked to `run`) */
                    for (int d = 0; d < SHELL_SYS_PATH_DIRS && !found; d++) {
                        struct fs_node* dir = shell_sys_path_dir(d);
                        if (!dir) continue;
                        n = fs_find_child(dir, name);
                        if (n && !n->is_dir) {
                            mrp_run(dir, name);
                            found = 1;
                        }
                    }
                }
            }
            if (!found) {
                printf("run: '%s' not found (cwd, /, /bin, /equinox/tools, /equinox/games)\n",
                       name);
            }
            syscall_set_args("");   // don't leak into the next program
        }
        // ----- ./file.mrp — shortcut for 'run file.mrp' (workflow A.1) -----
        // Auto-appends ".mrp" if the user omits the extension (A.2).
        // v10.6: subpaths now SUPPORTED ("./equinox/games/snake.mrp"
        // from root, "./../x.mrp" from a subfolder) — shell_resolve_path
        // canonicalizes the argument, then the node runs from its
        // parent.
        else if (starts_with(input, "./")) {
            const char* src = input + 2;

            // Skip leading whitespace after ./ (rare, but defensive)
            while (*src == ' ') src++;

            if (*src == '\0') {
                printf("./: empty file name (try: ./hello.mrp)\n");
                continue;
            }

            char name[80];
            int i = 0;
            // -5 leaves room for the auto-appended ".mrp\0" (4 chars
            // + null). Stop at the first space: the rest is the
            // program's ARGUMENTS ( ./tcc.mrp hello.c ), not part of
            // the file name.
            while (src[i] && src[i] != ' ' && i < (int)sizeof(name) - 5) {
                name[i] = src[i];
                i++;
            }
            name[i] = '\0';
            int len = i;
            const char* mrp_args = src + i;
            while (*mrp_args == ' ') mrp_args++;

            // Auto-append .mrp if there is no extension yet
            if (len < 4 ||
                name[len-4] != '.' ||
                (name[len-3] != 'm' && name[len-3] != 'M') ||
                (name[len-2] != 'r' && name[len-2] != 'R') ||
                (name[len-1] != 'p' && name[len-1] != 'P')) {
                name[len]   = '.';
                name[len+1] = 'm';
                name[len+2] = 'r';
                name[len+3] = 'p';
                name[len+4] = '\0';
            }

            /* resolve "./x" / "x" / "a/b" relative to cwd -> clean absolute */
            char full[300];
            shell_resolve_path(cwd, name, full, sizeof(full));
            struct fs_node* n = fs_get_node_from_path(fs_get_root(), full);
            if (n && !n->is_dir && n->parent) {
                syscall_set_args(mrp_args);
                mrp_run(n->parent, n->name);
                syscall_set_args("");
            } else {
                printf("./: '%s' not found\n", name);
            }
        }
        else if (strcmp(input, "tree") == 0) {
            fs_tree(cwd, 0);
        }
        else if (starts_with(input, "rm ")) {
            const char* name = input + 3;
            int ret = fs_delete_node(cwd, name);
            if (ret == -2) printf("rm: '%s' not found\n", name);
            else if (ret == -4) printf("rm: '%s' is a non-empty directory\n", name);
            else if (ret != 0) printf("rm: failed\n");
        }
        else if (starts_with(input, "rmdir ")) {
            const char* name = input + 6;
            int ret = fs_delete_node(cwd, name);
            if (ret == -2) printf("rmdir: '%s' not found\n", name);
            else if (ret == -4) printf("rmdir: directory not empty\n");
            else if (ret != 0) printf("rmdir: failed\n");
        }
        else if (starts_with(input, "edit ")) {
            const char* filename = input + 5;
            /* v10.4: shell_resolve_path() replaces the manual FIX(K4)
             * assembly — `edit ../x.txt`, `edit ./x.txt` and double
             * slashes are all canonical before reaching the editor
             * (editor_load/editor_save only understand clean absolute
             * paths). Empty arguments are rejected explicitly. */
            if (!filename[0]) {
                printf("edit: missing file name\n");
            } else {
                char fullpath[300];
                shell_resolve_path(cwd, filename, fullpath, sizeof(fullpath));
                editor_open(fullpath);
                clear_screen();
                printf("Editor closed.\n");
            }
        }
        else if (strcmp(input, "settings") == 0) {
            settings_open();
        }
        else if (strcmp(input, "fm") == 0) {
#ifdef HAS_LVGL
            /* FIX(V2): without VESA, `fm` just prints a message and
             * returns to the prompt — never call filemanager_open
             * (LVGL). */
            if (lvgl_ensure_init() != 0) continue;   /* prompt reprinted at the top of the loop */
#endif
            filemanager_open(nullptr);
            clear_screen();
            print_intro();
        }
        else if (starts_with(input, "fm ")) {
#ifdef HAS_LVGL
            /* FIX(V2): same as `fm` — bail out without crashing. */
            if (lvgl_ensure_init() != 0) continue;
#endif
            filemanager_open(input + 3);
            clear_screen();
            print_intro();
        }
        else if (strcmp(input, "clock") == 0) {
            clock_run();
        }
        else if (starts_with(input, "sys ")) {
            const char* cmd = input + 4;
            if (strcmp(cmd, "cwd") == 0) {
                char buf[256];
                sys_getcwd(buf, sizeof(buf));
                printf("%s\n", buf);
            }
            else if (starts_with(cmd, "mkdir ")) {
                const char* name = cmd + 6;
                int ret = sys_create_dir(name);
                if (ret == 0) printf("Directory created.\n");
                else if (ret == -2) printf("Already exists.\n");
                else printf("Failed.\n");
            }
            else if (starts_with(cmd, "touch ")) {
                const char* name = cmd + 6;
                int ret = sys_create_file(name, NULL);
                if (ret == 0) printf("File created.\n");
                else if (ret == -2) printf("Already exists.\n");
                else printf("Failed.\n");
            }
            else if (starts_with(cmd, "rm ")) {
                const char* name = cmd + 3;
                int ret = sys_delete(name);
                if (ret == 0) printf("Deleted.\n");
                else if (ret == -3) printf("Not found.\n");
                else if (ret == -4) printf("Directory not empty.\n");
                else printf("Failed.\n");
            }
            else {
                printf("sys: cwd | mkdir <name> | touch <name> | rm <name>\n");
            }
        }
        else if (strcmp(input, "testvector") == 0) {
            printf("Testing Vector...\n");
            Vector* v = vector_create(sizeof(int));
            if (!v) { printf("Failed to create vector.\n"); return; }
            int nums[] = {10, 20, 30, 40, 50};
            for (int i = 0; i < 5; i++) {
                vector_push(v, &nums[i]);
                printf("Pushed %d, size: %zu\n", nums[i], vector_size(v));
            }
            for (size_t i = 0; i < vector_size(v); i++) {
                int* val = (int*)vector_get(v, i);
                printf("v[%zu] = %d\n", i, val ? *val : -1);
            }
            vector_pop(v);
            printf("After pop, size: %zu\n", vector_size(v));
            vector_clear(v);
            printf("After clear, size: %zu\n", vector_size(v));
            vector_free(v);
            printf("Vector test done.\n");
        }
        else if (strcmp(input, "random") == 0) {
            random_seed(get_tick() + 12345);
            printf("Random 32-bit: %u\n", random_uint32());
            printf("Random int [0,100]: %d\n", random_int(0, 100));
            printf("Random float: %f\n", random_float());
        }
        else if (strcmp(input, "math") == 0) {
            float angle = deg_to_rad(45.0f);
            printf("sin(45°) = %f\n", sinf(angle));
            printf("cos(45°) = %f\n", cosf(angle));
            printf("tan(45°) = %f\n", tanf(angle));
        }
        else if (strcmp(input, "beep") == 0) {
            audio_beep(440, 500);
        }
        else if (strcmp(input, "song") == 0) {
            /* v10.5: queue-based playback. The 14 notes go into the
             * kernel note ring and the TIMER IRQ plays them out over
             * the next ~4.5 s — this loop only queues (~microseconds)
             * and the shell is responsive the whole time. The old
             * audio_play_song() blocked the CPU in sleep loops. */
            static const char*  notes[] = {"C","C","G","G","A","A","G",
                                           "F","F","E","E","D","D","C"};
            static const uint32_t ms[]  = {250,250,250,250,250,250,500,
                                           250,250,250,250,250,250,500};
            int queued = 0;
            for (int i = 0; i < 14; i++) {
                uint32_t f = audio_note_freq(notes[i], 4);
                if (f != 0 && audio_queue_tone(f, ms[i]) == 0) queued++;
            }
            printf("Twinkle queued: %d notes (playing in background)\n",
                   queued);
        }
        else if (strcmp(input, "ringstats") == 0) {
            /* v10.5: one-stop view of the three kernel ring buffers.
             * drops>0 on kbd/mouse means the consumer stalled longer
             * than the ring depth — for audio it means the queue was
             * full when someone pushed (SYS_SNDBEEP -> EBUSY). */
            uint32_t kc, kd, kcap, mc, md, mcap, ac, ad, acap;
            keyboard_ring_stats(&kc, &kd, &kcap);
            mouse_ring_stats(&mc, &md, &mcap);
            audio_queue_stats(&ac, &ad, &acap);
            uint32_t pf, pms;
            audio_queue_playing(&pf, &pms);
            printf("ring buffers:\n");
            printf("  kbd   : %u/%u pending, %u dropped (drop-newest)\n",
                   kc, kcap, kd);
            printf("  mouse : %u/%u pending, %u dropped (overwrite-oldest)\n",
                   mc, mcap, md);
            printf("  audio : %u/%u pending, %u dropped (drop-newest)\n",
                   ac, acap, ad);
            if (pf != 0) {
                printf("  audio now playing %u Hz, %u ms left\n", pf, pms);
            } else if (ac != 0) {
                printf("  audio next note is a rest, %u ms\n", pms);
            }
        }
        else if (strcmp(input, "ring") == 0) {
            /* v10.8: current privilege level of whoever runs this.
             * The shell lives in the kernel = ring 0; .mrp programs
             * run at ring 3 since v10.7. CPL is simply CS & 3 — read
             * the live segment registers straight from asm. */
            uint32_t cs_v, ss_v, ds_v;
            asm volatile("movl %%cs, %0" : "=r"(cs_v));
            asm volatile("movl %%ss, %0" : "=r"(ss_v));
            asm volatile("movl %%ds, %0" : "=r"(ds_v));
            uint32_t cpl = cs_v & 3u;
            printf("current privilege level (CPL = CS & 3): ring %u\n", cpl);
            printf("  CS=0x%02x  SS=0x%02x  DS=0x%02x\n", cs_v, ss_v, ds_v);
            if (cpl == 0) {
                printf("  this is the KERNEL shell — supervisor, full\n");
                printf("  hardware access, faults here panic the OS.\n");
                printf("ring map:\n");
                printf("  ring 0  kernel + shell + interrupt handlers\n");
                printf("  ring 3  .mrp user programs (paging U/S guards\n");
                printf("          kernel memory; crash = killed, not panic)\n");
                printf("try: run hello.mrp   (banner says RING 3)\n");
                printf("     mtcc /test/libc.c  (prints its own ring via\n");
                printf("     syscall 29 ringinfo())\n");
            } else {
                /* Unreachable today (shell is ring 0) — kept for the
                 * day the shell itself moves to ring 3. */
                printf("  running as USER — kernel memory is off limits.\n");
            }
        }
        else if (strcmp(input, "memmap") == 0) {
            /* v10.7: memory map + ring-3 status — a quick way to
             * confirm paging & TSS are alive before running user
             * programs. */
            usermode_print_memmap();
        }
        else if (strcmp(input, "doom") == 0 || starts_with(input, "doom ")) {
            /* v10.9: DOOM one-liner. Finds /equinox/games/doom.mrp (ISO
             * module); the rest of the command line becomes the
             * program's argv (default "-iwad /doom1.wad" — the
             * shareware WAD sits in the RAMFS root via the zero-copy
             * module staging path). Examples:
             *   doom              (title screen → menu → game)
             *   doom -nosound
             *   doom -warp 1      (straight to level E1M1)        */
            const char* rest = input + 4;
            while (*rest == ' ') rest++;
            char args[96];
            /* v10.9: always make sure -iwad is present (mirroring
             * doom.exe, which searches for the WAD in the cwd — here
             * the shareware WAD lives in the RAMFS root). If the user
             * passes their own -iwad, respect it. */
            int ai = 0;
            if (strstr(rest, "-iwad") == NULL) {
                const char* def = "-iwad /doom1.wad ";
                while (def[ai] && ai < (int)sizeof(args) - 1) {
                    args[ai] = def[ai];
                    ai++;
                }
            }
            for (int r = 0; rest[r] && ai < (int)sizeof(args) - 1; r++, ai++) {
                args[ai] = rest[r];
            }
            args[ai] = '\0';
            struct fs_node* gdir = fs_get_node_from_path(fs_get_root(), "/equinox/games");
            struct fs_node* dn = gdir ? fs_find_child(gdir, "doom.mrp") : NULL;
            if (!dn) {
                printf("doom: /equinox/games/doom.mrp not found (rebuild ISO with the doom module)\n");
            } else {
                syscall_set_args(args);
                mrp_run(gdir, "doom.mrp");
                syscall_set_args("");
            }
        }
        else if (strcmp(input, "teststr") == 0) {
            char test[64] = "hello,world,this,is,test";
            char* tokens[10];
            int count = split(test, ",", tokens, 10);
            printf("Split result: %d tokens\n", count);
            for (int i = 0; i < count; i++) {
                printf("  %d: %s\n", i, tokens[i]);
            }
            char buffer[64];
            sprintf(buffer, "Number: %d, Hex: %x, String: %s", 42, 0xFF, "Hello");
            printf("%s\n", buffer);
        }
        else if (strcmp(input, "mouse") == 0) {
            uint32_t count = mouse_get_irq_count();
            uint8_t raw = mouse_get_raw_byte();
            printf("IRQ count: %u  Last raw: 0x%x (%d)\n", count, raw, raw);
            mouse_packet_t pkt = mouse_get_packet();
            if (pkt.valid) {
                printf("Packet valid: buttons=0x%x dx=%d dy=%d\n", pkt.buttons, pkt.dx, pkt.dy);
            } else {
                printf("No packet ready.\n");
            }
        }
        else if (strcmp(input, "ifconfig") == 0) {
            net_print_info();
        }
        else if (strcmp(input, "netdbg") == 0) {
            ne2000_dbg_probe();
        }
        else if (starts_with(input, "ping ") || strcmp(input, "ping") == 0) {
            net_cmd_ping(input[4] == ' ' ? input + 5 : "");
        }
        else if (starts_with(input, "tcpping ") || strcmp(input, "tcpping") == 0) {
            net_cmd_tcpping(input[7] == ' ' ? input + 8 : "");
        }
        else if (starts_with(input, "dns ") || strcmp(input, "dns") == 0) {
            net_cmd_dns(input[3] == ' ' ? input + 4 : "");
        }
        else if (starts_with(input, "mget ") || strcmp(input, "mget") == 0) {
            net_cmd_mget(input[4] == ' ' ? input + 5 : "", cwd);
        }
        else if (strcmp(input, "httpd") == 0) {
            if (!net_is_up()) {
                printf("net: down (no NIC)\n");
            } else if (net_httpd_running()) {
                printf("httpd: running on :80  %u hits  %u bytes served\n",
                       net_httpd_hits(), net_httpd_bytes());
                printf("       host: hostfwd=tcp::8080-:80 then browse\n");
            } else if (net_httpd_start()) {
                printf("httpd: listening on :80\n");
            } else {
                printf("httpd: start failed (out of PCBs?)\n");
            }
        }
        else if (starts_with(input, "color ")) {
            const char* args = input + 6;
            if (strcmp(args, "list") == 0) {
                color_list();
            }
            else if (strcmp(args, "reset") == 0) {
                set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                printf("Color reset to default (white on black).\n");
            }
            else {
                char fg_name[20] = {0}, bg_name[20] = {0};
                int fg = -1, bg = -1;
                int i = 0;
                while (args[i] && args[i] != ' ') {
                    fg_name[i] = args[i];
                    i++;
                }
                fg_name[i] = '\0';
                fg = get_color_by_name(fg_name);
                if (args[i] == ' ') {
                    i++;
                    int j = 0;
                    while (args[i] && args[i] != ' ') {
                        bg_name[j] = args[i];
                        i++; j++;
                    }
                    bg_name[j] = '\0';
                    bg = get_color_by_name(bg_name);
                }
                if (fg == -1) {
                    printf("Unknown color: '%s'\n", fg_name);
                    printf("Type 'color list' to see available colors.\n");
                } else {
                    if (bg == -1) {
                        set_color(fg, VGA_COLOR_BLACK);
                        printf("Foreground set to %s.\n", color_names[fg]);
                    } else {
                        set_color(fg, bg);
                        printf("Foreground set to %s, background set to %s.\n",
                               color_names[fg], color_names[bg]);
                    }
                }
            }
        }
        else if (starts_with(input, "calc ")) {
            cmd_calc(input + 5);
        }
        else if (starts_with(input, "hex ")) {
            cmd_hex(input + 4);
        }
        else if (starts_with(input, "dec ")) {
            cmd_dec(input + 4);
        }
        else if (starts_with(input, "mem ")) {
            cmd_mem(input + 4);
        }
#ifdef HAS_LVGL
        else if (strcmp(input, "gui") == 0) {
            lvgl_demo();
            clear_screen();
            print_intro();
        }
#endif
        else if (input[0] != '\0') {
            /* Not a builtin command -> try GLOBAL TOOL DISPATCH: a
             * .mrp tool on the system path (root + /bin) can be
             * called by name from any directory. */
            if (!try_run_tool(input)) {
                if (strstr(input, ".mrp")) {
                    printf("'%s' is not a command or a tool in the system path.\n", input);
                    printf("Try: run %s   (or cd to its folder and use ./)\n", input);
                } else {
                    printf("Unknown command: '%s'. Type 'help'.\n", input);
                }
            }
        }
    }
}

// ============================================================
//  KERNEL MAIN — full boot sequence
// ============================================================

// Forward declaration: mrp_bootloader.cpp (loads GRUB multiboot modules into the RAMFS)
extern "C" int mrp_bootloader_load_modules(const multiboot_info_t* mb_info);

extern "C" void kernel_main(uint32_t mb_magic, multiboot_info_t* mb_info) {
    /* Boot banner. The console is still VGA text at this point; if
     * VESA comes up later, init_display() switches the view and
     * boot_log_replay() redraws the whole boot log. */
    clear_screen();
    /* Purple boot banner — matches the Equinox emblem palette
     * (set_fg_rgb snaps to light magenta in VGA text mode). */
    set_fg_rgb(0xB794FFu);
    printf("Equinox OS v0.1 Beta - booting\n\n");
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

    boot_log("Initializing syscall layer (int 0x80)");
    syscall_init();   /* reset tabel fd (file descriptor RAMFS) */
    sys_init();

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

    shell();
}
