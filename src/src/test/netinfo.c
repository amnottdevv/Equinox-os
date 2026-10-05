/* netinfo.c — ring-3 network demo (v10.12 Phase C).
 * Run in Equinox OS:  mtcc /test/netinfo.c
 * Uses ONLY mtcc-supported constructs (no preprocessor, no static,
 * no typedef): built-ins net_info (10-word status, see syscall.h)
 * + net_ping (blocking ICMP echo) — proof that the network is
 * reachable from a ring-3 userland program via int 0x80.
 */

/* print a signed int (printint() itself is unsigned-only) */
void prn(int v) {
    if (v < 0) {
        print("-");
        v = -v;
    }
    printint(v);
}

/* print an IPv4 from one host-order word (first octet = low byte) */
void pr_ip(int w) {
    printint(w & 255);        print(".");
    printint((w >> 8) & 255); print(".");
    printint((w >> 16) & 255); print(".");
    printint((w >> 24) & 255);
}

int main() {
    int w[10];
    int i;

    for (i = 0; i < 10; i++) w[i] = 0;

    if (net_info(w) != 0) {
        print("netinfo: syscall error\n");
        return 1;
    }
    if (w[0] == 0) {
        print("netinfo: net down (no NIC)\n");
        return 1;
    }

    print("== Equinox OS netinfo (ring 3) ==\n");
    print("dhcp    : "); printint(w[1]); print("\n");
    print("ip      : "); pr_ip(w[2]); print("\n");
    print("netmask : "); pr_ip(w[3]); print("\n");
    print("gateway : "); pr_ip(w[4]); print("\n");
    print("mac     : ");
    printint(w[5] & 255);         print(":");
    printint((w[5] >> 8) & 255);  print(":");
    printint((w[5] >> 16) & 255); print(":");
    printint((w[5] >> 24) & 255); print(":");
    printint(w[6] & 255);         print(":");
    printint((w[6] >> 8) & 255);
    print("\n");
    print("rx/tx   : "); printint(w[7]); print("/");
    printint(w[8]); print("  drop "); printint(w[9]); print("\n");

    print("ping 10.0.2.2 (blocking ~5 s)...\n");
    i = net_ping("10.0.2.2");
    print("replies : "); printint(i); print("/4\n");

    return 0;
}
