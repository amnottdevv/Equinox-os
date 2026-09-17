// ============================================================
//  cc.h — lwIP architecture glue for the Equinox OS kernel
// ------------------------------------------------------------
//  Compiled TOGETHER with the lwIP sources (gcc, rule
//  $(BUILD_DIR)/lwip/%.o) against the kernel include path —
//  the kernel headers (stdio.h, string.h) are C-safe
//  (__cplusplus guards) and provide printf + mem* with
//  C linkage (proven by the LVGL build).
// ============================================================
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>      /* kernel console printf (extern "C")    */
#include <string.h>     /* kernel libstring: mem*/str*           */

/* x86 = little endian */
#define BYTE_ORDER LITTLE_ENDIAN

/* Packing struct (GCC) */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x __attribute__((packed))

/* Diag + assert ke console kernel */
#ifdef __cplusplus
extern "C" {
#endif
int  printf(const char* fmt, ...);
#ifdef __cplusplus
}
#endif

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    printf("lwIP assert: %s (file " "%s" " line %d)\n", \
           (x), __FILE__, __LINE__);                     \
    for (;;) { }                                         \
} while (0)

/* Does lwIP 2.1.3 need its own "struct __errno"? No — err_t
 * is independent of errno. Rand is unused (LWIP_RAND undefined). */

#endif /* LWIP_ARCH_CC_H */
