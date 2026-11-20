/*
 * Copyright © 2026 JimVulkan
 *
 * SPDX-License-Identifier: MIT
 *
 * Bounded, opt-in trace for the Xclipse native task/mesh path. The trace is
 * collected while recording and read back at the Android frame boundary. It
 * never changes the command stream and never dumps a full BO.
 */
#include "radv_xclipse_native_trace.h"

#include <stdio.h>
#include <stdlib.h>

#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "ac_gpu_info.h"
#include "radv_cmd_buffer.h"
#include "radv_device.h"
#include "radv_physical_device.h"
#include "radv_radeon_winsys.h"
#include "radv_shader.h"
#include "radv_xclipse_mesh_diag.h"

struct radv_xclipse_native_trace {
   struct radv_device *device;

   bool have_task;
   bool task_indirect;
   uint32_t task_grid[3];
   uint32_t task_first;
   uint32_t task_n;
   uint32_t task_mesh_draw_count;
   uint32_t task_draw_id;
   uint64_t task_count_va;
   uint64_t task_draw_ring_va;
   const struct radv_shader *task;
   const struct radv_shader *mesh;

   bool have_convert;
   bool convert_task;
   uint64_t convert_src_va;
   uint64_t convert_count_va;
   uint64_t convert_dst_va;
   uint32_t convert_stride;
   uint32_t convert_max_count;
   uint32_t convert_dst_offset;
   struct radeon_winsys_bo *convert_upload_bo;

   bool have_submit;
   unsigned submit_ip_type;
   unsigned submit_ring;
   unsigned submit_ibs;
   unsigned submit_handles;
   uint64_t submit_seq_no;
   bool submit_user_fence;
   int submit_result;

   uint64_t last_present_ns;
   unsigned slow_run;
   unsigned slow_reported;
};

static struct radv_xclipse_native_trace trace;
static simple_mtx_t trace_lock = SIMPLE_MTX_INITIALIZER;

static bool
trace_enabled(void)
{
   return radv_xclipse_mesh_diag_level() >= 3;
}

void
radv_xclipse_native_trace_record_task(struct radv_cmd_buffer *cmd_buffer, const uint32_t grid[3], uint32_t first,
                                      uint32_t n, bool indirect, uint64_t count_va, uint64_t draw_ring_va,
                                      uint32_t mesh_draw_count, uint32_t draw_id, const struct radv_shader *task,
                                      const struct radv_shader *mesh)
{
   if (!trace_enabled())
      return;

   simple_mtx_lock(&trace_lock);
   trace.device = radv_cmd_buffer_device(cmd_buffer);
   trace.have_task = true;
   trace.task_indirect = indirect;
   trace.task_grid[0] = grid[0];
   trace.task_grid[1] = grid[1];
   trace.task_grid[2] = grid[2];
   trace.task_first = first;
   trace.task_n = n;
   trace.task_mesh_draw_count = mesh_draw_count;
   trace.task_draw_id = draw_id;
   trace.task_count_va = count_va;
   trace.task_draw_ring_va = draw_ring_va;
   trace.task = task;
   trace.mesh = mesh;
   simple_mtx_unlock(&trace_lock);
}

void
radv_xclipse_native_trace_record_convert(struct radv_cmd_buffer *cmd_buffer, uint64_t src_va, uint32_t stride,
                                         uint64_t count_va, uint32_t max_count, bool task, uint64_t dst_va,
                                         uint32_t dst_offset)
{
   if (!trace_enabled())
      return;

   simple_mtx_lock(&trace_lock);
   trace.device = radv_cmd_buffer_device(cmd_buffer);
   trace.have_convert = true;
   trace.convert_task = task;
   trace.convert_src_va = src_va;
   trace.convert_count_va = count_va;
   trace.convert_dst_va = dst_va;
   trace.convert_stride = stride;
   trace.convert_max_count = max_count;
   trace.convert_dst_offset = dst_offset;
   trace.convert_upload_bo = cmd_buffer->upload.upload_bo;
   simple_mtx_unlock(&trace_lock);
}

void
radv_xclipse_native_trace_record_submit(unsigned ip_type, unsigned ring, unsigned ibs, unsigned handles,
                                        uint64_t seq_no, bool user_fence, int result)
{
   if (!trace_enabled())
      return;

   simple_mtx_lock(&trace_lock);
   trace.have_submit = true;
   trace.submit_ip_type = ip_type;
   trace.submit_ring = ring;
   trace.submit_ibs = ibs;
   trace.submit_handles = handles;
   trace.submit_seq_no = seq_no;
   trace.submit_user_fence = user_fence;
   trace.submit_result = result;
   simple_mtx_unlock(&trace_lock);
}

static void
trace_task_ring(struct radv_device *device, const struct radv_xclipse_native_trace *snapshot)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_winsys_bo *bo = device->xclipse_task_bo;

   if (!bo) {
      RADV_XCLIPSE_MESH_DIAG(3, "TRACE task ring absent\n");
      return;
   }

   const uint64_t rings_va = radv_buffer_get_va(bo);
   const uint32_t bo_size = pdev->task_info.bo_size_bytes;
   const uint32_t draw_off = (uint32_t)(snapshot->task_draw_ring_va - rings_va);
   const uint32_t payload_off = pdev->task_info.payload_ring_offset;
   const uint32_t *map = radv_buffer_map(device->ws, bo);

   if (!map) {
      RADV_XCLIPSE_MESH_DIAG(3, "TRACE task ring map failed bo_size=%u\n", bo_size);
      return;
   }

   /* The task ring BO belongs to the device and nothing keeps its CPU mapping between calls
    * (radv_xclipse_task_bo and radv_initialise_task_control_buffer both map and unmap it), so
    * mapping it here and unmapping it at the end hands back no one else's mapping. Unlike the
    * command buffer's upload BO below: that one stays mapped. */

   uint32_t count = 0;
   uint32_t count_off = 0;
   if (snapshot->task_count_va) {
      count_off = (uint32_t)(snapshot->task_count_va - rings_va);
      if (count_off + sizeof(count) <= bo_size)
         count = map[count_off / 4];
      else
         RADV_XCLIPSE_MESH_DIAG(3, "TRACE task count offset out of range off=%u bo_size=%u\n", count_off, bo_size);
   } else {
      /* Direct task draws carry their workgroup count on the CPU; there is no
       * GPU-written count entry to read. */
      count = snapshot->task_n;
   }

   RADV_XCLIPSE_MESH_DIAG(3,
                          "TRACE task indirect=%u grid=%u,%u,%u first=%u n=%u mesh_draw_count=%u draw_id=%u "
                          "count_off=%u count=%u draw_off=%u payload_off=%u task=%p mesh=%p\n",
                          snapshot->task_indirect, snapshot->task_grid[0], snapshot->task_grid[1],
                          snapshot->task_grid[2], snapshot->task_first, snapshot->task_n,
                          snapshot->task_mesh_draw_count, snapshot->task_draw_id, count_off, count, draw_off,
                          payload_off, (const void *)snapshot->task, (const void *)snapshot->mesh);

   if (draw_off + 8 * AC_TASK_DRAW_ENTRY_BYTES <= bo_size) {
      const uint32_t *d = &map[draw_off / 4];
      RADV_XCLIPSE_MESH_DIAG(3,
                             "TRACE draw0..7 %u,%u,%u,%u %u,%u,%u,%u %u,%u,%u,%u %u,%u,%u,%u "
                             "%u,%u,%u,%u %u,%u,%u,%u %u,%u,%u,%u %u,%u,%u,%u\n",
                             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13],
                             d[14], d[15], d[16], d[17], d[18], d[19], d[20], d[21], d[22], d[23], d[24], d[25], d[26],
                             d[27], d[28], d[29], d[30], d[31]);
   } else {
      RADV_XCLIPSE_MESH_DIAG(3, "TRACE draw ring offset out of range off=%u bo_size=%u\n", draw_off, bo_size);
   }

   if (payload_off + 8 * sizeof(uint32_t) <= bo_size) {
      const uint32_t *p = &map[payload_off / 4];
      RADV_XCLIPSE_MESH_DIAG(3, "TRACE payload0..7 %u,%u,%u,%u,%u,%u,%u,%u\n", p[0], p[1], p[2], p[3], p[4], p[5], p[6],
                             p[7]);
   }

   device->ws->buffer_unmap(device->ws, bo, false);
}

static void
trace_converted_record(const struct radv_xclipse_native_trace *snapshot)
{
   if (!snapshot->convert_upload_bo) {
      RADV_XCLIPSE_MESH_DIAG(3, "TRACE convert upload BO absent max=%u stride=%u task=%u\n",
                             snapshot->convert_max_count, snapshot->convert_stride, snapshot->convert_task);
      return;
   }

   /* The converted records live in the command buffer's upload BO, whose CPU mapping the command
    * buffer owns (cmd_buffer->upload.map) and writes through while it records. buffer_map would
    * hand back that very mapping and buffer_unmap would munmap it, leaving the command buffer to
    * write into an unmapped range on the next frame (a SIGSEGV at a page boundary, which is what
    * the level-3 build did at the first slow frame). So the records are not read back here: the
    * CONV line at record time already carries src/count/dst/stride/max. */
   RADV_XCLIPSE_MESH_DIAG(3,
                          "TRACE convert task=%u src=0x%llx count_va=0x%llx dst=0x%llx off=%u stride=%u max=%u "
                          "records=%u (not read back: the upload BO's mapping belongs to the command buffer)\n",
                          snapshot->convert_task, (unsigned long long)snapshot->convert_src_va,
                          (unsigned long long)snapshot->convert_count_va, (unsigned long long)snapshot->convert_dst_va,
                          snapshot->convert_dst_offset, snapshot->convert_stride, snapshot->convert_max_count,
                          snapshot->convert_max_count < 4 ? snapshot->convert_max_count : 4);
}

void
radv_xclipse_native_trace_present(struct radv_device *device)
{
   if (!trace_enabled())
      return;

   const uint64_t now = os_time_get_nano();
   struct radv_xclipse_native_trace snapshot;
   bool have_task, have_convert, have_submit;
   unsigned slow;

   simple_mtx_lock(&trace_lock);
   const uint64_t frame_ns = trace.last_present_ns ? now - trace.last_present_ns : 0;
   trace.last_present_ns = now;

   /* A normal 30 FPS frame is ~33 ms. Report only a run of slow frames: the
    * first eight frames of one run. Isolated slow frames (shader compilation
    * or scene loading at startup) are ignored, and a fast frame starts a new
    * run. The trace state is the last task pass/conversion/submit of the frame
    * being reported, which is what the readback needs. */
   if (frame_ns < 100000000ull) {
      trace.slow_run = 0;
      trace.slow_reported = 0;
      simple_mtx_unlock(&trace_lock);
      return;
   }

   trace.slow_run++;
   if (trace.slow_run < 2 || trace.slow_reported >= 8) {
      simple_mtx_unlock(&trace_lock);
      return;
   }

   slow = trace.slow_reported++;
   snapshot = trace;
   have_task = trace.have_task;
   have_convert = trace.have_convert;
   have_submit = trace.have_submit;

   /* Do not report the same frame's state at later slow presents. */
   trace.have_task = false;
   trace.have_convert = false;
   trace.have_submit = false;
   simple_mtx_unlock(&trace_lock);

   RADV_XCLIPSE_MESH_DIAG(3, "TRACE present slow=%u frame_ms=%llu task=%u convert=%u submit=%u\n", slow,
                          (unsigned long long)(frame_ns / 1000000ull), have_task, have_convert, have_submit);

   if (snapshot.device == device && have_task)
      trace_task_ring(device, &snapshot);
   if (snapshot.device == device && have_convert)
      trace_converted_record(&snapshot);
   if (have_submit)
      RADV_XCLIPSE_MESH_DIAG(3, "TRACE submit ip=%u ring=%u ibs=%u handles=%u seq=%llu user_fence=%u result=%d\n",
                             snapshot.submit_ip_type, snapshot.submit_ring, snapshot.submit_ibs,
                             snapshot.submit_handles, (unsigned long long)snapshot.submit_seq_no,
                             snapshot.submit_user_fence, snapshot.submit_result);
}

/* ---- submit-side accounting ---------------------------------------------------------------- */

#define XD_SYNC_RING 64

struct xd_sync {
   const void *sync;
   uint64_t submitted_ns;
   uint64_t seq;
};

static struct xd_sync xd_ring[XD_SYNC_RING];
static uint64_t xd_seq;
static uint64_t xd_last_submit_ns;
static uint32_t xd_submit_n;
static uint32_t xd_reports;

bool
radv_xclipse_submit_diag(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("RADV_XCLIPSE_SUBMIT_DIAG");
      cached = e && *e ? atoi(e) : 0;
      if (cached)
         fprintf(stderr, "radv: xclipse: submit diag %d: one line a vkQueueSubmit and the age of every fence "
                         "wait over 100 ms\n", cached);
   }
   return cached != 0;
}

/* A sync signalled by a submission; the ring is small because only the last few submissions can still
 * be waited on (the app's slots are reused in order). */
void
radv_xclipse_diag_note_signal(const void *sync)
{
   if (!sync || !radv_xclipse_submit_diag())
      return;
   struct xd_sync *e = &xd_ring[(xd_seq++) % XD_SYNC_RING];
   e->sync = sync;
   e->submitted_ns = os_time_get_nano();
   e->seq = xd_seq;
}

void
radv_xclipse_diag_submit(uint32_t ip_type, uint32_t cmd_buffers, uint32_t general_draws, uint32_t taskmesh_draws,
                         uint32_t task_draws, uint32_t executions, uint32_t dispatch_pkts, uint32_t mesh_pkts,
                         uint32_t wait_count, uint32_t signal_count)
{
   if (!radv_xclipse_submit_diag())
      return;
   const uint64_t now = os_time_get_nano();
   const uint64_t gap_ms = xd_last_submit_ns ? (now - xd_last_submit_ns) / 1000000ull : 0;
   xd_last_submit_ns = now;

   /* Every submission is printed until the first 64, then one in 16: at 1.7 fps a frame is ~10
    * submissions, so 64 lines is several frames of the steady state, and the gap to the previous
    * submit is what shows a stalled queue (the ioctl itself is timed by RADV_XCLIPSE_PERF). */
   const uint32_t submit_no = xd_submit_n++;
   if (submit_no < 64 || (submit_no % 16) == 0)
      fprintf(stderr, "radv: xclipse: submit #%u ip=%u cbs=%u draws=%u (general %u, taskmesh %u) task=%u exec=%u "
                      "dispatch_pkts=%u mesh_pkts=%u waits=%u signals=%u since_last_ms=%llu\n",
              submit_no, ip_type, cmd_buffers, general_draws + taskmesh_draws, general_draws, taskmesh_draws,
              task_draws, executions, dispatch_pkts, mesh_pkts, wait_count, signal_count, (unsigned long long)gap_ms);
   xd_reports++;
}

void
radv_xclipse_native_trace_fence_wait(const void *active_sync, uint32_t fence_count, uint32_t wait_all, int result,
                                     uint64_t wait_ns)
{
   if (radv_xclipse_submit_diag() && wait_ns >= 100000000ull) {
      /* How long ago the submission behind this fence was handed to the kernel. If the age is about
       * the wait, the GPU/kernel held the work for the whole wait; if it is much larger, the wait is
       * for a submission that was already old when the wait began. */
      uint64_t age_ms = 0, seq = 0;
      bool found = false;
      /* Backwards: the same sync object is signalled again every frame, so the ring can hold several
       * submissions for this pointer and the newest one is the submission the wait is for. */
      if (active_sync) {
         const uint64_t newest = xd_seq;
         for (unsigned n = 0; n < XD_SYNC_RING && n < newest; n++) {
            const struct xd_sync *e = &xd_ring[(newest - 1 - n) % XD_SYNC_RING];
            if (e->sync == active_sync) {
               age_ms = (os_time_get_nano() - e->submitted_ns) / 1000000ull;
               seq = e->seq;
               found = true;
               break;
            }
         }
      }
      fprintf(stderr, "radv: xclipse: fence wait_ms=%llu sync=%s submit_age_ms=%llu seq=%llu queues=%u wait_all=%u "
                      "result=%d submits_reported_so_far=%u\n",
              (unsigned long long)(wait_ns / 1000000ull), found ? "tracked" : "untracked",
              (unsigned long long)age_ms, (unsigned long long)seq, fence_count, wait_all, result, xd_reports);
   }

   if (!trace_enabled() || wait_ns < 100000000ull)
      return;

   RADV_XCLIPSE_MESH_DIAG(3, "TRACE fence wait_ms=%llu fences=%u wait_all=%u result=%d\n",
                          (unsigned long long)(wait_ns / 1000000ull), fence_count, wait_all, result);
}
