/* mf_shared.h — header bersama untuk test multi-file (lihat mf_main.c).
 *
 * Dipakai oleh DUA translation unit. Include-guard MORPH-style dipakai juga
 * di sini: mtcc mempertahankan tabel makro antar file dalam satu daftar
 * argumen, jadi include kedua ditekan guard-nya (model "semantik splice").
 *
 * Catatan: struct/union BY VALUE tidak didukung mtcc — prototipe memakai
 * pointer, bukan `struct Point mk(...)`.
 */
#ifndef MF_SHARED_H
#define MF_SHARED_H

struct Point { int x; int y; };

void mk(struct Point* out, int a, int b);
int  dx(struct Point* p);

extern int total;          /* didefinisikan di mf_helper.c */

#endif
