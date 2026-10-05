// ============================================================
//  shell.cpp — Phase A: the Equinox OS interactive shell (split out
//  of kernel.cpp). Every SHELL TASK runs its own shell_entry() with
//  its own virtual console (F1 = new shell/console).
// ------------------------------------------------------------
//  arg == NULL  : the boot shell (task 0) — full intro + /user
//  arg != NULL  : an F1 shell — short banner + /user
// ============================================================

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
#include "gui/equinox_desktop/desktop.h"   /* 0.4 Beta: desktop EquiX + tvgdemo */
#include "library/header/itoa_atoi.h"
#include "library/header/clock.h"
#include "library/header/sys.h"
#include "library/header/vector.h"
#include "library/header/random.h"
#include "library/header/math_pi.h"
#include "library/header/vesa.h"
#include "library/header/panic.h"
#include "library/header/syscall.h"
#include "library/header/task.h"
#include "library/header/serial.h"
#include "library/header/fs_fat32.h"    // diskinfo / mount / umount
#include "library/header/blk.h"         // 0.4 Beta: slot table (PATA + AHCI)
#include "library/header/ecf.h"         // 0.4 Beta: .ecf (builtin `set`)
#include "library/header/eggkg.h"       // 0.4 Beta: eggkg package manager
#include "library/header/mrp_format.h"  // 0.4 Beta: pren (preview .mrp)
#include "library/header/usermode.h"   // usermode_print_memmap
#include "library/header/pci.h"        // 0.4 Beta: lspci (FR-12)
#include "net/net.h"
#include "net/ne2000.h"
#include <stdint.h>

// LVGL (compiled when LVGL source tree is present)
#ifdef HAS_LVGL
  #include "lvgl.h"
  #include "lv_port/lv_port_disp.h"
  #include "lv_port/lv_port_indev.h"
#endif

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

    /* 8. exit status echo (single-task: status returns to the caller) */
    int ex = syscall1(SYS_EXIT, 42);
    if (ex == 42) { printf("  PASS  exit(42) -> status echoed back %d\n", ex); pass++; }
    else          { printf("  FAIL  exit(42) returned %d\n", ex); fail++; }

    printf("== sctest finished: %d PASS, %d FAIL, %d SKIP ==\n", pass, fail, skip);
}

// ============================================================
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
    const char* version  = "0 . 4   B E T A";
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
//  SYSTEM SEARCH PATH (0.4 Beta): directories scanned by this
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
    /* 0.4 Beta: `dir` = the directory where the tool was FOUND (root,
     * /bin, /equinox/tools, /equinox/games) — not always root anymore. */
    syscall_set_args(args);
    mrp_set_quiet(1);
    mrp_run_hint(dir, fname, mrp_arena_hint_for(fname));
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

    // 0.4 Beta: system path = /, /bin, /equinox/tools, /equinox/games.
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

/* ============================================================
 *  0.4 Beta — eqbash file builtins + bash compatibility aliases
 * ------------------------------------------------------------
 *  9 bash-class tools (ls/cat/cp/mv/mkdir/rmdir/rm/touch/stat)
 *  moved to the eggkg repo (bash package), so eqbash must be rich
 *  enough for daily work: cdir/cfile/ccfile/save/delfile/
 *  deldir/pren + alias mkdir/touch/rm/rmdir/cp so old reflexes
 *  keep working on a thin base ISO.
 * ============================================================ */
static void sh_cdir(struct fs_node* dir, const char* name) {
    int ret = fs_create_dir(dir, name);
    if (ret == -2) printf("cdir: '%s' already exists\n", name);
    else if (ret == -7) printf("cdir: name too long (max 63 chars)\n");
    else if (ret != 0) printf("cdir: failed\n");
}
static void sh_cfile(struct fs_node* dir, const char* name) {
    int ret = fs_create_file(dir, name, NULL);
    if (ret == -2) printf("cfile: '%s' already exists\n", name);
    else if (ret == -7) printf("cfile: name too long (max 63 chars)\n");
    else if (ret != 0) printf("cfile: failed\n");
}
static void sh_delfile(struct fs_node* dir, const char* name) {
    struct fs_node* n = fs_find_child(dir, name);
    if (!n) { printf("delfile: '%s' not found\n", name); return; }
    if (n->is_dir) { printf("delfile: '%s' is a directory (use deldir)\n", name); return; }
    int ret = fs_delete_node(dir, name);
    if (ret == -9) printf("delfile: '%s' is an active mount point\n", name);
    else if (ret != 0) printf("delfile: failed (%d)\n", ret);
    else printf("delfile: '%s' removed\n", name);
}
static void sh_deldir(struct fs_node* dir, const char* name) {
    struct fs_node* n = fs_find_child(dir, name);
    if (!n) { printf("deldir: '%s' not found\n", name); return; }
    if (!n->is_dir) { printf("deldir: '%s' is a file (use delfile)\n", name); return; }
    int ret = fs_delete_node(dir, name);
    if (ret == -4) printf("deldir: '%s' not empty\n", name);
    else if (ret == -9) printf("deldir: '%s' is an active mount point\n", name);
    else if (ret != 0) printf("deldir: failed (%d)\n", ret);
    else printf("deldir: '%s' removed\n", name);
}

/* ============================================================
 *  0.4 Beta FR-01 — FILE UTILITIES AS USER PROGRAMS
 * ------------------------------------------------------------
 *  ls / cat / cp / mv / mkdir / rmdir / rm / touch / stat are
 *  USERLAND tools now: when <cmd>.mrp exists on the system path
 *  (/, /bin, /equinox/tools, /equinox/games) the user program
 *  takes PRECEDENCE over the kernel builtin — the tools gained
 *  the full syscall file API (open2/write/stat/readdir, #40-48)
 *  and moved out of ring 0. Returns 1 when the input line was
 *  consumed (tool ran / loader printed its own error), 0 = no
 *  tool found -> the kernel builtin path continues unchanged.
 * ============================================================ */
static int shell_try_user_tool(const char* input) {
    char word[32];
    uint32_t i = 0;
    while (input[i] && input[i] != ' ' && i < sizeof(word) - 1) {
        word[i] = input[i];
        i++;
    }
    word[i] = '\0';
    if (!word[0]) return 0;

    static const char* const k_tools[] = {
        "ls", "cat", "cp", "mv", "mkdir", "rmdir",
        "rm", "touch", "stat", NULL
    };
    int is_tool = 0;
    for (int t = 0; k_tools[t]; t++) {
        if (strcmp(word, k_tools[t]) == 0) { is_tool = 1; break; }
    }
    if (!is_tool) return 0;

    const char* args = input + i;
    while (*args == ' ') args++;

    char fname[44];
    for (int d = 0; d < SHELL_SYS_PATH_DIRS; d++) {
        struct fs_node* dir = shell_sys_path_dir(d);
        if (!dir || !dir->is_dir) continue;
        snprintf(fname, sizeof(fname), "%s.mrp", word);
        struct fs_node* n = fs_find_child(dir, fname);
        if (n && !n->is_dir) {
            return run_tool_quiet(dir, fname, args);
        }
    }
    return 0;                              /* no user tool: builtin path */
}


//  equinoxinstall — installer job model (0.4 Beta: 2-thread pool)
// ------------------------------------------------------------
//  One job = one mtcc compile. Three phases, one global counter:
//    1. libc   — check-compile every /equinox/libc module
//                (mtcc --lib -q -c): validates the dependency
//                graph in-OS. The .mrp artifact is deleted after
//                verification — the .c sources STAY (mtcc splices
//                them for every later #include <morph.h>).
//    2. tools  — compile + install; source removed on success.
//    3. games  — compile + install; source removed on success.
//
//  Execution: TWO user tasks running mtcc.mrp in parallel (Phase B
//  multitasking — each task maps its own MRP arena at VMA 0x500000,
//  so the two compiler instances never share state). The shell
//  BLOCKS in task_wait_pid() while they work, then verifies the
//  output files and prints the per-job log lines itself (mtcc runs
//  with -q: silent on success, errors only).
// ============================================================
#define EQINSTALL_MAX 40        /* per-directory source snapshot cap */
#define EQINSTALL_NAME 64
#define EQI_THREADS 2           /* parallel compiler tasks           */

enum { EQJ_LIBC = 0, EQJ_TOOL = 1, EQJ_GAME = 2 };

struct EqJob {
    char            src[96];                  /* /equinox/<dir>/<file>.c */
    char            sname[EQINSTALL_NAME];   /* foo.c                   */
    char            mname[EQINSTALL_NAME];   /* foo.mrp                 */
    struct fs_node* dir;                     /* directory of both       */
    int             kind;                    /* EQJ_*                   */
};

static const char* eqjob_dirname(int kind) {
    return (kind == EQJ_LIBC) ? "libc"
         : (kind == EQJ_TOOL) ? "tools" : "games";
}

/* ---- 0.4 Beta wizard state (used by plan/collect too) -------------
 * eqi_src_root  job source prefix: "/equinox" (in place, base
 *               volume) or "/mnt/equinox" (generic target just
 *               mounted) — mtcc writes the .mrp next to its source,
 *               so the artifact lands on the TARGET VOLUME automatically.
 * eqi_on_target 1 = job runs from /mnt: sources are NOT deleted
 *               (the sources are the install content themselves) and
 *               there is no extra export (the result is already on disk). */
static int        eqi_on_target = 0;
static const char* eqi_src_root  = "/equinox";

/* Colored log prefix: "[tag] : " — the message follows on the same
 * line (printf by the caller), terminated by the caller's \n. */
static void eqi_tag(const char* tag) {
    if (strcmp(tag, "info") == 0) {
        set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    } else if (strcmp(tag, "warn") == 0) {
        set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    } else {
        set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    }
    printf("[%s] : ", tag);
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* Full log line: "[tag] : msg\n". */
static void eqi_log(const char* tag, const char* msg) {
    eqi_tag(tag);
    printf("%s\n", msg);
}

/* Installer banner: clear screen, emblem (gradient), wordmark.
 * Kept visually consistent with print_intro() on purpose. */
static void eqinstall_banner(void) {
    clear_screen();
    int cols = term_get_cols();

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

    const char* wordmark = "E Q U I N O X   I N S T A L L E R";
    const char* version  = "0 . 4   B E T A";
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

/* Snapshot the .c file names of `dir` into names[][]. Returns the
 * count (0..EQINSTALL_MAX); names beyond the cap are skipped. */
static int eqinstall_scan(struct fs_node* dir, char names[][EQINSTALL_NAME]) {
    int n = 0;
    if (!dir || !dir->is_dir) return 0;
    /* 0.4 Beta: a FAT32 directory lazily mirrors its CONTENT on the
     * first lookup — without this the list is still empty although the
     * node exists, and the plan reports "no .c sources found" on a base install. */
    if (dir->backing == 1 && !dir->populated) fat32_populate_dir(dir);
    for (struct fs_node* c = dir->children; c && n < EQINSTALL_MAX; c = c->next) {
        if (c->is_dir) continue;
        size_t l = strlen(c->name);
        /* ".c" OR ".C": 8.3 names on FAT32 often come upper-case
         * ("WHICH.C") when mtools does not create an LFN entry for it. */
        if (l > 2 && c->name[l - 2] == '.' &&
            (c->name[l - 1] == 'c' || c->name[l - 1] == 'C')) {
            snprintf(names[n], EQINSTALL_NAME, "%s", c->name);
            n++;
        }
    }
    return n;
}

/* Build the job list from the three RAMFS directories. The snapshot
 * is taken BEFORE any compile (nodes are deleted while going).
 * Returns the job count. */
/* 0.4 Beta — JOB ORDER: products first (tools + games), libc check-compile
 * LAST. Memory reason: every job writes a .mrp (~31 KB) then discards
 * its source, while the libc artifact is deleted right after it is
 * verified (it is temporary). If libc runs first, 12 holes of ~31 KB
 * are scattered across the heap BEFORE the 29 big products are
 * allocated; the free block gets fragmented and the last 31 KB malloc
 * fails (kernel heap is only 1.29 MB) -> "compile failed (source kept)".
 * Products first = large allocations run in sequence on one free
 * block; the libc holes are then made and immediately reused at the end. */
static int eqinstall_plan(struct fs_node* ldir, struct fs_node* tdir,
                          struct fs_node* gdir, struct EqJob* jobs) {
    int total = 0;

    static char tnames[EQINSTALL_MAX][EQINSTALL_NAME];
    int tn = eqinstall_scan(tdir, tnames);
    for (int i = 0; i < tn && total < EQINSTALL_MAX * 3; i++) {
        struct EqJob* j = &jobs[total];
        snprintf(j->src, sizeof(j->src), "%s/tools/%s", eqi_src_root, tnames[i]);
        snprintf(j->sname, EQINSTALL_NAME, "%s", tnames[i]);
        snprintf(j->mname, EQINSTALL_NAME, "%s", tnames[i]);
        size_t ml = strlen(j->mname);
        if (ml >= 2) {
            j->mname[ml - 2] = '\0';
            snprintf(j->mname + ml - 2, EQINSTALL_NAME - (ml - 2), ".mrp");
        }
        j->dir = tdir;
        j->kind = EQJ_TOOL;
        total++;
    }

    static char gnames[EQINSTALL_MAX][EQINSTALL_NAME];
    int gn = eqinstall_scan(gdir, gnames);
    for (int i = 0; i < gn && total < EQINSTALL_MAX * 3; i++) {
        struct EqJob* j = &jobs[total];
        snprintf(j->src, sizeof(j->src), "%s/games/%s", eqi_src_root, gnames[i]);
        snprintf(j->sname, EQINSTALL_NAME, "%s", gnames[i]);
        snprintf(j->mname, EQINSTALL_NAME, "%s", gnames[i]);
        size_t ml = strlen(j->mname);
        if (ml >= 2) {
            j->mname[ml - 2] = '\0';
            snprintf(j->mname + ml - 2, EQINSTALL_NAME - (ml - 2), ".mrp");
        }
        j->dir = gdir;
        j->kind = EQJ_GAME;
        total++;
    }

    static char lnames[EQINSTALL_MAX][EQINSTALL_NAME];
    int ln = eqinstall_scan(ldir, lnames);
    for (int i = 0; i < ln && total < EQINSTALL_MAX * 3; i++) {
        struct EqJob* j = &jobs[total];
        snprintf(j->src, sizeof(j->src), "%s/libc/%s", eqi_src_root, lnames[i]);
        snprintf(j->sname, EQINSTALL_NAME, "%s", lnames[i]);
        snprintf(j->mname, EQINSTALL_NAME, "%s", lnames[i]);
        size_t ml = strlen(j->mname);
        if (ml >= 2) {
            j->mname[ml - 2] = '\0';
            snprintf(j->mname + ml - 2, EQINSTALL_NAME - (ml - 2), ".mrp");
        }
        j->dir = ldir;
        j->kind = EQJ_LIBC;
        total++;
    }
    return total;
}

/* Verify one finished job and print its log line.
 *  - libc: the .mrp is a check-compile artifact -> verified, logged,
 *    then DELETED (the .c source stays: mtcc splices it later).
 *  - tools/games: the .mrp is the installed program -> verified,
 *    logged, and the .c source is removed (already-built marker).
 * Returns 1 on success. *bytes_out accumulates installed sizes. */
/* 0.4 Beta — BASE BUILD (where compile output goes is set by the FAT32 volume):
 *   "base"  FAT32 volume mounted as ROOT (/ , label EQUINOXBASE)
 *           -> /equinox/* is already on disk, every build result is
 *              written through and survives reboot.
 *   "mnt"   FAT32 volume at /mnt (generic disk, the equinox tree is NOT
 *           there) -> sources stay in RAMFS, the built .mrp artifacts
 *           are COPIED to /mnt/equinox/{tools,games}.
 *   "ramfs" no volume -> everything in RAMFS (lost on reboot). */
static const char* eqi_base_mode(void) {
    struct fs_node* root = fs_get_root();
    if (!root) return "ramfs";
    if (root->backing == 1) return "base";
    struct fs_node* mnt = fs_find_child(root, "mnt");
    if (mnt && mnt->backing == 1) return "mnt";
    return "ramfs";
}

/* mkdir one level under `parent` (idempotent). */
static struct fs_node* eqi_dir_under(struct fs_node* parent, const char* name) {
    if (!parent || !parent->is_dir) return NULL;
    struct fs_node* d = fs_find_child(parent, name);
    if (!d) {
        if (fs_create_dir(parent, name) != 0) return NULL;
        d = fs_find_child(parent, name);
    }
    return (d && d->is_dir) ? d : NULL;
}

/* Mode "mnt": copy build artifacts to the FAT32 volume at /mnt.
 * (mtcc has no -o, so the result is always written next to the source —
 * this move is what puts it on disk.) */
static void eqi_export_to_mnt(struct EqJob* j) {
    if (j->kind == EQJ_LIBC) return;          /* libc artifacts are discarded anyway */
    if (eqi_base_mode()[0] != 'm') return;
    struct fs_node* src = fs_find_child(j->dir, j->mname);
    if (!src || src->is_dir) return;
    if (fs_ensure_content(src) != 0 || !src->content || src->size == 0)
        return;
    struct fs_node* mnt = fs_find_child(fs_get_root(), "mnt");
    struct fs_node* eq  = eqi_dir_under(mnt, "equinox");
    struct fs_node* sub = eqi_dir_under(eq, eqjob_dirname(j->kind));
    if (!sub) return;
    if (fs_find_child(sub, j->mname)) return;  /* already on disk */
    if (fs_write_binary(sub, j->mname,
                        (const uint8_t*)src->content, src->size) == 0) {
        eqi_tag("info");
        printf("       -> /mnt/equinox/%s/%s  (FAT32, persistent)\n",
               eqjob_dirname(j->kind), j->mname);
    }
}

static int eqinstall_collect(struct EqJob* j, int idx, int total,
                             int thread, uint32_t* bytes_out) {
    struct fs_node* out = fs_find_child(j->dir, j->mname);
    if (!out || out->is_dir || out->size == 0) {
        eqi_tag("fail");
        printf("[%2d/%d] T%d %s/%s — compile failed (source kept)\n",
               idx, total, thread, eqjob_dirname(j->kind), j->sname);
        /* 0.4 Beta: mtcc only reports "error writing" — the heap is
         * the most common cause (fs_write_binary -> -3 when malloc fails) */
        printf("        (kernel heap: used %u/%u B, largest free %u K)\n",
               (unsigned)get_heap_used(), (unsigned)get_heap_total(),
               (unsigned)(get_heap_largest_free() / 1024));
        return 0;
    }

    if (j->kind == EQJ_LIBC) {
        fs_delete_node(j->dir, j->mname);       /* check artifact only */
        eqi_tag("info");
        printf("[%2d/%d] T%d libc/%-10s   OK   %6u B  module verified\n",
               idx, total, thread, j->sname, out->size);
        return 1;
    }

    /* 0.4 Beta — build di volume TARGET (/mnt): sumber .c berada di disk
     * target, jadi .mrp juga tertulis langsung di sana. Sumbernya
     * TIDAK dihapus: ia adalah isi install (make img juga menyimpan
     * .c di image) dan jadi penanda "sudah dibangun". */
    if (eqi_on_target) {
        eqi_tag("info");
        printf("[%2d/%d] T%d %s/%-10s -> %-10s %6u B  installed (target)\n",
               idx, total, thread, eqjob_dirname(j->kind),
               j->sname, j->mname, out->size);
        eqi_tag("info");
        printf("       -> %s/%s/%s  (FAT32, persistent)\n",
               eqi_src_root, eqjob_dirname(j->kind), j->mname);
        *bytes_out += out->size;

        /* Cermin hasil ke pohon RAMFS (/equinox/...): volume target
         * terpasang di /mnt, bukan di /, jadi shell sesi INI tetap
         * mencari tool di /equinox — tanpa salinan ini `ls.mrp`
         * tidak ada dan tool jatuh ke builtin. Kepemilikan: salinan
         * sementara (RAMFS), versi persistent ada di volume target. */
        char rp[64];
        snprintf(rp, sizeof(rp), "/equinox/%s", eqjob_dirname(j->kind));
        struct fs_node* ramdir = fs_get_node_from_path(fs_get_root(), rp);
        if (ramdir && ramdir->is_dir) {
            if (!fs_find_child(ramdir, j->mname) &&
                fs_ensure_content(out) == 0 && out->content &&
                fs_write_binary(ramdir, j->mname,
                                (const uint8_t*)out->content, out->size) == 0) {
                eqi_tag("info");
                printf("       = /equinox/%s/%s  (RAMFS sesi ini)\n",
                       eqjob_dirname(j->kind), j->mname);
            }
            /* sumber .c di RAMFS TIDAK dipakai build (jalurnya di
             * volume) — buang agar pohon RAMFS selaras dengan mode
             * in-place dan heap tetap lega untuk cerminan di atas;
             * salinan persistentnya ada di volume target. */
            fs_delete_node(ramdir, j->sname);
        }
        return 1;
    }

    if (fs_delete_node(j->dir, j->sname) == 0) {
        eqi_tag("info");
        printf("[%2d/%d] T%d %s/%-10s -> %-10s %6u B  installed\n",
               idx, total, thread, eqjob_dirname(j->kind),
               j->sname, j->mname, out->size);
    } else {
        eqi_tag("warn");
        printf("[%2d/%d] T%d %s/%-10s -> %-10s %6u B  installed (source kept: rm failed)\n",
               idx, total, thread, eqjob_dirname(j->kind),
               j->sname, j->mname, out->size);
    }
    *bytes_out += out->size;
    eqi_export_to_mnt(j);          /* 0.4 Beta: mode FAT32 /mnt */
    return 1;
}

/* Synchronous fallback: run mtcc for one job INSIDE the shell task
 * (the 0.4 Beta path). Used when task_create_user fails — the pool
 * degrades to single-thread instead of aborting the install. */
static int eqinstall_run_sync(struct fs_node* tdir, struct EqJob* j,
                              int idx, int total, uint32_t* bytes_out) {
    char args[128];
    if (j->kind == EQJ_LIBC)
        snprintf(args, sizeof(args), "--lib -q -c %s", j->src);
    else
        snprintf(args, sizeof(args), "-q -c %s", j->src);
    syscall_set_args(args);
    mrp_set_quiet(1);
    mrp_run_hint(tdir, "mtcc.mrp", mrp_arena_hint_for("mtcc.mrp"));
    mrp_set_quiet(0);
    syscall_set_args("");
    return eqinstall_collect(j, idx, total, 0, bytes_out);
}

/* ============================================================
 *  0.4 Beta — WIZARD HELPERS (equinoxinstall interaktif)
 * ------------------------------------------------------------
 *  [1/4] pilih disk target      tabel hda..hdd dari slot ATA
 *  [2/4] siapkan volume         format (fat32_mkfs) + label
 *        + salin layout         EQUINOXBASE + mount, salin pohon
 *                                /equinox /user /test *.mrp *.wad
 *                                + /boot/kernel.elf + README.TXT
 *  [3/4] driver jaringan        tulis <target>/equinox/conf/system.ecf
 *  [4/4] build                  pool mtcc dari sumber di VOLUME
 *                                TARGET + baris status live
 * ============================================================ */

/* ---- prompt: gets() satu baris, Enter = konfirmasi ---- */
static char eqi_ans[80];

static const char* eqi_ask(const char* q) {
    printf("%s", q);
    eqi_ans[0] = '\0';
    gets(eqi_ans, (int)sizeof(eqi_ans));
    return eqi_ans;
}

static int eqi_yes(const char* q) {
    const char* a = eqi_ask(q);
    return (a[0] == 'y' || a[0] == 'Y' || a[0] == '1' ||
            a[0] == 't' || a[0] == 'T');
}

/* ---- [1/4] tabel disk: hanya slot yang benar-benar berisi drive ---- */
static int eqi_slot_of[BLK_MAX_DRIVES];  /* nomor menu -> slot blk    */

static int eqi_disk_menu(void) {
    int n = 0;
    for (int s = 0; s < blk_count(); s++) {
        struct fat32_slot_info in;
        if (fat32_slot_scan(s, &in) != 0) continue;   /* slot kosong   */
        n++;
        eqi_slot_of[n - 1] = s;
        /* penamaan disk mengikuti konvensi Linux: hda, hdb, ... (slot
         * PATA 0..3 = hda..hdD, disk AHCI 4..7 = hde..hdh) */
        printf("   %d) hd%c  %-8s ", n, 'a' + s, in.size);
        if (in.has_fat32) {
            printf("FAT32 \"%s\"  partisi %s",
                   in.label[0] ? in.label : "-", in.part_size);
            if (in.active)
                printf(in.is_base ? "   [ ROOT / ]" : "   [ /mnt ]");
            printf("\n");
        } else {
            printf("kosong — belum dipartisi (bisa diformat)\n");
        }
    }
    return n;
}

/* ---- D: progres salin file besar (doom1.wad / doom.wad 4 MB) ----
 * fat32_write_file() memanggil hook ini tiap ~128 KB selama penulisan.
 * Cetak maksimal tiap 10 % supaya log tetap ringan; file < 512 KB
 * dilewati (selesai sekejap — baris "copy ... OK" sudah cukup).
 * Deteksi file baru: `done` turun dari akhir file sebelumnya. */
static uint32_t eqi_prog_last_done = 0;
static uint32_t eqi_prog_next_pct  = 10;
static void eqi_copy_progress(uint32_t done, uint32_t total) {
    if (total < 524288u) return;
    if (done < eqi_prog_last_done) eqi_prog_next_pct = 10;   /* file baru */
    eqi_prog_last_done = done;
    uint32_t pct = (uint32_t)(((uint64_t)done * 100) / total);
    if (pct < eqi_prog_next_pct || pct >= 100) return;
    eqi_prog_next_pct = pct + 10;
    eqi_tag("info");
    printf("[wizard]       salin progres: %u%% (%u/%u KB)\n",
           pct, (unsigned)(done / 1024), (unsigned)(total / 1024));
}

/* ---- salin satu subpohon (file ditulis ulang bila sudah ada) ---- */
static void eqi_copy_tree(struct fs_node* src, struct fs_node* dst,
                          int* nfiles, int* nbytes) {
    if (!src || !src->is_dir || !dst || !dst->is_dir) return;
    if (src->backing == 1 && !src->populated) fat32_populate_dir(src);
    int copied = 0;
    for (struct fs_node* c = src->children; c; c = c->next) {
        if (c->is_dir) {
            struct fs_node* sub = eqi_dir_under(dst, c->name);
            if (sub) eqi_copy_tree(c, sub, nfiles, nbytes);
            continue;
        }
        if (fs_ensure_content(c) != 0 || !c->content || c->size == 0)
            continue;
        /* sudah ada dengan ukuran sama -> jangan ditulis ulang
         * (pemasangan ulang jadi cepat dan idempoten) */
        struct fs_node* ex = fs_find_child(dst, c->name);
        if (ex && !ex->is_dir && ex->size == c->size) continue;
        eqi_tag("info");
        printf("[wizard]       copy: %s/%s (%u bytes) ...\n",
               src->name, c->name, (unsigned)c->size);
        if (fs_write_binary(dst, c->name,
                            (const uint8_t*)c->content, c->size) == 0) {
            eqi_tag("info");
            printf("[wizard]       copy: %s/%s OK\n", src->name, c->name);
            (*nfiles)++;
            *nbytes += (int)c->size;
            copied++;
        } else {
            eqi_tag("fail");
            printf("[wizard]       copy: %s/%s GAGAL\n", src->name, c->name);
        }
    }
    if (copied > 0) {
        eqi_tag("info");
        printf("[wizard]     eqi_copy_tree: %d file disalin dari %s\n", copied, src->name);
    }
}

/* ---- [2/4] salin layout ISO -> volume target (meniru
 *  scripts/make_equinox_img.py: boot/kernel.elf, pohon equinox/,
 *  user/, test/, modul .mrp + .wad datar, README.TXT). ---- */
static void eqi_export_layout(struct fs_node* mnt, int* nfiles,
                              int* nbytes) {
    struct fs_node* root = fs_get_root();
    if (!mnt || !root) return;

    eqi_tag("info");
    printf("[wizard] eqi_export_layout: mulai salin /equinox /user /test + .mrp/.wad + kernel.elf\n");
    /* (D) progres hidup selama salin — dilepas di akhir fungsi */
    fat32_set_write_progress(eqi_copy_progress);

    struct fs_node* seq = fs_get_node_from_path(root, "/equinox");
    struct fs_node* meq = eqi_dir_under(mnt, "equinox");
    if (seq && meq) {
        eqi_tag("info");
        printf("[wizard]   salin /equinox -> /mnt/equinox/\n");
        eqi_copy_tree(seq, meq, nfiles, nbytes);
    }

    struct fs_node* su = fs_get_node_from_path(root, "/user");
    struct fs_node* mu = eqi_dir_under(mnt, "user");
    if (su && mu) {
        eqi_tag("info");
        printf("[wizard]   salin /user -> /mnt/user/\n");
        eqi_copy_tree(su, mu, nfiles, nbytes);
    }

    struct fs_node* st = fs_get_node_from_path(root, "/test");
    struct fs_node* mt = eqi_dir_under(mnt, "test");
    if (st && mt) {
        eqi_tag("info");
        printf("[wizard]   salin /test -> /mnt/test/\n");
        eqi_copy_tree(st, mt, nfiles, nbytes);
    }

    /* module GRUB datar (*.mrp, *.wad) -> akar volume target */
    int flat_copied = 0;
    for (struct fs_node* c = root->children; c; c = c->next) {
        if (c->is_dir) continue;
        size_t l = strlen(c->name);
        if (l < 5) continue;
        if (strcmp(c->name + l - 4, ".mrp") != 0 &&
            strcmp(c->name + l - 4, ".wad") != 0) continue;
        eqi_tag("info");
        printf("[wizard]     copy flat: %s (%u bytes) ...\n", c->name, (unsigned)c->size);
        if (fs_ensure_content(c) != 0 || !c->content || c->size == 0) {
            eqi_tag("warn");
            printf("[wizard]     copy flat: %s SKIP (no content)\n", c->name);
            continue;
        }
        struct fs_node* ex = fs_find_child(mnt, c->name);
        if (ex && !ex->is_dir && ex->size == c->size) {
            eqi_tag("info");
            printf("[wizard]     copy flat: %s SKIP (sudah ada, ukuran sama)\n", c->name);
            continue;
        }
        if (fs_write_binary(mnt, c->name, (const uint8_t*)c->content,
                            c->size) == 0) {
            eqi_tag("info");
            printf("[wizard]     copy flat: %s OK\n", c->name);
            (*nfiles)++;
            *nbytes += (int)c->size;
            flat_copied++;
        } else {
            eqi_tag("fail");
            printf("[wizard]     copy flat: %s GAGAL\n", c->name);
        }
    }
    if (flat_copied > 0) {
        eqi_tag("info");
        printf("[wizard]   salin %d file .mrp/.wad datar ke /mnt/\n", flat_copied);
    }

    /* /boot/kernel.elf — kernel GRUB untuk boot berikutnya
     * (modul ISO: salinan stripped, 1,5 MB; lihat boot/grub/grub.cfg) */
    struct fs_node* sk = fs_get_node_from_path(root, "/boot/kernel.elf");
    struct fs_node* mb = eqi_dir_under(mnt, "boot");
    if (sk && mb && !sk->is_dir && fs_ensure_content(sk) == 0 &&
        sk->content && sk->size > 0) {
        eqi_tag("info");
        printf("[wizard]   salin /boot/kernel.elf -> /mnt/boot/kernel.elf (%u bytes)\n",
               (unsigned)sk->size);
        if (fs_write_binary(mb, "kernel.elf",
                            (const uint8_t*)sk->content, sk->size) == 0) {
            eqi_tag("info");
            printf("[wizard]     copy kernel.elf OK\n");
            (*nfiles)++;
            *nbytes += (int)sk->size;
        } else {
            eqi_tag("fail");
            printf("[wizard]     copy kernel.elf GAGAL\n");
        }
    }

    static const char* rd =
        "equinox OS INSTALL image (EQUINOXBASE)\r\n"
        "=====================================\r\n\r\n"
        "Dibuat oleh equinoxinstall, di dalam OS.\r\n"
        "Label EQUINOXBASE = volume ini dipasang sebagai ROOT (/).\r\n"
        "Isi: /boot /user /equinox {libc,tools,games} /test *.mrp\r\n";
    if (fs_write_binary(mnt, "README.TXT", (const uint8_t*)rd,
                        (uint32_t)strlen(rd)) == 0) {
        (*nfiles)++;
        *nbytes += (int)strlen(rd);
    }
    fat32_set_write_progress(NULL);   /* (D) lepas hook progres */
}

/* ---- [3/4] driver choice -> <target>/equinox/conf/system.ecf ---
 * The single-word boot/nic.cfg format is GONE, replaced by system.ecf
 * in INI-lite form:
 *     # comment
 *     [net]
 *     driver = e1000
 * Parsed by ecf.c (ecf_parse) — keys become "net.driver" and are
 * consumed by net_nic_init() at boot.
 * The file lives in /equinox/conf/system.ecf so it stays easy to edit
 * by hand (boot reads it through ecf_active_path: conf first, legacy
 * boot/ as fallback). */
static int eqi_pick_nic(struct fs_node* where, const char* tname) {
    int have_e1000 = (pci_find(0x8086, 0x100E) != NULL) ||
                     (pci_find(0x8086, 0x100F) != NULL);
    printf("\n   1) ne2000   NE2000 ISA 0x300 IRQ9 — default\n");
    printf("   2) e1000    Intel PRO/1000 PCI — %s\n",
           have_e1000 ? "TERDETEKSI di bus PCI" : "tidak terlihat di PCI");
    printf("   3) tanpa jaringan\n");

    int v = 0;
    while (v < 1 || v > 3) {
        const char* a = eqi_ask("   pilihan [1]: ");
        v = atoi(a);
        if (v == 0 && !a[0]) v = 1;              /* Enter = default    */
    }
    const char* w = (v == 2) ? "e1000" : (v == 3) ? "none" : "ne2000";

    /* 0.4 Beta: /equinox/conf/system.ecf (dahulu /boot/system.ecf). */
    struct fs_node* eqx  = eqi_dir_under(where, "equinox");
    struct fs_node* conf = eqx ? eqi_dir_under(eqx, "conf") : NULL;
    if (!conf) { eqi_log("fail", "tidak bisa membuat /equinox/conf di target"); return -1; }
    char cfg[160];
    uint32_t n = 0;
    /* salin manual — snprintf kernel tanpa precision, tapi ini pun
     * dibangun baris per baris supaya aman. */
    const char* hdr = "# konfigurasi sistem — ditulis oleh equinoxinstall\n[net]\ndriver = ";
    for (const char* s = hdr; *s && n < sizeof(cfg) - 2; s++) cfg[n++] = *s;
    for (const char* s = w; *s && n < sizeof(cfg) - 2; s++) cfg[n++] = *s;
    cfg[n++] = '\n';
    cfg[n] = '\0';
    if (fs_write_binary(conf, "system.ecf", (const uint8_t*)cfg, n) != 0) {
        eqi_log("fail", "gagal menulis equinox/conf/system.ecf");
        return -1;
    }
    eqi_tag("info");
    printf("system.ecf -> %s/equinox/conf/system.ecf = %s  (edit manual, efektif saat reboot)\n",
           tname, w);
    if (v == 2 && !have_e1000)
        eqi_log("warn", "tidak ada e1000 di PCI — boot nanti jatuh ke ne2000");
    return 0;
}

/* ---- [4/4] baris status live di baris bawah layar ---------------
 * Digambar sebelum blok tunggu, dihapus setelahnya: kembali selalu
 * di baris bawah col 0, jadi log berikutnya menulis tepat di situ
 * dan layar naik normal (tanpa baris kosong sisa status). */
static void eqi_status_draw(int done, int total, int ln, int lok,
                            int tn, int tok, int gn, int gok,
                            uint32_t t0) {
    int rows = term_get_rows();
    int cols = term_get_cols();
    char line[160];
    char blank[160];
    /* get_tick() = TICK (100 Hz), bukan milidetik — dulu dibagi 1000
     * sehingga status selalu "0:00". Detik = tick / frekuensi timer. */
    uint32_t hz = timer_freq_hz();
    uint32_t sec = hz ? (get_tick() - t0) / hz : 0;
    /* h = largest free kernel-heap block (KB): the .mrp writes go
     * through malloc(), so this is the value that decides whether a
     * job can land (fs_write_binary returns -3 on OOM). */
    snprintf(line, sizeof(line),
             " [build] %d/%d  libc %d/%d  tools %d/%d  games %d/%d  "
             "h=%uK  %u:%02u ",
             done, total, lok, ln, tok, tn, gok, gn,
             (unsigned)(get_heap_largest_free() / 1024),
             (unsigned)(sec / 60), (unsigned)(sec % 60));
    term_set_cursor_visible(0);
    set_cursor_position(rows - 1, 0);
    set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    int used = (int)strlen(line);
    if (used > 157) used = 157;
    /* terpisah dari serial: digambar ulang puluhan kali per build */
    term_console_puts(line);
    int pad = 0;
    while (used + pad < cols && used + pad < 157) blank[pad++] = ' ';
    blank[pad] = '\0';
    term_console_puts(blank);
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

static void eqi_status_erase(void) {
    int rows = term_get_rows();
    int cols = term_get_cols();
    char blank[160];
    int n = (cols < 157) ? cols : 157;
    for (int i = 0; i < n; i++) blank[i] = ' ';
    blank[n] = '\0';
    set_cursor_position(rows - 1, 0);
    term_console_puts(blank);
    set_cursor_position(rows - 1, 0);
    term_set_cursor_visible(1);
}

static void eqi_build_userland(int build_mode, const char* tname);
static void eqi_compile_only(struct fs_node* cwd, const char* dirarg);
static void qfs_install_boot(const char* name);   /* 0.4 Beta -install-boot */
static void eqi_build_ruf(struct fs_node* cwd, const char* rufarg);
static void shell_resolve_path(struct fs_node* cwd, const char* arg,
                               char* out, uint32_t outsz);
static void shell_copy(char* dst, uint32_t dstsz, const char* src);

static char eqi_src_buf[300];   /* -compile: salinan path sumber */

/* ============================================================
 *  equinoxinstall
 *    (tanpa argumen)      wizard 4 fase — PERILAKU LAMA, tak berubah
 *    -compile <dir>       HANYA fase [4/4] dari <dir>; tanpa wizard,
 *                         tanpa format disk, tanpa ekspor layout.
 *    -build <file.ruf>    build via `mtcc -make` (build file .ruf);
 *                         sumber ditunjuk sendiri oleh .ruf.
 *    -build <nama>        build satu tool dari /equinox/tools
 *                         (mtcc -c); "mtcc" = status self-host.
 *    -build <pola*>       glob .ruf — mis. `*.ruf` di /equinox.
 * ============================================================ */
static void cmd_equinoxinstall(struct fs_node* cwd, const char* args) {
    if (args && args[0]) {
        if (strncmp(args, "-compile", 8) == 0 &&
            (args[8] == '\0' || args[8] == ' ')) {
            const char* d = args + 8;
            while (*d == ' ') d++;
            eqi_compile_only(cwd, d);
            return;
        }
        if (strncmp(args, "-build", 6) == 0 &&
            (args[6] == '\0' || args[6] == ' ')) {
            const char* r = args + 6;
            while (*r == ' ') r++;
            eqi_build_ruf(cwd, r);
            return;
        }
        printf("equinoxinstall              wizard instalasi 4 fase\n");
        printf("equinoxinstall -compile <dir>   hanya kompilasi userland\n");
        printf("equinoxinstall -build <file.ruf>   build via mtcc -make\n");
        printf("equinoxinstall -build <nama>       build satu tool (mtcc -c)\n");
        printf("equinoxinstall -build <pola*>      mis. *.ruf — build semua\n");
        return;
    }

    struct fs_node* root = fs_get_root();

    /* 1. clear + banner (emblem + installer wordmark) */
    eqinstall_banner();

    /* ===== [1/4] PILIH DISK TARGET ===== */
    printf("[1/4] pilih disk target\n");
    int nmenu = eqi_disk_menu();
    int disk_slot = -1;
    char tname[16];
    snprintf(tname, sizeof(tname), "(in place)");
    if (nmenu == 0) {
        /* Tanpa disk: wizard dilewati, build berjalan persis seperti
         * sebelumnya (RAMFS, atau volume base bila sedang terpasang). */
        eqi_log("info", "tidak ada disk ATA terpasang — build in place");
    } else {
        printf("   0)  build in place (tanpa install ke disk)\n");
        while (disk_slot < 0) {
            const char* a = eqi_ask("  target [0]: ");
            int v = atoi(a);
            if (v == 0) break;                       /* Enter / 0 */
            if (v >= 1 && v <= nmenu) disk_slot = eqi_slot_of[v - 1];
        }
        if (disk_slot < 0)
            eqi_log("info", "build in place — instalasi ke disk dilewati");
        else
            snprintf(tname, sizeof(tname), "hd%c", 'a' + disk_slot);
    }

    int build_mode = 0;   /* 1 = in-place pada volume base (root /)  */
                          /* 2 = volume di /mnt, build dari sana     */
    if (disk_slot >= 0) {
    /* ===== [2/4] SIAPKAN VOLUME + SALIN LAYOUT ===== */
    printf("\n[2/4] siapkan volume target (%s)\n", tname);
    struct fat32_slot_info in;
    if (fat32_slot_scan(disk_slot, &in) != 0) {
        eqi_log("fail", "slot kehilangan drive di tengah jalan");
        return;
    }
    eqi_tag("info");
    printf("[wizard] cek volume %s: has_fat32=%d label='%s' active=%d is_base=%d size=%s\n",
           tname, in.has_fat32, in.label[0] ? in.label : "-",
           in.active, in.is_base, in.size);

    if (in.active && in.is_base) {
        eqi_tag("info");
        printf("%s = volume aktif (/ , label EQUINOXBASE) — build in place\n",
               tname);
        build_mode = 1;
    } else {
        int relabel = 0;
        if (!in.has_fat32) {
            eqi_tag("warn");
            printf("%s (%s) belum berisi partisi FAT32\n", tname, in.size);
            eqi_tag("info");
            printf("[wizard] memulai format %s -> FAT32 label EQUINOXBASE...\n", tname);
            char q[112];
            snprintf(q, sizeof(q),
                     "  Format %s jadi base equinox? SEMUA ISI AKAN HILANG [y/t]: ",
                     tname);
            if (!eqi_yes(q)) {
                eqi_log("info", "installer dibatalkan");
                return;
            }
            eqi_tag("info");
            printf("[wizard] memanggil fat32_mkfs(slot=%d)...\n", disk_slot);
            int r = fat32_mkfs(disk_slot, "EQUINOXBASE");
            if (r != 0) { eqi_tag("fail"); printf("fat32_mkfs gagal (%d)\n", r); return; }
            eqi_tag("info");
            printf("%s diformat — MBR + partisi FAT32 0x0C @ LBA 2048, label EQUINOXBASE\n",
                   tname);
            in.has_fat32 = 1;
        } else if (strcmp(in.label, "EQUINOXBASE") != 0) {
            char q[128];
            snprintf(q, sizeof(q),
                     "  partisi label \"%s\" — ganti jadi EQUINOXBASE agar bootable [y/t]: ",
                     in.label[0] ? in.label : "-");
            relabel = eqi_yes(q);
        }

        eqi_tag("info");
        printf("[wizard] memasang volume slot %d ke /mnt...\n", disk_slot);
        int r = fat32_mount_slot(disk_slot);
        if (r != 0) {
            eqi_tag("fail");
            printf("mount slot %d gagal (%d) — volume base sedang memakai /\n",
                   disk_slot, r);
            return;
        }
        eqi_tag("info");
        printf("[wizard] volume terpasang di /mnt\n");
        if (relabel && fat32_set_label("EQUINOXBASE") == 0) {
            eqi_tag("info");
            printf("label diganti -> EQUINOXBASE (volume jadi ROOT pada boot berikutnya)\n");
        }
        build_mode = 2;
    }
    }   /* disk_slot >= 0 — tanpa pilihan disk, build_mode tetap 0 */

    /* Baris kompatibilitas lama — tes dan dokumentasi mencarinya:
     * memberitahu di mana hasil build akan mendarat. */
    eqi_tag("info");
    if (build_mode == 1) {
        printf("build base = FAT32 / (EQUINOXBASE) — hasil di DISK\n");
    } else if (build_mode == 2) {
        printf("build base = FAT32 /mnt — sumber & hasil di volume target\n");
    } else {
        const char* bm = eqi_base_mode();
        if (bm[0] == 'b')
            printf("build base = FAT32 / (EQUINOXBASE) — hasil di DISK\n");
        else if (bm[0] == 'm')
            printf("build base = FAT32 /mnt — artefak disalin ke /mnt/equinox\n");
        else
            printf("build base = RAMFS (tanpa volume FAT32 — hilang saat reboot)\n");
    }

    if (build_mode == 2) {
        eqi_tag("info");
        printf("[wizard] build_mode=2: menyalin layout ISO -> /mnt/equinox/ ...\n");
        int nf = 0, nb = 0;
        eqi_export_layout(fs_find_child(root, "mnt"), &nf, &nb);
        eqi_tag("info");
        printf("layout disalin ke volume: %d file, %u B (ke /mnt)\n",
               nf, (unsigned)nb);
        eqi_on_target = 1;
        eqi_src_root  = "/mnt/equinox";
    } else {
        eqi_on_target = 0;
        eqi_src_root  = "/equinox";
    }

    /* ===== [3/4] DRIVER JARINGAN ===== */
    if (build_mode != 0) {
        printf("\n[3/4] driver jaringan\n");
        struct fs_node* cfgdir = (build_mode == 2) ? fs_find_child(root, "mnt")
                                                   : root;
        if (eqi_pick_nic(cfgdir, tname) != 0) return;
    } else {
        eqi_tag("info");
        printf("nic: pakai driver terdaftar (ne2000) — tanpa system.ecf\n");
    }

    eqi_build_userland(build_mode, tname);

    /* ===== 0.4 Beta — [5/5] BOOTLOADER: boot without CD =====
     * Offered only when the target is a real disk. The volume must use
     * the whole-disk mkfs layout (reserved 2048). */
    if (disk_slot >= 0) {
        printf("\n[5/5] pasang bootloader GRUB ke %s (boot tanpa CD)? [y/N] ",
               tname);
        char ans[16];
        gets(ans, sizeof(ans));
        if (ans[0] == 'y' || ans[0] == 'Y') {
            qfs_install_boot(tname);
        } else {
            printf("[5/5] dilewati — kapan pun bisa: Qfs -install-boot %s\n",
                   tname);
        }
    }
}

/* ============================================================
 *  [4/4] BUILD USERLAND — dipakai wizard DAN `equinoxinstall -compile`
 * ------------------------------------------------------------
 *  Tidak menyentuh disk, tidak memformat, tidak menyalin layout:
 *  sumber harus sudah berada di <dir> (hasil `copy ./* -> /mnt`
 *  misalnya). Hasil .mrp tertulis di samping sumbernya.
 * ============================================================ */
static void eqi_build_userland(int build_mode, const char* tname) {
    struct fs_node* root = fs_get_root();

    /* ===== [4/4] BUILD USERLAND ===== */
    printf("\n[4/4] build userland dari %s\n", eqi_src_root);

    char pbuf[64];
    snprintf(pbuf, sizeof(pbuf), "%s/libc", eqi_src_root);
    struct fs_node* ldir = fs_get_node_from_path(root, pbuf);
    snprintf(pbuf, sizeof(pbuf), "%s/tools", eqi_src_root);
    struct fs_node* tdir = fs_get_node_from_path(root, pbuf);
    snprintf(pbuf, sizeof(pbuf), "%s/games", eqi_src_root);
    struct fs_node* gdir = fs_get_node_from_path(root, pbuf);

    /* mtcc dijalankan dari /equinox/tools (modul ISO: ada di RAMFS
     * maupun di volume base). Kalau di situ tak ada (ISO tipis),
     * pakai salinannya di target. */
    struct fs_node* rdir = fs_get_node_from_path(root, "/equinox/tools");
    struct fs_node* mtcc = rdir ? fs_find_child(rdir, "mtcc.mrp") : NULL;
    if (mtcc && mtcc->is_dir) mtcc = NULL;
    if ((!mtcc || mtcc->size == 0) && tdir) {
        struct fs_node* alt = fs_find_child(tdir, "mtcc.mrp");
        if (alt && !alt->is_dir && alt->size > 0) { mtcc = alt; rdir = tdir; }
    }
    if (!mtcc || mtcc->size == 0) {
        eqi_log("fail", "mtcc.mrp tidak ditemukan — build mustahil");
        eqi_log("info", "kompilator datang prebuilt dari ISO; ini kesalahan ISO");
        return;
    }
    eqi_tag("info");
    printf("kompilator in-OS siap: mtcc.mrp (%u bytes)\n", mtcc->size);

    /* 3. plan the jobs (snapshot BEFORE building — nodes are deleted
     *    while going: libc sources stay, tool/game sources do not). */
    static struct EqJob jobs[EQINSTALL_MAX * 3];
    int total = eqinstall_plan(ldir, tdir, gdir, jobs);
    int ln = 0, tn = 0, gn = 0;
    for (int i = 0; i < total; i++) {
        if (jobs[i].kind == EQJ_LIBC) ln++;
        else if (jobs[i].kind == EQJ_TOOL) tn++;
        else gn++;
    }

    if (total == 0) {
        eqi_log("info", "tidak ada sumber .c — userland sudah terpasang");
        eqi_log("info", "equinoxinstall selesai — 0 program dibangun");
        return;
    }

    eqi_tag("info");
    printf("libc  — %d modul  di %s/libc (check-compile)\n", ln, eqi_src_root);
    eqi_tag("info");
    printf("tools — %d sumber di %s/tools\n", tn, eqi_src_root);
    if (gn > 0) {
        /* 0.4 Beta: fase games hanya dilaporkan bila ada sumbernya —
         * base ISO slim tidak mengirim /equinox/games (games ->
         * paket eggkg). */
        eqi_tag("info");
        printf("games — %d sumber di %s/games\n", gn, eqi_src_root);
    }
    eqi_tag("info");
    printf("parallel pool: %d compiler thread(s), %d job(s) total\n",
           EQI_THREADS, total);

    /* 4. run the pool: keep up to EQI_THREADS mtcc tasks in flight,
     *    wait for each slot in order, verify + log, refill. */
    static struct {
        int pid;             /* 0 = idle slot               */
        int jidx;            /* job index running in slot   */
    } slot[EQI_THREADS];
    for (int s = 0; s < EQI_THREADS; s++) { slot[s].pid = 0; slot[s].jidx = -1; }

    uint32_t t0 = get_tick();
    int next_job = 0;
    int lok = 0, tok = 0, gok = 0, done = 0, degraded = 0;
    uint32_t bytes = 0;

    while (done < total) {
        /* fill idle slots with fresh mtcc tasks */
        for (int s = 0; s < EQI_THREADS && next_job < total; s++) {
            if (slot[s].pid != 0) continue;
            struct EqJob* j = &jobs[next_job];
            char tname[12];
            snprintf(tname, sizeof(tname), "mtcc-T%d", s + 1);
            char args[128];
            if (j->kind == EQJ_LIBC)
                snprintf(args, sizeof(args), "--lib -q -c %s", j->src);
            else
                snprintf(args, sizeof(args), "-q -c %s", j->src);
            struct Task* t = task_create_user(tname, rdir, "mtcc.mrp",
                                              args, mrp_arena_hint_for("mtcc.mrp"));
            if (!t) {
                /* spawn failed (task table full / OOM): degrade to the
                 * synchronous path for THIS job and keep going. */
                if (!degraded) {
                    eqi_log("warn", "parallel spawn failed — single-thread fallback");
                    degraded = 1;
                }
                if (eqinstall_run_sync(rdir, j, next_job + 1, total, &bytes)) {
                    if (j->kind == EQJ_LIBC) lok++;
                    else if (j->kind == EQJ_TOOL) tok++;
                    else gok++;
                }
                next_job++;
                done++;
                continue;
            }
            slot[s].pid = (int)t->pid;
            slot[s].jidx = next_job;
            next_job++;
        }

        /* wait for slot 0, then slot 1 (sequential reaping, parallel
         * execution). A finished child is a zombie: the wait reaps it
         * and frees the task slot immediately. */
        for (int s = 0; s < EQI_THREADS; s++) {
            if (slot[s].pid == 0) continue;
            /* 0.4 Beta live status: gambar baris bawah SEBELUM blok, hapus
             * setelahnya — kembali selalu (baris terakhir, kolom 0),
             * jadi log berikutnya menulis rapi di situ. */
            eqi_status_draw(done, total, ln, lok, tn, tok, gn, gok, t0);
            uint32_t status = 0xdead;
            int r = task_wait_pid(slot[s].pid, &status);
            eqi_status_erase();
            if (r <= 0) {
                eqi_tag("warn");
                printf("wait(T%d, pid %d) returned %d — job result unknown\n",
                       s + 1, slot[s].pid, r);
            }
            int jidx = slot[s].jidx;
            slot[s].pid = 0;
            slot[s].jidx = -1;
            if (eqinstall_collect(&jobs[jidx], jidx + 1, total,
                                  s + 1, &bytes)) {
                if (jobs[jidx].kind == EQJ_LIBC) lok++;
                else if (jobs[jidx].kind == EQJ_TOOL) tok++;
                else gok++;
            }
            done++;
        }
    }

    uint32_t wall_ms = get_tick() - t0;
    int fail = total - lok - tok - gok;

    /* 5. summary */
    eqi_tag("info");
    if (gn > 0)
        printf("summary — libc %d/%d verified, tools %d/%d, games %d/%d, %d failed\n",
               lok, ln, tok, tn, gok, gn, fail);
    else
        printf("summary — libc %d/%d verified, tools %d/%d, %d failed\n",
               lok, ln, tok, tn, fail);
    if (fail == 0 && gn > 0) {
        eqi_tag("info");
        printf("games installed — try: snake | breakout | pong\n");
    }
    eqi_tag("info");
    printf("parallel build: %d job(s) on %d thread(s), wall %u.%u s\n",
           total, EQI_THREADS, wall_ms / 100, (wall_ms / 10) % 10);

    /* 0.4 Beta — hasil instalasi pada disk target */
    if (build_mode == 0) {
        eqi_tag("info");
        printf("build in place — tidak ada disk target yang diubah\n");
    } else {
        eqi_tag("info");
        printf("target: %s — %s\n", tname,
               build_mode == 1 ? "volume base, hasil di /"
                               : "volume dipasang di /mnt, hasil di situ");
        eqi_tag("info");
        printf("nic: %s/equinox/conf/system.ecf + /boot/kernel.elf + layout lengkap\n",
               build_mode == 1 ? "" : tname);
        if (build_mode == 2) {
            eqi_tag("info");
            printf("boot berikutnya dengan disk itu terpasang: EQUINOXBASE -> ROOT (/)\n");
        }
    }
    eqi_tag("info");
    printf("equinoxinstall done — %d program terpasang (%u bytes), %d modul libc terverifikasi\n",
           tok + gok, bytes, lok);
}

/* ------------------------------------------------------------
 *  equinoxinstall -compile <dir> — fase [4/4] SAJA
 *  Tanpa pilih disk, tanpa format, tanpa [3/4] nic, tanpa ekspor
 *  layout: sumber sudah berada di <dir> (mis. lewat `copy ./*`).
 * ------------------------------------------------------------ */
static void eqi_compile_only(struct fs_node* cwd, const char* dirarg) {
    struct fs_node* root = fs_get_root();
    if (!dirarg || !dirarg[0]) {
        printf("equinoxinstall: -compile butuh <dir>\n");
        return;
    }
    char abs[300];
    shell_resolve_path(cwd, dirarg, abs, sizeof(abs));

    struct fs_node* d = fs_get_node_from_path(root, abs);
    if (!d || !d->is_dir) {
        printf("equinoxinstall: bukan direktori: %s\n", abs);
        return;
    }
    char p[320];
    snprintf(p, sizeof(p), "%s/tools", abs);
    struct fs_node* td = fs_get_node_from_path(root, p);
    snprintf(p, sizeof(p), "%s/libc", abs);
    struct fs_node* ld = fs_get_node_from_path(root, p);
    if (!td || !ld) {
        printf("equinoxinstall: %s harus punya libc/ dan tools/\n", abs);
        return;
    }

    /* sumber di volume FAT32 -> .c dipertahankan (isi install);
     * sumber di RAMFS -> jalur build in-place lama (sumber dibuang). */
    eqi_on_target = (d->backing == 1) ? 1 : 0;
    shell_copy(eqi_src_buf, sizeof(eqi_src_buf), abs);
    eqi_src_root = eqi_src_buf;

    printf("[compile] sumber: %s (%s)\n", abs,
           eqi_on_target ? "volume — sumber dipertahankan"
                         : "RAMFS — sumber dipakai sekali");
    eqi_build_userland(0, "(compile)");
}

/* ------------------------------------------------------------
 *  equinoxinstall -build (0.4 Beta)
 *  Three argument forms, none of which runs the wizard or formats
 *  the disk:
 *    -build <file.ruf>   build via `mtcc -make <file>` — old path
 *                        (the .ruf points at its own sources)
 *    -build <name>       build ONE tool from /equinox/tools:
 *                        spawn `mtcc -c /equinox/tools/<name>.c`
 *                        (any name present in tools — "just parse
 *                        it"; building mtcc itself is the self-host
 *                        status message, see eqi_build_tool)
 *    -build <glob*>      single-star glob — e.g. `*.ruf` (default
 *                        dir /equinox) or `/equinox/*.ruf` —
 *                        build every matching .ruf
 *  All three are thin wrappers over mtcc.mrp: find mtcc, spawn one
 *  task, wait, report the exit code.
 * ------------------------------------------------------------ */

/* cari node mtcc.mrp: coba 'prefer' dulu, lalu /equinox/tools, lalu
 * 'alt' (semua boleh NULL/duplikat — di-dedup). Node yang valid:
 * file, size > 0. *out_dir = dir tempat mtcc.mrp ditemukan (dipakai
 * sebagai cwd task spawn). */
static struct fs_node* eqi_find_mtcc(struct fs_node* prefer,
                                     struct fs_node* alt,
                                     struct fs_node** out_dir) {
    struct fs_node* tools =
        fs_get_node_from_path(fs_get_root(), "/equinox/tools");
    struct fs_node* order[3];
    int n = 0;
    if (prefer) order[n++] = prefer;
    if (tools && tools != prefer) order[n++] = tools;
    if (alt && alt != prefer && alt != tools) order[n++] = alt;
    for (int i = 0; i < n; i++) {
        struct fs_node* m = fs_find_child(order[i], "mtcc.mrp");
        if (m && !m->is_dir && m->size > 0) { *out_dir = order[i]; return m; }
    }
    return NULL;
}

/* spawn mtcc.mrp (cwd = mdir) dengan argumen margs; return exit code
 * mtcc (0 ok / >0 gagal) atau -1 saat spawn/wait gagal (pesan sudah
 * dicetak di sini). */
static int eqi_spawn_mtcc(struct fs_node* mdir, const char* margs,
                          const char* tname) {
    struct Task* t = task_create_user(tname, mdir, "mtcc.mrp",
                                      margs, mrp_arena_hint_for("mtcc.mrp"));
    if (!t) {
        eqi_tag("fail");
        printf("spawn mtcc gagal (task table penuh / OOM)\n");
        return -1;
    }
    uint32_t status = 0xdead;
    int r = task_wait_pid(t->pid, &status);
    if (r <= 0) {
        eqi_tag("warn");
        printf("wait(pid %d) = %d — hasil build tidak diketahui\n", t->pid, r);
        return -1;
    }
    return (int)(status & 0xff);
}

/* -build <file.ruf> — path lengkap/relatif ke satu file .ruf */
static void eqi_build_ruf_path(struct fs_node* cwd, const char* rufarg) {
    char abs[300];
    shell_resolve_path(cwd, rufarg, abs, sizeof(abs));

    struct fs_node* root = fs_get_root();
    struct fs_node* rf = fs_get_node_from_path(root, abs);
    if (!rf || rf->is_dir) {
        printf("equinoxinstall: file .ruf tidak ditemukan: %s\n", abs);
        return;
    }

    /* mtcc.mrp: /equinox/tools dulu (modul ISO/RAMFS), lalu di samping
     * .ruf (untuk volume target yang sudah berisi kompilator). */
    struct fs_node* tools = fs_get_node_from_path(root, "/equinox/tools");
    struct fs_node* mdir = NULL;
    struct fs_node* mtcc = eqi_find_mtcc(tools, rf->parent, &mdir);
    if (!mtcc) {
        printf("equinoxinstall: mtcc.mrp tidak ditemukan — build mustahil\n");
        return;
    }

    char margs[160];
    snprintf(margs, sizeof(margs), "-make %s", abs);
    eqi_tag("info");
    printf("[build] mtcc %s (mtcc.mrp %u bytes)\n", margs,
           (unsigned)mtcc->size);
    int rc = eqi_spawn_mtcc(mdir, margs, "mtcc-make");
    if (rc == 0) {
        eqi_tag("info");
        printf("[build] selesai — semua job ok\n");
    } else if (rc > 0) {
        eqi_tag("fail");
        printf("[build] mtcc exit %u — ada job gagal\n", (unsigned)rc);
    }
}

/* 0.4 Beta — hook untuk eggkg (kernel/eggkg.cpp): build satu .ruf via
 * spawn mtcc.mrp. Return 0 ok / >0 exit mtcc / -1 gagal. */
int shell_eggkg_build_ruf(const char* abspath) {
    struct fs_node* root = fs_get_root();
    struct fs_node* rf = fs_get_node_from_path(root, abspath);
    if (!rf || rf->is_dir) {
        printf("eggkg: build.ruf tidak ditemukan: %s\n", abspath);
        return -1;
    }
    struct fs_node* tools = fs_get_node_from_path(root, "/equinox/tools");
    struct fs_node* mdir = NULL;
    struct fs_node* mtcc = eqi_find_mtcc(tools, rf->parent, &mdir);
    if (!mtcc) {
        printf("eggkg: mtcc.mrp tidak ditemukan — build mustahil\n");
        return -1;
    }
    char margs[160];
    snprintf(margs, sizeof(margs), "-make %s", abspath);
    return eqi_spawn_mtcc(mdir, margs, "mtcc-make");
}

/* -build <nama> — build satu tool dari /equinox/tools via `mtcc -c`.
 * Nama apa pun yang punya <nama>.c di tools diterima ("parse aja");
 * kegagalan mencari sumber = daftar .c yang tersedia + catatan builtin.
 * "mtcc" mendapat perlakuan khusus: status self-host yang jujur. */
static void eqi_build_tool(struct fs_node* cwd, const char* rawname) {
    (void)cwd;
    char nm[64];
    uint32_t i = 0;
    while (rawname[i] && i < sizeof(nm) - 1) { nm[i] = rawname[i]; i++; }
    nm[i] = '\0';
    uint32_t l = i;
    if (l >= 2 && nm[l - 2] == '.' && nm[l - 1] == 'c') nm[l - 2] = '\0';

    if (strcmp(nm, "mtcc") == 0) {
        eqi_tag("info");
        printf("[build] mtcc: self-compile belum didukung — mtcc.c memakai\n");
        printf("        struct/typedef/sizeof/unsigned yang memang di luar\n");
        printf("        subset bahasa mtcc (lihat mtcc.c:86-88).\n");
        printf("        mtcc.mrp tetap di-pack dari host: make mtcc.\n");
        printf("        Userland penuh (libc+tools+games) tetap bisa dibangun\n");
        printf("        in-OS: equinoxinstall -build *.ruf\n");
        return;
    }

    struct fs_node* root = fs_get_root();
    struct fs_node* tools = fs_get_node_from_path(root, "/equinox/tools");
    if (!tools || !tools->is_dir) {
        printf("equinoxinstall: /equinox/tools tidak ada\n");
        return;
    }
    char cname[68];
    snprintf(cname, sizeof(cname), "%s.c", nm);
    struct fs_node* src = fs_find_child(tools, cname);
    if (!src || src->is_dir) {
        printf("equinoxinstall: tidak ada sumber '%s' di /equinox/tools\n",
               cname);
        int n = 0;
        for (struct fs_node* c = tools->children; c; c = c->next) {
            uint32_t cl = (uint32_t)strlen(c->name);
            if (!c->is_dir && cl >= 2 && c->name[cl - 2] == '.' &&
                c->name[cl - 1] == 'c') {
                printf("  - %s\n", c->name);
                n++;
            }
        }
        if (!n) printf("  (tidak ada .c di tools)\n");
        printf("catatan: copy/del/mount/set/Qfs adalah builtin shell — tanpa sumber\n");
        return;
    }

    struct fs_node* mdir = NULL;
    struct fs_node* mtcc = eqi_find_mtcc(tools, NULL, &mdir);
    if (!mtcc) {
        printf("equinoxinstall: mtcc.mrp tidak ditemukan — build mustahil\n");
        return;
    }
    char abs[300];
    snprintf(abs, sizeof(abs), "/equinox/tools/%s.c", nm);
    char margs[180];
    snprintf(margs, sizeof(margs), "-c %s", abs);
    eqi_tag("info");
    printf("[build] mtcc %s (mtcc.mrp %u bytes)\n", margs,
           (unsigned)mtcc->size);
    int rc = eqi_spawn_mtcc(mdir, margs, "mtcc-c");
    if (rc == 0) {
        eqi_tag("info");
        printf("[build] %s.c -> %s.mrp — ok\n", nm, nm);
    } else if (rc > 0) {
        eqi_tag("fail");
        printf("[build] mtcc exit %u — %s gagal\n", (unsigned)rc, cname);
    }
}

/* -build <pola*> — glob SATU bintang. "*.ruf" tanpa '/' -> dir
 * /equinox (rumah .ruf bawaan RAMFS); "/equinox/*.ruf" atau
 * "sub/*.ruf" -> dir itu. Hanya file berakhiran .ruf yang dibangun;
 * match lain dihitung dilewati. */
static void eqi_build_glob(struct fs_node* cwd, const char* pat) {
    char dirarg[280];
    const char* base = pat;
    uint32_t dl = 0;
    for (const char* q = pat; *q; q++)
        if (*q == '/') { base = q + 1; dl = (uint32_t)(q + 1 - pat); }
    if (dl > 0) {
        if (dl >= sizeof(dirarg)) dl = sizeof(dirarg) - 1;
        for (uint32_t i = 0; i < dl; i++) dirarg[i] = pat[i];
        dirarg[dl] = '\0';
    } else {
        snprintf(dirarg, sizeof(dirarg), "/equinox");
    }
    char dirabs[300];
    shell_resolve_path(cwd, dirarg, dirabs, sizeof(dirabs));
    struct fs_node* d = fs_get_node_from_path(fs_get_root(), dirabs);
    if (!d || !d->is_dir) {
        printf("equinoxinstall: direktori tidak ada: %s\n", dirabs);
        return;
    }

    const char* star = strchr(base, '*');
    if (!star) {                          /* tak mungkin — sudah difilter */
        eqi_build_ruf_path(cwd, pat);
        return;
    }
    uint32_t plen = (uint32_t)(star - base);
    const char* suffix = star + 1;
    uint32_t slen = (uint32_t)strlen(suffix);
    if (strchr(suffix, '*')) {
        printf("equinoxinstall: glob hanya mendukung satu '*': %s\n", pat);
        return;
    }

    int built = 0, skip = 0;
    for (struct fs_node* c = d->children; c; c = c->next) {
        if (c->is_dir) continue;
        uint32_t nl = (uint32_t)strlen(c->name);
        if (nl < plen + slen) continue;
        if (plen && strncmp(c->name, base, plen) != 0) continue;
        if (slen && strcmp(c->name + nl - slen, suffix) != 0) continue;
        uint32_t rl = nl;
        if (rl < 4 || strcmp(c->name + rl - 4, ".ruf") != 0) { skip++; continue; }
        char childabs[340];
        snprintf(childabs, sizeof(childabs), "%s/%s", dirabs, c->name);
        eqi_build_ruf_path(cwd, childabs);
        built++;
    }
    printf("[build] glob '%s': %d .ruf dibangun, %d file lain dilewati\n",
           pat, built, skip);
    if (!built)
        printf("[build] tidak ada .ruf yang cocok di %s\n", dirabs);
}

/* dispatcher -build: pola glob / nama tool / path .ruf */
static void eqi_build_ruf(struct fs_node* cwd, const char* rufarg) {
    if (!rufarg || !rufarg[0]) {
        printf("equinoxinstall: -build butuh <file.ruf | nama_tool | pola*>\n");
        printf("  equinoxinstall -build /equinox/eq.ruf   build satu file .ruf\n");
        printf("  equinoxinstall -build ls                build satu tool dari /equinox/tools\n");
        printf("  equinoxinstall -build *.ruf             build semua .ruf di /equinox\n");
        return;
    }
    if (strchr(rufarg, '*')) {
        eqi_build_glob(cwd, rufarg);
        return;
    }
    if (!strchr(rufarg, '/')) {
        uint32_t l = (uint32_t)strlen(rufarg);
        int is_ruf = (l >= 4 && strcmp(rufarg + l - 4, ".ruf") == 0);
        if (!is_ruf) {                    /* "ls" / "ls.c" -> nama tool */
            eqi_build_tool(cwd, rufarg);
            return;
        }
    }
    eqi_build_ruf_path(cwd, rufarg);
}


// ============================================================
//  SHELL PATH RESOLUTION (0.4 Beta: editor round-trip, fix K4)
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
//  0.4 Beta — BUILTIN `set` (.ecf) + `eqgu` (editor + cek kompilasi)
// ------------------------------------------------------------
//  Letak: sesudah shell_resolve_path() (dipakai untuk argumen file)
//  dan sebelum shell_entry().
// ============================================================

static struct ecf_store ecf_tmp_store;   /* `set -a` — statis ~12,5 KB */

/* didefinisikan di blok 0.4 Beta penjalankan .es (di bawah). */
static void es_start(const char* path);

/* Ambil direktori induk dari path absolut ("/a/b" -> "/a"). */
static void shell_dir_of(const char* path, char* out, uint32_t outsz) {
    out[0] = '/';
    out[1] = '\0';
    if (!path || outsz < 2) return;
    uint32_t last = 0;
    for (uint32_t i = 0; path[i]; i++) if (path[i] == '/') last = i;
    if (last == 0) return;
    uint32_t n = (last < outsz - 1) ? last : outsz - 1;
    for (uint32_t i = 0; i < n; i++) out[i] = path[i];
    out[n] = '\0';
}

/* Salin terbatas (snprintf kernel tanpa precision). */
static void shell_copy(char* dst, uint32_t dstsz, const char* src) {
    uint32_t i = 0;
    if (dstsz == 0) return;
    while (src && src[i] && i < dstsz - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void ecf_copy_msg(char* dst, int dstsz, const char* src) {
    int i = 0;
    if (dstsz < 1) return;
    while (src && src[i] && i < dstsz - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* `set` tanpa argumen: daftar seluruh entri store aktif. */
static void ecf_cmd_list(void) {
    struct ecf_store* st = ecf_store();
    if (st->count == 0) {
        printf("ecf: (kosong) — belum ada entri\n");
        return;
    }
    for (int i = 0; i < ECF_MAX_ENTRIES; i++)
        if (st->ent[i].used)
            printf("%s = %s\n", st->ent[i].key, st->ent[i].val);
}

/* `set KEY` — tampilkan nilai. */
static void ecf_cmd_show(const char* key) {
    const char* v = ecf_get(ecf_store(), key);
    if (v) printf("%s = %s\n", key, v);
    else   printf("ecf: tidak ada kunci '%s'\n", key);
}

/* Validasi skema lalu PATCH berkas .ecf aktif (in-place) + store. */
static int ecf_cmd_put(const char* key, const char* val) {
    int c = ecf_check(key, val);
    if (c == 1) {
        printf("ecf: kunci tak dikenal: %s\n", key);
        return -1;
    }
    if (c == 2) {
        const char* vs = ecf_key_values(key);
        printf("ecf: nilai '%s' tidak valid — pilihan: %s\n",
               val, vs ? vs : "(bebas)");
        return -1;
    }
    const char* path = ecf_active_path(1);
    if (!path) {
        printf("ecf: tidak bisa menulis system.ecf\n");
        return -1;
    }
    if (ecf_set_file(path, key, val) != 0) {
        printf("ecf: gagal menulis %s\n", path);
        return -1;
    }
    ecf_put(ecf_store(), key, val);
    printf("ecf: %s = %s -> %s\n", key, val, path);
    if (ecf_needs_reboot(key))
        printf("ecf: PERLU REBOOT — net.driver hanya dibaca saat boot\n");
    return 0;
}

/* `set -a FILE` — parse FILE, terapkan tiap entri valid. */
static void ecf_cmd_apply(struct fs_node* cwd, const char* arg) {
    if (!arg[0]) { printf("ecf: set -a butuh nama file\n"); return; }
    char path[300];
    shell_resolve_path(cwd, arg, path, sizeof(path));
    if (ecf_load(path, &ecf_tmp_store) != 0) {
        printf("ecf: gagal membaca %s\n", path);
        return;
    }
    int n = 0;
    const char* ap = ecf_active_path(1);
    if (ap) {
        for (int i = 0; i < ECF_MAX_ENTRIES; i++) {
            struct ecf_entry* e = &ecf_tmp_store.ent[i];
            if (!e->used) continue;
            if (ecf_set_file(ap, e->key, e->val) == 0) {
                ecf_put(ecf_store(), e->key, e->val);
                n++;
            }
        }
    }
    printf("ecf: %d diterapkan, %d tak dikenal, %d error\n",
           n, ecf_tmp_store.warn, ecf_tmp_store.err);
    /* Baris TAMBAHAN saja — baris di atas harus tetap persis seperti
     * dulu (suite ecf_test memeriksanya sebagai substring). */
    if (ecf_tmp_store.none)
        printf("ecf: %d key masih NONE (belum diisi)\n",
               ecf_tmp_store.none);
}

/* `set -w FILE` — tulis SELURUH store aktif ke FILE. */
static void ecf_cmd_write(struct fs_node* cwd, const char* arg) {
    if (!arg[0]) { printf("ecf: set -w butuh nama file\n"); return; }
    char path[300];
    shell_resolve_path(cwd, arg, path, sizeof(path));
    struct ecf_store* st = ecf_store();
    if (ecf_write_store(path, st) != 0) {
        printf("ecf: gagal menulis %s\n", path);
        return;
    }
    printf("ecf: ditulis %s (%d entri)\n", path, st->count);
}

/* ============================================================
 *  0.4 Beta — `set -d` / `set -b` / `set -x`
 * ------------------------------------------------------------
 *  set -d FILE [-path DIR] [-base SRC]
 *      Tunjuk FILE sebagai berkas konf UTAMA. `set KEY V`,
 *      `set -a` dan `set -w` semuanya menulis ke sana.
 *      FILE yang belum ada DIBUAT: dari -base SRC (semua key
 *      salinan, semua value = NONE supaya user mengisi sendiri);
 *      base otomatis diambil dari <dir>/<nama>.base.ecf, lalu
 *      fallback ke berkas .ecf aktif (yang ditulis equinoxinstall).
 *      -path DIR = direktori khusus; tanpa itu direktorinya
 *      mengikuti berkas .ecf aktif (tempat conf dibaca).
 *      Pointer `active.conf = <path>` ditulis ke berkas .ecf
 *      default supaya init saat boot ikut membaca FILE itu.
 *
 *  set -b PATH
 *      Rekam pivot base (key bebas `base.path`). TIDAK di-promote
 *      saat itu juga — diterapkan oleh fat32_boot_init() saat
 *      reboot berikutnya.
 *
 *  set -x FILE
 *      Jalankan skrip eqshell (.es).
 * ============================================================ */
static void ecf_cmd_setdir(struct fs_node* cwd, const char* rest) {
    char file[ECF_PATH_MAX];
    char pdir[ECF_PATH_MAX];
    char base[ECF_PATH_MAX];
    file[0] = pdir[0] = base[0] = '\0';
    int have_path = 0;

    const char* p = rest;
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;
        char tok[ECF_PATH_MAX];
        uint32_t n = 0;
        while (*p && *p != ' ' && n < sizeof(tok) - 1) tok[n++] = *p++;
        tok[n] = '\0';
        if (strcmp(tok, "-path") == 0) {
            while (*p == ' ') p++;
            n = 0;
            while (*p && *p != ' ' && n < sizeof(pdir) - 1) pdir[n++] = *p++;
            pdir[n] = '\0';
            have_path = pdir[0] != '\0';
        } else if (strcmp(tok, "-base") == 0) {
            while (*p == ' ') p++;
            n = 0;
            while (*p && *p != ' ' && n < sizeof(base) - 1) base[n++] = *p++;
            base[n] = '\0';
        } else if (!file[0]) {
            shell_copy(file, sizeof(file), tok);
        }
    }

    /* --- tanpa FILE: kembali ke perilaku lama --- */
    if (!file[0]) {
        ecf_target_clear();
        const char* dp = ecf_active_path(1);
        if (dp) ecf_set_file(dp, ECF_ACTIVE_KEY, "system.ecf");
        printf("set: target kembali ke system.ecf\n");
        return;
    }

    /* --- berkas konf default (SEBELUM override diaktifkan) --- */
    ecf_target_clear();
    const char* dp = ecf_active_path(1);
    char defdir[ECF_PATH_MAX];
    defdir[0] = '\0';
    if (dp) shell_dir_of(dp, defdir, sizeof(defdir));

    /* --- tentukan path target --- */
    char target[300];
    if (strchr(file, '/')) {
        shell_resolve_path(cwd, file, target, sizeof(target));
    } else if (have_path) {
        char dir[300];
        shell_resolve_path(cwd, pdir, dir, sizeof(dir));
        uint32_t l = (uint32_t)strlen(dir);
        while (l > 0 && dir[l - 1] == '/') { dir[--l] = '\0'; }
        if (l + 1 + (uint32_t)strlen(file) >= sizeof(target)) {
            printf("set: path terlalu panjang\n");
            return;
        }
        for (uint32_t i = 0; i < l; i++) target[i] = dir[i];
        target[l] = '/';
        shell_copy(target + l + 1, sizeof(target) - l - 1, file);
    } else {
        uint32_t l = (uint32_t)strlen(defdir);
        if (l + 1 + (uint32_t)strlen(file) >= sizeof(target)) {
            printf("set: path terlalu panjang\n");
            return;
        }
        for (uint32_t i = 0; i < l; i++) target[i] = defdir[i];
        target[l] = '/';
        shell_copy(target + l + 1, sizeof(target) - l - 1, file);
    }

    struct fs_node* exists = fs_get_node_from_path(fs_get_root(), target);

    /* --- berkas belum ada: kloning dari base (value = NONE) --- */
    if (!exists) {
        char bpath[300];
        bpath[0] = '\0';
        if (base[0]) {
            if (base[0] == '/') shell_copy(bpath, sizeof(bpath), base);
            else shell_resolve_path(cwd, base, bpath, sizeof(bpath));
        } else {
            /* auto: <dir-target>/<nama-tanpa-ekstensi>.base.ecf */
            char tdir[ECF_PATH_MAX];
            shell_dir_of(target, tdir, sizeof(tdir));
            char stem[ECF_PATH_MAX];
            shell_copy(stem, sizeof(stem), file);
            uint32_t sl = (uint32_t)strlen(stem);
            uint32_t dot = 0;
            for (uint32_t i = 0; i < sl; i++) if (stem[i] == '.') dot = i;
            if (dot) stem[dot] = '\0';
            uint32_t tl = (uint32_t)strlen(tdir);
            char cand[ECF_PATH_MAX];
            uint32_t k = 0;
            for (; tdir[k] && k < sizeof(cand) - 1; k++) cand[k] = tdir[k];
            if (k && cand[k - 1] != '/' && k < sizeof(cand) - 1) cand[k++] = '/';
            for (uint32_t i = 0; stem[i] && k < sizeof(cand) - 8; i++)
                cand[k++] = stem[i];
            const char* suf = ".base.ecf";
            for (uint32_t i = 0; suf[i] && k < sizeof(cand) - 1; i++)
                cand[k++] = suf[i];
            cand[k] = '\0';
            (void)tl;
            struct fs_node* c = fs_get_node_from_path(fs_get_root(), cand);
            if (c && !c->is_dir) shell_copy(bpath, sizeof(bpath), cand);
            else if (dp)        shell_copy(bpath, sizeof(bpath), dp);
        }

        if (bpath[0] &&
            strcmp(bpath, target) != 0 &&
            ecf_load(bpath, &ecf_tmp_store) == 0) {
            int nk = 0;
            for (int i = 0; i < ECF_MAX_ENTRIES; i++) {
                struct ecf_entry* e = &ecf_tmp_store.ent[i];
                if (!e->used) continue;
                if (ecf_set_file(target, e->key, "NONE") == 0) nk++;
            }
            if (nk)
                printf("set: %s dibuat dari %s — %d key = NONE\n",
                       target, bpath, nk);
        } else {
            printf("set: base tidak ditemukan: %s — lanjut tanpa key\n",
                   bpath[0] ? bpath : "(auto)");
            printf("set: %s dibuat kosong (isi dengan `set KEY VALUE`)\n",
                   target);
        }
    }

    /* --- pointer boot: tulis ke berkas .ecf DEFAULT ---
     * Nilai pointer = nama berkas saja bila target masih se-dir dengan
     * konf default (paling portabel: ikut dimana pun volume dipasang);
     * path absolut bila -path menaruhnya di tempat lain. */
    if (dp) {
        char tdir[ECF_PATH_MAX];
        shell_dir_of(target, tdir, sizeof(tdir));
        char ptr[ECF_PATH_MAX];
        if (defdir[0] && strcmp(tdir, defdir) == 0) {
            const char* nm = target;
            for (const char* s = target; *s; s++) if (*s == '/') nm = s + 1;
            shell_copy(ptr, sizeof(ptr), nm);
        } else {
            shell_copy(ptr, sizeof(ptr), target);
        }
        if (ecf_set_file(dp, ECF_ACTIVE_KEY, ptr) != 0)
            printf("set: gagal menulis pointer %s\n", dp);
    }

    if (ecf_target_set(target) != 0) {
        printf("set: gagal menunjuk %s\n", target);
        return;
    }
    printf("set: target = %s\n", target);
}

/* `set -b PATH` — rekam pivot base (diterapkan saat reboot). */
static void ecf_cmd_basepath(struct fs_node* cwd, const char* arg) {
    if (!arg[0]) {
        const char* v = ecf_get(ecf_store(), "base.path");
        printf("ecf: base.path = %s\n", v ? v : "(belum di-set)");
        return;
    }
    char path[300];
    shell_resolve_path(cwd, arg, path, sizeof(path));
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), path);
    if (!n || !n->is_dir) {
        printf("ecf: bukan direktori: %s\n", path);
        return;
    }
    const char* ap = ecf_active_path(1);
    if (!ap) { printf("ecf: tidak bisa menulis system.ecf\n"); return; }
    if (ecf_set_file(ap, "base.path", path) != 0) {
        printf("ecf: gagal menulis %s\n", ap);
        return;
    }
    ecf_put(ecf_store(), "base.path", path);
    printf("ecf: base.path = %s -> %s (diterapkan saat reboot)\n",
           path, ap);
}

/* `set -x FILE` — jalankan skrip eqshell (.es). */
static void ecf_cmd_run(struct fs_node* cwd, const char* arg) {
    if (!arg[0]) { printf("set: set -x butuh nama file\n"); return; }
    char path[300];
    shell_resolve_path(cwd, arg, path, sizeof(path));
    es_start(path);
}

static void ecf_cmd_usage(void) {
    printf("set: builtin .ecf\n");
    printf("  set              daftar entri aktif\n");
    printf("  set KEY          tampilkan nilai KEY\n");
    printf("  set KEY VALUE    ubah nilai (validasi skema + tulis)\n");
    printf("  set -a FILE      terapkan seluruh FILE\n");
    printf("  set -w FILE      tulis store aktif ke FILE\n");
    printf("  set -d FILE [-path DIR] [-base SRC]\n");
    printf("                   jadikan FILE konf utama (nilai baru = NONE)\n");
    printf("  set -b PATH      rekam pivot base.path (efektif saat reboot)\n");
    printf("  set -x FILE      jalankan skrip eqshell (.es)\n");
}

/* Pemanggil builtin `set` — `args` = sisa baris setelah kata "set". */
static void shell_cmd_set(struct fs_node* cwd, const char* args) {
    while (*args == ' ') args++;

    if (!args[0]) { ecf_cmd_list(); return; }

    if (args[0] == '-') {
        if ((args[1] == 'a' && (args[2] == ' ' || args[2] == '\0'))) {
            const char* p = args + 2;
            while (*p == ' ') p++;
            ecf_cmd_apply(cwd, p);
            return;
        }
        if ((args[1] == 'w' && (args[2] == ' ' || args[2] == '\0'))) {
            const char* p = args + 2;
            while (*p == ' ') p++;
            ecf_cmd_write(cwd, p);
            return;
        }
        if (args[1] == 'd' && (args[2] == ' ' || args[2] == '\0')) {
            const char* p = args + 2;
            while (*p == ' ') p++;
            ecf_cmd_setdir(cwd, p);
            return;
        }
        if (args[1] == 'b' && (args[2] == ' ' || args[2] == '\0')) {
            const char* p = args + 2;
            while (*p == ' ') p++;
            ecf_cmd_basepath(cwd, p);
            return;
        }
        if (args[1] == 'x' && (args[2] == ' ' || args[2] == '\0')) {
            const char* p = args + 2;
            while (*p == ' ') p++;
            ecf_cmd_run(cwd, p);
            return;
        }
        ecf_cmd_usage();
        return;
    }

    /* KEY [VALUE] — potong token pertama. */
    char key[ECF_KEY_MAX];
    uint32_t i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(key) - 1) {
        key[i] = args[i];
        i++;
    }
    key[i] = '\0';
    const char* rest = args + i;
    while (*rest == ' ') rest++;

    if (!rest[0]) { ecf_cmd_show(key); return; }
    ecf_cmd_put(key, rest);
}

/* ============================================================
 *  0.4 Beta — penjalankan skrip eqshell (`.es`)  lewat `set -x FILE`
 * ------------------------------------------------------------
 *  Desain: TIDAK menyentuh rantai dispatch. `set -x` menumpuk
 *  berkas ke stack skrip; shell_entry() lalu mengambil baris
 *  berikutnya dari stack (bukan dari keyboard) sampai habis.
 *
 *  Format berkas:
 *      [Eqshell]      <- baris pertama berarti, WAJIB
 *      Log=True       <- directive  Key = Value
 *      # komentar     <- dilewati
 *      <perintah>     <- dijalankan oleh dispatch biasa
 *
 *  Directive yang dikenal: Log (True/False). Directive lain tetap
 *  dikonsumsi + peringatan. `Key = Value` dikenali dari: berisi '='
 *  dan bagian sebelum '=' tanpa spasi + huruf/angka/underscore
 *  semua — jadi perintah shell (yang memakai spasi) tak tertelan.
 * ============================================================ */
#define ES_MAX_DEPTH   3        /* `es: terlalu dalam` pada depth 4   */
#define ES_FILE_MAX    4096     /* isi .es maksimum                   */
#define ES_MAX_LINES   512      /* baris perintah per berkas          */
#define ES_LINE_MAX    127      /* = sizeof(shell input) - 1          */
#define ES_CAP_MAX     2048     /* jendela capture output per baris   */
#define ES_LOG_MAX     8192     /* transkrip /eqshell.log             */

struct es_frame {
    char     buf[ES_FILE_MAX + 1];
    uint32_t len;
    uint32_t pos;               /* offset baris berikutnya            */
    int      lines;             /* baris perintah dieksekusi         */
    int      errs;              /* kesalahan format/berkas           */
    int      log;               /* Log=True                          */
};

static struct es_frame es_stack[ES_MAX_DEPTH];
static int      es_depth;                    /* 0 = interaktif        */
static int      es_log_any;                  /* ada Log=True          */
static char     es_cap[ES_CAP_MAX];          /* jendela capture        */
static int      es_cap_on;
static char     es_log[ES_LOG_MAX];          /* transkrip RAM          */
static uint32_t es_loglen;
static int      es_log_ovf;

static int es_ncmpi(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

static void es_log_append(const char* s, uint32_t n) {
    if (!es_log_any || !s || n == 0) return;
    if (es_loglen + n >= ES_LOG_MAX) {
        es_log_ovf = 1;
        n = ES_LOG_MAX - 1 - es_loglen;
        if ((int)n <= 0) return;
    }
    for (uint32_t i = 0; i < n; i++) es_log[es_loglen + i] = s[i];
    es_loglen += n;
}

static void es_log_puts(const char* s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    es_log_append(s, n);
}

static void es_log_flush(void) {
    if (!es_log_any) return;
    struct fs_node* root = fs_get_root();
    if (root)
        fs_write_binary(root, "eqshell.log",
                        (const uint8_t*)es_log, es_loglen);
    if (es_log_ovf)
        printf("es: transkrip terpotong (maks %d B) -> /eqshell.log\n",
               ES_LOG_MAX);
    es_loglen = 0;
    es_log_ovf = 0;
    es_log_any = 0;
}

/* Tutup capture baris skrip: simpan ke transkrip + tampilkan ulang
 * di layar. Dipanggil di ATAS loop shell_entry, SEBELUM prompt —
 * jadi prompt tidak ikut terekam. */
static void es_flush_line(void) {
    if (!es_cap_on) return;
    es_cap_on = 0;
    term_capture_end();
    es_cap[ES_CAP_MAX - 1] = '\0';
    es_log_puts(es_cap);
    term_console_puts(es_cap);      /* layar saja — serial sudah dapat */
}

static void es_pop(void) {
    struct es_frame* f = &es_stack[es_depth - 1];
    printf("es: selesai (%d baris, %d error)\n", f->lines, f->errs);
    es_depth--;
    if (es_depth == 0) es_log_flush();
}

/* Ambil baris berikutnya dari stack skrip. 1 = ada baris (diisi ke
 * `out`), 0 = tak ada (kembali ke keyboard). */
static int es_next_line(char* out, int outsz) {
    while (es_depth > 0) {
        struct es_frame* f = &es_stack[es_depth - 1];

        /* sampai akhir berkas -> pop */
        if (f->pos >= f->len) { es_pop(); continue; }

        /* ---- ambil satu baris mentah ---- */
        uint32_t ls = f->pos;
        while (f->pos < f->len && f->buf[f->pos] != '\n') f->pos++;
        uint32_t ll = f->pos - ls;
        if (f->pos < f->len) f->pos++;

        char line[ES_LINE_MAX + 8];
        uint32_t n = ll < sizeof(line) - 1 ? ll : (uint32_t)(sizeof(line) - 1);
        for (uint32_t k = 0; k < n; k++) line[k] = f->buf[ls + k];
        line[n] = '\0';
        if (n < ll) { line[n] = '\0'; f->errs++; }   /* terlalu panjang */

        /* ---- potong spasi, lewati kosong + komentar ---- */
        uint32_t a = 0;
        while (line[a] == ' ' || line[a] == '\t' || line[a] == '\r') a++;
        uint32_t b = (uint32_t)strlen(line + a);
        while (b > 0 && (line[a + b - 1] == ' ' || line[a + b - 1] == '\t' ||
                         line[a + b - 1] == '\r')) b--;
        if (b == 0) continue;
        if (line[a] == '#') continue;
        for (uint32_t k = 0; k < b; k++) line[k] = line[a + k];
        line[b] = '\0';

        if (f->lines >= ES_MAX_LINES) {
            printf("es: terlalu banyak baris (maks %d)\n", ES_MAX_LINES);
            f->errs++;
            es_pop();
            continue;
        }

        f->lines++;

        /* ---- transkrip + capture output baris ini ---- */
        if (f->log) {
            es_log_puts("> ");
            es_log_puts(line);
            es_log_puts("\n");
            es_cap[0] = '\0';
            es_cap_on = 1;
            term_capture_begin(es_cap, sizeof(es_cap));
        }

        /* ---- serahkan ke loop shell (potong ke buffer input) ---- */
        uint32_t m = b;
        if (m > (uint32_t)outsz - 1) m = (uint32_t)outsz - 1;
        for (uint32_t k = 0; k < m; k++) out[k] = line[k];
        out[m] = '\0';
        if (m < b) { f->errs++; printf("es: baris terpotong\n"); }
        return 1;
    }
    return 0;
}

/* Baca seluruh berkas ke dst. 0 = ok, <0 = gagal (pesan dicetak). */
static int es_read_all(const char* path, char* dst, uint32_t cap,
                       uint32_t* outlen) {
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), path);
    if (!n || n->is_dir) {
        printf("es: tidak ada: %s\n", path);
        return -1;
    }
    if (fs_ensure_content(n) != 0 || (n->size > 0 && !n->content)) {
        printf("es: gagal membaca %s\n", path);
        return -2;
    }
    if (n->size > cap) {
        printf("es: terlalu besar %s (%u B, maks %u B)\n",
               path, (unsigned)n->size, (unsigned)cap);
        return -3;
    }
    for (uint32_t i = 0; i < n->size; i++) dst[i] = n->content[i];
    dst[n->size] = '\0';
    *outlen = n->size;
    return 0;
}

/* Apakah baris ini directive `Key = Value`? */
static int es_is_directive(const char* s, char* key, int ksz,
                           char* val, int vsz) {
    const char* eq = NULL;
    for (const char* p = s; *p; p++) if (*p == '=') { eq = p; break; }
    if (!eq || eq == s) return 0;
    uint32_t kl = (uint32_t)(eq - s);
    if ((int)kl >= ksz) return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')))
        return 0;
    for (uint32_t i = 0; i < kl; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    for (uint32_t i = 0; i < kl; i++) key[i] = s[i];
    key[kl] = '\0';

    const char* v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    uint32_t vl = (uint32_t)strlen(v);
    while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
    if ((int)vl >= vsz) vl = (uint32_t)vsz - 1;
    for (uint32_t i = 0; i < vl; i++) val[i] = v[i];
    val[vl] = '\0';
    return 1;
}

static int es_streq_ci(const char* a, const char* b) {
    uint32_t i = 0;
    for (; a[i] && b[i]; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return a[i] == b[i];
}

/* `set -x FILE` — muat berkas ke stack skrip. */
static void es_start(const char* path) {
    if (es_depth >= ES_MAX_DEPTH) {
        printf("es: terlalu dalam (maks %d)\n", ES_MAX_DEPTH);
        return;
    }
    struct es_frame* f = &es_stack[es_depth];
    f->len = 0;
    f->pos = 0;
    f->lines = 0;
    f->errs = 0;
    f->log = 0;
    f->buf[0] = '\0';

    if (es_read_all(path, f->buf, ES_FILE_MAX, &f->len) != 0) return;

    /* ---- baris pertama berarti: [Eqshell] ---- */
    uint32_t p = 0;
    int hdr = 0;
    while (p < f->len) {
        uint32_t ls = p;
        while (p < f->len && f->buf[p] != '\n') p++;
        uint32_t ll = p - ls;
        if (p < f->len) p++;
        char line[64];
        uint32_t n = ll < 63 ? ll : 63;
        for (uint32_t k = 0; k < n; k++) line[k] = f->buf[ls + k];
        line[n] = '\0';
        uint32_t a = 0;
        while (line[a] == ' ' || line[a] == '\t') a++;
        uint32_t b = (uint32_t)strlen(line + a);
        while (b > 0 && (line[a + b - 1] == ' ' || line[a + b - 1] == '\t'))
            b--;
        if (b == 0 || line[a] == '#') continue;
        for (uint32_t k = 0; k < b; k++) line[k] = line[a + k];
        line[b] = '\0';
        if (es_ncmpi(line, "[Eqshell]", 9) && b == 9) hdr = 1;
        break;
    }
    if (!hdr) {
        printf("es: bukan berkas eqshell (butuh [Eqshell])\n");
        return;
    }

    /* ---- directive Key = Value setelah header ---- */
    while (p < f->len) {
        uint32_t ls = p;
        while (p < f->len && f->buf[p] != '\n') p++;
        uint32_t ll = p - ls;
        if (p < f->len) p++;
        char line[ES_LINE_MAX + 4];
        uint32_t n = ll < sizeof(line) - 1 ? ll : (uint32_t)(sizeof(line) - 1);
        for (uint32_t k = 0; k < n; k++) line[k] = f->buf[ls + k];
        line[n] = '\0';
        uint32_t a = 0;
        while (line[a] == ' ' || line[a] == '\t') a++;
        uint32_t b = (uint32_t)strlen(line + a);
        while (b > 0 && (line[a + b - 1] == ' ' || line[a + b - 1] == '\t'))
            b--;
        if (b == 0 || line[a] == '#') continue;
        for (uint32_t k = 0; k < b; k++) line[k] = line[a + k];
        line[b] = '\0';

        char key[64], val[160];
        if (!es_is_directive(line, key, sizeof(key), val, sizeof(val))) {
            p = ls;                      /* baris ini perintah -> eksekusi */
            break;
        }
        if (es_streq_ci(key, "Log")) {
            if (es_streq_ci(val, "true") || es_streq_ci(val, "1")) f->log = 1;
            else if (es_streq_ci(val, "false") || es_streq_ci(val, "0"))
                f->log = 0;
            else {
                printf("es: nilai Log tak dikenal: %s\n", val);
                f->errs++;
            }
        } else {
            printf("es: directive tak dikenal: %s\n", key);
            f->errs++;
        }
    }
    f->pos = p;

    /* ---- aktifkan ---- */
    es_depth++;
    if (es_depth == 1 && f->log) {
        es_log_any = 1;
        es_loglen = 0;
        es_log_ovf = 0;
        es_log_puts("== eqshell: ");
        es_log_puts(path);
        es_log_puts(" ==\n");
    } else if (f->log && !es_log_any) {
        es_log_any = 1;
        es_log_puts("== eqshell: ");
        es_log_puts(path);
        es_log_puts(" ==\n");
    }
    printf("es: menjalankan %s (Log=%s)\n", path, f->log ? "True" : "False");
}

/* ============================================================
 *  `eqgu <file.c>` — editor dengan cek-kompilasi saat save
 * ------------------------------------------------------------
 *  Hook editor (lihat codeEditor.h): dipasang HANYA selama editor
 *  terbuka lewat eqgu, dilepas lagi setelahnya — `edit` biasa tidak
 *  pernah berubah perilaku.
 * ============================================================ */
static int eqgu_is_c(const char* p) {
    uint32_t n = (uint32_t)strlen(p);
    return n >= 2 && p[n - 1] == 'c' && p[n - 2] == '.';
}

/* fn(fullpath, msg, msgsz) -> <0 none / 0 OK / 1 gagal. */
static int eqgu_check(const char* fullpath, char* msg, int msgsz) {
    if (!fullpath || !eqgu_is_c(fullpath)) return -1;
    if (msgsz < 16) return -1;

    struct fs_node* dir = NULL;
    for (int d = 0; d < SHELL_SYS_PATH_DIRS && !dir; d++) {
        struct fs_node* dd = shell_sys_path_dir(d);
        if (dd && fs_find_child(dd, "mtcc.mrp")) dir = dd;
    }
    if (!dir) {
        ecf_copy_msg(msg, msgsz, "Cek: GAGAL: mtcc.mrp tak ada");
        return 1;
    }

    char args[128];
    snprintf(args, sizeof(args), "-q -c %s", fullpath);

    static char cap[768];
    term_capture_begin(cap, sizeof(cap));
    syscall_set_args(args);
    mrp_set_quiet(1);
    mrp_run_hint(dir, "mtcc.mrp", mrp_arena_hint_for("mtcc.mrp"));
    mrp_set_quiet(0);
    syscall_set_args("");
    term_capture_end();

    if (mrp_last_exit_status() == 0) {
        ecf_copy_msg(msg, msgsz, "Cek: OK");
        return 0;
    }

    /* Baris PERTAMA yang diawali "mtcc: error" -> pesan gagal. */
    const char* line = NULL;
    uint32_t k = 0;
    while (cap[k]) {
        uint32_t s = k;
        while (cap[k] && cap[k] != '\n') k++;
        uint32_t e = k;
        if (cap[k] == '\n') k++;
        if (e - s >= 11 &&
            cap[s] == 'm' && cap[s + 1] == 't' && cap[s + 2] == 'c' &&
            cap[s + 3] == 'c' && cap[s + 4] == ':' && cap[s + 5] == ' ' &&
            cap[s + 6] == 'e' && cap[s + 7] == 'r' && cap[s + 8] == 'r') {
            line = cap + s;
            break;
        }
    }

    int n = 0;
    const char* pre = "Cek: GAGAL: ";
    for (int j = 0; pre[j] && n < msgsz - 1; j++) msg[n++] = pre[j];
    if (line) {
        for (uint32_t j = 0; line[j] && line[j] != '\n' && n < msgsz - 1; j++)
            msg[n++] = line[j];
    } else {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "exit %u", (unsigned)mrp_last_exit_status());
        for (int j = 0; tmp[j] && n < msgsz - 1; j++) msg[n++] = tmp[j];
    }
    msg[n] = '\0';
    return 1;
}

// ============================================================
//  0.4 Beta — Qfs / copy / del  (alur instalasi MANUAL, tanpa wizard)
// ------------------------------------------------------------
//  Qfs                  bantuan
//  Qfs -list-disk       daftar disk pakai nama hda..hdh
//  Qfs -t hdX -format fat32
//                       format partisi jadi FAT32 label EQUINOXBASE
//  copy SRC [->] DST    salin file/pohon; SRC boleh *./* (glob isi
//                       direktori); direktori tujuan DIBUAT otomatis
//  del PATH             hapus rekursif (file atau pohon)
// ============================================================

/* "hda".."hdh" -> slot 0..7 (sama dengan keluaran Qfs -list-disk). */
static int qfs_slot_of(const char* name) {
    if (!name) return -1;
    if (name[0] != 'h' || name[1] != 'd') return -1;
    if (name[2] < 'a' || name[2] > 'h') return -1;
    if (name[3] != '\0') return -1;
    return name[2] - 'a';
}

/* "hda".."hdh" -> slot 0..7 (sama dengan keluaran Qfs -list-disk). */

static void qfs_list_disk(void) {
    printf("Qfs -list-disk\n");
    for (int s = 0; s < 8; s++) {
        char nm[8];
        nm[0] = 'h'; nm[1] = 'd'; nm[2] = (char)('a' + s); nm[3] = '\0';
        struct fat32_slot_info in;
        if (fat32_slot_scan(s, &in) != 0) {
            printf("%s  (kosong)\n", nm);
            continue;
        }
        const char* st;
        if (in.is_base)       st = "base";
        else if (in.active)   st = "mounted";
        else if (in.has_fat32) st = "siap";
        else                  st = "tanpa-fat32";
        printf("%s  %s  FAT32=%d label=%s %s\n",
               nm, in.size, in.has_fat32,
               in.label[0] ? in.label : "-", st);
    }
}

static void qfs_format(const char* name) {
    int slot = qfs_slot_of(name);
    if (slot < 0) {
        printf("Qfs: nama disk tak dikenal: %s (pakai hda..hdh)\n",
               name ? name : "");
        return;
    }
    struct fat32_slot_info in;
    if (fat32_slot_scan(slot, &in) != 0) {
        printf("Qfs: %s tidak ada disk\n", name);
        return;
    }
    if (in.active) {
        printf("Qfs: %s sedang terpasang — lepas dulu (umount %s)\n",
               name, name);
        return;
    }
    printf("Qfs: format %s -> FAT32 label EQUINOXBASE (semua isi hilang)\n",
           name);
    int r = fat32_mkfs(slot, "EQUINOXBASE");
    if (r != 0) printf("Qfs: format %s gagal (%d)\n", name, r);
}

static void qfs_usage(void) {
    printf("Qfs: alat disk equinox\n");
    printf("  Qfs                     bantuan ini\n");
    printf("  Qfs -list-disk          daftar disk (hda..hdh)\n");
    printf("  Qfs -t hdX -format fat32\n");
    printf("                          format -> FAT32 whole-disk EQUINOXBASE\n");
    printf("  Qfs -install-boot [hdX]\n");
    printf("                          pasang bootloader GRUB (boot tanpa CD)\n");
}

/* ============================================================
 *  0.4 Beta — Qfs -install-boot [hdX]: BOOT DARI DISK, TANPA CD
 * ------------------------------------------------------------
 *  Layout volume mkfs (partisi 0x0C @ LBA 2048) menyisakan gap
 *  LBA 1..2047. install-boot menulis:
 *
 *    LBA 0     GRUB boot.img — kode MBR; TABEL PARTISI DISK
 *              DIPERTAHANKAN (byte 446..509 dari MBR lama);
 *              kernel_sector @0x5C = 1
 *    LBA 1..N  GRUB core.img (biosdisk+part_msdos+fat+multiboot+
 *              normal+search) dari /equinox/bootimg/core.img
 *    volume    /boot/grub/grub.cfg di-generate: search label
 *              EQUINOXBASE -> root (partisi), multiboot
 *              /boot/kernel.elf + baris module per file
 *              (.mrp/.c/.wad/.ruf/.elf/.h) di root flat,
 *              /equinox/**, /test/**, /user/** (boot/bootimg
 *              dikecualikan). Dest cmdline = path RAMFS.
 *
 *  FSInfo/VBR berada DI DALAM partisi (@2048+) — tidak mungkin
 *  bentrok dengan core.img di gap. Layout ini dibukukan oleh
 *  scripts/bootproof.sh (boot dari disk tanpa CD, berulang).
 * ============================================================ */

#define QIB_CFG_MAX     24576   /* buffer grub.cfg                 */
#define QIB_MOD_MAX     256     /* batas baris module              */
#define QIB_WALK_DEPTH  6

static const char* const QIB_MOD_EXT[] = {
    ".mrp", ".c", ".wad", ".ruf", ".elf", ".h", NULL
};

static int qib_ext_ok(const char* name) {
    size_t l = strlen(name);
    for (int i = 0; QIB_MOD_EXT[i]; i++) {
        size_t e = strlen(QIB_MOD_EXT[i]);
        if (l > e && strcmp(name + l - e, QIB_MOD_EXT[i]) == 0) return 1;
    }
    return 0;
}

/* Tulis satu baris module ke buffer cfg; kembalikan 0 ok / -1 penuh */
static int qib_emit_module(char* cfg, uint32_t* cl, const char* rel) {
    if (*cl + strlen(rel) + 24 >= QIB_CFG_MAX) return -1;
    *cl += (uint32_t)snprintf(cfg + *cl, QIB_CFG_MAX - *cl,
                              "    module /%s %s\n", rel, rel);
    return 0;
}

/* DFS pohon volume -> baris module. `rel` = path tanpa slash depan. */
static void qib_walk(struct fs_node* dir, const char* rel, int depth,
                     int* nmod, uint32_t* cl, char* cfg) {
    if (depth > QIB_WALK_DEPTH || *nmod >= QIB_MOD_MAX) return;
    if (dir->backing == 1 && !dir->populated)
        fat32_populate_dir(dir);          /* mirror FAT dir lazily */
    for (struct fs_node* c = dir->children; c; c = c->next) {
        if (c->is_dir) {
            /* skip: /boot (kernel + grub.cfg ditangani eksplisit)
             * dan /equinox/bootimg (gambar bootloader sendiri)  */
            if (strcmp(c->name, "boot") == 0) continue;
            if (strcmp(c->name, "bootimg") == 0) continue;
            char nrel[160];
            snprintf(nrel, sizeof(nrel), "%s%s%s",
                     rel, rel[0] ? "/" : "", c->name);
            qib_walk(c, nrel, depth + 1, nmod, cl, cfg);
            if (*nmod >= QIB_MOD_MAX) return;
        } else {
            if (!qib_ext_ok(c->name)) continue;
            char nrel[160];
            snprintf(nrel, sizeof(nrel), "%s%s%s",
                     rel, rel[0] ? "/" : "", c->name);
            if (qib_emit_module(cfg, cl, nrel) == 0) (*nmod)++;
        }
    }
    /* fs_ls lazily-populates children untuk node FAT — paksa */
}

/* Pastikan anak-dir bernama `name` ada (buat bila perlu). */
static struct fs_node* qib_dir_under(struct fs_node* parent, const char* name) {
    struct fs_node* d = fs_find_child(parent, name);
    if (d && d->is_dir) return d;
    if (fs_create_dir(parent, name) != 0) return NULL;
    return fs_find_child(parent, name);
}

static void qfs_install_boot(const char* name) {
    int slot = name ? qfs_slot_of(name) : -1;
    if (name && name[0] && slot < 0) {
        printf("Qfs: nama disk tak dikenal: %s (pakai hda..hdh)\n", name);
        return;
    }
    if (slot < 0) {
        /* tanpa argumen: disk FAT32 pertama yang bukan volume aktif */
        for (int s = 0; s < 8; s++) {
            struct fat32_slot_info in;
            if (fat32_slot_scan(s, &in) != 0 || !in.has_fat32) continue;
            if (in.active) continue;
            slot = s;
            break;
        }
        if (slot < 0) {
            printf("Qfs: tidak ada kandidat disk (format dulu: Qfs -t hdX -format fat32)\n");
            return;
        }
    }
    char nm[8];
    nm[0] = 'h'; nm[1] = 'd'; nm[2] = (char)('a' + slot); nm[3] = '\0';

    /* --- 1. volume harus WHOLE-DISK FAT32 (layout mkfs 0.4 Beta,
     *         satu-satunya yang GRUB core minimal bisa mount) --- */
    struct mbr_partition parts[4];
    int npt = mbr_read_partitions(slot, parts);
    if (npt < 0) { printf("Qfs: %s tidak ada disk\n", nm); return; }
    if (npt > 0) {
        printf("Qfs: %s berlayout partisi — format ulang dulu:\n"
               "     Qfs -t %s -format fat32 (whole-disk)\n",
               nm, nm);
        return;
    }
    struct fat32_slot_info in;
    if (fat32_slot_scan(slot, &in) != 0 || !in.has_fat32) {
        printf("Qfs: %s bukan volume FAT32 (format dulu: Qfs -t %s -format fat32)\n",
               nm, nm);
        return;
    }

    /* --- 2. sumber boot.img + core.img dari /equinox/bootimg --- */
    struct fs_node* root = fs_get_root();
    struct fs_node* nboot = fs_get_node_from_path(root, "/equinox/bootimg/boot.img");
    struct fs_node* ncore = fs_get_node_from_path(root, "/equinox/bootimg/core.img");
    if (!nboot || !ncore || nboot->is_dir || ncore->is_dir ||
        fs_ensure_content(nboot) != 0 || fs_ensure_content(ncore) != 0 ||
        !nboot->content || !ncore->content || nboot->size != 512) {
        printf("Qfs: /equinox/bootimg/{boot.img,core.img} tidak lengkap di RAMFS\n");
        return;
    }
    uint32_t core_secs = (ncore->size + 511) / 512;
    if (core_secs < 2 || core_secs > 2046) {
        printf("Qfs: core.img ukuran aneh (%u sektor) — tolak\n",
               (unsigned)core_secs);
        return;
    }

    /* --- 3. node volume: pakai mount aktif bila sama, else mount ---
     * (tanpa deref struct fat32_mount — status via fat32_slot_scan) */
    int we_mounted = 0;
    struct fs_node* mnt = NULL;
    int active_slot = -1;
    for (int s = 0; s < 8; s++) {
        struct fat32_slot_info a;
        if (fat32_slot_scan(s, &a) == 0 && a.active) { active_slot = s; break; }
    }
    if (active_slot == slot) {
        mnt = fs_get_node_from_path(fs_get_root(), "/mnt");
        if (!mnt || !mnt->backing)
            mnt = fs_get_root();      /* volume base dipromosikan jadi / */
    } else {
        if (active_slot >= 0) {
            printf("Qfs: volume lain sedang terpasang — umount dulu\n");
            return;
        }
        if (fat32_mount_slot(slot) != 0) {
            printf("Qfs: mount %s gagal\n", nm);
            return;
        }
        mnt = fs_get_node_from_path(fs_get_root(), "/mnt");
        we_mounted = 1;
    }
    if (!mnt) { printf("Qfs: node volume tidak ketemu\n"); return; }
    if (!fs_find_child(mnt, "equinox")) {
        printf("Qfs: %s bukan volume instalasi (tidak ada /equinox)\n", nm);
        if (we_mounted) printf("Qfs: (volume %s dibiarkan terpasang di /mnt)\n", nm);
        return;
    }

    /* --- 4. MBR: boot.img + BPB volume + area PT di-nol-kan + ks=1.
     *         (BPB copy + zero-PT = persis resep bootproof yang
     *         terbukti boot; zero-PT mencegah entri partisi palsu
     *         dari data internal boot.img.) --- */
    uint8_t mbr[512];
    memcpy(mbr, nboot->content, 512);
    uint8_t vbr[512];
    if (blk_read(slot, 0, 1, vbr) != 0) {
        printf("Qfs: baca VBR %s gagal\n", nm);
        return;
    }
    memcpy(mbr + 3, vbr + 3, 0x5A - 3);          /* BPB+EBPB volume */
    memset(mbr + 446, 0, 64);                    /* tanpa entri partisi */
    uint32_t ks = 1;                             /* core.img @ LBA 1 */
    memcpy(mbr + 0x5C, &ks, 4);
    mbr[510] = 0x55; mbr[511] = 0xAA;

    /* --- 5. grub.cfg dari isi volume --- */
    char* cfg = (char*)malloc(QIB_CFG_MAX);
    if (!cfg) { printf("Qfs: heap habis untuk grub.cfg\n"); return; }
    uint32_t cl = 0;
    int nmod = 0;
    cl += (uint32_t)snprintf(cfg + cl, QIB_CFG_MAX - cl,
        /* 0.4 Beta: whole-disk -> volume = (hd0); TANPA `search` */
        "set root=(hd0)\n"
        "set prefix=(hd0)/boot/grub\n"
        "set timeout=2\n"
        "set default=0\n"
        "menuentry \"Equinox OS\" {\n"
        "    multiboot /boot/kernel.elf\n"
        "    module /boot/kernel.elf boot/kernel.elf\n");
    nmod++;
    if (mnt->backing == 1 && !mnt->populated)
        fat32_populate_dir(mnt);
    /* root flat (file .mrp/.wad hasil wizard) */
    for (struct fs_node* c = mnt->children; c; c = c->next) {
        if (c->is_dir || !qib_ext_ok(c->name)) continue;
        if (qib_emit_module(cfg, &cl, c->name) == 0) nmod++;
    }
    /* pohon /equinox, /test, /user */
    static const char* const trees[] = { "equinox", "test", "user", NULL };
    for (int t = 0; trees[t]; t++) {
        struct fs_node* d = fs_find_child(mnt, trees[t]);
        if (d && d->is_dir) qib_walk(d, trees[t], 1, &nmod, &cl, cfg);
    }
    if (cl + 8 >= QIB_CFG_MAX) nmod = QIB_MOD_MAX;   /* flag penuh */
    cl += (uint32_t)snprintf(cfg + cl, QIB_CFG_MAX - cl, "}\n");

    /* --- 6. TULIS: FS dulu (grub.cfg — bisa memicu flush FSInfo
     *         FAT), BARU core.img, dan MBR paling akhir. --- */
    printf("Qfs: install-boot %s — core.img %u sektor @ LBA 1, %d module line\n",
           nm, (unsigned)core_secs, nmod);
    struct fs_node* dboot = qib_dir_under(mnt, "boot");
    if (!dboot) { printf("Qfs: buat /boot gagal\n"); free(cfg); return; }
    struct fs_node* dgrub = qib_dir_under(dboot, "grub");
    if (!dgrub) { printf("Qfs: buat /boot/grub gagal\n"); free(cfg); return; }
    if (fs_write_binary(dgrub, "grub.cfg", (const uint8_t*)cfg, cl) != 0) {
        printf("Qfs: tulis /boot/grub/grub.cfg gagal\n");
        free(cfg);
        return;
    }
    free(cfg);
    struct fs_node* kern = fs_find_child(dboot, "kernel.elf");
    if (!kern || kern->is_dir)
        printf("Qfs: PERINGATAN /boot/kernel.elf tidak ada di volume —\n"
               "     jalankan wizard dulu, atau salin manual.\n");

    const uint8_t* csrc = (const uint8_t*)ncore->content;
    for (uint32_t off = 0; off < core_secs; off += 128) {
        uint32_t n = (core_secs - off) > 128 ? 128 : (core_secs - off);
        if (blk_write(slot, 1 + off, n, csrc + off * 512) != 0) {
            printf("Qfs: tulis core.img @ LBA %u GAGAL\n",
                   (unsigned)(1 + off));
            return;
        }
    }
    if (blk_write(slot, 0, 1, mbr) != 0) {
        printf("Qfs: tulis MBR GAGAL — volume TIDAK bootable (ulangi)\n");
        return;
    }

    /* --- 7. verifikasi baca-balik --- */
    uint8_t rb[512];
    int ok0 = (blk_read(slot, 0, 1, rb) == 0 &&
               memcmp(rb, mbr, 512) == 0);
    int ok1 = (blk_read(slot, 1, 1, rb) == 0 &&
               memcmp(rb, csrc, 512) == 0);
    int ok2 = (blk_read(slot, core_secs, 1, rb) == 0 &&
               memcmp(rb, csrc + (core_secs - 1) * 512, 512) == 0);
    printf("Qfs: verifikasi MBR=%s core-awal=%s core-akhir=%s\n",
           ok0 ? "ok" : "GAGAL", ok1 ? "ok" : "GAGAL", ok2 ? "ok" : "GAGAL");
    if (ok0 && ok1 && ok2) {
        printf("Qfs: %s BOOTABLE — reboot tanpa CD (QEMU: -boot order=c)\n", nm);
        printf("Qfs: grub.cfg = /boot/grub/grub.cfg (%u bytes)\n", (unsigned)cl);
    } else {
        printf("Qfs: verifikasi gagal — cek disk/kabel, ulangi install-boot\n");
    }
    if (we_mounted)
        printf("Qfs: volume %s dibiarkan terpasang di /mnt (umount bila perlu)\n", nm);
}

static void cmd_qfs(struct fs_node* cwd, const char* rest) {
    while (*rest == ' ') rest++;
    if (!rest[0]) { qfs_usage(); return; }

    char tname[16];
    char tformat[16];
    tname[0] = '\0';
    tformat[0] = '\0';
    int want_ib = 0;

    const char* p = rest;
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;
        char tok[32];
        uint32_t n = 0;
        while (*p && *p != ' ' && n < sizeof(tok) - 1) tok[n++] = *p++;
        tok[n] = '\0';
        if (strcmp(tok, "-list-disk") == 0 || strcmp(tok, "-l") == 0) {
            qfs_list_disk();
            return;
        } else if (strcmp(tok, "-install-boot") == 0 ||
                   strcmp(tok, "-ib") == 0) {
            want_ib = 1;
        } else if (want_ib && !tname[0] && qfs_slot_of(tok) >= 0) {
            snprintf(tname, sizeof(tname), "%s", tok);   /* -install-boot hdX */
        } else if (strcmp(tok, "-t") == 0) {
            while (*p == ' ') p++;
            n = 0;
            while (*p && *p != ' ' && n < sizeof(tname) - 1) tname[n++] = *p++;
            tname[n] = '\0';
        } else if (strcmp(tok, "-format") == 0) {
            while (*p == ' ') p++;
            n = 0;
            while (*p && *p != ' ' && n < sizeof(tformat) - 1)
                tformat[n++] = *p++;
            tformat[n] = '\0';
        } else {
            printf("Qfs: opsi tak dikenal: %s\n", tok);
            qfs_usage();
            return;
        }
    }

    (void)cwd;
    if (want_ib) {
        qfs_install_boot(tname[0] ? tname : NULL);
        return;
    }
    if (tname[0] && tformat[0]) {
        if (strcmp(tformat, "fat32") != 0) {
            printf("Qfs: format tak dikenal: %s (hanya fat32)\n", tformat);
            return;
        }
        qfs_format(tname);
        return;
    }
    qfs_usage();
}

/* ------------------------------------------------------------
 *  copy
 * ------------------------------------------------------------ */
/* Apakah dua path absolut saling terkait (sama / satu di bawah yang
 * lain) pada batas komponen? Dipakai menahan salin ke dirinya sendiri. */
static int path_at(const char* p, const char* pre) {
    uint32_t i = 0;
    while (pre[i]) { if (p[i] != pre[i]) return 0; i++; }
    if (i == 0) return 0;
    return (pre[i - 1] == '/') ? 1 : (p[i] == '\0' || p[i] == '/');
}

static int path_related(const char* a, const char* b) {
    return path_at(a, b) || path_at(b, a);
}

static void path_join(char* out, uint32_t outsz,
                      const char* dir, const char* name) {
    uint32_t i = 0;
    for (; dir[i] && i < outsz - 1; i++) out[i] = dir[i];
    if (i && out[i - 1] != '/' && i < outsz - 1) out[i++] = '/';
    for (uint32_t k = 0; name[k] && i < outsz - 1; k++) out[i++] = name[k];
    out[i] = '\0';
}

/* Ambil nama terakhir dari path absolut. */
static const char* path_base(const char* p) {
    const char* n = p;
    for (const char* s = p; *s; s++) if (*s == '/') n = s + 1;
    return n;
}

static int copy_prog_next;

static void copy_progress(uint32_t done, uint32_t total) {
    if (!total) return;
    uint32_t pct = (uint32_t)(((uint64_t)done * 100) / total);
    if ((int)pct < copy_prog_next) return;
    copy_prog_next = (int)pct + 10;
    printf("copy: progres %u%% (%u/%u KB)\n",
           pct, (unsigned)(done / 1024), (unsigned)(total / 1024));
}

/* Salin isi `src` ke direktori `dst`.
 *   sabs / dabs : path absolut masing-masing (untuk guard)
 *   rootdest    : path tujuan ASLI — subpohon yang berada di dalamnya
 *                 dilewati supaya salin tidak masuk ke dirinya sendiri. */
static void copy_children(struct fs_node* src, struct fs_node* dst,
                          const char* sabs, const char* dabs,
                          const char* rootdest,
                          int* nfiles, int* nbytes) {
    if (!src || !src->is_dir || !dst || !dst->is_dir) return;
    if (src->backing == 1 && !src->populated) fat32_populate_dir(src);

    for (struct fs_node* c = src->children; c; c = c->next) {
        char sc[300], dc[300];
        path_join(sc, sizeof(sc), sabs, c->name);
        path_join(dc, sizeof(dc), dabs, c->name);

        /* tujuan berada di dalam subpohon sumber ini -> lewati */
        if (path_related(sc, rootdest)) {
            printf("copy: lewati %s (beririsan dengan tujuan)\n", sc);
            continue;
        }

        if (c->is_dir) {
            struct fs_node* sub = eqi_dir_under(dst, c->name);
            if (sub) copy_children(c, sub, sc, dc, rootdest,
                                   nfiles, nbytes);
            else printf("copy: gagal membuat %s\n", dc);
            continue;
        }
        if (fs_ensure_content(c) != 0 || !c->content) continue;
        /* idempoten: sudah ada dengan ukuran sama -> jangan ditulis */
        struct fs_node* ex = fs_find_child(dst, c->name);
        if (ex && !ex->is_dir && ex->size == c->size) continue;

        if (fs_write_binary(dst, c->name,
                            (const uint8_t*)c->content, c->size) == 0) {
            (*nfiles)++;
            *nbytes += (int)c->size;
        } else {
            printf("copy: GAGAL %s\n", dc);
        }
    }
}

static void copy_usage(void) {
    printf("copy: usage:\n");
    printf("  copy <src> [->] <dst>        salin file / direktori\n");
    printf("  copy <dir>/* [->] <dst>      salin ISI direktori (glob)\n");
    printf("  direktori tujuan dibuat otomatis (tak perlu mkdir)\n");
}

/* Buat direktori absolut (rekursif) -> node direktori terdalam. */
static struct fs_node* shell_mkdir_p(const char* dir) {
    struct fs_node* root = fs_get_root();
    if (!root || !dir || dir[0] != '/') return NULL;
    struct fs_node* cur = root;
    char comp[64];
    uint32_t i = 0;
    while (dir[i]) {
        while (dir[i] == '/') i++;
        if (!dir[i]) break;
        uint32_t s = i;
        while (dir[i] && dir[i] != '/') i++;
        uint32_t n = i - s;
        if (n == 0 || n >= sizeof(comp)) return NULL;
        for (uint32_t k = 0; k < n; k++) comp[k] = dir[s + k];
        comp[n] = '\0';
        struct fs_node* nx = fs_find_child(cur, comp);
        if (!nx) {
            if (fs_create_dir(cur, comp) != 0) return NULL;
            nx = fs_find_child(cur, comp);
        }
        if (!nx || !nx->is_dir) return NULL;
        cur = nx;
    }
    return cur;
}

static void cmd_copy(struct fs_node* cwd, const char* rest) {
    while (*rest == ' ') rest++;
    if (!rest[0]) { copy_usage(); return; }

    char src[300], dst[300];
    uint32_t i = 0;
    while (*rest && *rest != ' ' && i < sizeof(src) - 1) src[i++] = *rest++;
    src[i] = '\0';
    while (*rest == ' ') rest++;
    if (rest[0] == '-' && rest[1] == '>') {          /* "a -> b" / "a b" */
        rest += 2;
        while (*rest == ' ') rest++;
    }
    if (!rest[0]) { copy_usage(); return; }
    i = 0;
    while (*rest && *rest != ' ' && i < sizeof(dst) - 1) dst[i++] = *rest++;
    dst[i] = '\0';

    /* ---- glob: src diakhiri '*' -> salin ISI direktori ---- */
    int glob = 0;
    char srckey[300];
    shell_copy(srckey, sizeof(srckey), src);
    if (strchr(srckey, '*')) {
        uint32_t l = (uint32_t)strlen(srckey);
        if (srckey[l - 1] != '*') {
            printf("copy: glob hanya mendukung '*' di akhir\n");
            return;
        }
        glob = 1;
        srckey[--l] = '\0';                        /* buang '*'        */
        while (l > 0 && srckey[l - 1] == '/') srckey[--l] = '\0';
    }

    /* ---- path absolut ---- */
    char sabs[300], dabs[300];
    shell_resolve_path(cwd, (glob && !srckey[0]) ? "." : srckey,
                       sabs, sizeof(sabs));
    shell_resolve_path(cwd, dst, dabs, sizeof(dabs));

    struct fs_node* sn = fs_get_node_from_path(fs_get_root(), sabs);
    if (!sn) { printf("copy: '%s' tidak ada\n", src); return; }
    if (glob && !sn->is_dir) {
        printf("copy: '%s' bukan direktori\n", sabs);
        return;
    }

    /* ---- FILE tunggal ---- */
    if (!glob && !sn->is_dir) {
        if (fs_ensure_content(sn) != 0 || !sn->content) {
            printf("copy: gagal membaca %s\n", sabs);
            return;
        }
        struct fs_node* tgt = fs_get_node_from_path(fs_get_root(), dabs);
        char pdir[300];
        const char* fname;
        if (tgt && tgt->is_dir) {                   /* dst direktori    */
            shell_copy(pdir, sizeof(pdir), dabs);
            fname = path_base(sabs);
        } else {                                    /* dst = nama file  */
            shell_dir_of(dabs, pdir, sizeof(pdir));
            fname = path_base(dabs);
            if (shell_mkdir_p(pdir) == NULL) {
                printf("copy: gagal membuat %s\n", pdir);
                return;
            }
        }
        struct fs_node* pn = fs_get_node_from_path(fs_get_root(), pdir);
        if (!pn) {
            printf("copy: tujuan tak bisa dibuat: %s\n", pdir);
            return;
        }
        struct fs_node* ex = fs_find_child(pn, fname);
        if (ex && !ex->is_dir && ex->size == sn->size) {
            printf("copy: %s sudah ada (ukuran sama) — dilewati\n", fname);
        } else if (fs_write_binary(pn, fname, (const uint8_t*)sn->content,
                                   sn->size) != 0) {
            printf("copy: GAGAL %s -> %s\n", sabs, dabs);
        } else {
            printf("copy: 1 file, %d B -> %s/%s\n",
                   (int)sn->size, pdir, fname);
        }
        return;
    }

    /* ---- direktori: tujuan harus direktori (buat bila perlu) ---- */
    struct fs_node* dn = fs_get_node_from_path(fs_get_root(), dabs);
    if (dn && !dn->is_dir) {
        printf("copy: tujuan bukan direktori: %s\n", dabs);
        return;
    }
    int made = 0;
    if (!dn) {
        dn = shell_mkdir_p(dabs);
        if (!dn) { printf("copy: gagal membuat %s\n", dabs); return; }
        made = 1;
        printf("copy: membuat %s\n", dabs);
    }
    if (!glob && path_related(sabs, dabs)) {
        printf("copy: tujuan di dalam sumber: %s\n", dabs);
        return;
    }

    struct fs_node* into = dn;
    char iabs[300];
    shell_copy(iabs, sizeof(iabs), dabs);
    if (!glob && !made) {                           /* cp -r dst/ada    */
        struct fs_node* sub = eqi_dir_under(dn, sn->name);
        if (!sub) {
            printf("copy: gagal membuat %s/%s\n", dabs, sn->name);
            return;
        }
        into = sub;
        path_join(iabs, sizeof(iabs), dabs, sn->name);
    }

    int nf = 0, nb = 0;
    copy_prog_next = 0;
    fat32_set_write_progress(copy_progress);
    copy_children(sn, into, sabs, iabs, dabs, &nf, &nb);
    fat32_set_write_progress(NULL);

    printf("copy: %d file, %d B -> %s\n", nf, nb, iabs);
}

/* ------------------------------------------------------------
 *  del — hapus rekursif
 * ------------------------------------------------------------ */
static int rm_rf(struct fs_node* parent, const char* name, int* n) {
    if (!parent || !parent->is_dir) return -1;
    struct fs_node* node = fs_find_child(parent, name);
    if (!node) return -2;
    /* titik mount di bawah RAMFS -> EBUSY (jangan dihapus) */
    if (parent->backing != 1 && node->backing == 1) return -9;
    if (node->is_dir) {
        if (node->backing == 1) fat32_populate_dir(node);
        int guard = 0;
        while (node->children && guard++ < 100000) {
            int r = rm_rf(node, node->children->name, n);
            if (r != 0) return r;
        }
    }
    int r = fs_delete_node(parent, name);
    if (r == 0) (*n)++;
    return r;
}

static void cmd_del(struct fs_node* cwd, const char* rest) {
    while (*rest == ' ') rest++;
    if (!rest[0]) { printf("del: usage: del <path>\n"); return; }

    char path[300];
    shell_resolve_path(cwd, rest, path, sizeof(path));
    if (strcmp(path, "/") == 0) { printf("del: menolak hapus /\n"); return; }

    char pdir[300];
    shell_dir_of(path, pdir, sizeof(pdir));
    const char* nm = path_base(path);
    if (!nm[0]) { printf("del: path tak valid\n"); return; }

    struct fs_node* parent = fs_get_node_from_path(fs_get_root(), pdir);
    if (!parent || !parent->is_dir) {
        printf("del: '%s' tidak ada\n", path);
        return;
    }
    if (!fs_find_child(parent, nm)) {
        printf("del: '%s' tidak ada\n", path);
        return;
    }
    /* jangan sentuh mount point (mis. /mnt saat volume terpasang) */
    struct fs_node* t = fs_find_child(parent, nm);
    if (parent->backing != 1 && t->backing == 1) {
        printf("del: '%s' sedang terpasang (device busy)\n", path);
        return;
    }

    int n = 0;
    int r = rm_rf(parent, nm, &n);
    if (r == -9) printf("del: '%s' sedang terpasang (device busy)\n", path);
    else if (r != 0) printf("del: gagal menghapus %s (%d)\n", path, r);
    else printf("del: %s dihapus (%d entri)\n", path, n);
}

/* ============================================================
 *  0.4 Beta — SHELL HIDUP: glob `*`, redirect `> >> <`, pipe `|`
 * ------------------------------------------------------------
 *  Bangun di atas SYS_PIPE (FR-02) + spawn task (task_create_user):
 *    A | B          stdout A -> pipe -> stdin B (keduanya .mrp)
 *    A > f          stdout segmen terakhir -> file (timpa)
 *    A >> f         stdout segmen terakhir -> file (append)
 *    A < f          stdin segmen pertama <- isi file (pompa 4 KB)
 *    *.c            glob satu-bintang (prefix*suffix) per argumen
 *  Aturan:
 *   - hanya program .mrp (builtin tidak bisa di-pipe — resolusi
 *     otomatis ke system path /equinox/tools/<nama>.mrp; kalau
 *     belum ada, pesan menyuruh build: equinoxinstall -build <nama>)
 *   - `copy` dan baris ber-`<<` (save) dilewati — sintaks lama utuh
 *   - pipe antar segmen memompa sendiri (kooperatif); capture `>`
 *     dibatasi 512 KB; `A < f` maks 256 KB
 * ============================================================ */
#define XC_MAXTOK    24
#define XC_TOKLEN    80
#define XC_POOL      2048
#define XC_MAXSEG    4
#define XC_ARGSMAX   120      /* SYS_GETARGS 128 */
#define XC_CAP_MAX   (512 * 1024)
#define XC_IN_MAX    (256 * 1024)

static char  xc_tok[XC_MAXTOK][XC_TOKLEN];
static int   xc_ntok = 0;
static char  xc_pool[XC_POOL];
static char* xc_arg[XC_MAXTOK + 8];
static int   xc_narg = 0;

static void xc_tokenize(const char* line) {
    xc_ntok = 0;
    const char* p = line;
    while (*p && xc_ntok < XC_MAXTOK) {
        while (*p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && n < XC_TOKLEN - 1) xc_tok[xc_ntok][n++] = *p++;
        xc_tok[xc_ntok][n] = '\0';
        xc_ntok++;
    }
}

/* simpan string ke pool (NUL-terminated), kembalikan pointer atau
 * NULL bila pool penuh */
static char* xc_pool_take(const char* s) {
    size_t used = 0;
    if (xc_narg > 0)
        used = (size_t)(xc_arg[xc_narg - 1] - xc_pool) +
               strlen(xc_arg[xc_narg - 1]) + 1;
    size_t l = strlen(s);
    if (used + l + 1 > XC_POOL) return NULL;
    char* out = xc_pool + used;
    memcpy(out, s, l + 1);
    return out;
}

/* glob satu-bintang pada SATU token -> string berisi semua match
 * dipisah spasi (di pool), atau NULL bila tidak cocok/tak didukung */
static char* xc_glob_one(struct fs_node* cwd, const char* tok) {
    const char* star = strchr(tok, '*');
    if (!star || strchr(star + 1, '*')) return NULL;   /* 0 atau 2+ bintang */
    if (tok[0] == '-') return NULL;                    /* flag, bukan path  */

    char dirpath[XC_TOKLEN];
    const char* pat = tok;
    dirpath[0] = '.'; dirpath[1] = '\0';
    const char* slash = strrchr(tok, '/');
    if (slash) {
        size_t dl = (size_t)(slash - tok);
        if (dl == 0) { dirpath[0] = '/'; dirpath[1] = '\0'; }
        else {
            if (dl >= sizeof(dirpath)) return NULL;
            memcpy(dirpath, tok, dl);
            dirpath[dl] = '\0';
        }
        pat = slash + 1;
    }
    int pre = (int)(star - pat);
    const char* suf = star + 1;
    size_t sufl = strlen(suf);

    struct fs_node* d = NULL;
    if (dirpath[0] == '.' && dirpath[1] == '\0') {
        d = cwd;
    } else {
        char abs[160];
        shell_resolve_path(cwd, dirpath, abs, sizeof(abs));
        d = fs_get_node_from_path(fs_get_root(), abs);
    }
    if (!d || !d->is_dir) return NULL;
    if (d->backing == 1 && !d->populated) fat32_populate_dir(d);

    static char gbuf[XC_POOL / 2];
    size_t go = 0;
    int found = 0;
    for (struct fs_node* c = d->children; c; c = c->next) {
        if (c->is_dir) continue;
        size_t l = strlen(c->name);
        if (l < (size_t)pre + sufl) continue;
        if (pre && strncmp(c->name, pat, (size_t)pre) != 0) continue;
        if (sufl && strcmp(c->name + l - sufl, suf) != 0) continue;
        if (go + l + 2 >= sizeof(gbuf)) break;
        if (found) gbuf[go++] = ' ';
        memcpy(gbuf + go, c->name, l);
        go += l;
        found++;
    }
    if (!found) return NULL;
    gbuf[go] = '\0';
    char* out = xc_pool_take(gbuf);
    return out;
}

static int xc_is_op(const char* t) {
    return strcmp(t, "|") == 0 || strcmp(t, ">") == 0 ||
           strcmp(t, ">>") == 0 || strcmp(t, "<") == 0;
}

/* Cari <nama> / <nama>.mrp di system path (root, /bin,
 * /equinox/tools, /equinox/games). 1 = ketemu. */
static int xc_lookup_tool(const char* name, struct fs_node** out_dir,
                          char* out_fname, uint32_t fnsz) {
    if (!name || !name[0] || strchr(name, '/')) return 0;
    for (int d = 0; d < SHELL_SYS_PATH_DIRS; d++) {
        struct fs_node* dir = shell_sys_path_dir(d);
        if (!dir || !dir->is_dir) continue;
        struct fs_node* n = fs_find_child(dir, name);
        if (n && !n->is_dir) {
            if (out_dir) *out_dir = dir;
            if (out_fname) snprintf(out_fname, fnsz, "%s", name);
            return 1;
        }
        char mrpname[XC_TOKLEN];
        snprintf(mrpname, sizeof(mrpname), "%s.mrp", name);
        n = fs_find_child(dir, mrpname);
        if (n && !n->is_dir) {
            if (out_dir) *out_dir = dir;
            if (out_fname) snprintf(out_fname, fnsz, "%s", mrpname);
            return 1;
        }
    }
    return 0;
}

static void xc_close_pipes(int nseg, const uint32_t pseg[XC_MAXSEG][2],
                           const uint32_t pin[2], const uint32_t pout[2],
                           int have_in, int have_out) {
    for (int i = 0; i < nseg - 1; i++) {
        syscall_pipe_close(pseg[i][0]);
        syscall_pipe_close(pseg[i][1]);
    }
    if (have_in)  { syscall_pipe_close(pin[0]);  syscall_pipe_close(pin[1]); }
    if (have_out) { syscall_pipe_close(pout[0]); syscall_pipe_close(pout[1]); }
}

/* Tulis buf ke file (path cwd-relative). append=1 -> gabung isi lama */
static void xc_write_file(struct fs_node* cwd, const char* path,
                          const uint8_t* buf, uint32_t len, int append) {
    char abs[160];
    shell_resolve_path(cwd, path, abs, sizeof(abs));
    char dirpath[160];
    const char* nm = abs;
    dirpath[0] = '/'; dirpath[1] = '\0';
    const char* sl = strrchr(abs, '/');
    if (sl) {
        size_t dl = (size_t)(sl - abs);
        if (dl >= sizeof(dirpath)) dl = sizeof(dirpath) - 1;
        memcpy(dirpath, abs, dl);
        dirpath[dl] = '\0';
        nm = sl + 1;
    }
    if (!nm[0]) { printf("shell: nama file '%s' tidak valid\n", path); return; }
    struct fs_node* parent = fs_get_node_from_path(fs_get_root(), dirpath);
    if (!parent || !parent->is_dir) {
        printf("shell: direktori '%s' tidak ada\n", dirpath);
        return;
    }
    const uint8_t* data = buf;
    uint32_t total = len;
    uint8_t* cat = NULL;
    if (append) {
        struct fs_node* ex = fs_find_child(parent, nm);
        if (ex && !ex->is_dir && fs_ensure_content(ex) == 0 &&
            ex->content && ex->size > 0) {
            if ((uint64_t)ex->size + len > XC_CAP_MAX) {
                printf("shell: '>>' melebihi batas %d KB\n", XC_CAP_MAX / 1024);
                return;
            }
            cat = (uint8_t*)malloc(ex->size + len);
            if (!cat) { printf("shell: heap habis (>>)\n"); return; }
            memcpy(cat, ex->content, ex->size);
            memcpy(cat + ex->size, buf, len);
            data = cat;
            total = ex->size + len;
        }
    }
    if (fs_write_binary(parent, nm, data, total) != 0)
        printf("shell: tulis '%s' gagal\n", path);
    else
        printf("shell: %u byte -> %s%s\n", (unsigned)total, path,
               append ? " (append)" : "");
    if (cat) free(cat);
}

/* Handler utama. Return 1 = baris dipakai; 0 = lanjut dispatch lama
 * (input BISA sudah ditulis-ulang dengan hasil glob). */
static int shell_extended_cmd(struct fs_node* cwd, char* input, uint32_t insz) {
    if (strstr(input, "<<")) return 0;      /* save "x << ..." */
    xc_tokenize(input);
    if (xc_ntok == 0) return 0;
    if (strcmp(xc_tok[0], "copy") == 0) return 0;   /* glob milik copy */

    /* ---- fase A: glob (token ber '*') ---- */
    int globbed = 0;
    xc_narg = 0;
    for (int t = 0; t < xc_ntok; t++) {
        char* slot = xc_pool_take(xc_tok[t]);
        if (!slot) break;
        xc_arg[xc_narg++] = slot;
        if (strchr(xc_tok[t], '*')) {
            char* exp = xc_glob_one(cwd, xc_tok[t]);
            if (exp) { xc_arg[xc_narg - 1] = exp; globbed = 1; }
        }
    }

    /* ---- fase B: ada operator? ---- */
    int nops = 0;
    for (int t = 0; t < xc_narg; t++)
        if (xc_is_op(xc_arg[t])) nops++;

    if (nops == 0) {
        if (!globbed) return 0;
        char out[XC_TOKLEN * XC_MAXTOK];
        size_t ol = 0;
        out[0] = '\0';
        for (int t = 0; t < xc_narg; t++) {
            size_t l = strlen(xc_arg[t]);
            if (ol + l + 2 >= sizeof(out)) break;
            if (t) out[ol++] = ' ';
            memcpy(out + ol, xc_arg[t], l + 1);
            ol += l;
        }
        snprintf(input, insz, "%s", out);
        return 0;
    }

    /* ---- fase C: eksekusi pipeline / redirect ---- */
    char* outpath = NULL;
    int appendmode = 0;
    char* inpath = NULL;
    int skip[XC_MAXTOK + 8];             /* token op + operand file */
    for (int t = 0; t < XC_MAXTOK + 8; t++) skip[t] = 0;
    int seg_arg0[XC_MAXSEG + 1];
    int seg_argc[XC_MAXSEG + 1];
    int nseg = 0;
    seg_arg0[0] = 0;
    for (int t = 0; t < xc_narg; t++) {
        const char* a = xc_arg[t];
        if (strcmp(a, "|") == 0) {
            skip[t] = 1;
            if (t == seg_arg0[nseg]) {
                printf("shell: segmen pipe kosong\n");
                return 1;
            }
            seg_argc[nseg++] = t - seg_arg0[nseg];
            if (nseg >= XC_MAXSEG) {
                printf("shell: maks %d segmen pipe\n", XC_MAXSEG);
                return 1;
            }
            seg_arg0[nseg] = t + 1;
            continue;
        }
        if (strcmp(a, ">") == 0 || strcmp(a, ">>") == 0) {
            if (t + 1 >= xc_narg || xc_is_op(xc_arg[t + 1])) {
                printf("shell: '%s' butuh nama file\n", a);
                return 1;
            }
            outpath = xc_arg[t + 1];
            appendmode = (a[1] == '>');
            skip[t] = 1;
            skip[t + 1] = 1;
            t++;
            continue;
        }
        if (strcmp(a, "<") == 0) {
            if (t + 1 >= xc_narg || xc_is_op(xc_arg[t + 1])) {
                printf("shell: '<' butuh nama file\n");
                return 1;
            }
            inpath = xc_arg[t + 1];
            skip[t] = 1;
            skip[t + 1] = 1;
            t++;
            continue;
        }
    }
    if (xc_narg == seg_arg0[nseg]) {
        printf("shell: segmen pipe kosong\n");
        return 1;
    }
    seg_argc[nseg++] = xc_narg - seg_arg0[nseg];
    for (int s = 0; s < nseg; s++) {
        if (seg_argc[s] <= 0) {
            printf("shell: segmen pipe kosong\n");
            return 1;
        }
    }

    /* buat pipe antar segmen + in/out */
    uint32_t pseg[XC_MAXSEG][2];
    uint32_t pin[2]  = { 0, 0 };
    uint32_t pout[2] = { 0, 0 };
    int have_in  = (inpath != NULL);
    int have_out = (outpath != NULL);
    int np = 0;
    for (int i = 0; i < nseg - 1; i++) np++;
    if (have_in) np++;
    if (have_out) np++;
    if (np > 6) { printf("shell: terlalu banyak pipe\n"); return 1; }
    for (int i = 0; i < nseg - 1; i++)
        if (syscall_pipe_create(pseg[i]) != 0) {
            printf("shell: pipe habis (maks 8)\n");
            return 1;
        }
    if (have_in && syscall_pipe_create(pin) != 0) {
        printf("shell: pipe habis (maks 8)\n");
        return 1;
    }
    if (have_out && syscall_pipe_create(pout) != 0) {
        printf("shell: pipe habis (maks 8)\n");
        return 1;
    }

    /* spawn semua segmen (attach fd SEBELUM shell yield).
     * 0.4 Beta: task_irq_lock menahan preemption IRQ0 — tanpa ini anak
     * bisa sempat jalan (attach telat -> output ke console / refcount
     * bocor ke task mati). */
    uint32_t irqf = task_irq_lock();
    int pid[XC_MAXSEG];
    int spawned = 0;
    for (int s = 0; s < nseg; s++) {
        char** argv = &xc_arg[seg_arg0[s]];
        int argc = seg_argc[s];
        char args[XC_ARGSMAX];
        int al = 0;
        args[0] = '\0';
        for (int i = 1; i < argc && al < XC_ARGSMAX - 1; i++) {
            int gi = seg_arg0[s] + i;
            if (skip[gi]) continue;          /* op / nama file redirect */
            int need = (int)strlen(argv[i]);
            if (al + need + 2 >= XC_ARGSMAX) break;
            if (al) args[al++] = ' ';
            memcpy(args + al, argv[i], (size_t)need);
            al += need;
            args[al] = '\0';
        }
        struct fs_node* dir = NULL;
        char fname[XC_TOKLEN];
        if (!xc_lookup_tool(argv[0], &dir, fname, sizeof(fname))) {
            printf("shell: '%s' bukan program .mrp (builtin tak bisa dipipe)\n"
                   "       build dulu bila ada: equinoxinstall -build %s\n",
                   argv[0], argv[0]);
            break;
        }
        struct Task* t = task_create_user(fname, dir, fname, args,
                                          mrp_arena_hint_for(fname));
        if (!t) {
            printf("shell: spawn '%s' gagal (task penuh / OOM)\n", fname);
            break;
        }
        /* 0.4 Beta PENTING: spawn-inheritance otomatis menyalin pipe fd
         * 3+ milik shell ke anak — anak lalu MEMEGANG ujung lawannya
         * sendiri (reader memegang write-end) dan EOF tidak pernah
         * datang (self-deadlock). Buang warisan; hanya attach
         * eksplisit di bawah yang berlaku. (pipedemo tidak terdampak:
         * inheritance tetap ada, ini khusus pipeline shell.) */
        syscall_task_close_pipes(t);
        int sin = (s == 0) ? (have_in ? (int)pin[0] : -1)
                           : (int)pseg[s - 1][0];
        int sout = (s == nseg - 1) ? (have_out ? (int)pout[1] : -1)
                                   : (int)pseg[s][1];
        if (sin >= 0)  syscall_task_attach_pipe(t, 0, sin);
        if (sout >= 0) syscall_task_attach_pipe(t, 1, sout);
        pid[spawned++] = t->pid;
    }
    task_irq_unlock(irqf);
    if (spawned != nseg) {
        /* tutup fd pipe shell -> anak yang jalan dapat EOF/EIO lalu
         * selesai sendiri; tunggu supaya tidak jadi zombie */
        xc_close_pipes(nseg, pseg, pin, pout, have_in, have_out);
        for (int s = 0; s < spawned; s++) {
            uint32_t st = 0;
            task_wait_pid(pid[s], &st);
        }
        return 1;
    }

    /* tutup ujung shell yang kini milik anak (kecuali pin[1] pompa
     * dan pout[0] capture — milik shell) */
    for (int i = 0; i < nseg - 1; i++) {
        syscall_pipe_close(pseg[i][0]);
        syscall_pipe_close(pseg[i][1]);
    }
    if (have_in)  syscall_pipe_close(pin[0]);
    if (have_out) syscall_pipe_close(pout[1]);

    /* pompa input file -> pin[1] */
    if (have_in) {
        char abs[160];
        shell_resolve_path(cwd, inpath, abs, sizeof(abs));
        struct fs_node* f = fs_get_node_from_path(fs_get_root(), abs);
        if (!f || f->is_dir || fs_ensure_content(f) != 0 || !f->content) {
            printf("shell: '%s' tidak bisa dibaca\n", inpath);
        } else if (f->size > XC_IN_MAX) {
            printf("shell: '<' maks %d KB\n", XC_IN_MAX / 1024);
        } else {
            uint32_t off = 0;
            while (off < f->size) {
                uint32_t n = f->size - off;
                if (n > 4096) n = 4096;
                int w = syscall_pipe_write(pin[1],
                                           (const char*)f->content + off, n);
                if (w <= 0) break;       /* pembaca pergi (EIO) */
                off += (uint32_t)w;
            }
        }
        syscall_pipe_close(pin[1]);
    }

    /* tunggu semua anak berurutan */
    for (int s = 0; s < spawned; s++) {
        uint32_t st = 0;
        task_wait_pid(pid[s], &st);
        if (st != 0 && !have_out)
            printf("shell: segmen %d exit %u\n", s + 1, (unsigned)st);
    }

    /* capture stdout segmen terakhir -> file */
    if (have_out) {
        uint32_t cap = 8192;
        uint8_t* buf = (uint8_t*)malloc(cap);
        uint32_t len = 0;
        int oom = 0;
        if (!buf) {
            printf("shell: heap habis (capture)\n");
            oom = 1;
        }
        while (!oom) {
            if (len >= cap - 1) {
                if (cap >= XC_CAP_MAX) {
                    printf("shell: output dipotong pada %d KB\n",
                           XC_CAP_MAX / 1024);
                    break;
                }
                uint32_t ncap = cap * 2;
                if (ncap > XC_CAP_MAX) ncap = XC_CAP_MAX;
                uint8_t* nb = (uint8_t*)malloc(ncap);
                if (!nb) { printf("shell: heap habis (capture)\n"); break; }
                memcpy(nb, buf, len);
                free(buf);
                buf = nb;
                cap = ncap;
            }
            int r = syscall_pipe_read(pout[0], (char*)buf + len,
                                      cap - 1 - len);
            if (r <= 0) break;           /* EOF / error */
            len += (uint32_t)r;
        }
        if (!oom) {
            buf[len] = 0;
            xc_write_file(cwd, outpath, buf, len, appendmode);
            free(buf);
        }
        syscall_pipe_close(pout[0]);
    }
    return 1;
}

// ============================================================
//  SHELL MAIN LOOP
// ============================================================
extern "C" void shell_entry(void* arg) {
    char input[128];
    struct fs_node* cwd = fs_get_root();
    int is_new_shell = (arg != NULL);   /* F1: shell bukan boot */

    /* 0.4 Beta: HOME DIRECTORY = /user ("user space"). The shell now
     * starts there — files the user creates (ccfile, editor, mtcc
     * output) land in /user by default instead of polluting the
     * root. Falls back to root should /user somehow not exist
     * (e.g. a corrupted fs). */
    struct fs_node* home = fs_get_node_from_path(fs_get_root(), "/user");
    if (home && home->is_dir) {
        cwd = home;
    }

    if (is_new_shell) {
        printf("\n[shell] new console active — pid %d. F1 = new console, F2 = previous console\n",
               task_getpid());
    } else {
        print_intro();
    }

    /* 0.4 Beta — eggkg auto-aktivasi: bila [dependencies] bash=true di
     * system.ecf, sinkronkan /equinox/.local -> /bin + satu baris
     * status. Diam total bila tidak ada paket & flag off. */
    eggkg_boot_check();

    while (1) {
        /* 0.4 Beta: tutup capture baris skrip SEBELUM prompt supaya
         * prompt tidak ikut terekam ke /eqshell.log. */
        es_flush_line();

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
        //
        // 0.4 Beta: `set -x FILE` menjalankan skrip .es — barisnya diambil
        // dari stack skrip, BUKAN keyboard, sampai skrip habis. Rantai
        // dispatch di bawah tidak berubah sama sekali.
        if (es_next_line(input, sizeof(input))) {
            /* baris skrip: sengaja TIDAK dimasukkan ke history */
        } else {
            gets_history(input, sizeof(input));
            history_add(input);
        }

        /* 0.4 Beta: glob `*`, redirect `> >> <`, pipe `|` — baris dengan
         * fitur ini diproses di sini; sisanya lewat dispatch lama */
        if (shell_extended_cmd(cwd, input, sizeof(input))) {
            continue;
        }

        /* 0.4 Beta FR-01: file utilities prefer the USER PROGRAM
         * (<cmd>.mrp on the system path) over the kernel builtin. */
        if (shell_try_user_tool(input)) {
            continue;
        }

        if (strcmp(input, "clear") == 0) {
            clear_screen();
        }
        else if (strcmp(input, "echo") == 0) {
            printf("Echo mode activated! (type anything)\n");
            char buf[64];
            gets(buf, sizeof(buf));
            printf("You said: %s\n", buf);
        }
        else if (strcmp(input, "info") == 0) {
            printf("Equinox OS 0.4 Beta (32-bit, i686)\n");
            printf("Build: %s %s\n", __DATE__, __TIME__);
            printf("Fitur build 0.4 Beta: ruf v3 (variabel, copy/move ? to), morph pkg API + ANSI warna, eqbash, eggkg, mtcc -make, equinoxinstall -build\n");
            printf("RAM: 256 MB\n");
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
        else if (strcmp(input, "diskinfo") == 0) {
            /* 0.4 Beta: ATA drives + mounted FAT32 volume details. */
            fat32_diskinfo();
        }
        else if (strcmp(input, "mount") == 0 || starts_with(input, "mount ")) {
            /* 0.4 Beta: mount tanpa argumen = perilaku lama (cari volume
             * pertama); mount hdX = pilih slot secara manual. */
            const char* mrest = input + 5;
            while (*mrest == ' ') mrest++;
            if (!mrest[0]) {
                fat32_mount_cmd();
            } else {
                int mslot = qfs_slot_of(mrest);
                if (mslot < 0) {
                    printf("mount: pakai 'mount' atau 'mount hdX' (hda..hdh)\n");
                } else {
                    struct fat32_slot_info min3;
                    if (fat32_slot_scan(mslot, &min3) != 0)
                        printf("mount: %s tidak ada disk\n", mrest);
                    else if (!min3.has_fat32)
                        printf("mount: %s belum FAT32 — Qfs -t %s -format fat32\n",
                               mrest, mrest);
                    else
                        fat32_mount_slot(mslot);
                }
            }
        }
        else if (strcmp(input, "umount") == 0 || starts_with(input, "umount ")) {
            const char* urest = input + 6;
            while (*urest == ' ') urest++;
            if (!urest[0]) {
                fat32_unmount_cmd();
            } else {
                int uslot = qfs_slot_of(urest);
                if (uslot < 0) {
                    printf("umount: pakai 'umount' atau 'umount hdX' (hda..hdh)\n");
                } else {
                    struct fat32_slot_info uin;
                    if (fat32_slot_scan(uslot, &uin) != 0)
                        printf("umount: %s tidak ada disk\n", urest);
                    else if (!uin.active)
                        printf("umount: %s bukan volume yang terpasang\n", urest);
                    else
                        fat32_unmount_cmd();
                }
            }
        }
        else if (strcmp(input, "Qfs") == 0 || starts_with(input, "Qfs ")) {
            cmd_qfs(cwd, input + (input[3] ? 4 : 3));
        }
        else if (strcmp(input, "copy") == 0 || starts_with(input, "copy ")) {
            cmd_copy(cwd, input + 4);
        }
        else if (starts_with(input, "cp ")) {
            /* 0.4 Beta: alias bash dari copy (Qfs) */
            cmd_copy(cwd, input + 3);
        }
        else if (strcmp(input, "del") == 0 || starts_with(input, "del ")) {
            cmd_del(cwd, input + 3);
        }
        else if (starts_with(input, "xxd ")) {
            /* 0.4 Beta: hex dump — also proves FAT32 binary reads are
             * byte-exact (first N bytes of any file, disk-backed or
             * RAMFS). Usage: xxd <file> [n]  (default 64 bytes) */
            const char* rest = input + 4;
            while (*rest == ' ') rest++;
            char name[80];
            int i = 0;
            while (rest[i] && rest[i] != ' ' && i < (int)sizeof(name) - 1) {
                name[i] = rest[i];
                i++;
            }
            name[i] = '\0';
            int cnt = 64;
            if (rest[i] == ' ') cnt = atoi(rest + i + 1);
            if (!name[0]) {
                printf("xxd: usage: xxd <file> [n]\n");
            } else {
                struct fs_node* node =
                    (name[0] == '/')
                        ? fs_get_node_from_path(fs_get_root(), name)
                        : fs_find_child(cwd, name);
                if (!node)        printf("xxd: '%s' not found\n", name);
                else if (node->is_dir) printf("xxd: '%s' is a directory\n", name);
                else if (fs_ensure_content(node) != 0)
                    printf("xxd: '%s' read error\n", name);
                else {
                    if (cnt < 0) cnt = 0;
                    if ((uint32_t)cnt > node->size) cnt = node->size;
                    for (int off = 0; off < cnt; off += 16) {
                        printf("%04x  ", (unsigned)off);
                        for (int i = 0; i < 16; i++) {
                            if (off + i < cnt)
                                printf("%02x ", (uint8_t)node->content[off + i]);
                            else printf("   ");
                            if (i == 7) printf(" ");
                        }
                        printf(" |");
                        for (int i = 0; i < 16 && off + i < cnt; i++) {
                            char c = node->content[off + i];
                            put_char((c >= 32 && c < 127) ? c : '.');
                        }
                        printf("|\n");
                    }
                    printf("(%u bytes total, showing %d)\n",
                           (unsigned)node->size, cnt);
                }
            }
        }
        else if (starts_with(input, "alloc ")) {
            cmd_alloc(input + 6);
        }
        else if (starts_with(input, "free ")) {
            cmd_free(input + 5);
        }
        else if (strcmp(input, "ls") == 0 || strcmp(input, "lf") == 0 ||
                 starts_with(input, "ls ") || starts_with(input, "lf ")) {
            /* 0.4 Beta: builtin menerima argumen path (dulu ditangani
             * ls.mrp yang pindah ke repo eggkg) + opsi -l. Tanpa
             * argumen = cwd, seperti semula. */
            const char* arg = input + 2;
            while (*arg == ' ') arg++;
            int det = 0;
            struct fs_node* dir = cwd;
            if (arg[0] == '-' && arg[1] == 'l') {
                det = 1;
                arg += 2;
                while (*arg == ' ') arg++;
            }
            if (*arg) {
                struct fs_node* tgt =
                    (arg[0] == '/')
                        ? fs_get_node_from_path(fs_get_root(), arg)
                        : fs_find_child(cwd, arg);
                if (!tgt) printf("ls: '%s' not found\n", arg);
                else if (!tgt->is_dir) printf("ls: '%s' bukan direktori\n", arg);
                else fs_ls(tgt, det);
            } else {
                fs_ls(cwd, det);
            }
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
            sh_cdir(cwd, input + 5);
        }
        else if (starts_with(input, "mkdir ")) {
            /* 0.4 Beta: alias bash dari cdir (tool mkdir.mrp pindah
             * ke repo eggkg; refleks lama tetap jalan). */
            sh_cdir(cwd, input + 6);
        }
        else if (starts_with(input, "cfile ")) {
            sh_cfile(cwd, input + 6);
        }
        else if (starts_with(input, "touch ")) {
            /* 0.4 Beta: alias bash dari cfile */
            sh_cfile(cwd, input + 6);
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
        else if (starts_with(input, "save ")) {
            /* 0.4 Beta: the missing write verb — ccfile only creates NEW
             * files, so overwriting from the shell meant opening the
             * editor. `save` is create-OR-overwrite through the same
             * binary-safe path the editor and mget use (works on the
             * RAMFS and write-through on the FAT32 volume). */
            const char* rest = input + 5;
            char* delim = strstr(rest, " << ");
            if (delim) {
                *delim = '\0';
                char* name = (char*)rest;
                char* content = delim + 4;
                if (*content == '"') content++;
                size_t len = strlen(content);
                if (len > 0 && content[len-1] == '"') content[--len] = '\0';
                while (*name == ' ') name++;          /* trim leading  */
                if (!name[0]) {
                    printf("save: missing file name\n");
                } else {
                    int ret = fs_write_binary(cwd, name,
                                              (const uint8_t*)content,
                                              (uint32_t)len);
                    if (ret == 0)         printf("save: wrote '%s' (%u bytes)\n",
                                                 name, (unsigned)len);
                    else if (ret == -6)  printf("save: '%s' is a directory\n", name);
                    else if (ret == -7)  printf("save: name too long (max 63 chars)\n");
                    else if (ret == -4)  printf("save: volume full\n");
                    else                  printf("save: failed (%d)\n", ret);
                }
            } else {
                printf("save: usage: save <name> << \"text\"\n");
            }
        }
        else if (starts_with(input, "delfile ")) {
            /* 0.4 Beta: hapus berkas (RAMFS + FAT write-through). */
            sh_delfile(cwd, input + 8);
        }
        else if (starts_with(input, "rm ")) {
            /* 0.4 Beta: alias bash dari delfile (bukan rm rekursif —
             * untuk pohon, pakai Qfs del yang memakai rm_rf). */
            sh_delfile(cwd, input + 3);
        }
        else if (starts_with(input, "deldir ")) {
            /* 0.4 Beta: hapus direktori — kosong saja (fs_delete_node
             * + fat32_delete menolak non-empty dengan -4). */
            sh_deldir(cwd, input + 7);
        }
        else if (starts_with(input, "rmdir ")) {
            /* 0.4 Beta: alias bash dari deldir */
            sh_deldir(cwd, input + 6);
        }
        else if (starts_with(input, "pren ")) {
            /* 0.4 Beta — pren = preview .mrp (pasangan show untuk
             * biner; sebelumnya hanya xxd generik). Baca header
             * MRP1 + verifikasi checksum additive + hex dump 48
             * byte pertama. Read-only, aman untuk berkas apa pun
             * (magic salah -> dilaporkan, tidak pernah crash). */
            const char* name = input + 5;
            /* 0.4 Beta.2a: terima nama cwd-relative DAN path absolut
             * (/equinox/tools/x.mrp) seperti cat/showf/Qfs. */
            struct fs_node* node =
                (name[0] == '/') ? fs_get_node_from_path(fs_get_root(), name)
                                 : fs_find_child(cwd, name);
            if (!node) printf("pren: '%s' not found\n", name);
            else if (node->is_dir) printf("pren: '%s' is a directory\n", name);
            else if (fs_ensure_content(node) != 0 || !node->content ||
                     node->size < sizeof(struct mrp_header))
                printf("pren: '%s' bukan .mrp valid (%u bytes)\n",
                       name, (unsigned)node->size);
            else {
                struct mrp_header h;
                memcpy(&h, node->content, sizeof(h));
                int okm = (h.magic[0] == 'M' && h.magic[1] == 'R' &&
                           h.magic[2] == 'P' && h.magic[3] == '1');
                printf("pren: %s — %u bytes\n", name, (unsigned)node->size);
                printf("  magic   : %02x %02x %02x %02x %s\n",
                       h.magic[0], h.magic[1], h.magic[2], h.magic[3],
                       okm ? "(valid MRP1)" : "(INVALID)");
                printf("  version : %u  flags: 0x%02x%s\n", h.version,
                       h.flags,
                       (h.flags & MRP_FLAG_NEEDS_GUI) ? " (needs GUI)" : "");
                printf("  code    : %u bytes, entry +0x%x\n",
                       h.code_size, h.entry_offset);
                uint32_t lim = h.code_size;
                if (lim > node->size - (uint32_t)sizeof(h))
                    lim = node->size - (uint32_t)sizeof(h);
                /* 0.4 Beta.2a: pakai mrp_checksum() (rotate-xor, seed
                 * 0x811C9DC5) — algoritma persis loader & packer. */
                uint32_t sum = mrp_checksum(
                    (const uint8_t*)node->content + sizeof(h), lim);
                printf("  checksum: header 0x%08x, hitung 0x%08x %s\n",
                       h.checksum, sum,
                       (okm && sum == h.checksum) ? "(COCOK)" : "(BEDA)");
                uint32_t hx = node->size < 48 ? (uint32_t)node->size : 48;
                for (uint32_t i = 0; i < hx; i++) {
                    if ((i & 15) == 0) printf("\n  %04x: ", i);
                    printf("%02x ",
                           (unsigned char)node->content[i]);
                }
                printf("\n");
            }
        }
        else if (starts_with(input, "cat ") ||
                 starts_with(input, "showf ") ||
                 starts_with(input, "show ")) {
            /* 0.4 Beta: showf = nama baru builtin cat (desain eggkg §8);
             * cat tetap hidup sbg jendela transisi, dan keduanya
             * kena timpa oleh paket (cat.mrp) lewat system path. */
            const char* name = (input[3] == ' ') ? input + 4 : input + 6;
            /* 0.4 Beta.2a: terima path absolut (mis. /eqshell.log,
             * /mnt/equinox/conf/system.ecf) — dulu ditangani cat.mrp
             * yang pindah ke repo eggkg (paket bash). */
            struct fs_node* node =
                (name[0] == '/')
                    ? fs_get_node_from_path(fs_get_root(), name)
                    : fs_find_child(cwd, name);
            if (!node) {
                printf("cat: '%s' not found\n", name);
            } else if (node->is_dir) {
                printf("cat: '%s' is a directory\n", name);
            } else if (fs_ensure_content(node) != 0) {
                printf("cat: '%s' read error\n", name);
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
        else if (starts_with(input, "spawn ")) {
            /* Phase B: spawn <name.mrp> [args] — run the program in a
             * NEW task (non-blocking!). The shell returns to its
             * prompt immediately; the program's output appears on the
             * same console.
             * Example: spawn primes   (computes in the background) */
            const char* rest = input + 6;
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
                printf("spawn: empty file name (try: spawn hello.mrp)\n");
                continue;
            }
            /* resolve relative to cwd, then the system path (like run) */
            char full[300];
            shell_resolve_path(cwd, name, full, sizeof(full));
            struct fs_node* n = fs_get_node_from_path(fs_get_root(), full);
            /* Phase A.1: arena sizing policy lives in mrp_arena_hint_for */
            uint32_t hint = mrp_arena_hint_for(name);
            syscall_set_args(args);
            struct Task* child = (n && !n->is_dir && n->parent)
                ? task_create_user(NULL, n->parent, n->name, args, hint)
                : NULL;
            if (child) {
                printf("spawn: task pid %d ('%s') running in the background\n",
                       child->pid, n->name);
            } else {
                /* fallback: search the system path */
                int done = 0;
                for (int d = 0; d < SHELL_SYS_PATH_DIRS && !done; d++) {
                    struct fs_node* dir = shell_sys_path_dir(d);
                    if (!dir) continue;
                    char mrp_name[86];
                    snprintf(mrp_name, sizeof(mrp_name), "%s.mrp", name);
                    struct fs_node* cand = fs_find_child(dir, name);
                    if (!cand) cand = fs_find_child(dir, mrp_name);
                    if (cand && !cand->is_dir) {
                        child = task_create_user(NULL, cand->parent, cand->name, args, hint);
                        if (child) {
                            printf("spawn: task pid %d ('%s') running in the background\n",
                                   child->pid, cand->name);
                        }
                        done = 1;
                    }
                }
                if (!done) printf("spawn: '%s' not found\n", name);
            }
            syscall_set_args("");
        }
        else if (strcmp(input, "yield") == 0) {
            /* Phase B: give the CPU slice to other tasks (SYS_YIELD demo). */
            task_yield();
        }
        else if (strcmp(input, "ps") == 0) {
            /* Phase C: list every live task. */
            task_ps_dump();
        }
        else if (starts_with(input, "kill ")) {
            /* Phase C: kill <pid> — mark the task dead (async: the
             * scheduler cleans it up on the next tick). */
            const char* rest = input + 5;
            while (*rest == ' ') rest++;
            int pid = 0;
            int neg = 0;
            if (*rest == '-') { neg = 1; rest++; }
            while (*rest >= '0' && *rest <= '9') {
                pid = pid * 10 + (*rest - '0');
                rest++;
            }
            if (neg) pid = -pid;
            if (pid <= 0) {
                printf("kill: invalid pid (example: kill 3)\n");
                continue;
            }
            int r = task_kill(pid);
            if (r == 0)      printf("kill: pid %d marked dead (reaped by the scheduler)\n", pid);
            else if (r == -2) printf("kill: pid %d is THIS task (the shell) — use exit\n", pid);
            else              printf("kill: pid %d not found (try 'ps')\n", pid);
        }
        else if (starts_with(input, "switch ")) {
            /* Phase C: switch <n> — move the display to console n. */
            const char* rest = input + 7;
            while (*rest == ' ') rest++;
            int n = 0;
            while (*rest >= '0' && *rest <= '9') {
                n = n * 10 + (*rest - '0');
                rest++;
            }
            if (n < 0 || n >= console_count()) {
                printf("switch: console %d does not exist (0..%d)\n",
                       n, console_count() - 1);
                continue;
            }
            console_activate(n);
            printf("[console] active: %d (keyboard/console focus)\n", n);
        }
        else if (strcmp(input, "meminfo") == 0) {
            /* 0.4 Beta (FR-05): demand-paging accounting — pool
             * usage + per-task reserved/faulted footprint. */
            task_mem_dump();
        }
        else if (strcmp(input, "equinoxinstall") == 0 ||
                 starts_with(input, "equinoxinstall ")) {
            /* 0.4 Beta: clear screen + banner, then compile the
             * shipped tool AND game sources (.c) with the in-OS
             * mtcc, professional [info]/[warn]/[fail] logs, remove
             * the .c after a successful build.
             * `-compile <dir>` = only the [4/4] phase. */
            const char* eargs = input + 14;
            while (*eargs == ' ') eargs++;
            cmd_equinoxinstall(cwd, eargs);
        }
        else if (strcmp(input, "lspci") == 0) {
            /* 0.4 Beta (FR-12): dump the enumerated PCI table
             * (bus/dev/fn, vendor:device, class, IRQ, BARs). */
            pci_lspci();
        }
        else if (strcmp(input, "wait") == 0
                 || starts_with(input, "wait ")) {
            /* 0.4 Beta (FR-02): wait [pid] — block until a spawned
             * child exits, then report its exit status (like POSIX
             * waitpid). `wait` with no argument = any child. */
            const char* rest = input + 4;
            while (*rest == ' ') rest++;
            int pid = 0;
            while (*rest >= '0' && *rest <= '9') {
                pid = pid * 10 + (*rest - '0');
                rest++;
            }
            uint32_t status = 0;
            int r = task_wait_pid(pid, &status);
            if (r > 0) {
                printf("wait: child pid %d exited with status %u (0x%x)\n",
                       r, status, status);
            } else {
                printf("wait: no matching child (try 'spawn hello.mrp' first)\n");
            }
        }
        else if (starts_with(input, "run ")) {
            /* run <name.mrp> [args] — arguments are stored in the
             * syscall layer (SYS_GETARGS #15) so .mrp programs can
             * read them. Example: run tcc.mrp -c hello.c (mtcc).
             *
             * 0.4 Beta: <name> may be a SUBPATH ("equinox/games/snake.mrp",
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
                    mrp_run_hint(n->parent, n->name, mrp_arena_hint_for(n->name));
                    found = 1;
                }
            } else {
                struct fs_node* n = fs_find_child(cwd, name);
                if (n && !n->is_dir) {
                    mrp_run_hint(cwd, name, mrp_arena_hint_for(name));
                    found = 1;
                } else {
                    /* fallback: system path (mrp_run verbose, not
                     * quiet — the user explicitly asked to `run`) */
                    for (int d = 0; d < SHELL_SYS_PATH_DIRS && !found; d++) {
                        struct fs_node* dir = shell_sys_path_dir(d);
                        if (!dir) continue;
                        n = fs_find_child(dir, name);
                        if (n && !n->is_dir) {
                            mrp_run_hint(dir, name, mrp_arena_hint_for(name));
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
        // 0.4 Beta: subpaths now SUPPORTED ("./equinox/games/snake.mrp"
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
                mrp_run_hint(n->parent, n->name, mrp_arena_hint_for(n->name));
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
            else if (ret == -9) printf("rm: '%s' is a mounted volume (umount first)\n", name);
            else if (ret != 0) printf("rm: failed (%d)\n", ret);
        }
        else if (starts_with(input, "rmdir ")) {
            const char* name = input + 6;
            int ret = fs_delete_node(cwd, name);
            if (ret == -2) printf("rmdir: '%s' not found\n", name);
            else if (ret == -4) printf("rmdir: directory not empty\n");
            else if (ret == -9) printf("rmdir: '%s' is a mounted volume (umount first)\n", name);
            else if (ret != 0) printf("rmdir: failed (%d)\n", ret);
        }
        else if (strcmp(input, "set") == 0 || starts_with(input, "set ")) {
            /* 0.4 Beta: builtin .ecf — `set`, `set KEY [VAL]`,
             * `set -a file`, `set -w file`. */
            shell_cmd_set(cwd, input + 3);
        }
        else if (strcmp(input, "eqgu") == 0 || starts_with(input, "eqgu ")) {
            /* 0.4 Beta: editor + cek-kompilasi mtcc saat Ctrl+S save.
             * Hanya berkas .c; hook dipasang selama editor terbuka. */
            const char* arg = input + 4;
            while (*arg == ' ') arg++;
            if (!arg[0]) {
                printf("eqgu: butuh nama berkas .c (eqgu main.c)\n");
            } else {
                char fullpath[300];
                shell_resolve_path(cwd, arg, fullpath, sizeof(fullpath));
                if (!eqgu_is_c(fullpath)) {
                    printf("eqgu: hanya berkas .c — %s\n", fullpath);
                } else {
                    editor_set_check_fn(eqgu_check);
                    editor_open(fullpath);
                    editor_set_check_fn(NULL);
                    clear_screen();
                    printf("Editor closed.\n");
                }
            }
        }
        else if (starts_with(input, "edit ")) {
            const char* filename = input + 5;
            /* 0.4 Beta: shell_resolve_path() replaces the manual FIX(K4)
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
            /* 0.4 Beta: queue-based playback. The 14 notes go into the
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
            /* 0.4 Beta: one-stop view of the three kernel ring buffers.
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
            /* 0.4 Beta: current privilege level of whoever runs this.
             * The shell lives in the kernel = ring 0; .mrp programs
             * run at ring 3. CPL is simply CS & 3 — read the live
             * segment registers straight from asm. */
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
            /* 0.4 Beta: memory map + ring-3 status — a quick way to
             * confirm paging & TSS are alive before running user
             * programs. */
            usermode_print_memmap();
        }
        else if (strcmp(input, "doom") == 0 || starts_with(input, "doom ")) {
            /* 0.4 Beta: DOOM one-liner. Finds /equinox/games/doom.mrp (ISO
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
            /* 0.4 Beta: always make sure -iwad is present (mirroring
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
                /* Phase A.1 DOOM regression fix: run with the DOOM-sized
                 * arena hint (24 MB) — the flat 2 MB default could not
                 * hold the WAD. */
                mrp_run_hint(gdir, "doom.mrp", MRP_ARENA_DOOM);
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
            /* 0.4 Beta FR-23: -k / --insecure opts into the warned
             * "encrypted but NOT verified" TLS fallback (fail-closed
             * stays the DEFAULT). Strip the flag from the args
             * net_cmd_mget sees. */
            char margs[160];
            const char* src = input[4] == ' ' ? input + 5 : "";
            uint32_t o = 0, q = 0;
            int insecure = 0;
            while (src[q] && o + 1 < sizeof(margs)) {
                if (src[q] == ' ') { q++; continue; }
                if (src[q] == '-') {
                    char tok[24];
                    uint32_t t = 0;
                    while (src[q] && src[q] != ' ' && t < sizeof(tok) - 1)
                        tok[t++] = src[q++];
                    tok[t] = '\0';
                    if (strcmp(tok, "-k") == 0 ||
                        strcmp(tok, "--insecure") == 0) {
                        insecure = 1;
                    } else if (o + t + 1 < sizeof(margs)) {
                        for (uint32_t k = 0; k < t; k++) margs[o++] = tok[k];
                    } else break;
                    continue;
                }
                margs[o++] = src[q++];
            }
            margs[o] = '\0';
            if (insecure) {
                printf("mget: -k — TLS verification failures downgrade to "
                       "ENCRYPTED but UNVERIFIED\n");
            }
            tls_set_insecure(insecure);
            net_cmd_mget(margs, cwd);
            tls_set_insecure(0);
        }
        else if (starts_with(input, "eggkg ") ||
                 strcmp(input, "eggkg") == 0) {
            /* 0.4 Beta — package manager (builtin; jaringan + spawn mtcc
             * tinggal di ring 0). args = setelah kata "eggkg". */
            eggkg_cmd(input[5] ? input + 5 : "");
        }
        else if (strcmp(input, "nettask") == 0) {
            net_dbg_dump();
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
        else if (strcmp(input, "desktop") == 0) {
            /* 0.4 Beta: desktop EquiX — restore dispatch sesi sebelumnya
             * (hilang saat clone project gagal). desktop_run sendiri
             * clear_screen + print status saat kembali. */
            equix::desktop_run();
        }
        else if (strcmp(input, "tvgdemo") == 0) {
            cmd_tvgdemo();
        }
        else if (strcmp(input, "tvgbench") == 0) {
            cmd_tvgbench();
        }
        else if (strcmp(input, "tvginfo") == 0) {
            cmd_tvginfo();
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
                    printf("Unknown command: '%s'.\n", input);


        /* 0.4 Beta SAFETY NET: after EVERY command, make sure a storage
         * path did not leak the sched lock (would starve the nettask
         * and every sleeping task). Detected -> force-release + warn. */
        task_sched_force_unlock();                }
            }
        }
    }
}

// ============================================================
//  KERNEL MAIN — full boot sequence
// ============================================================

// Forward declaration: mrp_bootloader.cpp (loads GRUB multiboot modules into the RAMFS)
extern "C" int mrp_bootloader_load_modules(const multiboot_info_t* mb_info);

