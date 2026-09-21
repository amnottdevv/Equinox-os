// ============================================================
//  tls_client.h — internal TLS 1.2 client API (BearSSL)
// ------------------------------------------------------------
//  Used by net_lwip.c (mget https:// support). The session is
//  opaque here; all implementation lives in tls_client.c.
//
//  Threading model (mirrors the rest of the net stack):
//    * lwIP callbacks run in IRQ0/IRQ9 context (net_poll);
//      they only memcpy raw TCP bytes into a ring buffer.
//    * The BearSSL engine (handshake, crypto, records) is
//      driven from the calling shell task under net_lock().
//
//  Verification policy (v0.3 FR-23 — FAIL-CLOSED):
//    1. strict attempt — chain checked against the 9 root CAs
//       embedded in third_party/bearssl/anchors/equinox_anchors.c
//    2. a certificate failure (unknown root / unsupported key /
//       expired) REFUSES the connection by default.
//    3. the parse-only retry ("encrypted but NOT verified") happens
//       ONLY when the user explicitly opted in with
//       tls_set_insecure(1) — e.g. `mget -k`. The leaf public key
//       is still extracted and every record authenticated +
//       encrypted, but the chain is not validated; a clear warning
//       is printed and tls_session_verified() reports 0.
// ============================================================
#ifndef EQUINOX_TLS_CLIENT_H
#define EQUINOX_TLS_CLIENT_H

#include <stdint.h>

struct tls_sess;                    /* opaque session handle */

#ifdef __cplusplus
extern "C" {
#endif

// (The embedded root CA anchor table lives in
// third_party/bearssl/anchors/equinox_anchors.c and is declared
// inside tls_client.c with the real BearSSL types.)

// Connect + full TLS 1.2 handshake.
//   host    : hostname (used for SNI and name checks)
//   dst     : resolved lwIP ip_addr_t (IPv4)
//   port    : TCP port (usually 443)
// Prints progress / warnings itself ("TLS: ...").
// return: session with handshake complete, or NULL (message
// already printed). Strict-then-fallback happens inside.
struct tls_sess* tls_connect_auto(const char* host,
                                  const void* dst, int port);

// v0.3 FR-23: opt-in for the "encrypted but not verified" retry.
// 0 (DEFAULT) = verification failure refuses the connection.
// 1 = allow the warned parse-only fallback (user asked for -k).
// Returns the previous value. Reset to 0 after each connection.
int  tls_set_insecure(int allow);

// True when the session's chain was verified against the
// embedded root CAs (0 in fallback mode).
int  tls_session_verified(const struct tls_sess* s);

// Send application data (one shot, full buffer). return 0 ok.
int  tls_write_all(struct tls_sess* s, const void* buf, int len);

// Receive application data (blocking pump with timeouts).
// return: >0 bytes copied (up to maxlen),
//          0 clean EOF (server close_notify),
//         -1 abrupt EOF (server FIN without close_notify; data
//            received so far stays valid),
//         -2 TLS error (tls_last_error() has the code),
//         -3 timeout (no progress).
int  tls_read(struct tls_sess* s, uint8_t* out, int maxlen);

// Last BearSSL error of a failed/aborted session (0 = clean).
// Custom negative codes: -1 abrupt EOF, -2 net error,
// -3 ring overflow, -4 timeout.
int  tls_last_error(const struct tls_sess* s);

// Send close_notify (best effort), abort the pcb, free memory.
void tls_close(struct tls_sess* s);

#ifdef __cplusplus
}
#endif

#endif // EQUINOX_TLS_CLIENT_H
