/* mf_helper.c — TU kedua dari test MULTI-FILE compile/link.
 *
 * File ini menyediakan semua simbol yang dipakai mf_main.c:
 *   - `total` (definisi storage untuk `extern int total` di header)
 *   - mk() / dx()
 * Struct BY VALUE tidak didukung, jadi mk() menulis lewat pointer.
 */
#include <morph.h>
#include "mf_shared.h"

int total = 41;

void mk(struct Point* out, int a, int b) {
    out->x = a;
    out->y = b;
}

int dx(struct Point* p) {
    return p->x - p->y;
}
