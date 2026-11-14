/*
 * Copyright © 2026 JimVulkan
 *
 * SPDX-License-Identifier: MIT
 *
 * Opt-in diagnostics for the v1.9 task/mesh emulation.  This file only logs; it
 * never changes the emitted command stream.
 *
 * RADV_XCLIPSE_MESH_DIAG=0  off (default)
 * RADV_XCLIPSE_MESH_DIAG=1  device/ring/pipeline info, sampled app-draw
 *                           summaries, and rate-limited warnings
 * RADV_XCLIPSE_MESH_DIAG=2  also sampled ring passes, indirect conversions
 *                           and DRAW_INDIRECT_MULTI packet arguments
 *
 * Lines are prefixed with "[X940-19]" so they can be filtered from a full
 * BananaNative log.
 */
#ifndef RADV_XCLIPSE_MESH_DIAG_H
#define RADV_XCLIPSE_MESH_DIAG_H

#include <stdio.h>

#include "util/u_debug.h"

DEBUG_GET_ONCE_NUM_OPTION(xclipse_mesh_diag, "RADV_XCLIPSE_MESH_DIAG", 0)

static inline int
radv_xclipse_mesh_diag_level(void)
{
   return (int)debug_get_option_xclipse_mesh_diag();
}

/* Per-draw summaries are sampled per recording thread so a frame cannot turn
 * logging into the bottleneck: the first 256, then every 1024th.  Some warning
 * classes are also rate-limited; the log documents those limits. */
static inline bool
radv_xclipse_mesh_diag_sample(unsigned *counter)
{
   const unsigned n = (*counter)++;
   return n < 256 || (n & 1023) == 0;
}

#define RADV_XCLIPSE_MESH_DIAG(level, ...)                                                                             \
   do {                                                                                                                \
      if (radv_xclipse_mesh_diag_level() >= (level))                                                                   \
         fprintf(stderr, "[X940-19] " __VA_ARGS__);                                                                    \
   } while (0)

#define RADV_XCLIPSE_MESH_DIAG_WARN(...)                                                                               \
   do {                                                                                                                \
      if (radv_xclipse_mesh_diag_level() > 0)                                                                          \
         fprintf(stderr, "[X940-19] WARN " __VA_ARGS__);                                                               \
   } while (0)

#endif /* RADV_XCLIPSE_MESH_DIAG_H */
