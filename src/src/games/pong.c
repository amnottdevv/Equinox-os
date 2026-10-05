/* pong.c — PONG vs the machine, self-hosting userland game (Equinox OS).
 *
 * Written in the mtcc C dialect: kernel syscall builtins only, no
 * includes. The player paddle (left) follows the mouse Y or the
 * arrow keys; the AI paddle (right) tracks the ball with a CAPPED
 * speed and a small aiming offset — good enough to rally, beatable
 * with angled shots. First to 5 points wins. Each paddle hit speeds
 * the ball up 4%, so rallies escalate.
 *
 * Rendering is incremental: the paddles repaint only when the
 * position they were last DRAWN at changes, the ball is
 * erased/redrawn as a filled circle, and the center dashes are
 * repainted whenever the ball may have crossed them.
 *
 * Controls: mouse Y or Up/Down (W/S) move the paddle, Q or Esc
 * quits.
 */

/* ---- special key codes (kernel pollkey ABI) ---- */
#define KEY_UP     -1
#define KEY_DOWN   -2
#define KEY_LEFT   -3
#define KEY_RIGHT  -4

/* ---- palette (0x00RRGGBB) ---- */
#define COL_FIELD  0x00080c14
#define COL_BAR    0x00224488
#define COL_PLAYER 0x0022cc44
#define COL_AI     0x00ff4444
#define COL_BALL   0x00ffffff
#define COL_DASH   0x00224466
#define WHITE      0x00ffffff
#define YELLOW     0x00ffe040
#define RED        0x00ff4444

#define HUD        24          /* score bar height            */
#define PADDLE_W   16
#define PADDLE_H   110
#define BALL_R     6
#define AI_SPEED   4           /* px per physics step (~33 Hz) */
#define WIN_SCORE  5

/* ---- state (globals: mtcc has no static locals) ---- */
int fbi[6];                    /* fb_info: addr, W, H, bpp, pitch, avail */
int SW, SH;
int ply, aiy;                  /* paddle top-left Y            */
int ply_x, ai_x;               /* paddle X (fixed sides)       */
int bx, by, bvx, bvy;          /* ball center + velocity       */
int pscore, ascore;
int serve_wait;                /* ticks left before auto-serve */
int serve_dir;                 /* +1 toward AI, -1 toward player */
int drawn_ply, drawn_aiy;      /* paddle Y as last DRAWN       */
int rng_state = 12345;

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

int center_x(char* s, int scale) {
    int n = 0;
    while (s[n]) n++;
    return (SW - n * 6 * scale) / 2;
}

/* ---- rendering ---- */

void draw_hud() {
    char line[9];
    rect(0, 0, SW, HUD, COL_BAR);
    rect(0, HUD - 2, SW, 2, 0x00aaFF66);
    line[0] = 'P';
    line[1] = ' ';
    line[2] = '0' + pscore;
    line[3] = ' ';
    line[4] = ':';
    line[5] = ' ';
    line[6] = '0' + ascore;
    line[7] = ' ';
    line[8] = 0;
    draw_text(center_x(line, 2), 4, line, 2, WHITE);
}

void draw_center_line() {
    int y = HUD + 8;
    while (y < SH) {
        rect(SW / 2 - 2, y, 4, 16, COL_DASH);
        y += 32;
    }
}

void draw_paddles() {
    rect(ply_x, ply, PADDLE_W, PADDLE_H, COL_PLAYER);
    rect(ai_x, aiy, PADDLE_W, PADDLE_H, COL_AI);
}

void banner(char* msg, int color) {
    rect(0, SH / 2 - 40, SW, 80, COL_FIELD);
    draw_text(center_x(msg, 3), SH / 2 - 24, msg, 3, color);
}

/* ---- game logic ---- */

void reset_ball(int toward_ai) {
    bx = SW / 2;
    by = SH / 2;
    bvx = toward_ai ? 4 : -4;
    bvy = rng_range(1, 3);
    if (rng_next() & 1) bvy = -bvy;
    serve_dir = toward_ai ? 1 : -1;
}

void serve() {
    serve_wait = 80;              /* 0.8 s pause between points */
    bx = SW / 2;
    by = SH / 2;
    bvx = 0;
    bvy = 0;
}

/* one physics step: 0 = rally, +1 = player point, -1 = AI point */
int physics_step() {
    int off;
    if (serve_wait) {
        serve_wait--;
        if (serve_wait == 0) reset_ball(serve_dir > 0);
        return 0;
    }

    bx += bvx;
    by += bvy;

    /* top / bottom walls */
    if (by - BALL_R < HUD) { by = HUD + BALL_R;   bvy = -bvy; snd_beep(330, 15); }
    if (by + BALL_R > SH)  { by = SH - BALL_R;    bvy = -bvy; snd_beep(330, 15); }

    /* player paddle (left): english from the hit offset */
    if (bvx < 0 && overlap(bx - BALL_R, by - BALL_R, BALL_R * 2, BALL_R * 2,
                           ply_x, ply, PADDLE_W, PADDLE_H)) {
        bx = ply_x + PADDLE_W + BALL_R;
        bvx = -bvx;
        off = by - (ply + PADDLE_H / 2);
        bvy += off / 12;
        bvx = bvx * 104 / 100;    /* 4% faster each hit */
        bvy = bvy * 104 / 100;
        if (bvx > 11) bvx = 11;
        snd_beep(440, 20);
    }

    /* AI paddle (right) */
    if (bvx > 0 && overlap(bx - BALL_R, by - BALL_R, BALL_R * 2, BALL_R * 2,
                           ai_x, aiy, PADDLE_W, PADDLE_H)) {
        bx = ai_x - BALL_R;
        bvx = -bvx;
        off = by - (aiy + PADDLE_H / 2);
        bvy += off / 12;
        bvx = bvx * 104 / 100;
        bvy = bvy * 104 / 100;
        if (bvx < -11) bvx = -11;
        snd_beep(392, 20);
    }

    /* keep vy sane (no flat trajectories) */
    if (bvy > 0 && bvy < 2) bvy = 2;
    if (bvy < 0 && bvy > -2) bvy = -2;
    if (bvy > 7)  bvy = 7;
    if (bvy < -7) bvy = -7;

    /* scoring: ball fully past a side */
    if (bx + BALL_R < 0)  return -1;      /* exited left  -> AI point  */
    if (bx - BALL_R > SW) return 1;       /* exited right -> player point */
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
    int k, last, last_my, quitting, outcome, r;
    int obx, oby, ai_target, t0;
    int m[3];

    if (gfx_init() != 0) {
        print("pong: needs a 32bpp VESA mode\n");
        exit(1);
    }

    ply_x = 40;
    ai_x = SW - 40 - PADDLE_W;
    ply = (SH - PADDLE_H) / 2;
    aiy = ply;
    pscore = 0;
    ascore = 0;

    rng_seed(gettick() ^ 0xBADC0DE);

    rect(0, 0, SW, SH, COL_FIELD);
    draw_hud();
    draw_center_line();
    draw_paddles();
    drawn_ply = ply;              /* paddles drawn here first */
    drawn_aiy = aiy;
    serve();

    last = gettick();
    last_my = -1;
    quitting = 0;
    outcome = 0;                  /* 0 = playing, 1 = player wins, -1 = AI */
    while (!quitting && outcome == 0) {
        /* drain every key pressed this frame */
        for (;;) {
            k = pollkey();
            if (k == 0) break;
            if (k == 'q' || k == 27) { quitting = 1; break; }
            if (k == KEY_UP   || k == 'w') ply -= 30;
            if (k == KEY_DOWN || k == 's') ply += 30;
        }
        if (quitting) break;

        /* mouse Y drives the player paddle — only when it moved,
         * so arrow keys and the mouse coexist */
        if (mouse_state(m) == 0 && m[1] != last_my) {
            ply = m[1] - PADDLE_H / 2;
            last_my = m[1];
        }
        if (ply < HUD) ply = HUD;
        if (ply > SH - PADDLE_H) ply = SH - PADDLE_H;

        /* AI: track the ball while it approaches, else drift home.
         * The -14 aim offset plus the speed cap make it miss
         * angled shots — deterministic, no hidden advantages. */
        ai_target = (bvx > 0 && serve_wait == 0) ? (by - 14)
                                                  : (SH - PADDLE_H) / 2;
        if (aiy + PADDLE_H / 2 < ai_target - 2) aiy += AI_SPEED;
        else if (aiy + PADDLE_H / 2 > ai_target + 2) aiy -= AI_SPEED;
        if (aiy < HUD) aiy = HUD;
        if (aiy > SH - PADDLE_H) aiy = SH - PADDLE_H;

        /* physics at ~33 steps/s */
        if (every(&last, 3)) {
            obx = bx;
            oby = by;

            r = physics_step();

            if (r == 0) {
                /* dirty tracking against the last DRAWN position */
                if (drawn_ply < 0) drawn_ply = ply;
                if (ply != drawn_ply) {
                    rect(ply_x, drawn_ply, PADDLE_W, PADDLE_H, COL_FIELD);
                    rect(ply_x, ply, PADDLE_W, PADDLE_H, COL_PLAYER);
                    drawn_ply = ply;
                }
                if (drawn_aiy < 0) drawn_aiy = aiy;
                if (aiy != drawn_aiy) {
                    rect(ai_x, drawn_aiy, PADDLE_W, PADDLE_H, COL_FIELD);
                    rect(ai_x, aiy, PADDLE_W, PADDLE_H, COL_AI);
                    drawn_aiy = aiy;
                }
                if (bx != obx || by != oby) {
                    circle_fill(obx, oby, BALL_R, COL_FIELD);
                    circle_fill(bx, by, BALL_R, COL_BALL);
                    draw_center_line();   /* the ball may have erased dashes */
                }
            } else if (r > 0) {
                pscore++;
                snd_beep(523, 200);
                draw_hud();
                if (pscore >= WIN_SCORE) outcome = 1;
                else serve();
            } else {
                ascore++;
                snd_beep(196, 200);
                draw_hud();
                if (ascore >= WIN_SCORE) outcome = -1;
                else serve();
            }
        }
    }

    if (outcome == 1) {
        snd_beep(523, 120); snd_beep(659, 120);
        snd_beep(784, 120); snd_beep(1047, 240);
        banner("YOU WIN", YELLOW);
    } else if (outcome < 0) {
        snd_beep(196, 150); snd_beep(147, 200); snd_beep(110, 300);
        banner("AI WINS", RED);
    }

    if (outcome != 0) {
        t0 = gettick();
        while (gettick() - t0 < 250) {
            if (pollkey() != 0) break;
            sleep(20);
        }
    }

    spk_silence();
    rect(0, 0, SW, SH, 0);
    if (outcome == 1)      print("pong: you win ");
    else if (outcome < 0)  print("pong: ai wins ");
    else                   print("pong: quit ");
    printint(pscore);
    print("-");
    printint(ascore);
    print("\n");
    exit(0);
    return 0;
}
