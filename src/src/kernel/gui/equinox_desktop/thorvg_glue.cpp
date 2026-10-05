/**
 * @file thorvg_glue.cpp
 * @brief Task 1 — integrasi ThorVG (TinyPiXOS graphics engine) ke kernel.
 *
 * Inisialisasi engine + perintah shell:
 *   tvgdemo   : render demo desktop preview (gradient wallpaper, jendela
 *               rounded transparan ala TinyPiXOS, lingkaran radial
 *               gradient, garis anti-aliased) langsung ke LFB VESA.
 *   tvgbench  : ukur kecepatan render full-screen (frame / ms).
 *   tvginfo   : versi + resolusi + arena GUI.
 *
 * Rendering single-thread (Initializer::init(0 threads) → scheduler
 * threadless = synchronous). Target = LFB VESA langsung (zero-copy).
 */
#include "thorvg.h"
#include "config.h"          /* THORVG_VERSION_STRING (port) */
#include "header/task.h"     /* task_yield */
#include "header/vesa.h"
#include "header/stdio.h"
#include "header/guiarena.h"
#include "header/timer.h"

using namespace tvg;

static bool tvg_ready = false;

bool equinox_tvg_init(void) {
    if (tvg_ready) return true;
    if (Initializer::init(0) != Result::Success) {
        printf("[tvg] Initializer::init FAILED\n");
        return false;
    }
    tvg_ready = true;
    return true;
}

void equinox_tvg_term(void) {
    if (tvg_ready) {
        Initializer::term();
        tvg_ready = false;
    }
}

/* Canvas yang render langsung ke LFB VESA (mode 32bpp). */
static SwCanvas* make_lfb_canvas(void) {
    uint32_t* fb = (uint32_t*)(uintptr_t)vesa_get_framebuffer();
    uint32_t stride = vesa_get_pitch() / 4;
    uint32_t w = vesa_get_width();
    uint32_t h = vesa_get_height();
    if (!fb || !w || !h || vesa_get_bpp() != 32) {
        printf("[tvg] VESA 32bpp belum aktif (boot dari ISO/GRUB).\n");
        return nullptr;
    }
    SwCanvas* c = SwCanvas::gen();
    if (!c) return nullptr;
    if (c->target(fb, stride, w, h, ColorSpace::ARGB8888) != Result::Success) {
        delete c;
        printf("[tvg] target() gagal\n");
        return nullptr;
    }
    return c;
}

/* ---------------- demo scene (preview desktop Task 2) ---------------- */

static Shape* solid_rect(float x, float y, float w, float h,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Shape* s = Shape::gen();
    s->appendRect(x, y, w, h, 6, 6);   /* rounded corner 6 */
    s->fill(r, g, b, a);
    return s;
}

/* dipakai juga desktop.cpp (diagnostik/membandingkan pipeline) */
namespace tvg {
void equinox_tvg_build_demo(Scene* scene, uint32_t W, uint32_t H) {
    /* ---- wallpaper: linear gradient gelap ala TinyPiXOS ---- */
    Shape* bg = Shape::gen();
    bg->appendRect(0, 0, W, H);
    LinearGradient* grad = LinearGradient::gen();
    grad->linear(0, 0, (float)W, (float)H);
    Fill::ColorStop stops[] = {
        {0.0f, 0,   0,   0,   255},   /* hitam      */
        {0.45f, 26,  16,  64,  255},   /* navy gelap */
        {0.75f, 64,  22, 104, 255},   /* ungu       */
        {1.0f, 120, 43, 140, 255},   /* magenta    */
    };
    grad->colorStops(stops, 4);
    bg->fill(grad);
    scene->push(bg);

    /* ---- lingkaran besar radial gradient (brand mark) ---- */
    Shape* ring = Shape::gen();
    ring->appendCircle((float)(W - 130), 120.0f, 80, 80);
    RadialGradient* rg = RadialGradient::gen();
    rg->radial((float)(W - 130), 120, 8, (float)(W - 130), 120, 90);
    Fill::ColorStop rstops[] = {
        {0.0f, 255, 120, 80,  255},
        {1.0f, 40,  8,   60,  0},
    };
    rg->colorStops(rstops, 2);
    ring->fill(rg);
    scene->push(ring);

    /* ---- "jendela 1" solid + title bar ---- */
    float wx = 60, wy = 90, ww = 480, wh = 320;
    scene->push(solid_rect(wx, wy, ww, wh, 242, 242, 248, 235));
    Shape* title = Shape::gen();
    title->appendRect(wx, wy, ww, 34, 6, 6);
    LinearGradient* tg = LinearGradient::gen();
    tg->linear(wx, wy, wx + ww, wy);
    Fill::ColorStop tstops[] = {
        {0.0f, 58, 124, 210, 255},
        {1.0f, 96, 78, 200, 255},
    };
    tg->colorStops(tstops, 2);
    title->fill(tg);
    scene->push(title);
    /* garis pemisah titlebar bawah — garis AA tipis */
    Shape* sep = Shape::gen();
    sep->moveTo(wx, wy + 34);
    sep->lineTo(wx + ww, wy + 34);
    sep->strokeWidth(1.5f);
    sep->strokeFill(20, 20, 30, 255);
    scene->push(sep);

    /* ---- "jendela 2" frosted glass (alpha rendah) ---- */
    float gx = 620, gy = 180, gw = 380, gh = 240;
    scene->push(solid_rect(gx, gy, gw, gh, 200, 220, 255, 110));
    scene->push(solid_rect(gx, gy, gw, 30, 90, 160, 240, 200));

    /* ---- taskbar preview di bawah ---- */
    float th = 44;
    scene->push(solid_rect(0, H - th, W, th, 18, 18, 28, 240));
    /* tombol menu (rounded) + 3 tombol app */
    scene->push(solid_rect(12, H - th + 8, 84, th - 16, 70, 130, 210, 255));
    scene->push(solid_rect(108, H - th + 8, 60, th - 16, 240, 240, 240, 180));
    scene->push(solid_rect(178, H - th + 8, 60, th - 16, 240, 240, 240, 180));
    scene->push(solid_rect(248, H - th + 8, 60, th - 16, 240, 240, 240, 180));

    /* ---- garis AA dekoratif ---- */
    for (int i = 0; i < 6; i++) {
        Shape* ln = Shape::gen();
        float x0 = 40.0f + i * 28.0f;
        ln->moveTo(x0, H - 90.0f - i * 4);
        ln->lineTo(x0 + 90.0f, H - 60.0f + i * 4);
        ln->strokeWidth(2.0f + i * 0.5f);
        ln->strokeFill(120 + i * 20, 200 - i * 20, 255, 200);
        scene->push(ln);
    }
}

} /* namespace tvg */

/* ---------------- perintah shell ---------------- */

void cmd_tvgdemo(void) {
    if (!equinox_tvg_init()) return;

    SwCanvas* canvas = make_lfb_canvas();
    if (!canvas) return;

    Scene* scene = Scene::gen();
    equinox_tvg_build_demo(scene, vesa_get_width(), vesa_get_height());

    uint32_t t0 = get_tick();
    canvas->push(scene);
    canvas->update();
    canvas->draw();
    canvas->sync();
    uint32_t t1 = get_tick();

    uint32_t W = vesa_get_width(), H = vesa_get_height();
    printf("[tvg] demo dirender: %ux%u, %u tick (%u ms @100Hz)\n",
           W, H, t1 - t0, t1 - t0);
    ga_stats();

    char buf[8];
    printf("[tvg] tekan Enter untuk kembali ke shell...\n");
    gets(buf, sizeof(buf));

    delete canvas;   /* scene & shape ikut ter-refcount */
    clear_screen();
}

void cmd_tvgbench(void) {
    if (!equinox_tvg_init()) return;

    SwCanvas* canvas = make_lfb_canvas();
    if (!canvas) return;

    const int N = 5;
    uint32_t total = 0;
    for (int i = 0; i < N; i++) {
        Scene* scene = Scene::gen();
        equinox_tvg_build_demo(scene, vesa_get_width(), vesa_get_height());
        uint32_t t0 = get_tick();
        canvas->push(scene);
        canvas->update();
        canvas->draw();
        canvas->sync();
        uint32_t t1 = get_tick();
        total += t1 - t0;
        canvas->remove(scene);   /* unref → scene ter-free */
        task_yield();
    }
    delete canvas;

    uint32_t W = vesa_get_width(), H = vesa_get_height();
    printf("[tvg] bench %u frame %ux%u: total %u tick, rata2 %u ms/frame\n",
           N, W, H, total, total / N);
    ga_stats();
    clear_screen();
}

void cmd_tvginfo(void) {
    printf("thorvg    : %s (port TinyPiXOS/ThorVG, sw-engine, threadless)\n",
           THORVG_VERSION_STRING);
    printf("vesa      : %ux%u @%u bpp, pitch %u, fb 0x%x\n",
           vesa_get_width(), vesa_get_height(), vesa_get_bpp(),
           vesa_get_pitch(), vesa_get_framebuffer());
    ga_stats();
}
