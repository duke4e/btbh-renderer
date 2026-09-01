# Radiance Cascades Shared Lighting and Visibility Design

Status: approved

Date: 2026-08-20

## Problem

The production GPU renderer already owns authored lights, shadow-caster draws,
depth and normal buffers, material emission, persistent scene geometry, and the
game's destruction system already owns stable building footprints and heights.
The first RC implementation duplicated point emitters and evaluated them at
every visible receiver without a source-to-receiver visibility query. Its
trace pass then averaged endpoints without testing the interval between them.
That makes indirect intensity behave like a tinted brightness control and lets
radiance cross obstacles.

## Chosen architecture

### One authored light, separate direct and indirect channels

`LightDescriptor` remains the authoritative source for position, direction,
color, range, shadow policy, and update cadence. It gains `indirectEnergy`,
whose default is zero, and an `omni` kind. `energy` continues to control direct
lighting. A light with zero direct energy and positive indirect energy is an
RC-only emitter; a light may contribute to both paths without duplicating its
transform or visibility state.

The existing `RadianceCascadesLightSource` list remains source-compatible for
temporary and effect-authored emitters. The GPU bridge deterministically
combines active indirect lights followed by explicit descriptor emitters and
clips the result to the existing 16-source budget.

### Reuse visibility instead of scene color

RC never derives radiance from final scene brightness. It reuses:

- the renderer's depth, world normal, material occlusion, camera matrices, and
  shadow-caster geometry;
- retained directional and spot shadow state where the RC source is the same
  authored light;
- the legacy R3D omni-shadow design (six face matrices, layer reuse, update
  cadence, bias, and filtered lookup) ported to SDL GPU for the bounded hero
  omni-light budget;
- a world-space 2.5D signed-distance/height field supplied by gameplay for
  city-scale visibility and RC interval traversal.

Direct shadow maps provide high-frequency visibility for selected authored
lights. The distance field is the shared fallback for many RC-only emitters
and prevents cascade intervals from merging through walls.

### Cascaded 2.5D city occlusion field

The public RC descriptor accepts up to three immutable occlusion cascades. A
cascade contains a snapped world XZ origin, cell size, width/height, revision,
and packed per-cell `(signedDistance, topHeight)` float pairs. Positive
distance is empty space, nonpositive distance is occupied, and `topHeight`
defines the vertical prism top. Missing or out-of-bounds data is conservative
only about known geometry: it does not invent an occluder.

The game builds these cascades from the already-maintained destructible
footprint polygons/AABBs and heights. The existing obstacle revision and
snapped cascade origins gate rebuilding. Native GPU buffers are likewise
uploaded only when dimensions, origin, cell size, or revision changes.

RC seed rays from receiver to source through the field before accepting local
radiance. RC trace rays test the interval between receiver and sampled hit,
preventing endpoint averaging across occupied cells. High tier uses finer/more
steps; low tier uses the same correctness path at a lower sampling budget.

### Preserve authored emission as a separate signal

The production geometry pass gains a dedicated linear HDR emission target.
Material and surface-shader emission remains part of the ordinary final scene
for visual compatibility, while RC consumes the separate target as radiance.
Ambient, direct diffuse/specular, tonemapped color, and post-processed scene
color never seed RC.

## Boundaries and invariants

- Legacy rendering ignores RC occlusion cascades and `indirectEnergy`, but
  supports the restored omni light kind using its existing implementation.
- The GPU backend supports at most one shadowed hero omni light initially;
  additional omni RC emitters use the shared occlusion field.
- At most 16 active RC emitters and three occlusion cascades reach native code.
- Source and cascade ordering is stable. Excess entries are ignored.
- Occlusion data is immutable for one scene submission and revision changes
  whenever its contents change.
- No RC path samples final scene color as incoming radiance.
- Empty emitter and emission inputs produce black indirect light.
- Thin/dynamic geometry absent from the gameplay field may use screen depth as
  a detail aid, but screen space is never the only obstacle representation.
- Existing direct lighting, shadow cadence, RC tier/debug controls, and legacy
  fallback behavior remain source-compatible.

## Acceptance criteria

- The player light is authored once and can independently control direct and
  indirect energy.
- A GPU scene with a wall reports substantially less `finalIndirect` behind
  the wall than on an equally distant visible receiver.
- Removing an obstacle and incrementing its revision updates RC visibility
  without recreating the renderer or restarting the game.
- Cascade trace does not transfer radiance across a represented wall.
- An emissive material contributes colored RC radiance without sampling final
  scene brightness.
- Existing directional/spot shadows and the ported hero omni visibility retain
  stable update scheduling.
- Public Swift contracts, C validation/layout tests, shader reflection, GPU
  readbacks, full renderer tests, game tests, and a GPU game build pass.

## Rollback and failure behavior

Invalid occlusion metadata fails RC closed through the existing invalid-
configuration status. Allocation or upload failures use the existing sticky RC
fallback without disabling ordinary scene lighting. A descriptor with no
occlusion field remains valid for compatibility, but its status/debug data
must expose that only limited visibility is available.
