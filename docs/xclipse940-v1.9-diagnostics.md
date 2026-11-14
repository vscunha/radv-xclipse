# Xclipse 940 v1.9 task/mesh diagnostics

This branch is **v1.9.0 (`cc87fdf`) plus opt-in diagnostics**. It does not
change the command stream, the emulation decision, or any shader. It is meant to
find out which v1.9 task/mesh path breaks emulated rendering.

## Enable

Set one environment variable in the BananaNative per-title environment:

```sh
RADV_XCLIPSE_MESH_DIAG=1
```

Level 1 prints device/ring/pipeline information, **sampled** application-draw
summaries, and rate-limited warnings. Level 2 also prints **sampled** ring
passes, indirect conversions, and the `DRAW_INDIRECT_MULTI` arguments.

Sampling is per recording thread: the first 256 records, then every 1024th
record. Pipeline changes are logged for the first 32 task/mesh pair changes per
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
- `DRAW direct path=...` — `task-emul`, `task-native`, or `mesh-native-direct`.
  `mesh-native-direct` means a mesh-only direct draw still used the firmware
  `DISPATCH_MESH_DIRECT` packet; it is not the v1.9 emulation. The pipeline
  line is emitted for this path too.
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
- `MESH direct path uses the native DISPATCH_MESH_DIRECT packet` — if rendering
  breaks only with mesh-only direct draws, this path is the suspect.
- `task direct pass ... n=0` — the CPU could not select any ring record for a
  direct pass.

The packed `x | y << 16` mesh grid emitted by a task shader is **not** inspected
by these CPU-side checks; it is produced in the shader and can differ from the
application task grid. Indirect grids and task-emitted mesh grids are likewise
not inspected.

## What this cannot tell you

The log does not read back the GPU-written ring records or the task payload.
A clean log only rules out the CPU-side cases printed above, within the sampling
and warning limits. If the log is clean, the next step is a GPU-side readback of
the ring and payload, not another build with more CPU logging.
