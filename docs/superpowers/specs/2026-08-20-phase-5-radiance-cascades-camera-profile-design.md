# Phase 5 Radiance Cascades and CameraProfile Design

Status: approved

Date: 2026-08-20

## Goal

Add safe, deterministic single-bounce diffuse indirect lighting to the
production SDL_GPU renderer and add deterministic camera-profile transitions to
the game. The feature must behave identically on Vulkan and Metal, preserve the
existing five launch modes, and fall back to the current IBL, ambient, and SSAO
result whenever Radiance Cascades (RC) is disabled or unavailable.

This specification refines Phase 5 of the approved sustainable-renderer-fork
program. The approved implementation shape is a narrow RC module integrated
into the existing staged scene submission. A generic frame graph is not a
prerequisite and is not part of this phase.

## Acceptance criteria

- `Off`, `Low`, `High`, and `Auto` are represented by stable renderer and game
  contracts.
- `Off` allocates no RC textures, creates no RC pipelines, and dispatches no RC
  work.
- `Low` and `High` execute the same semantic pass chain on Vulkan and Metal:
  seed, far-to-near cascade trace/merge, resolve/confidence, and compose.
- RC pipeline, resource, or dispatch failure never prevents the scene from
  rendering. The renderer records a typed fallback reason and uses the existing
  IBL + ambient + SSAO path.
- RC debug output exposes each cascade, hit distance, confidence, final
  indirect, and fallback reason without changing the normal scene contract.
- `Auto` uses warmed rolling p95 completion time with hysteresis, but applies a
  pending tier only when the game marks a level-transition boundary.
- Camera profiles are deterministic for player type, run seed, and level;
  remain inside the approved yaw, pitch, distance, and framing envelope; and
  apply atomically behind the level-up cards.
- A profile/tier transaction invalidates camera-dependent renderer state and
  renders one hidden warm-up frame before gameplay is revealed.
- Five-mode visual, cue, lifecycle, and performance gates pass on Vulkan and
  Metal. `High` targets an RC p95 of at most 4.0 ms at 1080p on the reference
  minimum hardware.

## Existing production constraints

The current renderer is staged manually in `src/gpu/btbh_gpu_scene.c`. Geometry
produces the HDR scene color and, when auxiliary outputs are requested, ambient
diffuse, encoded normal/material occlusion, and ambient specular/roughness.
Depth is sampleable. SSAO and post processing then consume those targets.

RC reuses those four production color targets rather than adding a fifth MRT.
The visible direct/emissive seed is reconstructed as the non-negative scene
color remaining after ambient diffuse and ambient specular are removed. The
receiver diffuse response is derived conservatively from the existing ambient
diffuse/material outputs. This keeps Phase 5 inside the proven cross-backend MRT
contract. A dedicated albedo/material target may only be introduced later if
golden scenes demonstrate a repeatable quality defect that the confidence and
fallback resolve cannot address.

RC therefore requests auxiliary geometry outputs independently of SSAO. The
existing one-target fast path remains unchanged when both SSAO and RC are off.

## Public contracts

### Renderer tier and debug configuration

The renderer seam adds:

- `RadianceCascadesTier`: `off`, `low`, `high`, `auto`;
- `RadianceCascadesDebugView`: `none`, cascade 0-3, hit distance,
  confidence, final indirect, fallback reason;
- `RadianceCascadesDescriptor`: requested tier, debug view, automatic frame
  budget, and a one-frame permission to apply a pending automatic tier;
- `RadianceCascadesStatus`: requested and active tier, pending tier, typed
  fallback reason, internal resolution, and CPU encode timing.

Default environment configuration is `off`. Existing callers remain source
compatible through default initializer arguments.

`Auto` is explicit state, not an alias for `High`. On first use it starts at
`Low`. Measurement may select a pending adjacent tier, but allocation and active
tier only change when `allowAutoTransition` is true. Hysteresis uses separate
upgrade and downgrade thresholds and never skips directly between `High` and
`Off`.

### C boundary

The C scene-post descriptor carries fixed-width tier/debug/transition fields and
the positive auto frame budget. The scene report carries active/pending tier,
fallback reason, RC dimensions, and CPU RC encode time. Enums have explicit
values and validation rejects unknown values before any GPU work.

The C implementation exposes deterministic, GPU-independent tier-state helpers
for unit tests. Persistent state is owned per `BTBHGPUWindow`; there is no
process-global RC state.

## Runtime module

RC state and resource helpers live in a dedicated
`src/gpu/btbh_gpu_radiance_cascades.c` module with an internal header. Scene
submission remains the owner of ordering and command-buffer lifetime.

Per-window state contains:

- requested, active, and pending tier;
- rolling completion samples and hysteresis state;
- sticky typed fallback reason and diagnostic text;
- compute pipelines for seed, trace/merge, and resolve;
- a graphics pipeline for compose/debug presentation;
- RC-sized seed, four cascade, resolve, and compose textures;
- dimensions and a generation used to invalidate camera-dependent data.

Resources are lazy. `Off` releases existing RC resources at a safe scene
lifetime boundary. Resize, tier change, explicit invalidation, or backend
failure clears the generation and recreates only the resources required by the
active tier.

## Quality tiers

Both active tiers run four semantic cascades and use 8x8 compute groups.

| Tier | Internal resolution | Angular samples | Trace scale |
| --- | --- | --- | --- |
| Low | quarter width and height | 4 per interval | conservative |
| High | half width and height | 8 per interval | full |

Dimensions round up to complete 8x8 groups while shaders clamp all reads and
writes to the logical RC extent. Cascade intervals grow exponentially in screen
space. The exact constants are shader-owned quality data and must be recorded in
reflection/source-contract tests.

## Pass chain

1. **Seed** samples scene color, ambient diffuse, ambient specular, normal, and
   depth. It writes visible direct/emissive radiance and initial validity.
2. **Trace and far merge** dispatches from cascade 3 to cascade 0. Each cascade
   traces a bounded screen-space interval at its angular scale and merges the
   already-resolved farther cascade.
3. **Resolve** performs depth/normal-aware filtering, computes confidence, and
   applies the receiver diffuse response. Low-confidence output tends toward
   zero so the existing IBL/ambient result remains authoritative.
4. **Compose** adds resolved indirect to the HDR scene input. Debug modes replace
   the composed scene with a stable visualization. The normal post chain then
   proceeds unchanged.

The baseline has no temporal accumulation. Explicit invalidation is still part
of the contract so a future history implementation cannot silently ignore
CameraProfile transitions.

## Failure and fallback policy

Fallback reasons are typed as: none, disabled, capability unavailable, shader
unavailable, pipeline creation failed, resource allocation failed, dispatch
failed, and invalid configuration.

An RC failure is local to RC. The renderer logs the first transition into a
failure state, records the reason in the scene report, skips all remaining RC
passes for that frame, and continues from the original HDR scene texture. It
does not overwrite the primary scene result with an RC setup error.

Pipeline/resource failures are sticky until a relevant configuration change,
resize, or explicit invalidation. This prevents repeated allocation attempts
and log spam. The fallback-reason debug view presents a stable diagnostic color
even when compute resources are unavailable.

## CameraProfile transaction

Camera profiles remain game content. The renderer only receives the resulting
camera and an invalidation/transition signal.

Profile selection is a pure function of player type, run seed, and level. Every
shipping profile stays within:

- yaw offset: `[-0.55, +0.55]` radians;
- pitch offset: `[-0.20, +0.14]` radians;
- distance factor: `[0.78, 1.26]`;
- a small, content-defined framing offset validated by cue masks.

The level-up flow is one transaction:

1. retain the last gameplay snapshot;
2. display the level-up cards;
3. select and apply the new profile;
4. permit any pending `Auto` tier transition;
5. invalidate RC, visibility/culling, and camera-dependent post state;
6. render one hidden warm-up frame;
7. reveal the new gameplay frame only after the cards close.

Camera shake stays a transient view offset. The old multi-second continuous
`cameraGlitch` behavior is removed after profile parity is verified.

## Verification

- C unit tests cover enum/config validation, first-use `Auto`, adjacent tier
  changes, hysteresis, transition gating, sticky fallback, and invalidation.
- Shader reflection tests require identical resource counts, 8x8 groups, and
  expected storage formats for every RC compute shader.
- Swift tests cover public defaults and exact projection to the C descriptor.
- GPU tests prove compute write-to-sample ordering, resize/reallocation, `Off`
  zero-work behavior, and failure fallback.
- Game tests prove profile determinism, bounds, level-boundary application, and
  the hidden warm-up transaction.
- The existing renderer/game suites and all five launch modes remain green.
- Milestone closeout records three-run development and ten-run final A/B data,
  visual comparisons, per-pass captures where tooling permits, and any result
  more than 5% from the accepted baseline.

## Risks and rollback

The main quality risk is incomplete off-screen visibility inherent to
screen-space RC. Confidence blending and the existing IBL/ambient solution are
mandatory mitigations, not optional polish. The main performance risk is four
compute traversals plus an HDR compose pass; tier resolution and dispatch counts
must be visible in telemetry before game rollout.

RC remains disabled by default until both native backends pass the milestone
gates. Any failed rollout is reversible by setting the environment descriptor
to `off`; no renderer/backend switch is required. Phase 6 legacy removal begins
only after Phase 5 cross-platform acceptance.

## Non-goals

- generic frame graph;
- temporal GI accumulation in the first milestone;
- specular RC;
- world-space or hybrid probe volume;
- GPU-driven culling work unrelated to RC;
- changing gameplay-critical cues to depend on indirect lighting;
- Phase 6 dependency removal before Phase 5 acceptance.
