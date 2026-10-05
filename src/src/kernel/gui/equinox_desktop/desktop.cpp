/**
 * @file desktop.cpp
 * @brief EquiX DE — layer cache + dirty rect (anti-lag), dynamic GUI
 *        arena (no fixed 10 MB), monochrome theme, Calculator.
 *
 * ======================= FIX "TASKBAR BLOCKED" =======================
 * Symptom: "touching the bottom taskbar area always feels blocked".
 * Empirical diagnosis (scripts/probe_taskbar.py, QEMU):
 *   (1) The taskbar HOVER zone was only the 30px button rect
 *       [H-th+7, H-th+37] while the CLICK zone was the full taskbar
 *       height [H-th, H). Moving the cursor down to a taskbar row
 *       (y 724..730 / 762..767 — exactly where users reach for the
 *       bottom taskbar) left the buttons unlit, so the desktop felt
 *       dead/"blocked" even though the click went through.
 *       FIX: hover zone = click zone = full taskbar height.
 *   (2) The hotspot marker drawn when a sprite is edge-clamped used
 *       colour 0xFF101018 — invisible against the black gray(16)
 *       taskbar, so the true click point (up to 15px below the
 *       sprite) could never be seen. FIX: white 3px caret.
 *   (3) A press+release landing between two poll loops (~16-20 ms) could
 *       be swallowed by mouse_get_state (only the LAST packet mask is
 *       stored). FIX: press/release edge counters in the IRQ context
 *       (ps2_mouse.cpp) — clicks can no longer be lost.
 *
 * ======================= FIX HEAVY LAG =================================
 * Symptom: "the lag is terrible" — every mouse move triggered:
 *   (1) a FULL scene rebuild (hundreds of Shapes via kernel malloc),
 *   (2) FULL-SCREEN 1360x768 ThorVG rasterisation (compositing + AA),
 *   (3) per-pixel text_blit of EVERY label,
 *   (4) a full 4 MB flip to the LFB.
 * On QEMU TCG one frame took hundreds of ms, so the desktop looked dead.
 *
 * Architecture — "layer cache + dirty rect":
 *   L_bg   : wallpaper + taskbar chrome  — ThorVG ONCE at startup.
 *   L_win  : 2 variants per window (focused/unfocused) — ThorVG ONCE
 *            when the window opens; static text is blitted into the layer.
 *   backbuf: composition target (fast RAM).
 *   compose(region): copy L_bg -> copy L_win (alpha fast-path) ->
 *            dynamic elements (taskbar buttons, clock, launcher, calc
 *            display, hover) -> cursor — ONLY inside the dirty region.
 *   blit(region): ONE region copy (rep movsl per row) to the LFB.
 *   Event-driven: idle = zero blits; cursor motion = ~1 KB of traffic.
 *   ThorVG is never called again after the first frame (unless a NEW
 *   window opens — rare).
 *
 * ======================= FIX "10 MB RAM" ==============================
 * The GUI arena is now DYNAMIC 0x3400000..RAM-top (paging.cpp maps
 * 64..128 MB according to the real RAM; kernel.cpp reads the multiboot
 * mem_upper). RAM 128 MB -> arena 76 MB (no longer 10 MB).
 *
 * ======================= FIX MOUSE "BLOCKED" ==========================
 * The driver clamps the cursor to 0..H-1 (FULL screen, taskbar
 * included). With instant rendering, moving to the bottom of the screen
 * is visible immediately. One-time serial trace the first time y == H-1:
 *   "[equix] cursor mencapai dasar layar (y=767)"
 *
 * Theme: monochrome black/grey/white (R=G=B on every channel).
 * Input: PS/2 mouse (mouse_get_state) + getkey_poll (ESC to quit,
 * keyboard for the Calculator). Text: font8x16. Clock: RTC CMOS.
 */
#include "thorvg.h"
#include "config.h"
#include "header/vesa.h"
#include "header/stdio.h"
#include "header/guiarena.h"
#include "header/timer.h"
#include "header/task.h"
#include "header/ps2_mouse.h"
#include "header/font8x16.h"
#include "header/libstring.h"   /* snprintf */
#include "header/serial.h"      /* test-hook calculator (tanpa noda layar) */
#include "header/math2.h"       /* sqrt/floor/fmod (x87) */
#include "header/fs_ram.h"      /* 0.4 Beta: File Manager (RAMFS + FAT32) */
#include "header/fs_fat32.h"    /* fat32_populate_dir (mirror lazy /mnt) */

using namespace tvg;

namespace equix {

/* ------------------------------------------------------------------ */
/*  Konstanta                                                          */
/* ------------------------------------------------------------------ */
static const float TASKBAR_H = 44.0f;
static const float TITLE_H   = 32.0f;

/* ------------------------------------------------------------------ */
/*  Tema — MONOKROM: hitam / abu-abu / putih                           */
/*  (semua channel R=G=B; hanya level abu yang berbeda-beda)           */
/* ------------------------------------------------------------------ */
enum {
    G_WALL_A = 6,    /* wallpaper: hitam pekat (kiri-atas)      */
    G_WALL_B = 30,   /* wallpaper: charcoal                     */
    G_WALL_C = 62,   /* wallpaper: abu gelap                    */
    G_WALL_D = 110,  /* wallpaper: abu medium (kanan-bawah)     */
    G_BAR    = 16,   /* taskbar: hitam                          */
    G_BAR_LN = 70,   /* garis separator atas taskbar            */
    G_MENU   = 236,  /* tombol MENU: putih gading               */
    G_MENU_H = 255,  /* tombol MENU hover                       */
    G_TASK   = 56,   /* tombol taskbar idle                     */
    G_TASK_H = 88,   /* tombol taskbar hover                    */
    G_TASK_A = 198,  /* tombol taskbar jendela aktif (terang)   */
    G_CLOCK  = 26,   /* panel jam                               */
    G_BODY   = 240,  /* body jendela: putih (OPAQUE di layer)   */
    G_TTL_AF = 208,  /* titlebar AKTIF gradasi awal (abu terang)*/
    G_TTL_AT = 128,  /* titlebar AKTIF gradasi akhir            */
    G_TTL_UF = 88,   /* titlebar tak-fokus awal                 */
    G_TTL_UT = 52,   /* titlebar tak-fokus akhir                */
    G_CLOSE_F = 35,  /* tombol close (aktif): lingkaran gelap   */
    G_MINI_F  = 135, /* tombol minimize (aktif): lingkaran abu  */
    G_CLOSE_U = 74,  /* tombol close (tak-fokus)                */
    G_MINI_U  = 100, /* tombol minimize (tak-fokus)             */
    G_LAUNCH  = 28,  /* background launcher                     */
    G_LAUNCH_H = 138,/* baris launcher hover                    */
    /* kalkulator */
    G_CALC_DISP = 16,  /* panel display: hitam LCD             */
    G_CALC_DIG  = 43,  /* tombol angka: abu gelap              */
    G_CALC_DIG_H = 62,
    G_CALC_OP   = 86,  /* tombol operator: abu                 */
    G_CALC_OP_H = 105,
    G_CALC_EQ   = 236, /* tombol = : putih                     */
    G_CALC_EQ_H = 255,
    G_CALC_CLR  = 130, /* tombol C: abu terang                 */
    G_CALC_CLR_H = 150,
    G_CALC_UTIL = 70,  /* +/- % . sqrt: abu gelap-medium       */
    G_CALC_UTIL_H = 92,
};

static inline uint32_t gray(uint32_t g) { return 0xFF000000u | (g << 16) | (g << 8) | g; }

/* ------------------------------------------------------------------ */
/*  Window state                                                       */
/* ------------------------------------------------------------------ */
enum AppId { APP_ABOUT = 0, APP_NOTES, APP_CALC, APP_FM, APP_TERM, APP_COUNT };

struct Win {
    bool    open;
    bool    minimized;
    float   x, y, w, h;
    int     z;
};

static Win wins[APP_COUNT];
static int  top_z = 1;
static int  focus_idx = -1;

static bool menu_open = false;
static int  click_count = 0;

static const char* APP_NAMES[APP_COUNT] = {
    "About EquiX", "Notes", "Calculator", "Files", "Terminal"
};

/* ukuran + posisi awal per app */
/* Terminal = 640x416: 80 kolom x 8 px + titlebar 32 + 24 baris x 16 px */
static const float APP_W[APP_COUNT] = { 420, 420, 292, 760, 640 };
static const float APP_H[APP_COUNT] = { 250, 250, 400, 460, 416 };
static const float APP_X[APP_COUNT] = { 90, 620, 980, 230, 400 };
/* Terminal di y=96 (bukan 100) supaya isi jendela mulai di y=128 =
 * kelipatan 16 — sejajar grid glyph 8x16, sehingga isi terminal terbaca
 * bersih oleh OCR harness (dan x=400 sudah kelipatan 8). */
static const float APP_Y[APP_COUNT] = { 64, 150, 110, 52, 96 };

/* ================================================================== */
/*  LAYER CACHE — semua surface di arena GUI (dinamis, bukan 10 MB)    */
/* ================================================================== */
static uint32_t* g_lfb = nullptr;       /* linear framebuffer VESA     */
static uint32_t  g_lfb_stride = 0;      /* dalam piksel                */
static uint32_t* g_bb = nullptr;        /* backbuffer = target compose */
static uint32_t  g_bb_stride = 0;       /* = W                         */
static uint32_t  g_W = 0, g_H = 0;

static uint32_t* L_bg = nullptr;        /* wallpaper + chrome taskbar  */

/* layer jendela: 2 varian — [0] tak-fokus, [1] fokus */
static uint32_t* L_win[APP_COUNT][2] = {};
static uint32_t  L_win_stride[APP_COUNT] = {};   /* = lebar jendela (px) */

/* ------------------------------------------------------------------ */
/*  Dirty rect — SATU union rect per frame + clip komposisi            */
/* ------------------------------------------------------------------ */
static int g_dr_x0 = 0x7FFFFFFF, g_dr_y0 = 0x7FFFFFFF;
static int g_dr_x1 = -1, g_dr_y1 = -1;  /* exclusive */

static void mark_dirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < g_dr_x0) g_dr_x0 = x;
    if (y < g_dr_y0) g_dr_y0 = y;
    if (x1 > g_dr_x1) g_dr_x1 = x1;
    if (y1 > g_dr_y1) g_dr_y1 = y1;
}

/* clip aktif saat compose (semua helper gambar menghormatinya) */
static int g_cl_x0 = 0, g_cl_y0 = 0, g_cl_x1 = 0, g_cl_y1 = 0;

/* ------------------------------------------------------------------ */
/*  Statistik sesi (bukti anti-lag — diprint saat exit)                */
/* ------------------------------------------------------------------ */
static uint32_t st_frames = 0;
static uint32_t st_blit_px = 0;

/* piksel ke backbuffer (bounds + clip-checked) */
static inline void bb_pixel(int x, int y, uint32_t rgb) {
    if (x < g_cl_x0 || y < g_cl_y0 || x >= g_cl_x1 || y >= g_cl_y1) return;
    if (x < 0 || y < 0 || (uint32_t)x >= g_W || (uint32_t)y >= g_H) return;
    g_bb[(size_t)y * g_bb_stride + (size_t)x] = rgb | 0xFF000000u;
}

/* ---- fill rect cepat: per-baris tulis word beruntun ---- */
static void bb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    int cx0 = x > g_cl_x0 ? x : g_cl_x0;
    int cy0 = y > g_cl_y0 ? y : g_cl_y0;
    int cx1 = x + w < g_cl_x1 ? x + w : g_cl_x1;
    int cy1 = y + h < g_cl_y1 ? y + h : g_cl_y1;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if ((uint32_t)cx1 > g_W) cx1 = (int)g_W;
    if ((uint32_t)cy1 > g_H) cy1 = (int)g_H;
    if (cx0 >= cx1 || cy0 >= cy1) return;

    uint32_t v = rgb | 0xFF000000u;
    for (int yy = cy0; yy < cy1; yy++) {
        uint32_t* row = g_bb + (size_t)yy * g_bb_stride + (size_t)cx0;
        uint32_t* end = row + (cx1 - cx0);
        while (row + 8 <= end) {
            row[0] = v; row[1] = v; row[2] = v; row[3] = v;
            row[4] = v; row[5] = v; row[6] = v; row[7] = v;
            row += 8;
        }
        while (row < end) *row++ = v;
    }
}

/* ------------------------------------------------------------------ */
/*  Rounded-rect fill r=6 — inset per baris (tombol dinamis/hover)    */
/* ------------------------------------------------------------------ */
static void bb_fill_round(int x, int y, int w, int h, uint32_t rgb) {
    /* inset kiri/kanan utk 6 baris pertama & terakhir (kurva r=6) */
    static const int ins[6] = {2, 1, 1, 0, 0, 0};
    if (w <= 0 || h <= 0) return;

    for (int r = 0; r < h; r++) {
        int d = (r < 6) ? ins[r] : ((r >= h - 6 && h >= 12) ? ins[h - 1 - r] : 0);
        int xx = x + d, ww = w - 2 * d;
        if (ww <= 0) continue;
        /* clip vertikal cepat */
        int py = y + r;
        if (py < g_cl_y0 || py >= g_cl_y1) continue;
        if (py < 0 || (uint32_t)py >= g_H) continue;
        /* fill baris via bb_fill_rect (sudah clip horizontal) */
        bb_fill_rect(xx, py, ww, 1, rgb);
    }
}

/* ------------------------------------------------------------------ */
/*  copy region antar-buffer — inti compose & blit                     */
/* ------------------------------------------------------------------ */
static void copy_region(uint32_t* dst, uint32_t dst_stride,
                        const uint32_t* src, uint32_t src_stride,
                        int sx, int sy, int w, int h,
                        int dx, int dy, int use_alpha) {
    /* clip ke g_clip (compose) atau layar penuh (blit — clip g_clip
     * di-set = region blit saat memanggil) */
    if (dx < g_cl_x0) { int d = g_cl_x0 - dx; dx += d; sx += d; w -= d; }
    if (dy < g_cl_y0) { int d = g_cl_y0 - dy; dy += d; sy += d; h -= d; }
    if (dx + w > g_cl_x1) w = g_cl_x1 - dx;
    if (dy + h > g_cl_y1) h = g_cl_y1 - dy;
    if (w <= 0 || h <= 0) return;

    if (!use_alpha) {
        /* raw: rep movsl per baris — jalur tercepat */
        for (int r = 0; r < h; r++) {
            uint32_t* d = dst + (size_t)(dy + r) * dst_stride + (size_t)dx;
            const uint32_t* s = src + (size_t)(sy + r) * src_stride + (size_t)sx;
            uint32_t n = (uint32_t)w;
            asm volatile("cld; rep movsl"
                         : "+D"(d), "+S"(s), "+c"(n)
                         :
                         : "memory");
        }
        return;
    }

    /* alpha fast-path: 255 = copy langsung, 0 = skip, else blend */
    for (int r = 0; r < h; r++) {
        uint32_t* d = dst + (size_t)(dy + r) * dst_stride + (size_t)dx;
        const uint32_t* s = src + (size_t)(sy + r) * src_stride + (size_t)sx;
        for (int i = 0; i < w; i++) {
            uint32_t p = s[i];
            uint32_t a = p >> 24;
            if (a == 255) { d[i] = 0xFF000000u | (p & 0xFFFFFF); continue; }
            if (a == 0)   { continue; }
            uint32_t q = d[i];
            uint32_t rb = ((p & 0x00FF00FF) * a + (q & 0x00FF00FF) * (255 - a) + 0x00FF00FF) >> 8;
            uint32_t g  = ((p & 0x0000FF00) * a + (q & 0x0000FF00) * (255 - a) + 0x0000FF00) >> 8;
            d[i] = 0xFF000000u | (rb & 0x00FF00FF) | (g & 0x0000FF00);
        }
    }
}

/* blit region dirty dari backbuffer ke LFB (clip = dirty rect) */
static void blit_dirty(int x, int y, int w, int h) {
    copy_region(g_lfb, g_lfb_stride, g_bb, g_bb_stride,
                x, y, w, h, x, y, 0);
    st_blit_px += (uint32_t)(w * h);
}

/* ------------------------------------------------------------------ */
/*  Teks font8x16 — ke buffer APA PUN (backbuffer / layer)             */
/* ------------------------------------------------------------------ */
static void text_blit_buf(uint32_t* buf, uint32_t stride, uint32_t bw,
                          uint32_t bh, int x, int y, const char* s,
                          uint32_t rgb, int use_clip) {
    while (*s) {
        uint8_t ch = (uint8_t)*s++;
        if (ch >= 128) ch = '?';
        for (int gy = 0; gy < 16; gy++) {
            uint8_t line = font8x16[ch][gy];
            if (!line) continue;
            int py = y + gy;
            if (py < 0 || (uint32_t)py >= bh) continue;
            if (use_clip && (py < g_cl_y0 || py >= g_cl_y1)) continue;
            uint32_t* row = buf + (size_t)py * stride;
            for (int gx = 0; gx < 8; gx++) {
                if (line & (1 << (7 - gx))) {
                    int px = x + gx;
                    if (px < 0 || (uint32_t)px >= bw) continue;
                    if (use_clip && (px < g_cl_x0 || px >= g_cl_x1)) continue;
                    row[px] = 0xFF000000u | (rgb & 0xFFFFFF);
                }
            }
        }
        x += 8;
    }
}

static void text_blit(int x, int y, const char* s, uint32_t rgb) {
    text_blit_buf(g_bb, g_bb_stride, g_W, g_H, x, y, s, rgb, 1);
}

/* ------------------------------------------------------------------ */
/*  Kursor mouse — sprite 16x16 precomputed                            */
/* ------------------------------------------------------------------ */
static uint32_t cur_sprite[16][16];     /* 0 = transparan */
static int cur_bottom_reported = 0;

static void cursor_init(void) {
    static const char* ARROW[16] = {
        "X               ",
        "XX              ",
        "X.X             ",
        "X..X            ",
        "X...X           ",
        "X....X          ",
        "X.....X         ",
        "X......X        ",
        "X.......X       ",
        "X........X      ",
        "X.....XXXX      ",
        "X..X..X         ",
        "X.X X..X        ",
        "XX  X..X        ",
        "X    X..X       ",
        "     XX         ",
    };
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            char c = ARROW[y][x];
            cur_sprite[y][x] = (c == 'X') ? 0xFF101018u
                            : (c == '.') ? 0xFFFFFFFFu
                                         : 0;
        }
}

/* hitung posisi GAMBAR sprite (edge-clamped) — dipakai draw_cursor
 * DAN mark-dirty agar region kotor selalu = area sprite sebenarnya */
static void cursor_clamp(int x, int y, int* dx, int* dy) {
    *dx = x; *dy = y;
    if (*dx > (int)g_W - 16) *dx = (int)g_W - 16;
    if (*dy > (int)g_H - 16) *dy = (int)g_H - 16;
    if (*dx < 0) *dx = 0;
    if (*dy < 0) *dy = 0;
}

static void draw_cursor(int mx, int my) {
    /* 0.4 Beta FIX "MOUSE GABISA KEBAWAH / KAYA KEBLOCK":
     * hotspot kursor = piksel (mx,my) kiri-atas. Saat mouse mentok
     * di dasar/tepi layar (y=767 / x=1359), sprite 16x16 lama TUMUS
     * seluruhnya di luar layar -> kursor HILANG dari pandangan ->
     * terasa "mouse-nya di-block". FIX: posisi GAMBAR digeser agar
     * sprite selalu utuh (klasik "edge clamp"), sedangkan posisi
     * HIT-TEST tetap memakai (mx,my) asli — taskbar/tombol di baris
     * paling bawah tetap bisa diklik dengan tepat. */
    int dx, dy;
    cursor_clamp(mx, my, &dx, &dy);
    if (mx >= 0 && mx < (int)g_W && my >= 0 && my < (int)g_H &&
        (dx != mx || dy != my)) {
        /* 0.4 Beta: penanda hotspot PUTIH (dulu 0xFF101018 — nyaris identik
         * dengan taskbar hitam gray(16) = TIDAK KELIHATAN). Caret 3px:
         * titik klik sejati selalu terlihat saat sprite digeser
         * edge-clamp (bb_pixel sudah clip ke layar, baris terakhir aman). */
        bb_pixel(mx, my, 0xFFFFFFFFu);
        bb_pixel(mx, my + 1, 0xFFFFFFFFu);
        bb_pixel(mx + 1, my, 0xFFFFFFFFu);
    }
    for (int y = 0; y < 16; y++) {
        int py = dy + y;
        if (py < g_cl_y0 || py >= g_cl_y1) continue;
        if (py < 0 || (uint32_t)py >= g_H) continue;
        const uint32_t* srow = cur_sprite[y];
        uint32_t* row = g_bb + (size_t)py * g_bb_stride;
        for (int x = 0; x < 16; x++) {
            uint32_t c = srow[x];
            if (!c) continue;
            int px = dx + x;
            if (px < g_cl_x0 || px >= g_cl_x1) continue;
            if (px < 0 || (uint32_t)px >= g_W) continue;
            row[px] = c;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  RTC                                                                */
/* ------------------------------------------------------------------ */
static void rtc_read(int* h, int* m, int* s) {
    auto cmos = [](uint8_t reg) -> uint8_t {
        outb(0x70, reg);
        return inb(0x71);
    };
    for (int t = 0; t < 30 && (cmos(0x0A) & 0x80); t++) { }
    uint8_t rb = cmos(0x0B);
    int bcd = !(rb & 0x04);
    *h = cmos(0x04); *m = cmos(0x02); *s = cmos(0x00);
    if (bcd) {
        auto bcd2 = [](int v) { return (v & 0x0F) + ((v >> 4) * 10); };
        *h = bcd2(*h); *m = bcd2(*m); *s = bcd2(*s);
    }
    if (*h > 23 || *m > 59 || *s > 59) { *h = 12; *m = 0; *s = 0; }
}

/* ================================================================== */
/*  CALCULATOR — state & logic (identik 0.4 Beta + input keyboard)        */
/* ================================================================== */
struct CalcState {
    double acc;      /* akumulator                                   */
    double val;      /* entry / nilai yang ditampilkan               */
    char   op;       /* operator pending: 0, '+', '-', '*', '/'      */
    bool   fresh;    /* digit berikutnya memulai entry baru          */
    bool   dot;      /* titik desimal sudah diketik                  */
    double scale;    /* 0.1^n utk digit setelah koma                 */
    int    ndig;     /* jumlah digit entry                           */
    bool   err;      /* error state (div-0, sqrt negatif, overflow)  */
};
static CalcState calc;

static void calc_reset(void) {
    calc.acc = 0; calc.val = 0; calc.op = 0; calc.fresh = true;
    calc.dot = false; calc.scale = 1; calc.ndig = 0; calc.err = false;
}

/* format double -> string (tanpa %f di libc kernel):
 * 12 digit signifikan, maks 10 digit pecahan, trailing-zero
 * dipangkas, pembulatan carry ditangani (0.999.. -> 1). */
static void calc_fmt(double v, char* out, int cap) {
    if (cap < 2) { if (cap > 0) out[0] = 0; return; }
    if (isnan(v) || isinf(v) || v > 9.999e14 || v < -9.999e14) {
        snprintf(out, cap, "Error");
        return;
    }
    if (v == 0.0) { snprintf(out, cap, "0"); return; }  /* termasuk -0 */

    bool neg = (v < 0);
    double a = neg ? -v : v;

    char buf[40]; int n = 0;
    if (neg) buf[n++] = '-';

    /* bagian integer (digit demi digit via fmod — aman utk 2^53) */
    char ipr[20]; int ipn = 0;
    double ip = floor(a);
    double t = ip;
    while (t >= 1.0 && ipn < 19) {
        ipr[ipn++] = (char)('0' + (int)fmod(t, 10.0));
        t = floor(t / 10.0);
    }
    if (ipn == 0) buf[n++] = '0';
    else for (int i = ipn - 1; i >= 0; i--) buf[n++] = ipr[i];

    /* bagian pecahan */
    int fdig = 12 - ipn;
    if (fdig > 10) fdig = 10;
    if (fdig < 0) fdig = 0;
    if (fdig > 0) {
        double fr = a - ip;
        char fb[12];
        for (int i = 0; i < fdig; i++) {
            fr *= 10;
            int d = (int)fr;
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            fb[i] = (char)('0' + d);
            fr -= d;
        }
        if (fr >= 0.5) {                      /* pembulatan ke atas */
            int i = fdig - 1;
            while (i >= 0 && fb[i] == '9') fb[i--] = '0';
            if (i < 0) {                      /* carry penuh: 0.99.. -> 1 */
                calc_fmt(neg ? -(ip + 1.0) : (ip + 1.0), out, cap);
                return;
            }
            fb[i]++;
        }
        int last = fdig;
        while (last > 0 && fb[last - 1] == '0') last--;
        if (last > 0) {
            buf[n++] = '.';
            for (int i = 0; i < last; i++) buf[n++] = fb[i];
        }
    }
    buf[n] = 0;

    int j = 0;
    while (j < cap - 1 && buf[j]) { out[j] = buf[j]; j++; }
    out[j] = 0;
}

static double calc_apply_op(double a, char op, double b, bool* err) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/':
        if (b == 0.0) { *err = true; return 0.0; }
        return a / b;
    }
    return b;
}

/* test-hook: state kalkulator ke SERIAL (bukan console — tidak
 * menodai layar; boot_test_equix.py memverifikasi dari sini). */
static void calc_trace(const char* key) {
    char num[40];
    char line[72];
    if (calc.err) snprintf(num, sizeof(num), "Error");
    else calc_fmt(calc.val, num, (int)sizeof(num));
    snprintf(line, sizeof(line), "[equix:calc] %s -> %s\n", key, num);
    serial_puts(line);
}

static void calc_key(char k) {
    if (calc.err) {
        if (k == 'C') calc_reset();       /* hanya C yang keluar dr Error */
        calc_trace(k == 'C' ? "C(err)" : "ign");
        return;
    }

    if (k >= '0' && k <= '9') {
        if (calc.fresh) {
            calc.val = 0; calc.dot = false; calc.scale = 1;
            calc.ndig = 0; calc.fresh = false;
        }
        if (calc.ndig < 12) {
            double d = (double)(k - '0');
            if (calc.dot) { calc.scale *= 0.1; calc.val += d * calc.scale; }
            else calc.val = calc.val * 10 + d;
            calc.ndig++;
        }
    } else if (k == '.') {
        if (calc.fresh) {
            calc.val = 0; calc.dot = false; calc.scale = 1;
            calc.ndig = 0; calc.fresh = false;
        }
        if (!calc.dot) calc.dot = true;
    } else if (k == 'C') {
        calc_reset();
    } else if (k == 'n') {                 /* +/- */
        calc.val = -calc.val;
    } else if (k == '%') {
        calc.val = calc.val / 100.0;
    } else if (k == 's') {                 /* sqrt */
        if (calc.val < 0) { calc.err = true; }
        else calc.val = sqrt(calc.val);
    } else if (k == '+' || k == '-' || k == '*' || k == '/') {
        if (calc.op && !calc.fresh) {
            bool err = false;
            calc.acc = calc_apply_op(calc.acc, calc.op, calc.val, &err);
            if (err) { calc.err = true; }
            else { calc.val = calc.acc; }
        } else if (!calc.op) {
            calc.acc = calc.val;
        }
        /* op && fresh -> operator diganti saja */
        if (!calc.err) { calc.op = k; calc.fresh = true; }
    } else if (k == '=') {
        if (calc.op) {
            bool err = false;
            double b = calc.fresh ? calc.acc : calc.val;  /* repeat-equals */
            calc.acc = calc_apply_op(calc.acc, calc.op, b, &err);
            if (err) { calc.err = true; }
            else { calc.val = calc.acc; }
            calc.op = 0;
        }
        calc.fresh = true;
    }

    /* label utk trace */
    const char* lbl;
    static char kbuf[2];
    if (k == 'n') lbl = "+/-";
    else if (k == 's') lbl = "sqrt";
    else if (k == 'C') lbl = "C";
    else { kbuf[0] = k; kbuf[1] = 0; lbl = kbuf; }
    calc_trace(lbl);
}

/* layout grid kalkulator — dipakai layer render DAN hit-test */
static const float CALC_M = 14, CALC_GAP = 8, CALC_DISP_H = 60;

static void calc_grid(const Win& w, int row, int col,
                      float* x, float* y, float* bw, float* bh) {
    float bwid = (w.w - 2 * CALC_M - 3 * CALC_GAP) / 4.0f;
    float bhei = (w.h - TITLE_H - 2 * CALC_M - CALC_DISP_H - CALC_GAP
                  - 4 * CALC_GAP) / 5.0f;
    *x = w.x + CALC_M + col * (bwid + CALC_GAP);
    *y = w.y + TITLE_H + CALC_M + CALC_DISP_H + CALC_GAP + row * (bhei + CALC_GAP);
    *bw = bwid;
    *bh = bhei;
}

/* key & label & kind per tombol (kind: 0 digit, 1 op, 2 '=', 3 C, 4 util) */
static const char CALC_KEYS[5][4] = {
    { 'C', 'n', '%', '/' },
    { '7', '8', '9', '*' },
    { '4', '5', '6', '-' },
    { '1', '2', '3', '+' },
    { 's', '0', '.', '=' },
};
static const char* CALC_LBL[5][4] = {
    { "C", "+/-", "%", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "sqrt", "0", ".", "=" },
};
static const uint8_t CALC_KIND[5][4] = {
    { 3, 4, 4, 1 },
    { 0, 0, 0, 1 },
    { 0, 0, 0, 1 },
    { 0, 0, 0, 1 },
    { 1, 0, 0, 2 },
};

/* warna dasar tombol per kind (hover = lebih terang) */
static uint8_t calc_btn_base(uint8_t kind) {
    switch (kind) {
    case 0: return G_CALC_DIG;
    case 1: return G_CALC_OP;
    case 2: return G_CALC_EQ;
    case 3: return G_CALC_CLR;
    default: return G_CALC_UTIL;
    }
}
static uint8_t calc_btn_hover(uint8_t kind) {
    switch (kind) {
    case 0: return G_CALC_DIG_H;
    case 1: return G_CALC_OP_H;
    case 2: return G_CALC_EQ_H;
    case 3: return G_CALC_CLR_H;
    default: return G_CALC_UTIL_H;
    }
}

/* ================================================================== */
/*  RENDER LAYER (ThorVG) — dipanggil SEKALI per surface               */
/* ================================================================== */
static Shape* rect_sharp(float x, float y, float w, float h,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Shape* s = Shape::gen();
    s->appendRect(x, y, w, h);
    s->fill(r, g, b, a);
    return s;
}

static Shape* rect_round(float x, float y, float w, float h,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Shape* s = Shape::gen();
    s->appendRect(x, y, w, h, 6, 6);
    s->fill(r, g, b, a);
    return s;
}

static Shape* circle(float cx, float cy, float rad,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Shape* s = Shape::gen();
    s->appendCircle(cx, cy, rad, rad);
    s->fill(r, g, b, a);
    return s;
}

static Shape* hline(float x0, float y, float x1, uint8_t r, uint8_t g,
                    uint8_t b, float w) {
    Shape* s = Shape::gen();
    s->moveTo(x0, y);
    s->lineTo(x1, y);
    s->strokeWidth(w);
    s->strokeFill(r, g, b, 255);
    return s;
}

/* render scene ke sebuah buffer layer (draw(true) = clear+render) */
static void render_layer(uint32_t* buf, uint32_t w, uint32_t h, Scene* sc) {
    SwCanvas* cv = SwCanvas::gen();
    cv->target(buf, w, w, h, ColorSpace::ARGB8888);
    cv->push(sc);
    cv->update();
    cv->draw(true);
    cv->sync();
    delete cv;      /* scene & shape ikut ter-refcount */
}

/* ---- LAYER BACKGROUND: wallpaper + chrome taskbar (SEKALI) ---- */
static void render_bg_layer(void) {
    uint32_t W = g_W, H = g_H;
    Scene* sc = Scene::gen();

    /* wallpaper: gradient monokrom hitam -> abu */
    Shape* bg = Shape::gen();
    bg->appendRect(0, 0, (float)W, (float)H);
    LinearGradient* grad = LinearGradient::gen();
    grad->linear(0, 0, (float)W, (float)H);
    Fill::ColorStop stops[4] = {
        {0.0f,  G_WALL_A, G_WALL_A, G_WALL_A, 255},
        {0.45f, G_WALL_B, G_WALL_B, G_WALL_B, 255},
        {0.75f, G_WALL_C, G_WALL_C, G_WALL_C, 255},
        {1.0f,  G_WALL_D, G_WALL_D, G_WALL_D, 255},
    };
    grad->colorStops(stops, 4);
    bg->fill(grad);
    sc->push(bg);

    /* brand mark: halo putih radial (top-kanan) */
    Shape* ring = Shape::gen();
    ring->appendCircle((float)(W - 130), 120.0f, 80, 80);
    RadialGradient* rg = RadialGradient::gen();
    rg->radial((float)(W - 130), 120, 8, (float)(W - 130), 120, 90);
    Fill::ColorStop rstops[2] = {
        {0.0f, 255, 255, 255, 70},
        {1.0f, 255, 255, 255, 0},
    };
    rg->colorStops(rstops, 2);
    ring->fill(rg);
    sc->push(ring);

    /* taskbar hitam + garis separator */
    float th = TASKBAR_H;
    sc->push(rect_sharp(0, H - th, (float)W, th, G_BAR, G_BAR, G_BAR, 245));
    sc->push(hline(0, H - th, (float)W, G_BAR_LN, G_BAR_LN, G_BAR_LN, 1.5f));

    /* tombol MENU (idle) + panel jam — tombol jendela DINAMIS (compose) */
    float mby = H - th + 7;
    sc->push(rect_round(10, mby, 92, 30, G_MENU, G_MENU, G_MENU, 255));
    sc->push(rect_round((float)(W - 128), mby, 118, 30, G_CLOCK, G_CLOCK, G_CLOCK, 255));

    render_layer(L_bg, W, H, sc);

    /* teks statis taskbar: label MENU (hitam di tombol putih) */
    text_blit_buf(L_bg, g_W, g_W, g_H, 34, (int)(H - th + 15), "MENU",
                  0x141414, 0);
}

/* ---- LAYER JENDELA per app, 2 varian fokus (SEKALI saat open) ---- */
static void render_win_layer(int i, int focused) {
    Win& w = wins[i];
    uint32_t lw = (uint32_t)w.w, lh = (uint32_t)w.h;

    uint32_t* buf = L_win[i][focused ? 1 : 0];
    if (!buf) buf = L_win[i][0];      /* fallback varian tunggal */
    if (!buf) return;

    Scene* sc = Scene::gen();

    /* body: putih — OPAQUE (alpha 255) supaya komposisi = copy cepat */
    sc->push(rect_round(0, 0, (float)lw, (float)lh, G_BODY, G_BODY, G_BODY, 255));

    /* titlebar: gradient horizontal — AKTIF abu terang, idle abu gelap */
    uint8_t ta = focused ? G_TTL_AF : G_TTL_UF;
    uint8_t tb = focused ? G_TTL_AT : G_TTL_UT;
    Shape* title = Shape::gen();
    title->appendRect(0, 0, (float)lw, TITLE_H, 6, 6);
    LinearGradient* tg = LinearGradient::gen();
    tg->linear(0, 0, (float)lw, 0);
    Fill::ColorStop tstops[2] = {
        {0.0f, ta, ta, ta, 255},
        {1.0f, tb, tb, tb, 255},
    };
    tg->colorStops(tstops, 2);
    title->fill(tg);
    sc->push(title);

    /* tombol close & minimize (lingkaran monokrom) */
    uint8_t cb = focused ? G_CLOSE_F : G_CLOSE_U;
    uint8_t mb = focused ? G_MINI_F : G_MINI_U;
    sc->push(circle((float)lw - 20, TITLE_H / 2, 7, cb, cb, cb, 255));
    sc->push(circle((float)lw - 42, TITLE_H / 2, 7, mb, mb, mb, 255));

    /* garis pemisah AA bawah titlebar */
    sc->push(hline(0, TITLE_H, (float)lw, 20, 20, 20, 1.5f));

    /* tombol kalkulator (bagian dari layer — statis) */
    if (i == APP_CALC) {
        sc->push(rect_round(CALC_M, TITLE_H + CALC_M,
                            (float)lw - 2 * CALC_M, CALC_DISP_H,
                            G_CALC_DISP, G_CALC_DISP, G_CALC_DISP, 255));
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 4; col++) {
                float bx, by, bw2, bh2;
                calc_grid(w, row, col, &bx, &by, &bw2, &bh2);
                uint8_t kind = CALC_KIND[row][col];
                uint8_t lv = calc_btn_base(kind);
                sc->push(rect_round(bx - w.x, by - w.y, bw2, bh2, lv, lv, lv, 255));
            }
        }
    }

    render_layer(buf, lw, lh, sc);

    /* ---- teks statis ke layer ---- */
    uint32_t ttl_col = focused ? 0x181818 : 0xD8D8D8;
    text_blit_buf(buf, lw, lw, lh, 12, 8, APP_NAMES[i], ttl_col, 0);
    uint32_t x_col = focused ? 0xFFFFFF : 0xE0E0E0;
    uint32_t m_col = focused ? 0x101010 : 0xE0E0E0;
    text_blit_buf(buf, lw, lw, lh, (int)lw - 23, 12, "x", x_col, 0);
    text_blit_buf(buf, lw, lw, lh, (int)lw - 45, 12, "-", m_col, 0);

    if (i == APP_ABOUT) {
        static const char* L1[] = {
            "EquiX Desktop 0.4 Beta - fast",
            "kernel  : Equinox OS 0.4.2",
            "render  : layer-cache + dirty-rect",
            "arena   : dinamik ikut RAM (bukan 10 MB)",
            "tema    : hitam / abu / putih",
            "klik area ini untuk counter:",
        };
        for (int k = 0; k < 6; k++)
            text_blit_buf(buf, lw, lw, lh, 14, 46 + k * 20, L1[k],
                          k ? 0x505050 : 0x282828, 0);
        /* baris counter = DINAMIS (compose) di 46+6*20 = 166 */
    } else if (i == APP_NOTES) {
        static const char* L2[] = {
            "Panduan cepat:",
            "- drag : tahan titlebar lalu geser",
            "- klik jendela utk fokus/minimize",
            "- MENU kiri-bawah: buka aplikasi",
            "Calculator:",
            "- angka, . lalu operator + - * /",
            "- = hasil, C reset, +/- negasi",
            "- sqrt akar, % dibagi seratus",
            "- KEYBOARD: ngetik langsung bisa",
        };
        for (int k = 0; k < 9; k++)
            text_blit_buf(buf, lw, lw, lh, 14, 46 + k * 18, L2[k],
                          (k == 0 || k == 4) ? 0x282828 : 0x484848, 0);
    } else if (i == APP_CALC) {
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 4; col++) {
                float bx, by, bw2, bh2;
                calc_grid(w, row, col, &bx, &by, &bw2, &bh2);
                const char* lbl = CALC_LBL[row][col];
                int ll = 0;
                while (lbl[ll]) ll++;
                int tx = (int)(bx - w.x) + ((int)bw2 - ll * 8) / 2;
                int ty = (int)(by - w.y) + ((int)bh2 - 16) / 2;
                uint8_t kind = CALC_KIND[row][col];
                uint32_t col2 = (kind == 2 || kind == 3)
                                ? 0x101010 : 0xFFFFFF;
                text_blit_buf(buf, lw, lw, lh, tx, ty, lbl, col2, 0);
            }
        }
    }
}

/* alokasikan + render kedua varian layer jendela */
/* ------------------------------------------------------------------ */
/*  Layer jendela                                                      */
/* ------------------------------------------------------------------ */
/* Status fokus yang tersimpan di buffer "varian tunggal" (kedua pointer
 * menunjuk buffer yang sama). Windows normal punya 2 varian (aktif/idle)
 * sehingga tidak disentuh sync_layer_focus(). */
static int lay_focus[APP_COUNT];

static void build_win_layers(int i) {
    Win& w = wins[i];
    size_t sz = (size_t)((int)w.w) * (int)w.h * 4;

    L_win_stride[i] = (uint32_t)w.w;

    /* 0.4 Beta: jendela Terminal sengaja TANPA layer. Arena GUI 11 MB sudah
     * penuh (bg + backbuffer 8160 KB + 4 jendela lama ~3920 KB, sisa
     * ~80 KB), sehingga layer 640x416 = 1040 KB SELALU gagal dialokasikan
     * -> jendela tidak pernah tampil. Chrome titlebar digambar langsung
     * oleh compose (term_chrome) dan isinya = mirror konsol (term_draw),
     * jadi layer statis memang tidak dibutuhkan sama sekali — justru
     * membebaskan 1040 KB. */
    if (i == APP_TERM) {
        L_win[i][0] = L_win[i][1] = nullptr;
        return;
    }

    L_win[i][0] = (uint32_t*)ga_malloc(sz);
    if (!L_win[i][0]) {
        /* Arena GUI habis. Compose melewati window dgn layer NULL, jadi
         * ini bukan fatal — tapi WAJIB dicek: ga_malloc() mengembalikan
         * nullptr dan render/copy ke alamat 0 = kernel panic. */
        L_win[i][1] = nullptr;
        printf("[equix] layer %d '%s' gagal alokasi %u KB (arena GUI penuh)\n",
               i, APP_NAMES[i], (uint32_t)(sz / 1024));
        return;
    }

    /* APP_FM sengaja memakai SATU buffer utk kedua status fokus.
     * Angkanya di arena 12 MB (0x3400000-0x3FE0000): bg 4080 + backbuffer
     * 4080 + 3 app lama 1276 = 9436 KB, sisa ~1448 KB — jendela Files
     * 760x460 per varian 1365 KB, jadi DUA varian tidak muat (varian
     * kedua gagal -> titlebar selalu terlihat "aktif").
     * Isi FM memang digambar ulang 100% di compose(); yang dipakai dari
     * layer hanya chrome titlebar, dan itu dirender ulang oleh
     * sync_layer_focus() HANYA saat fokus berubah (langka, ~4 ms). */
    if (i == APP_FM) L_win[i][1] = L_win[i][0];
    else             L_win[i][1] = (uint32_t*)ga_malloc(sz);
    if (!L_win[i][1]) L_win[i][1] = L_win[i][0];   /* fallback varian tunggal */

    render_win_layer(i, 0);
    if (L_win[i][1] != L_win[i][0]) render_win_layer(i, 1);
    lay_focus[i] = 0;
}

/* Render ulang window yang memakai varian tunggal bila status fokusnya
 * berubah — jadi titlebar tetap benar tanpa menggandakan buffer. */
static void sync_layer_focus(void) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (!L_win[i][0] || L_win[i][1] != L_win[i][0]) continue;
        int want = (focus_idx == i) ? 1 : 0;
        if (lay_focus[i] == want) continue;
        lay_focus[i] = want;
        render_win_layer(i, want);
    }
}

static void free_win_layers(int i) {
    if (L_win[i][0] && L_win[i][0] != L_win[i][1]) ga_free(L_win[i][0]);
    if (L_win[i][1]) ga_free(L_win[i][1]);
    L_win[i][0] = L_win[i][1] = nullptr;
}

/* ================================================================== */
/*  COMPOSE — bangun region kotor di backbuffer lalu blit             */
/* ================================================================== */
static int  hov_menu = 0;             /* hover tombol MENU            */
static int  hov_task_idx = -1;        /* hover tombol jendela i       */
static int  hov_launch = -1;          /* hover baris launcher         */
static int  hov_calc_row = -1, hov_calc_col = -1;
static int  prev_clock_sec = -1;
static int  prev_clock_h = 12, prev_clock_m = 0;   /* jam utk compose */
static int  g_cur_x = -1, g_cur_y = -1;            /* pos kursor      */

static int win_z_order[APP_COUNT];    /* urutan z (kecil = bawah) */

static void sort_z(void) {
    for (int i = 0; i < APP_COUNT; i++) win_z_order[i] = i;
    for (int i = 1; i < APP_COUNT; i++) {
        int k = win_z_order[i], j = i - 1;
        while (j >= 0 && wins[win_z_order[j]].z > wins[k].z) {
            win_z_order[j + 1] = win_z_order[j]; j--;
        }
        win_z_order[j + 1] = k;
    }
}

/* geometry launcher (dipakai compose + hit-test) */
static float launch_lx(void)  { return 10.0f; }
static float launch_lw(void)  { return 240.0f; }
static float launch_lh(void)  { return 36.0f * (APP_COUNT + 1) + 16.0f; }
static float launch_ly(void)  { return (float)g_H - TASKBAR_H - launch_lh() - 6.0f; }

/* ================================================================== */
/*  FILE MANAGER (APP_FM) — tampilan standar ala Nautilus/PCManFM     */
/* ------------------------------------------------------------------ */
/*  Ringkasan keputusan desain:                                        */
/*   * SELURUH isi jendela digambar di compose(), BUKAN di layer.      */
/*     Layer hanya memuat chrome (titlebar + body putih). Konsekuensi  */
/*     navigasi/seleksi/hover cukup mark_dirty rect yang berubah —     */
/*     biaya repaint sebanding dengan area kotor (bukan layar penuh),  */
/*     dan tidak perlu membangun ulang scene ThorVG tiap klik.         */
/*   * Semua helper gambar (bb_fill_rect / text_blit) sudah menghormati */
/*     clip compose, jadi menggambar "semuanya" tetap murah: baris di   */
/*     luar region kotor lolos di uji py dan tidak menyentuh memori.   */
/*   * Operasi tulis memakai API fs_ yang sama dengan shell, jadi      */
/*     RAMFS dan FAT32 (/mnt) sama-sama jalan, write-through.          */
/* ================================================================== */
#define FM_M           6
#define FM_SIDE        150
#define FM_TOOL_Y      (TITLE_H + 2)
#define FM_TOOL_H      34
#define FM_LOC_Y       (FM_TOOL_Y + FM_TOOL_H + 2)
#define FM_LOC_H       24
#define FM_BODY_Y      (FM_LOC_Y + FM_LOC_H + 4)
#define FM_HDR_H       18
#define FM_ROW_H       22
#define FM_STATUS_H    22
#define FM_SB_W        12
#define FM_MAX_ENT     192
#define FM_HIST        16
#define FM_DLG_W       368

/* toolbar: 4 tombol navigasi (kiri) + 3 tombol aksi (kanan) */
static const char* FM_TOOL_L[]  = { "Back", "Up", "Home", "Reload" };
static const int   FM_TOOL_LW[] = { 72, 60, 68, 78 };
static const char* FM_TOOL_R[]  = { "New Folder", "Rename", "Delete" };
static const int   FM_TOOL_RW[] = { 104, 84, 80 };
#define FM_TOOL_NL 4
#define FM_TOOL_NR 3
#define FM_TOOL_N  (FM_TOOL_NL + FM_TOOL_NR)

/* sidebar "Places" */
/* Tempat pada sidebar — SEMUA harus path nyata yang ada di FS.
 * Catatan: "/bin" hanya path pencarian tool (lihat `which`), direktorinya
 * tidak pernah dibuat, jadi jangan dipakai sebagai tujuan. "/user" adalah
 * home (shell juga start di sana). fm_init() tetap buka "/" agar tampilan
 * awal tidak kosong. */
static const char* FM_PLACE_L[] = { "Home", "Filesystem", "Disk", "System",
                                    "Tools", "Games", "Samples" };
static const char* FM_PLACE_P[] = { "/user", "/", "/mnt", "/equinox",
                                    "/equinox/tools", "/equinox/games", "/test" };
#define FM_PLACES   7
#define FM_PLACE_Y  26
#define FM_PLACE_H  24

/* dialog modal: 1 = folder baru, 2 = rename, 3 = hapus, 4 = properti */
#define FM_DLG_NONE 0
#define FM_DLG_NEW  1
#define FM_DLG_REN  2
#define FM_DLG_DEL  3
#define FM_DLG_PROP 4

/* kunci urutan — dipilih dengan klik pada header kolom */
#define FM_SORT_NAME 0
#define FM_SORT_SIZE 1
#define FM_SORT_TYPE 2

/* menu konteks klik kanan (item, tinggi bar, padding) */
#define FM_CTX_N    4
#define FM_CTX_H    24
#define FM_CTX_PAD  6
#define FM_CTX_W    148

struct FmState {
    struct fs_node* dir;
    char   path[128];
    char   hist[FM_HIST][128];
    int    hist_n;
    int    nent;
    struct fs_node* ent[FM_MAX_ENT];
    int    sel;                 /* indeks entri terpilih, -1 = none      */
    int    top;                 /* baris pertama yang terlihat (scroll)  */
    int    hrow, hbtn, hplace;  /* hover: baris / tombol / tempat        */
    int    hdbtn;               /* hover tombol dialog (0 OK, 1 Batal)   */
    int    hcol;                /* hover header kolom, -1 = none         */
    int    scroll;              /* 1 = sedang men-drag scrollbar         */
    int    last_row, last_tick; /* deteksi klik ganda                    */
    char   status[96];
    int    dlg;
    char   dbuf[64];
    int    dlen;
    /* menu konteks (klik kanan): koordinat LAYAR, item = -1 bila tak ada */
    int    ctx;                 /* 1 = menu terbuka                      */
    int    ctx_item;
    int    ctx_x, ctx_y;
    int    sort_key;            /* FM_SORT_*                             */
    int    sort_dir;            /* 1 = naik, -1 = turun                  */
};
static FmState fm;

/* ------------------------------------------------------------------ */
/*  Terminal (GUI Terminal) state                                      */
/*  ------------------------------------------------------------------ */
/*  Jendela ini HANYA "layar" bagi sebuah shell task yang berjalan di   */
/*  konsolnya sendiri (task_create_shell -> shell_entry, persis jalur    */
/*  F1). Dengan begitu semua perintah shell.cpp yang asli (kernel-side,  */
/*  program tetap ring 3) jalan tanpa satu baris pun diubah di sana:     */
/*                                                                        */
/*    output  : printf task itu menulis CELL MIRROR konsolnya saja       */
/*              (g_out != g_act -> tak pernah menyentuh framebuffer)     */
/*    input   : keystroke disuntikkan ke ring konsol terminal            */
/*              (keyboard_push_scancode) lalu di-decode oleh getkey()    */
/*              milik task itu sendiri                                    */
/*    display : compose() membaca mirror ini tiap frame (term_draw)      */
/* ------------------------------------------------------------------ */
#define TERM_COLS   80    /* 640 px / 8  — lebar jendela               */
#define TERM_ROWS   24    /* 384 px / 16 — tinggi isi (h - titlebar)   */
#define TERM_SB_LINES 400 /* fase 2: baris riwayat scrollback (64 KB)  */
#define TERM_SB_PAGE   8  /* fase 2: PgUp/PgDn = 1 "halaman" riwayat    */

struct TermState {
    int      con;             /* konsol shell terminal, -1 = belum ada */
    int      pid;             /* pid shell task,         -1 = belum ada */
    uint32_t last_seq;        /* edit_seq mirror terakhir (pemicu repaint) */
    int      err;             /* 1 = gagal spawn shell (task table penuh) */
    /* --- fase 2: SCROLLBACK -------------------------------------
     * follow = 1 -> tampilan LIVE (baris terbaru selalu di bawah).
     * follow = 0 -> tampilan DI-PIN pada baris virtual `top` (baris
     * riwayat tetap terlihat walau output baru terus masuk, seperti
     * xterm). `top` = indeks baris virtual teratas; indeks baris
     * absolutnya STABIL (riwayat tumbuh di tengah, bukan menggeser
     * nomor baris lama).
     * e0 = status deteksi scancode panjang E0-49/E0-51 (PgUp/PgDn)
     * yang dipegang sampai byte keduanya tiba. */
    int      top;
    int      follow;
    int      e0;
    /* kode make PgUp/PgDn (0x49/0x51) yang sedang kita telan — pasangan
     * BREAK-nya (E0 C9 / E0 D1) harus ikut ditelan dan TIDAK boleh
     * dianggap "kunci asing" yang menarik tampilan ke live. */
    int      sb_hold;
};
static TermState term;

/* ------------------------------------------------------------------ */
/*  Geometri (relatif ke jendela)                                      */
/* ------------------------------------------------------------------ */
static int fm_list_x0(void)                 { return FM_M + FM_SIDE + 8; }
static int fm_list_x1(const Win& w)         { return (int)w.w - FM_M - FM_SB_W - 6; }
static int fm_rows_y0(void)                 { return FM_BODY_Y + FM_HDR_H + 2; }
static int fm_rows_y1(const Win& w)         { return (int)w.h - FM_M - FM_STATUS_H - 4; }
static int fm_vis(const Win& w)             {
    int v = (fm_rows_y1(w) - fm_rows_y0()) / FM_ROW_H;
    return v > 0 ? v : 1;
}

/* tombol toolbar: rect jendela-relatif */
static void fm_tool_rect(const Win& w, int b, int* x, int* y, int* ww, int* hh) {
    *hh = FM_TOOL_H - 8;
    *y  = FM_TOOL_Y + 4;
    if (b < FM_TOOL_NL) {
        int px = FM_M;
        for (int i = 0; i < b; i++) px += FM_TOOL_LW[i] + 6;
        *x = px; *ww = FM_TOOL_LW[b];
    } else {
        /* tombol aksi dirapatkan ke tepi KANAN jendela:
         *   [New Folder] [Rename] [Delete]  <- Delete paling kanan */
        int k = b - FM_TOOL_NL;
        int px = (int)w.w - FM_M;
        for (int i = FM_TOOL_NR - 1; i > k; i--) px -= FM_TOOL_RW[i] + 6;
        px -= FM_TOOL_RW[k];
        *x = px; *ww = FM_TOOL_RW[k];
    }
}

static const char* fm_tool_label(int b) {
    return (b < FM_TOOL_NL) ? FM_TOOL_L[b] : FM_TOOL_R[b - FM_TOOL_NL];
}

/* tombol nonaktif = abu pudar (Home/Back/Reload selalu aktif) */
static bool fm_tool_enabled(int b) {
    if (b == 0) return fm.hist_n > 0;                    /* Back   */
    if (b == 1) return fm.dir && fm.dir->parent;         /* Up     */
    if (b == 5 || b == 6) return fm.sel >= 0;            /* Ren/Del*/
    return true;
}

static int fm_place_y(const Win& w, int p) {
    (void)w;
    return FM_BODY_Y + FM_PLACE_Y + p * FM_PLACE_H;
}

/* Batas kiri kolom pada header: Name | Size | Type.
 * Nilainya mengikuti rumus yang sama dgn penataan teks di fm_draw
 * (kolom Size diakhiri col_size, kolom Type mulai di col_type). */
static void fm_col_edges(const Win& w, int* c0, int* c1, int* c2) {
    int col_type = fm_list_x1(w) - 96;
    *c0 = fm_list_x0();
    *c1 = col_type - 10 - 72;   /* Name | Size  (72 = lebar teks Size)  */
    *c2 = col_type;             /* Size  | Type                          */
}

/* ------------------------------------------------------------------ */
/*  Status bar + penanda kotor                                         */
/* ------------------------------------------------------------------ */
static void fm_set_status(const char* s) {
    int n = (int)strlen(s);
    if (n > (int)sizeof(fm.status) - 1) n = (int)sizeof(fm.status) - 1;
    for (int i = 0; i < n; i++) fm.status[i] = s[i];
    fm.status[n] = '\0';
    Win& w = wins[APP_FM];
    if (w.open && !w.minimized)
        mark_dirty((int)w.x + FM_M, (int)w.y + (int)w.h - FM_M - FM_STATUS_H,
                   (int)w.w - 2 * FM_M, FM_STATUS_H);
}

static void fm_dirty_win(void) {
    Win& w = wins[APP_FM];
    if (w.open && !w.minimized)
        mark_dirty((int)w.x, (int)w.y, (int)w.w, (int)w.h);
}

static void fm_dirty_list(void) {
    Win& w = wins[APP_FM];
    if (!w.open || w.minimized) return;
    mark_dirty((int)w.x + fm_list_x0() - 4, (int)w.y + fm_rows_y0() - 2,
               (int)w.w - fm_list_x0() + 4 - FM_M, (int)w.h);
}

/* ------------------------------------------------------------------ */
/*  Utilitas teks                                                      */
/* ------------------------------------------------------------------ */
/* banding nama tak peka huruf besar/kecil (strcasecmp tidak ada) */
static int fm_icmp(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int fm_ext_is(const char* ext, const char* want) {
    return fm_icmp(ext, want) == 0;
}

/* potong EKOR — utk nama file (kiri lebih penting) */
static void fm_fit_tail(const char* s, int maxch, char* out, int cap) {
    int n = (int)strlen(s);
    if (maxch < 2) maxch = 2;
    if (n <= maxch) { snprintf(out, cap, "%s", s); return; }
    int k = maxch - 1;
    if (k >= cap) k = cap - 1;
    for (int i = 0; i < k; i++) out[i] = s[i];
    out[k] = '>'; out[k + 1] = '\0';
}

/* potong KEPALA — utk path di location bar (ekor lebih penting) */
static void fm_fit_head(const char* s, int maxch, char* out, int cap) {
    int n = (int)strlen(s);
    if (maxch < 2) maxch = 2;
    if (n <= maxch) { snprintf(out, cap, "%s", s); return; }
    int skip = n - (maxch - 2);
    int i = 0;
    out[i++] = '>';
    for (int k = 0; k < 2 && i < cap - 1; k++) out[i++] = '.';
    for (int k = skip; k < n && i < cap - 1; k++) out[i++] = s[k];
    out[i] = '\0';
}

static void fm_size_str(uint32_t sz, char* out, int cap) {
    if (sz < 1024u)          snprintf(out, cap, "%u B", (unsigned)sz);
    else if (sz < 1048576u)  snprintf(out, cap, "%u.%u KB",
                                      (unsigned)(sz / 1024u),
                                      (unsigned)((sz % 1024u) / 103u));
    else                     snprintf(out, cap, "%u.%u MB",
                                      (unsigned)(sz / 1048576u),
                                      (unsigned)((sz % 1048576u) / 104858u));
}

static const char* fm_typename(const struct fs_node* n) {
    if (n->is_dir) return "Folder";
    const char* dot = strrchr(n->name, '.');
    if (!dot || dot == n->name || !dot[1]) return "File";
    const char* e = dot + 1;
    if (fm_ext_is(e, "c"))     return "C source";
    if (fm_ext_is(e, "h"))     return "Header";
    if (fm_ext_is(e, "cpp") || fm_ext_is(e, "cc")) return "C++ source";
    if (fm_ext_is(e, "mrp"))   return "Program";
    if (fm_ext_is(e, "elf"))   return "ELF binary";
    if (fm_ext_is(e, "txt"))   return "Text";
    if (fm_ext_is(e, "md"))    return "Markdown";
    if (fm_ext_is(e, "wad"))   return "WAD data";
    if (fm_ext_is(e, "py"))    return "Python";
    if (fm_ext_is(e, "sh"))    return "Shell script";
    if (fm_ext_is(e, "asm"))   return "Assembly";
    if (fm_ext_is(e, "ld"))    return "Linker script";
    if (fm_ext_is(e, "json"))  return "JSON";
    if (fm_ext_is(e, "cfg") || fm_ext_is(e, "conf") || fm_ext_is(e, "ini"))
                               return "Config";
    return "File";
}

/* ------------------------------------------------------------------ */
/*  Ikon                                                               */
/* ------------------------------------------------------------------ */
static void fm_icon_dir(int x, int y, uint32_t fill) {
    bb_fill_rect(x,     y + 2, 6, 3, fill);      /* tab              */
    bb_fill_rect(x,     y + 5, 14, 7, fill);     /* badan            */
    bb_fill_rect(x + 1, y + 6, 12, 1, 0xFFFFFFFFu | 0x00303030u);
}

static void fm_icon_file(int x, int y, bool inv) {
    uint32_t edge = inv ? 0xFFFFFFFFu : gray(120);
    uint32_t body = inv ? gray(60)       : 0xFFFFFFFFu;
    uint32_t line = inv ? gray(180)      : gray(150);
    bb_fill_rect(x, y, 11, 14, edge);
    bb_fill_rect(x + 1, y + 1, 9, 12, body);
    bb_fill_rect(x + 3, y + 4, 5, 1, line);
    bb_fill_rect(x + 3, y + 6, 5, 1, line);
    bb_fill_rect(x + 3, y + 8, 3, 1, line);
}

/* panah urutan kecil (7x4) di samping label header kolom aktif:
 * up = A->Z / naik, down = Z-A / turun. */
static void fm_sort_arrow(int x, int y, bool up, uint32_t col) {
    for (int r = 0; r < 4; r++) {
        int k = up ? r : 3 - r;                 /* 0..3: lebar bertambah */
        int wid = 1 + 2 * k;
        bb_fill_rect(x + (7 - wid) / 2, y + r, wid, 1, col);
    }
}

/* ------------------------------------------------------------------ */
/*  Isi direktori                                                      */
/* ------------------------------------------------------------------ */
static void fm_sort(void) {
    /* insertion sort: folder dulu, lalu kunci urutan yang dipilih
     * di header (Name/Size/Type) dengan arah fm.sort_dir. Nama tetap
     * jadi tie-break supaya hasilnya deterministik. */
    for (int i = 1; i < fm.nent; i++) {
        struct fs_node* k = fm.ent[i];
        int j = i - 1;
        while (j >= 0) {
            struct fs_node* a = fm.ent[j];
            int c;
            if (a->is_dir != k->is_dir)
                c = (int)k->is_dir - (int)a->is_dir;        /* folder dulu */
            else {
                switch (fm.sort_key) {
                    case FM_SORT_SIZE:
                        c = (a->size < k->size) ? -1
                          : (a->size > k->size) ?  1 : 0;
                        break;
                    case FM_SORT_TYPE:
                        c = fm_icmp(fm_typename(a), fm_typename(k));
                        break;
                    default:
                        c = 0;
                        break;
                }
                if (c == 0) c = fm_icmp(a->name, k->name);
                c *= fm.sort_dir;
            }
            if (c <= 0) break;
            fm.ent[j + 1] = a;
            j--;
        }
        fm.ent[j + 1] = k;
    }
}

static void fm_refresh(void) {
    fm.nent = 0;
    if (!fm.dir) return;
    /* FAT32: mirror direktori diisi lazy saat pertama dibaca */
    if (fm.dir->backing == 1 && !fm.dir->populated)
        fat32_populate_dir(fm.dir);
    for (struct fs_node* c = fm.dir->children;
         c && fm.nent < FM_MAX_ENT; c = c->next)
        fm.ent[fm.nent++] = c;
    fm_sort();
    if (fm.sel >= fm.nent) fm.sel = fm.nent ? fm.nent - 1 : -1;
    int vis = fm_vis(wins[APP_FM]);
    if (fm.top > fm.nent - vis) fm.top = fm.nent - vis;
    if (fm.top < 0) fm.top = 0;
}

static void fm_go(const char* path, int push_hist) {
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), path);
    if (!n || !n->is_dir) {
        char b[128];
        snprintf(b, sizeof(b), "No such folder: %s", path);
        fm_set_status(b);
        return;
    }
    if (push_hist && fm.dir && fm.hist_n < FM_HIST)
        snprintf(fm.hist[fm.hist_n++], sizeof(fm.hist[0]), "%s", fm.path);
    fm.dir = n;
    fs_get_path(n, fm.path, sizeof(fm.path));
    fm.sel = -1;
    fm.top = 0;
    fm.hrow = -1;
    fm_refresh();
    char b[96];
    snprintf(b, sizeof(b), "%d item%s in %s", fm.nent,
             fm.nent == 1 ? "" : "s", fm.path);
    fm_set_status(b);
    fm_dirty_win();
}

static void fm_back(void) {
    if (fm.hist_n <= 0) { fm_set_status("No previous folder"); return; }
    char prev[128];
    fm.hist_n--;
    snprintf(prev, sizeof(prev), "%s", fm.hist[fm.hist_n]);
    /* jangan mendorong diri sendiri ke history */
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), prev);
    if (!n || !n->is_dir) { fm_set_status("Folder no longer exists"); return; }
    fm.dir = n;
    fs_get_path(n, fm.path, sizeof(fm.path));
    fm.sel = -1;
    fm.top = 0;
    fm.hrow = -1;
    fm_refresh();
    char b[96];
    snprintf(b, sizeof(b), "%d item%s in %s", fm.nent,
             fm.nent == 1 ? "" : "s", fm.path);
    fm_set_status(b);
    fm_dirty_win();
}

static void fm_up(void) {
    if (!fm.dir || !fm.dir->parent) { fm_set_status("Already at root"); return; }
    char p[128];
    fs_get_path(fm.dir->parent, p, sizeof(p));
    fm_go(p, 1);
}

static void fm_open_selected(void) {
    if (fm.sel < 0 || fm.sel >= fm.nent) return;
    struct fs_node* n = fm.ent[fm.sel];
    if (n->is_dir) {
        char p[128];
        fs_get_path(n, p, sizeof(p));
        fm_go(p, 1);
    } else {
        fm.dlg = FM_DLG_PROP;
        fm.dlen = 0;
        fm.dbuf[0] = '\0';
        fm.hdbtn = -1;
        fm_dirty_win();
    }
}

/* ------------------------------------------------------------------ */
/*  Dialog modal                                                       */
/* ------------------------------------------------------------------ */
static void fm_dlg_box(int* rx, int* ry, int* rw, int* rh) {
    Win& w = wins[APP_FM];
    *rw = FM_DLG_W;
    *rh = (fm.dlg == FM_DLG_DEL) ? 132
        : (fm.dlg == FM_DLG_PROP) ? 168 : 156;
    *rx = ((int)w.w - *rw) / 2;
    *ry = TITLE_H + ((int)w.h - TITLE_H - *rh) / 2;
}

static void fm_dlg_btn(int which, int* x, int* y, int* ww, int* hh) {
    int rx, ry, rw, rh;
    fm_dlg_box(&rx, &ry, &rw, &rh);
    *ww = 84; *hh = 26;
    *y  = ry + rh - 14 - *hh;
    int cancel = rx + rw - 14 - *ww;
    if (fm.dlg == FM_DLG_PROP) { *x = cancel; return; }   /* OK di pojok */
    *x = (which == 1) ? cancel : cancel - 10 - *ww;
}

static void fm_dirty_dlg(void) {
    Win& w = wins[APP_FM];
    if (!w.open || !fm.dlg) return;
    int rx, ry, rw, rh;
    fm_dlg_box(&rx, &ry, &rw, &rh);
    mark_dirty((int)w.x + rx - 6, (int)w.y + ry - 6, rw + 12, rh + 12);
}

static void fm_dlg_cancel(void) {
    fm.dlg = FM_DLG_NONE;
    fm.dlen = 0;
    fm.dbuf[0] = '\0';
    fm.hdbtn = -1;
    fm_dirty_win();
}

/* nama valid: tak kosong, tanpa '/', bukan "." / "..", < 64 */
static int fm_name_ok(const char* s, int* why) {
    int n = (int)strlen(s);
    if (n == 0)                  { *why = 0; return 0; }
    if (n >= 64)                 { *why = 1; return 0; }
    if (strchr(s, '/'))          { *why = 2; return 0; }
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) { *why = 3; return 0; }
    *why = 0;
    return 1;
}

/* rename RAMFS = relink; rename FAT = copy+delete (sama dgn sys_rename) */
static int fm_rename_node(struct fs_node* src, const char* new_name) {
    if (!src || !src->parent) return -1;
    struct fs_node* dst = src->parent;
    if (fs_find_child(dst, new_name)) return -2;
    if (src->backing == 0) {
        return fs_ram_relink_node(src, dst, new_name) == 0 ? 0 : -3;
    }
    if (src->is_dir) return -4;                 /* FAT dir: belum didukung */
    if (fs_ensure_content(src) != 0) return -5;
    if (fs_write_binary(dst, new_name,
                        (const uint8_t*)src->content, src->size) != 0) return -5;
    if (fs_delete_node(src->parent, src->name) != 0) return -6;
    return 0;
}

static void fm_dlg_ok(void) {
    int why = 0;
    if (fm.dlg == FM_DLG_NEW) {
        if (!fm_name_ok(fm.dbuf, &why)) {
            const char* m[] = { "Name cannot be empty", "Name too long (63 max)",
                                "Name cannot contain '/'",
                                "'.' and '..' are reserved" };
            fm_set_status(m[why]);
            fm_dirty_dlg();
            return;
        }
        if (fs_find_child(fm.dir, fm.dbuf)) {
            fm_set_status("Already exists");
            fm_dirty_dlg();
            return;
        }
        int r = fs_create_dir(fm.dir, fm.dbuf);
        if (r != 0) {
            fm_set_status(r == -2 ? "Already exists" : "Cannot create folder");
            fm_dirty_dlg();
            return;
        }
        char nm[64];
        snprintf(nm, sizeof(nm), "%s", fm.dbuf);
        fm.dlg = FM_DLG_NONE;
        fm_refresh();
        fm.sel = -1;
        for (int i = 0; i < fm.nent; i++)
            if (strcmp(fm.ent[i]->name, nm) == 0) { fm.sel = i; break; }
        char b[96];
        snprintf(b, sizeof(b), "Created folder '%s'", nm);
        fm_set_status(b);
        fm_dirty_win();
        return;
    }
    if (fm.dlg == FM_DLG_REN) {
        if (fm.sel < 0 || fm.sel >= fm.nent) { fm_dlg_cancel(); return; }
        struct fs_node* n = fm.ent[fm.sel];
        if (!fm_name_ok(fm.dbuf, &why)) {
            const char* m[] = { "Name cannot be empty", "Name too long (63 max)",
                                "Name cannot contain '/'",
                                "'.' and '..' are reserved" };
            fm_set_status(m[why]);
            fm_dirty_dlg();
            return;
        }
        if (strcmp(n->name, fm.dbuf) != 0) {
            int r = fm_rename_node(n, fm.dbuf);
            const char* msg = (r == 0)  ? "Renamed"
                            : (r == -2) ? "Target name already exists"
                            : (r == -4) ? "Renaming folders on FAT is not supported"
                            : "Rename failed";
            char nm[64];
            snprintf(nm, sizeof(nm), "%s", fm.dbuf);
            fm.dlg = FM_DLG_NONE;
            fm_refresh();
            fm.sel = -1;
            if (r == 0)
                for (int i = 0; i < fm.nent; i++)
                    if (strcmp(fm.ent[i]->name, nm) == 0) { fm.sel = i; break; }
            char b[96];
            snprintf(b, sizeof(b), "%s: '%s'", msg, nm);
            fm_set_status(b);
            fm_dirty_win();
            return;
        }
        fm_dlg_cancel();
        return;
    }
    if (fm.dlg == FM_DLG_DEL) {
        if (fm.sel < 0 || fm.sel >= fm.nent) { fm_dlg_cancel(); return; }
        struct fs_node* n = fm.ent[fm.sel];
        char nm[64];
        snprintf(nm, sizeof(nm), "%s", n->name);
        int r = fs_delete_node(fm.dir, nm);
        fm.dlg = FM_DLG_NONE;
        fm_refresh();
        if (fm.sel >= fm.nent) fm.sel = fm.nent ? fm.nent - 1 : -1;
        char b[96];
        if (r == 0)      snprintf(b, sizeof(b), "Deleted '%s'", nm);
        else             snprintf(b, sizeof(b),
                                  "Cannot delete '%s' (folder not empty?)", nm);
        fm_set_status(b);
        fm_dirty_win();
        return;
    }
    /* FM_DLG_PROP: tombol OK saja */
    fm_dlg_cancel();
}

/* ------------------------------------------------------------------ */
/*  Gambar dialog (dipanggil compose SETELAH semua jendela)            */
/* ------------------------------------------------------------------ */
static void fm_draw_dlg(void) {
    if (!fm.dlg || !wins[APP_FM].open) return;
    int rx, ry, rw, rh;
    fm_dlg_box(&rx, &ry, &rw, &rh);
    int X = (int)wins[APP_FM].x + rx;
    int Y = (int)wins[APP_FM].y + ry;

    /* Dialog mengikuti tema jendela (terang), bukan panel gelap:
     * jendela Files body-nya F0F0F0 dgn teks gelap, jadi dialog harus
     * sama agar tidak tampak "nyangkut" di tengah jendela terang. */
    bb_fill_round(X - 3, Y - 3, rw + 6, rh + 6, gray(120));   /* bingkai */
    bb_fill_round(X, Y, rw, rh, gray(245));                   /* panel   */

    const char* title =
        fm.dlg == FM_DLG_NEW  ? "New Folder" :
        fm.dlg == FM_DLG_REN  ? "Rename"     :
        fm.dlg == FM_DLG_DEL  ? "Delete"     : "Properties";
    text_blit(X + 16, Y + 14, title, 0x181818);
    bb_fill_rect(X + 16, Y + 34, rw - 32, 1, gray(190));

    if (fm.dlg == FM_DLG_NEW || fm.dlg == FM_DLG_REN) {
        text_blit(X + 16, Y + 42, "Enter a name:", gray(90));
        int fx = X + 16, fy = Y + 62, fw = rw - 32, fh = 26;
        bb_fill_round(fx - 1, fy - 1, fw + 2, fh + 2, gray(168));
        bb_fill_round(fx, fy, fw, fh, 0xFFFFFFFFu);
        /* teks + kursor, tampilan ekor bila melebihi lebar kolom */
        char shown[64];
        int capch = (fw - 12) / 8;
        fm_fit_tail(fm.dbuf, capch, shown, (int)sizeof(shown));
        text_blit(fx + 6, fy + 5, shown, 0x181818);
        int cx = fx + 6 + (int)strlen(shown) * 8;
        if (cx < fx + fw - 6) bb_fill_rect(cx, fy + 4, 2, fh - 8, gray(40));
        text_blit(X + 16, Y + rh - 52, "Enter = OK     Esc = Cancel", gray(110));
    } else if (fm.dlg == FM_DLG_DEL) {
        char l1[64], l2[96];
        if (fm.sel >= 0 && fm.sel < fm.nent) {
            fm_fit_tail(fm.ent[fm.sel]->name, 34, l1, (int)sizeof(l1));
            snprintf(l2, sizeof(l2), "Delete '%s' permanently?", l1);
        } else {
            snprintf(l2, sizeof(l2), "Nothing selected.");
        }
        text_blit(X + 16, Y + 46, l2, 0x181818);
        text_blit(X + 16, Y + 70, "This cannot be undone.", gray(110));
    } else {
        /* properti */
        if (fm.sel >= 0 && fm.sel < fm.nent) {
            struct fs_node* n = fm.ent[fm.sel];
            char a[64], b[96], c[40], d[48];
            fm_fit_tail(n->name, 34, a, (int)sizeof(a));
            fs_get_path(n, b, sizeof(b));
            fm_fit_head(b, 40, b, (int)sizeof(b));
            if (n->is_dir) snprintf(c, sizeof(c), "-");
            else fm_size_str(n->size, c, (int)sizeof(c));
            snprintf(d, sizeof(d), "%s", fm_typename(n));
            text_blit(X + 16, Y + 46, "Name :", gray(90));
            text_blit(X + 16 + 56, Y + 46, a, 0x181818);
            text_blit(X + 16, Y + 68, "Path :", gray(90));
            text_blit(X + 16 + 56, Y + 68, b, 0x181818);
            text_blit(X + 16, Y + 90, "Size :", gray(90));
            text_blit(X + 16 + 56, Y + 90, c, 0x181818);
            text_blit(X + 16, Y + 112, "Type :", gray(90));
            text_blit(X + 16 + 56, Y + 112, d, 0x181818);
        }
        text_blit(X + 16, Y + rh - 52, "Open files from the shell.", gray(110));
    }

    /* tombol OK / Batal (Batal tidak ada di dialog properti) */
    int nbtn = (fm.dlg == FM_DLG_PROP) ? 1 : 2;
    for (int k = 0; k < nbtn; k++) {
        int which = (nbtn == 1) ? 0 : k;         /* 0 = OK, 1 = Batal */
        int bx, by, bw2, bh2;
        fm_dlg_btn(which, &bx, &by, &bw2, &bh2);
        int sx = (int)wins[APP_FM].x + bx;
        int sy = (int)wins[APP_FM].y + by;
        uint8_t bg = (fm.hdbtn == which) ? 172 : 232;   /* hover = pekat,
                                                             sama dgn toolbar */
        bb_fill_round(sx, sy, bw2, bh2, gray(168));
        bb_fill_round(sx + 1, sy + 1, bw2 - 2, bh2 - 2, gray(bg));
        const char* lb = which ? "Cancel" : "OK";
        int ll = (int)strlen(lb);
        text_blit(sx + (bw2 - ll * 8) / 2, sy + 5, lb, 0x101010);
    }
}

/* ------------------------------------------------------------------ */
/*  Gambar isi jendela (dipanggil compose, sudah ter-clip)             */
/* ------------------------------------------------------------------ */
static void fm_draw(Win& w) {
    int X = (int)w.x, Y = (int)w.y;
    int W = (int)w.w, H = (int)w.h;
    int listx0 = fm_list_x0();
    int listx1 = fm_list_x1(w);
    int col_type = listx1 - 96;              /* kolom "Type" (kiri)     */
    int col_size = col_type - 10;            /* ujung kanan kolom "Size"*/
    int name_x   = listx0 + 8;
    int name_w   = col_size - 72 - name_x;
    int rows_y0  = fm_rows_y0();
    int rows_y1  = fm_rows_y1(w);
    int vis      = fm_vis(w);

    /* ---------- toolbar ---------- */
    bb_fill_rect(X, Y + FM_TOOL_Y, W, FM_TOOL_H, gray(214));
    bb_fill_rect(X, Y + FM_TOOL_Y + FM_TOOL_H - 1, W, 1, gray(178));
    for (int b = 0; b < FM_TOOL_N; b++) {
        int bx, by, bw2, bh2;
        fm_tool_rect(w, b, &bx, &by, &bw2, &bh2);
        bool en = fm_tool_enabled(b);
        uint8_t inner = (b == fm.hbtn && en) ? 172 : (en ? 232 : 222);
        bb_fill_round(X + bx, Y + by, bw2, bh2, gray(168));        /* border */
        bb_fill_round(X + bx + 1, Y + by + 1, bw2 - 2, bh2 - 2, gray(inner));
        const char* lb = fm_tool_label(b);
        int ll = (int)strlen(lb);
        text_blit(X + bx + (bw2 - ll * 8) / 2, Y + by + (bh2 - 16) / 2,
                  lb, en ? 0x181818 : 0x9E9E9E);
    }

    /* ---------- location bar ---------- */
    {
        int lx = X + FM_M, ly = Y + FM_LOC_Y, lw = W - 2 * FM_M;
        bb_fill_round(lx, ly, lw, FM_LOC_H, gray(168));
        bb_fill_round(lx + 1, ly + 1, lw - 2, FM_LOC_H - 2, 0xFFFFFFFFu);
        char shown[128];
        fm_fit_head(fm.path, (lw - 40) / 8, shown, (int)sizeof(shown));
        text_blit(lx + 8, ly + 4, shown, 0x202020);
        text_blit(lx + lw - 96, ly + 4, "Location", gray(150));
    }

    /* ---------- sidebar Places ---------- */
    {
        int sx = X + FM_M, sy = Y + FM_BODY_Y;
        int sw = FM_SIDE, sh = rows_y1 - FM_BODY_Y;
        bb_fill_round(sx, sy, sw, sh, gray(236));
        text_blit(sx + 10, sy + 6, "PLACES", gray(140));
        bb_fill_rect(sx + 10, sy + 22, sw - 20, 1, gray(206));
        for (int p = 0; p < FM_PLACES; p++) {
            int py = sy + FM_PLACE_Y + p * FM_PLACE_H;
            bool cur = (strcmp(fm.path, FM_PLACE_P[p]) == 0);
            bool hov = (p == fm.hplace);
            if (cur || hov) {
                uint8_t bg = cur ? (hov ? 180 : 196) : 214;
                bb_fill_round(sx + 4, py, sw - 8, FM_PLACE_H - 2, gray(bg));
            }
            fm_icon_dir(sx + 10, py + 4, gray(110));
            text_blit(sx + 30, py + 3, FM_PLACE_L[p], 0x1E1E1E);
        }
        text_blit(sx + 10, sy + sh - 22, fm.dir && fm.dir->backing
                  ? "FAT32 volume" : "RAM filesystem", gray(150));
    }

    /* ---------- header kolom (klik = ganti urutan sort) ---------- */
    bb_fill_rect(X + listx0, Y + FM_BODY_Y, listx1 - listx0, FM_HDR_H, gray(224));
    {
        int c0, c1, c2;
        fm_col_edges(w, &c0, &c1, &c2);
        int zx[4] = { c0, c1, c2, listx1 };
        for (int c = 0; c < 3; c++) {
            bool act = (c == fm.sort_key);
            if (act)
                bb_fill_rect(X + zx[c], Y + FM_BODY_Y,
                             zx[c + 1] - zx[c], FM_HDR_H, gray(210));
            else if (c == fm.hcol)
                bb_fill_rect(X + zx[c], Y + FM_BODY_Y,
                             zx[c + 1] - zx[c], FM_HDR_H, gray(234));
        }
        bb_fill_rect(X + listx0, Y + FM_BODY_Y + FM_HDR_H - 1,
                     listx1 - listx0, 1, gray(178));
        for (int c = 1; c < 3; c++)
            bb_fill_rect(X + zx[c], Y + FM_BODY_Y + 4, 1, FM_HDR_H - 8,
                         gray(198));
        const char* lb[3] = { "Name", "Size", "Type" };   /* ke-3: 4 huruf  */
        int lx2[3] = { name_x, col_size - 32, col_type + 8 };
        for (int c = 0; c < 3; c++) {
            bool act = (c == fm.sort_key);
            uint32_t fg = act ? 0x101010u : 0x303030u;
            text_blit(X + lx2[c], Y + FM_BODY_Y + 1, lb[c], fg);
            if (act) fm_sort_arrow(X + lx2[c] + 34,
                                   Y + FM_BODY_Y + (FM_HDR_H - 4) / 2,
                                   fm.sort_dir > 0, fg);
        }
    }

    /* ---------- baris ---------- */
    if (fm.nent == 0) {
        text_blit(X + name_x, Y + rows_y0 + 6, "(empty folder)", gray(150));
    }
    for (int r = 0; r < vis; r++) {
        int idx = fm.top + r;
        if (idx >= fm.nent) break;
        struct fs_node* n = fm.ent[idx];
        int ry = Y + rows_y0 + r * FM_ROW_H;
        bool ssel = (idx == fm.sel), shov = (idx == fm.hrow);
        if (ssel || shov) {
            uint8_t bg = ssel ? (shov ? 74 : 96) : 230;
            bb_fill_rect(X + listx0, ry, listx1 - listx0, FM_ROW_H, gray(bg));
        }
        uint32_t fg  = ssel ? 0xFFFFFFu : 0x1E1E1Eu;
        uint32_t fg2 = ssel ? 0xD8D8D8u : gray(100);
        if (n->is_dir) fm_icon_dir(X + name_x, ry + 5,
                                   ssel ? 0xFFFFFFu : gray(110));
        else           fm_icon_file(X + name_x + 1, ry + 4, ssel);
        char nm[64];
        fm_fit_tail(n->name, name_w / 8, nm, (int)sizeof(nm));
        text_blit(X + name_x + 20, ry + 3, nm, fg);
        if (!n->is_dir) {
            char sz[24];
            fm_size_str(n->size, sz, (int)sizeof(sz));
            text_blit(X + col_size - (int)strlen(sz) * 8, ry + 3, sz, fg2);
        }
        text_blit(X + col_type + 8, ry + 3, fm_typename(n), fg2);
    }

    /* ---------- scrollbar ---------- */
    {
        int sbx = X + W - FM_M - FM_SB_W;
        int sby = Y + rows_y0, sbh = rows_y1 - rows_y0;
        bb_fill_round(sbx, sby, FM_SB_W, sbh, gray(230));
        if (fm.nent > vis) {
            int th = sbh * vis / fm.nent;
            if (th < 18) th = 18;
            int ty = sby + (sbh - th) * fm.top / (fm.nent - vis);
            bb_fill_round(sbx + 2, ty, FM_SB_W - 4, th, gray(150));
        }
    }

    /* ---------- status bar ---------- */
    {
        int sty = Y + H - FM_M - FM_STATUS_H;
        bb_fill_round(X + FM_M, sty, W - 2 * FM_M, FM_STATUS_H, gray(224));
        bb_fill_rect(X + FM_M, sty, W - 2 * FM_M, 1, gray(190));
        text_blit(X + FM_M + 8, sty + 3, fm.status, 0x383838);
        char it[40];
        snprintf(it, sizeof(it), "%d items", fm.nent);
        text_blit(X + W - FM_M - 10 - (int)strlen(it) * 8, sty + 3, it, 0x383838);
    }
}

/* ------------------------------------------------------------------ */
/*  Hit-test                                                           */
/* ------------------------------------------------------------------ */
static int fm_row_at(const Win& w, int px, int py) {
    int lx = px - (int)w.x, ly = py - (int)w.y;
    if (lx < fm_list_x0() || lx > fm_list_x1(w)) return -1;
    if (ly < fm_rows_y0() || ly >= fm_rows_y1(w)) return -1;
    int r = (ly - fm_rows_y0()) / FM_ROW_H;
    int idx = fm.top + r;
    if (idx < 0 || idx >= fm.nent) return -1;
    return idx;
}

static int fm_toolbar_at(const Win& w, int px, int py) {
    int lx = px - (int)w.x, ly = py - (int)w.y;
    if (ly < FM_TOOL_Y || ly > FM_TOOL_Y + FM_TOOL_H) return -1;
    for (int b = 0; b < FM_TOOL_N; b++) {
        int bx, by, bw2, bh2;
        fm_tool_rect(w, b, &bx, &by, &bw2, &bh2);
        if (lx >= bx && lx <= bx + bw2 && ly >= by && ly <= by + bh2) return b;
    }
    return -1;
}

static int fm_place_at(const Win& w, int px, int py) {
    int lx = px - (int)w.x;
    if (lx < FM_M || lx > FM_M + FM_SIDE) return -1;
    int ly = py - (int)w.y;
    for (int p = 0; p < FM_PLACES; p++) {
        int ry = fm_place_y(w, p);
        if (ly >= ry && ly <= ry + FM_PLACE_H - 2) return p;
    }
    return -1;
}

/* klik header kolom = ganti kunci urutan (kembalikan FM_SORT_* / -1) */
static int fm_header_at(const Win& w, int px, int py) {
    int lx = px - (int)w.x, ly = py - (int)w.y;
    if (ly < FM_BODY_Y || ly >= FM_BODY_Y + FM_HDR_H) return -1;
    int c0, c1, c2;
    fm_col_edges(w, &c0, &c1, &c2);
    if (lx < c0 || lx > fm_list_x1(w)) return -1;
    if (lx < c1) return FM_SORT_NAME;
    if (lx < c2) return FM_SORT_SIZE;
    return FM_SORT_TYPE;
}

static int fm_scrollbar_at(const Win& w, int px, int py) {
    int lx = px - (int)w.x, ly = py - (int)w.y;
    int sbx = (int)w.w - FM_M - FM_SB_W;
    if (lx < sbx || lx > sbx + FM_SB_W) return 0;
    if (ly < fm_rows_y0() || ly >= fm_rows_y1(w)) return 0;
    return 1;
}

static int fm_dlg_click(int px, int py) {
    int rx, ry, rw, rh;
    fm_dlg_box(&rx, &ry, &rw, &rh);
    int X = (int)wins[APP_FM].x, Y = (int)wins[APP_FM].y;
    int lx = px - X, ly = py - Y;
    int nbtn = (fm.dlg == FM_DLG_PROP) ? 1 : 2;
    for (int k = 0; k < nbtn; k++) {
        int which = (nbtn == 1) ? 0 : k;
        int bx, by, bw2, bh2;
        fm_dlg_btn(which, &bx, &by, &bw2, &bh2);
        if (lx >= bx && lx <= bx + bw2 && ly >= by && ly <= by + bh2) {
            if (which == 0) fm_dlg_ok(); else fm_dlg_cancel();
            return 1;
        }
    }
    (void)rx; (void)ry; (void)rw; (void)rh;
    return 1;                       /* modal: klik di luar diabaikan */
}

/* jalankan aksi toolbar */
static void fm_toolbar_action(int b) {
    if (!fm_tool_enabled(b)) {
        if (b == 5 || b == 6) fm_set_status("Select an item first");
        else fm_set_status("Nothing to go back to");
        return;
    }
    switch (b) {
        case 0: fm_back(); break;
        case 1: fm_up();   break;
        case 2: fm_go("/", 1); break;
        case 3: {
            fm_refresh();
            char buf[96];
            snprintf(buf, sizeof(buf), "Reloaded - %d item%s", fm.nent,
                     fm.nent == 1 ? "" : "s");
            fm_set_status(buf);
            fm_dirty_win();
            break;
        }
        case 4:
            fm.dlg = FM_DLG_NEW;
            fm.dbuf[0] = '\0'; fm.dlen = 0;
            fm.hdbtn = -1;
            fm_dirty_win();
            break;
        case 5:
            if (fm.sel >= 0 && fm.sel < fm.nent) {
                fm.dlg = FM_DLG_REN;
                snprintf(fm.dbuf, sizeof(fm.dbuf), "%s", fm.ent[fm.sel]->name);
                fm.dlen = (int)strlen(fm.dbuf);
                fm.hdbtn = -1;
                fm_dirty_win();
            }
            break;
        case 6:
            if (fm.sel >= 0 && fm.sel < fm.nent) {
                fm.dlg = FM_DLG_DEL;
                fm.dbuf[0] = '\0'; fm.dlen = 0;
                fm.hdbtn = -1;
                fm_dirty_win();
            }
            break;
    }
}

/* klik header kolom: ganti kunci urutan / arah, lalu urutkan ulang.
 * Seleksi disimpan lewat pointer node supaya baris terpilih tidak lompat. */
static void fm_header_click(int col) {
    if (col != FM_SORT_NAME && col != FM_SORT_SIZE && col != FM_SORT_TYPE) return;
    struct fs_node* keep =
        (fm.sel >= 0 && fm.sel < fm.nent) ? fm.ent[fm.sel] : (struct fs_node*)0;
    if (fm.sort_key == col) fm.sort_dir = -fm.sort_dir;
    else {
        fm.sort_key = col;
        fm.sort_dir = (col == FM_SORT_SIZE) ? -1 : 1;  /* Size: besar dulu */
    }
    fm_sort();
    fm.sel = -1;
    if (keep)
        for (int i = 0; i < fm.nent; i++)
            if (fm.ent[i] == keep) { fm.sel = i; break; }
    int vis = fm_vis(wins[APP_FM]);
    if (fm.top > fm.nent - vis) fm.top = fm.nent - vis;
    if (fm.top < 0) fm.top = 0;
    fm.hrow = -1;
    char b[96];
    if (fm.sort_key == FM_SORT_SIZE)
        snprintf(b, sizeof(b), "Sorted by Size (%s first)",
                 fm.sort_dir > 0 ? "smallest" : "largest");
    else if (fm.sort_key == FM_SORT_TYPE)
        snprintf(b, sizeof(b), "Sorted by Type (%s)",
                 fm.sort_dir > 0 ? "A to Z" : "Z to A");
    else
        snprintf(b, sizeof(b), "Sorted by Name (%s)",
                 fm.sort_dir > 0 ? "A to Z" : "Z to A");
    fm_set_status(b);
    fm_dirty_win();
}

/* ------------------------------------------------------------------ */
/*  Menu konteks (klik kanan)                                          */
/*  Digambar di compose() pada koordinat layar (setelah semua jendela), */
/*  sama seperti dialog modal — jadi tidak ter-clip lapisan jendela.    */
/* ------------------------------------------------------------------ */
static const char* fm_ctx_label(int i) {
    switch (i) {
        case 0:  return "Open";
        case 1:  return "Rename...";
        case 2:  return "Delete";
        default: return "Properties";
    }
}

static int fm_ctx_h(void) { return FM_CTX_PAD * 2 + FM_CTX_N * FM_CTX_H; }

static void fm_ctx_dirty(void) {
    if (!fm.ctx) return;
    mark_dirty(fm.ctx_x - 4, fm.ctx_y - 4, FM_CTX_W + 8, fm_ctx_h() + 8);
}

/* item di bawah kursor, -1 bila kursor di luar menu */
static int fm_ctx_at(int px, int py) {
    if (!fm.ctx) return -1;
    if (px < fm.ctx_x || px >= fm.ctx_x + FM_CTX_W) return -1;
    int y0 = fm.ctx_y + FM_CTX_PAD;
    if (py < y0 || py >= y0 + FM_CTX_N * FM_CTX_H) return -1;
    return (py - y0) / FM_CTX_H;
}

static void fm_ctx_close(void) {
    if (!fm.ctx) return;
    fm_ctx_dirty();                 /* tandai dulu: rect lama harus hilang */
    fm.ctx = 0;
    fm.ctx_item = -1;
}

/* jalankan aksi item, lalu tutup menu */
static void fm_ctx_run(int item) {
    fm_ctx_close();
    if (item < 0) return;
    switch (item) {
        case 0:  fm_open_selected(); break;      /* Open        */
        case 1:  fm_toolbar_action(5); break;    /* Rename...   */
        case 2:  fm_toolbar_action(6); break;    /* Delete      */
        default:                                 /* Properties  */
            if (fm.sel >= 0 && fm.sel < fm.nent) {
                fm.dlg = FM_DLG_PROP;
                fm.dbuf[0] = '\0'; fm.dlen = 0;
                fm.hdbtn = -1;
                fm_dirty_win();
            }
            break;
    }
}

static void fm_ctx_click(int px, int py) { fm_ctx_run(fm_ctx_at(px, py)); }

/* buka menu di (px,py) bila kena baris di daftar isi */
static void fm_ctx_open(int px, int py) {
    Win& w = wins[APP_FM];
    int idx = fm_row_at(w, px, py);
    if (idx < 0) return;                       /* di luar baris: tanpa menu */
    if (idx != fm.sel) { fm.sel = idx; fm_dirty_list(); }
    fm.last_row = -1;                          /* klik kanan bukan dblclick */
    fm.last_tick = -1000;

    /* clamp ke dalam jendela agar tetap rapi di dekat kursor */
    int hh = fm_ctx_h();
    int x = px, y = py;
    if (x + FM_CTX_W > (int)w.x + (int)w.w - 6)
        x = (int)w.x + (int)w.w - 6 - FM_CTX_W;
    if (y + hh > (int)w.y + (int)w.h - 6)
        y = (int)w.y + (int)w.h - 6 - hh;
    if (x < (int)w.x + 6)          x = (int)w.x + 6;
    if (y < (int)w.y + TITLE_H + 4) y = (int)w.y + TITLE_H + 4;

    int it = -1;
    if (px >= x && px < x + FM_CTX_W) {
        int iy0 = y + FM_CTX_PAD;
        if (py >= iy0 && py < iy0 + FM_CTX_N * FM_CTX_H)
            it = (py - iy0) / FM_CTX_H;
    }
    fm.ctx_x = x;
    fm.ctx_y = y;
    fm.ctx_item = it;
    fm.ctx = 1;
    fm_ctx_dirty();

    char b[96];
    snprintf(b, sizeof(b), "'%s' - right-click menu", fm.ent[idx]->name);
    fm_set_status(b);
}

static void fm_ctx_draw(void) {
    if (!fm.ctx) return;
    Win& w = wins[APP_FM];
    if (!w.open || w.minimized) return;
    int hh = fm_ctx_h();
    int x = fm.ctx_x, y = fm.ctx_y;
    bb_fill_round(x - 3, y - 3, FM_CTX_W + 6, hh + 6, gray(120));
    bb_fill_round(x, y, FM_CTX_W, hh, gray(245));
    for (int i = 0; i < FM_CTX_N; i++) {
        int iy = y + FM_CTX_PAD + i * FM_CTX_H;
        bool hi = (i == fm.ctx_item);
        if (hi) bb_fill_round(x + 3, iy, FM_CTX_W - 6, FM_CTX_H, gray(180));
        text_blit(x + 14, iy + 4, fm_ctx_label(i), hi ? 0x101010 : 0x181818);
    }
}

/* klik di dalam jendela FM */
static void fm_click(int px, int py, uint32_t now) {
    Win& w = wins[APP_FM];
    int lx = px - (int)w.x, ly = py - (int)w.y;
    if (ly < FM_TOOL_Y || ly > (int)w.h - FM_M) return;

    int b = fm_toolbar_at(w, px, py);
    if (b >= 0) { fm_toolbar_action(b); return; }

    int p = fm_place_at(w, px, py);
    if (p >= 0) { fm_go(FM_PLACE_P[p], 1); return; }

    int hc = fm_header_at(w, px, py);
    if (hc >= 0) { fm_header_click(hc); return; }

    if (fm_scrollbar_at(w, px, py)) {
        fm.scroll = 1;
        int vis = fm_vis(w);
        int rows_y0 = fm_rows_y0(), rows_y1 = fm_rows_y1(w);
        int rel = ly - rows_y0;
        if (fm.nent > vis) {
            int th = (rows_y1 - rows_y0) * vis / fm.nent;
            if (th < 18) th = 18;
            int top = (rel - th / 2) * (fm.nent - vis) / (rows_y1 - rows_y0 - th);
            if (top < 0) top = 0;
            if (top > fm.nent - vis) top = fm.nent - vis;
            if (top != fm.top) { fm.top = top; fm_dirty_list(); }
        }
        return;
    }

    if (lx >= fm_list_x0() && lx <= fm_list_x1(w) &&
        ly >= fm_rows_y0() && ly < fm_rows_y1(w)) {
        int idx = fm_row_at(w, px, py);
        if (idx < 0) return;
        int dbl = (idx == fm.last_row && (int)(now - fm.last_tick) < 400);
        fm.last_row = idx;
        fm.last_tick = (int)now;
        if (idx != fm.sel) { fm.sel = idx; fm_dirty_list(); }
        if (dbl) fm_open_selected();
        else {
            char b2[96];
            struct fs_node* n = fm.ent[idx];
            if (n->is_dir) snprintf(b2, sizeof(b2), "'%s' - double-click to open", n->name);
            else {
                char sz[24];
                fm_size_str(n->size, sz, sizeof(sz));
                snprintf(b2, sizeof(b2), "%s - %s, %s", n->name, sz, fm_typename(n));
            }
            fm_set_status(b2);
        }
    }
}

/* keyboard */
static void fm_key(int key) {
    /* menu konteks memegang keyboard: ESC tutup, Enter jalankan item hover */
    if (fm.ctx) {
        if (key == 27)              { fm_ctx_close(); return; }
        if (key == 13 || key == 10) { fm_ctx_run(fm.ctx_item); return; }
        return;
    }
    if (fm.dlg) {
        if (fm.dlg == FM_DLG_NEW || fm.dlg == FM_DLG_REN) {
            if (key == 13 || key == 10) { fm_dlg_ok(); return; }
            if (key == 27)              { fm_dlg_cancel(); return; }
            if (key == 8) {
                if (fm.dlen > 0) { fm.dbuf[--fm.dlen] = '\0'; fm_dirty_dlg(); }
                return;
            }
            if (key >= 32 && key < 127 && fm.dlen < (int)sizeof(fm.dbuf) - 1) {
                fm.dbuf[fm.dlen++] = (char)key;
                fm.dbuf[fm.dlen] = '\0';
                fm_dirty_dlg();
            }
            return;
        }
        if (key == 13 || key == 10 || key == 27) fm_dlg_ok();
        return;
    }

    if (fm.nent == 0) return;
    int vis = fm_vis(wins[APP_FM]);
    int old = fm.sel;
    switch (key) {
        case -1: fm.sel = (fm.sel < 0) ? 0 : fm.sel - 1; break;         /* Up   */
        case -2: fm.sel = (fm.sel < 0) ? 0 : fm.sel + 1; break;         /* Down */
        case -7: fm.sel = fm.sel - vis; break;                          /* PgUp */
        case -8: fm.sel = fm.sel + vis; break;                          /* PgDn */
        case -5: fm.sel = 0; break;                                     /* Home */
        case -6: fm.sel = fm.nent - 1; break;                           /* End  */
        case 13: case 10: fm_open_selected(); return;                   /* Enter*/
        case 8:  fm_up(); return;                                       /* Bksp */
        case -3: fm_back(); return;                                     /* Left */
        case -9: fm_toolbar_action(6); return;                          /* Del  */
        default: return;
    }
    if (fm.sel < 0) fm.sel = 0;
    if (fm.sel > fm.nent - 1) fm.sel = fm.nent - 1;
    if (fm.sel != old) {
        /* jaga baris terpilih tetap terlihat */
        if (fm.sel < fm.top) fm.top = fm.sel;
        if (fm.sel >= fm.top + vis) fm.top = fm.sel - vis + 1;
        if (fm.top < 0) fm.top = 0;
        fm_dirty_list();
    }
}

static void fm_hover(int px, int py, int* hrow, int* hbtn, int* hpl, int* hdb,
                     int* hcol) {
    *hrow = *hbtn = *hpl = *hdb = -1;
    if (hcol) *hcol = -1;
    if (fm.dlg) {
        int nbtn = (fm.dlg == FM_DLG_PROP) ? 1 : 2;
        for (int k = 0; k < nbtn; k++) {
            int which = (nbtn == 1) ? 0 : k;
            int bx, by, bw2, bh2;
            fm_dlg_btn(which, &bx, &by, &bw2, &bh2);
            int sx = (int)wins[APP_FM].x + bx;
            int sy = (int)wins[APP_FM].y + by;
            if (px >= sx && px <= sx + bw2 && py >= sy && py <= sy + bh2)
                *hdb = which;
        }
        return;
    }
    Win& w = wins[APP_FM];
    *hbtn  = fm_toolbar_at(w, px, py);
    *hpl   = fm_place_at(w, px, py);
    *hcol  = fm_header_at(w, px, py);
    *hrow  = fm_row_at(w, px, py);
}

static void fm_init(void) {
    fm.dir = NULL;
    fm.path[0] = '\0';
    fm.hist_n = 0;
    fm.nent = 0;
    fm.sel = -1; fm.top = 0;
    fm.hrow = fm.hbtn = fm.hplace = fm.hdbtn = -1;
    fm.hcol = -1;
    fm.scroll = 0;
    fm.last_row = -2; fm.last_tick = -1000;
    fm.dlg = FM_DLG_NONE;
    fm.dbuf[0] = '\0'; fm.dlen = 0;
    fm.ctx = 0; fm.ctx_item = -1; fm.ctx_x = fm.ctx_y = 0;
    fm.sort_key = FM_SORT_NAME;
    fm.sort_dir = 1;
    snprintf(fm.status, sizeof(fm.status), "Ready");
    fm_go("/", 0);
}

/* ================================================================== */
/*  TERMINAL — shell task di konsolnya sendiri, jendela ini = layarnya  */
/*  ================================================================== */
/*  Shell asli (shell_entry di shell.cpp) dijalankan sebagai task       */
/*  terpisah lewat task_create_shell() — jalur yang sama dengan F1.      */
/*  Outputnya masuk cell mirror konsolnya (tak pernah ke framebuffer),   */
/*  inputnya dari ring konsolnya sendiri. Nol perubahan di shell.cpp.    */
/* ================================================================== */

/* Palet 16 warna VGA — nibble tinggi atribut = bg, rendah = fg. */
static const uint32_t TERM_VGA_RGB[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static void term_init(void) {
    term.con = -1;
    term.pid = -1;
    term.last_seq = 0;
    term.err = 0;
    term.top = 0;
    term.follow = 1;
    term.e0 = 0;
}

/* Jendela dibuka -> spawn shell task + konsolnya. Konsolnya TIDAK
 * diaktifkan (konsol aktif tetap milik desktop) dan diberi geometri
 * 80x24 = ukuran isi jendela, sehingga wrapping baris shell persis
 * mengikuti jendela (bukan lebar layar 128..160 kolom). */
static void term_open(void) {
    if (term.con >= 0) return;        /* sudah hidup */
    term.err = 0;
    struct Task* t = task_create_shell();
    if (!t) {
        term.err = 1;                 /* task/konsol penuh — pesan di jendela */
        return;
    }
    term.con = t->console;
    term.pid = t->pid;
    console_set_size(term.con, TERM_ROWS, TERM_COLS);
    /* fase 2: ring riwayat (baris yang tergeser keluar) — di-alokasi di
     * sini (konteks task) lalu diisi otomatis oleh scroll_cells(). */
    term.top = 0;
    term.follow = 1;
    term.e0 = 0;
    term.sb_hold = 0;
    {
        /* diagnostik: gagal alokasi ring riwayat -> term_scroll() selalu
         * no-op (off ke-clamp jadi 0) dan badge tak pernah muncul. */
        int rc = console_scrollback_enable(term.con, TERM_SB_LINES);
        char db[72];
        snprintf(db, sizeof(db), "[term] sb enable=%d con=%d cap=%d\n",
                 rc, term.con, console_scrollback_count(term.con));
        serial_puts(db);
    }
    term.last_seq = console_edit_seq(term.con);
}

/* Jendela ditutup / desktop keluar -> akhiri SELURUH sesi terminal:
 * shell task DAN semua program .mrp yang dijalankannya (semuanya memakai
 * konsol yang sama, diwarisi dari spawner). Scheduler melepas konsol +
 * slot task untuk tiap korban (task_release_resources). Membunuh hanya
 * shell-nya akan menyisakan program grafis yang parkir di draw gate
 * dengan console id yang slotnya baru dilepas — task usang yang bisa
 * terbangun oleh console_activate() pada sesi lain yang memakai ulang id. */
static void term_shutdown(void) {
    if (term.con >= 0) task_kill_console(term.con);
    else if (term.pid > 0) task_kill(term.pid);
    term.pid = -1;
    term.con = -1;
    term.last_seq = 0;
    term.err = 0;
    term.top = 0;
    term.follow = 1;
    term.e0 = 0;
    term.sb_hold = 0;
}

/* ---- fase 2: TAMPILAN SCROLLBACK --------------------------------
 * Baris virtual = riwayat (0 = tertua .. nhist-1) lalu baris live;
 * NOMOR baris absolutnya stabil sepanjang riwayat tumbuh, jadi tampilan
 * bisa di-PIN (term.follow = 0, baris `term.top` di paling atas) tanpa
 * ikut menggeser keluar saat output baru terus datang — persis seperti
 * xterm. follow = 1 berarti tampilan LIVE (baris terbaru di bawah).
 * term_off() = jumlah baris riwayat di ATAS tampilan (0 = live).   */
static int term_off(void) {
    if (term.con < 0 || term.follow) return 0;
    int n = console_scrollback_count(term.con);
    int off = n - term.top;
    if (off < 0) off = 0;
    if (off > n) off = n;
    return off;
}

static void term_scroll_to(int off) {
    int n = (term.con >= 0) ? console_scrollback_count(term.con) : 0;
    if (off < 0) off = 0;
    if (off > n) off = n;
    if (off == term_off()) return;
    term.follow = (off == 0);
    term.top = n - off;
    {
        char db[64];
        snprintf(db, sizeof(db), "[term] scroll off=%d n=%d\n", off, n);
        serial_puts(db);
    }
    Win& w = wins[APP_TERM];
    mark_dirty((int)w.x, (int)w.y, (int)w.w, (int)w.h);
}

/* delta > 0 = naik ke riwayat (roda ke atas / PgUp), < 0 = turun. */
static void term_scroll(int delta) { term_scroll_to(term_off() + delta); }

/* Modifier yang TIDAK mengeluarkan tampilan dari mode riwayat (menekan
 * Shift/Ctrl/Alt saja tidak boleh melompat ke baris terbaru). */
static int term_is_modifier(int sc) {
    return sc == 0x2A || sc == 0x36 || sc == 0x1D || sc == 0x38 ||
           sc == 0xAA || sc == 0xB6 || sc == 0x9D || sc == 0xB8;
}

/* Teruskan scancode MENTAH dari ring konsol desktop ke ring konsol
 * terminal — dipanggil tiap frame selama terminal fokus. Decoding
 * (shift/caps/panah) terjadi di getkey() milik task terminal, jadi
 * termasuk E0-prefix diteruskan apa adanya. F1/F2 tak pernah sampai
 * sini: IRQ1 sudah menangkapnya sebelum masuk ring.
 *
 * Pengecualian fase 2: PgUp (E0 49) / PgDn (E0 51) DIPEGANG di sini
 * sebagai scrollback view (roda mouse memakai jalur yang sama lewat
 * term_scroll()). Selain itu, mengetik apa pun saat tampilan sedang
 * di riwayat mengembalikannya ke baris terbaru (live). */
static void term_forward_keys(void) {
    if (term.con < 0) return;
    for (int n = 0; n < 8; n++) {       /* batasi per frame */
        int sc = keyboard_read_byte_noblock();
        if (sc < 0) break;

        if (term.e0) {                  /* byte kedua dari pasangan E0-xx */
            term.e0 = 0;
            int brk  = (sc & 0x80) ? 1 : 0;
            int code = sc & 0x7F;

            /* break (tombol DILEPAS) dari PgUp/PgDn yang sudah kita
             * telan: buang diam-diam. Tanpa ini E0 C9 jatuh ke cabang
             * "kunci E0 asing" di bawah dan langsung menarik tampilan
             * kembali ke live — PgUp jadi tak pernah terlihat. */
            if (term.sb_hold && brk && code == term.sb_hold) {
                term.sb_hold = 0;
                continue;
            }
            if (!brk && code == 0x49) {        /* PgUp (make) */
                term.sb_hold = 0x49;
                serial_puts("[term] key PgUp\n");
                term_scroll(TERM_SB_PAGE);
                continue;
            }
            if (!brk && code == 0x51) {        /* PgDn (make) */
                term.sb_hold = 0x51;
                serial_puts("[term] key PgDn\n");
                term_scroll(-TERM_SB_PAGE);
                continue;
            }
            keyboard_push_scancode(term.con, 0xE0);
            keyboard_push_scancode(term.con, (uint8_t)sc);
            /* HANYA make asing yang mengembalikan tampilan ke live;
             * byte break hanyalah "tombol dilepas" — tak boleh merusak
             * mode riwayat (panah Home/End dst tetap keluar riwayat). */
            if (!brk) {
                if (term_off() > 0) term_scroll_to(0);
                {
                    char db[28];
                    snprintf(db, sizeof(db), "[term] e0=%02x\n", code);
                    serial_puts(db);
                }
            }
            continue;
        }
        if (sc == 0xE0) { term.e0 = 1; continue; }

        if (term_off() > 0 && sc < 0x80 && !term_is_modifier(sc))
            term_scroll_to(0);          /* mengetik -> kembali ke live */

        keyboard_push_scancode(term.con, (uint8_t)sc);
    }
}

/* Gambar isi terminal dari mirror konsol: 1 sel = glyph 8x16. */
static void term_draw(Win& w) {
    int cx = (int)w.x;
    int cy = (int)w.y + (int)TITLE_H;
    int cols = (int)(w.w / 8);
    int rows = (int)((w.h - TITLE_H) / 16);
    if (cols > TERM_COLS) cols = TERM_COLS;
    if (rows > TERM_ROWS) rows = TERM_ROWS;
    if (cols <= 0 || rows <= 0) return;

    /* body: hitam pekat (bg default console) */
    bb_fill_rect(cx, cy, cols * 8, rows * 16, 0x000000);

    if (term.con < 0) {
        const char* msg = term.err
            ? "Terminal: gagal membuat shell (task table penuh)"
            : "Terminal: starting...";
        text_blit(cx + 8, cy + 8, msg, 0xAAAAAA);
        return;
    }

    /* ---- fase 2: campurkan RIWAYAT dengan mirror live -------------
     * Buffer virtual = nhist baris riwayat (0 = tertua) lalu baris live;
     * baris layar r menampilkan baris virtual vidx = top + r:
     *   vidx <  nhist -> riwayat baris vidx   (di-pin saat scrollback)
     *   vidx >= nhist -> mirror live baris (vidx - nhist)
     * follow = 1 -> top = nhist (tampilan LIVE, identik dengan versi
     * sebelum fase 2).                                                 */
    int nhist = console_scrollback_count(term.con);
    int top   = term.follow ? nhist : term.top;
    if (top > nhist) top = nhist;
    if (top < 0)     top = 0;

    for (int r = 0; r < rows; r++) {
        int vidx = top + r;
        int from_hist = (vidx < nhist);
        uint8_t hch[TERM_COLS], hat[TERM_COLS];
        if (from_hist) {
            for (int i = 0; i < TERM_COLS; i++) { hch[i] = ' '; hat[i] = 0x0F; }
            console_scrollback_line(term.con, vidx, hch, hat, TERM_COLS);
        }
        for (int c = 0; c < cols; c++) {
            uint8_t ch = ' ', attr = 0x0F;
            if (from_hist) {
                ch   = hch[c];
                attr = hat[c];
            } else if (console_read_cell(term.con, vidx - nhist, c,
                                         &ch, &attr) != 0) {
                ch = ' ';
                attr = 0x0F;
            }
            int px = cx + c * 8, py = cy + r * 16;
            uint32_t fg = TERM_VGA_RGB[attr & 0x0F];
            uint32_t bg = TERM_VGA_RGB[(attr >> 4) & 0x0F];
            if (((attr >> 4) & 0x0F) != 0) bb_fill_rect(px, py, 8, 16, bg);
            if (ch > 32) {                     /* > spasi: perlu glyph */
                char s[2] = { (char)ch, 0 };
                text_blit(px, py, s, fg);
            } else if (ch != 0 && ch != ' ' && ch != '\n' && ch != '\r') {
                char s[2] = { '?', 0 };        /* kontrol tak ter-render */
                text_blit(px, py, s, fg);
            }
        }
    }

    /* kursor: blok terbalik di posisi kursor konsol (tanpa blink —
     * konsolnya bukan yang aktif, jadi ISR blink tidak menyentuhnya).
     * Hanya saat tampilan LIVE (follow): kursor selalu ada di baris
     * terakhir riwayat, jadi ia tidak ikut ketika tampilan di-pin. */
    int crow = 0, ccol = 0, cvis = 0;
    console_cursor(term.con, &crow, &ccol, &cvis);
    if (term.follow && cvis && crow >= 0 && crow < rows && ccol >= 0 && ccol < cols) {
        uint8_t ch = ' ', attr = 0x0F;
        console_read_cell(term.con, crow, ccol, &ch, &attr);
        uint32_t fg = TERM_VGA_RGB[attr & 0x0F];
        uint32_t bg = TERM_VGA_RGB[(attr >> 4) & 0x0F];
        int px = cx + ccol * 8, py = cy + crow * 16;
        bb_fill_rect(px, py, 8, 16, fg);
        if (ch > 32) {
            char s[2] = { (char)ch, 0 };
            text_blit(px, py, s, bg);
        }
    }
}

/* ---- lingkaran terisi r kecil (tombol close/minimize titlebar) ---- */
static void bb_fill_circle(int ccx, int ccy, int r, uint32_t rgb) {
    for (int dy = -r; dy <= r; dy++) {
        int d2 = r * r - dy * dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= d2) dx++;
        bb_fill_rect(ccx - dx, ccy + dy, 2 * dx + 1, 1, rgb);
    }
}

/* Chrome jendela Terminal TANPA layer (lihat build_win_layers): titlebar
 * gradasi horizontal + label + lingkaran close/minimize — identik dengan
 * render_win_layer(), digambar langsung ke backbuffer dan ter-clip ke
 * region kotor. Sudut membulat r=6 persis seperti layer jendela lain. */
static void term_chrome(Win& w, int focused) {
    int x0 = (int)w.x, y0 = (int)w.y, lw = (int)w.w;
    if (lw <= 0) return;
    uint8_t ta = (uint8_t)(focused ? G_TTL_AF : G_TTL_UF);
    uint8_t tb = (uint8_t)(focused ? G_TTL_AT : G_TTL_UT);

    /* dasar titlebar hitam — sliver sudut membulat menampilkan hitam
     * (= warna body terminal), bukan wallpaper di belakangnya */
    bb_fill_rect(x0, y0, lw, TITLE_H, 0x000000);

    static const int ins[6] = {2, 1, 1, 0, 0, 0};
    for (int x = 0; x < lw; x++) {
        int d = (x < 6) ? ins[x] : ((x >= lw - 6) ? ins[lw - 1 - x] : 0);
        uint8_t g = (uint8_t)(ta + ((int)tb - ta) * x / (lw > 1 ? lw - 1 : 1));
        bb_fill_rect(x0 + x, y0 + d, 1, TITLE_H - 2 * d,
                     ((uint32_t)g << 16) | ((uint32_t)g << 8) | g);
    }

    uint32_t ttl_col = focused ? 0x181818 : 0xD8D8D8;
    uint32_t x_col   = focused ? 0xFFFFFF : 0xE0E0E0;
    uint32_t m_col   = focused ? 0x101010 : 0xE0E0E0;
    text_blit(x0 + 12, y0 + 8, APP_NAMES[APP_TERM], ttl_col);
    /* fase 2: penanda scrollback di titlebar — chip hitam + teks putih
     * agar kontras tinggi, DAN sejajar grid OCR 8x16 (x kelipatan 8,
     * y = y0+16 = baris layar ke-7) sehingga terbaca pengujian otomatis.
     * Teksnya "scrollback -N baris"; N = baris riwayat di atas tampilan. */
    {
        int off = term_off();
        if (off > 0) {
            char sb[40];
            snprintf(sb, sizeof(sb), "scrollback -%d baris", off);
            int len = 0;
            while (sb[len]) len++;
            bb_fill_rect(x0 + 84, y0 + 12, len * 8 + 8, 20, 0x000000);
            text_blit(x0 + 88, y0 + 16, sb, 0xFFFFFF);
        }
    }
    bb_fill_circle(x0 + lw - 20, y0 + TITLE_H / 2, 7,
                   (uint32_t)(focused ? G_CLOSE_F : G_CLOSE_U) * 0x010101u);
    bb_fill_circle(x0 + lw - 42, y0 + TITLE_H / 2, 7,
                   (uint32_t)(focused ? G_MINI_F : G_MINI_U) * 0x010101u);
    text_blit(x0 + lw - 23, y0 + 12, "x", x_col);
    text_blit(x0 + lw - 45, y0 + 12, "-", m_col);
}

static void compose(int rx, int ry, int rw, int rh) {
    /* window dgn layer "varian tunggal": render ulang titlebar bila
     * status fokusnya berubah (sekali per klik, bukan per frame) */
    sync_layer_focus();

    /* clip = region yang direkomposisi */
    g_cl_x0 = rx; g_cl_y0 = ry;
    g_cl_x1 = rx + rw; g_cl_y1 = ry + rh;
    if (g_cl_x0 < 0) g_cl_x0 = 0;
    if (g_cl_y0 < 0) g_cl_y0 = 0;
    if ((uint32_t)g_cl_x1 > g_W) g_cl_x1 = (int)g_W;
    if ((uint32_t)g_cl_y1 > g_H) g_cl_y1 = (int)g_H;

    /* 1. background (opaque — raw copy) */
    copy_region(g_bb, g_bb_stride, L_bg, g_bb_stride, 0, 0, (int)g_W, (int)g_H,
                0, 0, 0);

    /* 2. jendela z kecil dulu (alpha fast-path) */
    sort_z();
    for (int k = 0; k < APP_COUNT; k++) {
        int i = win_z_order[k];
        Win& w = wins[i];
        if (!w.open || w.minimized) continue;

        /* intersect region kotor dengan rect jendela */
        int wx0 = (int)w.x, wy0 = (int)w.y;
        int wx1 = (int)(w.x + w.w), wy1 = (int)(w.y + w.h);
        int ix0 = wx0 > rx ? wx0 : rx;
        int iy0 = wy0 > ry ? wy0 : ry;
        int ix1 = wx1 < rx + rw ? wx1 : rx + rw;
        int iy1 = wy1 < ry + rh ? wy1 : ry + rh;
        if (ix0 >= ix1 || iy0 >= iy1) continue;

        if (i == APP_TERM) {
            /* Terminal TANPA layer (arena penuh — lihat build_win_layers):
             * chrome titlebar digambar langsung, lalu isi = mirror konsol;
             * keduanya ter-clip ke region kotor ini, jadi idempoten. */
            term_chrome(w, i == focus_idx);
            term_draw(w);
            continue;
        }

        uint32_t* src = L_win[i][(i == focus_idx) ? 1 : 0];
        if (!src) src = L_win[i][0];     /* fallback varian tunggal */
        if (!src) continue;
        copy_region(g_bb, g_bb_stride, src, L_win_stride[i],
                    ix0 - wx0, iy0 - wy0, ix1 - ix0, iy1 - iy0,
                    ix0, iy0, 1);

        /* 2b. konten dinamis per-app */
        if (i == APP_FM) {
            /* File Manager: seluruh isi jendela (toolbar, location bar,
             * sidebar Places, daftar, scrollbar, status) digambar di
             * sini — sudah ter-clip ke region kotor oleh compose(). */
            fm_draw(w);
        } else if (i == APP_CALC) {
            /* display: angka right-aligned putih + indikator op */
            char num[40];
            if (calc.err) snprintf(num, sizeof(num), "Error");
            else calc_fmt(calc.val, num, (int)sizeof(num));
            int len = 0;
            while (num[len]) len++;
            int dx = (int)(w.x + CALC_M + (w.w - 2 * CALC_M) - 10) - len * 8;
            int dy = (int)(w.y + TITLE_H + CALC_M + (CALC_DISP_H - 16) / 2);
            text_blit(dx, dy, num, 0xFFFFFF);
            if (calc.op) {
                char oc[2] = { calc.op, 0 };
                text_blit((int)(w.x + CALC_M + 10), dy, oc, 0x8A8A8A);
            }
            /* hover tombol calc */
            if (hov_calc_row >= 0) {
                float bx, by, bw2, bh2;
                calc_grid(w, hov_calc_row, hov_calc_col, &bx, &by, &bw2, &bh2);
                uint8_t kind = CALC_KIND[hov_calc_row][hov_calc_col];
                bb_fill_round((int)bx, (int)by, (int)bw2, (int)bh2,
                              gray(calc_btn_hover(kind)));
            }
        } else if (i == APP_ABOUT) {
            char buf[32];
            snprintf(buf, sizeof(buf), "klik app: %d", click_count);
            text_blit((int)w.x + 14, (int)w.y + 46 + 6 * 20, buf, 0x282828);
        }
    }

    /* 3. taskbar dinamis: tombol jendela + jam + hover MENU */
    float th = TASKBAR_H;
    int tby = (int)(g_H - th);

    if (g_cl_y1 > tby) {   /* region menyentuh taskbar */
        /* hover MENU: highlight putih murni */
        if (hov_menu) {
            bb_fill_round(10, (int)(g_H - th + 7), 92, 30, gray(G_MENU_H));
            text_blit(34, (int)(g_H - th + 15), "MENU", 0x141414);
        }

        float bx = 112;
        for (int i = 0; i < APP_COUNT; i++) {
            if (!wins[i].open) continue;
            bool act = (i == focus_idx && !wins[i].minimized);
            bool hi = (i == hov_task_idx);
            uint8_t lv = act ? G_TASK_A : (hi ? G_TASK_H : G_TASK);
            bb_fill_round((int)bx, (int)(g_H - th + 7), 132, 30, gray(lv));
            if (wins[i].minimized) {
                char buf[24];
                snprintf(buf, sizeof(buf), "[%s]", APP_NAMES[i]);
                text_blit((int)bx + 8, (int)(g_H - th + 15), buf, 0x9A9A9A);
            } else if (act) {
                text_blit((int)bx + 8, (int)(g_H - th + 15), APP_NAMES[i], 0x141414);
            } else {
                text_blit((int)bx + 8, (int)(g_H - th + 15), APP_NAMES[i], 0xE8E8E8);
            }
            bx += 140;
        }

        /* jam (dinamis per detik) */
        if (prev_clock_sec >= 0) {
            char tb[16];
            snprintf(tb, sizeof(tb), "%02d:%02d:%02d",
                     prev_clock_h, prev_clock_m, prev_clock_sec);
            text_blit((int)(g_W - 128 + 27), (int)(g_H - th + 15), tb, 0xE8E8E8);
        }
    }

    /* 4. launcher popup (di atas taskbar) */
    if (menu_open) {
        float lx = launch_lx(), ly = launch_ly();
        float lw2 = launch_lw(), lh2 = launch_lh();
        bb_fill_round((int)lx, (int)ly, (int)lw2, (int)lh2, gray(G_LAUNCH));
        /* border tipis */
        bb_fill_rect((int)lx, (int)ly, (int)lw2, 1, gray(G_BAR_LN));
        bb_fill_rect((int)lx, (int)(ly + lh2 - 1), (int)lw2, 1, gray(G_BAR_LN));
        bb_fill_rect((int)lx, (int)ly, 1, (int)lh2, gray(G_BAR_LN));
        bb_fill_rect((int)(lx + lw2 - 1), (int)ly, 1, (int)lh2, gray(G_BAR_LN));

        for (int i = 0; i <= APP_COUNT; i++) {
            float iy = ly + 8 + i * 36;
            const char* nm = (i < APP_COUNT) ? APP_NAMES[i] : "Exit to shell";
            bool hi = (i == hov_launch);
            if (hi) {
                bb_fill_round((int)(lx + 6), (int)iy, (int)(lw2 - 12), 32,
                              gray(G_LAUNCH_H));
            }
            text_blit((int)lx + 18, (int)(iy + 8), nm, hi ? 0x101010 : 0xF0F0F0);
        }
    }

    /* 4b. dialog modal File Manager (di atas segalanya, di bawah kursor) */
    if (fm.dlg) fm_draw_dlg();

    /* 4c. menu konteks klik kanan File Manager */
    fm_ctx_draw();

    /* 5. kursor (paling atas) */
    if (g_cur_x >= 0)
        draw_cursor(g_cur_x, g_cur_y);
}

/* ================================================================== */
/*  Interaksi                                                          */
/* ================================================================== */
static int hit_window(float px, float py) {
    int best = -1, bestz = -1;
    for (int i = 0; i < APP_COUNT; i++) {
        Win& w = wins[i];
        if (!w.open || w.minimized) continue;
        if (px >= w.x && px <= w.x + w.w && py >= w.y && py <= w.y + w.h) {
            if (w.z > bestz) { bestz = w.z; best = i; }
        }
    }
    return best;
}

static void open_window(int i) {
    int prev_focus = focus_idx;
    Win& w = wins[i];
    if (!w.open) {
        w.open = true;
        w.minimized = false;
        w.x = APP_X[i];
        w.y = APP_Y[i];
        w.w = APP_W[i];
        w.h = APP_H[i];
        if (i == APP_CALC) calc_reset();
        build_win_layers(i);          /* ThorVG: SEKALI per open */
        if (i == APP_TERM) term_open();   /* spawn shell task + konsolnya */
    }
    w.z = ++top_z;
    focus_idx = i;
    /* kedua jendela yang berubah fokus harus direfresh */
    if (prev_focus >= 0 && prev_focus != i) {
        Win& pw = wins[prev_focus];
        mark_dirty((int)pw.x, (int)pw.y, (int)pw.w, (int)pw.h);
    }
    mark_dirty((int)w.x, (int)w.y, (int)w.w, (int)w.h);
    mark_dirty(112, (int)(g_H - TASKBAR_H), 760, (int)TASKBAR_H);  /* taskbar buttons */
}

static void close_window(int i) {
    Win& w = wins[i];
    mark_dirty((int)w.x, (int)w.y, (int)w.w, (int)w.h);
    if (i == APP_TERM) term_shutdown();   /* matikan shell task konsolnya */
    w.open = false;
    if (focus_idx == i) focus_idx = -1;
    free_win_layers(i);               /* kembalikan arena */
    mark_dirty(112, (int)(g_H - TASKBAR_H), 760, (int)TASKBAR_H);
}

static void focus_window(int i) {
    if (focus_idx == i) return;
    int prev = focus_idx;
    focus_idx = i;
    wins[i].z = ++top_z;
    if (prev >= 0) {
        Win& pw = wins[prev];
        mark_dirty((int)pw.x, (int)pw.y, (int)pw.w, (int)pw.h);
    }
    Win& w = wins[i];
    mark_dirty((int)w.x, (int)w.y, (int)w.w, (int)w.h);
    mark_dirty(112, (int)(g_H - TASKBAR_H), 760, (int)TASKBAR_H);
}

/* area yang wajib direfresh saat elemen taskbar berubah */
static void dirty_taskbar(void) {
    mark_dirty(0, (int)(g_H - TASKBAR_H), (int)g_W, (int)TASKBAR_H);
}

/* area launcher */
static void dirty_launcher(void) {
    mark_dirty((int)launch_lx() - 6, (int)launch_ly() - 6,
               (int)launch_lw() + 12, (int)launch_lh() + 12);
}

/* area display calc */
static void dirty_calc_disp(void) {
    Win& w = wins[APP_CALC];
    if (!w.open) return;
    mark_dirty((int)(w.x + CALC_M), (int)(w.y + TITLE_H + CALC_M),
               (int)(w.w - 2 * CALC_M), (int)CALC_DISP_H + 1);
}

/* ================================================================== */
/*  Entry point                                                        */
/* ================================================================== */
void desktop_run(void) {
    uint32_t W = vesa_get_width();
    uint32_t H = vesa_get_height();
    if (vesa_get_bpp() != 32) {
        printf("[equix] butuh mode VESA 32bpp (boot dari ISO).\n");
        return;
    }

    if (Initializer::init(0) != Result::Success) {
        printf("[equix] ThorVG init gagal\n");
        return;
    }

    /* ---- alokasi layer (arena GUI DINAMIS — ikut RAM terpasang) ---- */
    size_t fbsize = (size_t)W * H * 4;
    uint32_t arena_end = ga_arena_end();
    if ((size_t)(arena_end - 0x3400000u) < 2 * fbsize) {
        printf("[equix] arena GUI %u KB terlalu kecil utk 2x framebuffer "
               "(RAM < ~61 MB?).\n", (uint32_t)((arena_end - 0x3400000u) >> 10));
        Initializer::term();
        return;
    }

    L_bg = (uint32_t*)ga_malloc(fbsize);
    g_bb = (uint32_t*)ga_malloc(fbsize);
    if (!L_bg || !g_bb) {
        printf("[equix] gagal alokasi layer %u KB (arena GUI)\n",
               (uint32_t)(fbsize / 1024));
        if (L_bg) ga_free(L_bg);
        if (g_bb) ga_free(g_bb);
        L_bg = g_bb = nullptr;
        Initializer::term();
        return;
    }
    g_W = W; g_H = H;
    g_bb_stride = W;
    g_lfb = (uint32_t*)(uintptr_t)vesa_get_framebuffer();
    g_lfb_stride = vesa_get_pitch() / 4;
    cursor_init();

    printf("[equix] layer-cache: bg %u KB + backbuffer %u KB, arena dinamis "
           "0x3400000-0x%x (%u MB)\n",
           (uint32_t)(fbsize / 1024), (uint32_t)(fbsize / 1024),
           ga_arena_end(), (ga_arena_end() - 0x3400000u) >> 20);

    for (int i = 0; i < APP_COUNT; i++) wins[i] = Win{};
    calc_reset();
    fm_init();
    term_init();
    /* fase 2: status roda mouse ke log serial (harness uji membacanya:
     * wheel=1 berarti perangkat menyetujui paket 4 byte / dz). */
    {
        char mb[64];
        snprintf(mb, sizeof(mb), "[equix] mouse wheel=%d id=%d\n",
                 mouse_wheel_enabled(), mouse_wheel_id());
        serial_puts(mb);
    }
    /* Kunci pergantian konsol selama desktop memegang layar: F1/F2 dan
     * perintah `switch n` akan me-repaint framebuffer dengan teks dan
     * menghancurkan layer desktop. Dilepas saat keluar. */
    console_lock(1);
    menu_open = false;
    click_count = 0;
    hov_menu = 0; hov_task_idx = -1; hov_launch = -1;
    hov_calc_row = hov_calc_col = -1;
    prev_clock_sec = -1;
    g_cur_x = g_cur_y = -1;
    cur_bottom_reported = 0;
    st_frames = 0; st_blit_px = 0;

    /* ---- frame pertama: render L_bg sekali via ThorVG ---- */
    uint32_t t_boot = get_tick();
    render_bg_layer();

    open_window(APP_ABOUT);
    open_window(APP_NOTES);
    wins[APP_NOTES].minimized = true;
    open_window(APP_CALC);
    open_window(APP_FM);          /* terakhir dibuka = fokus */
    /* NOTE: open_window mark_dirty semua rect jendela — frame pertama
     * otomatis full karena union mencakup semuanya + taskbar. */
    mark_dirty(0, 0, (int)W, (int)H);
    {
        /* compose penuh + blit penuh SEKALI */
        compose(0, 0, (int)W, (int)H);
        g_cl_x0 = 0; g_cl_y0 = 0; g_cl_x1 = (int)W; g_cl_y1 = (int)H;
        blit_dirty(0, 0, (int)W, (int)H);
        st_frames++;
    }
    uint32_t t_first = get_tick();
    printf("[equix] desktop aktif - EquiX 0.4 Beta (layer-cache+dirty-rect, mono) "
           "- build terminal-window (shell task di konsol sendiri)\n");
    printf("[equix] first frame: %u ms (ThorVG render layer + 1 blit penuh)\n",
           t_first - t_boot);
    ga_stats();
    task_sleep(400);

    int32_t mx = (int32_t)W / 2, my = (int32_t)H / 2;
    uint8_t buttons = 0;          /* mask live — dipakai utk status drag */
    /* 0.4 Beta: snapshot penghitung tepi IRQ — klik/lepas dideteksi dari
     * perubahan penghitung mono-naik, BUKAN dari mask tombol hasil drain
     * (yang bisa tertelan bila tekan+lepas terjadi antara dua poll). */
    uint32_t last_press_edges = mouse_press_edge_count();
    uint32_t last_release_edges = mouse_release_edge_count();
    uint32_t last_rpress_edges = mouse_rpress_edge_count();   /* 0.4 Beta */
    int drag_idx = -1;
    float drag_off_x = 0, drag_off_y = 0;
    int prev_mx = -1, prev_my = -1;
    uint32_t t0 = get_tick();

    bool running = true;
    bool term_was_focused = false;
    while (running) {
        mouse_get_state(&mx, &my, &buttons);

        /* ---- keyboard routing (0.4 Beta) --------------------------
         * Terminal fokus -> desktop BERHENTI mendecode key dan hanya
         * meneruskan scancode mentah ke ring konsol shell terminal
         * (term_forward_keys). Karena semua key ikut diteruskan, ESC
         * pun masuk ke terminal: keluar desktop lewat MENU > Exit to
         * shell. Di luar terminal fokus, perilaku lama persis.      */
        bool term_focused = (wins[APP_TERM].open && !wins[APP_TERM].minimized &&
                             focus_idx == APP_TERM && term.con >= 0);
        int key = 0;
        if (term_focused) {
            if (!term_was_focused)
                getkey_poll_reset();   /* buang separuh seq 0xE0 lama */
            term_forward_keys();
        } else {
            key = getkey_poll();
            term.e0 = 0;               /* fase 2: lupakan E0 yang tertahan */
        }
        term_was_focused = term_focused;

        /* Modal File Manager (dialog baru/rename/hapus/properti) menahan
         * ESC — batal dulu, baru keluar desktop. Menu konteks juga modal. */
        bool fm_modal = ((fm.dlg != 0 || fm.ctx) && focus_idx == APP_FM &&
                         wins[APP_FM].open && !wins[APP_FM].minimized);
        if (key == 27 && !fm_modal) running = false;

        /* ---- keyboard utk kalkulator (fokus) ---- */
        if (key > 0 && focus_idx == APP_CALC && wins[APP_CALC].open &&
            !wins[APP_CALC].minimized) {
            char k = 0;
            if (key >= '0' && key <= '9') k = (char)key;
            else if (key == '.') k = '.';
            else if (key == '+') k = '+';
            else if (key == '-') k = '-';
            else if (key == '*') k = '*';
            else if (key == '/') k = '/';
            else if (key == '=') k = '=';
            else if (key == 13 || key == 10) k = '=';
            else if (key == 'c' || key == 'C' || key == 8) k = 'C';
            else if (key == 'n' || key == 'N') k = 'n';
            else if (key == 's' || key == 'S') k = 's';
            else if (key == '%') k = '%';
            if (k) {
                calc_key(k);
                dirty_calc_disp();
            }
        }

        /* ---- keyboard utk File Manager (fokus) ----
         * Arrow/PgUp/PgDn/Home/End pilih baris, Enter buka,
         * Backspace naik satu level, Del = hapus, ESC = batal dialog. */
        if (key != 0 && focus_idx == APP_FM && wins[APP_FM].open &&
            !wins[APP_FM].minimized) {
            fm_key(key);
        }

        /* ---- fase 2: roda mouse / dua jari touchpad (scrollback) ----
         * Delta terakumulasi LOSSLESS di IRQ lalu dibaca sekali per
         * frame (mouse_get_wheel), jadi tidak ada putaran roda yang
         * hilang walau poll lambat. Arahkan ke jendela di bawah
         * kursor: Terminal = geser riwayat (positif = ke riwayat),
         * File Manager = halaman naik/turun (semantik PgUp/PgDn). */
        {
            int wheel = mouse_get_wheel();
            if (wheel != 0) {
                int hit = hit_window((float)mx, (float)my);
                {
                    /* log terbatas (bukti dz sampai ke desktop + tujuannya) */
                    static int wheel_n = 0;
                    if (wheel_n < 16) {
                        char wb[64];
                        snprintf(wb, sizeof(wb),
                                 "[term] wheel dz=%d hit=%d\n", wheel, hit);
                        serial_puts(wb);
                        wheel_n++;
                    }
                }
                if (hit == APP_TERM && wins[APP_TERM].open &&
                    !wins[APP_TERM].minimized) {
                    if (focus_idx != APP_TERM) focus_window(APP_TERM);
                    int lines = wheel * 3;        /* 1 detent = 3 baris */
                    if (lines >  15) lines =  15;
                    if (lines < -15) lines = -15;
                    term_scroll(lines);
                    {
                        char wb[48];
                        snprintf(wb, sizeof(wb), "[term] wheel->off=%d\n",
                                 term_off());
                        serial_puts(wb);
                    }
                } else if (hit == APP_FM && wins[APP_FM].open &&
                           !wins[APP_FM].minimized && !fm.dlg && !fm.ctx) {
                    if (focus_idx != APP_FM) focus_window(APP_FM);
                    int pages = (wheel > 0) ? wheel : -wheel;
                    if (pages > 3) pages = 3;
                    for (int i = 0; i < pages; i++)
                        fm_key(wheel > 0 ? -7 : -8);
                }
            }
        }

        /* 0.4 Beta: klik/lepas LOSSLESS dari penghitung edge IRQ — klik
         * tekan+lepas sekalipun di antara dua poll tetap terdeteksi.
         * (buttons tetap dipakai untuk status DRAG yang sedang berjalan.) */
        uint32_t pe = mouse_press_edge_count();
        uint32_t re = mouse_release_edge_count();
        bool clicked = (pe != last_press_edges);
        bool released = (re != last_release_edges);
        last_press_edges = pe;
        last_release_edges = re;

        /* 0.4 Beta: klik KANAN dari penghitung tepi yang sama (lossless) */
        uint32_t rpe = mouse_rpress_edge_count();
        bool rclicked = (rpe != last_rpress_edges);
        last_rpress_edges = rpe;

        /* menu konteks ikut hilang bila jendela FM tertutup/minimize */
        if (fm.ctx && (!wins[APP_FM].open || wins[APP_FM].minimized))
            fm_ctx_close();

        /* ---- klik kanan: menu konteks (hanya di baris daftar isi) ---- */
        if (rclicked) {
            fm_ctx_close();                 /* tutup menu lama dulu */
            if (!fm.dlg && wins[APP_FM].open && !wins[APP_FM].minimized &&
                mx >= 0 && my >= 0 &&
                hit_window((float)mx, (float)my) == APP_FM) {
                if (focus_idx != APP_FM) focus_window(APP_FM);
                fm_ctx_open((int)mx, (int)my);
            }
        }

        /* ---- kursor: dirty = rect lama + baru (posisi sprite edge-clamped,
         * SAMA dengan yang digambar compose — jika tidak, sprite dekat
         * tepi layar meninggalkan jejak) ---- */
        if (mx != prev_mx || my != prev_my) {
            if (prev_mx >= 0) {
                int cx, cy;
                cursor_clamp(prev_mx, prev_my, &cx, &cy);
                mark_dirty(cx, cy, 16, 16);
            }
            {
                int cx, cy;
                cursor_clamp(mx, my, &cx, &cy);
                mark_dirty(cx, cy, 16, 16);
            }
            prev_mx = mx; prev_my = my;
        }
        g_cur_x = mx; g_cur_y = my;
        if (!cur_bottom_reported && my == (int32_t)H - 1) {
            cur_bottom_reported = 1;
            char b[64];
            snprintf(b, sizeof(b), "[equix] cursor mencapai dasar layar (y=%u)\n",
                     (unsigned)my);
            serial_puts(b);
        }

        /* ---- drag jendela: dirty = rect lama + baru ---- */
        if ((buttons & 1) && drag_idx >= 0) {
            Win& w = wins[drag_idx];
            int ox = (int)w.x, oy = (int)w.y;
            w.x = (float)mx - drag_off_x;
            w.y = (float)my - drag_off_y;
            if (w.x < 0) w.x = 0;
            if (w.y < 0) w.y = 0;
            if (w.x + w.w > (float)W) w.x = (float)W - w.w;
            if (w.y + w.h > (float)(H - TASKBAR_H)) w.y = (float)H - TASKBAR_H - w.h;
            if ((int)w.x != ox || (int)w.y != oy) {
                mark_dirty(ox, oy, (int)w.w, (int)w.h);
                mark_dirty((int)w.x, (int)w.y, (int)w.w, (int)w.h);
            }
        }
        /* ---- drag scrollbar File Manager ---- */
        if (fm.scroll && (buttons & 1)) {
            Win& w = wins[APP_FM];
            int vis = fm_vis(w);
            if (fm.nent > vis) {
                int y0 = (int)w.y + fm_rows_y0();
                int y1 = (int)w.y + fm_rows_y1(w);
                int sbh = y1 - y0;
                int th = sbh * vis / fm.nent;
                if (th < 18) th = 18;
                if (sbh > th + 1) {
                    int top = ((int)my - y0 - th / 2) * (fm.nent - vis)
                              / (sbh - th);
                    if (top < 0) top = 0;
                    if (top > fm.nent - vis) top = fm.nent - vis;
                    if (top != fm.top) { fm.top = top; fm_dirty_list(); }
                }
            }
        }
        if (released) { drag_idx = -1; fm.scroll = 0; }

        /* ---- hover File Manager (hanya bila FM jendela teratas) ---- */
        {
            int hr = -1, hb = -1, hp = -1, hd = -1, hco = -1;
            if (fm.ctx) {
                /* menu konteks memegang hover: hanya item menu */
                int ci = fm_ctx_at((int)mx, (int)my);
                if (ci != fm.ctx_item) {
                    int old = fm.ctx_item;
                    fm.ctx_item = ci;
                    if (old >= 0)
                        mark_dirty(fm.ctx_x + 3,
                                   fm.ctx_y + FM_CTX_PAD + old * FM_CTX_H,
                                   FM_CTX_W - 6, FM_CTX_H);
                    if (ci >= 0)
                        mark_dirty(fm.ctx_x + 3,
                                   fm.ctx_y + FM_CTX_PAD + ci * FM_CTX_H,
                                   FM_CTX_W - 6, FM_CTX_H);
                }
            } else {
                if (mx >= 0 && my >= 0 &&
                    hit_window((float)mx, (float)my) == APP_FM)
                    fm_hover((int)mx, (int)my, &hr, &hb, &hp, &hd, &hco);
                else if (fm.dlg && focus_idx == APP_FM)
                    fm_hover((int)mx, (int)my, &hr, &hb, &hp, &hd, &hco);
            }

            if (hr != fm.hrow) {
                Win& w = wins[APP_FM];
                int old = fm.hrow;
                fm.hrow = hr;
                if (w.open && !w.minimized) {
                    int vis = fm_vis(w);
                    if (old >= fm.top && old < fm.top + vis) {
                        mark_dirty((int)w.x + fm_list_x0(),
                                   (int)w.y + fm_rows_y0()
                                       + (old - fm.top) * FM_ROW_H,
                                   (int)w.w - fm_list_x0() - FM_M, FM_ROW_H);
                    }
                    if (hr >= fm.top && hr < fm.top + vis) {
                        mark_dirty((int)w.x + fm_list_x0(),
                                   (int)w.y + fm_rows_y0()
                                       + (hr - fm.top) * FM_ROW_H,
                                   (int)w.w - fm_list_x0() - FM_M, FM_ROW_H);
                    }
                }
            }
            if (hb != fm.hbtn) {
                Win& w = wins[APP_FM];
                int old = fm.hbtn;
                fm.hbtn = hb;
                if (w.open && !w.minimized) {
                    int bx, by, bw2, bh2;
                    if (old >= 0) {
                        fm_tool_rect(w, old, &bx, &by, &bw2, &bh2);
                        mark_dirty((int)w.x + bx - 2, (int)w.y + by - 2,
                                   bw2 + 4, bh2 + 4);
                    }
                    if (hb >= 0) {
                        fm_tool_rect(w, hb, &bx, &by, &bw2, &bh2);
                        mark_dirty((int)w.x + bx - 2, (int)w.y + by - 2,
                                   bw2 + 4, bh2 + 4);
                    }
                }
            }
            if (hp != fm.hplace) {
                Win& w = wins[APP_FM];
                int old = fm.hplace;
                fm.hplace = hp;
                if (w.open && !w.minimized) {
                    if (old >= 0 && old < FM_PLACES)
                        mark_dirty((int)w.x + FM_M,
                                   (int)w.y + fm_place_y(w, old),
                                   FM_SIDE, FM_PLACE_H);
                    if (hp >= 0 && hp < FM_PLACES)
                        mark_dirty((int)w.x + FM_M,
                                   (int)w.y + fm_place_y(w, hp),
                                   FM_SIDE, FM_PLACE_H);
                }
            }
            if (hd != fm.hdbtn) {
                Win& w = wins[APP_FM];
                int old = fm.hdbtn;
                fm.hdbtn = hd;
                if (w.open && fm.dlg) {
                    int bx, by, bw2, bh2;
                    if (old >= 0) {
                        fm_dlg_btn(old, &bx, &by, &bw2, &bh2);
                        mark_dirty((int)w.x + bx - 2, (int)w.y + by - 2,
                                   bw2 + 4, bh2 + 4);
                    }
                    if (hd >= 0) {
                        fm_dlg_btn(hd, &bx, &by, &bw2, &bh2);
                        mark_dirty((int)w.x + bx - 2, (int)w.y + by - 2,
                                   bw2 + 4, bh2 + 4);
                    }
                }
            }
            if (hco != fm.hcol) {
                Win& w = wins[APP_FM];
                int old = fm.hcol;
                fm.hcol = hco;
                if (w.open && !w.minimized) {
                    int hx = (int)w.x + fm_list_x0();
                    int hw = fm_list_x1(w) - fm_list_x0();
                    if (old >= 0)
                        mark_dirty(hx, (int)w.y + FM_BODY_Y, hw, FM_HDR_H);
                    if (hco >= 0)
                        mark_dirty(hx, (int)w.y + FM_BODY_Y, hw, FM_HDR_H);
                }
            }
        }

        /* ---- hover states (dirty hanya area yang berubah) ---- */
        {
            float th = TASKBAR_H;
            /* 0.4 Beta FIX "taskbar suka ke block": zona hover MENU =
             * SELURUH tinggi taskbar [H-th, H) — sama persis dengan zona
             * kliknya. Dulu hanya rect tombol 30px [H-th+7, H-th+37]:
             * kursor di baris atas/bawah taskbar tidak menyalakan
             * highlight apa pun -> terasa "diblock". */
            int hm = (my >= (int)(H - th) && mx >= 10 && mx <= 102) ? 1 : 0;
            if (hm != hov_menu) {
                hov_menu = hm;
                mark_dirty(10, (int)(H - th + 7), 92, 30);
            }

            int hti = -1;
            /* 0.4 Beta: zona hover tombol jendela = seluruh tinggi taskbar
             * (zona klik sudah selalu full-height — sekarang hover
             * mengikuti, feedback selalu nyala selama kursor di taskbar). */
            if (my >= (int)(H - th) && mx >= 112) {
                float bx = 112;
                for (int i = 0; i < APP_COUNT; i++) {
                    if (wins[i].open && mx >= (int)bx && mx <= (int)bx + 132)
                        { hti = i; break; }
                    bx += 140;
                }
            }
            if (hti != hov_task_idx) {
                hov_task_idx = hti;
                dirty_taskbar();
            }

            int hl = -1;
            if (menu_open) {
                float lx = launch_lx(), ly = launch_ly(), lw2 = launch_lw();
                if (mx >= (int)(lx + 6) && mx <= (int)(lx + lw2 - 6) &&
                    my >= (int)ly && my <= (int)(ly + launch_lh())) {
                    hl = (int)((my - (int)ly - 8) / 36);
                    if (hl > APP_COUNT) hl = -1;
                }
            }
            if (hl != hov_launch) {
                int old = hov_launch;
                hov_launch = hl;
                if (old >= 0)
                    mark_dirty((int)(launch_lx() + 6), (int)(launch_ly() + 8 + old * 36),
                               (int)(launch_lw() - 12), 32);
                if (hl >= 0)
                    mark_dirty((int)(launch_lx() + 6), (int)(launch_ly() + 8 + hl * 36),
                               (int)(launch_lw() - 12), 32);
            }

            int hcr = -1, hcc = -1;
            if (focus_idx == APP_CALC && wins[APP_CALC].open &&
                !wins[APP_CALC].minimized) {
                Win& w = wins[APP_CALC];
                float gx0 = w.x + CALC_M;
                float gy0 = w.y + TITLE_H + CALC_M + CALC_DISP_H + CALC_GAP;
                float bwid = (w.w - 2 * CALC_M - 3 * CALC_GAP) / 4.0f;
                float bhei = (w.h - TITLE_H - 2 * CALC_M - CALC_DISP_H
                              - CALC_GAP - 4 * CALC_GAP) / 5.0f;
                if (mx >= (int)gx0 && mx <= (int)(w.x + w.w - CALC_M) &&
                    my >= (int)gy0 &&
                    my <= (int)(gy0 + 5 * bhei + 4 * CALC_GAP)) {
                    int col = (int)((mx - gx0) / (bwid + CALC_GAP));
                    int row = (int)((my - gy0) / (bhei + CALC_GAP));
                    if (col >= 0 && col < 4 && row >= 0 && row < 5) {
                        float bxp = gx0 + col * (bwid + CALC_GAP);
                        float byp = gy0 + row * (bhei + CALC_GAP);
                        if (mx <= (int)(bxp + bwid) && my <= (int)(byp + bhei)) {
                            hcr = row; hcc = col;
                        }
                    }
                }
            }
            if (hcr != hov_calc_row || hcc != hov_calc_col) {
                Win& w = wins[APP_CALC];
                if (hov_calc_row >= 0 && w.open) {
                    float bx, by, bw2, bh2;
                    calc_grid(w, hov_calc_row, hov_calc_col, &bx, &by, &bw2, &bh2);
                    mark_dirty((int)bx, (int)by, (int)bw2, (int)bh2);
                }
                if (hcr >= 0 && w.open) {
                    float bx, by, bw2, bh2;
                    calc_grid(w, hcr, hcc, &bx, &by, &bw2, &bh2);
                    mark_dirty((int)bx, (int)by, (int)bw2, (int)bh2);
                }
                hov_calc_row = hcr; hov_calc_col = hcc;
            }
        }

        /* ---- klik ---- */
        if (clicked) {
            float px = (float)mx, py = (float)my;
            float th = TASKBAR_H;

            bool popup_consumed = false;

            /* ---- modal File Manager: semua klik diarahkan ke dialog ---- */
            if (fm.dlg && focus_idx == APP_FM && wins[APP_FM].open) {
                fm_dlg_click((int)px, (int)py);
                popup_consumed = true;
            }

            /* ---- menu konteks terbuka: klik pilih item / tutup menu.
             * Klik di LUAR menu tetap diserap (seperti menu modal). ---- */
            if (fm.ctx) {
                fm_ctx_click((int)px, (int)py);
                popup_consumed = true;
            }

            if (menu_open) {
                float lx = launch_lx(), ly = launch_ly();
                float lw2 = launch_lw(), lh2 = launch_lh();
                if (px >= lx - 6 && px <= lx + lw2 + 6 &&
                    py >= ly - 6 && py <= ly + lh2 + 6) {
                    int item = (int)((py - ly - 8) / 36);
                    if (item >= 0 && item <= APP_COUNT) {
                        if (item < APP_COUNT) {
                            open_window(item);
                            if (item != APP_ABOUT) click_count++;
                            if (item == APP_ABOUT) {
                                /* refresh baris counter */
                                mark_dirty((int)wins[APP_ABOUT].x + 14,
                                           (int)wins[APP_ABOUT].y + 166, 200, 16);
                            }
                        } else {
                            running = false;
                        }
                    }
                    popup_consumed = true;
                }
                menu_open = false;
                dirty_launcher();
                dirty_taskbar();
            }

            if (!popup_consumed && py >= (float)H - th) {
                if (px >= 10 && px <= 102) {
                    menu_open = !menu_open;
                    dirty_launcher();
                } else if (px >= 112) {
                    float bx = 112;
                    for (int i = 0; i < APP_COUNT; i++) {
                        if (!wins[i].open) continue;
                        if (px >= bx && px <= bx + 132) {
                            if (focus_idx == i && !wins[i].minimized) {
                                wins[i].minimized = true;
                                focus_idx = -1;
                                mark_dirty((int)wins[i].x, (int)wins[i].y,
                                           (int)wins[i].w, (int)wins[i].h);
                            } else {
                                open_window(i);
                                wins[i].minimized = false;
                            }
                            break;
                        }
                        bx += 140;
                    }
                    dirty_taskbar();
                }
            } else if (!popup_consumed) {
                int hit = hit_window(px, py);
                if (hit >= 0) {
                    Win& w = wins[hit];
                    if (py <= w.y + TITLE_H) {
                        float bx2 = w.x + w.w - 27;
                        if (px >= bx2 && px <= bx2 + 14) {
                            close_window(hit);
                        } else if (px >= bx2 - 22 && px <= bx2 - 8) {
                            w.minimized = true;
                            if (focus_idx == hit) focus_idx = -1;
                            mark_dirty((int)w.x, (int)w.y, (int)w.w, (int)w.h);
                            dirty_taskbar();
                        } else {
                            drag_idx = hit;
                            drag_off_x = px - w.x;
                            drag_off_y = py - w.y;
                            focus_window(hit);
                        }
                    } else {
                        focus_window(hit);

                        /* ---- File Manager: klik toolbar/places/daftar ---- */
                        if (hit == APP_FM)
                            fm_click((int)px, (int)py, get_tick());

                        if (hit == APP_ABOUT) {
                            click_count++;
                            mark_dirty((int)w.x + 14, (int)w.y + 46 + 6 * 20, 200, 16);
                        }

                        /* ---- Calculator: hit-test grid tombol ---- */
                        if (hit == APP_CALC) {
                            float gx0 = w.x + CALC_M;
                            float gy0 = w.y + TITLE_H + CALC_M + CALC_DISP_H + CALC_GAP;
                            float bwid = (w.w - 2 * CALC_M - 3 * CALC_GAP) / 4.0f;
                            float bhei = (w.h - TITLE_H - 2 * CALC_M - CALC_DISP_H
                                          - CALC_GAP - 4 * CALC_GAP) / 5.0f;
                            if (px >= gx0 && px <= w.x + w.w - CALC_M &&
                                py >= gy0 &&
                                py <= gy0 + 5 * bhei + 4 * CALC_GAP) {
                                int col = (int)((px - gx0) / (bwid + CALC_GAP));
                                int row = (int)((py - gy0) / (bhei + CALC_GAP));
                                if (col >= 0 && col < 4 && row >= 0 && row < 5) {
                                    float bxp = gx0 + col * (bwid + CALC_GAP);
                                    float byp = gy0 + row * (bhei + CALC_GAP);
                                    if (px <= bxp + bwid && py <= byp + bhei) {
                                        calc_key(CALC_KEYS[row][col]);
                                        dirty_calc_disp();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        /* ---- terminal: repaint saat shell menulis ke mirror ----
         * Satu-satunya sinyal bahwa konten jendela berubah: edit_seq
         * konsol naik (tulis sel / scroll / clear / gerak kursor). */
        if (wins[APP_TERM].open && !wins[APP_TERM].minimized && term.con >= 0) {
            uint32_t seq = console_edit_seq(term.con);
            if (seq != term.last_seq) {
                term.last_seq = seq;
                Win& tw = wins[APP_TERM];
                mark_dirty((int)tw.x, (int)(tw.y + TITLE_H),
                           (int)tw.w, (int)tw.h - (int)TITLE_H);
            }
        }

        /* ---- jam: dirty kecil per detik ---- */
        {
            int hh, mm, ss;
            rtc_read(&hh, &mm, &ss);
            if (ss != prev_clock_sec) {
                prev_clock_sec = ss; prev_clock_h = hh; prev_clock_m = mm;
                mark_dirty((int)(g_W - 128), (int)(g_H - TASKBAR_H + 7), 118, 30);
            }
        }

        /* ---- flush dirty ---- */
        if (g_dr_x1 > g_dr_x0 && g_dr_y1 > g_dr_y0) {
            int rx = g_dr_x0 < 0 ? 0 : g_dr_x0;
            int ry = g_dr_y0 < 0 ? 0 : g_dr_y0;
            int rw = g_dr_x1 - rx, rh = g_dr_y1 - ry;
            if (rx + rw > (int)g_W) rw = (int)g_W - rx;
            if (ry + rh > (int)g_H) rh = (int)g_H - ry;
            if (rw > 0 && rh > 0) {
                compose(rx, ry, rw, rh);
                g_cl_x0 = 0; g_cl_y0 = 0;
                g_cl_x1 = (int)g_W; g_cl_y1 = (int)g_H;
                blit_dirty(rx, ry, rw, rh);
                st_frames++;
            }
            g_dr_x0 = 0x7FFFFFFF; g_dr_y0 = 0x7FFFFFFF;
            g_dr_x1 = -1; g_dr_y1 = -1;
        }

        task_sleep(15);
    }

    uint32_t t1 = get_tick();
    uint32_t ms = t1 - t0;

    /* Lepas kunci konsol + matikan shell terminal sebelum kembali ke
     * shell teks (kalau tidak, F1/F2 tetap mati dan task-nya bocor). */
    term_shutdown();
    console_lock(0);

    for (int i = 0; i < APP_COUNT; i++) free_win_layers(i);
    ga_free(L_bg); ga_free(g_bb);
    L_bg = g_bb = nullptr;
    Initializer::term();

    char sb[96];
    uint32_t fps = ms ? (st_frames * 1000u) / ms : 0;
    snprintf(sb, sizeof(sb),
             "[equix] stats: %u frame, %u px blit, rata2 %u px/frame, ~%u fps\n",
             st_frames, st_blit_px,
             st_frames ? st_blit_px / st_frames : 0, fps);
    serial_puts(sb);

    clear_screen();
    printf("[equix] desktop selesai - kembali ke shell\n");
    printf("[equix] stats sesi: %u frame / %u ms (~%u fps), %u px ke LFB\n",
           st_frames, ms, fps, st_blit_px);
}

} /* namespace equix */
