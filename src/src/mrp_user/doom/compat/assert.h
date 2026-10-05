/* compat/assert.h — prints and exits through the Equinox OS console. */
#ifndef MORPH_COMPAT_ASSERT_H
#define MORPH_COMPAT_ASSERT_H
void __assert_fail(const char* expr, const char* file, int line);
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__))
#endif
