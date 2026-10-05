/* undeftest.c — FR-20 regression: a prototype that is CALLED
 * but never DEFINED must fail at "link" time with a real
 * undefined-reference diagnostic (called-from line included), not
 * silently emit a .mrp that jumps to garbage.
 *
 * Expect: mtcc: error line 12: undefined reference to 'ghost' ...
 * (line 12 = the CALL SITE, reported by the FR-20 check), compile
 * FAILS, no .mrp is written.
 */
int ghost(int x);

int main() {
    return ghost(5);
}
