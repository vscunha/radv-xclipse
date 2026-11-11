# A1: direct task/mesh recording versus kernel gang-submit acceptance

**Diagnostic experiment, not a fix. Device result: pending.**

## Baseline gate completed

[Actions run 43114700647](https://github.com/vscunha/radv-xclipse/actions/runs/43114700647)
built pristine `fe9b8943aaeec11ed9fccbfcc382da07379afbe5` successfully.
Workflow commit: `f1d0c908f1f17545348255dd5b2f35d00efeba8d`.

- GitHub-hosted `ubuntu-24.04`, x86-64, image `20261101.360.1`.
- NDK `29.0.14206865` / r29; Android Clang 21.0.0, build 13989888,
  LLVM revision `5e96669f06077099aa41290cdb4c5e6fa0f59349`.
- Meson 1.9.1; actual build Ninja `/usr/bin/ninja` 1.11.1 (the original
  provenance also prints PATH's Ninja 1.13.2; the build wrapper uses `/usr/bin`).
- glslang 15.1.0; Python 3.12.16; Mako 1.3.10, packaging 25.0.
- Job: 3m54s. Build/verification: 198s; upstream build.sh: 191.36s.
  `/usr/bin/time` maximum child RSS: 651,848 KiB (not total job peak memory).
- Sampled runner filesystem used bytes: 63,430,541,312 to 63,845,163,008.
  This is filesystem use during sampling, not an exact process disk peak.
- Installable ZIP: `radv-xclipse-baseline-43114700647-1-fe9b894.zip`.
  SHA-256: `4a370adc1d2c2185b91399829b798ed61ef8900cab1366ff9f1e0caf2ee107ae`.
- Whole Actions artifact: 21,947,360 bytes (includes logs, comparison and
  source-produced package, not just the installable ZIP).
- Library: 17,991,688 bytes, versus official 17,991,880. Not byte-identical.
  Source-produced metadata matches official exactly. AArch64 ET_DYN, Android
  API 34/r29 note, 16-KiB LOAD alignment, stripped sections, loader exports,
  license notices and dependency set passed comparison. Dependencies:
  `libhardware.so`, `liblog.so`, `libnativewindow.so`, `libsync.so`, `libm.so`,
  `libdl.so`, `libc.so`; no host GLIBC dependency.
- Warnings: deprecated cross-file `cpp_link_args` placement, cross-compile TLS
  dialect autodetection, and two existing shadowed variables in radv_queue.c.
- First orchestration attempt failed because `sdkmanager` was absent from
  PATH. Explicit SDK-tool installation fixed it. No baseline driver patches.

These are build/static-verification results, **not a device-validation claim**.

## Source investigation

1. The exact upstream guard/removal commit is
   [5d95a8f141d8](https://github.com/chaotic-cx/mesa-mirror/commit/5d95a8f141d83a9a429262f9f692363d19e56ef9),
   Mesa [MR 44149](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44149),
   dated 2026-09-07. It simultaneously removes the indirect-count workaround
   and disables task/mesh on the affected old MEC firmware. It does not
   establish that *all direct dispatches* are inherently broken there.
2. The Xclipse 1.9 change removes that capability guard and reintroduces the
   indirect-count workaround. The E4a direct `13,1,1` reproduction does not
   exercise that conditional-count workaround.
3. Direct API path in `fe9b894`: `radv_CmdDrawMeshTasksEXT` ->
   `radv_before_taskmesh_draw` (shader state, descriptors/constants, cache flush
   and gang synchronization) -> `radv_emit_direct_taskmesh_draw_packets` ->
   `DISPATCH_TASKMESH_DIRECT_ACE` plus `DISPATCH_TASKMESH_GFX`.
4. Direct ACE packet is six DWORDs: header, X/Y/Z, dispatch initiator, 16-bit
   task ring user-SGPR index. Its shape matches AMD PAL's public
   [packet definition](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h).
   Matching the generic AMD layout is not a firmware-compatibility proof.
5. GFX and ACE share a task-control BO (write/read/dealloc pointers), a draw
   ring and payload ring. The draw-ring ready store follows release ordering
   in `ac_nir_lower_taskmesh_io_to_mem.c`. Ring descriptors already use the
   Xclipse `desc_gfx_level`; a blanket "switch descriptors to GFX11" is not
   a justified fix. CP encoding remains GFX10.3 while shader ISA is GFX11.
6. Command-buffer leader/follower semaphores, queue gang pre/postambles and
   `DISPATCH_TASK_STATE_INIT` are present. Queue submission combines the ACE
   follower and GFX leader IBs into one `ac_drm_cs_submit_raw2` request.
7. A public Exynos 2400 kernel snapshot has a highly relevant restriction:
   [`amdgpu_cs_ib_fill`](https://github.com/AhmetTheDev0725/android_kernel_samsung_s5e9945/blob/46da7d8bd06f162b3fb8deadfa999c39bdef769b/drivers/gpu/drm/samsung/gpu/sgpu/amdgpu_cs.c#L1076)
   returns `-EINVAL` when IBs in one request map to different scheduler
   entities. That snapshot is from 2025; it is **not confirmed to match the
   phone's Android 17 kernel**. This is a concrete hypothesis, not a root-cause
   declaration. An `-EINVAL` return alone is not uniquely diagnostic.
8. Upstream also has mesh-state fixes such as
   [48da0f3a9bd5](https://github.com/chaotic-cx/mesa-mirror/commit/48da0f3a9bd556e98c1f92af7a7c852fcf75b6b6)
   (stale vertex SGPR state) and
   [14337da451d1](https://github.com/chaotic-cx/mesa-mirror/commit/14337da451d103c97dbc0d6b915648de41f50e64)
   (indirect mesh user data before barriers). They are not bundled into A1:
   neither is established as the cause of E4a's direct task+mesh failure.

## Ranked hypotheses and predictions

1. **Kernel gang incompatibility:** first mixed ACE/GFX request returns
   `-EINVAL` immediately. A1 will identify each IB's engine/ring/flags and
   the exact ioctl result. Confirming the phone kernel's matching rejection
   path still requires kernel build/source or kernel diagnostics.
2. **Accepted submission, execution/synchronization failure:** gang ioctl
   succeeds, but execution later stalls or loses the device. This points the
   next experiment at CP task-state/packets or ACE/GFX scheduling rather than
   treating an upload's later device-lost return as the original fault.
3. **Task/mesh shader resources or payload ABI:** acceptance succeeds and
   the recording shows problematic local sizes, waves, LDS/scratch or ring
   setup. Further GPU/synthetic evidence would be required; CPU metadata
   cannot establish shader execution or memory visibility.

## Exact A1 delta

`RADV_XCLIPSE_MESH_DIAG=1`, default off and restricted to Xclipse 940:

- `radv_xclipse_mesh_diag.h`: cached diagnostic enable flag.
- `radv_cmd_buffer.c`: first 16 direct task+mesh recordings report dimensions,
  local sizes/waves, resource counts, packet header/initiator, ring SGPR
  indices and shared-ring layout. Prefix `[X940-MESH-A]`.
- `winsys/amdgpu/radv_amdgpu_cs.c`: first four submits, first 16 subsequently
  encountered mixed GFX/compute submits, and error returns report IB
  engines/rings/sizes/flags, queue counts, ioctl return and accepted sequence.
  At most 32 IB metadata lines per reported submit.
- No shader code, BO contents, GPU addresses or handles are dumped.
- No packet, feature bit, cache flush, semaphore, barrier, memory allocation,
  submission ordering or error-handling behavior is changed. Logging can
  perturb CPU timing; an apparent recovery is not a fix or acceptance.

## Owner-operated device test

No device operation has been performed by this work. Before a run:

1. Preserve/selectable official 1.8 and official 1.9, existing settings,
   caches, packs and saves. Do not clear caches or reboot for this experiment.
2. Install/select the specifically labeled **A1 diagnostic** driver through
   Banana Native. Confirm its loaded source SHA in the app log; a ZIP's name
   alone is not enough.
3. Reuse E4a's saved 28-key skip list and other settings unchanged. Keep mesh
   enabled (`DM_VK_MESH=1`), and add `RADV_XCLIPSE_MESH_DIAG=1` plus
   `DM_VK_MESH_LOG=1` if not already set. No `RADV_DEBUG=hang` is needed.
4. Enter gameplay once. On device loss/hang, stop and collect the app log,
   corresponding logcat, driver/load lines and `[X940-MESH-A]` lines. Also
   collect read-only `uname -a` and `/proc/version` to identify the kernel.
   Do not retry before inspecting the result.
5. Restore only the added diagnostic env values and driver selection to their
   recorded previous state. No Android diagnostic properties need changing.

A1's deliverable is the acceptance/rejection chronology, **not a stable-gameplay
claim**. If mixed submissions are rejected, do not simply split ACE and GFX
into ordered CPU submits: the task path needs concurrent progress and its
cross-engine waits, BO dependencies and multi-process scheduling must remain
correct. If accepted, use a separate packet/state/synchronization experiment
with a source-supported delta. Do not mix that with indirect-zero changes.

A future fix must first pass E4a, then run the full scene **without the skip
list or mesh disabling**, for at least five minutes with door/vegetation,
rendering, frame times, memory and rollback verification recorded.
