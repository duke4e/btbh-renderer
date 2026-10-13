# FSR 3.1 upscaler shaders

AMD FidelityFX Super Resolution 3.1 temporal upscaler (no frame generation), driven by the renderer as its
own SDL GPU compute passes (`src/gpu/btbh_gpu_fsr3.c`, `UpscaleMode.fsr3`). The FidelityFX SDK itself needs
native D3D12 or Vulkan command lists, which SDL GPU does not expose, so only its HLSL is used.

## Provenance

- Source: <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>, tag `v2.0.0` (FSR upscaler 3.1.5),
  fetched 2026-10-13.
- Licence: MIT, `LICENSE.md` (Copyright (c) 2025 Advanced Micro Devices, Inc.).
- `ffx/`: `Kits/FidelityFX/api/internal/gpu/ffx_*.h` (core HLSL helpers), byte for byte.
- `spd/ffx_spd.h`: `Kits/FidelityFX/upscalers/fsr3/include/gpu/spd/ffx_spd.h`, byte for byte.
- `fsr3upscaler/`: `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/*.h` except the callbacks file,
  byte for byte apart from three include paths:
  - `ffx_fsr3upscaler_luma_pyramid.h`, `ffx_fsr3upscaler_shading_change_pyramid.h`:
    `"spd/ffx_spd.h"` became `"../spd/ffx_spd.h"`;
  - `ffx_fsr3upscaler_rcas.h`: `"../../../../../api/internal/gpu/ffx_core.h"` became `"../ffx/ffx_core.h"`
    (the RCAS pass is not compiled: the renderer sharpens with its own FSR 1 RCAS pass).
- `btbh_fsr3_callbacks_hlsl.h`: generated from AMD's `ffx_fsr3upscaler_callbacks_hlsl.h` with SDL GPU's
  compute bindings: read-only textures `t<n>` in `space0`, each with its own sampler `s<n>` (AMD's two shared
  samplers became one per texture, which SDL binds as pairs), read-write textures `u<n>` in `space1` with
  their storage format (`[[vk::image_format]]`), uniform buffers `b<n>` in `space2`. One addition: a branch
  that declares only mip 5 of the downsampler's pyramid, which is all the luma pyramid pass stores and
  reloads (all six mips took it past the 8 read-write textures SDL GPU binds per compute pass).
- `btbh_fsr3_options.hlsli`: the permutation (AMD's 32-bit wave32 variant, LDR input, render-resolution
  motion vectors without jitter, standard depth, reference reprojection, no built-in sharpening); SPD uses
  group-shared memory instead of wave operations because SDL_shadercross compiles compute as `cs_6_0`.
- `../production_fsr3_*.comp.hlsl`: the pass entry points, AMD's `ffx_fsr3upscaler_<pass>_pass.hlsl` with
  the bindings renumbered without gaps in the order `btbh_gpu_fsr3.c` binds them. `production_fsr3_clear`
  (R32_UINT fill) and `production_fsr3_motion_view` (debug views) are the renderer's own.

`python tools/fsr3/vendor_fsr3.py <FidelityFX-SDK checkout>` regenerates everything above from a checkout of
the tag (the two debug/clear passes excepted).

## Debugging

- `BTBH_DEBUG_MOTION_VECTORS=1`: the frame under its motion vectors (hue the direction, brightness the speed)
  and the reactive mask (red). A held camera leaves the static city dark.
- `BTBH_DEBUG_MOTION_VECTORS=2` with `BTBH_FSR3_JITTER=off`: each pixel's difference from the previous frame
  where its motion vector points; black where the motion vectors are right (aliased edges stay lit).
- `BTBH_FSR3_DEBUG_VIEW=1`: AMD's debug view (motion vectors, locks, depth; disocclusion, shading change,
  reactivity).

## Updating

Check the SDK's newest 3.1.x upscaler, rerun the script against it, rebuild the shaders and compare the
pass sequence in `Kits/FidelityFX/upscalers/fsr3/internal/ffx_fsr3upscaler.cpp` (`fsr3upscalerDispatch`,
resource formats in `getInternalResourceDescriptions`, the constants struct) with `btbh_gpu_fsr3.c`.
