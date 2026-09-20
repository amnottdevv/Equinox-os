// ============================================================
//  net_lwip.c — lwIP <-> NE2000 <-> Equinox OS shell glue
// ------------------------------------------------------------
//  Model: NO_SYS=1 cooperative polling, v0.3 FR-09 two-stage:
//    * ne2000_isr (IRQ9): ONLY copies frames from the NIC into the
//      RX RING (bounded memcpy, no lwIP) — heavy traffic can no
//      longer stall the timer/keyboard/mouse IRQs.
//    * net_service()    : drains the ring -> pbuf -> ethernet_input
//      + sys_check_timeouts(). Runs in TASK context only: the
//      dedicated "net" kernel task (net_task_loop), the boot-time
//      DHCP wait, and blocking waits (mget/ping). ALWAYS under
//      net_lock (cli-guard) -> no re-entrancy.
//    * net_poll()       : legacy alias of net_service() (task ctx).
//    * sys_now()        : from get_tick() (PIT 100 Hz).
//
//  This file is compiled as C++ (the makefile .c rule uses g++),
//  while lwIP files are compiled as plain C — interop is safe
//  because lwIP headers carry __cplusplus guards (extern "C").
// ============================================================
#include <stdint.h>
#include <stddef.h>

#include "net.h"
#include "nic.h"
#include "ne2000.h"      /* NE_MAXFRAME only (driver detail) */
#include "tls_client.h"              /* v0.1: mget https:// (BearSSL) */

#include "library/header/stdio.h"
#include "library/header/libstring.h"
#include "library/header/timer.h"
#include "library/header/task.h"   /* v0.3 FR-09: nettask */

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "lwip/icmp.h"
#include "lwip/raw.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"                 /* v10.12: DHCP              */
#include "lwip/tcp.h"                  /* v10.12: mget (HTTP GET)   */
#include "lwip/dns.h"                  /* v10.13: hostname resolve  */
#include "lwip/err.h"
#include "library/header/malloc.h"    /* kernel heap (mget buffer) */
#include "library/header/fs_ram.h"    /* save mget results to RAMFS */
#include "library/header/itoa_atoi.h" /* atoi (parse ports)        */

// ---------------- state ------------------------------------
static struct netif nif;
static volatile int      net_up = 0;

static volatile uint32_t st_rx    = 0;
static volatile uint32_t st_tx    = 0;
static volatile uint32_t st_drop  = 0;

/* v10.12: 0 = not initialized, 1 = static fallback, 2 = DHCP */
static volatile int      dhcp_mode = 0;

static uint8_t net_txbuf[NE_MAXFRAME] __attribute__((aligned(16)));
static uint8_t net_rxbuf[NE_MAXFRAME] __attribute__((aligned(16)));

// ---------------- cli-guard --------------------------------
// net_lock()/net_unlock() are inline in net.h (v10.12: also used
// by httpd.c and syscall.cpp).
// -----------------------------------------------------------

// ---------------- sys_now (needed by timeouts.c) ------------
extern "C" uint32_t sys_now(void) {
    uint32_t hz  = timer_freq_hz();
    uint32_t per = (hz == 0) ? 10 : (1000 / hz);
    if (per == 0) per = 1;
    return get_tick() * per;
}

// ---------------- netif glue -------------------------------
static err_t low_level_output(struct netif* netif, struct pbuf* p) {
    (void)netif;
    if (p->tot_len > NE_MAXFRAME) return ERR_MEM;

    int len = pbuf_copy_partial(p, net_txbuf, p->tot_len, 0);
    if (len <= 0) return ERR_BUF;

    if (nic_send(net_txbuf, len) == 0) {
        st_tx++;
        return ERR_OK;
    }
    return ERR_TIMEOUT;
}

static err_t netif_init_cb(struct netif* netif) {
    netif->name[0]     = 'n';
    netif->name[1]     = 'e';
    netif->output      = etharp_output;
    netif->linkoutput  = low_level_output;
    netif->hwaddr_len  = 6;
    memcpy(netif->hwaddr, nic_mac(), 6);
    netif->mtu         = 1500;
    netif->flags       = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    return ERR_OK;
}

// ---------------- API: init / poll -------------------------
int net_init(void) {
    /* v0.3 (FR-13): the stack talks to the active NIC via the
     * registry (kernel/net/nic.c) — ne2000 is just the first driver.
     * net_nic_init() also reports recognized PCI NICs (e1000...). */
    if (!net_nic_init()) {
        printf("net: no NIC driver claimed a card - network off\n");
        return 0;
    }

    uint32_t f = net_lock();
    lwip_init();

    /* Static fallback (slirp defaults) — used if DHCP fails. */
    ip4_addr_t ip, nm, gw;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&nm, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);

    netif_add(&nif, &ip, &nm, &gw, NULL, netif_init_cb, ethernet_input);
    netif_set_up(&nif);
    netif_set_link_up(&nif);
    /* v10.13: default netif (routing to destinations outside the
     * subnet, e.g. internet DNS results) + fallback DNS server
     * 10.0.2.3 = slirp's built-in DNS forwarder. DHCP overrides
     * it via option 6 when present. */
    netif_set_default(&nif);
    {
        ip4_addr_t dnssrv;
        IP4_ADDR(&dnssrv, 10, 0, 2, 3);
        dns_setserver(0, &dnssrv);
    }

    /* v10.12: net_up is set HERE (not at the very end) so IRQ0
     * starts polling the stack while DHCP is pending — DORA needs
     * incoming UDP packets processed by net_poll from the timer
     * tick. */
    net_up = 1;
    net_unlock(f);

    /* --- DHCP: slirp's dhcpd answers within tens of ms --- */
    f = net_lock();
    dhcp_start(&nif);
    net_unlock(f);

    uint32_t t0 = sys_now();
    while (!dhcp_supplied_address(&nif) && (sys_now() - t0) < 3000) {
        /* v0.3 FR-09: no tasks exist yet (this runs BEFORE
         * task_init0) and IRQ0 no longer services lwIP — poll the
         * RX ring + timers ourselves right here. */
        net_service();
    }

    int dhcp_ok = dhcp_supplied_address(&nif);
    if (dhcp_ok) {
        dhcp_mode = 2;
    } else {
        f = net_lock();
        dhcp_stop(&nif);
        netif_set_addr(&nif, &ip, &nm, &gw);
        net_unlock(f);
        dhcp_mode = 1;
    }

    const uint8_t* m = nic_mac();
    printf("net: ne0 up  mac %02x:%02x:%02x:%02x:%02x:%02x\n",
           m[0], m[1], m[2], m[3], m[4], m[5]);
    /* ip4addr_ntoa uses ONE static buffer -> one printf per address. */
    printf("net:      ip %s", ipaddr_ntoa(&nif.ip_addr));
    printf("  nm %s", ipaddr_ntoa(&nif.netmask));
    printf("  gw %s\n", ipaddr_ntoa(&nif.gw));
    printf("net:      config: %s\n",
           dhcp_ok ? "dhcp" : "static (dhcp timeout)");
    return 1;
}

/* ============================================================
 *  v0.3 FR-09 — RX RING (ISR -> task decoupling)
 * ------------------------------------------------------------
 *  Single producer (IRQ9 ISR) + single consumer (net_service in
 *  task context). 16 x 1536 B slots in .bss. When the ring is
 *  full the ISR drops the frame (counted) — lwIP/TCP retransmits
 *  recover; the timer/keyboard IRQs are never held hostage.
 * ============================================================ */
#define NET_RX_RING       16u
#define NET_RX_FRAME_MAX  1536u

static volatile uint32_t rxr_head = 0;   /* producer cursor (ISR)     */
static volatile uint32_t rxr_tail = 0;   /* consumer cursor (task)    */
static uint16_t          rxr_len[NET_RX_RING];
static uint8_t           rxr_buf[NET_RX_RING][NET_RX_FRAME_MAX];
static volatile uint32_t rxr_drops = 0;

/* IRQ9 context: pull frames off the NIC into the ring. Returns the
 * number of frames stored. NO lwIP calls here — memcpy + cursors. */
int net_rxr_drain_isr(void) {
    int stored = 0;
    while (1) {
        uint32_t h = rxr_head;
        uint32_t next = (h + 1u) % NET_RX_RING;
        if (next == rxr_tail) { rxr_drops++; break; }    /* ring full */
        int len = nic_recv(rxr_buf[h], NET_RX_FRAME_MAX);
        if (len <= 0) break;                             /* NIC empty */
        rxr_len[h] = (uint16_t)len;
        asm volatile("" ::: "memory");   /* publish len before cursor */
        rxr_head = next;
        stored++;
    }
    return stored;
}

/* Task context: feed every ringed frame to lwIP + run the timers.
 * Returns the number of frames accepted. This is the ONLY place
 * lwIP processes input now. */
int net_service(void) {
    if (!net_up) return 0;
    uint32_t f = net_lock();
    int fed = 0;

    while (rxr_tail != rxr_head) {
        uint32_t t = rxr_tail;
        uint16_t len = rxr_len[t];
        struct pbuf* p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p == NULL) {
            st_drop++;                   /* pool exhausted: drop  */
        } else {
            pbuf_take(p, rxr_buf[t], (u16_t)len);
            if (nif.input(p, &nif) != ERR_OK) {
                pbuf_free(p);
                st_drop++;
            } else {
                st_rx++;
                fed++;
            }
        }
        rxr_tail = (t + 1u) % NET_RX_RING;
        asm volatile("" ::: "memory");
    }

    /* lwIP TCP/ARP/ICMP/DNS timers */
    sys_check_timeouts();

    net_unlock(f);
    return fed;
}

/* Legacy alias — TASK CONTEXT ONLY since v0.3 (the ISRs no longer
 * call this; they enqueue into the RX ring instead). */
void net_poll(void) {
    net_service();
}

/* v0.3 FR-09: IRQ0 no longer runs the stack (kept for ABI — no-op). */
void net_timer_tick(void) {
}

/* ---- the "net" kernel task ----------------------------------
 * A console-less round-robin task that services the stack ~100 Hz
 * (and hot-drains with yields while frames keep arriving). Created
 * by net_start_task() from kernel_main AFTER task_init0(). */
/* task.h (included above, extern "C") provides task_create_kernel /
 * task_kernel_start / task_sleep with the right linkage. */

static struct Task* g_net_task = NULL;   /* forward (defined below) */

static void net_task_loop(void* arg) {
    (void)arg;
    task_kernel_start();                /* first-activation setup    */
    while (1) {
        int fed = net_service();
        if (fed > 0) {
            /* traffic in flight: yield, then drain again immediately */
            asm volatile("pause" ::: "memory");
            int more = net_service();
            if (more == 0) task_sleep(10);
            else task_yield();
        } else {
            task_sleep(10);             /* idle: 100 Hz heartbeat    */
        }
    }
}

int net_start_task(void) {
    if (!net_up) return 0;              /* no NIC: nothing to service */
    g_net_task = task_create_kernel("net", net_task_loop, NULL);
    return g_net_task ? 1 : 0;
}

/* v0.3 debug: ring + nettask state for the `netdbg` shell command —
 * diagnoses the "network dies after heavy disk I/O" hunt. */
extern "C" int task_sched_lock_depth(void);

void net_dbg_dump(void) {
    printf("netdbg: ring head=%u tail=%u drops=%u\n",
           rxr_head, rxr_tail, rxr_drops);
    printf("netdbg: isr=%u rx=%u tx=%u drop=%u SCHEDLOCK=%d\n",
           nic_irq_count(), st_rx, st_tx, st_drop,
           task_sched_lock_depth());
    if (g_net_task) {
        const char* st = "?";
        switch (g_net_task->state) {
            case 0: st = "FREE"; break;
            case 1: st = "READY"; break;
            case 2: st = "RUNNING"; break;
            case 3: st = "BLOCKED"; break;
            case 4: st = "DEAD"; break;
        }
        printf("netdbg: nettask state=%s sleep_until=%u now=%u used=%d\n",
               st, g_net_task->sleep_until, get_tick(), g_net_task->used);
    } else {
        printf("netdbg: nettask NOT created\n");
    }
}

// ---------------- info / getters ---------------------------
int              net_is_up(void)        { return net_up; }
const uint8_t*   net_mac(void)          { return nic_mac(); }
uint32_t         net_rx_packets(void)   { return st_rx; }
uint32_t         net_tx_packets(void)   { return st_tx; }
uint32_t         net_drops(void)        { return st_drop; }

void net_print_info(void) {
    if (!net_up) {
        printf("net: down (no NIC)\n");
        return;
    }
    const uint8_t* m = nic_mac();
    printf("ne0  HWaddr %02x:%02x:%02x:%02x:%02x:%02x\n",
           m[0], m[1], m[2], m[3], m[4], m[5]);
    /* NOTE: lwIP 2.1.3 ip4addr_ntoa uses ONE static buffer — three
     * calls inside a single printf would clobber each other (plus
     * GCC right-to-left argument evaluation). So: one printf per
     * address. */
    printf("     inet %s", ipaddr_ntoa(&nif.ip_addr));
    printf("  netmask %s", ipaddr_ntoa(&nif.netmask));
    printf("  gateway %s\n", ipaddr_ntoa(&nif.gw));
    printf("     RX %u  TX %u  drop %u  irq %u  ovw %u\n",
           st_rx, st_tx, st_drop,
           nic_irq_count(), nic_rx_overflow());
}

// ---------------- ICMP ping (raw API) ----------------------
static struct raw_pcb*     ping_pcb;
static volatile uint16_t   ping_my_id;
static volatile uint16_t   ping_seq_wait;
static volatile uint32_t   ping_recv_cnt;
static volatile uint32_t   ping_recv_rtt_ms;
static ip4_addr_t          ping_peer;

static u8_t ping_recv_cb(void* arg, struct raw_pcb* pcb,
                         struct pbuf* p, const ip_addr_t* addr) {
    (void)arg; (void)pcb;
    /* The RAW cb gets p->payload POINTING AT THE IP HEADER (lwIP
     * idiom: contrib/apps/ping uses payload + PBUF_IP_HLEN).
     * ip4_input already drops packets with IP options
     * (IP_OPTIONS_ALLOWED=0), so the header is always 20 bytes. */
    if (p->len >= 20 + (int)sizeof(struct icmp_echo_hdr)) {
        struct icmp_echo_hdr* ic =
            (struct icmp_echo_hdr*)((uint8_t*)p->payload + 20);
        if (ICMPH_TYPE(ic) == ICMP_ER &&
            lwip_ntohs(ic->id)  == ping_my_id &&
            lwip_ntohs(ic->seqno) == ping_seq_wait) {
            ping_recv_cnt++;
            ping_recv_rtt_ms = sys_now();   /* arrival time (ms) */
            ping_peer = *ip_2_ip4(addr);
            return 1;                    /* consumed               */
        }
    }
    return 0;
}

static void ping_send_once(const ip4_addr_t* dst, uint16_t seq) {
    struct pbuf* p = pbuf_alloc(PBUF_IP,
                                (u16_t)(sizeof(struct icmp_echo_hdr) + 16),
                                PBUF_RAM);
    if (p == NULL) return;

    struct icmp_echo_hdr* ic = (struct icmp_echo_hdr*)p->payload;
    ICMPH_TYPE_SET(ic, ICMP_ECHO);
    ICMPH_CODE_SET(ic, 0);
    ic->chksum = 0;
    ic->id     = lwip_htons(ping_my_id);
    ic->seqno    = lwip_htons(seq);
    /* payload pattern */
    uint8_t* pl = (uint8_t*)ic + sizeof(struct icmp_echo_hdr);
    for (int i = 0; i < 16; i++) pl[i] = (uint8_t)(0x61 + (i % 26));
    ic->chksum = inet_chksum(p->payload, p->len);

    ip_addr_t d;
    *(ip_2_ip4(&d)) = *dst;
    raw_sendto(ping_pcb, p, &d);
    pbuf_free(p);
}

static int ping_run(const char* ipstr, int count, int verbose) {
    if (!net_up) {
        if (verbose) printf("net: down (no NIC)\n");
        return 0;
    }
    while (*ipstr == ' ') ipstr++;
    if (*ipstr == 0) {
        if (verbose) printf("usage: ping <ip>\n");
        return 0;
    }
    if (count <= 0)  count = 4;
    if (count > 16)  count = 16;

    ip4_addr_t dst;
    char rip[16];                     /* resolve result (stable for display) */
    if (!ip4addr_aton(ipstr, &dst)) {
        /* v10.13: try resolving as a hostname (DNS 10.0.2.3) */
        if (verbose) printf("ping: resolving %s...\n", ipstr);
        if (!net_resolve(ipstr, rip)) {
            if (verbose) printf("ping: resolve failed: %s\n", ipstr);
            return 0;
        }
        ip4addr_aton(rip, &dst);
        ipstr = rip;              /* show the resolved IP */
    }
    if (verbose) printf("PING %s: 24 data bytes\n", ipstr);

    uint32_t f = net_lock();
    ping_pcb    = raw_new(IP_PROTO_ICMP);
    raw_bind(ping_pcb, &nif.ip_addr);
    raw_recv(ping_pcb, ping_recv_cb, NULL);
    ping_my_id  = (uint16_t)(get_tick() & 0xFFFF);
    ping_recv_cnt = 0;
    net_unlock(f);

    /* One uncounted warm-up round: triggers ARP resolution (packets
     * waiting on ARP are dropped because ARP_QUEUEING=0). */
    ping_seq_wait = 0;
    {
        uint32_t t0 = get_tick();
        ping_send_once(&dst, 0);
        while ((get_tick() - t0) < 50) { /* 500 ms */ }
    }

    int sent = 0, recv = 0;
    for (int seq = 1; seq <= count; seq++) {
        uint32_t before = ping_recv_cnt;
        ping_seq_wait   = (uint16_t)seq;
        uint32_t t0     = sys_now();    /* ms (10 ms resolution) */
        f = net_lock();
        ping_send_once(&dst, (uint16_t)seq);
        sent++;
        net_unlock(f);

        /* wait up to 1 s for the reply; IRQ0 processes packets */
        while ((sys_now() - t0) < 1000) { }
        uint32_t t_reply = ping_recv_rtt_ms;

        if (ping_recv_cnt > before) {
            recv++;
            if (verbose)
                printf("24 bytes from %s: icmp_seq=%d time=%u ms\n",
                       ipstr, seq,
                       (unsigned)((t_reply > t0) ? (t_reply - t0) : 0));
        } else if (verbose) {
            printf("Request timeout for icmp_seq %d\n", seq);
        }
        sleep_ms(200);                  /* gap between pings    */
    }

    f = net_lock();
    raw_remove(ping_pcb);
    ping_pcb = NULL;
    net_unlock(f);

    if (verbose) {
        int loss = (sent - recv) * 100 / sent;
        printf("--- %s ping statistics ---\n", ipstr);
        printf("%d packets transmitted, %d received, %d%% packet loss\n",
               sent, recv, loss);
        /* v10.13: dedicated 100%-loss hint — QEMU user-net (slirp)
         * on Windows hosts does NOT relay ICMP, so ping always
         * times out even on a healthy connection. Point the user
         * to tcpping instead. */
        if (recv == 0) {
            printf("all requests timed out: QEMU user-net (slirp) does not relay ICMP.\n");
            printf("try: tcpping <host> [port]   or   dns <hostname>\n");
        }
    }
    return recv;
}

int net_cmd_ping(const char* ipstr) {
    return ping_run(ipstr, 4, 1);
}

/* v10.12: quiet path for the SYS_NETPING #35 syscall (called from
 * ring-3 .mrp programs). */
int net_ping_raw(const char* ipstr, int count) {
    return ping_run(ipstr, count, 0);
}

// ============================================================
//  v10.13 — DNS resolver (hostname -> IP via 10.0.2.3)
// ------------------------------------------------------------
//  NO_SYS: the dns_gethostbyname callback runs from net_poll
//  (IRQ0 context) while the shell task busy-waits. A request
//  token prevents a stale callback (a late old reply) from
//  marking a new request as complete.
// ============================================================
static volatile uint32_t dns_token;    /* active request id */
static volatile int      dns_status;   /* 1 ok, -1 failed   */
static ip4_addr_t        dns_result;

static void dns_found_cb(const char* name, const ip_addr_t* ipaddr,
                         void* arg) {
    (void)name;
    if ((uint32_t)(uintptr_t)arg != dns_token) return;  /* stale */
    if (ipaddr != NULL) {
        dns_result = *ip_2_ip4(ipaddr);
        dns_status = 1;
    } else {
        dns_status = -1;                 /* NXDOMAIN / failure */
    }
}

int net_resolve(const char* host, char* out_ipstr) {
    if (!net_up || !host || !host[0]) return 0;

    ip4_addr_t a;
    uint32_t tok = ++dns_token;
    dns_status  = 0;

    uint32_t f = net_lock();
    err_t e = dns_gethostbyname(host, &a, dns_found_cb,
                                (void*)(uintptr_t)tok);
    net_unlock(f);

    if (e == ERR_OK) {                   /* input = dotted IP */
        const char* s = ipaddr_ntoa(&a);
        int i = 0;
        for (; s[i] && i < 15; i++) out_ipstr[i] = s[i];
        out_ipstr[i] = 0;
        return 1;
    }
    if (e != ERR_INPROGRESS) return 0;   /* ERR_ARG etc */

    /* wait for the callback; IRQ0 net_poll drives query + retransmit */
    uint32_t t0 = sys_now();
    while (dns_status == 0 && (sys_now() - t0) < 9000) { }

    if (dns_status == 1) {
        const char* s = ipaddr_ntoa(&dns_result);
        int i = 0;
        for (; s[i] && i < 15; i++) out_ipstr[i] = s[i];
        out_ipstr[i] = 0;
        return 1;
    }
    return 0;
}

int net_cmd_dns(const char* args) {
    if (!net_up) { printf("net: down (no NIC)\n"); return 0; }
    while (*args == ' ') args++;

    const ip_addr_t* srv = dns_getserver(0);
    if (!*args) {
        if (srv && srv->addr) printf("dns server: %s\n", ipaddr_ntoa(srv));
        printf("usage: dns <hostname>\n");
        return 0;
    }
    char host[64];
    int i = 0;
    for (; args[i] && args[i] != ' ' && i < 62; i++) host[i] = args[i];
    host[i] = 0;

    printf("dns: resolving %s...\n", host);
    char rip[16];
    if (net_resolve(host, rip)) {
        printf("dns: %s -> %s\n", host, rip);
        return 1;
    }
    printf("dns: %s did not resolve (NXDOMAIN / timeout)\n", host);
    return 0;
}

// ---------------- info 10-word (syscall #34 + httpd) -------
int net_dhcp_bound(void) { return dhcp_mode == 2; }

void net_get_info(uint32_t* w) {
    w[0] = net_up ? 1u : 0u;
    w[1] = (dhcp_mode == 2) ? 1u : 0u;
    /* IMPORTANT (phase-C lesson): on x86-LE, lwIP's ip_addr.addr
     * already has the LOW byte = FIRST octet (lwIP's ip4_addr1()
     * macro is also just & 0xFF — see ipaddr_ntoa). lwip_ntohl
     * would swap it back -> the httpd page once showed
     * "15.2.0.10". */
    w[2] = nif.ip_addr.addr;
    w[3] = nif.netmask.addr;
    w[4] = nif.gw.addr;
    const uint8_t* m = nic_mac();
    w[5] = (uint32_t)m[0] | ((uint32_t)m[1] << 8)
         | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
    w[6] = (uint32_t)m[4] | ((uint32_t)m[5] << 8);
    w[7] = st_rx;
    w[8] = st_tx;
    w[9] = st_drop;
}

// ============================================================
//  mget — HTTP client (v10.14; grew out of the v10.12 "wget")
// ------------------------------------------------------------
//  Blocking HTTP/1.0 GET from shell context: the pcb and its
//  callbacks are registered, then the task busy-waits while
//  IRQ0 (net_timer_tick -> net_poll) drives every TCP event
//  (connect / recv / close). Callbacks never touch the console;
//  the shell task prints the result after the transfer ends.
//
//  Syntax:  mget <url> [-port <n>]
//    url   : http://host[:port]/path    explicit scheme
//            https://host[:port]/path   TLS 1.2 (BearSSL, v0.1)
//            host[:port]/path           http:// assumed
//    -port : force the destination port, overriding the URL
//  Port priority: -port option > ":port" in the URL > scheme
//  default (80 http / 443 https).
//  HTTPS: strict chain validation against the embedded Mozilla
//  root subset first; if the chain cannot be verified (unknown
//  root / unsupported curve, e.g. github.com's P-384 Sectigo
//  chain) the transfer automatically retries in parse-only
//  mode with a clear warning (see tls_client.c).
//  The body is saved to the current directory under the URL's
//  basename, preserving the file format (.json, .png, ...).
//  Redirects (301/302/303/307/308) are followed, max 3 hops.
// ============================================================
#define MGET_MAX_BYTES     (384 * 1024)
#define MGET_TIMEOUT_MS    12000
#define MGET_MAX_REDIRECTS 3

struct mget_ctx {
    struct tcp_pcb* pcb;
    volatile int    state;       /* 0 running, 1 ok, -1 failed */
    volatile int    got_hdr;
    volatile int    http_ok;
    volatile int    connected;   /* SYN-ACK received            */
    volatile int    http_status; /* NNN status code             */
    volatile uint32_t len;
    uint32_t        hdr_end;
    uint32_t        cap;
    uint8_t*        buf;
    char            host[64];    /* original name for Host: hdr */
    char            path[128];
};

/* Scan the accumulated response bytes for the header end
 * (\r\n\r\n) and, once found, lift out the HTTP status code.
 * Shared by the plain-HTTP recv callback (IRQ context) and the
 * HTTPS task-side reader — pure memory access, no console
 * output, so it is safe from either context. */
static void mget_scan_hdrs(struct mget_ctx* c) {
    if (c->got_hdr) return;
    for (uint32_t i = 0; i + 3 < c->len; i++) {
        if (c->buf[i]     == 0x0D && c->buf[i + 1] == 0x0A &&
            c->buf[i + 2] == 0x0D && c->buf[i + 3] == 0x0A) {
            c->hdr_end = i + 4;
            c->got_hdr = 1;
            /* "HTTP/1.x NNN" — status in bytes [9..11] */
            if (c->len >= 12 &&
                c->buf[9]  >= '0' && c->buf[9]  <= '9' &&
                c->buf[10] >= '0' && c->buf[10] <= '9' &&
                c->buf[11] >= '0' && c->buf[11] <= '9') {
                c->http_status = (c->buf[9]  - '0') * 100
                               + (c->buf[10] - '0') * 10
                               + (c->buf[11] - '0');
                if (c->http_status == 200)
                    c->http_ok = 1;
            }
            return;
        }
    }
}

static err_t mget_recv_cb(void* arg, struct tcp_pcb* pcb,
                          struct pbuf* p, err_t err) {
    struct mget_ctx* c = (struct mget_ctx*)arg;
    if (p == NULL) {                       /* FIN from server = EOF */
        if (c->state == 0)
            c->state = (c->got_hdr && c->http_ok) ? 1 : -1;
        tcp_close(pcb);
        c->pcb = NULL;
        return ERR_OK;
    }
    if (err == ERR_OK) {
        uint32_t tot = p->tot_len;
        if (c->len + tot > c->cap) {
            c->state = -1;                 /* body exceeds buffer */
            pbuf_free(p);
            tcp_abort(pcb);
            c->pcb = NULL;
            return ERR_ABRT;
        }
        pbuf_copy_partial(p, c->buf + c->len, tot, 0);
        c->len += tot;

        mget_scan_hdrs(c);                 /* find header end + status */
        tcp_recved(pcb, (u16_t)tot);
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t mget_connected_cb(void* arg, struct tcp_pcb* pcb, err_t err) {
    struct mget_ctx* c = (struct mget_ctx*)arg;
    if (err != ERR_OK) { c->state = -1; return err; }
    c->connected = 1;                  /* handshake really completed */

    /* Safe size: 5+126+11 + 6+62+2 + 30 + 2 = 244 < 320. (v10.13
     * lesson: a 160-byte buffer could overflow in IRQ context.) */
    char req[320];
    int n = sprintf(req,
                    "GET %s HTTP/1.0\r\n"
                    "Host: %s\r\n"
                    "User-Agent: equinox-mget/0.1\r\n"
                    "\r\n", c->path, c->host);
    err_t e = tcp_write(pcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) { c->state = -1; return e; }
    tcp_output(pcb);
    return ERR_OK;
}

static void mget_err_cb(void* arg, err_t err) {
    (void)err;
    struct mget_ctx* c = (struct mget_ctx*)arg;
    c->state = -1;
    c->pcb   = NULL;
}

/* Case-insensitive header lookup inside the received header
 * block; fills out (max outmax-1 chars) with the trimmed value.
 * Returns 1 when found, 0 otherwise. Used for Location (redirect
 * following) and Content-Type (transfer report). */
static int mget_hdr_value(const uint8_t* hdr, uint32_t hlen,
                          const char* name, char* out, int outmax) {
    int nlen = 0;
    while (name[nlen]) nlen++;
    for (uint32_t i = 0; i + (uint32_t)nlen + 1 < hlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char a = (char)hdr[i + j];
            char b = name[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) { match = 0; break; }
        }
        if (match && hdr[i + nlen] == ':') {
            uint32_t v = i + (uint32_t)nlen + 1;
            while (v < hlen && (hdr[v] == ' ' || hdr[v] == '\t')) v++;
            int o = 0;
            while (v < hlen && hdr[v] != '\r' && hdr[v] != '\n'
                   && o < outmax - 1) {
                out[o++] = (char)hdr[v++];
            }
            out[o] = 0;
            return 1;
        }
    }
    return 0;
}

/* ---- HTTPS transfer over a BearSSL session ----
 * Runs entirely in task context (the engine never executes in
 * IRQ context): connect + handshake, send the request, pump
 * decrypted app data into c->buf until EOF / error. Fills the
 * same mget_ctx fields the callback path produces, so result
 * reporting, redirect following and saving stay shared. */
static int mget_https_transfer(struct mget_ctx* c, const char* host,
                               ip_addr_t* dst, int port) {
    struct tls_sess* tls = tls_connect_auto(host, dst, port);
    if (!tls) { c->state = -1; return 0; }
    c->connected = 1;

    char req[320];
    int n = sprintf(req,
                    "GET %s HTTP/1.0\r\n"
                    "Host: %s\r\n"
                    "User-Agent: equinox-mget/0.1\r\n"
                    "\r\n", c->path, c->host);
    if (tls_write_all(tls, req, n) != 0) {
        printf("mget: TLS write failed\n");
        tls_close(tls);
        c->state = -1;
        return 0;
    }

    for (;;) {
        if (c->len >= c->cap) {
            printf("mget: response too large (%u KB limit)\n",
                   (unsigned)(c->cap / 1024));
            c->state = -1;
            break;
        }
        int r = tls_read(tls, c->buf + c->len,
                         (int)(c->cap - c->len));
        if (r > 0) {
            c->len += (uint32_t)r;
            mget_scan_hdrs(c);
            continue;
        }
        if (r == 0 || r == -1) {
            /* clean close_notify, or abrupt FIN — both count as
             * complete when a 200 header arrived (mirrors the
             * HTTP path's "slow close still counts" rule) */
            c->state = (c->got_hdr && c->http_ok) ? 1 : -1;
            break;
        }
        if (r == -3)
            printf("mget: TLS transfer timed out\n");
        else
            printf("mget: TLS error during transfer (code %d)\n",
                   tls_last_error(tls));
        c->state = -1;
        break;
    }
    tls_close(tls);
    return c->state == 1;
}

int net_cmd_mget(const char* args, struct fs_node* save_dir) {
    if (!net_up) { printf("net: down (no NIC)\n"); return 0; }
    if (!save_dir) save_dir = fs_get_root();

    /* ---- tokenize: <url> [-port <n>] ---- */
    char url[256];
    int  url_len = 0;
    int  port_override = 0;                /* 0 = not given            */
    const char* s = args;
    while (*s == ' ') s++;
    while (*s) {
        char tok[200];
        int  tl = 0;
        while (*s && *s != ' ' && tl < 199) tok[tl++] = *s++;
        tok[tl] = 0;
        while (*s == ' ') s++;

        if (strcmp(tok, "-port") == 0) {
            char vt[8];
            int  vl = 0;
            while (*s && *s != ' ' && vl < 7) vt[vl++] = *s++;
            vt[vl] = 0;
            while (*s == ' ') s++;
            int digits = (vl > 0);
            for (int i = 0; i < vl; i++)
                if (vt[i] < '0' || vt[i] > '9') digits = 0;
            if (!digits) {
                printf("mget: -port expects a number (got '%s')\n", vt);
                return 0;
            }
            port_override = atoi(vt);
        } else if (url_len == 0) {
            for (int i = 0; tok[i] && url_len < 254; i++)
                url[url_len++] = tok[i];
            url[url_len] = 0;
        } else {
            printf("mget: unexpected argument '%s'\n", tok);
            return 0;
        }
    }
    if (url_len == 0) {
        printf("usage: mget <url> [-port <n>]\n");
        printf("  url   : http://host[:port]/path     plain HTTP\n");
        printf("          https://host[:port]/path    TLS 1.2 (BearSSL)\n");
        printf("          host[:port]/path            http:// assumed\n");
        printf("  -port : force the destination port (overrides the URL)\n");
        printf("  saves : <basename> into the current directory (any file type)\n");
        printf("examples:\n");
        printf("  mget http://10.0.2.2:8022/data.json\n");
        printf("  mget https://raw.githubusercontent.com/torvalds/linux/master/README\n");
        printf("  mget https://github.com/octocat/Hello-World -port 443\n");
        return 0;
    }
    if (port_override < 0 || port_override > 65535) {
        printf("mget: invalid port: %d\n", port_override);
        return 0;
    }

    uint8_t* buf = (uint8_t*)malloc(MGET_MAX_BYTES);
    if (!buf) {
        printf("mget: kernel heap exhausted (need %d KB)\n",
               MGET_MAX_BYTES / 1024);
        return 0;
    }

    char sbuf[70];                    /* final save name           */
    char host[64];                    /* current hop's host        */
    char loc[192];                    /* Location header value     */
    sbuf[0] = 0;

    for (int hop = 0; hop <= MGET_MAX_REDIRECTS; hop++) {
        /* ---- parse URL: [http://|https://]host[:port]/path ---- */
        const char* p = url;
        int is_tls = 0;
        if (memcmp(p, "https://", 8) == 0) { is_tls = 1; p += 8; }
        else if (memcmp(p, "http://", 7) == 0) p += 7;

        int hl = 0;
        while (*p && *p != '/' && *p != ':' && hl < 63) host[hl++] = *p++;
        host[hl] = 0;
        if (!hl) {
            printf("mget: no host in URL '%s'\n", url);
            free(buf);
            return 0;
        }

        int port = is_tls ? 443 : 80;  /* default by scheme       */
        if (*p == ':') {              /* ":port" inside the URL    */
            p++;
            port = 0;
            while (*p >= '0' && *p <= '9') {
                if (port < 65536) port = port * 10 + (*p - '0');
                p++;
            }
        }
        if (port_override) port = port_override;
        if (port <= 0 || port > 65535) {
            printf("mget: invalid port: %d\n", port);
            free(buf);
            return 0;
        }

        /* the request path must be absolute */
        char pbuf[130];
        if (*p == '/') {
            int i = 0;
            for (; p[i] && i < 128; i++) pbuf[i] = p[i];
            pbuf[i] = 0;
        } else {
            pbuf[0] = '/'; pbuf[1] = 0;
        }

        /* save name = basename of this hop's path (format kept) */
        {
            const char* b = pbuf;
            for (const char* q = pbuf; *q; q++) if (*q == '/') b = q + 1;
            if (!*b) b = "index.html";
            int i = 0;
            for (; b[i] && i < 68; i++) sbuf[i] = b[i];
            sbuf[i] = 0;
        }

        /* resolve: dotted IP goes direct, hostname via DNS.
         * NOTE: the Host: header keeps the ORIGINAL name — CDNs
         * (Akamai/Cloudflare) reject a resolved-IP Host value
         * (v10.13 lesson: 404 / 403 "error code: 1020"). */
        ip4_addr_t dst;
        char rip[16];
        const char* dstip = host;
        if (!ip4addr_aton(host, &dst)) {
            if (strcmp(host, "localhost") == 0) {
                printf("mget: note: 'localhost' is this Equinox OS machine;\n");
                printf("      a server on the QEMU host is at 10.0.2.2\n");
            }
            printf("mget: resolving %s...\n", host);
            if (!net_resolve(host, rip)) {
                printf("mget: resolve failed: %s\n", host);
                free(buf);
                return 0;
            }
            ip4addr_aton(rip, &dst);
            printf("mget: %s -> %s\n", host, rip);
            dstip = rip;              /* display the IP, Host keeps the name */
        }
        printf("mget: %s://%s:%d%s -> %s\n",
               is_tls ? "https" : "http", dstip, port, pbuf, sbuf);

        struct mget_ctx c;
        c.state = 0; c.got_hdr = 0; c.http_ok = 0; c.len = 0;
        c.connected = 0; c.http_status = 0;
        c.hdr_end = 0; c.cap = MGET_MAX_BYTES; c.buf = buf;
        c.pcb = NULL;
        {
            int i = 0;
            for (; host[i] && i < 62; i++) c.host[i] = host[i];
            c.host[i] = 0;
            for (i = 0; pbuf[i] && i < 126; i++) c.path[i] = pbuf[i];
            c.path[i] = 0;
        }

        uint32_t t0 = sys_now();
        int ok;
        ip_addr_t d;
        *(ip_2_ip4(&d)) = dst;

        if (is_tls) {
            /* ---- HTTPS: BearSSL session in task context ---- */
            ok = mget_https_transfer(&c, host, &d, port);
        } else {
            /* ---- plain HTTP: callback path (IRQ0 driven) ---- */
            uint32_t f = net_lock();
            struct tcp_pcb* pcb = tcp_new();
            if (!pcb) {
                net_unlock(f);
                free(buf);
                printf("mget: out of PCBs\n");
                return 0;
            }
            c.pcb = pcb;
            tcp_arg(pcb, &c);
            tcp_err(pcb, mget_err_cb);
            tcp_recv(pcb, mget_recv_cb);
            err_t e = tcp_connect(pcb, &d, (u16_t)port,
                                  mget_connected_cb);
            net_unlock(f);
            if (e != ERR_OK) {
                f = net_lock();
                tcp_close(pcb);
                net_unlock(f);
                free(buf);
                printf("mget: connect failed (%d)\n", (int)e);
                return 0;
            }

            while (c.state == 0 && (sys_now() - t0) < MGET_TIMEOUT_MS) { }

            /* a slow server close still counts if the data is complete */
            if (c.state == 1) ok = 1;
            else if (c.state == 0 && c.got_hdr && c.http_ok &&
                     c.len > c.hdr_end)
                ok = 1;
            else ok = 0;

            if (c.pcb) {
                f = net_lock();
                tcp_abort(c.pcb);
                c.pcb = NULL;
                net_unlock(f);
            }
        }

        /* report the status honestly whenever a header arrived */
        if (c.got_hdr)
            printf("mget: HTTP status %d\n", c.http_status);

        /* ---- follow redirects (301/302/303/307/308) ---- */
        if (ok && c.got_hdr &&
            (c.http_status == 301 || c.http_status == 302 ||
             c.http_status == 303 || c.http_status == 307 ||
             c.http_status == 308)) {
            if (!mget_hdr_value(buf, c.hdr_end, "Location",
                                loc, sizeof(loc)) || !loc[0]) {
                printf("mget: redirect without a Location header\n");
                free(buf);
                return 0;
            }
            if (hop == MGET_MAX_REDIRECTS) {
                printf("mget: too many redirects (max %d)\n",
                       MGET_MAX_REDIRECTS);
                free(buf);
                return 0;
            }
            char next[256];
            if (memcmp(loc, "http://", 7) == 0 ||
                memcmp(loc, "https://", 8) == 0) {
                snprintf(next, sizeof(next), "%s", loc);
            } else if (loc[0] == '/') {
                snprintf(next, sizeof(next), "%s%s%s",
                         is_tls ? "https://" : "http://", host, loc);
            } else {
                /* relative: replace the last path component */
                char dir[130];
                int last = -1;
                for (int i = 0; pbuf[i]; i++)
                    if (pbuf[i] == '/') last = i;
                int di = 0;
                for (int i = 0; i <= last && i < 127; i++)
                    dir[di++] = pbuf[i];
                dir[di] = 0;
                snprintf(next, sizeof(next), "%s%s%s%s",
                         is_tls ? "https://" : "http://",
                         host, dir, loc);
            }
            printf("mget: following redirect -> %s\n", next);
            snprintf(url, sizeof(url), "%s", next);
            continue;
        }

        if (!ok) {
            if (is_tls && !c.connected) {
                /* TCP or handshake failure — the TLS layer (task
                 * context) already printed the detailed reason. */
                printf("mget: TLS connection failed - see the TLS messages above\n");
            } else if (!c.connected) {
                printf("mget: connect failed - no response or refused (state %d).\n",
                       c.state);
                printf("      check the host and port; a firewall may reject it.\n");
            } else if (!c.got_hdr) {
                printf("mget: connected but the server sent no data (state %d).\n",
                       c.state);
            } else if (!c.http_ok) {
                printf("mget: server refused (HTTP %d) - file not saved\n",
                       c.http_status);
            } else {
                printf("mget: connection lost during transfer - %u bytes (state %d).\n",
                       c.len, c.state);
            }
            free(buf);
            return 0;
        }

        char ctype[48];
        if (mget_hdr_value(buf, c.hdr_end, "Content-Type",
                           ctype, sizeof(ctype)) && ctype[0])
            printf("mget: type %s\n", ctype);

        uint8_t* body = buf + c.hdr_end;
        uint32_t blen = c.len - c.hdr_end;
        if (fs_write_binary(save_dir, sbuf, body, blen) != 0) {
            printf("mget: save failed (RAMFS heap full?)\n");
            free(buf);
            return 0;
        }
        uint32_t ms = sys_now() - t0;
        if (!ms) ms = 1;
        printf("mget: %s saved (HTTP %d, %u bytes",
               sbuf, c.http_status ? c.http_status : 200, blen);
        if (ms > 500) printf(", %u KB/s", (unsigned)(blen / ms));
        printf(")\n");
        free(buf);
        return 1;
    }

    printf("mget: too many redirects (max %d)\n", MGET_MAX_REDIRECTS);
    free(buf);
    return 0;
}

// ============================================================
//  v10.13 — tcpping: TCP connect probe
// ------------------------------------------------------------
//  A TCP flavor of "ping": open a connection + measure the SYN ->
//  SYN/ACK -> ACK handshake RTT. Useful under QEMU on Windows:
//  user-net (slirp) blocks ICMP but TCP/NAT works normally.
//  Blocking from shell context — same pattern as mget (callbacks
//  run in IRQ0, the task busy-waits, no printf on the IRQ path).
// ============================================================
#define TPING_TIMEOUT_MS 6000

struct tping_ctx {
    volatile int       state;     /* 0 running, 1 open, -1 err, 2 FIN */
    struct tcp_pcb* volatile pcb;
    uint32_t           t0;
    volatile uint32_t  rtt_ms;
};

static err_t tping_connected_cb(void* arg, struct tcp_pcb* pcb,
                                err_t err) {
    struct tping_ctx* c = (struct tping_ctx*)arg;
    if (err != ERR_OK) { c->state = -1; return err; }
    c->rtt_ms = sys_now() - c->t0;
    c->state  = 1;
    return ERR_OK;
}

static err_t tping_recv_cb(void* arg, struct tcp_pcb* pcb,
                           struct pbuf* p, err_t err) {
    struct tping_ctx* c = (struct tping_ctx*)arg;
    if (p == NULL) {                     /* FIN = port open     */
        if (c->state == 0) c->state = 2;
        return ERR_OK;
    }
    if (err == ERR_OK) tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void tping_err_cb(void* arg, err_t err) {
    (void)err;
    struct tping_ctx* c = (struct tping_ctx*)arg;
    c->state = -1;
    c->pcb   = NULL;                     /* stack already purged */
}

int net_cmd_tcpping(const char* args) {
    if (!net_up) { printf("net: down (no NIC)\n"); return 0; }

    /* parse: <host> [port] */
    char tok[2][64];
    int  ntok = 0;
    const char* s = args;
    while (*s == ' ') s++;
    while (*s && ntok < 2) {
        int tl = 0;
        while (*s && *s != ' ' && tl < 62) tok[ntok][tl++] = *s++;
        tok[ntok][tl] = 0;
        ntok++;
        while (*s == ' ') s++;
    }
    if (ntok == 0) {
        printf("usage: tcpping <host> [port]\n");
        printf("  TCP-based alternative to ping (works under QEMU user-net)\n");
        return 0;
    }
    const char* host = tok[0];
    int port = 80;
    if (ntok == 2) {
        int digits = 1;
        for (const char* q = tok[1]; *q; q++)
            if (*q < '0' || *q > '9') digits = 0;
        if (digits) port = atoi(tok[1]);
    }
    if (port <= 0 || port > 65535) {
        printf("tcpping: invalid port: %d\n", port);
        return 0;
    }

    /* resolve: dotted IP goes direct, hostname via DNS */
    ip4_addr_t dst;
    char rip[16];
    if (!ip4addr_aton(host, &dst)) {
        printf("tcpping: resolving %s...\n", host);
        if (!net_resolve(host, rip)) {
            printf("tcpping: resolve failed: %s\n", host);
            return 0;
        }
        ip4addr_aton(rip, &dst);
        printf("tcpping: %s -> %s\n", host, rip);
    }

    struct tping_ctx c;
    c.state = 0; c.rtt_ms = 0;

    uint32_t f = net_lock();
    struct tcp_pcb* pcb = tcp_new();
    if (!pcb) {
        net_unlock(f);
        printf("tcpping: out of PCBs\n");
        return 0;
    }
    c.pcb = pcb;
    tcp_arg(pcb, &c);
    tcp_err(pcb, tping_err_cb);
    tcp_recv(pcb, tping_recv_cb);
    c.t0 = sys_now();
    ip_addr_t d;
    *(ip_2_ip4(&d)) = dst;
    err_t e = tcp_connect(pcb, &d, (u16_t)port, tping_connected_cb);
    net_unlock(f);
    if (e != ERR_OK) {
        f = net_lock();
        tcp_close(pcb);
        net_unlock(f);
        printf("tcpping: connect failed (%d)\n", (int)e);
        return 0;
    }

    printf("tcpping: connect %s port %d ...\n", host, port);
    uint32_t t0 = sys_now();
    while (c.state == 0 && (sys_now() - t0) < TPING_TIMEOUT_MS) { }

    int ok = 0;
    if (c.state == 1) {
        printf("tcpping: %s:%d OPEN (handshake %u ms)\n",
               host, port, c.rtt_ms);
        ok = 1;
    } else if (c.state == 2) {
        printf("tcpping: %s:%d open, then closed by peer\n", host, port);
        ok = 1;
    } else if (c.state == -1) {
        printf("tcpping: %s:%d REFUSED / reset (port closed?)\n",
               host, port);
    } else {
        printf("tcpping: %s:%d TIMEOUT (no answer for %d ms)\n",
               host, port, TPING_TIMEOUT_MS);
    }

    /* clean close — err_cb already freed the pcb (c.pcb == NULL).
     * Under net_lock no IRQ can change the state. */
    f = net_lock();
    if (c.pcb) {
        tcp_recv(c.pcb, NULL);
        if (tcp_close(c.pcb) != ERR_OK)
            tcp_abort(c.pcb);
        c.pcb = NULL;
    }
    net_unlock(f);
    return ok;
}
