#ifndef EGGKG_H
#define EGGKG_H

/* ============================================================
 *  eggkg.h — package manager Equinox OS (0.4 Beta)
 * ------------------------------------------------------------
 *  eggkg adalah BUILTIN shell (ring 0) — bukan .mrp. Alasan
 *  jujur (deviasi terdokumentasi dari desain §5): transport
 *  jaringan (lwIP/BearSSL) dan mesin build (spawn mtcc.mrp)
 *  tinggal di kernel; userland belum punya syscall socket.
 *  Versi userland menyusul saat ring-3 net API tersedia.
 *
 *  Perintah:
 *    eggkg update [sumber]     unduh package.list (+index.idx)
 *    eggkg install <nama> [-y] unduh sumber + build + pasang /bin
 *    eggkg remove <nama> [-y]  hapus file /bin tercatat
 *    eggkg list                paket terpasang
 *    eggkg search <pola>       cari di index
 *    eggkg info <nama>         metadata paket
 *    eggkg sync                auto-detect .local -> /bin (dipanggil
 *                              juga saat boot bila dependencies.bash=true)
 *    eggkg help
 * ============================================================ */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatch utama. args = string SETELAH kata "eggkg" (boleh ""). */
void eggkg_cmd(const char* args);

/* Auto-aktivasi saat shell siap (shell_entry):
 *  - baca [dependencies] bash di system.ecf aktif
 *  - self-heal: salin .mrp dari /equinox/.local/<pkg>/ ke /bin
 *  - satu baris status bila ada paket; diam bila kosong */
void eggkg_boot_check(void);

/* Hook build (implementasi di shell.cpp): jalankan
 *   mtcc -make <abspath>
 * lewat spawn mtcc.mrp + task_wait_pid. Return: 0 = ok,
 * >0 = exit code mtcc, -1 = spawn/file gagal (pesan dicetak). */
int shell_eggkg_build_ruf(const char* abspath);

#ifdef __cplusplus
}
#endif

#endif /* EGGKG_H */
