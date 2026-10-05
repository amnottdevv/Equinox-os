#ifndef EGG_SHA256_H
#define EGG_SHA256_H

/* ============================================================
 *  egg_sha256.h — SHA-256 murni C untuk eggkg (0.4 Beta)
 * ------------------------------------------------------------
 *  Dipakai eggkg untuk verifikasi integritas paket (field
 *  sha256 di index.idx). Implementasi FIPS 180-4 tanpa
 *  dependensi: tanpa malloc, tanpa struct publik, buffer
 *  statis 64-byte di dalam fungsi.
 *
 *  Verifikasi rencana uji (FIPS 180-4 appendix):
 *    egg_sha256_hex("abc", 3, out) ->
 *      ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
 * ============================================================ */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hitung digest 32-byte dari len byte data. */
void egg_sha256(const uint8_t* data, uint32_t len, uint8_t out[32]);

/* Sama di atas, tapi hasilnya 65-byte hex NUL-terminated (lowercase). */
void egg_sha256_hex(const uint8_t* data, uint32_t len, char out[65]);

/* Bandingkan dua string hex (case-insensitive). 1 = sama, 0 = beda. */
int  egg_sha_eq(const char* a, const char* b);

#ifdef __cplusplus
}
#endif

#endif /* EGG_SHA256_H */
