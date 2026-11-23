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
#include "radv_cs.h"
#include "radv_device.h"
#include "radv_physical_device.h"
#include "radv_radeon_winsys.h"
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>

#include "radv_query.h"
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
static uint64_t xd_seq;      /* submissions seen: the one sequence number the fence ring and the
                              * recording chain both use, so a wait can be resolved against records */
static uint64_t xd_ring_n;   /* ring position, advanced per signalled sync */
static uint64_t xd_last_submit_ns;
static uint32_t xd_reports;
static uint64_t xd_subs_at_last_wait;

/* Called once per vkQueueSubmit, before the submission is described, so both the fence ring and the
 * recording chain carry the same number. */
uint64_t
radv_xclipse_next_submit_seq(void)
{
   return ++xd_seq;
}

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
radv_xclipse_diag_note_signal(const void *sync, uint64_t submit_seq)
{
   if (!sync || !radv_xclipse_submit_diag())
      return;
   /* The ring position advances per signal; the sequence it records is the submission's, so a wait
    * resolves to a submission that also has recording records. */
   struct xd_sync *e = &xd_ring[(xd_ring_n++) % XD_SYNC_RING];
   e->sync = sync;
   e->submitted_ns = os_time_get_nano();
   e->seq = submit_seq;
}

void
radv_xclipse_diag_submit(uint64_t submit_seq, uint32_t ip_type, uint32_t cmd_buffers, uint32_t general_draws,
                         uint32_t taskmesh_draws, uint32_t task_draws, uint32_t executions, uint32_t dispatch_pkts,
                         uint32_t mesh_pkts, uint32_t wait_count, uint32_t signal_count)
{
   if (!radv_xclipse_submit_diag())
      return;
   const uint64_t now = os_time_get_nano();
   const uint64_t gap_ms = xd_last_submit_ns ? (now - xd_last_submit_ns) / 1000000ull : 0;
   xd_last_submit_ns = now;

   /* Every submission is printed until the first 64, then one in 16: at 1.7 fps a frame is ~10
    * submissions, so 64 lines is several frames of the steady state, and the gap to the previous
    * submit is what shows a stalled queue (the ioctl itself is timed by RADV_XCLIPSE_PERF). */
   const uint64_t submit_no = submit_seq;
   if (submit_no < 64 || (submit_no % 16) == 0)
      fprintf(stderr, "radv: xclipse: submit #%llu ip=%u cbs=%u draws=%u (general %u, taskmesh %u) task=%u exec=%u "
                      "dispatch_pkts=%u mesh_pkts=%u waits=%u signals=%u since_last_ms=%llu\n",
              (unsigned long long)submit_no, ip_type, cmd_buffers, general_draws + taskmesh_draws, general_draws, taskmesh_draws,
              task_draws, executions, dispatch_pkts, mesh_pkts, wait_count, signal_count, (unsigned long long)gap_ms);
   xd_reports++;
}


/* ---- GPU-side end-of-submission clock + CPU attribution (RADV_XCLIPSE_LATENCY, default off) ---- */

/* The emulator's GPU-busy figures come from CP timestamps, so they can only say how long the parts
 * it stamped took. This writes one bottom-of-pipe timestamp at the end of every command buffer and,
 * at each long fence wait, reports whether that value moved while the application was blocked (the
 * GPU retiring work during the wait) and which threads burned the CPU meanwhile. */

/* One recording instance gets two slots: top-of-pipe written at BeginCommandBuffer and bottom-of-pipe
 * at EndCommandBuffer. Slots are allocated at recording time, which is the only time a packet can be
 * written, so identity comes from the recording instance and the submission attached to it rather
 * than from the slot number. A recording that is executed twice is reported as reused, not counted
 * as two completions. */
#define XLAT_REC_MAX 1024
#define XLAT_SLOTS (XLAT_REC_MAX * 2)
#define XLAT_IDS_MAX 8

struct xlat_rec {
   const void *cb;
   uint64_t rec; /* recording instance, 0 = unused */
   uint64_t submit;
   uint64_t end_host_ns;
   bool ended;
};

static struct {
   struct radeon_winsys *ws;
   struct radeon_winsys_bo *bo;
   volatile uint64_t *map;
   uint64_t va;
   double ns_per_tick;
   bool failed;
} xlat;

static struct xlat_rec xlat_recs[XLAT_REC_MAX];
static uint64_t xlat_seen[XLAT_SLOTS]; /* last sampled value of every slot, host side */
static uint64_t xlat_rec_next;
static uint64_t xlat_uninstrumented; /* command buffers whose queue family cannot carry the packet */
static uint64_t xlat_wraps;          /* ring entries reused before their submission was seen */
static uint64_t xlat_unrecorded;     /* submitted command buffers with no recording record */
static uint64_t xlat_reused;         /* command buffers submitted again after a first submit */
static uint64_t xlat_slotless;       /* retirements seen in slots whose record was overwritten */

/* Nothing in this instrument may block a recording thread. An earlier version held a mutex across a
 * fence wait, which is held for seconds here, and took the same mutex while recording: that stalls
 * submission behind the wait, and the emulator reports the resulting gap as a GPU hang. So the tables
 * are lock-free diagnostics (benign races, per-recording entries) and only buffer creation serialises. */
static pthread_mutex_t xlat_bo_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool xlat_wait_active;
static unsigned xlat_overlaps; /* waits skipped because another wait was already bracketed */


/* 1: completion timestamp per recording, submission chain, per-wait and per-window thread CPU.
 * 2: also the top-of-pipe arrival timestamp, which is what a per-submission GPU span needs. Kept
 *    separate because the level-2 build is the only one that writes a packet ahead of a recording's
 *    own work, and the run that did so hung at a scene entry. */
static int
xclipse_latency_level(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("RADV_XCLIPSE_LATENCY");
      cached = e && *e ? atoi(e) : 0;
      if (cached)
         fprintf(stderr, "radv: xclipse: latency %d: %s, submission chain between recording and fence, per-wait and "
                         "per-window thread CPU\n", cached,
                 cached >= 2 ? "top and bottom of pipe timestamp per recording" : "completion timestamp per recording");
   }
   return cached > 0 ? cached : 0;
}

static bool
xclipse_latency_enabled(void)
{
   return xclipse_latency_level() != 0;
}

static void xclipse_thread_cpu_window(void);

/* Every slot is cleared before the GPU sees the buffer: a stale non-zero value would read as a
 * retirement that never happened. */
static bool
xlat_ensure_bo(struct radv_device *device)
{
   if (__atomic_load_n(&xlat.bo, __ATOMIC_ACQUIRE))
      return true;
   pthread_mutex_lock(&xlat_bo_mu);
   if (xlat.bo) {
      pthread_mutex_unlock(&xlat_bo_mu);
      return true;
   }
   struct radeon_winsys *ws = device->ws;
   if (ws->buffer_create(ws, XLAT_SLOTS * 8, 4096, RADEON_DOMAIN_GTT,
                         RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_GTT_WC | RADEON_FLAG_NO_INTERPROCESS_SHARING, 0, 0,
                         &xlat.bo) != VK_SUCCESS ||
       !xlat.bo) {
      xlat.failed = true;
      pthread_mutex_unlock(&xlat_bo_mu);
      fprintf(stderr, "radv: xclipse: latency: no timestamp buffer, instrument off\n");
      return false;
   }
   xlat.ws = ws;
   xlat.map = ws->buffer_map(ws, xlat.bo, false, NULL);
   xlat.va = radv_buffer_get_va(xlat.bo);
   xlat.ns_per_tick = 1000000.0 / (double)radv_device_physical(device)->info.clock_crystal_freq;
   if (!xlat.map) {
      xlat.failed = true;
      pthread_mutex_unlock(&xlat_bo_mu);
      fprintf(stderr, "radv: xclipse: latency: timestamp buffer not mapped, instrument off\n");
      return false;
   }
   for (unsigned i = 0; i < XLAT_SLOTS; i++)
      xlat.map[i] = 0;
   for (unsigned i = 0; i < XLAT_SLOTS; i++)
      xlat_seen[i] = 0;
   __atomic_store_n(&xlat.bo, xlat.bo, __ATOMIC_RELEASE);
   pthread_mutex_unlock(&xlat_bo_mu);
   return true;
}

/* Retirement sampler. Compares every slot against `pre` (a snapshot) or against the last sample, and
 * returns how many changed, naming the recording instances involved. Slots whose record has been
 * overwritten are counted separately rather than attributed to the wrong recording. */
static unsigned
xlat_scan_retired(const uint64_t *pre, char *ids, size_t ids_size, unsigned *unattributed)
{
   size_t off = 0;
   unsigned n = 0;
   ids[0] = 0;
   *unattributed = 0;
   for (unsigned i = 0; i < XLAT_SLOTS; i++) {
      const uint64_t value = xlat.map[i];
      if (!value || value == (pre ? pre[i] : xlat_seen[i]))
         continue;
      xlat_seen[i] = value;
      const struct xlat_rec *r = &xlat_recs[i / 2];
      if (!r->rec) {
         (*unattributed)++;
         continue;
      }
      n++;
      if (off + 1 < ids_size && n <= XLAT_IDS_MAX) {
         const int w = snprintf(ids + off, ids_size - off, "%s%llu%c", n > 1 ? "," : "", (unsigned long long)r->rec,
                                (i & 1) ? 'e' : 's');
         if (w > 0)
            off += (size_t)w;
      }
   }
   return n;
}

void
radv_xclipse_latency_cb_begin(struct radv_cmd_buffer *cmd_buffer)
{
   if (!xclipse_latency_enabled() || xlat.failed)
      return;

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   if (cmd_buffer->qf != RADV_QUEUE_GENERAL && cmd_buffer->qf != RADV_QUEUE_COMPUTE) {
      __atomic_add_fetch(&xlat_uninstrumented, 1, __ATOMIC_RELAXED);
      return;
   }

   if (!xlat_ensure_bo(device))
      return;
   const uint64_t rec = __atomic_add_fetch(&xlat_rec_next, 1, __ATOMIC_RELAXED);
   const unsigned ring = (unsigned)(rec % XLAT_REC_MAX);
   struct xlat_rec *r = &xlat_recs[ring];
   /* Overwriting an entry whose submission was never seen means a gap in the chain, not a
    * retirement; count it so the coverage figures stay honest. */
   if (__atomic_load_n(&r->rec, __ATOMIC_RELAXED) && !__atomic_load_n(&r->submit, __ATOMIC_RELAXED))
      __atomic_add_fetch(&xlat_wraps, 1, __ATOMIC_RELAXED);
   r->cb = cmd_buffer;
   r->submit = 0;
   r->ended = false;
   __atomic_store_n(&r->rec, rec, __ATOMIC_RELAXED);
   cmd_buffer->xc_lat_rec = rec;

   if (xclipse_latency_level() < 2)
      return;
   /* Top of pipe: when the GPU reached this recording's first packet after its prologue. Only at
    * level 2, and never the first packet of the stream. */
   radeon_check_space(device->ws, cmd_buffer->cs->b, 32);
   radv_cs_add_buffer(device->ws, cmd_buffer->cs->b, xlat.bo);
   radv_write_timestamp(cmd_buffer, xlat.va + (uint64_t)ring * 16, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
}

void
radv_xclipse_latency_cb_end(struct radv_cmd_buffer *cmd_buffer)
{
   xclipse_thread_cpu_window();
   if (!xclipse_latency_enabled() || xlat.failed)
      return;

   const uint64_t rec = cmd_buffer->xc_lat_rec;
   if (!rec)
      return;
   cmd_buffer->xc_lat_rec = 0;

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const unsigned ring = (unsigned)(rec % XLAT_REC_MAX);
   radeon_check_space(device->ws, cmd_buffer->cs->b, 32);
   radv_cs_add_buffer(device->ws, cmd_buffer->cs->b, xlat.bo);
   radv_write_timestamp(cmd_buffer, xlat.va + (uint64_t)ring * 16 + 8, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

   struct xlat_rec *r = &xlat_recs[ring];
   if (__atomic_load_n(&r->rec, __ATOMIC_RELAXED) == rec) {
      r->ended = true;
      r->end_host_ns = os_time_get_nano();
   }
}

/* Called once per vkQueueSubmit, after the submission was handed to the kernel. Attaches the submit
 * sequence to every recording instance in it, so a later retirement in a slot can be placed in the
 * submit order, and reports what has been observed to retire since the previous sample. */
void
radv_xclipse_latency_submit(const void *const *cbs, unsigned count, uint64_t submit)
{
   if (!xclipse_latency_enabled() || xlat.failed || !xlat.map)
      return;

   for (unsigned i = 0; i < count; i++) {
      struct xlat_rec *best = NULL;
      for (unsigned r = 0; r < XLAT_REC_MAX; r++) {
         if (xlat_recs[r].rec && xlat_recs[r].cb == cbs[i] && (!best || xlat_recs[r].rec > best->rec))
            best = &xlat_recs[r];
      }
      if (!best)
         xlat_unrecorded++;
      else if (best->submit)
         xlat_reused++;
      else
         best->submit = submit;
   }

   char ids[192];
   unsigned unattributed = 0;
   const unsigned retired = xlat_scan_retired(NULL, ids, sizeof(ids), &unattributed);
   if (retired)
      fprintf(stderr, "radv: xclipse: lat retired_observed submit=%llu count=%u recs=[%s] unattributed=%u\n",
              (unsigned long long)submit, retired, ids, unattributed);
   fprintf(stderr,
           "radv: xclipse: lat submit=%llu cbs=%u host_ns=%llu coverage{instr=%llu uninstr=%llu unrecorded=%llu "
           "reused=%llu wraps=%llu slotless=%llu}\n",
           (unsigned long long)submit, count, (unsigned long long)os_time_get_nano(),
           (unsigned long long)xlat_rec_next, (unsigned long long)xlat_uninstrumented,
           (unsigned long long)xlat_unrecorded, (unsigned long long)xlat_reused, (unsigned long long)xlat_wraps,
           (unsigned long long)xlat_slotless);
}

/* ---- per-thread CPU accounting (RADV_XCLIPSE_LATENCY) --------------------------------------- */

/* The question this answers is attribution, not magnitude: during a fence wait that blocks the
 * application thread, and over ordinary rendering windows, which threads of this process are
 * consuming CPU, and how much of the process total do they account for. Tick period comes from the
 * C library rather than an assumed USER_HZ, every thread is enumerated with an explicit truncation
 * flag, and the measured per-thread sum is compared against CLOCK_PROCESS_CPUTIME_ID so a missing
 * account is visible instead of silent. Nothing here runs unless the knob is on. */

#define XLAT_THREADS_MAX 512

struct xlat_thread {
   int tid;
   uint64_t cpu_ns;
   char comm[24];
};

struct xlat_snap {
   unsigned n;
   bool truncated;
   struct xlat_thread t[XLAT_THREADS_MAX];
};

struct xlat_top {
   long long cpu_ms;
   int tid;
   char comm[24];
};

static pthread_mutex_t xlat_win_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t xlat_tick_ns; /* nanoseconds per clock tick, from sysconf(_SC_CLK_TCK) */

static struct xlat_snap xlat_win_pre, xlat_win_now;

/* Where the wall clock goes, split by what this driver can see. 'other' is time the process spends
 * outside every Vulkan call the driver can hook, which is the emulator's own work plus the game's:
 * the split decides whether the frame is spent on our side of the API boundary at all. */
static uint64_t xlat_phase_submit_ns, xlat_phase_wait_ns;
static uint64_t xlat_phase_submits, xlat_phase_waits;
/* Accumulated call durations are per thread and can overlap in wall time, so their sum is not a
 * partition of the window. Counting in-flight calls makes that visible instead of arithmetic. */
static unsigned xlat_inflight_submit, xlat_inflight_wait;
static unsigned xlat_overlap_submit, xlat_overlap_wait, xlat_inflight_max;
static struct xlat_snap xlat_wait_pre, xlat_wait_now;
static uint64_t xlat_win_start_ns, xlat_win_pre_proc_ns;
static bool xlat_win_have;

static struct {
   bool armed;
   uint64_t slots[XLAT_SLOTS];
   uint64_t host_ns;
   uint64_t proc_ns;
} xlat_wait = {0};

static uint64_t
xclipse_tick_ns(void)
{
   if (!xlat_tick_ns) {
      const long hz = sysconf(_SC_CLK_TCK);
      xlat_tick_ns = hz > 0 ? 1000000000ull / (uint64_t)hz : 10000000ull;
   }
   return xlat_tick_ns;
}

static uint64_t
xclipse_proc_cpu_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Reads every thread under /proc/self/task. comm comes from the same directory entry read, so no
 * second syscall per thread and no stale cache. */
static unsigned
xclipse_snapshot_threads(struct xlat_snap *s)
{
   DIR *dir = opendir("/proc/self/task");
   s->n = 0;
   s->truncated = false;
   if (!dir)
      return 0;

   const uint64_t tick_ns = xclipse_tick_ns();
   const struct dirent *de;
   while ((de = readdir(dir))) {
      const int tid = atoi(de->d_name);
      if (tid <= 0)
         continue;
      if (s->n == XLAT_THREADS_MAX) {
         s->truncated = true;
         break;
      }

      char path[64], buf[512];
      snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
      const int fd = open(path, O_RDONLY);
      if (fd < 0)
         continue;
      const ssize_t got = read(fd, buf, sizeof(buf) - 1);
      close(fd);
      if (got <= 0)
         continue;
      buf[got] = 0;

      struct xlat_thread *t = &s->t[s->n];
      t->tid = tid;
      t->comm[0] = 0;
      snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
      const int cfd = open(path, O_RDONLY);
      if (cfd >= 0) {
         const ssize_t cgot = read(cfd, t->comm, sizeof(t->comm) - 1);
         close(cfd);
         if (cgot > 0) {
            t->comm[cgot] = 0;
            char *nl = strchr(t->comm, '\n');
            if (nl)
               *nl = 0;
         } else {
            t->comm[0] = 0;
         }
      }

      /* Fields after ") ": state is field 3, so utime and stime are fields 14 and 15 counted from 1
       * in proc(5), which is what the loop counter is, avoiding an off-by-one on a shifting token. */
      const char *tail = strrchr(buf, ')');
      if (!tail)
         continue;
      const char *q = tail + 1;
      unsigned long utime = 0, stime = 0;
      for (int f = 3; f <= 15 && q && *q; f++) {
         while (*q == ' ')
            q++;
         if (f == 14)
            utime = strtoul(q, NULL, 10);
         else if (f == 15)
            stime = strtoul(q, NULL, 10);
         q = strchr(q, ' ');
         if (!q)
            break;
      }
      t->cpu_ns = (uint64_t)(utime + stime) * tick_ns;
      s->n++;
   }
   closedir(dir);
   return s->n;
}

/* Deltas between two snapshots, largest first, plus how much of the process total they explain. */
static unsigned
xclipse_top_deltas(const struct xlat_snap *before, const struct xlat_snap *after, uint64_t proc_delta_ns,
                   struct xlat_top *out, unsigned want, uint64_t *sum_delta_ns)
{
   unsigned n = 0;
   uint64_t sum = 0;
   for (unsigned i = 0; i < after->n; i++) {
      const struct xlat_thread *a = &after->t[i];
      const struct xlat_thread *b = NULL;
      for (unsigned j = 0; j < before->n; j++) {
         if (before->t[j].tid == a->tid) {
            b = &before->t[j];
            break;
         }
      }
      if (!b || a->cpu_ns < b->cpu_ns)
         continue; /* a new thread, or a tid reused with a smaller counter: not attributable */
      const uint64_t d = a->cpu_ns - b->cpu_ns;
      sum += d;
      if (!d)
         continue;
      /* keep the list sorted, largest first, at most `want` entries */
      unsigned pos = n;
      while (pos > 0 && out[pos - 1].cpu_ms < (long long)(d / 1000000ull))
         pos--;
      if (pos >= want)
         continue;
      if (n < want)
         n++;
      for (unsigned k = n - 1; k > pos; k--)
         out[k] = out[k - 1];
      out[pos].cpu_ms = (long long)(d / 1000000ull);
      out[pos].tid = a->tid;
      memcpy(out[pos].comm, a->comm, sizeof(out[pos].comm));
   }
   *sum_delta_ns = sum;
   return n;
}

static void
xclipse_format_top(char *dst, size_t size, const struct xlat_top *top, unsigned n)
{
   size_t off = 0;
   dst[0] = 0;
   for (unsigned i = 0; i < n && off + 1 < size; i++) {
      const int w = snprintf(dst + off, size - off, "%s%s/%d:%lld", i ? ";" : "", top[i].comm[0] ? top[i].comm : "?",
                             top[i].tid, top[i].cpu_ms);
      if (w <= 0)
         break;
      off += (size_t)w;
   }
}

/* Ordinary rendering windows, so the mesh-off configuration produces data at all: it has almost no
 * long waits. Cheap when nothing is due (one clock read per command buffer end). */
static void
xclipse_thread_cpu_window(void)
{
   if (!xclipse_latency_enabled())
      return;

   const uint64_t now = os_time_get_nano();
   if (xlat_win_have && now - xlat_win_start_ns < 2000000000ull)
      return;
   if (pthread_mutex_trylock(&xlat_win_mu) != 0)
      return;

   if (xclipse_snapshot_threads(&xlat_win_now)) {
      const uint64_t proc = xclipse_proc_cpu_ns();
      if (xlat_win_have) {
         struct xlat_top top[5];
         uint64_t sum = 0;
         const unsigned n = xclipse_top_deltas(&xlat_win_pre, &xlat_win_now, proc - xlat_win_pre_proc_ns, top, 5, &sum);
         char list[256];
         xclipse_format_top(list, sizeof(list), top, n);
         const uint64_t proc_delta = proc - xlat_win_pre_proc_ns;
         const uint64_t ph_submit = __atomic_exchange_n(&xlat_phase_submit_ns, 0, __ATOMIC_RELAXED);
         const uint64_t ph_wait = __atomic_exchange_n(&xlat_phase_wait_ns, 0, __ATOMIC_RELAXED);
         const uint64_t ph_nsub = __atomic_exchange_n(&xlat_phase_submits, 0, __ATOMIC_RELAXED);
         const uint64_t ph_nwait = __atomic_exchange_n(&xlat_phase_waits, 0, __ATOMIC_RELAXED);
         const unsigned ov_submit = __atomic_exchange_n(&xlat_overlap_submit, 0, __ATOMIC_RELAXED);
         const unsigned ov_wait = __atomic_exchange_n(&xlat_overlap_wait, 0, __ATOMIC_RELAXED);
         const unsigned inflight_max = __atomic_exchange_n(&xlat_inflight_max, 0, __ATOMIC_RELAXED);
         const uint64_t wall_ns = now - xlat_win_start_ns;
         /* Calls on several threads can overlap in wall time and a call can straddle a window edge,
          * so this difference is NOT a partition and the remainder is NOT "emulator-only time": it is
          * wall time not covered by a call that this driver timed, which includes untimed Vulkan
          * entry points, the emulator, the game, and any call spanning a boundary. */
         const uint64_t unattr_ns = wall_ns > ph_submit + ph_wait ? wall_ns - ph_submit - ph_wait : 0;
         fprintf(stderr,
                 "radv: xclipse: phase window_ms=%llu submits=%llu in_submit_ms=%llu waits=%llu in_wait_ms=%llu "
                 "unattributed_ms=%llu (%.0f%%) overlap{submit=%u wait=%u} inflight_max=%u\n",
                 (unsigned long long)(wall_ns / 1000000ull), (unsigned long long)ph_nsub,
                 (unsigned long long)(ph_submit / 1000000ull), (unsigned long long)ph_nwait,
                 (unsigned long long)(ph_wait / 1000000ull), (unsigned long long)(unattr_ns / 1000000ull),
                 wall_ns ? (double)unattr_ns * 100.0 / (double)wall_ns : 0.0, ov_submit, ov_wait, inflight_max);
         fprintf(stderr,
                 "radv: xclipse: threadcpu window_ms=%llu threads=%u%s proc_cpu_ms=%llu acct=%d%% top5=[%s]\n",
                 (unsigned long long)((now - xlat_win_start_ns) / 1000000ull), xlat_win_now.n,
                 xlat_win_now.truncated ? "(truncated)" : "", (unsigned long long)(proc_delta / 1000000ull),
                 proc_delta ? (int)(sum * 100ull / proc_delta) : -1, list);
      }
      xlat_win_pre = xlat_win_now;
      xlat_win_start_ns = now;
      xlat_win_pre_proc_ns = proc;
      xlat_win_have = true;
   }
   pthread_mutex_unlock(&xlat_win_mu);
}
/* A wait is bracketed so the retirements observed inside it are attributable to it. The mutex is
 * held across the pair, so overlapping waits are skipped and counted instead of interleaved. */
void
radv_xclipse_phase_submit(uint64_t ns)
{
   if (!xclipse_latency_enabled())
      return;
   __atomic_add_fetch(&xlat_phase_submit_ns, ns, __ATOMIC_RELAXED);
   __atomic_add_fetch(&xlat_phase_submits, 1, __ATOMIC_RELAXED);
}

void
radv_xclipse_phase_wait(uint64_t ns)
{
   if (!xclipse_latency_enabled())
      return;
   __atomic_add_fetch(&xlat_phase_wait_ns, ns, __ATOMIC_RELAXED);
   __atomic_add_fetch(&xlat_phase_waits, 1, __ATOMIC_RELAXED);
}

void
radv_xclipse_phase_enter(bool is_wait)
{
   if (!xclipse_latency_enabled())
      return;
   unsigned *inflight = is_wait ? &xlat_inflight_wait : &xlat_inflight_submit;
   const unsigned now = __atomic_add_fetch(inflight, 1, __ATOMIC_RELAXED);
   if (now > 1)
      __atomic_add_fetch(is_wait ? &xlat_overlap_wait : &xlat_overlap_submit, 1, __ATOMIC_RELAXED);
   unsigned prev = __atomic_load_n(&xlat_inflight_max, __ATOMIC_RELAXED);
   while (now > prev && !__atomic_compare_exchange_n(&xlat_inflight_max, &prev, now, false, __ATOMIC_RELAXED,
                                                     __ATOMIC_RELAXED)) {
   }
}

void
radv_xclipse_phase_exit(bool is_wait)
{
   if (!xclipse_latency_enabled())
      return;
   __atomic_sub_fetch(is_wait ? &xlat_inflight_wait : &xlat_inflight_submit, 1, __ATOMIC_RELAXED);
}

void
radv_xclipse_native_trace_fence_wait_begin(uint32_t fence_count, uint32_t wait_all)
{
   (void)fence_count;
   (void)wait_all;
   if (!xclipse_latency_enabled() || xlat.failed || !xlat.map || !xlat.ws)
      return;
   /* Non-blocking claim instead of a lock: a second concurrent wait is skipped, never queued behind
    * the first, which can block for seconds. */
   if (atomic_exchange(&xlat_wait_active, true)) {
      xlat_overlaps++;
      return;
   }
   for (unsigned i = 0; i < XLAT_SLOTS; i++)
      xlat_wait.slots[i] = xlat.map[i];
   xlat_wait.host_ns = os_time_get_nano();
   xlat_wait.proc_ns = xclipse_proc_cpu_ns();
   xclipse_snapshot_threads(&xlat_wait_pre);
   xlat_wait.armed = true;
}

void
radv_xclipse_latency_wait(uint64_t wait_ns, uint64_t waited_submit)
{
   if (!xlat_wait.armed)
      return;
   xlat_wait.armed = false;

   if (!xclipse_latency_enabled() || xlat.failed || !xlat.map || !xlat.ws || wait_ns < 100000000ull) {
      atomic_store(&xlat_wait_active, false);
      return;
   }

   /* Not a wall clock on this platform: this shim returns the raw CP tick count although the UAPI
    * says nanoseconds (measured over one wait: 14.28M ticks against 13M "ns", ratio 1.06; the CP
    * clock works out at 26.18 MHz against a declared 25.6). Printed as ticks so nobody scales it. */
   const uint64_t ioctl_ticks = xlat.ws->query_value(xlat.ws, RADEON_TIMESTAMP);

   char ids[192];
   unsigned unattributed = 0;
   const unsigned retired = xlat_scan_retired(xlat_wait.slots, ids, sizeof(ids), &unattributed);

   /* Was the waited submission already finished before this thread blocked? Sampling at wait start
    * answers that with no clock comparison at all: if its completion timestamp was already written,
    * the wait is signal latency, not work; if not, the completion happened during the wait. */
   unsigned waited_end_before_wait = 0, waited_recs = 0;
   for (unsigned r = 0; r < XLAT_REC_MAX; r++) {
      const struct xlat_rec *rec = &xlat_recs[r];
      if (!rec->rec || (waited_submit && rec->submit != waited_submit))
         continue;
      waited_recs++;
      if (xlat_wait.slots[(unsigned)(rec->rec % XLAT_REC_MAX) * 2 + 1])
         waited_end_before_wait++;
   }

   /* Spacing between successive completions measured on the GPU's own clock. Host cadence alone
    * cannot say whether the GPU spent that time or the host did; this can, because both ends of a gap
    * are GPU timestamps. A gap near the host cadence means the GPU advanced between them; a gap near
    * zero with a host cadence of ~15 ms means the GPU finished them back to back and the host spaced
    * the completions. */
   {
      static uint64_t ticks[XLAT_REC_MAX];
      unsigned nt = 0;
      for (unsigned r = 0; r < XLAT_REC_MAX; r++) {
         const uint64_t rec = xlat_recs[r].rec;
         if (!rec)
            continue;
         const unsigned slot = (unsigned)(rec % XLAT_REC_MAX) * 2 + 1;
         const uint64_t end = xlat_seen[slot];
         /* Only completions that landed during this wait: gaps across the whole run mix bursts with
          * holes and cannot answer how the completions inside one wait were spaced. */
         if (end && end != xlat_wait.slots[slot])
            ticks[nt++] = end;
      }
      for (unsigned i = 1; i < nt; i++) { /* insertion sort, nt is a few dozen in practice */
         const uint64_t v = ticks[i];
         unsigned j = i;
         while (j > 0 && ticks[j - 1] > v) {
            ticks[j] = ticks[j - 1];
            j--;
         }
         ticks[j] = v;
      }
      if (nt > 1) {
         static uint64_t gaps[XLAT_REC_MAX];
         unsigned ng = 0;
         for (unsigned i = 1; i < nt; i++) {
            if (ticks[i] > ticks[i - 1])
               gaps[ng++] = ticks[i] - ticks[i - 1];
         }
         for (unsigned i = 1; i < ng; i++) {
            const uint64_t v = gaps[i];
            unsigned j = i;
            while (j > 0 && gaps[j - 1] > v) {
               gaps[j] = gaps[j - 1];
               j--;
            }
            gaps[j] = v;
         }
         if (ng) {
            const double p50 = (double)gaps[ng / 2] * xlat.ns_per_tick / 1000000.0;
            const double pmax = (double)gaps[ng - 1] * xlat.ns_per_tick / 1000000.0;
            fprintf(stderr,
                    "radv: xclipse: retire_gpu during_wait gaps=%u p50_ms=%.2f max_ms=%.2f span_ms=%.1f "
                    "waited_end_at_wait_start=%u of %u (gpu clock)\n",
                    ng, p50, pmax, (double)(ticks[nt - 1] - ticks[0]) * xlat.ns_per_tick / 1000000.0,
                    waited_end_before_wait, waited_recs);
         }
      }
   }

   /* The submission this fence belongs to, resolved from the submit-side ring: its own GPU span says
    * whether the time went into this submission (start to end large) or before it started or after it
    * finished. A span is a difference of two GPU timestamps, never of a GPU and a host one. */
   int64_t span_ms = -1;
   unsigned span_recs = 0, span_missing_end = 0;
   if (waited_submit) {
      int64_t lo = 0, hi = 0;
      bool have = false;
      for (unsigned r = 0; r < XLAT_REC_MAX; r++) {
         const struct xlat_rec *rec = &xlat_recs[r];
         if (!rec->rec || rec->submit != waited_submit)
            continue;
         span_recs++;
         const uint64_t start_ticks = xlat_seen[(unsigned)(rec->rec % XLAT_REC_MAX) * 2];
         const uint64_t end_ticks = xlat_seen[(unsigned)(rec->rec % XLAT_REC_MAX) * 2 + 1];
         if (!end_ticks) {
            span_missing_end++;
            continue;
         }
         if (!start_ticks || end_ticks < start_ticks)
            continue;
         if (!have || (int64_t)start_ticks < lo)
            lo = (int64_t)start_ticks;
         if (!have || (int64_t)end_ticks > hi)
            hi = (int64_t)end_ticks;
         have = true;
      }
      if (have)
         span_ms = (int64_t)((double)(hi - lo) * xlat.ns_per_tick / 1000000.0);
   }

   const uint64_t now = os_time_get_nano();
   const int64_t host_adv_ms = (int64_t)((now - xlat_wait.host_ns) / 1000000ull);
   const uint64_t now_cpu = xclipse_proc_cpu_ns();
   const uint64_t proc_delta =
      (now_cpu && xlat_wait.proc_ns && now_cpu > xlat_wait.proc_ns) ? now_cpu - xlat_wait.proc_ns : 0;
   const int64_t proc_cpu_ms = proc_delta ? (int64_t)(proc_delta / 1000000ull) : -1;

   struct xlat_top top[5];
   uint64_t sum = 0;
   unsigned n = 0;
   int acct = -1;
   if (xclipse_snapshot_threads(&xlat_wait_now)) {
      n = xclipse_top_deltas(&xlat_wait_pre, &xlat_wait_now, proc_delta, top, 5, &sum);
      if (proc_delta)
         acct = (int)(sum * 100ull / proc_delta);
   }
   char list[320];
   xclipse_format_top(list, sizeof(list), top, n);

   /* One line, one wait. cbs_retired counts slots that changed while this thread was blocked, which
    * is evidence of GPU progress, not proof of idleness when it is zero: uninstrumented command
    * buffers and overwritten records are reported alongside so a zero can be classified. */
   fprintf(stderr,
           "radv: xclipse: latency wait_ms=%llu waited_submit=%llu waited_span_ms=%lld waited_end_missing=%u of %u "
           "cbs_retired=%u[%s] unattributed=%u coverage{instr=%llu uninstr=%llu unrecorded=%llu reused=%llu wraps=%llu} "
           "host_advanced_ms=%lld proc_cpu_ms=%lld acct=%d%% threads=%u%s ioctl_ticks=%llu top5=[%s]\n",
           (unsigned long long)(wait_ns / 1000000ull), (unsigned long long)waited_submit, (long long)span_ms,
           span_missing_end, span_recs, retired, ids, unattributed, (unsigned long long)xlat_rec_next,
           (unsigned long long)xlat_uninstrumented, (unsigned long long)xlat_unrecorded,
           (unsigned long long)xlat_reused, (unsigned long long)xlat_wraps, (long long)host_adv_ms,
           (long long)proc_cpu_ms, acct, xlat_wait_now.n, xlat_wait_now.truncated ? "(truncated)" : "",
           (unsigned long long)ioctl_ticks, list);

   atomic_store(&xlat_wait_active, false);
}

void
radv_xclipse_native_trace_fence_wait(const void *active_sync, uint32_t fence_count, uint32_t wait_all, int result,
                                     uint64_t wait_ns)
{
   radv_xclipse_phase_wait(wait_ns);

   uint64_t waited_submit = 0;
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
      /* submits_since_last_wait is the frame's submission count when one long wait closes a frame: it is
       * the number that has to fall if the per-submission cost is what sets the frame time. */
      fprintf(stderr, "radv: xclipse: fence wait_ms=%llu sync=%s submit_age_ms=%llu seq=%llu queues=%u wait_all=%u "
                      "result=%d total_submits=%llu submits_since_last_wait=%llu\n",
              (unsigned long long)(wait_ns / 1000000ull), found ? "tracked" : "untracked",
              (unsigned long long)age_ms, (unsigned long long)seq, fence_count, wait_all, result,
              (unsigned long long)xd_seq, (unsigned long long)(xd_seq - xd_subs_at_last_wait));
      xd_subs_at_last_wait = xd_seq;
      if (found)
         waited_submit = seq;
   }
   /* After resolving the submit, so the report can say what that submission's own GPU span was. */
   radv_xclipse_latency_wait(wait_ns, waited_submit);

   if (!trace_enabled() || wait_ns < 100000000ull)
      return;

   RADV_XCLIPSE_MESH_DIAG(3, "TRACE fence wait_ms=%llu fences=%u wait_all=%u result=%d\n",
                          (unsigned long long)(wait_ns / 1000000ull), fence_count, wait_all, result);
}
