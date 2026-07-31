# DPI-Independent Scene Projection Design

## Goal

Make world-to-screen projection and screen-to-world unprojection independent
of Retina or other drawable scaling, so world-anchored Canvas2D elements and
pointer rays remain aligned with the 3D scene at 1×, 2×, and future scale
factors.

## Root Cause

`SceneBackend.project` currently returns pixels computed with the raw drawable
size. Game UI and editor overlays consume those values in logical Canvas2D
space. On a 1280×720 logical window with a 2560×1440 drawable, the center of
the world therefore projects to `(1280, 720)` instead of `(640, 360)`.

The inverse path has the same defect: logical pointer coordinates are passed
to `GetScreenToWorldRayEx` together with drawable dimensions. This changes the
ray direction on Retina displays.

This is also a simulation bug, not only a presentation bug.
`cameraGroundFootprint` unprojects the four logical screen corners. At 2×,
the logical bottom-right corner is interpreted as the drawable center, so the
computed footprint covers only part of the visible world. Enemies visibly
inside the right or lower part of the screen are then classified as offscreen
and incorrectly receive the existing `2.6×` maximum-speed and `3.2×`
acceleration catch-up multipliers.

The mismatch predates the direct preview viewport change. It became visible
when game call sites migrated from raylib `GetWorldToScreen`, which used
logical screen dimensions, to the renderer seam.

## Considered Approaches

### A. Normalized viewport coordinates — selected

The renderer returns and consumes points normalized relative to a requested
scene viewport. Game and editor presentation code convert between normalized
points and their authoritative logical Canvas2D dimensions.

This keeps DPI, backing scale, and backend pixel conventions outside the
public projection contract. It also composes with the normalized scene
viewport added for the player preview.

### B. Pass logical pixel dimensions into every projection call

This is smaller, but continues to define a renderer query in UI pixels and
allows callers to accidentally pass drawable dimensions again.

### C. Divide affected game overlays by the Retina scale

This is rejected. It would duplicate conversion in damage popups, health
bars, encounter labels, indicators, editors, and pointer picking while
leaving the renderer contract ambiguous.

## Public Renderer Contract

Add a dedicated value type:

```swift
public struct NormalizedViewportPoint: Hashable, Sendable {
    public let x: Float
    public let y: Float
}
```

Values are top-left normalized and relative to the requested viewport:

- `(0, 0)` is the viewport's top-left corner;
- `(1, 1)` is the viewport's bottom-right corner;
- values outside `0...1` are valid and represent offscreen points;
- values do not encode drawable or logical pixels.

Replace the ambiguous scene queries with:

```swift
func projectNormalized(
    _ world: RenderVec3,
    camera: RenderCamera,
    viewport: NormalizedViewport
) -> NormalizedViewportPoint

func unprojectRay(
    _ point: NormalizedViewportPoint,
    camera: RenderCamera,
    viewport: NormalizedViewport
) -> RenderRay
```

Protocol-extension overloads may supply `.full`, but no public projection
method may return an undocumented pixel coordinate.

Invalid viewports or unavailable backends keep the existing nonthrowing,
fail-closed behavior: projection returns `.zero`; unprojection returns a zero
ray.

## Legacy Backend Mapping

The legacy adapter resolves the requested `NormalizedViewport` against its
current drawable size using the existing four-edge viewport resolver.

For projection:

1. use only the resolved viewport width and height as the raylib projection
   dimensions;
2. call `GetWorldToScreenEx`;
3. divide the result by those same dimensions;
4. return the resulting `NormalizedViewportPoint`.

For unprojection:

1. multiply the normalized point by the resolved viewport width and height;
2. call `GetScreenToWorldRayEx` with those same dimensions.

The absolute drawable viewport origin is not part of the camera projection
math because normalized points are relative to that viewport.

The raw raylib math will be factored into focused pure helpers so it can be
tested without an OpenGL window.

## Game and Editor Mapping

Add one game-side projection utility responsible for crossing between
normalized viewport space and logical Canvas2D space.

For a full-window logical canvas:

```text
logical.x = normalized.x × logicalWidth
logical.y = normalized.y × logicalHeight
```

The inverse operation divides logical pointer coordinates by the same
logical dimensions before calling `unprojectRay`.

All existing world-anchored UI and picking call sites migrate through that
utility:

- damage popups and enemy health bars;
- elite/boss indicators and encounter labels;
- beam screen-density calculations;
- growth-trial pointer rays and camera footprints;
- enemy visible-footprint classification and offscreen catch-up movement;
- city-block editor overlays and picking;
- particle preview picking.

The Canvas2D coordinate system remains logical and unchanged.

## Error Handling and Invariants

- Logical dimensions must be positive before normalization.
- Nonfinite normalized points remain nonfinite so existing visibility guards
  can reject them.
- Offscreen normalized points are not clamped.
- Invalid normalized viewports fail closed at the scene backend boundary.
- Projection and unprojection must use the same viewport aspect ratio.
- No call site may infer or hardcode a backing scale such as `2`.

## Testing

Renderer tests lock the `NormalizedViewportPoint` value semantics and the
source-breaking normalized scene contract.

Legacy adapter tests use the pure raylib helpers to prove:

- a camera target projects to `(0.5, 0.5)` at both 1280×720 and 2560×1440;
- the same normalized center produces the same ray at both sizes;
- a non-full viewport with the same aspect ratio produces the same normalized
  result;
- invalid viewports fail closed.

Game contract tests prove that world-to-Canvas and Canvas-to-world conversion
use logical dimensions and that no direct ambiguous `scene.project` calls
remain.

Camera-footprint regression tests prove that the four logical screen corners
produce the same ground polygon at 1× and 2× drawable scales. An enemy located
inside the right or lower half of that visible polygon must remain in the
normal movement branch and must not receive the `2.6×`/`3.2×` offscreen
catch-up multipliers.

Focused suites run before the full renderer and game suites. Final manual
verification checks damage numbers, enemy health bars, encounter labels,
pointer picking, and enemy speed across both halves of the Retina viewport.

## Non-Goals

- Changing camera angles, FOV, animation, UI scale, or Canvas2D rendering.
- Adding a hardcoded macOS backing-scale correction.
- Reworking the direct preview viewport.
- Introducing an offscreen render target.
