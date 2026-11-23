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

/* ---- GPU-side end-of-submission clock (RADV_XCLIPSE_LATENCY, default off) --------------------------
 *
 * The emulator's GPU-busy figures come from CP timestamps, so they can only say how long the parts it
 * stamped took, not when they ran. This writes one bottom-of-pipe timestamp at the end of every
 * command buffer and, at each long fence wait, reads it back beside the GPU clock, which gives the
 * only quantity that separates the two explanations for a 538 ms wait: how long ago the GPU finished
 * the work the wait blocks on. Same clock on both sides, so no calibration is needed. */
void radv_xclipse_latency_cb_begin(struct radv_cmd_buffer *cmd_buffer);
void radv_xclipse_latency_cb_end(struct radv_cmd_buffer *cmd_buffer);
void radv_xclipse_latency_submit(const void *const *cbs, unsigned count, uint64_t submit);
uint64_t radv_xclipse_next_submit_seq(void);
void radv_xclipse_phase_submit(uint64_t ns);
void radv_xclipse_phase_wait(uint64_t ns);
void radv_xclipse_phase_enter(bool is_wait);
void radv_xclipse_phase_exit(bool is_wait);
void radv_xclipse_latency_wait(uint64_t wait_ns, uint64_t waited_submit, const void *active_sync);
void radv_xclipse_native_trace_fence_wait_begin(const void *active_sync, uint32_t fence_count, uint32_t wait_all);

/* Weak hook used by the shared Vulkan fence code. `active_sync` is the vk_sync the wait blocks on
 * (vk_fence_get_active_sync), which is the key radv_xclipse_diag_note_signal() recorded. */
void radv_xclipse_native_trace_fence_wait(const void *active_sync, uint32_t fence_count, uint32_t wait_all,
                                          int result, uint64_t wait_ns);

/* ---- submit-side accounting (RADV_XCLIPSE_SUBMIT_DIAG, default off) -------------------------------
 *
 * Recording and execution are different quantities and the census only sees the first, so the
 * submit side is counted where the hardware sees it: one line per vkQueueSubmit with the packets
 * being handed over, and the age of the submission every long fence wait blocks on. The age is what
 * separates "the GPU is executing this" from "this was queued ages ago and the GPU sits idle", which
 * is the question the 550-630 ms frame time asks. */
bool radv_xclipse_submit_diag(void);                                   /* env RADV_XCLIPSE_SUBMIT_DIAG */
void radv_xclipse_diag_note_signal(const void *sync, uint64_t submit_seq);                   /* one per signalled sync */
void radv_xclipse_diag_submit(uint64_t submit_seq, uint32_t ip_type, uint32_t cmd_buffers, uint32_t general_draws, uint32_t taskmesh_draws,
                              uint32_t task_draws, uint32_t executions, uint32_t dispatch_pkts, uint32_t mesh_pkts,
                              uint32_t wait_count, uint32_t signal_count);

#endif /* RADV_XCLIPSE_NATIVE_TRACE_H */
