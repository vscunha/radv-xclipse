# Xclipse 940 v1.9 task/mesh diagnostics

This branch is **v1.9.0 (`cc87fdf`) plus logging only**. It does not change the
command stream, the emulation decision, or any shader. It is meant to find out
which v1.9 task/mesh path breaks emulated rendering.

## Enable

Set one environment variable in the BananaNative per-title environment:

```sh
RADV_XCLIPSE_MESH_DIAG=1
```

Level 1 prints device/ring/pipeline information, one summary line per
application mesh draw, and every warning. Level 2 also prints every ring pass,
the indirect conversion, and the `DRAW_INDIRECT_MULTI` arguments. The switch is
excluded from the shader-cache knob hash, so enabling it does not invalidate
cached shaders.

Filter the native log for `[X940-19]`.

## Lines to look for

- `DEV emul=... entries=... max=...` — task draw ring size. `max` is one less
  than `entries`; the last entry holds the indirect count.
- `RING ... draw_va=... payload_va=...` — the device-wide task ring.
- `PIPE task=... wg=... wave=...` / `PIPE mesh=... wg=... has_task=...` — the
  task and mesh shader local sizes and whether the mesh shader was compiled with
  a task shader.
- `DRAW direct path=...` — `task-emul`, `task-native`, or `mesh-native-direct`.
  `mesh-native-direct` means a mesh-only direct draw still used the firmware
  `DISPATCH_MESH_DIRECT` packet; it is not the v1.9 emulation.
- `TASK draw x=... y=... z=... product=...` — the application grid.
- `TASK pass ... grid=... ring_entry=0-n ...` — the compute dispatch used to run
  the task shader and the ring entries it is allowed to write.
- `MESH indirect data=... count=... count_va=...` and
  `PKT DRAW_INDIRECT_MULTI ...` — the mesh draw that consumes the ring.
- `CONV` / `CONV-CS` — conversion of application mesh-task commands to the
  16-byte draw records.

## High-signal warnings

- `task direct grid ... product=... exceeds ring entries=...` — a
  multidimensional direct task grid has more workgroups than the ring has
  entries. v1.9 dispatches every workgroup anyway; workgroups past the ring
  write past the draw ring. This is a correctness bug independent of the GPU.
- `grid ... does not fit the packed x|y<<16 user SGPR` — the emulation packs x
  in the low 16 bits and y in the high 16 bits, so x or y above 65535 is
  truncated.
- `task indirect drawCount=... exceeds the emulation cap=64` or
  `mesh indirect drawCount=... exceeds the emulation cap=16384` — later draws
  are dropped.
- `MESH direct path uses the native DISPATCH_MESH_DIRECT packet` — if rendering
  breaks only with mesh-only direct draws, this path is the suspect.

## What this cannot tell you

The log does not read back the GPU-written ring records or the task payload.
A clean log only rules out the CPU-side grid/count/ring-size cases above. If the
log is clean, the next step is a GPU-side readback of the ring and payload, not
another build with more CPU logging.
