/* breakout.c — BREAKOUT, self-hosting userland game (Equinox OS).
 *
 * Written in the mtcc C dialect: kernel syscall builtins only, no
 * includes. Mouse or arrow keys drive the paddle, SPACE (or a
 * click) launches the ball. 3 lives, 10x6 bricks, top rows score
 * more, the ball speeds up 3% per brick (capped), and paddle
 * "english" (hit off-center) steers the ball.
 *
 * Rendering is incremental: the paddle and ball are erased at
 * their previously DRAWN position only, and the HUD repaints when
 * a value changes. Every bounce queues a timed note, so clearing
 * a board plays a rising scale.
 *
 * Controls: mouse moves the paddle, arrows nudge, SPACE/click
 * launches, Q or Esc quits.
 */

/* ---- special key codes (kernel pollkey ABI) ---- */
#define KEY_UP     -1
#define KEY_DOWN   -2
#define KEY_LEFT   -3
#define KEY_RIGHT  -4

/* ---- palette (0x00RRGGBB) ---- */
#define COL_FIELD  0x00101828
#define COL_BAR    0x00224488
#define COL_PADDLE 0x00ddeeff
#define COL_BALL   0x00ffffff
#define WHITE      0x00ffffff
#define GREY       0x00909090

#define HUD        24          /* score bar height */
#define COLS       10          /* brick columns    */
#define ROWS       6           /* brick rows       */
#define GAP        2           /* brick inset      */
#define N_BRICK    60          /* ROWS * COLS      */

/* ---- state (globals: mtcc has no static locals) ---- */
int fbi[6];                    /* fb_info: addr, W, H, bpp, pitch, avail */
int SW, SH;
int brick[N_BRICK];            /* 1 = alive, index r*COLS+c */
int bw, bh, y0;                /* brick cell geometry  */
int px, py, pw, ph;            /* paddle               */
int bx, by, bvx, bvy, br;      /* ball center + velocity + radius */
int stuck;                     /* 1 = ball rides the paddle */
int drawn_px;                  /* paddle x as last DRAWN */
int score, lives, bricks_left;
int hud_score, hud_lives, hud_bricks;
int rng_state = 12345;

/* row appearance + the note played when a brick in that row dies */
int row_col[6] = { 0x00ff4444, 0x00ffa020, 0x00ffe040, 0x0022cc44, 0x0040c8dc, 0x006080ff };
int row_hz[6]  = { 659, 587, 523, 440, 349, 262 };

/* ---- 5x7 HUD font (bit 0 = top row, one byte per column) ---- */
char FCHARS[44] = " !-./:?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
int FONT[215] = {
    0x00, 0x00, 0x00, 0x00, 0x00,   /* ' ' */
    0x00, 0x00, 0x5F, 0x00, 0x00,   /* '!' */
    0x08, 0x08, 0x08, 0x08, 0x08,   /* '-' */
    0x00, 0x60, 0x60, 0x00, 0x00,   /* '.' */
    0x20, 0x10, 0x08, 0x04, 0x02,   /* '/' */
    0x00, 0x36, 0x36, 0x00, 0x00,   /* ':' */
    0x02, 0x01, 0x51, 0x09, 0x06,   /* '?' */
    0x3E, 0x51, 0x49, 0x45, 0x3E,   /* '0' */
    0x00, 0x42, 0x7F, 0x40, 0x00,   /* '1' */
    0x42, 0x61, 0x51, 0x49, 0x46,   /* '2' */
    0x21, 0x41, 0x45, 0x4B, 0x31,   /* '3' */
    0x18, 0x14, 0x12, 0x7F, 0x10,   /* '4' */
    0x27, 0x45, 0x45, 0x45, 0x39,   /* '5' */
    0x3C, 0x4A, 0x49, 0x49, 0x30,   /* '6' */
    0x01, 0x71, 0x09, 0x05, 0x03,   /* '7' */
    0x36, 0x49, 0x49, 0x49, 0x36,   /* '8' */
    0x06, 0x49, 0x49, 0x29, 0x1E,   /* '9' */
    0x7E, 0x11, 0x11, 0x11, 0x7E,   /* 'A' */
    0x7F, 0x49, 0x49, 0x49, 0x36,   /* 'B' */
    0x3E, 0x41, 0x41, 0x41, 0x22,   /* 'C' */
    0x7F, 0x41, 0x41, 0x22, 0x1C,   /* 'D' */
    0x7F, 0x49, 0x49, 0x49, 0x41,   /* 'E' */
    0x7F, 0x09, 0x09, 0x09, 0x01,   /* 'F' */
    0x3E, 0x41, 0x49, 0x49, 0x7A,   /* 'G' */
    0x7F, 0x08, 0x08, 0x08, 0x7F,   /* 'H' */
    0x00, 0x41, 0x7F, 0x41, 0x00,   /* 'I' */
    0x20, 0x40, 0x41, 0x3F, 0x01,   /* 'J' */
    0x7F, 0x08, 0x14, 0x22, 0x41,   /* 'K' */
    0x7F, 0x40, 0x40, 0x40, 0x40,   /* 'L' */
    0x7F, 0x02, 0x0C, 0x02, 0x7F,   /* 'M' */
    0x7F, 0x04, 0x08, 0x10, 0x7F,   /* 'N' */
    0x3E, 0x41, 0x41, 0x41, 0x3E,   /* 'O' */
    0x7F, 0x09, 0x09, 0x09, 0x06,   /* 'P' */
    0x3E, 0x41, 0x51, 0x21, 0x5E,   /* 'Q' */
    0x7F, 0x09, 0x16, 0x22, 0x41,   /* 'R' */
    0x46, 0x49, 0x49, 0x49, 0x31,   /* 'S' */
    0x01, 0x01, 0x7F, 0x01, 0x01,   /* 'T' */
    0x3F, 0x40, 0x40, 0x40, 0x3F,   /* 'U' */
    0x1F, 0x20, 0x40, 0x20, 0x1F,   /* 'V' */
    0x3F, 0x40, 0x38, 0x40, 0x3F,   /* 'W' */
    0x63, 0x14, 0x08, 0x14, 0x63,   /* 'X' */
    0x07, 0x08, 0x70, 0x08, 0x07,   /* 'Y' */
    0x61, 0x51, 0x49, 0x45, 0x43    /* 'Z' */
};

/* ---- graphics helpers ---- */

int gfx_init() {
    if (fb_info(fbi) != 0) return -1;
    if (fbi[5] == 0 || fbi[3] != 32) return -1;
    SW = fbi[1];
    SH = fbi[2];
    return 0;
}

void rect(int x, int y, int w, int h, int c) {
    fill_rect(x | (w << 16), y | (h << 16), c);
}

/* floor(sqrt(n)) — digit-by-digit, exact for small radii */
int isqrt(int n) {
    int r = 0;
    int bit = 1 << 30;
    if (n <= 0) return 0;
    while (bit > n) bit = bit >> 2;
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r = r >> 1;
        }
        bit = bit >> 2;
    }
    return r;
}

void circle_fill(int cx, int cy, int r, int c) {
    int dy, hw;
    if (r < 0) return;
    if (r > 512) r = 512;
    for (dy = -r; dy <= r; dy++) {
        hw = isqrt(r * r - dy * dy);
        rect(cx - hw, cy + dy, hw * 2 + 1, 1, c);
    }
}

/* axis-aligned bounding box overlap */
int overlap(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2) {
    return x1 < x2 + w2 && x2 < x1 + w1 && y1 < y2 + h2 && y2 < y1 + h1;
}

/* ---- RNG: xorshift32 ---- */

void rng_seed(int seed) {
    rng_state = seed | 1;
}

int rng_next() {
    rng_state = rng_state ^ (rng_state << 13);
    rng_state = rng_state ^ (rng_state >> 17);
    rng_state = rng_state ^ (rng_state << 5);
    return rng_state;
}

int rng_range(int lo, int hi) {
    int m;
    if (hi <= lo) return lo;
    m = rng_next() & 0x7fffffff;
    return lo + m % (hi - lo + 1);
}

/* ---- HUD text (5x7, integer scale) ---- */

int font_idx(char c) {
    int i;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    i = 0;
    while (FCHARS[i]) {
        if (FCHARS[i] == c) return i;
        i++;
    }
    return 0;
}

void draw_char(int x, int y, char c, int scale, int color) {
    int g = font_idx(c) * 5;
    int col, row, bits;
    if (scale < 1) scale = 1;
    for (col = 0; col < 5; col++) {
        bits = FONT[g + col];
        for (row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                if (scale == 1) put_pixel(x + col, y + row, color);
                else rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

int draw_text(int x, int y, char* s, int scale, int color) {
    int cx = x;
    while (*s) {
        draw_char(cx, y, *s, scale, color);
        cx += 6 * scale;
        s++;
    }
    return cx;
}

int text_uint(int x, int y, int v, int scale, int color) {
    char buf[12];
    int i = 11;
    if (v < 0) v = 0;
    buf[11] = 0;
    do {
        i--;
        buf[i] = '0' + v % 10;
        v = v / 10;
    } while (v > 0);
    return draw_text(x, y, buf + i, scale, color);
}

int center_x(char* s, int scale) {
    int n = 0;
    while (s[n]) n++;
    return (SW - n * 6 * scale) / 2;
}

/* ---- rendering ---- */

void brick_rect(int r, int c, int* x, int* y, int* w, int* h) {
    *x = c * bw + GAP;
    *y = y0 + r * bh + GAP;
    *w = bw - GAP * 2;
    *h = bh - GAP * 2;
}

void draw_hud() {
    int x;
    rect(0, 0, SW, HUD, COL_BAR);
    rect(0, HUD - 2, SW, 2, 0x00aaFF66);
    x = 12;
    x = draw_text(x, 8, "SCORE", 1, WHITE);
    x = text_uint(x + 8, 8, score, 1, WHITE);
    x = draw_text(x + 40, 8, "LIVES", 1, WHITE);
    x = text_uint(x + 8, 8, lives, 1, WHITE);
    draw_text(SW - 12 - 6 * 6, 8, "BRICKS", 1, WHITE);
    text_uint(SW - 12, 8, bricks_left, 1, WHITE);
    hud_score = score;
    hud_lives = lives;
    hud_bricks = bricks_left;
}

void refresh_hud() {
    if (score != hud_score || lives != hud_lives || bricks_left != hud_bricks)
        draw_hud();
}

void draw_bricks_all() {
    int r, c, x, y, w, h;
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++)
            if (brick[r * COLS + c]) {
                brick_rect(r, c, &x, &y, &w, &h);
                rect(x, y, w, h, row_col[r]);
            }
}

void kill_brick(int r, int c) {
    int x, y, w, h;
    brick[r * COLS + c] = 0;
    brick_rect(r, c, &x, &y, &w, &h);
    rect(x, y, w, h, COL_FIELD);
    bricks_left--;
}

void draw_hint() {
    draw_text(center_x("SPACE OR CLICK TO LAUNCH", 1), SH - 140,
              "SPACE OR CLICK TO LAUNCH", 1, GREY);
}

void erase_hint() {
    rect(0, SH - 142, SW, 10, COL_FIELD);
}

void banner(char* msg, int color) {
    rect(0, SH / 2 - 40, SW, 80, COL_FIELD);
    draw_text(center_x(msg, 3), SH / 2 - 24, msg, 3, color);
}

/* ---- game logic ---- */

void reset_ball() {
    stuck = 1;
    bx = px + pw / 2;
    by = py - br - 1;
    bvx = 0;
    bvy = 0;
}

void launch_ball() {
    if (!stuck) return;
    stuck = 0;
    erase_hint();
    bvx = rng_range(1, 4);
    if (rng_next() & 1) bvx = -bvx;
    bvy = -5;
}

/* clamp |bvx| <= 9 and |bvy| >= 3 (no flat trajectories) */
void tame_velocity() {
    if (bvx > 9) bvx = 9;
    if (bvx < -9) bvx = -9;
    if (bvy > 0 && bvy < 3) bvy = 3;
    if (bvy < 0 && bvy > -3) bvy = -3;
}

/* one physics step: 0 = play on, -1 = ball lost, 1 = board clear */
int physics_step() {
    int prev_y = by;
    int r, c, x, y, w, h, off;

    if (stuck) {                 /* ride the paddle */
        bx = px + pw / 2;
        by = py - br - 1;
        return 0;
    }

    bx += bvx;
    by += bvy;

    /* side + top walls */
    if (bx - br < 0)     { bx = br;        bvx = -bvx; snd_beep(180, 15); }
    if (bx + br > SW)    { bx = SW - br;   bvx = -bvx; snd_beep(180, 15); }
    if (by - br < HUD)   { by = HUD + br;  bvy = -bvy; snd_beep(180, 15); }

    /* paddle: only while falling, reflect with english */
    if (bvy > 0 && overlap(bx - br, by - br, br * 2, br * 2, px, py, pw, ph)) {
        by = py - br;
        bvy = -bvy;
        off = bx - (px + pw / 2);
        bvx += off * 6 / (pw / 2);
        tame_velocity();
        snd_beep(220, 25);
    }

    /* bricks: first overlap dies; reflect by the crossed edge */
    for (r = 0; r < ROWS; r++) {
        for (c = 0; c < COLS; c++) {
            if (!brick[r * COLS + c]) continue;
            brick_rect(r, c, &x, &y, &w, &h);
            if (!overlap(bx - br, by - br, br * 2, br * 2, x, y, w, h))
                continue;
            kill_brick(r, c);
            score += (ROWS - r) * 10;
            if (prev_y + br <= y || prev_y - br >= y + h) bvy = -bvy;
            else                                          bvx = -bvx;
            bvx = bvx * 103 / 100;   /* 3% speedup per brick, capped */
            bvy = bvy * 103 / 100;
            tame_velocity();
            snd_beep(row_hz[r], 40);
            if (bricks_left == 0) return 1;
            return 0;              /* one brick per step */
        }
    }

    if (by - br > SH) return -1;  /* floor: ball gone */
    return 0;
}

/* fixed-timestep gate: fires at most every `period` ticks */
int every(int* last, int period) {
    int now = gettick();
    if (now - *last >= period) {
        *last = now;
        return 1;
    }
    return 0;
}

int main() {
    int i, r, c, k, last, last_mx, quitting, outcome;
    int obx, oby, t0;
    int m[3];

    if (gfx_init() != 0) {
        print("breakout: needs a 32bpp VESA mode\n");
        exit(1);
    }

    /* geometry from the actual screen */
    bw = SW / COLS;
    bh = 26;
    y0 = 64;
    pw = SW / 10;
    if (pw < 100) pw = 100;
    ph = 14;
    px = (SW - pw) / 2;
    py = SH - 56;
    br = 7;

    score = 0;
    lives = 3;
    bricks_left = N_BRICK;
    for (i = 0; i < N_BRICK; i++) brick[i] = 1;

    rng_seed(gettick() ^ 0xC0FFEE);

    /* paint the field once, then only dirty rects */
    rect(0, 0, SW, SH, COL_FIELD);
    draw_bricks_all();
    draw_hud();
    rect(px, py, pw, ph, COL_PADDLE);
    reset_ball();
    circle_fill(bx, by, br, COL_BALL);
    draw_hint();
    drawn_px = px;

    last = gettick();
    last_mx = -1;
    quitting = 0;
    outcome = 0;                  /* 0 = playing, 1 = win, -1 = game over */
    while (!quitting && outcome == 0) {
        /* drain every key pressed this frame */
        for (;;) {
            k = pollkey();
            if (k == 0) break;
            if (k == 'q' || k == 27) { quitting = 1; break; }
            if (k == ' ') { launch_ball(); continue; }
            if (k == KEY_LEFT  || k == 'a') px -= 28;
            if (k == KEY_RIGHT || k == 'd') px += 28;
        }
        if (quitting) break;

        /* mouse drives the paddle, but ONLY when it moved — an
         * unconditional write every frame would stomp the keys */
        if (mouse_state(m) == 0 && m[0] != last_mx) {
            px = m[0] - pw / 2;
            last_mx = m[0];
        }
        /* any click also launches */
        if (stuck && m[2] != 0) launch_ball();

        if (px < 0) px = 0;
        if (px > SW - pw) px = SW - pw;

        /* physics at ~33 steps/s */
        if (every(&last, 3)) {
            obx = bx;
            oby = by;

            r = physics_step();

            if (r == 0) {
                /* erase at the position as last DRAWN, then repaint */
                if (drawn_px < 0) drawn_px = px;
                if (px != drawn_px) {
                    rect(drawn_px, py, pw, ph, COL_FIELD);
                    rect(px, py, pw, ph, COL_PADDLE);
                    drawn_px = px;
                }
                if (bx != obx || by != oby) {
                    circle_fill(obx, oby, br, COL_FIELD);
                    circle_fill(bx, by, br, COL_BALL);
                }
            } else if (r < 0) {
                lives--;
                snd_beep(150, 300);
                if (lives == 0) {
                    outcome = -1;
                } else {
                    reset_ball();
                    draw_hint();
                }
            } else {
                outcome = 1;      /* board cleared */
            }
            refresh_hud();
        }
    }

    if (outcome == 1) {
        snd_beep(523, 120); snd_beep(659, 120);
        snd_beep(784, 120); snd_beep(1047, 240);
        banner("YOU WIN", 0x00ffe040);
    } else if (outcome < 0) {
        snd_beep(110, 400);
        banner("GAME OVER", 0x00ff4444);
    }

    if (outcome != 0) {
        /* let the banner + jingle breathe ~2.5 s (or a keypress) */
        t0 = gettick();
        while (gettick() - t0 < 250) {
            if (pollkey() != 0) break;
            sleep(20);
        }
    }

    spk_silence();
    rect(0, 0, SW, SH, 0);
    if (outcome == 1)      print("breakout: you win - score ");
    else if (outcome < 0)  print("breakout: game over - score ");
    else                   print("breakout: quit - score ");
    printint(score);
    print("\n");
    exit(0);
    return 0;
}
