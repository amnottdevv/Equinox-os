/* divzero.c — RING 3 in-OS test (mtcc): division by zero from code
 * compiled by mtcc itself. Proves that programs built by the in-OS
 * compiler also run at CPL 3: 1/0 = program killed + report,
 * shell stays alive (not a kernel panic like pre-v10.7).
 *
 * Compile inside Equinox OS:   mtcc /test/divzero.c
 * Run:                      run divzero.mrp
 */
int main() {
    int zero;
    int fourtytwo;
    int result;

    zero = 0;
    fourtytwo = 42;

    print("divzero: 42 / 0 = ");
    result = fourtytwo / zero;
    printint(result);
    print("\n");
    return 0;
}
