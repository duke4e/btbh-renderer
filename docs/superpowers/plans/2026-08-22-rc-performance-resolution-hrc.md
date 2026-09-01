# RC Performance, Resolve Cache, and HRC Implementation Plan

Status: implemented and validated; HRC remains experimental

Date: 2026-08-22

Design:
`docs/superpowers/specs/2026-08-22-rc-performance-resolution-hrc-design.md`

## Goal and acceptance

Remove provably dead classic RC GPU work, reduce native-resolution material
resolve cost without lowering transport angular resolution, and add an
opt-in paper-conformant HRC backend with a 2x linear transport domain. Preserve
classic RC as the default and require fresh build, shader-reflection, unit,
capture, and performance evidence before promoting HRC.

## Implementation steps

1. **Classic dispatch pruning and capture labels**
   - Add a tested CPU decision helper in
     `src/gpu/btbh_gpu_radiance_cascades.{h,c}` for whether a raw trace is
     required under reprojected merging.
   - Dispatch only the top raw cascade in production and give trace/merge
     labels stable cascade-specific names.
   - Keep raw resources bound for the initial rollback-safe change.
   - Verify the C RC contract test, shader suite, and a deterministic before /
     after image hash or pixel comparison.

2. **Diffuse irradiance cache for classic High**
   - Add `shaders/gpu_spike/production_rc_diffuse_cache.comp.hlsl` and register
     its SPIR-V/MSL/reflection artifacts in `CMakeLists.txt` and shader tests.
   - Extend the RC runtime with the cache pipeline, 3x3-per-probe RGBA16F
     texture, allocation/release logic, and a dispatch after cascade merge.
   - Update `production_rc_resolve.comp.hlsl` so rough non-metallic pixels use
     the cache while glossy, metallic, downward-facing, and relevant debug
     modes retain the 16-direction BRDF loop.
   - Add source-contract tests for cache bin count, full-64-direction input,
     material gating, and reflection bindings.
   - Compare deterministic screenshots and record cache hit coverage and frame
     timing in the plan closeout.

3. **Backend API and HRC CPU layout contract**
   - Add classic/holographic backend enums and requested/active status to
     `include/btbh_gpu/btbh_gpu.h`, Swift render descriptors, adapters, and
     bridge tests while keeping classic as the zero/default value.
   - Add `src/gpu/btbh_gpu_holographic_radiance_cascades.{h,c}` with pure layout
     helpers for grid levels, direction counts, atlas dimensions, power-of-two
     validation, 2x scale bounds, and the 128 MiB estimate.
   - Cover valid 256-to-512 layout, maximum level, overflow, non-square input,
     texture limits, and budget rejection in the C RC test.

4. **HRC acceleration and fluence solver**
   - Add short-trace, merge-up, merge-down, finish, and opacity-aware cross
     filter compute shaders based on HRC equations 14-21 and Amitabha's
     odd/even probe mapping.
   - Implement HRC resource preparation/release and encode scheduling in the
     new runtime module: traced `T_0..T_2`, merged `T_3..T_N`, ping-pong
     `R_N..R_0`, four rotated quadrants, finish, and cross blur.
   - Reuse the existing source, directional-source, occlusion, direct-light,
     material, and environment inputs. Do not introduce an analytic-light loop
     or temporal history.
   - Add shader reflection/source conformance tests for the over operator,
     short-trace limit, merge-up parity branches, merge-down parity branches,
     four rotations, and opacity rejection.

5. **HRC material resolve, selection, and fallback**
   - Add the HRC diffuse material resolve and route runtime encoding by backend.
   - Keep authored emission, RC-native direct lighting, environment diffuse,
     and environment specular. Document and expose that local glossy HRC is not
     yet implemented.
   - On unsupported domain/resource constraints, select classic for the same
     tier, keep lighting enabled, and report the active backend/fallback reason.
   - Expose backend selection to existing runtime/debug configuration surfaces.

6. **Verification, captures, and decision**
   - Build all Windows targets and shader artifacts; run focused C and Swift RC
     tests plus the broader relevant CTest/Swift suites.
   - Capture Classic before/after, Classic cache on/off, and HRC 512 output at
     the same deterministic frame. Check RMSE, blocker/cone edges, sky colors,
     small emitters, translucent projectile shadows, and checkerboard artifacts.
   - Measure named GPU passes and median/p95 frame times. Record whether Classic
     improves and whether HRC satisfies the 10 ms RC-overhead gate.
   - Request one independent final review, fix material findings, and keep HRC
     experimental unless both quality and performance gates pass.

## Invariants and migration concerns

- Existing tier raw values and default initializers remain source-compatible.
- RC-on never invokes legacy PBR direct/ambient lighting; RC-off remains
  unchanged.
- Production source count remains absent from trace, merge, and resolve loops.
- All radiance math is linear HDR until the existing presentation/tonemap path.
- No temporal data is required for accepted HRC output.
- No automatic commits, worktrees, or unrelated worktree cleanup are part of
  this plan.

## Unresolved promotion decision

HRC is implemented and tested as experimental. Promotion to the default High
backend requires the user-approved screenshot comparison and the measured
10 ms RC-overhead gate; implementation alone does not imply promotion.

## Closeout evidence

- Windows release Vulkan runtime telemetry confirmed High tier with requested
  and active holographic backends, no fallback, and the packaged HRC shaders.
- Deterministic 1280x720 `radiance` scenario, 360 uncapped frames, one frame in
  flight: Off median/p95 2.2/2.4 ms; Classic High 16.0/19.9 ms; HRC High
  3.1/3.7 ms. Relative to Off, Classic costs 13.8 ms and HRC costs 0.9 ms.
- Deterministic 1920x1080 capture session, 120 measured frames: Classic High
  median/p95 19.892/31.819 ms; HRC High 3.506/3.724 ms.
- HRC therefore clears the performance gate, but it is not promoted. In the
  1920x1080 world ROI it is 12.842 luma values brighter on average than Classic,
  with 41.65% of pixels exceeding a 24-value maximum RGB-channel delta. The
  final-indirect comparison remains 9.079 luma values brighter on average.
  Obvious cascade-grid blockiness is gone, but white/cyan local sources are
  harder and hotter than Classic and require energy/local-reconstruction
  tuning.
- Verification: 19/19 CTest tests, 80/80 renderer XCTest tests plus the Windows
  lifecycle test, 21/21 focused game configuration/debug-menu/packaging tests,
  release game build, Vulkan bootstrap smoke, live HRC telemetry, and paired
  deterministic PNG/QOI captures.
- Named per-pass GPU timing and local glossy HRC reconstruction remain required
  before reconsidering promotion.
