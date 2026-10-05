#ifndef MRP_API_H_KERNEL_SHIM
#define MRP_API_H_KERNEL_SHIM

/*
 * ============================================================================
 *  mrp_api.h (kernel side) — THIN SHIM ONLY
 * ----------------------------------------------------------------------------
 *  Do NOT define struct mrp_api_t here. The single canonical definition
 *  lives in mrp_user/mrp_api.h. This file only exists so that kernel code
 *  can #include "header/mrp_api.h" (a path consistent with the other
 *  kernel headers) without having to know the relative path to mrp_user/.
 *
 *  The path ../../../mrp_user/mrp_api.h is computed from this file:
 *    kernel/library/header/mrp_api.h        (this file)
 *    -> ../                                  kernel/library/
 *    -> ../../                               kernel/
 *    -> ../../../                            morphos root
 *    -> ../../../mrp_user/mrp_api.h          target
 *
 *  If the kernel directory structure changes (e.g. header/ moves),
 *  recompute this path. Or, more robust: add a -I flag to the makefile
 *  INCLUDES so that both kernel & userland can #include a plain
 *  <mrp_api.h> regardless of the relative path.
 * ============================================================================
 */
#include "../../../mrp_user/mrp_api.h"

#endif /* MRP_API_H_KERNEL_SHIM */
