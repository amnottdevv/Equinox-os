// ============================================================
//  lwipopts.h — lwIP 2.1.3 configuration for Equinox OS
// ------------------------------------------------------------
//  Mode: NO_SYS=1 (cooperative polling, no threads).
//  Memory: lwIP's internal heap (mem.c, static BSS arrays) —
//  does NOT use the kernel heap, so it cannot collide with the
//  kernel malloc / the user arena.
//
//  v10.12: DHCP ON + pools raised for the :80 httpd
//  + mget (several TCP connections + 4 MB WAD file transfers
//  per-chunk via sndbuf ).
// ============================================================
#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* ---------- platform / mode ---------- */
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0    /* our own cli/sti guard */
#define LWIP_PROVIDE_ERRNO          0

/* ---------- protocols enabled ---------- */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1    /* ping via raw pcb      */
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_ETHERNET               1    /* netif/ethernet.c      */

/* ---------- disabled ---------- */
#define LWIP_DHCP                   1    /* v10.12: slirp dhcpd */
#define DHCP_DOES_ARP_CHECK         0    /* fast bind (link-local) */
#define LWIP_AUTOIP                 0
/* v10.13: DNS ON — hostname resolution via server 10.0.2.3 (slirp
 * forwards the query to the host resolver; UDP works under
 * Windows user-net even though ICMP is blocked). LWIP_DNS_SECURE=0
 * -> txid counter, no LWIP_RAND. dns_tmr runs automatically via
 * sys_check_timeouts (timeouts.c). */
#define LWIP_DNS                    1
#define LWIP_DNS_SECURE             0    /* no LWIP_RAND needed (txid counter) */
#define DNS_MAX_NAME_LENGTH         64
#define DNS_MAX_SERVERS             2
#define DNS_TABLE_SIZE              4
#define DNS_MAX_RETRIES             4
#define LWIP_IGMP                   0
#define LWIP_SNMP                   0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_NETIF_API              0
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0
#define LWIP_LOOPIF                 0
#define LWIP_MULTICAST_PING         0
#define LWIP_BROADCAST_PING         0

/* ---------- heap internal (BSS, bukan malloc kernel) -------- */
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (96 * 1024)   /* v10.12: +TCP buffers */
#define MEMP_OVERFLOW_CHECK         0
#define LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT 0

/* ---------- pool ---------- */
#define MEMP_NUM_PBUF               16    /* ref/ROM pbuf       */
#define MEMP_NUM_RAW_PCB            2
#define MEMP_NUM_UDP_PCB            4     /* dhcp + spare       */
#define MEMP_NUM_TCP_PCB            8     /* listen + httpd conns */
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            64
#define PBUF_POOL_SIZE              24
/* full 1514-byte frame + alignment */
#define PBUF_POOL_BUFSIZE           LWIP_MEM_ALIGN_SIZE(1536)

/* ---------- TCP tuning (small = memory friendly) ---------- */
#define TCP_MSS                     536
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF / TCP_MSS)) + 4)
#define TCP_QUEUE_OOSEQ             1
#define TCP_SNDLOWAT                (4 * TCP_MSS)
#define LWIP_TCP_SACK_OUT           0
#define LWIP_WND_SCALE              0
#define LWIP_TCP_TIMESTAMPS         0

/* ---------- ARP ---------- */
#define ARP_TABLE_SIZE              8
#define ARP_QUEUEING                0     /* drop while unresolved */
#define ETHARP_SUPPORT_VLAN         0
#define ETH_PAD_SIZE                0

/* ---------- checksums: fully in software (NE2000 has no offload) - */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1

/* ---------- statistik ringan ---------- */
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          0
#define MEM_STATS                   1
#define MEMP_STATS                  1
#define LINK_STATS                  1
#define IP_STATS                    0
#define ICMP_STATS                  1
#define UDP_STATS                   0
#define TCP_STATS                   0

/* ---------- misc ---------- */
/* checksums: lwIP's portable default algorithm (default 1) */
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 0
#define DEFAULT_THREAD_STACKSIZE    0    /* no threads           */
#define DEFAULT_THREAD_PRIO         0
#define DEFAULT_RAW_RECVMBOX_SIZE   0
#define DEFAULT_UDP_RECVMBOX_SIZE   0
#define DEFAULT_TCP_RECVMBOX_SIZE   0
#define DEFAULT_ACCEPTMBOX_SIZE     0

#endif /* LWIP_LWIPOPTS_H */
