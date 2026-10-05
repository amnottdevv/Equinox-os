/**
 * @file desktop.h
 * @brief Public API desktop EquiX + perintah ThorVG (dipakai shell.cpp).
 *
 * 0.4 Beta: dispatch shell "desktop"/"tvg*" sempat hilang bersama sesi
 * sebelumnya (clone gagal) — header ini + restore dispatch di shell.cpp.
 */
#ifndef EQUINOX_DESKTOP_H
#define EQUINOX_DESKTOP_H

#ifdef __cplusplus
namespace equix {
#endif

/* Jalankan desktop penuh (blocking sampai ESC / MENU > Exit to shell). */
void desktop_run(void);

#ifdef __cplusplus
}
#endif

/* thorvg_glue.cpp */
void cmd_tvgdemo(void);
void cmd_tvgbench(void);
void cmd_tvginfo(void);

#endif /* EQUINOX_DESKTOP_H */
