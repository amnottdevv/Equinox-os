// ============================================================
//  net.h — Equinox OS TCP/IP stack (v10.11+)
// ------------------------------------------------------------
//  Stack: lwIP 2.1.3 (BSD-3, third_party/lwip-2.1.3) NO_SYS=1
//  NIC  : NE2000 ISA (QEMU -device ne2k_isa) @ 0x300, IRQ9
//  Model: cooperative polling — net_poll() is called from the
//         timer IRQ0 (net_timer_tick) and the IRQ9 NIC ISR.
//         Every lwIP access is guarded by cli/sti (net_lock /
//         net_unlock), so it is safe from both contexts at once.
//
//  Used by: kernel.cpp (init + shell commands ifconfig / ping),
//  timer.cpp (net_timer_tick), idt.cpp (ne2000_isr).
// ============================================================
#ifndef EQUINOX_NET_H
#define EQUINOX_NET_H

#include <stdint.h>

struct fs_node;   /* forward — fs_ram.h (mget saves to shell cwd) */

#ifdef __cplusplus
extern "C" {
#endif

// Call once in kernel_main after the display is up.
// return: 1 = NIC found + stack running, 0 = no NIC
//         (network commands become a polite no-op).
int  net_init(void);

// Drain NIC RX -> pbuf -> lwIP, then sys_check_timeouts().
// Re-entrancy safe (internal cli-guard). May be called from any
// context: a task (shell), the IRQ0 timer, or IRQ9 (NIC).
void net_poll(void);

// Called from timer_handler (IRQ0) — only polls when up.
void net_timer_tick(void);

// Shell: print MAC/IP/counters.
void net_print_info(void);

// Shell: ping <ip> — 4x ICMP echo, one per second, print RTT.
// return: number of replies received (0..4).
int  net_cmd_ping(const char* ipstr);

// Fast status access (for info/sys).
int         net_is_up(void);
const uint8_t* net_mac(void);       // 6 bytes
uint32_t    net_rx_packets(void);
uint32_t    net_tx_packets(void);
uint32_t    net_drops(void);

// ============================================================
//  v10.12 — DHCP + TCP apps + ring-3 syscalls
// ============================================================

// IRQ guard for any lwIP context — used by net_lwip.c, httpd.c
// and the syscall layer (IRQ0/IRQ9 vs shell task re-entrancy).
static inline uint32_t net_lock(void) {
    uint32_t f;
    asm volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void net_unlock(uint32_t f) {
    if (f & 0x200) asm volatile("sti" ::: "memory");
}

// 1 = IP active from DHCP (not the static fallback).
int  net_dhcp_bound(void);

// 10-word ABI for the SYS_NETINFO #34 syscall + the httpd status
// page (exact layout — see the comments in syscall.h):
//   w[0]=up w[1]=dhcp w[2]=ip w[3]=netmask w[4]=gw (host order)
//   w[5]=mac_lo w[6]=mac_hi w[7]=rx w[8]=tx w[9]=drop
void net_get_info(uint32_t* w);

// Non-verbose ping (syscall #35): return reply count 0..count.
int  net_ping_raw(const char* ipstr, int count);

// ---- httpd :80 (kernel/net/httpd.c) ----
// start = idempotent; return 1 success/already, 0 failed (no pcb).
int      net_httpd_start(void);
int      net_httpd_running(void);
uint32_t net_httpd_hits(void);
uint32_t net_httpd_bytes(void);

// ---- mget — HTTP(S) client (blocking, shell context) ----
// v10.14 (was "wget"): URL-based downloads.
//   args: "<url> [-port <n>]"
//     url : http://host[:port]/path | https://host[:port]/path
//           host[:port]/path (http:// assumed)
//   -port overrides the port from the URL; defaults are 80
//   (http) and 443 (https).
// HTTPS rides on BearSSL 0.6 (third_party/bearssl, TLS 1.2):
// strict chain validation against 9 embedded Mozilla roots,
// with an automatic parse-only retry (clearly warned) when the
// server's chain cannot be verified (unknown root / P-384).
// The response body is saved via fs_write_binary into save_dir
// (the shell cwd) under the URL's basename — the file format
// (.json, .png, ...) is preserved byte-exact.
// Redirects (301/302/303/307/308) are followed, max 3 hops,
// across schemes and hosts.
// return 1 success (file saved), 0 failure.
int  net_cmd_mget(const char* args, struct fs_node* save_dir);

// ============================================================
//  v10.13 — DNS + testing tools that work under QEMU Windows
// ============================================================

// Resolve "<host>" -> dotted IP into out_ipstr (min 16 bytes).
// The host may be a dotted IP (used directly, no query) or a
// hostname (DNS query to 10.0.2.3 — slirp forwards it to the
// host resolver). return 1 success, 0 failure (net down / bad
// name / NXDOMAIN / timeout).
int  net_resolve(const char* host, char* out_ipstr);

// Shell: dns <hostname> — show the resolve result + DNS server.
int  net_cmd_dns(const char* args);

// Shell: tcpping <host> [port] — TCP connect probe + RTT in ms.
// A replacement for ping under QEMU on Windows (user-net blocks
// ICMP, but TCP/NAT works). Default port 80.
int  net_cmd_tcpping(const char* args);

#ifdef __cplusplus
}
#endif

#endif // EQUINOX_NET_H
