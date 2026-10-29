# S9 Metal/PanVK bring-up — not installed

The full-screen OpenGL ES/Panfrost setup remains the working phone baseline.
The unscaled trial was rejected by the owner (still laggy, smaller UI) and BOOT
was restored with complete SHA256 readback verification. Do not repeat that
layout change or replace the working compositor with an untested Metal plugin.

## Pinned evidence (2026-10-29)

- iOSPlus10 `167f7d8edb5089ab5cf0daa7ffa83bdf29916f60`: Snapdragon SM8450,
  Adreno730, Turnip Vulkan. Its notes section30 reports Metal device opening and
  completed command buffers. The Metal runtime/frontend are useful references;
  Qualcomm kernel/Turnip code is not a Mali driver.
- metal2vulkan `eeb7934025c270ca2ee3ce7817c7daab84840413`: runtime requires API1.2,
  timelineSemaphore and hostQueryReset (enables.rs). A version string override
  cannot supply these semantics.
- Current S9 Mesa25.0.7 source: PanVK get_vk_version returns1.0 for arch6/G72.
  It also gates arch6 behind PAN_I_WANT_A_BROKEN_VULKAN_DRIVER. Existing sweetgl
  build disables vulkan-drivers entirely. OpenGL ES is already GPU accelerated.
- Mesa upstream `fc8ba4378f37c152e0443dadcb9384a7da0cad4a` (27.0.0-devel):
  panvk_vX_physical_device.c reports1.3 for older GPUs, timelineSemaphore and
  hostQueryReset true. No external_memory_host implementation found under
  src/panfrost/vulkan. Its Linux functionality does not establish XNU support.
- Existing SweetGPU adapter uses IOSurface import selector12, buffers10/11,
  submission13, wait14, registers15, CPU cache prep16. It supplies a fake DRM FD.
  PanVK device enumeration/open, memory mapping, sync capabilities and shared
  image handling must be adapted and verified. Old binary syncobj helpers
  ignore timeline point arguments; audit PanVK's software timeline wrapper
  before assuming native timeline behavior or changing the working GL bridge.

## Integration order

1. Build a separate current PanVK dylib for iOS arm64, keeping sweetgl untouched.
   Adapt device discovery to SweetGPU; explicitly reject unsupported ioctls.
2. Run vkprobe on the S9: identify actual Mali hardware, check required features,
   clear a64x64 image and verify its pixels. Host/CPU-driver results do not count.
3. Implement/test IOSurface host import and cache ownership; verify shared pixels
   in both directions and ordered completion across multiple submissions.
   Implementing VK_EXT_external_memory_host or an equivalent frontend resource
   adapter is needed for the referenced zero-copy Metal route.
4. Bring over the Metal frontend/runtime with Mali capability answers. Run one
   isolated Metal test process, then shader/pipeline tests and image comparison.
5. Enable per-process with a working GL fallback only after the above pass.
   Preserve full-screen presentation. Do not remove glFinish without fences.

## Diagnostic

vkprobe.c is adapted from the iOSPlus10 MIT-licensed diagnostic. Changes: PanVK
path/environment, correct ICD negotiation function type, Vulkan1.2 promotion
for timeline semaphore, required hostQueryReset, and explicitly limited PASS
wording. It does not install a driver, enable Metal or alter boot configuration.

Host build (requires Vulkan headers):

```sh
cc -O2 -Wall -Wextra -Werror vkprobe.c -ldl -o vkprobe
./vkprobe --icd /path/to/libvulkan_panfrost.so --clear
```

Future iOS build needs the iPhoneOS SDK and Vulkan headers; the Linux executable
cannot run as an iOS application. Default iOS ICD path is
/usr/local/lib/libvulkan_panfrost.dylib; override with --icd or S9_PANVK_ICD.
A missing driver exits2. A passed baseline check alone does not prove Metal or
all shaders work. The --clear flag submits work only to the explicitly loaded
Vulkan driver. Never report the host GPU's result as an S9 result.

Validation so far: host compilation with warnings-as-errors passed; absent ICD
exits2; host RADV enumerates zero devices and exits2. The standalone iOS diagnostic cross-build and macOS binary-sync tests now pass
in CI40890094548. The PanVK driver build is still in progress; no S9 Vulkan test
has passed. This is diagnostic groundwork, not GPU support.

Sources:
- https://github.com/akshaynexus/iOSPlus10/blob/167f7d8edb5089ab5cf0daa7ffa83bdf29916f60/docs/metal-design.md
- https://github.com/steelbrain/metal2vulkan
- https://docs.mesa3d.org/drivers/panfrost.html
- https://gitlab.freedesktop.org/mesa/mesa/-/commit/fc8ba4378f37c152e0443dadcb9384a7da0cad4a


## Memory correctness gate before device testing

SweetGPU selector10 maps CPU-cached memory; its GPU mappings are noncoherent.
Mesa's Linux panfrost1.1 path instead assumes uncached/coherent host memory.
The current shim still reports1.1, so its advertised HOST_COHERENT type needs
correction before this driver is installable. Do not bump the DRM version to
pretend that the missing1.6 cache-sync ioctl exists.

Selector16 flushes physical pages after checking the BO's last submission is
complete. It is a possible implementation component for PANFROST_SYNC_BO, but
range validation, concurrent CPU/GPU access, cached memory-type selection and
both CPU-to-GPU/GPU-to-CPU tests remain required. Ordinary Vulkan coherence
must be correct before adding external host-memory/IOSurface imports.

Sync-file import now rejects unknown descriptors instead of treating them as
completed; exporting an empty syncobj fails. Tests exercise generated adapter
functions, including the documented -1 already-signaled import sentinel.
These host tests do not establish GPU completion or cache coherency.
