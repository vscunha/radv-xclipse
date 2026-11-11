# Xclipse 940 task/mesh development

Experimental branch: `fix/xclipse940-mesh` in `vscunha/radv-xclipse`.
Upstream: <https://github.com/JimVulkan/radv-xclipse>, issue [#23](https://github.com/JimVulkan/radv-xclipse/issues/23).
No game binaries, shaders, firmware, private device logs, or credentials are included.

## Objective and acceptance

Execute task+mesh draws on Xclipse 940 without device loss, while keeping
`VK_EXT_mesh_shader` enabled. Acceptance requires real Galaxy S24+ testing:
pass the previous failing draw/frame, at least five minutes of gameplay,
correct house-door/vegetation rendering, and recorded performance/memory.
A successful build or driver load does **not** satisfy this.

## Existing evidence (user-supplied; not repeated)

- 1.8 / `269238e`: 3,300 presents, 24–29 fps; mesh extension unavailable.
- 1.9 RC1 / `fe9b894`: device loss with mesh enabled; `DM_VK_MESH=0`
  survives 3,150 frames at 26.9 fps. Disabling depth-flush changes did not help.
- E4a leaves one direct task+mesh draw active (13 workgroups), and still
  loses the device around frame 968. An indirect-count-zero explanation
  alone therefore cannot explain the failure.
- Other skip experiments also failed. CPU observations of indirect argument
  buffers are **not** proof of GPU-executed zero workgroups without a
  synchronization guarantee. Pipeline creation is not proof of execution.
- The reported CS `-EINVAL`/device-loss and failed GPUVM query do not identify
  the offending packet or prove a particular upload batch caused the fault.

## First gate: source-identical baseline

The Actions workflow checks out **pristine**
`fe9b8943aaeec11ed9fccbfcc382da07379afbe5` separately from its CI orchestration.
It calls that revision's unchanged `build.sh` on Ubuntu 24.04 x86-64 with NDK
r29 (`29.0.14206865`), Meson 1.9.1 and the repository's checksum-pinned wraps.
Only AMD Vulkan is built; no Gallium, other Vulkan drivers, test suite or
unrelated tools. Three Ninja jobs; 45-minute timeout; same-branch builds do
not overlap. Only verified wrap downloads are cached, not an NDK or build tree.

Outputs have a seven-day Actions retention and include:

- source-produced ZIP plus distinctly labeled installable test ZIP;
- SHA-256 sums and source/workflow SHA/build ID;
- ELF AArch64/ET_DYN, Android API/NDK note, 16-KiB alignment, loader exports,
  stripped sections, Android dependency and official-release comparisons;
- exact tool versions, build log/warnings, elapsed time and sampled disk use.

The label changes ZIP metadata only, never the driver binary. Byte-identical
output to the official release is measured, not promised (toolchain, build
host and build paths can affect it). Static checks cannot establish runtime
compatibility or GPU correctness.

## Official rollback packages

Keep these official non-Wayland ZIPs separately; never replace them with a test build:

| release | SHA-256 |
| --- | --- |
| [1.8](https://github.com/JimVulkan/radv-xclipse/releases/download/v1.8.0/radv-xclipse-27.0.0-devel-269238e.zip) | `0545b1b24817cc0ed4a9e4fc440a7080cbaf69a9ce5d1e4bb22b0e697129c223` |
| [1.9 RC1](https://github.com/JimVulkan/radv-xclipse/releases/download/v1.9.0-RC1/radv-xclipse-27.0.0-devel-fe9b894.zip) | `9325769617812ddca2051c40364e33a2d85c6b3fa8c6c3e6208eb251549d4745` |

## Device protocol

Explain each hypothesis and code delta before asking the owner to select a
new driver. Confirm the loaded source/build ID, preserve the prior settings,
coordinate manual reproduction and collect logs. Stop after a hang and
inspect it before retrying. No silent install/reboot/cache deletion/save
changes. Restore any changed diagnostic properties afterward.

## Status

Baseline workflow prepared; build verification and device acceptance pending.
No driver source modifications yet. No fix or root cause is claimed.
