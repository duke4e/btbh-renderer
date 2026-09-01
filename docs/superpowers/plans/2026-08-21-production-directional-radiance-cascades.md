# Production Directional Radiance Cascades Plan

Status: approved and in progress

Date: 2026-08-21

Design:
`docs/superpowers/specs/2026-08-21-production-directional-radiance-cascades-design.md`

## Goal

Replace the bounded-source hybrid RC prototype with a paper-conformant,
directional, unlimited-emitter RC lighting mode and preserve the current PBR
renderer only as the explicit RC-off mode.

## Implementation steps

1. Add a CPU conformance model for cascade layout, direction mapping,
   contiguous interval ranges, radiance/transmittance over composition, child
   direction filtering, and four-probe spatial interpolation. Lock the
   probe-tile atlas contract and selectable 2x/4x angular branching with C
   and Swift tests.
2. Replace the fixed 16-source public/native contract with frame-scoped,
   growable `RadianceEmitter` submissions and separate emitter/source-field
   telemetry. Preserve temporary source compatibility only in the adapter,
   never in native shader uniforms.
3. Add world-space source-field capture for material emission and instanced
   analytic emitters. Reuse the existing snapped 2.5D occlusion cascades and
   verify unlimited emitter injection independently from the RC trace.
4. Implement per-cascade directional interval tracing and far-to-near merge
   in compute shaders. Allocate cascade-specific probe-tile atlases and
   use physical radiance/transmittance composition instead of averaged RGB
   and constant lerp weights.
5. Add the RC material resolve with depth/normal/world-position-aware spatial
   reconstruction. Introduce explicit `legacy` and `radianceCascades`
   lighting modes so the RC path cannot evaluate or compose legacy lighting.
6. Add published bilinear/parallax correction as the research quality tier,
   tune base probe spacing, direction count, interval scale, and emitter area
   against the penumbra conformance scenes, then decide whether the correction
   cost belongs in high tier.
7. Delete the approximate seed/trace/resolve path and fixed source ABI after
   conformance, mirror the canonical renderer into the game, connect gameplay
   emitter streams, and run full renderer/game build, packaging, GPU readback,
   performance, and independent review gates.

## Interfaces and invariants

- No shader loop is bounded by a maximum light-source constant.
- Every cascade atlas stores `linear RGB radiance + transmittance` per
  direction.
- Higher cascades decrease probe density and increase direction count.
- The farther cascade is immutable while a lower cascade is merged.
- The single-frame result is correct with temporal history absent.
- RC-on and legacy lighting are mutually exclusive frame modes.
- Source injection and RC solve timing are reported separately.
- Canonical and vendored renderer files remain content-identical after each
  game integration checkpoint.
- No commits, worktrees, or unrelated cleanup are part of this plan.

## Verification

- CPU unit tests establish interval math and atlas addressing before GPU
  shader implementation.
- Shader reflection verifies resource counts, thread-group sizes, and packed
  uniform/storage layouts for SPIR-V and metallib artifacts.
- GPU readbacks compare raw and merged atlas texels with the CPU reference.
- Visual conformance scenes verify area-light penumbra, blocker silhouettes,
  thin-wall containment, colored-source separation, and camera stability.
- A 1/100/1,000/10,000-emitter benchmark verifies unchanged RC trace work and
  reports source injection scaling.
- RC-on captures prove legacy light energy changes do not affect output;
  RC-off captures preserve the existing legacy baselines.
- Full native, Swift, GPU, game, Windows, and macOS packaging checks close the
  migration.

## Risks and mitigations

- Atlas dimensions may exceed conservative device limits at extreme profiles;
  validate profiles against runtime capabilities and reduce domain extent or
  probe density, never direction count silently.
- Thin occluders can violate the penumbra criterion; near-field cell/probe
  density and the published bilinear/parallax fix are explicit quality tools.
- Source-field overdraw can dominate with very large emitters; telemetry and
  spatial clipping isolate this from the constant-cost RC trace.
- 2.5D height fields cannot represent overhangs or stacked interiors; the
  emitter/probe ABI is designed for migration to sparse 3D Split RC rather
  than hiding this limitation.
- Metal and Vulkan storage-texture limits differ; shader reflection and
  capability-gated profile selection are required on both backends.
