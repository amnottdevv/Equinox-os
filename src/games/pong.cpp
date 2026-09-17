/* pong.cpp — PONG vs the machine (.mrp), third Libgame game.
 * ----------------------------------------------------------------------------
 * Player paddle (left) follows the mouse Y or the arrow keys; the AI
 * paddle (right) tracks the ball with a CAPPED speed and a small aiming
 * offset — good enough to rally, beatable with angled shots.
 *
 * First to 5 points wins. Each paddle hit speeds the ball up 4%, so
 * rallies escalate. Every event has its own queued note (paddle / wall /
 * player point / ai point) — the audio ring plays them out while the
 * game keeps running.
 *
 * Built on Libgame: lg_text score HUD (scale 2), lg_circle_fill ball,
 * lg_every fixed timestep, lg_overlap AABB bounces. Zero malloc.
 */
#include "libgame.h"

#define HUD_H       24
#define PADDLE_W    16
#define PADDLE_H    110
#define BALL_R      6
#define AI_SPEED    4           /* px per physics step (33 Hz) — beatable  */
#define WIN_SCORE   5
#define PHYS_PERIOD 3u          /* 100Hz / 3 ~= 33 steps / s               */
#define SERVE_PAUSE 80u         /* ticks to breathe between points (0.8 s) */

#define COL_FIELD  0x00080c14u
#define COL_BAR    0x00224488u
#define COL_PLAYER 0x0022cc44u   /* green  — matches the snake palette     */
#define COL_AI     0x00ff4444u   /* red                                    */
#define COL_BALL   0x00ffffffu
#define COL_DASH   0x00224466u

/* ---- state ---- */
static int ply, aiy;                 /* paddle top-left Y                  */
static int ply_x, ai_x;              /* paddle X (fixed sides)             */
static int bx, by, bvx, bvy;         /* ball center + velocity             */
static uint32_t pscore, ascore;
static uint32_t serve_wait;          /* ticks left before auto-serve       */
static int serve_dir;                /* +1 = toward AI, -1 = toward player */
static int drawn_ply = -1, drawn_aiy = -1;  /* paddle Y as last DRAWN     */

static void draw_hud(void) {
    lg_rect(0, 0, LG_W, HUD_H, COL_BAR);
    lg_rect(0, HUD_H - 2, LG_W, 2, 0x00aaFF66u);
    /* "P : A" centered, scale 2 */
    char line[16];
    int n = 0;
    line[n++] = 'P'; line[n++] = ' ';
    line[n++] = (char)('0' + pscore);
    line[n++] = ' '; line[n++] = ':'; line[n++] = ' ';
    line[n++] = (char)('0' + ascore);
    line[n++] = ' '; line[n++] = 'A';
    line[n] = '\0';
    lg_text(lg_center_x(line, 2), 4, line, 2, LG_WHITE);
}

static void draw_center_line(void) {
    for (int y = HUD_H + 8; y < LG_H; y += 32)
        lg_rect(LG_W / 2 - 2, y, 4, 16, COL_DASH);
}

static void draw_paddles(void) {
    lg_rect(ply_x, ply, PADDLE_W, PADDLE_H, COL_PLAYER);
    lg_rect(ai_x, aiy, PADDLE_W, PADDLE_H, COL_AI);
}

static void reset_ball(int toward_ai) {
    bx = LG_W / 2;
    by = LG_H / 2;
    bvx = toward_ai ? 4 : -4;
    bvy = (int)lg_rand_range(1, 3);
    if (lg_rand() & 1u) bvy = -bvy;
    serve_dir = toward_ai ? 1 : -1;
}

static void serve(void) {
    serve_wait = SERVE_PAUSE;
    bx = LG_W / 2;
    by = LG_H / 2;
    bvx = 0;
    bvy = 0;
}

/* one physics step; returns 0 = rally, +1 = player point, -1 = ai point */
static int physics_step(void) {
    if (serve_wait) {
        serve_wait--;
        if (serve_wait == 0) reset_ball(serve_dir > 0);
        return 0;
    }

    int prev_x = bx;
    bx += bvx;
    by += bvy;

    /* top / bottom walls */
    if (by - BALL_R < HUD_H)  { by = HUD_H + BALL_R;   bvy = -bvy; lg_sfx(330u, 15u); }
    if (by + BALL_R > LG_H)   { by = LG_H - BALL_R;    bvy = -bvy; lg_sfx(330u, 15u); }

    /* player paddle (left) */
    if (bvx < 0 && lg_overlap(bx - BALL_R, by - BALL_R, BALL_R * 2, BALL_R * 2,
                              ply_x, ply, PADDLE_W, PADDLE_H)) {
        bx = ply_x + PADDLE_W + BALL_R;
        bvx = -bvx;
        /* english: hit near the paddle tips angles the ball */
        int off = by - (ply + PADDLE_H / 2);
        bvy += off / 12;
        bvx = bvx * 104 / 100;  bvy = bvy * 104 / 100;   /* 4% faster */
        if (bvx > 11) bvx = 11;
        lg_sfx(440u, 20u);
    }

    /* ai paddle (right) */
    if (bvx > 0 && lg_overlap(bx - BALL_R, by - BALL_R, BALL_R * 2, BALL_R * 2,
                              ai_x, aiy, PADDLE_W, PADDLE_H)) {
        bx = ai_x - BALL_R;
        bvx = -bvx;
        int off = by - (aiy + PADDLE_H / 2);
        bvy += off / 12;
        bvx = bvx * 104 / 100;  bvy = bvy * 104 / 100;
        if (bvx < -11) bvx = -11;
        lg_sfx(392u, 20u);
    }

    /* keep vy sane (no flat trajectories) */
    if (bvy > 0 && bvy < 2) bvy = 2;
    if (bvy < 0 && bvy > -2) bvy = -2;
    if (bvy > 7)  bvy = 7;
    if (bvy < -7) bvy = -7;

    /* scoring: ball fully past a side */
    if (bx + BALL_R < 0)  return -1;     /* exited left  -> ai point  */
    if (bx - BALL_R > LG_W) return 1;    /* exited right -> player point */
    (void)prev_x;
    return 0;
}

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
        print("pong: needs a 32bpp VESA mode\n");
        exit(1);
    }

    ply_x = 40;
    ai_x = LG_W - 40 - PADDLE_W;
    ply = (LG_H - PADDLE_H) / 2;
    aiy = ply;
    pscore = 0;
    ascore = 0;

    lg_srand(lg_ticks() ^ 0xBADC0DEu);

    lg_clear(COL_FIELD);
    draw_hud();
    draw_center_line();
    draw_paddles();
    drawn_ply = ply;                /* paddles drawn here first */
    drawn_aiy = aiy;
    serve();

    uint32_t last = lg_ticks();
    int last_my = -1;               /* last mouse y (stomp guard)         */
    int quitting = 0;
    int outcome = 0;                    /* 0 playing, 1 player wins, -1 ai */
    while (!quitting && outcome == 0) {
        /* --- input --- */
        for (;;) {
            int k = lg_key();
            if (k == 0) break;
            if (k == 'q' || k == 27) { quitting = 1; break; }
            if (k == MORPH_KEY_UP   || k == 'w') ply -= 30;
            if (k == MORPH_KEY_DOWN || k == 's') ply += 30;
        }
        if (quitting) break;

        /* mouse Y drives the player paddle — but ONLY when the mouse
         * actually moved (same stomp guard as breakout: an unconditional
         * write every iteration kills the arrow-key controls).        */
        morph_mouse_t ms;
        if (lg_mouse(&ms) == 0 && (int)ms.y != last_my) {
            ply = (int)ms.y - PADDLE_H / 2;
            last_my = (int)ms.y;
        }
        if (ply < HUD_H) ply = HUD_H;
        if (ply > LG_H - PADDLE_H) ply = LG_H - PADDLE_H;

        /* --- AI: track the ball while it approaches, else drift home.
         * The -14 aiming offset plus the speed cap make it miss angled
         * shots — purely deterministic, no hidden advantages.        */
        int ai_target = (bvx > 0 && !serve_wait)
                        ? (by - 14)
                        : (LG_H - PADDLE_H) / 2;
        if (aiy + PADDLE_H / 2 < ai_target - 2) aiy += AI_SPEED;
        else if (aiy + PADDLE_H / 2 > ai_target + 2) aiy -= AI_SPEED;
        if (aiy < HUD_H) aiy = HUD_H;
        if (aiy > LG_H - PADDLE_H) aiy = LG_H - PADDLE_H;

        /* --- physics at fixed rate --- */
        if (lg_every(&last, PHYS_PERIOD)) {
            int obx = bx, oby = by;   /* ball pre-step pos */

            int r = physics_step();

            if (r == 0) {
                /* incremental render — paddles use DIRTY TRACKING (the
                 * position they were last DRAWN at), because input updates
                 * ply/aiy BEFORE this block: comparing against a value
                 * captured here would always be equal -> no repaint
                 * (the exact bug the QEMU harness caught in breakout). */
                if (drawn_ply < 0) drawn_ply = ply;
                if (ply != drawn_ply) {
                    lg_rect(ply_x, drawn_ply, PADDLE_W, PADDLE_H, COL_FIELD);
                    lg_rect(ply_x, ply, PADDLE_W, PADDLE_H, COL_PLAYER);
                    drawn_ply = ply;
                }
                if (drawn_aiy < 0) drawn_aiy = aiy;
                if (aiy != drawn_aiy) {
                    lg_rect(ai_x, drawn_aiy, PADDLE_W, PADDLE_H, COL_FIELD);
                    lg_rect(ai_x, aiy, PADDLE_W, PADDLE_H, COL_AI);
                    drawn_aiy = aiy;
                }
                if (bx != obx || by != oby) {
                    lg_circle_fill(obx, oby, BALL_R, COL_FIELD);
                    lg_circle_fill(bx, by, BALL_R, COL_BALL);
                    /* the ball can sweep across the center line — repaint
                     * the dashes it may have erased (cheap: ~22 rects) */
                    draw_center_line();
                }
            } else if (r > 0) {
                pscore++;
                lg_sfx(523u, 200u);
                draw_hud();
                if (pscore >= WIN_SCORE) outcome = 1;
                else serve();
            } else {
                ascore++;
                lg_sfx(196u, 200u);
                draw_hud();
                if (ascore >= WIN_SCORE) outcome = -1;
                else serve();
            }
        }
    }

    /* --- end states --- */
    if (outcome == 1) {
        lg_sfx(LG_N_C5, 120u); lg_sfx(LG_N_E5, 120u);
        lg_sfx(LG_N_G5, 120u); lg_sfx(LG_N_C6, 240u);
        banner("YOU WIN", LG_YELLOW);
    } else if (outcome < 0) {
        lg_sfx(196u, 150u); lg_sfx(147u, 200u); lg_sfx(110u, 300u);
        banner("AI WINS", LG_RED);
    }

    if (outcome != 0) {
        uint32_t t0 = lg_ticks();
        while (lg_ticks() - t0 < 250u) {
            if (lg_key() != 0) break;
            lg_sleep(20u);
        }
    }

    lg_finish();
    if (outcome == 1)      print("pong: you win ");
    else if (outcome < 0)  print("pong: ai wins ");
    else                   print("pong: quit ");
    printint(pscore);
    print("-");
    printint(ascore);
    print("\n");
    exit(0);
}
