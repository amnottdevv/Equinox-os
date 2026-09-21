// ============================================================
//  httpd.c — Equinox OS mini web server (v10.12)
// ------------------------------------------------------------
//  lwIP raw TCP API, listening on :80, serving:
//    GET /            -> status page (generated HTML)
//    GET /<ramfspath> -> a RAMFS file (e.g. /doom1.wad,
//                        /equinox/games/doom.mrp, /test/hello.c)
//  HTTP/1.0 + Connection: close. File bodies are sent ZERO-COPY
//  from the RAMFS content node (memory is stable for the
//  connection's lifetime).
//
//  ALL callbacks run from net_poll() (IRQ0 timer / IRQ9 NIC
//  context) — NEVER printf on this path; counters only (same
//  pattern as the ne2000 driver).
//
//  Host-side demo:
//    qemu ... -netdev user,id=net0,hostfwd=tcp::8080-:80 \
//             -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
//  then open http://localhost:8080/ in a browser on the host.
// ============================================================
#include <stdint.h>
#include <stddef.h>

#include "net.h"

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"

#include "library/header/stdio.h"      // snprintf
#include "library/header/libstring.h"  // strcmp/memcmp/strlen/strstr
#include "library/header/fs_ram.h"     // RAMFS listing + files

// sys_now() is defined in net_lwip.c (the lwIP port).
extern "C" uint32_t sys_now(void);

#define HTTPD_PORT        80
#define HTTPD_MAX_CONN    4
#define HTTPD_BODY_MAX    2048
#define HTTPD_HDR_MAX     100

struct http_state {
    int             used;      /* slot in use */
    const uint8_t*  data;      /* body (RAMFS content / body buffer) */
    uint32_t        total;     /* body length */
    uint32_t        offset;    /* send position */
    uint16_t        hdr_len;
    uint8_t         hdr_done;
    char            hdr[HTTPD_HDR_MAX];
    char            body[HTTPD_BODY_MAX];
};

static struct tcp_pcb*   httpd_pcb;
static volatile int      httpd_runs;
static volatile uint32_t httpd_hits;
static volatile uint32_t httpd_bytes;

static struct http_state hs_pool[HTTPD_MAX_CONN];

// ---- mime sederhana ----
static const char* mime_for(const char* path) {
    int n = (int)strlen(path);
    if (n >= 5 && strcmp(path + n - 5, ".html") == 0) return "text/html";
    if (n >= 4 && strcmp(path + n - 4, ".htm")  == 0) return "text/html";
    if (n >= 4 && strcmp(path + n - 4, ".txt")  == 0) return "text/plain";
    if (n >= 4 && strcmp(path + n - 4, ".md")   == 0) return "text/plain";
    if (n >= 3 && strcmp(path + n - 3, ".md")   == 0) return "text/plain";
    if (n >= 2 && strcmp(path + n - 2, ".c")    == 0) return "text/plain";
    if (n >= 4 && strcmp(path + n - 4, ".cpp")  == 0) return "text/plain";
    return "application/octet-stream";
}

// ---- send as much as sndbuf allows; done -> close ----
static err_t httpd_send(struct tcp_pcb* pcb, struct http_state* hs) {
    if (!hs->hdr_done) {
        u16_t snd = tcp_sndbuf(pcb);
        if (snd < hs->hdr_len) return ERR_OK;      /* later via poll */
        err_t e = tcp_write(pcb, hs->hdr, hs->hdr_len, TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK) return e;
        hs->hdr_done = 1;
    }
    while (hs->offset < hs->total) {
        u16_t snd = tcp_sndbuf(pcb);
        if (snd == 0) break;                       /* tunggu sent_cb */
        uint32_t chunk = hs->total - hs->offset;
        if (chunk > (uint32_t)snd) chunk = snd;
        if (chunk > 0xFFFFu)       chunk = 0xFFFFu;
        /* ZERO-COPY: content RAMFS stabil selama koneksi aktif */
        err_t e = tcp_write(pcb, hs->data + hs->offset, (u16_t)chunk, 0);
        if (e != ERR_OK) break;
        hs->offset   += chunk;
        httpd_bytes  += chunk;
    }
    tcp_output(pcb);
    if (hs->offset >= hs->total) {
        if (tcp_close(pcb) == ERR_OK)
            hs->used = 0;                          /* slot freed */
        /* tcp_close failed (queue not empty yet) -> poll retries */
    }
    return ERR_OK;
}

// ---- 404 ----
static void httpd_404(struct http_state* hs) {
    static const char msg[] = "404 not found\n";
    hs->data  = (const uint8_t*)msg;
    hs->total = sizeof(msg) - 1;
    hs->hdr_len = (uint16_t)snprintf(hs->hdr, HTTPD_HDR_MAX,
        "HTTP/1.0 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %u\r\n\r\n", (unsigned)hs->total);
}

// ---- listing direktori utk halaman index ----
static int emit_dir(struct fs_node* dir, const char* label,
                    const char* prefix, char* b, int n) {
    if (!dir) return n;
    int rem = HTTPD_BODY_MAX - n - 8;
    if (rem < 80) return n;
    n += snprintf(b + n, (size_t)rem, "%s\n", label);
    for (struct fs_node* c = dir->children; c; c = c->next) {
        if (c->is_dir) continue;
        rem = HTTPD_BODY_MAX - n - 8;
        if (rem < 120) {
            n += snprintf(b + n, 64, "  ...\n");
            break;
        }
        n += snprintf(b + n, (size_t)rem, "  <a href=\"%s%s\">%s</a> (%u)\n",
                      prefix, c->name, c->name, c->size);
    }
    return n;
}

// ---- route GET ----
static void httpd_route(struct http_state* hs, const char* path) {
    httpd_hits++;

    if (path[0] == 0 || strcmp(path, "/") == 0 ||
        strcmp(path, "/index.html") == 0) {
        /* ---- halaman status dibangkitkan ---- */
        uint32_t w[10];
        net_get_info(w);

        char* b = hs->body;
        int   n = 0;
#define EMIT(...) do { int rem_ = HTTPD_BODY_MAX - n - 8; \
            if (rem_ > 0) { int w_ = snprintf(b + n, (size_t)rem_, \
                __VA_ARGS__); n += (w_ < rem_) ? w_ : (rem_ - 1); } } while (0)

        EMIT("<html><head><title>Equinox OS</title></head>"
             "<body bgcolor=\"#0e0e16\" text=\"#e8e8e8\">"
             "<h2>Equinox OS v0.1 Beta &mdash; TCP/IP alive</h2><pre>");
        EMIT("uptime : %u s\n",  (unsigned)(sys_now() / 1000));
        EMIT("nic    : ne0 (NE2000 ISA 0x300 irq9)\n");
        EMIT("ip     : %u.%u.%u.%u  gw %u.%u.%u.%u  (%s)\n",
             (unsigned)(w[2] & 255),        (unsigned)((w[2] >> 8) & 255),
             (unsigned)((w[2] >> 16) & 255),(unsigned)((w[2] >> 24) & 255),
             (unsigned)(w[4] & 255),        (unsigned)((w[4] >> 8) & 255),
             (unsigned)((w[4] >> 16) & 255),(unsigned)((w[4] >> 24) & 255),
             w[1] ? "dhcp" : "static");
        EMIT("mac    : %02x:%02x:%02x:%02x:%02x:%02x\n",
             (unsigned)(w[5] & 255),        (unsigned)((w[5] >> 8) & 255),
             (unsigned)((w[5] >> 16) & 255),(unsigned)((w[5] >> 24) & 255),
             (unsigned)(w[6] & 255),        (unsigned)((w[6] >> 8) & 255));
        EMIT("rx/tx  : %u/%u pkts  drop %u\n",
             w[7], w[8], w[9]);
        EMIT("httpd  : %u hits  %u bytes served\n",
             httpd_hits, httpd_bytes);
        EMIT("</pre><hr><pre>RAMFS:\n");

        n = emit_dir(fs_get_root(), "/", "", b, n);
        n = emit_dir(fs_get_node_from_path(fs_get_root(), "/equinox/games"),
                     "/equinox/games", "/equinox/games/", b, n);
        n = emit_dir(fs_get_node_from_path(fs_get_root(), "/equinox/tools"),
                     "/equinox/tools", "/equinox/tools/", b, n);
        n = emit_dir(fs_get_node_from_path(fs_get_root(), "/test"),
                     "/test", "/test/", b, n);
        EMIT("</pre></body></html>\n");
#undef EMIT

        hs->data  = (const uint8_t*)hs->body;
        hs->total = (uint32_t)n;
        hs->hdr_len = (uint16_t)snprintf(hs->hdr, HTTPD_HDR_MAX,
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %u\r\n\r\n", (unsigned)hs->total);
        return;
    }

    /* reject path traversal - the RAMFS is a read-only share */
    if (strstr(path, "..")) { httpd_404(hs); return; }

    struct fs_node* node = fs_get_node_from_path(fs_get_root(), path);
    if (node && !node->is_dir) {
        hs->data  = (const uint8_t*)node->content;
        hs->total = node->size;
        hs->hdr_len = (uint16_t)snprintf(hs->hdr, HTTPD_HDR_MAX,
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %u\r\n\r\n",
            mime_for(path), (unsigned)node->size);
    } else {
        httpd_404(hs);
    }
}

// ---- callbacks TCP ----
static err_t httpd_recv_cb(void* arg, struct tcp_pcb* pcb,
                           struct pbuf* p, err_t err) {
    struct http_state* hs = (struct http_state*)arg;
    if (p == NULL) {                       /* client menutup */
        tcp_close(pcb);
        hs->used = 0;
        return ERR_OK;
    }
    u16_t tot = p->tot_len;
    int handled = 0;
    if (err == ERR_OK && hs->total == 0 && !hs->hdr_done) {
        char req[128];
        int len = pbuf_copy_partial(p, req, sizeof(req) - 1, 0);
        req[len] = 0;
        if (len >= 5 && memcmp(req, "GET ", 4) == 0) {
            char path[96];
            int i = 4, j = 0;
            while (req[i] && req[i] != ' ' && req[i] != '\r' && j < 95)
                path[j++] = req[i++];
            path[j] = 0;
            httpd_route(hs, path);
        } else {
            httpd_404(hs);                 /* non-GET -> 404 */
        }
        handled = 1;
    }
    /* MUST happen before send/close: restore rcv_wnd first.
     * Phase-C lesson (pcap): calling tcp_close() from recv_cb while
     * rcv_wnd is still reduced (data not yet tcp_recved'ed) makes
     * lwIP 2.1.3 send an RST and PURGE the pcb — everything already
     * tcp_write'n is discarded unsent. */
    tcp_recved(pcb, tot);
    pbuf_free(p);
    if (handled) httpd_send(pcb, hs);
    return ERR_OK;
}

static err_t httpd_sent_cb(void* arg, struct tcp_pcb* pcb, u16_t len) {
    (void)len;
    return httpd_send(pcb, (struct http_state*)arg);
}

static err_t httpd_poll_cb(void* arg, struct tcp_pcb* pcb) {
    struct http_state* hs = (struct http_state*)arg;
    if (hs->total) return httpd_send(pcb, hs);
    return ERR_OK;
}

static void httpd_err_cb(void* arg, err_t err) {
    (void)err;
    struct http_state* hs = (struct http_state*)arg;
    if (hs) hs->used = 0;
}

static err_t httpd_accept_cb(void* arg, struct tcp_pcb* pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || pcb == NULL) return ERR_VAL;

    struct http_state* hs = NULL;
    for (int i = 0; i < HTTPD_MAX_CONN; i++)
        if (!hs_pool[i].used) { hs = &hs_pool[i]; break; }
    if (!hs) { tcp_close(pcb); return ERR_OK; }    /* pool full */

    memset(hs, 0, sizeof(*hs));
    hs->used = 1;
    httpd_hits++;

    tcp_arg(pcb, hs);
    tcp_recv(pcb, httpd_recv_cb);
    tcp_sent(pcb, httpd_sent_cb);
    tcp_poll(pcb, httpd_poll_cb, 4);
    tcp_err(pcb, httpd_err_cb);
    return ERR_OK;
}

// ---- API publik ----
int net_httpd_start(void) {
    if (httpd_runs) return 1;              /* idempotent */

    uint32_t f = net_lock();
    struct tcp_pcb* p = tcp_new();
    if (!p) { net_unlock(f); return 0; }
    if (tcp_bind(p, IP_ADDR_ANY, HTTPD_PORT) != ERR_OK) {
        tcp_close(p);
        net_unlock(f);
        return 0;
    }
    p = tcp_listen(p);
    if (!p) { net_unlock(f); return 0; }
    tcp_accept(p, httpd_accept_cb);
    httpd_pcb  = p;
    httpd_runs = 1;
    net_unlock(f);
    return 1;
}

int      net_httpd_running(void) { return httpd_runs; }
uint32_t net_httpd_hits(void)    { return httpd_hits; }
uint32_t net_httpd_bytes(void)   { return httpd_bytes; }
