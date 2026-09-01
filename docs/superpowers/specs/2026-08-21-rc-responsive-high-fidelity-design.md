# RC Responsive High-Fidelity Design

Status: Approved

Date: 2026-08-21

## Goal

Make the production High Radiance Cascades mode responsive and visually
credible at 1080p without hiding transport defects behind exposure, a global
blur, or the legacy lighting path.

The same deterministic gameplay capture must retain the RC-only lighting
contract, correctly composite transparent projectile shadows and effects,
remove the most visible near-field grid, and move from the current roughly
26 ms median frame toward the 16.7 ms 60 Hz budget.

## Confirmed design

### Latency and measurement

- RC High uses one allowed frame in flight in the game launcher/profile used
  for visual evaluation. Immediate presentation remains selected when the
  backend supports it.
- Swapchain queue depth, full-frame fence completion, RC CPU encode time, and
  deterministic frame statistics remain separately reported. CPU encode time
  is never presented as GPU pass time.
- RC passes retain stable GPU debug labels so Vulkan/Metal captures can report
  trace, merge, angular prefilter, resolve, direct visibility, and composition
  independently.

### Transparent composition

- Opaque geometry writes the RC material G-buffer before the RC solve.
- RC transport, material resolve, and opaque composition complete before
  transparent geometry is drawn.
- Transparent projectiles, projectile shadows, particles, and other forward
  materials then alpha blend directly onto the composed HDR target with the
  opaque depth buffer loaded and depth writes disabled.
- Transparent batches do not write RC material, normal, emission, or other
  auxiliary targets. RC-on transparent shaders do not evaluate legacy lights.
- Existing blend modes and authored alpha values remain the source of truth.

### Angular-prefiltered material resolve

- High retains 64 directional intervals in cascade zero and the existing
  `64 -> 128 -> 256 -> 512 -> 1024` directional transport hierarchy.
- After far-to-near merge, a compute pass performs an energy-preserving
  circular angular reduction of cascade zero from 64 directions to 16
  receiver-facing resolve directions per probe.
- Each output sector is the average radiance and transmittance of its four
  contiguous merged input directions. Circular ordering and total angular
  measure are preserved.
- The full-resolution material resolve evaluates the BRDF against those 16
  filtered directions rather than evaluating a full BRDF 64 times per pixel.
- Low retains its four native directions and bypasses unnecessary reduction.
- Raw and merged cascade debug views continue to expose the unfiltered
  transport. The prefiltered atlas has its own debug label and contract tests.

This is angular convolution at probe level, not a screen blur. Sharp direct
visibility is intentionally owned by the separate direct layer; transported
indirect radiance remains directional through all cascade merges.

### Independent direct-light fidelity

- Direct-light target resolution is independent from the 256 x 256 transport
  and occlusion domain. Raising direct receiver density must not resize the
  cascade atlases.
- High initially moves from a 2x to a 3x field target (0.5 m to approximately
  0.33 m receivers over the same world span). Low retains the base field
  target.
- Direct visibility continues to trace the authoritative 2.5D SDF. Its final
  blocker transition uses a bounded sub-cell visibility ramp rather than a
  binary post threshold, reducing staircase edges without allowing light
  through represented solid cells.
- The direct target remains additive and instanced. Source injection and RC
  trace/resolve contain no receiver-side loop over every light.

### Temporal reuse

- Same-frame raw and merged cascade textures remain distinct resources; they
  are not described as temporal ping-pong.
- Temporal history is introduced only after the single-frame filtered resolve
  passes the visual and performance gate. It requires explicit A/B history,
  previous camera data, disocclusion rejection, neighborhood clipping, and a
  debug acceptance view.
- Near/direct lighting remains current-frame. Any later staggered scheduling
  applies only to stable far cascades and must invalidate on field-origin,
  obstacle-revision, or significant emitter changes.

## Acceptance criteria

- In the deterministic 115-projectile scene, High reports a median frame time
  at or below 16.7 ms or demonstrates that a named non-RC pass is the remaining
  blocker; p95 must be at or below 25 ms after startup samples are excluded.
- The High launcher/profile reports one allowed frame in flight. Frame-fence
  completion is no longer clustered around three rendered frame durations.
- Projectile shadows preserve their authored alpha range of 24...110 and are
  visibly translucent over both bright and dark opaque receivers.
- Transparent effects are drawn after the RC compose and do not modify RC
  G-buffer attachments.
- High cascade zero still contains 64 directions. The prefilter target contains
  16 sectors, and the full-resolution resolve loops over 16 directions.
- A uniform directional-radiance conformance input produces the same integrated
  energy before and after angular reduction within RGBA16F tolerance.
- Increasing direct resolution does not change any cascade atlas dimension or
  allocation. Near cone and blocker edges are visibly finer than the previous
  0.5 m target.
- RC-on frames contain no legacy ambient, directional, spot, or omni lighting
  contribution.
- Renderer C/Swift tests, shader contract tests, Vulkan live-GPU smoke, game
  build/tests, and deterministic legacy/RC/debug captures pass.

## Go/no-go gate

Continue optimizing this classic directional RC backend when the angular
prefilter plus independent direct layer reaches the 60 Hz median target or
leaves no more than 8-10 ms of measured RC GPU work.

If RC still owns materially more than 10 ms after these changes, do not raise
the global field or atlas resolution. Stop this path and evaluate Holographic
Radiance Cascades or Split Radiance Cascades as an explicit backend rewrite.

## Affected components

- `src/gpu/btbh_gpu_window.c`
- `src/gpu/btbh_gpu_scene.c`
- `src/gpu/btbh_gpu_radiance_cascades.c`
- `src/gpu/btbh_gpu_radiance_cascades_runtime.h`
- `shaders/gpu_spike/production_rc_prefilter.comp.hlsl`
- `shaders/gpu_spike/production_rc_resolve.comp.hlsl`
- `shaders/gpu_spike/production_rc_direct_light.frag.hlsl`
- renderer and game shader build/package manifests
- C and Swift RC/shader-contract tests
- game RC launcher and deterministic capture harness

## Risks and controls

- Sixteen resolve directions can over-soften glossy indirect reflections.
  Raw 64-direction transport is retained, the filtered sector count is a
  named profile constant, and visual captures include rough and metallic
  receivers before accepting the reduction.
- A denser direct target increases covered-fragment SDF work by 2.25x versus
  the current 2x target. It is enabled only after resolve savings are measured,
  and can fall back to 2x independently of cascade quality.
- Moving transparent rendering changes pass ordering. Legacy mode remains on
  its existing path, and RC tests assert opaque-compose-forward ordering and
  depth-write invariants.
- One frame in flight can reduce throughput on a saturated GPU. Responsiveness
  and throughput are recorded separately, and the setting is scoped to the RC
  High evaluation path before becoming a broader default.
