#ifndef ECF_H
#define ECF_H

/* ============================================================
 *  ecf.h — Equinox Config File (.ecf) — sistem konfigurasi 0.4 Beta
 * ------------------------------------------------------------
 *  Format: INI-ringan, menggantikan boot/nic.cfg TOTAL (tanpa
 *  fallback ke format lama):
 *
 *      # komentar baris penuh
 *      [net]                 -> key berikutnya jadi "net.key"
 *      driver = e1000        -> "net.driver = e1000"
 *      net.driver = ne2000   -> key berisi '.' dipakai apa adanya
 *
 *  Batas: baris 160 B, key 63, value 127, file 8 KB, 64 entri.
 *  Parser statis tanpa malloc.
 *
 *  Kebijakan:
 *      key tak dikenal   -> hitung warning + skip
 *      nilai invalid     -> error + skip
 *      duplikat key      -> last-wins
 *
 *  Script / autoexec TIDAK dicampur di .ecf — berkas ini murni
 *  "key = value" untuk keputusan boot.
 * ============================================================ */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECF_MAX_ENTRIES 64      /* entri per store                     */
#define ECF_KEY_MAX     64      /* 63 karakter + NUL                   */
#define ECF_VAL_MAX     128     /* 127 karakter + NUL                   */
#define ECF_LINE_MAX    160     /* panjang baris maksimum              */
#define ECF_FILE_MAX    8192    /* ukuran berkas maksimum (8 KB)       */
#define ECF_PATH_MAX    64      /* ecf_active_path() buffer            */

struct ecf_entry {
    char     key[ECF_KEY_MAX];
    char     val[ECF_VAL_MAX];
    uint8_t  used;              /* 0 = slot kosong                     */
};

struct ecf_store {
    struct ecf_entry ent[ECF_MAX_ENTRIES];
    int count;                  /* entri terisi                        */
    int warn;                   /* key tak dikenal pada parse terakhir */
    int err;                    /* nilai/berkas invalid pada parse     */
    /* 0.4 Beta: baris dengan nilai placeholder "NONE" dilewati (bukan
     * error) — dipakai `set -d FILE -base SRC` yang menyalin SEMUA key
     * milik base dengan value NONE supaya user mengisinya sendiri. */
    int none;                   /* key masih "NONE" (belum diisi)      */
};

/* Parse berkas .ecf (tanpa malloc). 0 = ok, -1 = file > 8 KB.
 * Store di-reset dulu; hasil parse ada di st->count/warn/err. */
int ecf_parse(const char* buf, uint32_t len, struct ecf_store* st);

/* Nilai key, NULL bila tak ada. */
const char* ecf_get(const struct ecf_store* st, const char* key);

/* Simpan/simpan-ulang satu entri (last-wins). 0 = ok, <0 = penuh. */
int ecf_put(struct ecf_store* st, const char* key, const char* val);

/* Baca + parse berkas dari path absolut. 0 = ok, <0 = gagal/tak ada. */
int ecf_load(const char* path, struct ecf_store* st);

/* 0.4 Beta: parse MENAMBAH ke store (tanpa reset) — overlay active.conf. */
int ecf_parse_merge(const char* buf, uint32_t len, struct ecf_store* st);
int ecf_load_merge(const char* path, struct ecf_store* st);

/* Path berkas konfigurasi aktif (buffer statis, valid sampai panggilan
 * berikutnya). for_write = 0 (baca) / 1 (tulis, boleh membuat dir).
 * NULL = tak ada berkas (baca) / tak bisa menulis. */
const char* ecf_active_path(int for_write);

/* Validasi: 0 valid, 1 key tak dikenal, 2 nilai invalid.
 * val == NULL -> cek key saja. */
int ecf_check(const char* key, const char* val);

/* Daftar nilai yang dikenal untuk pesan error ("ne2000|e1000|none"),
 * NULL bila key tak dikenal. */
const char* ecf_key_values(const char* key);

/* 1 bila nilai key ini hanya dibaca saat boot (perlu reboot). */
int ecf_needs_reboot(const char* key);

/* Patch IN-PLACE: baca berkas -> ganti bagian value baris terAKHIR
 * yang full-key-nya sama (komentar/format lain dipertahankan); bila
 * tak ada baris, append "full.key = value\n". 0 = ok, <0 = gagal. */
int ecf_set_file(const char* path, const char* key, const char* val);

/* Tulis SELURUH store sebagai berkas .ecf. 0 = ok, <0 = gagal. */
int ecf_write_store(const char* path, struct ecf_store const* st);

/* Store GLOBAL (file-scope static), lazy-load dari
 * ecf_active_path(0) pada panggilan pertama. */
struct ecf_store* ecf_store(void);

/* ============================================================
 *  0.4 Beta — target override (`set -d FILE [-path DIR]`)
 * ------------------------------------------------------------
 *  `set -d` menunjuk sebuah berkas .ecf LAIN sebagai "konf utama":
 *  set KEY VALUE / set -a / set -w semuanya menulis ke sana.
 *  Override hanya berlaku di sesi berjalan (RAM) — TIDAK
 *  persisten. Yang persisten adalah kunci bebas `active.conf`
 *  yang ditulis `set -d` ke system.ecf, supaya pembaca saat boot
 *  ikut meng-overlay berkas itu.
 * ============================================================ */

/* Path target aktif, atau NULL bila belum pernah `set -d`. */
const char* ecf_target(void);

/* Tunjuk `path` sebagai target SEMUA operasi tulis+baca berkas
 * .ecf pada sesi ini, dan paksa store GLOBAL di-muat ulang dari
 * path itu. 0 = ok, <0 = path bukan absolute / terlalu panjang. */
int ecf_target_set(const char* path);

/* Kembali ke perilaku lama (ecf_active_path) + reload store. */
void ecf_target_clear(void);

/* Tandai store GLOBAL belum dimuat (dipanggil setelah path berubah). */
void ecf_store_invalidate(void);

/* ============================================================
 *  0.4 Beta — pointer "konf utama" untuk pembacaan saat BOOT
 * ------------------------------------------------------------
 *  Kunci bebas `active.conf = <nama berkas>` di system.ecf
 *  menyatakan berkas .ecf mana yang harus di-overlay di atas
 *  system.ecf saat boot. Dengan itu `drivers.ecf` yang ditunjuk
 *  `set -d drivers.ecf` ikut dibaca oleh init (net driver) —
 *  "init berfungsi sesuai conf".
 * ============================================================ */
#define ECF_ACTIVE_KEY "active.conf"

/* 0.4 Beta — baca SATU key dari sebuah berkas .ecf tanpa menyentuh store
 * global (dipakai fat32_boot_init untuk `base.path`). Mengikuti pointer
 * `active.conf` bila key tidak ada di berkas utama. Buffer statis. */
const char* ecf_file_get(const char* path, const char* key);

#ifdef __cplusplus
}
#endif

#endif
