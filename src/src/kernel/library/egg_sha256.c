/* ============================================================
 *  egg_sha256.c — SHA-256 (FIPS 180-4) untuk eggkg
 * ------------------------------------------------------------
 *  Kernel side (dikompilasi i686-elf-g++ sebagai C++), tapi
 *  ditulis gaya C portabel. Semua rotasi/shift pada uint32_t
 *  sehingga tidak ada UB signed-shift.
 * ============================================================ */
#include "library/header/egg_sha256.h"

#define EGG_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t egg_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t egg_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void egg_be32_put(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Proses satu blok 64-byte ke state[8]. */
static void egg_sha_block(uint32_t st[8], const uint8_t blk[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = egg_be32(blk + i * 4);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = EGG_ROTR(w[i - 15], 7) ^ EGG_ROTR(w[i - 15], 18)
                    ^ (w[i - 15] >> 3);
        uint32_t s1 = EGG_ROTR(w[i - 2], 17) ^ EGG_ROTR(w[i - 2], 19)
                    ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = st[0]; b = st[1]; c = st[2]; d = st[3];
    e = st[4]; f = st[5]; g = st[6]; h = st[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = EGG_ROTR(e, 6) ^ EGG_ROTR(e, 11) ^ EGG_ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + egg_k[i] + w[i];
        uint32_t S0 = EGG_ROTR(a, 2) ^ EGG_ROTR(a, 13) ^ EGG_ROTR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void egg_sha256(const uint8_t* data, uint32_t len, uint8_t out[32]) {
    uint32_t st[8];
    uint8_t  tail[128];          /* sisa blok + padding + len (worst 2 blk) */
    uint32_t i, remain;

    st[0] = 0x6a09e667u; st[1] = 0xbb67ae85u;
    st[2] = 0x3c6ef372u; st[3] = 0xa54ff53au;
    st[4] = 0x510e527fu; st[5] = 0x9b05688cu;
    st[6] = 0x1f83d9abu; st[7] = 0x5be0cd19u;

    /* blok penuh langsung dari data */
    for (i = 0; i + 64 <= len; i += 64)
        egg_sha_block(st, data + i);

    remain = len - i;
    for (uint32_t j = 0; j < remain; j++)
        tail[j] = data[i + j];
    tail[remain] = 0x80;
    remain++;

    /* padding nol sampai len % 64 == 56, lalu 8 byte bit-length */
    while ((remain % 64) != 56) {
        tail[remain] = 0;
        remain++;
        if (remain == 128) {           /* kasus lintas blok (jarang) */
            egg_sha_block(st, tail);
            remain = 0;
        }
    }
    /* len dalam bit, 64-bit big-endian (uint32 cukup utk < 512 MB) */
    for (int z = 0; z < 4; z++) tail[remain + z] = 0;
    egg_be32_put(tail + remain + 4, len * 8u);
    remain += 8;
    if (remain > 64) {
        egg_sha_block(st, tail);
        egg_sha_block(st, tail + 64);
    } else {
        egg_sha_block(st, tail);
    }

    for (i = 0; i < 8; i++)
        egg_be32_put(out + i * 4, st[i]);
}

void egg_sha256_hex(const uint8_t* data, uint32_t len, char out[65]) {
    static const char* hexd = "0123456789abcdef";
    uint8_t dig[32];
    egg_sha256(data, len, dig);
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hexd[dig[i] >> 4];
        out[i * 2 + 1] = hexd[dig[i] & 0x0f];
    }
    out[64] = '\0';
}

int egg_sha_eq(const char* a, const char* b) {
    int la = 0, lb = 0;
    while (a[la]) la++;
    while (b[lb]) lb++;
    if (la != lb) return 0;
    for (int i = 0; i < la; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'F') x = (char)(x + 32);
        if (y >= 'A' && y <= 'F') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}
