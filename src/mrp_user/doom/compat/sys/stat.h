/* compat/sys/stat.h — m_misc.c includes it on the non-WIN32 branch
 * and calls mkdir() (savegame directory creation). Equinox OS has a flat
 * RAMFS; mkdir is a harmless no-op stub. */
#ifndef MORPH_COMPAT_SYS_STAT_H
#define MORPH_COMPAT_SYS_STAT_H
#include <sys/types.h>
#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IRWXU 00700
int mkdir(const char* path, ...);
#endif
