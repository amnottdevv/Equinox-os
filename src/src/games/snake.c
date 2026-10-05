/* snake.c — SNAKE, self-hosting userland game (Equinox OS).
 *
 * Written in the mtcc C dialect: kernel syscall builtins only, no
 * includes, so the compiled .mrp stays small. The snake body is a
 * ring buffer over grid cells; movement runs on a fixed timestep
 * (~8 steps/s) and rendering is incremental (only the cells that
 * change), which keeps the game flicker-free on the VESA console.
 *
 * Controls: arrows or WASD steer, Q / Esc quits.
 * Wall or self collision ends the run; food grows and scores.
 */

/* ---- special key codes (kernel pollkey ABI) ---- */
#define KEY_UP     -1
#define KEY_DOWN   -2
#define KEY_LEFT   -3
#define KEY_RIGHT  -4

/* ---- palette (0x00RRGGBB) ---- */
#define COL_BG     0x00100818
#define COL_GRID   0x001c1024
#define COL_SNAKE  0x0022cc44
#define COL_HEAD   0x00aaFF66
#define COL_FOOD   0x00ff4444
#define COL_BAR    0x00224488

#define CELL   16        /* pixels per grid cell   */
#define HUD    24        /* score bar height       */
#define RING_N 4096      /* ring capacity (>= grid area) */

/* ---- state (globals: mtcc has no static locals) ---- */
int fbi[6];              /* fb_info: addr, W, H, bpp, pitch, avail */
int SW, SH;              /* screen size            */
int gx, gy;              /* grid dims in cells     */
int sx[RING_N], sy[RING_N];
int slen, head;          /* ring length and head index */
int dir;                 /* 0=right 1=down 2=left 3=up */
int dir_dx[4] = { 1, 0, -1, 0 };
int dir_dy[4] = { 0, 1, 0, -1 };
int fx, fy;              /* food cell              */
int score;
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
    /* fill_rect packs x|w<<16 and y|h<<16 into the 3-slot ABI */
    fill_rect(x | (w << 16), y | (h << 16), c);
}

/* ---- RNG: xorshift32 (never zero) ---- */

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
    return 0;                     /* unknown -> space */
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

/* ---- game rendering ---- */

void draw_cell(int cx, int cy, int color) {
    rect(cx * CELL + 1, cy * CELL + HUD + 1, CELL - 2, CELL - 2, color);
}

void erase_cell(int cx, int cy) {
    int c = ((cx + cy) & 1) ? COL_BG : COL_GRID;
    rect(cx * CELL, cy * CELL + HUD, CELL, CELL, c);
}

void draw_bar() {
    rect(0, 0, SW, HUD, COL_BAR);
    rect(0, HUD - 2, SW, 2, COL_HEAD);
    rect(SW - 16, 4, 12, 12, COL_FOOD);   /* fixed anchor for tests */
    draw_text(12, 8, "SCORE", 1, 0x00ffffff);
    text_uint(52, 8, score, 1, 0x00ffffff);
}

void draw_all() {
    int i, idx, c;
    for (i = 0; i < gy; i++) {
        int j;
        for (j = 0; j < gx; j++) {
            c = ((j + i) & 1) ? COL_BG : COL_GRID;
            rect(j * CELL, i * CELL + HUD, CELL, CELL, c);
        }
    }
    draw_cell(fx, fy, COL_FOOD);
    /* tail first so the head paints on top */
    for (i = slen - 1; i >= 0; i--) {
        idx = head - i;
        while (idx < 0) idx += RING_N;
        idx = idx % RING_N;
        draw_cell(sx[idx], sy[idx], i == 0 ? COL_HEAD : COL_SNAKE);
    }
    draw_bar();
}

/* ---- game logic ---- */

void place_food() {
    int tries, i, idx, hit;
    for (tries = 0; tries < 4000; tries++) {
        int x = rng_range(0, gx - 1);
        int y = rng_range(0, gy - 1);
        hit = 0;
        for (i = 0; i < slen; i++) {
            idx = head - i;
            while (idx < 0) idx += RING_N;
            idx = idx % RING_N;
            if (sx[idx] == x && sy[idx] == y) { hit = 1; break; }
        }
        if (!hit) {
            fx = x;
            fy = y;
            return;
        }
    }
    fx = 0;                       /* board full: degenerate */
    fy = 0;
}

/* one tick: 0 = moved, 1 = ate, -1 = dead */
int step() {
    int nx = sx[head];
    int ny = sy[head];
    int i, idx;
    nx += dir_dx[dir];
    ny += dir_dy[dir];

    if (nx < 0 || ny < 0 || nx >= gx || ny >= gy) return -1;

    /* self collision (the tail tip vacates its cell this tick) */
    for (i = 0; i < slen - 1; i++) {
        idx = head - i;
        while (idx < 0) idx += RING_N;
        idx = idx % RING_N;
        if (sx[idx] == nx && sy[idx] == ny) return -1;
    }

    if (nx == fx && ny == fy) {
        if (slen < RING_N) {
            head = (head + 1) % RING_N;
            slen++;
            sx[head] = nx;
            sy[head] = ny;
        }
        place_food();
        return 1;
    }

    head = (head + 1) % RING_N;
    sx[head] = nx;
    sy[head] = ny;
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
    int i, k, last, alive, r;
    int tail_idx, prev_head, phx, phy, tx, ty;

    if (gfx_init() != 0) {
        print("snake: needs a 32bpp VESA mode\n");
        exit(1);
    }

    gx = SW / CELL;
    gy = (SH - HUD) / CELL;
    if (gx > 84) gx = 84;
    if (gy > 48) gy = 48;
    if (gx < 8 || gy < 8) {
        print("snake: screen too small\n");
        exit(1);
    }

    rng_seed(gettick() ^ 0x1234abcd);

    /* initial snake: 4 cells heading right, centered */
    slen = 4;
    head = 3;
    for (i = 0; i < 4; i++) {
        sx[i] = gx / 2 - 3 + i;
        sy[i] = gy / 2;
    }
    dir = 0;
    score = 0;
    place_food();
    draw_all();

    last = gettick();
    alive = 1;
    while (alive) {
        /* drain every key pressed this frame */
        for (;;) {
            k = pollkey();
            if (k == 0) break;
            if (k == KEY_UP    || k == 'w') { if (dir != 1) dir = 3; }
            else if (k == KEY_DOWN  || k == 's') { if (dir != 3) dir = 1; }
            else if (k == KEY_LEFT  || k == 'a') { if (dir != 0) dir = 2; }
            else if (k == KEY_RIGHT || k == 'd') { if (dir != 2) dir = 0; }
            else if (k == 'q' || k == 27) { alive = 0; break; }
        }
        if (!alive) break;

        /* ~8 steps/s (period 12 ticks at 100 Hz) */
        if (every(&last, 12)) {
            /* capture the cells that change before stepping */
            prev_head = head;
            phx = sx[prev_head];
            phy = sy[prev_head];
            tail_idx = head - (slen - 1);
            while (tail_idx < 0) tail_idx += RING_N;
            tail_idx = tail_idx % RING_N;
            tx = sx[tail_idx];
            ty = sy[tail_idx];

            r = step();
            if (r < 0) {
                alive = 0;
                snd_beep(120, 300);          /* death buzz */
            } else if (r > 0) {
                score++;
                snd_beep(880, 40);           /* munch */
                draw_cell(phx, phy, COL_SNAKE);
                draw_cell(sx[head], sy[head], COL_HEAD);
                draw_cell(fx, fy, COL_FOOD);
                draw_bar();
            } else {
                erase_cell(tx, ty);          /* vacated tail */
                draw_cell(phx, phy, COL_SNAKE);
                draw_cell(sx[head], sy[head], COL_HEAD);
            }
        }
    }

    spk_silence();
    rect(0, 0, SW, SH, 0);                   /* clean canvas for the shell */
    print("snake: game over - score ");
    printint(score);
    print("\n");
    exit(0);
    return 0;
}
