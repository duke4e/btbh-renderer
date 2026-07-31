# Windows Visual Parity Closeout Design

Status: Approved

## Goal

Close the remaining Windows Vulkan visual gaps against the retained legacy
renderer before returning to shadow-performance optimization or macOS/Metal
closeout.

The unchanged authority remains the acceptance source. Goldens, cue bounds,
entity signatures, and the `0.98` SSIM thresholds are not changed.

## Current evidence

The final warmed Vulkan captures already pass every Particle Editor and
City-block Editor gate. Game and Combat HUD/tool cues pass. Three gates remain:

- Game world viewport;
- Combat Lab world viewport;
- Unified City control panel.

Direct comparison shows that Game and Combat share one dominant difference in
the animated player's surface lighting and detail. The native legacy-to-GPU
single-clip matrix probe matches within approximately `9.2e-5`, so the imported
skeleton and valid-time sampled skin matrices are not the source of the
capture-scale mismatch.

A focused capture also disproved the animation-clock hypothesis: routing the
baseline simulation delta into the scene animation path changed Game SSIM by
only approximately `0.00002`, left the player crop unchanged, and was fully
reverted. The remaining investigation must therefore prove a material, normal,
weight, or blended-pose boundary before changing production code.

Unified City has a separate canvas defect. The GPU rounded-rectangle fan omits
the closing triangle from the last boundary point to the first. The resulting
hole is exactly the large top triangle visible through the translucent control
panel; it is not water or scene geometry.

## Selected approach

### 1. Close the rounded-rectangle fan

Append the first rounded-rectangle boundary vertex after the four corner arcs,
so the existing triangle-list fan emits the missing top segment. Keep the
generic circle fan unchanged because it already supplies both the zero- and
360-degree boundary points.

Cover the defect with a real Windows Vulkan readback test that samples a pixel
inside the previously missing segment. A real Unified capture must then
demonstrate removal of the triangle without changing game UI code or goldens.

### 2. Prove the Game/Combat surface mismatch

Use focused probes and real captures to distinguish animated normal transform,
material channel, weight normalization, and blended-pose behavior. Change only
the lowest boundary supported by evidence. Do not revive the disproven clock
change, approximate the legacy image with arbitrary lighting changes, or
special-case the player asset.

### 3. Visual acceptance

After focused Game, Combat, and Unified captures are green, run two warmed
five-mode series. Both series must:

- exit cleanly with no failure marker;
- emit 1920x1080 output;
- preserve the canonical entity signatures;
- pass the unchanged global and named-cue gates;
- retain same-source repeatability.

Particle and City results must remain green. The historical Windows-to-Apple
font, translucent-edge, and antialiasing limitation remains documented and is
not relabeled as solved.

## Ordering

Visual parity is the current priority. Shadow batching/caching and the final
10-run FPS comparison resume only after the three remaining visual gates are
closed or a residual is proven to be solely the already documented
cross-platform text/antialiasing issue.

## Confirmed target post-processing path

The shipping performance target is the FXAA path without the legacy SSAO pass.
SMAA and SSAO remain available only as diagnostic references while migration
parity is being isolated:

- SMAA cost is not a release-performance blocker because FXAA replaces it;
- SSAO cost is not a release-performance blocker because Radiance Cascades
  replaces the current ambient-occlusion approach;
- neither substitution excuses unrelated geometry, material, normal, lighting,
  compositing, or UI defects;
- the final Windows A/B performance gate measures the target FXAA/no-SSAO path;
- Radiance Cascades receives its own visual and performance acceptance work
  when that pass is integrated.

This decision changes benchmark scope, not the unchanged golden images, cue
bounds, entity signatures, or SSIM thresholds used to identify current visual
regressions.
