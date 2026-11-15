/*
 * Copyright © 2026 JimVulkan
 *
 * SPDX-License-Identifier: MIT
 *
 * Opt-in targeted trace for the Xclipse native task/mesh path.
 *
 * RADV_XCLIPSE_MESH_DIAG=3 enables a bounded readback at the first slow
 * Android frame boundaries. It records the last task pass (GPU-written count
 * entry, first draw-ring records, first payload words), the last indirect
 * conversion record, and the last submit metadata. Only the first eight slow
 * frames are reported; full BOs are never dumped.
 *
 * The trace is log-only and runs only when the level-3 gate is enabled.
 */
#ifndef RADV_XCLIPSE_NATIVE_TRACE_H
#define RADV_XCLIPSE_NATIVE_TRACE_H

#include <stdbool.h>
#include <stdint.h>

struct radv_cmd_buffer;
struct radv_device;
struct radv_shader;

void radv_xclipse_native_trace_record_task(struct radv_cmd_buffer *cmd_buffer, const uint32_t grid[3],
                                           uint32_t first, uint32_t n, bool indirect, uint64_t count_va,
                                           uint64_t draw_ring_va, uint32_t mesh_draw_count, uint32_t draw_id,
                                           const struct radv_shader *task, const struct radv_shader *mesh);

void radv_xclipse_native_trace_record_convert(struct radv_cmd_buffer *cmd_buffer, uint64_t src_va, uint32_t stride,
                                              uint64_t count_va, uint32_t max_count, bool task, uint64_t dst_va,
                                              uint32_t dst_offset);

void radv_xclipse_native_trace_record_submit(unsigned ip_type, unsigned ring, unsigned ibs, unsigned handles,
                                             uint64_t seq_no, bool user_fence, int result);

void radv_xclipse_native_trace_present(struct radv_device *device);

/* Weak hook used by the shared Vulkan fence code. */
void radv_xclipse_native_trace_fence_wait(uint32_t fence_count, uint32_t wait_all, int result, uint64_t wait_ns);

#endif /* RADV_XCLIPSE_NATIVE_TRACE_H */
