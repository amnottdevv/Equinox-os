#ifndef CONFIG_CMD_H
#define CONFIG_CMD_H

/* ============================================================
 *  config_cmd.h — builtin shell `config` + `call` + handler bawaan
 * ------------------------------------------------------------
 *  - `config` : inspeksi .config/ + store + registry ecf_caller
 *      config            ringkas (store aktif + jumlah tool + caller)
 *      config list       daftar berkas .config (semua tool)
 *      config show <t>   tampilkan isi .config/<t>.ecf
 *      config get <t> <k>  baca satu key dari .config/<t>.ecf
 *      config callers    daftar nama aksi terdaftar (ecf_caller)
 *  - `call <nama> [args...]` : dispatch ecf_call dari prompt.
 *  - config_init() : daftarkan handler bawaan (set.*) sekali saat
 *    shell siap.
 * ============================================================ */

#include "library/header/fs_ram.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Perintah `config` (args = string SETELAH kata "config", boleh ""). */
void config_cmd(struct fs_node* cwd, const char* args);

/* Perintah `call` (args = string SETELAH kata "call"). */
void config_call_cmd(struct fs_node* cwd, const char* args);

/* Daftarkan handler bawaan ke registry ecf_caller (idempoten). */
void config_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_CMD_H */
