// ============================================================
//  tls_client.c — TLS 1.2 client for mget https:// (BearSSL)
// ------------------------------------------------------------
//  Library: BearSSL 0.6 (MIT, third_party/bearssl) — chosen
//  because it is pure C, needs no dynamic allocation, no OS,
//  no threads, and runs its whole state machine synchronously
//  through a pull/pump API. Ideal for a kernel.
//
//  Data flow (same model as the plain-HTTP mget path):
//
//    IRQ0 (net_poll -> lwIP callbacks)          shell task
//    ------------------------------    ------------------------------
//    tls_recv_cb:  pbuf ----memcpy--->  raw ring --> tls_pump():
//    tls_err_cb:   set flags                (a) ring  -> engine  (RECVREC)
//                                            (b) engine -> tcp    (SENDREC)
//    NO crypto ever runs in interrupt            (c) engine -> caller (RECVAPP)
//    context; every BearSSL call happens
//    under net_lock() in the caller task.
//
//  Verification (tls_connect_auto):
//    1. strict: full chain validation against the embedded
//       Mozilla root subset (see equinox_anchors.c).
//    2. fallback: if the strict handshake dies with a
//       certificate error (unknown root, unsupported curve -
//       e.g. github.com's current P-384 Sectigo chain), retry
//       with a parse-only validator: the leaf public key is
//       still extracted (the ECDHE signature check and all
//       record encryption/authentication stay active), but the
//       trust chain is not validated. A clear warning is
//       printed and tls_session_verified() reports 0.
// ============================================================
#include <stdint.h>
#include <stddef.h>

#include "library/header/stdio.h"     /* printf               */
#include "library/header/libstring.h"/* memcpy/memset/strcmp */
#include "library/header/malloc.h"   /* kernel heap          */

#include "net.h"                     /* net_lock / net_unlock*/
#include "tls_client.h"

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"

// sys_now() is defined in net_lwip.c (ms clock, PIT 100 Hz)
extern "C" uint32_t sys_now(void);

// ---- BearSSL (plain C library) ----
extern "C" {
#include "bearssl.h"
// exported by third_party/bearssl/anchors/equinox_anchors.c
extern const br_x509_trust_anchor* const eq_tls_anchors;
extern const unsigned                    eq_tls_anchors_num;
}

// ============================================================
//  Tunables
// ============================================================
#define TLS_RING_SIZE   16384u   /* raw record ring (power of 2) */
#define TLS_LEAF_MAX    8192u   /* leaf cert buffer, parse-only  */
#define TLS_HANDSHAKE_MS 15000  /* handshake inactivity timeout  */
#define TLS_IO_TIMEOUT_MS 15000 /* app-data inactivity timeout   */
#define TLS_HARDCAP_MS  45000   /* absolute cap per pump call    */
#define TLS_CONNECT_MS   6000   /* TCP SYN/SYN-ACK timeout       */

static inline uint32_t rol32(uint32_t x, unsigned r) {
    return (x << r) | (x >> (32 - r));
}

/* error code of the most recently closed session (read by
 * tls_connect_auto to decide on the fallback retry) */
static int tls_last_err = 0;

/* v0.3 FR-23: fail-closed by default — the parse-only retry needs
 * an explicit opt-in (mget -k / tls_set_insecure(1)). */
static int tls_insecure_ok = 0;

int tls_set_insecure(int allow) {
    int prev = tls_insecure_ok;
    tls_insecure_ok = allow ? 1 : 0;
    return prev;
}

// ============================================================
//  Session
// ============================================================
struct tls_sess {
    /* ---- lwIP plumbing (written from IRQ context) ---- */
    struct tcp_pcb* volatile pcb;
    volatile int    connected;      /* SYN-ACK seen            */
    volatile int    net_err;        /* RST / pcb died          */
    volatile int    fin;            /* server FIN (p == NULL)  */
    volatile int    ring_overflow;  /* ring full -> abort      */
    volatile uint32_t r_head;       /* ring write cursor (IRQ) */
    volatile uint32_t r_tail;       /* ring read cursor (task) */
    uint8_t         ring[TLS_RING_SIZE];

    /* ---- BearSSL engine (task context only) ---- */
    br_ssl_client_context   cc;
    br_x509_minimal_context xc;        /* strict validator      */
    uint8_t                 iobuf[BR_SSL_BUFSIZE_BIDI];
    int    strict;
    int    tls_err;                    /* 0 = clean, see header */
    int    engine_ready;               /* reset() done -> close() is safe */
    char   sni[64];
};

// ============================================================
//  Wall-clock source — CMOS RTC (ports 0x70/0x71)
// ------------------------------------------------------------
//  The strict X.509 validator needs the current date/time to check
//  certificate validity windows. Without it BearSSL fails every
//  chain with BR_ERR_X509_TIME_UNKNOWN (code 53) — found live:
//  raw.githubusercontent.com "verified" never triggered until this
//  was wired. QEMU exposes the host UTC clock on the CMOS RTC.
// ============================================================
static uint8_t tls_rtc_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static int tls_bcd(uint8_t v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

/* days since 1970-01-01 (Howard Hinnant's civil-days algorithm) */
static uint32_t tls_days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468);
}

/* BearSSL's X.509 epoch is NOT the Unix epoch: br_x509_minimal_set_time()
 * wants "days since January 1st, 0 AD (Gregorian)" — that is
 * 719,528 days MORE than days since 1970-01-01. Feeding Unix days
 * made every certificate look 1970 years "not yet valid" (code 54). */
#define TLS_X509_UNIX_TO_0AD  719528u

/* Read the RTC into BearSSL's (days-since-0-AD, seconds-of-day).
 * Retries while an update is in progress; returns 0 on failure
 * (caller then leaves the time unset -> strict verify degrades to
 * the warned fallback path instead of lying about validity). */
static int tls_rtc_now(uint32_t* days, uint32_t* secs) {
    for (int attempt = 0; attempt < 30; attempt++) {
        if (tls_rtc_read(0x0A) & 0x80) continue;   /* update in progress */
        uint8_t st  = tls_rtc_read(0x0B);
        int bin     = (st & 0x04) != 0;
        int is24    = (st & 0x02) != 0;
        uint8_t r0  = tls_rtc_read(0x00);         /* seconds */
        uint8_t r2  = tls_rtc_read(0x02);         /* minutes */
        uint8_t r4  = tls_rtc_read(0x04);         /* hours   */
        uint8_t r7  = tls_rtc_read(0x07);         /* day     */
        uint8_t r8  = tls_rtc_read(0x08);         /* month   */
        uint8_t r9  = tls_rtc_read(0x09);         /* year (2d) */
        uint8_t r32 = tls_rtc_read(0x32);         /* century */
        if (tls_rtc_read(0x0A) & 0x80) continue;   /* rolled over */

        int pm = 0;
        if (!is24 && (r4 & 0x80)) { pm = 1; r4 &= 0x7F; }
        int sec = bin ? r0 : tls_bcd(r0);
        int min = bin ? r2 : tls_bcd(r2);
        int hr  = bin ? r4 : tls_bcd(r4);
        int day = bin ? r7 : tls_bcd(r7);
        int mon = bin ? r8 : tls_bcd(r8);
        int yr  = bin ? r9 : tls_bcd(r9);
        int cen = bin ? r32 : (r32 ? tls_bcd(r32) : 20);
        if (r32 == 0) cen = 20;
        if (!is24) {                 /* 12h -> 24h */
            if (pm && hr < 12) hr += 12;
            if (!pm && hr == 12) hr = 0;
        }
        if (yr < 100 && mon >= 1 && mon <= 12 && day >= 1 && day <= 31 &&
            hr <= 23 && min <= 59 && sec <= 59) {
            *days = tls_days_from_civil(cen * 100 + yr,
                                        (unsigned)mon, (unsigned)day)
                    + TLS_X509_UNIX_TO_0AD;
            *secs = (uint32_t)(hr * 3600 + min * 60 + sec);
            return 1;
        }
    }
    return 0;
}

// ============================================================
//  Entropy — RDRAND when the CPU offers it, jitter mix otherwise
// ============================================================
static int tls_cpu_has_rdrand(void) {
    static int cached = -1;
    if (cached < 0) {
        uint32_t a, b, c, d;
        asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(1) : "memory");
        cached = (c & (1u << 30)) ? 1 : 0;   /* ECX bit 30 */
    }
    return cached;
}

static int tls_rdrand32(uint32_t* out) {
    uint32_t v;
    uint8_t  ok;
    asm volatile("rdrand %0\n\tsetc %b1"
                 : "=r"(v), "=q"(ok)
                 :
                 : "cc");
    *out = v;
    return ok;
}

/* Fill `len` bytes of seed material. On a VM without RDRAND
 * (QEMU's default qemu32 CPU) this falls back to a RDTSC/PIT
 * jitter accumulator — NOT cryptographically strong, but the
 * best a bare-metal kernel without hardware entropy has. */
static void tls_seed(uint8_t* out, size_t len) {
    uint32_t v[12];
    memset(v, 0, sizeof v);

    if (tls_cpu_has_rdrand()) {
        for (unsigned i = 0; i < 12; i++) {
            for (int retry = 0; retry < 8; retry++)
                if (tls_rdrand32(&v[i])) break;
        }
    } else {
        /* jitter mix: RDTSC deltas, tick count, net counters */
        uint32_t st = 0x9E3779B9u;
        uint32_t rx = net_rx_packets(), tx = net_tx_packets();
        for (unsigned i = 0; i < 12; i++) {
            uint32_t lo, hi;
            for (int j = 0; j < 32; j++) {
                asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
                st ^= lo + rol32(st, 7) + hi;
                st *= 0x85EBCA6Bu;
            }
            v[i] = st ^ (rx + tx + sys_now());
        }
    }
    size_t n = len < sizeof v ? len : sizeof v;
    memcpy(out, v, n);
    if (len > n) {               /* expand by xorshift, rarely hit */
        uint32_t x = v[11] | 1u;
        for (size_t i = n; i < len; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            out[i] = (uint8_t)x;
        }
    }
    memset(v, 0, sizeof v);
}

// ============================================================
//  Raw ring: IRQ0 producer -> task consumer.
//  Both sides only ever run with interrupts disabled, and each
//  cursor is written by exactly one side (release semantics via
//  the volatile + net_lock "memory" clobber).
// ============================================================
static uint32_t tls_ring_count(const struct tls_sess* s) {
    return s->r_head - s->r_tail;          /* free-running u32   */
}

static void tls_ring_read(struct tls_sess* s, uint8_t* out, uint32_t n) {
    uint32_t pos  = s->r_tail & (TLS_RING_SIZE - 1);
    uint32_t first = TLS_RING_SIZE - pos;
    if (first > n) first = n;
    memcpy(out, s->ring + pos, first);
    if (n > first) memcpy(out + first, s->ring, n - first);
    s->r_tail += n;
}

// ============================================================
//  lwIP callbacks (IRQ context — memcpy only)
// ============================================================
static err_t tls_connected_cb(void* arg, struct tcp_pcb* pcb, err_t err) {
    struct tls_sess* s = (struct tls_sess*)arg;
    if (err != ERR_OK) { s->net_err = 1; return err; }
    s->connected = 1;
    return ERR_OK;
}

static err_t tls_recv_cb(void* arg, struct tcp_pcb* pcb,
                         struct pbuf* p, err_t err) {
    struct tls_sess* s = (struct tls_sess*)arg;
    if (p == NULL) {                       /* FIN = server EOF */
        s->fin = 1;
        return ERR_OK;                     /* task side closes */
    }
    if (err == ERR_OK) {
        uint32_t tot  = p->tot_len;
        uint32_t head = s->r_head;
        if (tot > TLS_RING_SIZE - (head - s->r_tail)) {
            s->ring_overflow = 1;          /* cannot happen in
                                             practice: ring 16 KB
                                             >> TCP_WND 4.2 KB  */
            pbuf_free(p);
            tcp_abort(pcb);
            s->pcb = NULL;
            return ERR_ABRT;
        }
        uint32_t pos   = head & (TLS_RING_SIZE - 1);
        uint32_t first = TLS_RING_SIZE - pos;
        if (first > tot) first = tot;
        pbuf_copy_partial(p, s->ring + pos, first, 0);
        if (tot > first)
            pbuf_copy_partial(p, s->ring, tot - first, first);
        s->r_head = head + tot;            /* publish last      */
        /* NO tcp_recved() here (v0.1 TLS lesson): acknowledging
         * on arrival re-opens the TCP window while the task may
         * still be mid-decryption with IF=0 — the sender floods
         * the 8 KB NE2000 buffer, frames drop, and GitHub resets
         * the connection ~5 KB into a large page. The window is
         * reopened by the CONSUMER (tls_pump) instead, giving the
         * sender real flow control at our decryption pace. */
    }
    pbuf_free(p);
    return ERR_OK;
}

static void tls_err_cb(void* arg, err_t err) {
    (void)err;
    struct tls_sess* s = (struct tls_sess*)arg;
    s->net_err = 1;
    s->pcb     = NULL;
}

// ============================================================
//  Parse-only X.509 validator (fallback mode)
// ------------------------------------------------------------
//  Accepts any chain, but still decodes the leaf certificate
//  and extracts its public key — the TLS engine needs it to
//  verify the ECDHE ServerKeyExchange signature, so an active
//  attacker still cannot inject data; only the trust decision
//  (who signed the leaf) is skipped. Mirrors the shape of
//  BearSSL's own tools (x509_noanchor / brssl --no-verify).
// ============================================================
struct tls_noverify_ctx {
    const br_x509_class*    vtable;
    br_x509_decoder_context dc;
    uint8_t  leaf[TLS_LEAF_MAX];
    uint32_t leaf_len;
    uint32_t leaf_cap;      /* 0 once the leaf is captured    */
    uint32_t cert_idx;
};
static struct tls_noverify_ctx tls_nv;   /* single session OS  */

static void tls_nv_start_chain(const br_x509_class** ctx,
                               const char* server_name) {
    (void)server_name;
    struct tls_noverify_ctx* c = (struct tls_noverify_ctx*)ctx;
    c->leaf_len = 0;
    c->leaf_cap = TLS_LEAF_MAX;
    c->cert_idx = 0;
}
static void tls_nv_start_cert(const br_x509_class** ctx,
                              uint32_t len) {
    (void)ctx; (void)len;
}
static void tls_nv_append(const br_x509_class** ctx,
                          const unsigned char* buf, size_t len) {
    struct tls_noverify_ctx* c = (struct tls_noverify_ctx*)ctx;
    if (c->cert_idx != 0 || c->leaf_cap == 0) return;
    if (c->leaf_len + len > c->leaf_cap) {   /* leaf too big */
        c->leaf_cap = 0;
        c->leaf_len = 0;
        return;
    }
    memcpy(c->leaf + c->leaf_len, buf, len);
    c->leaf_len += (uint32_t)len;
}
static void tls_nv_end_cert(const br_x509_class** ctx) {
    struct tls_noverify_ctx* c = (struct tls_noverify_ctx*)ctx;
    c->cert_idx++;
}
static unsigned tls_nv_end_chain(const br_x509_class** ctx) {
    struct tls_noverify_ctx* c = (struct tls_noverify_ctx*)ctx;
    br_x509_decoder_init(&c->dc, 0, 0);
    br_x509_decoder_push(&c->dc, c->leaf, c->leaf_len);
    return 0;               /* accept: parse-only mode */
}
static const br_x509_pkey* tls_nv_get_pkey(
        const br_x509_class* const* ctx, unsigned* usages) {
    struct tls_noverify_ctx* c = (struct tls_noverify_ctx*)ctx;
    if (usages) *usages = BR_KEYTYPE_SIGN | BR_KEYTYPE_KEYX;
    return br_x509_decoder_get_pkey(&c->dc);
}
static const br_x509_class tls_nv_vtable = {
    sizeof(struct tls_noverify_ctx),
    tls_nv_start_chain,
    tls_nv_start_cert,
    tls_nv_append,
    tls_nv_end_cert,
    tls_nv_end_chain,
    tls_nv_get_pkey
};

// ============================================================
//  Error reporting
// ============================================================
static const char* tls_x509_reason(int e) {
    switch (e) {
    case BR_ERR_X509_NOT_TRUSTED:       return "no trusted root CA";
    case BR_ERR_X509_EXPIRED:           return "certificate expired";
    case BR_ERR_X509_BAD_SIGNATURE:     return "bad signature";
    case BR_ERR_X509_UNSUPPORTED:       return "unsupported feature";
    case BR_ERR_X509_WRONG_KEY_TYPE:    return "unsupported key type or curve";
    case BR_ERR_X509_WEAK_PUBLIC_KEY:   return "weak public key";
    case BR_ERR_X509_BAD_SERVER_NAME:   return "server name mismatch";
    case BR_ERR_X509_LIMIT_EXCEEDED:    return "chain too long";
    default:                            return "certificate rejected";
    }
}

// ============================================================
//  The pump — one step of engine progress from the task side
// ============================================================
/* Drive the engine until one of the `want` states (or failure).
 * return 1 = a wanted state is ready, 0 = closed/failed/EOF.
 * Inactivity timeout resets on progress; a hard cap bounds the
 * total time of a single call. */
static int tls_pump(struct tls_sess* s, unsigned want,
                    uint32_t idle_ms) {
    uint32_t t_idle = sys_now();
    uint32_t t_hard = sys_now();

    for (;;) {
        unsigned st = br_ssl_engine_current_state(&s->cc.eng);

        if (st & BR_SSL_CLOSED) {
            s->tls_err = br_ssl_engine_last_error(&s->cc.eng);
            return 0;
        }
        if (st & want)
            return 1;

        int progressed = 0;

        /* (a) ring bytes -> engine record input */
        if ((st & BR_SSL_RECVREC) && tls_ring_count(s) != 0) {
            uint32_t f = net_lock();
            size_t rn;
            unsigned char* rb =
                br_ssl_engine_recvrec_buf(&s->cc.eng, &rn);
            uint32_t avail = tls_ring_count(s);
            if (rn > avail) rn = avail;
            tls_ring_read(s, rb, (uint32_t)rn);
            /* consume-side ACK: re-opens the receive window at our
             * decryption pace (see tls_recv_cb) */
            if (s->pcb)
                tcp_recved((struct tcp_pcb*)s->pcb, (u16_t)rn);
            br_ssl_engine_recvrec_ack(&s->cc.eng, rn);
            net_unlock(f);
            progressed = 1;
        }

        /* (b) engine output records -> TCP */
        st = br_ssl_engine_current_state(&s->cc.eng);
        if (st & BR_SSL_SENDREC) {
            uint32_t f = net_lock();
            if (s->pcb) {
                size_t sn;
                unsigned char* sb =
                    br_ssl_engine_sendrec_buf(&s->cc.eng, &sn);
                u16_t snd = tcp_sndbuf(s->pcb);
                if (sn > snd) sn = snd;
                if (sn != 0) {
                    err_t e = tcp_write(s->pcb, sb, (u16_t)sn,
                                        TCP_WRITE_FLAG_COPY);
                    if (e == ERR_OK) {
                        br_ssl_engine_sendrec_ack(&s->cc.eng, sn);
                        tcp_output(s->pcb);
                        progressed = 1;
                    }
                }
            }
            net_unlock(f);
        }

        /* fatal network conditions */
        if (s->net_err)      { s->tls_err = -2; return 0; }
        if (s->ring_overflow){ s->tls_err = -3; return 0; }

        /* server EOF while the engine still wants bytes */
        st = br_ssl_engine_current_state(&s->cc.eng);
        if ((st & BR_SSL_RECVREC) && s->fin &&
            tls_ring_count(s) == 0) {
            s->tls_err = -1;            /* abrupt EOF (no close_notify) */
            return 0;
        }

        uint32_t now = sys_now();
        if (progressed) t_idle = now;
        if (now - t_idle > idle_ms) { s->tls_err = -4; return 0; }
        if (now - t_hard > TLS_HARDCAP_MS) { s->tls_err = -4; return 0; }
    }
}

// ============================================================
//  Connect / handshake
// ============================================================
static struct tls_sess* tls_attempt(const char* host,
                                    const ip_addr_t* dst,
                                    int port, int strict) {
    struct tls_sess* s =
        (struct tls_sess*)malloc(sizeof *s);
    if (!s) {
        printf("TLS: kernel heap exhausted (need %u KB)\n",
               (unsigned)(sizeof *s / 1024));
        return NULL;
    }
    memset(s, 0, sizeof *s);
    s->strict = strict;
    for (int i = 0; host[i] && i < 63; i++) s->sni[i] = host[i];
    s->sni[63] = 0;

    /* ---- TCP connect (same pattern as mget/tcpping) ---- */
    uint32_t f = net_lock();
    struct tcp_pcb* pcb = tcp_new();
    if (!pcb) {
        net_unlock(f);
        free(s);
        printf("TLS: out of PCBs\n");
        return NULL;
    }
    s->pcb = pcb;
    tcp_arg(pcb, s);
    tcp_err(pcb, tls_err_cb);
    tcp_recv(pcb, tls_recv_cb);
    err_t e = tcp_connect(pcb, dst, (u16_t)port, tls_connected_cb);
    net_unlock(f);
    if (e != ERR_OK) {
        s->tls_err = -2;
        tls_close(s);
        return NULL;
    }

    uint32_t t0 = sys_now();
    while (!s->connected && !s->net_err &&
           (sys_now() - t0) < TLS_CONNECT_MS) { }
    if (!s->connected) {
        printf("TLS: TCP connect failed - refused or unreachable\n");
        s->tls_err = -2;
        tls_close(s);
        return NULL;
    }

    /* ---- BearSSL engine setup ---- */
    /* The engine carries its own HMAC-DRBG; we only inject the
     * seed (RDRAND / jitter mix). On a freestanding build
     * br_prng_seeder_system() is a no-op stub, so this injection
     * is what marks the RNG as properly seeded. */
    uint8_t seed[48];
    tls_seed(seed, sizeof seed);

    /* init_full wires the full TLS 1.0-1.2 client profile with
     * the i31 RSA / P-256 EC / AES-CT / ChaCha20 implementations
     * and the strict X.509 minimal validator. */
    br_ssl_client_init_full(&s->cc, &s->xc,
                            (const br_x509_trust_anchor*)eq_tls_anchors,
                            (size_t)eq_tls_anchors_num);

    if (strict) {
        /* certificate validity windows need the current time */
        uint32_t days, secs;
        if (tls_rtc_now(&days, &secs))
            br_x509_minimal_set_time(&s->xc, days, secs);
    }

    if (!strict) {                        /* fallback: parse-only */
        tls_nv.vtable = &tls_nv_vtable;
        br_ssl_engine_set_x509(&s->cc.eng, &tls_nv.vtable);
    }

    br_ssl_engine_inject_entropy(&s->cc.eng, seed, sizeof seed);
    memset(seed, 0, sizeof seed);
    br_ssl_engine_set_buffer(&s->cc.eng, s->iobuf,
                             sizeof s->iobuf, 1);
    if (!br_ssl_client_reset(&s->cc, s->sni, 0)) {
        printf("TLS: engine init failed (no entropy)\n");
        s->tls_err = -5;
        tls_close(s);
        return NULL;
    }
    s->engine_ready = 1;

    /* ---- run the handshake ---- */
    if (!tls_pump(s, BR_SSL_SENDAPP | BR_SSL_RECVAPP,
                  TLS_HANDSHAKE_MS)) {
        int err = s->tls_err;
        if (err > 0)
            printf("TLS: handshake failed - %s (code %d)\n",
                   tls_x509_reason(err), err);
        else if (err == -1)
            printf("TLS: server closed during handshake\n");
        else if (err == -2)
            printf("TLS: connection lost during handshake\n");
        else
            printf("TLS: handshake timed out\n");
        tls_close(s);
        return NULL;
    }
    return s;
}

struct tls_sess* tls_connect_auto(const char* host,
                                  const void* dst, int port) {
    printf("TLS: handshake %s:%d (TLS 1.2, BearSSL, "
           "%u root CAs)\n", host, port, eq_tls_anchors_num);

    struct tls_sess* s = tls_attempt(host, (const ip_addr_t*)dst,
                                     port, 1);
    if (s) {
        printf("TLS: connection established - %s, chain verified\n",
               br_ssl_engine_get_version(&s->cc.eng));
        return s;
    }

    /* v0.3 FR-23 (fail-closed): a certificate problem REFUSES the
     * connection unless the user explicitly opted into the warned
     * parse-only retry (mget -k / tls_set_insecure(1)). A name
     * mismatch is a real attack signal - never falls back. */
    int e = tls_last_err;
    if (e >= BR_ERR_X509_INVALID_VALUE &&
        e <= BR_ERR_X509_NOT_TRUSTED &&
        e != BR_ERR_X509_BAD_SERVER_NAME) {
        if (!tls_insecure_ok) {
            printf("TLS: FAILED: %s (code %d)\n",
                   tls_x509_reason(e), e);
            printf("TLS: connection REFUSED - certificate not trusted "
                   "(fail-closed)\n");
            printf("TLS: use 'mget -k <url>' to allow an ENCRYPTED but "
                   "unverified connection\n");
            return NULL;
        }
        printf("TLS: warning: %s (code %d)\n",
               tls_x509_reason(e), e);
        printf("TLS: retrying WITHOUT certificate verification "
                   "(user opted in)\n");
        s = tls_attempt(host, (const ip_addr_t*)dst, port, 0);
        if (s) {
            printf("TLS: connection established - %s, "
                   "ENCRYPTED but NOT VERIFIED\n",
                   br_ssl_engine_get_version(&s->cc.eng));
        }
    }
    tls_set_insecure(0);              /* one-shot: reset after use */
    return s;
}

// ============================================================
//  Application data I/O
// ============================================================
int tls_write_all(struct tls_sess* s, const void* buf, int len) {
    const uint8_t* p = (const uint8_t*)buf;
    while (len > 0) {
        if (!tls_pump(s, BR_SSL_SENDAPP | BR_SSL_CLOSED,
                      TLS_IO_TIMEOUT_MS))
            return -1;
        size_t n;
        unsigned char* b = br_ssl_engine_sendapp_buf(&s->cc.eng, &n);
        if (n > (size_t)len) n = (size_t)len;
        memcpy(b, p, n);
        br_ssl_engine_sendapp_ack(&s->cc.eng, n);
        br_ssl_engine_flush(&s->cc.eng, 1);
        p += n;
        len -= (int)n;
    }
    return 0;
}

int tls_read(struct tls_sess* s, uint8_t* out, int maxlen) {
    if (maxlen <= 0) return 0;
    if (!tls_pump(s, BR_SSL_RECVAPP | BR_SSL_CLOSED,
                  TLS_IO_TIMEOUT_MS)) {
        int e = s->tls_err;
        if (e == 0)  return 0;    /* clean close_notify  */
        if (e == -1) return -1;   /* abrupt FIN          */
        if (e == -4) return -3;   /* timeout             */
        return -2;                /* TLS/protocol error  */
    }
    size_t n;
    unsigned char* b = br_ssl_engine_recvapp_buf(&s->cc.eng, &n);
    if (n > (size_t)maxlen) n = (size_t)maxlen;
    memcpy(out, b, n);
    br_ssl_engine_recvapp_ack(&s->cc.eng, n);
    return (int)n;
}

void tls_close(struct tls_sess* s) {
    if (!s) return;
    tls_last_err = s->tls_err;

    /* best-effort close_notify (only safe once the engine was
     * actually initialised - never on a zeroed context) */
    if (s->engine_ready &&
        !(br_ssl_engine_current_state(&s->cc.eng) & BR_SSL_CLOSED)) {
        br_ssl_engine_close(&s->cc.eng);
        tls_pump(s, BR_SSL_CLOSED, 600);   /* flush it out */
    }

    if (s->pcb) {
        uint32_t f = net_lock();
        tcp_abort(s->pcb);       /* RST: fine for one-shot HTTP */
        s->pcb = NULL;
        net_unlock(f);
    }
    free(s);
}

// ---- accessors ----
int tls_last_error(const struct tls_sess* s) { return s->tls_err; }
int tls_session_verified(const struct tls_sess* s) {
    return s && s->strict && s->tls_err == 0;
}
