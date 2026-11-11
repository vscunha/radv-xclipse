# Newly published upstream 1.9 final: next device candidate

**Phone result pending. Do not call the objective complete.**

During this investigation upstream published
[`v1.9.0`, cc87fdf71267dd0acc3b470920a6566a1a46f0be](https://github.com/JimVulkan/radv-xclipse/releases/tag/v1.9.0)
(release timestamp 2026-11-11 22:39:02 UTC). It is distinct from 1.9 **RC1**
`fe9b894`. The author's [issue response](https://github.com/JimVulkan/radv-xclipse/issues/23#issuecomment-6809176160)
announces the fix, but that is not our phone's acceptance evidence.

## Candidate and integrity

Use the **Android** ZIP, not the glibc or Wayland assets:

- [radv-xclipse-27.0.0-devel-cc87fdf.zip](https://github.com/JimVulkan/radv-xclipse/releases/download/v1.9.0/radv-xclipse-27.0.0-devel-cc87fdf.zip)
- 4,396,585 bytes; SHA-256:
  `204e0201bc70b0476fa134ab642071e79d1a943b07af0434478179406de28a8f`.
- Metadata: packageVersion **20**, Vulkan **1.4.368**, Mesa 27.0.0-devel.
- Downloaded unchanged from upstream and statically inspected locally:
  required three ZIP entries, stripped AArch64 ET_DYN, Android API 34 / NDK
  r29 build 14206865 note, 16-KiB LOAD alignment, expected Android dependencies
  and Vulkan loader exports, no GLIBC dependency.
- Library: 18,019,048 bytes; SHA-256
  `a0e50b2fe1c2101312b0ccd94b2d8328a98959c6cf3ae6f5b2947793a3c256ea`.

This is **the author's release**, not a third Actions build or our patch.
No redundant rebuild was performed. It remains separate from both rollback
ZIPs and the source-identical baseline we built on Actions.

## Why test this before A1

Source review shows a genuinely different implementation, not mesh disabling:

- Task shaders are compiled/executed as ordinary compute work on the GFX
  stream, with their descriptor/push-constant state and task payload ring.
- Each task workgroup writes an indirect draw command; NGG mesh draws consume
  those GPU-generated commands and payload. Direct Vulkan task+mesh calls
  still execute task and mesh shader logic, without native taskmesh packets.
- Task/mesh work no longer creates an ACE gang on Xclipse. Explicit compute
  completion/cache barriers precede draw-ring and payload consumption.
- Mesh-only indirect commands are also converted to supported draw packets.
- The source comments cite independent 920 probes: task draws rejected by the
  SGPU kernel, while indirect mesh firmware packets hang even with a count of
  one. These are the author's findings, not a new test on this Xclipse 940.

This addresses the two architectural suspects more directly than a guessed
indirect-zero patch. It does **not** establish full correctness merely from
source review. The release also contains unrelated compiler, texture, SPI,
barrier and build changes: a successful integration test cannot attribute
success to one isolated hunk. Keep the fe9b894 branch/A1 for causal diagnosis
if this candidate fails; extract a cohesive backport only when useful.

## Concrete source-review caveats

Do not describe this release as complete Vulkan mesh support based on its
extension advertisement alone:

1. [`radv_xclipse_draw_mesh_tasks_indirect`](https://github.com/JimVulkan/radv-xclipse/blob/cc87fdf71267dd0acc3b470920a6566a1a46f0be/src/amd/vulkan/radv_cmd_buffer.c#L15701-L15722)
   silently caps task draw counts at **64** and mesh-only draw counts at
   **16,384**. The advertised `maxDrawIndirectCount` is `UINT32_MAX`. A valid
   65-command task call therefore reaches only 64 commands at this source
   level; a 16,385-command mesh-only call is similarly clipped. This is a
   source finding, not a claimed on-device rendering result.
2. [`radv_xclipse_emit_task_draw`](https://github.com/JimVulkan/radv-xclipse/blob/cc87fdf71267dd0acc3b470920a6566a1a46f0be/src/amd/vulkan/radv_cmd_buffer.c#L15683-L15691)
   tiles direct X-only work, but not arbitrary Y/Z grids. Other paths dispatch
   the full grid while limiting consumed ring records to `num_entries - 1`.
   Larger valid multidimensional/indirect grids need ring-capacity-safe tiling,
   not silent clipping. The advertised task total remains 4,194,304.

Neither caveat proves the E4a `13,1,1` draw will fail. They are reasons to avoid
unqualified support claims and to add synthetic boundary tests before upstream
conformance acceptance. Other untested semantics, queries and multi-queue
sharing still require review/validation.

## Coordinated next test (requires owner action)

No installation, setting change, reboot, cache/save deletion or GPU run was
performed by this investigation.

1. Preserve official 1.8 fallback, current driver/config/env and all saves.
   Install/select **1.9 final `cc87fdf`, packageVersion 20** manually. Verify the
   actual loaded driver string/source in the app log, not only the ZIP name.
2. First use the saved E4a configuration unchanged: its 28-key skip list,
   mesh enabled, one remaining direct task+mesh draw with 13 workgroups, same
   app/build/resolution/fileback/PSO settings. Keep `DM_VK_MESH_LOG=1` for
   identification. A1's `RADV_XCLIPSE_MESH_DIAG` switch does not exist here.
3. Enter gameplay once. If a GPU hang/device loss appears, stop and collect
   app/logcat plus driver identity and read-only kernel version. Inspect
   before another run. Do not fall back to disabling mesh and call it fixed.
4. If that passes, coordinate a **separate full-workload run**, removing the
   skip list, not clearing caches. Require at least five stable gameplay
   minutes beyond the old crash, correct house door and vegetation, and no
   new significant corruption or CS rejection/device loss. Record FPS/frame
   times, elapsed time/presents and process PSS/RSS; confirm 1.8 still usable.
5. Restore diagnostic env values to their prior values after collection.

**Current blocker:** physical-device execution and rendering/performance/memory
acceptance have not been measured. No fixed-driver or root-cause success is
claimed. Prefer this official candidate to spending a redundant hang on A1.
