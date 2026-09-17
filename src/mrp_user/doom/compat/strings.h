/* compat/strings.h — doomtype.h includes <strings.h> for
 * strcasecmp/strncasecmp, which live in our string.h shim. */
#ifndef MORPH_COMPAT_STRINGS_H
#define MORPH_COMPAT_STRINGS_H
#include <string.h>
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);
int ffs(int x);
#endif
