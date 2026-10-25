# ios-sailfish GPU path: notes for a Mali-G72/Panfrost port to the Galaxy S9

Source: /tmp/claude-0/sf (one squashed commit, so there's no history). I rebuilt the final kernel files by applying every patch, filtered to the GPU, surface and framebuffer files, into /tmp/claude-0/sfsrc (bsd/dev/sweet_gpu.{c,h}, sweet_gpu_shaders.h, iokit/Kernel/arm/Sweet{GPU,Surface,Framebuffer}.cpp). The NOTES.md and research/ that the tools refer to are not in the repo.

## 1. Userland architecture

- **Process:** only backboardd renders on the GPU. The boot-arg `sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib` makes the kernel add that variable at exec time: `bsd/kern/kern_exec.c`, patch 0104, extended to several processes separated by `;` in 0163. So launchd.plist is not edited for this. dyld only honours DYLD_* because 0139 adds a fake "AMFI" MAC policy: syscall 0x5a returns dyld policy flags 0x7b. Boot-arg `sweetnull=backboardd` (0048) copies writes to /dev/null into the kernel log; sweetgl logs this way. Root tweaks (`tools/sweet-root-tweaks.py`) are not GPU-specific: CA_BACKDROP_SCALE=1 for backboardd, MediaToolbox StartRecorderServer=false.
- **Why CoreAnimation uses GLES and not Metal:** there's no Metal driver. QuartzCore's `CAMetalContextCreate` polls `IOServiceMatching("IOAcceleratorES")` 300 times at 100 ms. 0166 (SweetFramebuffer.cpp) publishes an empty `IOAcceleratorES` with no MetalPluginName. That ends the 30 s poll, Metal skips it, and CA falls back to its `CA::OGL::GLESContext` renderer. Boot-arg `sweetaccel=0` turns this off.
- **EAGL shim** (`tools/sweetgl/eagl.c`, `sweetgl_eagl_install`, called from a constructor in sweetgl.c that runs in every process except SpringBoard): it swizzles `EAGLContext` `initWithAPI:properties:`, `dealloc`, `API`, `setParameter:to:`, `getMacroContextPrivate`, `texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:`, `attachImage:toCoreSurface:invertedRender:`, and on the class `+setCurrentContext:` and `+currentContext`. CA makes every GL call through the GLI dispatch table it gets from `getMacroContextPrivate`, a `{rctx, disp[1036]}` block with GLEngine's layout. `gen-dispatch.py` generates that table (`out/dispatch.c`) from `ios-dispatch.txt` (slot to GL name) and Mesa's glapi bridge, with hand-written slots for the ARB object calls. IOSurfaces are imported as GEM BOs (selector 12), wrapped in a pipe_resource and bound with `_mesa_EGLImageTargetTexture2DOES` through the frontend's `get_egl_image`. Contexts are surfaceless: CA draws only into FBOs.
- **sweetgl.dylib build** (`mesa-setup.sh`, `build.sh`, `ios-cross.txt`, `mesa.patch`):
  - Mesa is an unpinned research/mesa checkout. The options used (teflon, display-info, glapi_bridge) suggest Mesa about 25.x; that's my inference, not stated anywhere.
  - Cross-compiled with meson, using the Xcode 15.4 clang and iPhoneOS 17.5 SDK, `-miphoneos-version-min=16.0`, `system=darwin subsystem=ios`.
  - Options: `-Dgallium-drivers=freedreno -Dfreedreno-kmds=msm -Dgles2=enabled -Dopengl=true -Dplatforms= -Degl=disabled -Dllvm=disabled`, static libraries.
  - The static libs are linked into one dylib together with sweetgl.c (a minimal gallium frontend modelled on Haiku's hgl; `fd_drm_screen_create_renderonly`, `st_api_create_context` for GLES 3.1), sweet_drm.c, eagl.c, layerdump.c and dispatch.c.
  - The dylib is ad-hoc signed and padded to 64 MiB, so `push.sh` can overwrite its extents in place from TWRP.
  - Environment it sets: `FD_MESA_DEBUG=sysmem,nofp16` (no GMEM tiling, because tile resolves overran IOSurfaces) and `IR3_SHADER_DEBUG=nofp16`, because CA's mediump shaders broke at fp16.
- **mesa.patch:** builds src/freedreno/drm on darwin; turns `os_mmap` in freedreno_bo.c into `sweet_drm_bo_map(fd, offset)`; forces a 64 B pitch alignment for imported linear a5xx surfaces.
- **libdrm/msm emulation** (`sweet_drm.c`, `include/xf86drm.h`):
  - The "DRM fd" is a dup of /dev/null. `drmOpenWithType("msm")`, `drmCommandWrite/WriteRead` and `drmIoctl` are implemented in user space over IOKit `SweetGPU` / `SweetGPUUserClient`.
  - GEM_NEW → selector 10, GEM_CLOSE → 11. GEM_INFO GET_OFFSET returns `handle<<32`, which mmap turns back into the kernel's user mapping; GET_IOVA returns the kernel VA (softpin).
  - GEM_SUBMIT → selector 13. Its CMD_BUFs become `{iova, dwords}` IBs plus the BO handle list. Relocs, fence fds and syncobjs are rejected.
  - WAIT_FENCE, and CPU_PREP (which then calls 16), → selector 14. GET_PARAM → 15, plus constants (GPU_ID 530, TIMESTAMP = mach_absolute_time). SUBMITQUEUE and MADVISE are stubbed.

## 2. Kernel side

- **Files:** `bsd/dev/sweet_gpu.c` (2250 lines, built up by patches 0119–0164), `sweet_gpu.h` (ABI), `iokit/Kernel/arm/SweetGPU.cpp` (user client, 0138/0141/0150/0151/0158).
- **User client selectors:**

  | Selector | Operation |
  | --- | --- |
  | 0 | map user memory |
  | 1 | map IOSurface |
  | 2 | unmap |
  | 3 | kernel-built textured quad (synchronous) |
  | 10 | BO new (out: handle, GPU VA, user address) |
  | 11 | BO free (waits for the BO's last seqno) |
  | 12 | BO import of an IOSurface |
  | 13 | submit (`{nr_ibs, nr_bos}` + ibs + handles) |
  | 14 | wait (seqno, timeout ms; taken without the client lock) |
  | 15 | info |
  | 16 | cpu_prep (cache invalidate) |

- **BOs:** `IOBufferMemoryDescriptor` (wired, zeroed). They are cacheable in the user mapping, because a write-combined mapping of managed pages froze boot 0150a (0151). Coherency is done by hand:
  - at submit, `flush_if_written()` cleans only pages whose pmap modified bit is set (flushing everything cost about 1/4 of backboardd's CPU);
  - selector 16 invalidates before CPU reads.
- **Single GPU address space** for all clients, with no isolation. Each BO is 16 KB pages that become 4 KB SMMU PTEs.
- **Fences:** a kernel ring of 4096 dwords. Each submit emits CP_INDIRECT_BUFFER_PFE for each IB, then CP_SET_RENDER_MODE, then CP_EVENT_WRITE CACHE_FLUSH_TS, which writes the seqno to a fence page. `sweet_gpu_wait` spins for 2 ms, then polls every 0.2 ms. If the fence hasn't moved for 2 s, or the ring stays full for 500 ms, the GPU is declared hung and the service is off until reboot.
- **No interrupts:** everything is polled.
- **SMMU** (`smmu_identity`, `vm_init`, 0121–0123, 0137, 0155, 0156, 0161):
  - Context bank 0 is stage 1, 4K granule, T0SZ 28. L1 1 GB blocks identity-map 2–8 GB, for kernel buffers and the panel buffers. GPU VA 8–16 GB is page-mapped through L2/L3 tables (`sweet_gpu_vm_map`, first fit from a rover).
  - Each mapping is followed by 8 padding pages (128 KB) that all point at one scratch page, because overruns hung the A530 (0156).
  - Faults are terminate-without-abort (no CFRE, 0155) and logged at submit.
  - TLBIALLNSNH plus sync on every map and unmap.
- **Power and clocks** (`gpu_thread`, 0119, 0127, 0144, 0164):
  1. PMI8994 S2 GFX rail over SPMI.
  2. MMCC GPU GDSC (CX) and AHB clock.
  3. GX GDSC, after releasing the IO clamp.
  4. gfx3d RCG = GPLL0/3; rbbmtimer; GCC bimc_gfx branches.
  5. GPMU RBCCU/SP power.
  6. Firmware: PM4 and PFP microcode from payload blobs; the zap shader loaded via a TZ SCM call (PAS id 13) so the GPU can leave secure mode (0130).
  7. Clock raised to 400 or 600 MHz (`sweetgpuclk`) with a voltage bump and an RPM VDD_MX vote.
- **Kernel compute shaders:** written in ir3 assembly (`tools/gpu-shaders/*.asm`), assembled by `tools/ir3asm` (built by `build-ir3asm.sh` from Mesa sources) into `sweet_gpu_shaders.h`. The quad kernel `sample4.asm` does the present scaling and blending.

## 3. Display path (SweetFramebuffer.cpp; 0046, 0056, 0092, 0154, 0164)

- AppleCLCD tells iOS the panel is 750×1334 (iPhone 8 @2x). backboardd alternates between two 750×1334 BGRA IOSurfaces. SwapEnd only stores the record and wakes a presenter thread, capped at 60 Hz (0056/0092/0182).
- **Present:** `gpuLayer()` runs `sweet_gpu_quad`, a bilinear COPY of each layer through the SMMU. The source surface is mapped read-only, with an LRU of 4 mappings. The destination is one of two 1080×1920×4 panel buffers at fixed PAs 0xc0800000 and 0xc1000000, which lie outside XNU's RAM and are not CPU-mapped.
- Then `sweet_fbcon_flip(pa, stride)` points the MDP VIG0 pipe at that buffer (32 bpp) and toggles the back buffer. Scale plus flip takes about 3.8 ms; one full-screen quad takes about 2 ms.
- Vsync is a 60 Hz timer notification (type 5).
- **IOSurface changes:**
  - Rows are 64-byte aligned for the GPU process (0153, 0154, 0160, 0162; CoreUI-style requests with 6 keys or fewer get the row bytes they asked for).
  - Wrap selectors 6/7 are refused (0152).
  - `IOSurfaceAddress` client memory is wired, read-only if needed (0157–0159).
  - The io map is raised to 64 MB (0165).

## 4. Reusable vs Adreno-specific

**Nearly as-is:**
- 0104/0163 sweetenv, 0139 AMFI dyld policy, 0048 /dev/null tee, 0166 IOAcceleratorES (move it out of SweetFramebuffer).
- The IOSurface patches 0152–0162, with the alignment changed from 64 to whatever Panfrost needs (64 B linear is fine).
- From SweetGPU.cpp: the client and BO bookkeeping, `boNew`, `boImport`, `flush_if_written`, cpu_prep. The Mali-G72 on Exynos 9810 is not IO-coherent, so the same CPU cache maintenance applies.
- eagl.c, gen-dispatch.py, ios-dispatch.txt and layerdump.c are GPU-agnostic. sweetgl.c only needs `fd_drm_screen_create_renderonly` swapped for `panfrost_drm_screen_create`, and its env vars replaced (`PAN_MESA_DEBUG`).
- The sysmem/fp16 and pitch lessons carry over.

**Rewrite:**
- `sweet_gpu.c` entirely: power, clocks, SMMU, ring, CP/zap firmware, ir3 kernels.
- `sweet_drm.c`, as a **panfrost** uAPI emulator:
  - CREATE_BO (size, flags → handle and `offset` = GPU VA), MMAP_BO (fake offset → user address), GET_BO_OFFSET;
  - SUBMIT (`jc` job-chain GPU VA, `requirements` FS vs vertex/tiler/compute, bo_handles, in_syncs/out_sync);
  - WAIT_BO (handle, absolute timeout);
  - GET_PARAM (GPU_PROD_ID 0x6221, SHADER_PRESENT, L2/TILER/MEM/MMU features, THREAD_*, TEXTURE_FEATURES0-3, AS_PRESENT, AFBC...);
  - MADVISE stubbed.

  Unlike freedreno's msm path, Panfrost depends on **DRM syncobjs** (`drmSyncobjCreate/Wait/Reset`, out_sync on every submit), so those libdrm calls need user-space emulation mapped onto kernel seqnos. The `os_mmap` hook moves into panfrost's kmod BO code (`src/panfrost/lib/kmod/panfrost_kmod.c`). Build Mesa with `-Dgallium-drivers=panfrost`, no kmsro.
- **Kernel Mali JM driver:**
  - Power: G3D PMU power domain and CMU_G3D clocks and voltage (ACPM/S2MPS18), then GPU-register L2 / SHADER / TILER PWRON with READY polling.
  - MMU: AS0 TRANSTAB / MEMATTR / TRANSCFG, LPAE-style tables in Mali legacy format (`ARM_MALI_LPAE` in Linux panfrost_mmu.c), `AS_COMMAND` UPDATE/FLUSH. This replaces the SMMU code; the VA allocator and the scratch-padding idea are reusable.
  - Submit: write the job chain to JS0 (fragment) or JS1 (vertex/tiler/compute) `HEAD_NEXT`, `CONFIG_NEXT`, `COMMAND_NEXT=START`. Completion by polling `JOB_IRQ_RAWSTAT`, as sailfish polls; interrupts are optional.
  - No microcode or zap shader is needed, so steps 0120, 0129 and 0130 disappear.
- **Present:** the ir3 quad has no Mali equivalent. Either use Exynos DECON/DPP hardware scaling (simplest), keep the CPU scale (our 0090), or render the scale in userland GLES.
- **Patch numbering:** sailfish 0079–0092 are Pixel patches and collide with our S9 0082–0090; only 0046/0056 are byte-identical to ours. Cherry-pick by content.

## 5. Milestones (patch order) and performance

1. 0119: power probe, ALWAYSON counter ticking.
2. 0120–0125: CP ring, MEM_WRITE test; identity SMMU on stream 0.
3. 0126–0130: 2D blit, but the GPU was stuck in secure mode until the zap shader was loaded through TZ.
4. 0131–0133: first compute shader, then a blend kernel checked against the CPU.
5. 0134–0135: benchmark. Cache maintenance 0.29 ms for 8 MB. blend at 200 MHz took 8.3 ms vs 4.6 ms on the CPU; blend4 at 300 MHz takes 1.7–1.9 ms, about 2.5× one CPU core.
6. 0136–0137: textured quad; VA space (non-contiguous memory).
7. 0138–0143: user client, quad blend modes (sweetgpu-test / sweetgpu-ca experiments offloading CA's SW `draw_polygon`; a 1 ms delay() floor was replaced by spinning).
8. 0144, 0164: 400 then 600 MHz.
9. 0149–0151: BO/submit uAPI, cached BOs: Mesa runs, and the `sweetgl_selftest` triangle passes.
10. 0152–0162: IOSurface fixes; SMMU faults non-fatal plus scratch padding.
11. 0164: GPU present, double-buffered flip; 0166: IOAcceleratorES, faster boot.

**Result:** UI at about 20–40 fps, limited by CA's CPU work, not the GPU; display up to 60 Hz; boot in about 20 s.

**Suggested S9 order:**
1. G3D power-up and GPU_ID read.
2. Mali MMU AS0 with an identity/VA map.
3. A hand-built job chain from the kernel (a WRITE_VALUE job, then a compute job).
4. User client BO/submit/wait.
5. Panfrost uAPI and syncobj emulation, plus the selftest.
6. IOAcceleratorES and the EAGL shim in backboardd.
7. Present via DECON.
