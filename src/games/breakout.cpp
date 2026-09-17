/* breakout.cpp — BREAKOUT game (.mrp), the first mouse-driven Equinox OS game.
 * ----------------------------------------------------------------------------
 * Exercises ALL THREE kernel ring buffers at once (v10.6):
 *   - kbd ring   : Q/Esc quit, arrows nudge, SPACE launches
 *   - mouse ring : paddle tracks the absolute mouse X (SYS_MOUSE)
 *   - audio ring : every bounce queues a timed note (SYS_SNDBEEP) — each
 *                  brick ROW has its own pitch, so clearing a board plays
 *                  a rising scale. No blocking, the queue does the music.
 *
 * Built on Libgame (lg_* helpers, 5x7 HUD font). All state is static —
 * zero malloc.
 *
 * Rules: 3 lives, 10x6 bricks, top rows score more (10..60). Ball speeds
 * up 3% per brick. Paddle "english": hit off-center steers the ball.
 * Win  = clear all 60 bricks. Lose = drop the ball three times.
 */
#include "libgame.h"

/* ---- layout constants ---- */
#define HUD_H        24          /* score bar on top (snake convention)   */
#define COLS         10          /* brick columns                         */
#define ROWS         6           /* brick rows                            */
#define GAP          2           /* brick inset (visual gap)              */
#define PHYS_PERIOD  3u          /* 100Hz / 3 ~= 33 physics steps / s     */
#define LOSE_MS      300u        /* death buzz length                     */

/* ---- palette ---- */
#define COL_FIELD  0x00101828u   /* deep navy field                       */
#define COL_BAR    0x00224488u   /* HUD bar                               */
#define COL_PADDLE 0x00ddeeffu   /* pale cyan paddle                      */
#define COL_BALL   0x00ffffffu   /* white ball                            */

/* row appearance + the note played when a brick in that row dies
 * (top row = highest pitch, so a clean sweep is a rising scale) */
static const uint32_t row_col[ROWS] = {
    LG_RED, LG_ORANGE, LG_YELLOW, LG_GREEN, LG_CYAN, LG_BLUE
};
static const uint32_t row_hz[ROWS] = {
    LG_N_E5, 587u, LG_N_C5, LG_N_A4, 349u, LG_N_C4
};

/* ---- game state (all static — no heap) ---- */
static uint8_t brick_alive[ROWS][COLS];
static int bw, bh, y0;              /* brick cell geometry                */
static int px, py, pw, ph;          /* paddle                             */
static int bx, by, bvx, bvy, br;    /* ball (center + velocity + radius)  */
static int stuck;                   /* 1 = ball rides the paddle          */
static int drawn_px = -1;          /* paddle x AS LAST DRAWN (dirty      */
                                    /* tracking: erase old, draw new)     */
static uint32_t score, lives, bricks_left;
static uint32_t hud_score, hud_lives, hud_bricks;   /* change detection   */

/* ---- rendering ---- */

static void draw_hud(void) {
    lg_rect(0, 0, LG_W, HUD_H, COL_BAR);
    lg_rect(0, HUD_H - 2, LG_W, 2, 0x00aaFF66u);        /* accent line   */
    int x = 12;
    x = lg_text(x, 8, "SCORE", 1, LG_WHITE);
    x = lg_text_uint(x + 8, 8, score, 1, LG_WHITE);
    x = lg_text(x + 40, 8, "LIVES", 1, LG_WHITE);
    x = lg_text_uint(x + 8, 8, lives, 1, LG_WHITE);
    lg_text(LG_W - 12 - 6 * 6, 8, "BRICKS", 1, LG_WHITE);
    lg_text_uint(LG_W - 12, 8, bricks_left, 1, LG_WHITE);
    hud_score = score; hud_lives = lives; hud_bricks = bricks_left;
}

static void refresh_hud(void) {
    if (score != hud_score || lives != hud_lives || bricks_left != hud_bricks)
        draw_hud();
}

static void brick_rect(int r, int c, int* x, int* y, int* w, int* h) {
    *x = c * bw + GAP;
    *y = y0 + r * bh + GAP;
    *w = bw - GAP * 2;
    *h = bh - GAP * 2;
}

static void draw_bricks_all(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (brick_alive[r][c]) {
                int x, y, w, h;
                brick_rect(r, c, &x, &y, &w, &h);
                lg_rect(x, y, w, h, row_col[r]);
            }
}

static void kill_brick(int r, int c) {
    brick_alive[r][c] = 0;
    int x, y, w, h;
    brick_rect(r, c, &x, &y, &w, &h);
    lg_rect(x, y, w, h, COL_FIELD);
    bricks_left--;
}

static void draw_paddle(void) { lg_rect(px, py, pw, ph, COL_PADDLE); }

static void draw_ball(void) { lg_circle_fill(bx, by, br, COL_BALL); }

static void draw_hint(void) {
    const char* msg = "SPACE OR CLICK TO LAUNCH - MOUSE MOVES PADDLE";
    lg_text(lg_center_x(msg, 1), LG_H - 140, msg, 1, LG_GREY);
}

static void erase_hint(void) {
    lg_rect(0, LG_H - 142, LG_W, 10, COL_FIELD);
}

/* ---- game logic ---- */

static void reset_ball(void) {
    stuck = 1;
    bx = px + pw / 2;
    by = py - br - 1;
    bvx = 0;
    bvy = 0;
}

static void launch_ball(void) {
    if (!stuck) return;
    stuck = 0;
    erase_hint();
    /* random-ish upward launch angle, never perfectly vertical */
    bvx = (int)lg_rand_range(1, 4);
    if (lg_rand() & 1u) bvx = -bvx;
    bvy = -5;
}

/* clamp |bvx| <= 9 and keep |bvy| >= 3 (no infinite horizontal skating) */
static void tame_velocity(void) {
    if (bvx > 9) bvx = 9;
    if (bvx < -9) bvx = -9;
    if (bvy > 0 && bvy < 3) bvy = 3;
    if (bvy < 0 && bvy > -3) bvy = -3;
}

/* one physics step; returns 0 = play on, -1 = ball lost, 1 = board clear */
static int physics_step(void) {
    if (stuck) {                              /* ride the paddle */
        bx = px + pw / 2;
        by = py - br - 1;
        return 0;
    }

    int prev_y = by;                 /* reflection side test */
    bx += bvx;
    by += bvy;

    /* side + top walls */
    if (bx - br < 0)      { bx = br;          bvx = -bvx; lg_sfx(180u, 15u); }
    if (bx + br > LG_W)   { bx = LG_W - br;   bvx = -bvx; lg_sfx(180u, 15u); }
    if (by - br < HUD_H)  { by = HUD_H + br;  bvy = -bvy; lg_sfx(180u, 15u); }

    /* paddle: only while falling, and reflect with english */
    if (bvy > 0 && lg_overlap(bx - br, by - br, br * 2, br * 2,
                              px, py, pw, ph)) {
        by = py - br;
        bvy = -bvy;
        int off = bx - (px + pw / 2);          /* -pw/2 .. +pw/2 */
        bvx += off * 6 / (pw / 2);
        tame_velocity();
        lg_sfx(220u, 25u);
    }

    /* bricks: first overlap dies; reflect by the crossed edge */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (!brick_alive[r][c]) continue;
            int x, y, w, h;
            brick_rect(r, c, &x, &y, &w, &h);
            if (!lg_overlap(bx - br, by - br, br * 2, br * 2, x, y, w, h))
                continue;
            kill_brick(r, c);
            score += (uint32_t)(ROWS - r) * 10u;
            if (prev_y + br <= y || prev_y - br >= y + h) bvy = -bvy;
            else                                          bvx = -bvx;
            /* 3% speedup per brick, gently capped */
            bvx = bvx * 103 / 100;
            bvy = bvy * 103 / 100;
            tame_velocity();
            lg_sfx(row_hz[r], 40u);
            if (bricks_left == 0) return 1;
            return 0;                          /* one brick per step */
        }
    }

    /* floor: ball gone */
    if (by - br > LG_H) return -1;
    return 0;
}

/* big centered banner for the end states */
static void banner(const char* msg, uint32_t color) {
    lg_rect(0, LG_H / 2 - 40, LG_W, 80, COL_FIELD);
    lg_text(lg_center_x(msg, 3), LG_H / 2 - 24, msg, 3, color);
}

static void game_main(void);

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;
    game_main();
}

static void game_main(void) {
    if (lg_init() != 0) {
        print("breakout: needs a 32bpp VESA mode\n");
        exit(1);
    }

    /* geometry from the actual screen */
    bw = LG_W / COLS;
    bh = 26;
    y0 = 64;
    pw = LG_W / 10; if (pw < 100) pw = 100;
    ph = 14;
    px = (LG_W - pw) / 2;
    py = LG_H - 56;
    br = 7;

    score = 0;
    lives = 3;
    bricks_left = ROWS * COLS;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            brick_alive[r][c] = 1;

    lg_srand(lg_ticks() ^ 0xC0FFEEu);

    /* paint the field once, then only dirty rects */
    lg_clear(COL_FIELD);
    draw_bricks_all();
    draw_hud();
    draw_paddle();
    reset_ball();
    draw_ball();
    draw_hint();
    drawn_px = px;                 /* paddle drawn here first */

    uint32_t last = lg_ticks();
    int last_mx = -1;               /* last mouse x (stomp guard)         */
    int quitting = 0;
    int outcome = 0;               /* 0 = playing, 1 = win, -1 = game over */
    while (!quitting && outcome == 0) {
        /* --- input --- */
        for (;;) {
            int k = lg_key();
            if (k == 0) break;
            if (k == 'q' || k == 27) { quitting = 1; break; }
            if (k == ' ') { launch_ball(); continue; }
            if (k == MORPH_KEY_LEFT  || k == 'a') px -= 28;
            if (k == MORPH_KEY_RIGHT || k == 'd') px += 28;
        }
        if (quitting) break;

        /* mouse drives the paddle, but ONLY when it actually moved —
         * an unconditional "px = ms.x - pw/2" every iteration would
         * stomp arrow-key nudges to death (found by the QEMU harness:
         * keys looked dead). Keyboard and mouse coexist this way.   */
        morph_mouse_t ms;
        if (lg_mouse(&ms) == 0 && (int)ms.x != last_mx) {
            px = (int)ms.x - pw / 2;
            last_mx = (int)ms.x;
        }
        if (px < 0) px = 0;
        if (px > LG_W - pw) px = LG_W - pw;

        /* --- physics at fixed rate --- */
        if (lg_every(&last, PHYS_PERIOD)) {
            int obx = bx, oby = by;   /* ball pre-step pos (paddle uses drawn_px) */

            int r = physics_step();

            /* incremental render: erase then repaint movers.
             * BUGFIX: the paddle's OLD position must be the position it
             * was last DRAWN at (drawn_px), NOT its value at frame start
             * — input (mouse/keys) updates px BEFORE this block, so the
             * naive "opx = px" capture made px == opx always true and
             * the paddle never repainted (it moved in state only). */
            if (r == 0) {
                if (drawn_px < 0) drawn_px = px;      /* first frame */
                if (px != drawn_px) {
                    lg_rect(drawn_px, py, pw, ph, COL_FIELD);  /* erase OLD */
                    lg_rect(px, py, pw, ph, COL_PADDLE);       /* draw NEW  */
                    drawn_px = px;
                }
                if (bx != obx || by != oby) {
                    lg_circle_fill(obx, oby, br, COL_FIELD);  /* erase OLD */
                    draw_ball();
                }
            } else if (r < 0) {
                /* ball lost */
                lives--;
                lg_sfx(150u, LOSE_MS);
                if (lives == 0) {
                    outcome = -1;
                } else {
                    reset_ball();    /* old ball is already off-screen */
                    draw_hint();
                }
            } else {
                outcome = 1;                    /* board cleared */
            }
            refresh_hud();
        }
    }

    /* --- end states --- */
    if (outcome == 1) {
        /* victory jingle: queued notes, non-blocking */
        lg_sfx(LG_N_C5, 120u); lg_sfx(LG_N_E5, 120u);
        lg_sfx(LG_N_G5, 120u); lg_sfx(LG_N_C6, 240u);
        banner("YOU WIN", LG_YELLOW);
    } else if (outcome < 0) {
        lg_sfx(110u, 400u);
        banner("GAME OVER", LG_RED);
    }

    if (outcome != 0) {
        /* let the banner + jingle breathe ~2.5 s (or a keypress) */
        uint32_t t0 = lg_ticks();
        while (lg_ticks() - t0 < 250u) {
            if (lg_key() != 0) break;
            lg_sleep(20u);
        }
    }

    lg_finish();
    if (outcome == 1)       print("breakout: you win - score ");
    else if (outcome < 0)   print("breakout: game over - score ");
    else                    print("breakout: quit - score ");
    printint(score);
    print("\n");
    exit(0);
}
