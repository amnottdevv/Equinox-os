#ifndef MORPH_H_INCLUDED
#define MORPH_H_INCLUDED
/* morph.h - Equinox OS libc prelude (mtcc in-OS build)
   Spliced by: #include <morph.h> (alias <stdio.h> <stdlib.h> <string.h>)
   v0.3 FR-19: printf/sprintf/snprintf render LOCALLY (up to 5
   conversion args: %d %i %u %x %X %o %p %c %s, width/pad/left-align).
   %u is a TRUE unsigned render (binary long division by 10 — the
   full 0..4294967295 range without an unsigned type). Also added:
   sscanf, ctype, strdup/strtok/strspn/strcspn/strcasecmp, abs,
   rand/srand, puts/fputs/fputc/fgetc, remove/rename, strerror.
   Generic qsort needs function pointers (not yet in the mtcc
   subset): use qsort_int / qsort_str. */

#define NULL 0
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define MORPH_KEY_UP (-1)
#define MORPH_KEY_DOWN (-2)
#define MORPH_KEY_LEFT (-3)
#define MORPH_KEY_RIGHT (-4)
#define MORPH_KEY_HOME (-5)
#define MORPH_KEY_END (-6)
#define MORPH_KEY_PGUP (-7)
#define MORPH_KEY_PGDN (-8)
#define MORPH_KEY_DEL (-9)

/* The libc lives as REAL FILES in /equinox/libc (v0.3.2):
   modules carry their own guards and explicit dependency
   includes, so they compile standalone (mtcc --lib) and can
   also be included one by one. Order = original prelude order
   (define-before-use). */
#include "/equinox/libc/memory.c"                /* memory */
#include "/equinox/libc/string.c"                /* string */
#include "/equinox/libc/convert.c"               /* convert */
#include "/equinox/libc/ctype.c"                 /* ctype */
#include "/equinox/libc/env.c"                   /* env */
#include "/equinox/libc/heap.c"                  /* heap */
#include "/equinox/libc/strx.c"                  /* strx */
#include "/equinox/libc/printf.c"                /* printf */
#include "/equinox/libc/sscanf.c"                /* sscanf */
#include "/equinox/libc/stdio.c"                 /* stdio */
#include "/equinox/libc/qsort.c"                 /* qsort */
#include "/equinox/libc/gfx.c"                   /* gfx */
#include "/equinox/libc/pkg.c"                   /* pkg (v0.9.3) */
#endif
