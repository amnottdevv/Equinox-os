#ifndef ECF_CALLER_H
#define ECF_CALLER_H

/* ============================================================
 *  ecf_caller.h — registry aksi bernama (dispatch layer)
 * ------------------------------------------------------------
 *  Versi string-keyed dari pola syscall_table[]: alih-alih nomor,
 *  kuncinya NAMA bertitik ("<namespace>.<action>[_kualifier]", mis.
 *  "mtcc.compile_ruf_eggkg", "set.path_local"). Dipicu dari config
 *  (.ecf) dan dari mana pun — eggkg, shell, boot, tool future —
 *  lewat SATU fungsi dispatch ecf_call().
 *
 *  Properti:
 *    - Registry TERBUKA: modul mana pun memanggil
 *      ecf_call_register() saat init. Menambah capability = tulis
 *      handler + daftarkan, TANPA menyentuh inti dispatch.
 *    - Ring-0 saja (library). Titik masuk interaktif = shell `call`.
 *    - Tanpa heap, array statis (pola ecf.c).
 *
 *  Dipakai eggkg: tiap baris step "<n> = <nama> <args...>" di
 *  eggkg.ecf di-resolve lalu ecf_call(nama, &ctx). Interpreter step
 *  = loop tipis di atas ecf_call; validasi-dulu-lalu-jalan (atomic).
 * ============================================================ */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECF_CALL_MAX       48     /* kapasitas registry                 */
#define ECF_CALL_NAME_MAX  48     /* "mtcc.compile_ruf_eggkg" + NUL     */
#define ECF_CALL_ARGC_MAX  8      /* argumen per panggilan              */

/* Konteks seragam yang diterima tiap handler. argv[] berisi argumen
 * yang SUDAH di-resolve ($var -> nilai) oleh pemanggil. */
struct ecf_call_ctx {
    int         argc;
    const char* argv[ECF_CALL_ARGC_MAX];
    const char* caller;             /* tool pemanggil, mis "eggkg"/"shell" */
};

typedef int (*ecf_call_fn)(const struct ecf_call_ctx* ctx);

struct ecf_call_ent {
    char        name[ECF_CALL_NAME_MAX];
    ecf_call_fn fn;
    uint8_t     used;               /* 0 = slot kosong                    */
};

/* Daftarkan satu handler bernama. return 0 = ok, -1 = nama invalid /
 * duplikat / registry penuh (pesan dicetak bila verbose). */
int ecf_call_register(const char* name, ecf_call_fn fn);

/* Dispatch: lookup nama persis -> panggil handler -> return kode
 * handler. Nama tak dikenal -> -1 (+ pesan bila ec verbose). */
int ecf_call(const char* name, const struct ecf_call_ctx* ctx);

/* 1 bila nama terdaftar, 0 bila tidak. */
int ecf_call_exists(const char* name);

/* Introspeksi (untuk `config callers` + validasi config). */
int         ecf_call_count(void);
const char* ecf_call_name_at(int i);

/* Kosongkan registry (hanya untuk tes / re-init). */
void ecf_call_reset(void);

/* Nama valid? bentuk "<ns>.<action>[_qual]", tanpa spasi, ada '.',
 * huruf/angka/'_'/'.'/'-' saja. 1 = valid. */
int ecf_call_name_valid(const char* name);

#ifdef __cplusplus
}
#endif

#endif /* ECF_CALLER_H */
