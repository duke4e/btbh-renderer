# Radiance Cascades Shared Lighting and Visibility Plan

Status: approved and in progress

Date: 2026-08-20

## Goal

Replace the duplicated, unoccluded RC point-light path with reusable authored
lighting, world-space city visibility, interval occlusion, and separated
emission while preserving current direct-light and debug behavior.

## Implementation steps

1. Extend public light/RC contracts with omni lights, independent indirect
   energy, bounded occlusion cascades, validation, and compatibility defaults.
2. Project active indirect lights and explicit transient emitters into one
   stable native source list; wire the game player through the shared light
   registry instead of duplicating its RC transform.
3. Build three snapped 2.5D signed-distance/height cascades from existing city
   obstacle footprints, heights, and revision data; upload revision-gated
   packed fields to native GPU storage buffers.
4. Add source-to-receiver and interval visibility queries to RC seed/trace,
   with low/high sampling budgets and GPU readbacks that compare exposed and
   occluded receivers.
5. Split material/surface emission into a dedicated HDR G-buffer target and
   seed RC from that signal without reading final scene color.
6. Port the legacy omni shadow design to the SDL GPU hero-light slot and reuse
   retained shadow scheduling and caster submission for high-frequency
   visibility.
7. Tune the player and debug controls against the obstacle-aware result, then
   run focused/native/GPU/full renderer and game verification plus independent
   review.

## Interfaces and invariants

- `LightDescriptor.energy` is direct energy; `indirectEnergy` is RC energy.
- `RadianceCascadesDescriptor.lightSources` remains compatible and is appended
  after handle-backed lights.
- Occlusion buffers are identified by stable revision and complete metadata;
  no native pointer survives the scene-present call.
- C structs and HLSL buffers remain 16-byte aligned and reflection tests define
  their exact resource counts.
- Canonical and vendored renderer changes remain content-identical.
- No commits, worktrees, or unrelated cleanup are part of this plan.

## Verification

- Contract tests cover defaults, validation, ordering, caps, revisions, and
  descriptor compatibility.
- CPU tests cover obstacle rasterization, signed-distance sign, cascade
  snapping, destruction invalidation, and stable results.
- GPU tests prove light color, source localization, wall occlusion, interval
  leak prevention, emission-only contribution, and empty-source black output.
- Native tests cover C validation, layouts, buffer upload/reuse, and shader
  reflection.
- Full renderer and game suites, debug GPU build, runtime smoke, mirror checks,
  and diff checks close the work.

## Risks

- Thin geometry can be lost at coarse cascade resolution; near-cascade cell
  size and shadow-map detail provide the mitigation.
- Six-face omni shadows are expensive; only one hero slot is updated on the
  existing cadence and all other sources use the field.
- CPU field rebuilding can spike if done every frame; snapped origins and the
  existing obstacle revision gate rebuilds.
- Emission MRT increases bandwidth; its target is allocated only while RC is
  active and uses the existing internal RC resolution where possible.
