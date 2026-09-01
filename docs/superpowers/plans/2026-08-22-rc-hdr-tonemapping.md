# RC HDR Tone Mapping Plan

Status: implemented and verified

Date: 2026-08-22

## Steps

1. Add the PBR Neutral tone-map mode to the public Swift descriptor, GPU probe,
   native scene-post ABI, validation, and post uniform without changing the
   existing Reinhard numeric value.
2. Add a shared Khronos PBR Neutral HLSL implementation and route the LUT,
   standard output, and fused tilt-shift output through it. Tone-map before
   the LUT clamp and suppress the second mapping at final output.
3. Add focused API, math, shader-source, ABI, and projection regressions, then
   select PBR Neutral only for RC-enabled gameplay environments.
4. Compile shaders, run renderer verification, synchronize the renderer vendor
   checkout, build the release game, and produce a deterministic HRC High
   capture with clipping and frame-time comparison against the current
   reference.

## Invariants and risk

- RC-Off remains on the legacy Reinhard path.
- The gameplay light reference energy remains 30; this milestone compresses
  display highlights rather than weakening scene radiance.
- PBR Neutral is applied once per pixel and only after clamping negative
  post-filter ringing to its documented non-negative input domain.
- The main visual risk is excess highlight desaturation. The A/B check must
  inspect saturated XP, shrine, projectile-hit, player-cone, and sky colors as
  well as aggregate white-pixel statistics.

## Verification results

- Renderer tonemapping tests: 5 passed.
- Full default renderer suite: 85 XCTest tests and 1 Swift Testing test passed.
- Production LUT, post, and fused tilt-shift shaders compiled to SPIR-V and MSL;
  the full production shader target and native phase-2 shader test passed.
- The game release build passed with the canonical renderer synchronized into
  `Vendor/BTBHRenderer`. The focused game test target also compiled and linked;
  direct execution remains blocked by the existing Swift 6.2 Windows XCTest
  package-enumeration runaway.
- Deterministic HRC High capture at 1920x1080, seed 424242, reference energy 30:
  all-RGB-white coverage fell from 10.1673% to 0.0085%, while colorful
  highlight coverage rose from 22.6607% to 32.4668%.
- Two HRC capture runs measured 4.3054 ms and 4.3001 ms median frame time
  (about 232-233 FPS). The prior 4.1704 ms reference had 7 captured projectiles;
  the new capture had 101, so it is not a strict tonemapper performance A/B.
