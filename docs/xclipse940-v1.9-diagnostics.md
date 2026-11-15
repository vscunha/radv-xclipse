# Xclipse 940 v1.9 task/mesh diagnostics

This branch is **v1.9.0 (`cc87fdf`) plus opt-in diagnostics and the direct
mesh-only fix**. The fix routes `vkCmdDrawMeshTasksEXT` with `emul=1` and no
task shader through the existing conversion + `DRAW_INDIRECT_MULTI` path; it no
longer emits the firmware `DISPATCH_MESH_DIRECT` packet that the code documents
as hanging. The diagnostics still do not change the command stream for other
paths. The branch is meant to find out which v1.9 task/mesh path breaks
emulated rendering.

## Enable

Set one environment variable in the BananaNative per-title environment:

```sh
RADV_XCLIPSE_MESH_DIAG=1
```

Level 1 prints device/ring/pipeline information, **sampled** application-draw
summaries, and rate-limited warnings. Level 2 also prints **sampled** ring
passes, indirect conversions, and the `DRAW_INDIRECT_MULTI` arguments. Level 3
also enables the bounded native-path readback described below: the first eight
Android frames of a run of consecutive slow frames dump the last
task count/draw-ring/payload words, the last conversion record, the last submit
metadata, and slow `vkWaitForFences` calls. A single isolated slow frame is
skipped and a fast frame starts a new run.

Sampling is per recording thread: the first 256 records, then every 1024th
record. The level-3 task/conversion/submit snapshot is shared across the
recording, submit and present threads; the last snapshot is copied and cleared
at the frame boundary. Pipeline changes are logged for the first 32 task/mesh pair changes per
recording thread, including repeats (a further-change note follows). Several warning classes log at most eight
occurrences; a few one-shot warnings log once. A clean log is therefore not a
complete trace.

The switch is excluded from the shader-cache knob hash, so enabling it does not
invalidate cached shaders. Filter the native log for `[X940-19]`.

The GitHub Actions artifact is a wrapper, not the installable driver. Import
the `packages/*.zip` file inside it; that package has exactly `meta.json`,
`vulkan.radeon.so`, and `NOTICE.txt` at its root. Do not import the Actions
artifact ZIP itself.

## Lines to look for

- `DEV emul=... entries=... max=...` — task draw ring size. `max` is one less
  than `entries`; the last entry holds the indirect count.
- `RING ... draw_va=... payload_va=...` — the device-wide task ring.
- `PIPE task=... wg=... wave=...` / `PIPE mesh=... wg=... has_task=...` — the
  task and mesh shader local sizes and whether the mesh shader was compiled with
  a task shader.
- `DRAW direct path=...` — `task-emul`, `task-native`, `mesh-emul-direct`, or
  `mesh-native-direct`. `mesh-emul-direct` is the fixed direct mesh-only path
  (converted to `DRAW_INDIRECT_MULTI`); `mesh-native-direct` is the native
  firmware packet used only when the emulation is disabled. The pipeline line is
  emitted for both paths.
- `TASK draw x=... y=... z=... product=...` for a direct task draw, or
  `TASK indirect count=... stride=...` for an indirect task draw. The indirect
  count is the number of task-draw commands, not the application workgroup grid.
- `TASK pass ... first=... n=... ring_range=[0,n) ...` — the compute dispatch
  used to run the task shader and the **usable** ring records it may write.
  An x-only direct draw is split into passes that all reuse `[0,n)`. An
  indirect pass has a CPU placeholder `n=0` and logs
  `ring_range=[0,max) (GPU-defined, max)` because the actual grid and count are
  read by the GPU.
- `MESH indirect data=... count=... count_va=...` and
  `PKT DRAW_INDIRECT_MULTI ...` — the mesh draw that consumes the ring.
- `CONV` / `CONV-CS` — conversion of application mesh-task commands to the
  16-byte draw records.

## High-signal warnings

- `task direct multidimensional grid ... exceeds usable ring records=...` — the
  grid has more workgroups than the `num_entries - 1` usable records. v1.9
  dispatches the whole grid anyway. `task_ring_entry_index` masks the ring index
  with `num_entries - 1`, so excess workgroups reach the reserved count entry,
  or wrap and overwrite earlier draw/payload records. Only the first
  `num_entries - 1` records are consumed; output is lost or corrupted. This is
  a correctness bug independent of the GPU.
- `task indirect drawCount=... is the declared maximum and exceeds the
  emulation cap=64` or `mesh indirect drawCount=...` with cap 16384 — the API
  supplied a maximum count above the emulation cap. At most 64/16384 draws are
  processed. This warning does **not** prove that the actual GPU-written count
  exceeded the cap, or that draws were dropped; the actual count is not visible
  on the CPU.
- `MESH direct path uses the native DISPATCH_MESH_DIRECT packet` — only when
  the emulation is disabled (or an old driver is in use); with emulation enabled
  the direct path now logs `MESH direct path uses the converted
  DRAW_INDIRECT_MULTI emulation`.
- `task direct pass ... n=0` — the CPU could not select any ring record for a
  direct pass.

The packed `x | y << 16` mesh grid emitted by a task shader is **not** inspected
by these CPU-side checks; it is produced in the shader and can differ from the
application task grid. Indirect grids and task-emitted mesh grids are likewise
not inspected.

## Level 3 native-path readback

Level 3 is a targeted trace for a clean level-1/2 log. At the Android frame
boundary (`QueueSignalReleaseImageANDROID`) it reports the first eight frames
of a run of consecutive frames that took at least 100 ms. A single isolated
slow frame is skipped, and a fast frame starts a new run. For each reported
frame it dumps:

- `TRACE task ...` — the last task pass: grid, first/n, count-entry offset and
  the GPU-written count, draw-ring offset, the first 8 draw records (32 words),
  and the first 8 payload words. For a direct task draw there is no
  GPU-written count entry; the CPU workgroup count (`n`) is reported instead.
- `TRACE convert ...` — the last indirect conversion: source/destination,
  stride, and max count. The converted records themselves are not read back:
  the conversion writes into the command buffer's upload BO, whose CPU mapping
  the command buffer owns and writes through (`cmd_buffer->upload.map`).
  `buffer_map` hands back that same mapping, so `buffer_unmap` would munmap it
  and leave the command buffer writing into an unmapped range on the next
  frame — a SIGSEGV at a page boundary (this is what the first level-3 build
  did at the first slow frame). The record contents are therefore left to the
  `CONV` line at record time.
- `TRACE submit ...` — the last submit's IP type, ring, IB count, BO-list count,
  sequence number, user-fence flag, and ioctl result.
- `TRACE fence wait_ms=...` — a `vkWaitForFences` call that blocked for at least
  100 ms.

The readback is not a full BO dump. It is limited to the first eight frames of
a slow run and the first eight draw/payload records; a normal 30 FPS run
produces no `TRACE` lines. Only the device-wide task ring BO is mapped (and
unmapped again) for the readback; no other buffer's mapping is touched. The
values are read after the frame boundary, so they describe the last task
pass/conversion/submit of that frame, not an arbitrary earlier draw.

## What this cannot tell you

The level-1/2 log alone does not read back the GPU-written ring records or the
task payload. A clean level-1/2 log only rules out the CPU-side cases printed
above, within the sampling and warning limits. Level 3 adds the bounded readback
above, but it still cannot prove how the GPU executed a record, and it does not
inspect a full payload or every draw in the frame.
