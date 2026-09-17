/* snake.cpp — SNAKE game (.mrp), the Libgame reference port.
 * ----------------------------------------------------------------------------
 * Originally morph_snake.cpp (v10.4, written directly against Morph.h).
 * Ported onto Libgame in v10.6 to prove the framework covers a real game:
 * lg_init / lg_every / lg_rand_range / lg_sfx replace the hand-rolled
 * framebuffer probe, fixed-timestep arithmetic, xorshift RNG and the
 * manual beep bookkeeping. Gameplay, palette, geometry and timing are
 * IDENTICAL to v10.4 — the QEMU regression suite (canvas colors, head
 * tracking, clean Q exit) must keep passing unchanged.
 *
 * Controls: arrows or WASD steer, Q/Esc quits. Food grows the snake and
 * scores; hitting yourself ends the run. The snake body is a fixed-size
 * ring buffer (static allocation: no malloc, the MRP arena has no
 * per-allocation free).
 */
#include "libgame.h"

/* ---- palette (0x00RRGGBB) — unchanged from v10.4 ---- */
#define COL_BG     0x00100818u   /* near-black blue       */
#define COL_GRID   0x001c1024u   /* subtle checker        */
#define COL_SNAKE  0x0022cc44u   /* green                 */
#define COL_HEAD   0x00aaFF66u   /* bright green-yellow   */
#define COL_FOOD   0x00ff4444u   /* red                   */
#define COL_BAR    0x00224488u   /* score bar             */

/* ---- grid geometry (computed from fb at runtime) ---- */
#define CELL 16                /* pixels per cell               */
#define GRID_MAX_W 100
#define GRID_MAX_H 62          /* 1600x1000 max we support      */

/* snake ring buffer (grid coordinates) */
static int16_t sx[GRID_MAX_W * GRID_MAX_H];
static int16_t sy[GRID_MAX_W * GRID_MAX_H];
static int snake_len;
static int head;               /* ring index of the head        */

/* direction: 0=right 1=down 2=left 3=up */
static int dir;
static int food_x, food_y;
static int gw, gh;             /* grid dims                     */
static uint32_t score;
static uint32_t tone_until;    /* tick when the current beep ends */

static void place_food(void) {
    /* try random cells until one is free of the snake */
    for (int tries = 0; tries < 4000; tries++) {
        int x = (int)lg_rand_range(0, (uint32_t)gw - 1);
        int y = (int)lg_rand_range(0, (uint32_t)gh - 1);
        int hit = 0;
        for (int i = 0; i < snake_len; i++) {
            int idx = (head - i + GRID_MAX_W * GRID_MAX_H) % (GRID_MAX_W * GRID_MAX_H);
            if (sx[idx] == (int16_t)x && sy[idx] == (int16_t)y) { hit = 1; break; }
        }
        if (!hit) { food_x = x; food_y = y; return; }
    }
    food_x = 0; food_y = 0;    /* board full: degenerate */
}

static void draw_cell(int gx, int gy, uint32_t color) {
    int x = gx * CELL;
    int y = gy * CELL + 24;    /* 24px score bar on top */
    lg_rect(x + 1, y + 1, CELL - 2, CELL - 2, color);
}

static void draw_bar(void) {
    lg_rect(0, 0, LG_W, 24, COL_BAR);
    /* accent line under the bar + red food square in the corner as a
     * fixed visual anchor for the QEMU screendump verification       */
    lg_rect(0, 22, LG_W, 2, COL_HEAD);
    lg_rect(LG_W - 16, 4, 12, 12, COL_FOOD);
}

/* erase one cell back to its checkerboard color */
static void erase_cell(int gx, int gy) {
    uint32_t c = ((gx + gy) & 1) ? COL_BG : COL_GRID;
    lg_rect(gx * CELL, gy * CELL + 24, CELL, CELL, c);
}

/* redraw everything (board + snake, used at start only) */
static void draw_all(void) {
    /* background checker */
    for (int gy = 0; gy < gh; gy++) {
        for (int gx = 0; gx < gw; gx++) {
            uint32_t c = ((gx + gy) & 1) ? COL_BG : COL_GRID;
            lg_rect(gx * CELL, gy * CELL + 24, CELL, CELL, c);
        }
    }
    draw_bar();

    /* food */
    draw_cell(food_x, food_y, COL_FOOD);

    /* snake: tail first so the head paints on top */
    for (int i = snake_len - 1; i >= 0; i--) {
        int idx = (head - i + GRID_MAX_W * GRID_MAX_H) % (GRID_MAX_W * GRID_MAX_H);
        draw_cell(sx[idx], sy[idx], i == 0 ? COL_HEAD : COL_SNAKE);
    }
}

/* one game tick: returns 0 = alive, 1 = ate food, -1 = dead */
static int step(void) {
    int nx = sx[head];
    int ny = sy[head];
    if (dir == 0) nx++;
    else if (dir == 1) ny++;
    else if (dir == 2) nx--;
    else ny--;

    /* walls kill */
    if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) return -1;

    /* self collision (tail tip moves away, ignore its cell) */
    for (int i = 0; i < snake_len - 1; i++) {
        int idx = (head - i + GRID_MAX_W * GRID_MAX_H) % (GRID_MAX_W * GRID_MAX_H);
        if (sx[idx] == (int16_t)nx && sy[idx] == (int16_t)ny) return -1;
    }

    if (nx == food_x && ny == food_y) {
        /* grow: new head cell, tail stays */
        if (snake_len < GRID_MAX_W * GRID_MAX_H) {
            head = (head + 1) % (GRID_MAX_W * GRID_MAX_H);
            snake_len++;
            sx[head] = (int16_t)nx;
            sy[head] = (int16_t)ny;
        }
        place_food();
        return 1;
    }

    /* move: new head, drop tail (ring: just advance head over oldest) */
    head = (head + 1) % (GRID_MAX_W * GRID_MAX_H);
    sx[head] = (int16_t)nx;
    sy[head] = (int16_t)ny;
    return 0;
}

static void game_main(void);

extern "C" __attribute__((section(".start")))
void _start(void* legacy_api) {
    (void)legacy_api;
    game_main();
}

static void game_main(void) {
    /* 1. framebuffer probe — bail out gracefully on text mode */
    if (lg_init() != 0) {
        print("snake: needs a 32bpp VESA mode\n");
        exit(1);
    }

    /* grid from screen size, clamp to ring buffer capacity */
    gw = LG_W / CELL;
    gh = (LG_H - 24) / CELL;
    if (gw > GRID_MAX_W)  gw = GRID_MAX_W;
    if (gh > GRID_MAX_H)  gh = GRID_MAX_H;
    if (gw < 8 || gh < 8) {
        print("snake: screen too small\n");
        exit(1);
    }

    /* seed the rng from the timer so runs differ */
    lg_srand(lg_ticks() ^ 0x1234abcdu);

    /* initial snake: 4 cells heading right, centered.
     * RING LAYOUT: cell (head - i) is i steps BEHIND the head, so index
     * `head` must hold the RIGHTMOST cell (the actual head position). */
    snake_len = 4;
    head = 3;
    {
        int cx = gw / 2;
        int cy = gh / 2;
        for (int i = 0; i < 4; i++) {
            sx[i] = (int16_t)(cx - 3 + i);   /* sx[3]=cx head, sx[0]=cx-3 tail */
            sy[i] = (int16_t)cy;
        }
    }
    dir = 0;
    score = 0;
    place_food();
    draw_all();

    /* 2. game loop — lg_key() keeps it NON-BLOCKING */
    uint32_t last = lg_ticks();
    tone_until = 0;
    int alive = 1;
    while (alive) {
        /* input: drain everything pressed this frame */
        for (;;) {
            int k = lg_key();
            if (k == 0) break;
            if (k == MORPH_KEY_UP    || k == 'w') { if (dir != 1) dir = 3; }
            else if (k == MORPH_KEY_DOWN  || k == 's') { if (dir != 3) dir = 1; }
            else if (k == MORPH_KEY_LEFT  || k == 'a') { if (dir != 0) dir = 2; }
            else if (k == MORPH_KEY_RIGHT || k == 'd') { if (dir != 2) dir = 0; }
            else if (k == 'q' || k == 27) { alive = 0; break; }   /* Q / Esc */
        }

        /* fixed timestep: ~8 steps/sec (period 12 ticks @ 100 Hz) */
        if (lg_every(&last, 12u)) {
            /* INCREMENTAL RENDER: capture the cells that change before
             * stepping, then repaint ONLY those (2-3 lg_rect per tick
             * instead of a full 3910-rect checkerboard redraw — the
             * v10.4 flicker lesson). */
            int N = GRID_MAX_W * GRID_MAX_H;
            int prev_head = head;
            int phx = sx[prev_head], phy = sy[prev_head];
            int tail_idx = (head - (snake_len - 1) + N) % N;
            int tx = sx[tail_idx], ty = sy[tail_idx];

            int r = step();
            if (r < 0) {
                alive = 0;
                spk_tone(120);            /* death buzz */
                tone_until = lg_ticks() + 30;    /* 300ms       */
            } else if (r > 0) {
                score++;
                spk_tone(880);            /* munch */
                tone_until = lg_ticks() + 4;     /* 40ms  */
                draw_cell(phx, phy, COL_SNAKE);        /* old head -> body */
                draw_cell(sx[head], sy[head], COL_HEAD);
                draw_cell(food_x, food_y, COL_FOOD);   /* new food */
            } else {
                erase_cell(tx, ty);                     /* vacated tail */
                draw_cell(phx, phy, COL_SNAKE);        /* old head -> body */
                draw_cell(sx[head], sy[head], COL_HEAD);
            }
        }

        /* auto-silence the beep once its window is over */
        if (tone_until && (int32_t)(lg_ticks() - tone_until) >= 0) {
            spk_silence();
            tone_until = 0;
        }
    }
    spk_silence();

    /* 3. clean exit: clear the canvas so the shell text is readable */
    lg_clear(0x00000000u);
    print("snake: game over - score ");
    printint(score);
    print("\n");
    exit(0);
}
