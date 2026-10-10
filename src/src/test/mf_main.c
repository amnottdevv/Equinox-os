/* mf_main.c — TU pertama dari test MULTI-FILE compile/link.
 *
 * Dipanggil sebagai:  mtcc_host run test/mf_main.c test/mf_helper.c
 * (atau dalam OS:     mtcc prog.mrp... / mtcc -c -o out.mrp a.c b.c )
 *
 * Yang diuji:
 *   1. panggilan fungsi yang didefinisikan di file LAIN (fixup forward
 *      lintas-file — mekanisme yang sama dengan prototipe dalam satu file)
 *   2. `extern int total` dari header, didefinisikan di mf_helper.c
 *      (deferred fixup: offset data diketahui baru setelah file ke-2)
 *   3. struct lewat pointer di kedua file + include header relatif
 *      `#include "mf_shared.h"` (resolve ke direktori sumber)
 *   4. #include <morph.h> di DUA file — guard-nya harus menekan splice
 *      ulang libc (kalau tidak: "duplicate global")
 */
#include <morph.h>
#include "mf_shared.h"

int main(void) {
    struct Point p;

    mk(&p, 10, 4);              /* defined in mf_helper.c */
    total = total + 1;          /* extern, defined in mf_helper.c */

    print("dx=");
    printint(dx(&p));           /* 10 - 4 */
    print(" total=");
    printint(total);            /* 41 + 1 */
    print(" done\n");
    return 0;
}
