# RC Roadmap 1-7 Windows Design

Status: approved; implementation in progress

Date: 2026-08-23

## Goal

Complete the seven-item renderer roadmap previously approved by the user on
Windows. Native macOS/Metal optimization and acceptance are explicitly deferred,
but shared HLSL and public contracts must remain cross-backend compatible.

## Required outcomes

1. **Frame-time stability.** Remove avoidable RC-only command-buffer submissions,
   warm pipelines inside the existing loading window, expose named HRC pass
   evidence, and reproduce camera-origin/occlusion revision changes without a
   periodic preparation hitch.
2. **Temporal HRC reconstruction.** Add bounded history, deterministic sample
   phasing, world/screen reprojection as appropriate, and explicit rejection for
   invalidated or disoccluded history. Dynamic lighting must not acquire a long
   convergence tail.
3. **Edge-aware reconstruction.** Reconstruct/upscale HRC lighting with depth,
   normal, and visibility guidance so low-resolution transport is softened
   without bleeding across geometry silhouettes.
4. **Light classes and budgets.** Generalize authoring into hero, world, pickup,
   and transient classes with class-specific energy and spatial density budgets.
   Budgets may attenuate dense contributions but may not truncate logical light
   counts by array order.
5. **Material response.** Preserve linear HDR transport and apply indirect light
   through albedo, normal, roughness/metalness, AO, emission, and environment
   response rather than as a tinted brightness overlay.
6. **Content coverage.** Add stable world lights and lifecycle-aware combat
   sources, including windows/lamps, shrines, enemy attacks, and environmental
   hazards, while RC Off continues to use only the legacy lighting path.
7. **Windows quality tiers.** Lock Low, High, and Auto quality behavior using
   deterministic visual/performance gates. High must remain responsive and Auto
   transitions must preserve the existing transition-boundary contract.

## Invariants

- HRC remains the cutting-edge path; no approximate screen-color seed fallback.
- RC On does not mix the old direct/ambient lighting path into the final frame.
- RC Off remains allocation/dispatch free and retains the old renderer result.
- No per-frame fence wait or auxiliary command-buffer submission is introduced.
- Temporal history is discarded on resize, backend/tier generation changes,
  explicit invalidation, incompatible origin shifts, and visibility mismatch.
- Mac execution is not required for this closeout; shared shader artifacts and
  API layout may not knowingly regress Metal compatibility.

## Verification

Each outcome requires focused tests, a production Release build, live Vulkan
telemetry, and a deterministic visual capture. Performance decisions use
no-readback profiles; screenshots are never treated as frame-time evidence.
