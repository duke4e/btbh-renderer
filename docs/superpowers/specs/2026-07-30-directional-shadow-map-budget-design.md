# Directional Shadow Map Budget

Status: approved automatically on 2026-07-30 under the repository
`AGENTS.md` instruction.

## Context

The Windows Vulkan gameplay capture uses FXAA with SSAO disabled. A warmed
1920x1080 A/B showed:

- full gameplay shadows: about 85.2 ms median;
- no global shadow passes: about 38.0 ms median;
- directional-only shadows: about 60.2 ms median;
- player spot-only shadows: about 37.9 ms median.

The consolidated scene contains 164 shadow-eligible batches and 4,746
instances. None are unlit or blended, so filtering accidental effect casters
does not address the measured bottleneck. The directional shadow target is
2048x2048.

## Decision

Reduce only the production directional shadow map size from 2048x2048 to
512x512. Keep the spot shadow map at 2048x2048 and preserve all light,
material, and caster semantics.

This is preferred over disabling directional shadows because it retains the
lighting feature, and over heuristic caster filtering because the measured
caster set contains no accidental unlit or blended batches. A 1024x1024
candidate measured about 50.1 ms median; the 512x512 candidate measured about
41.4 ms versus about 85.2 ms at 2048x2048. The captured 512 image differed
from the 2048 image in about 0.04% of RGB channels with mean absolute
difference about 0.012/255.

Disabling auxiliary MRT color writes and stores while SSAO was disabled did
not improve the 512 candidate beyond measurement noise, so a no-SSAO pipeline
fork is not part of this change.

## Acceptance

- Renderer and game test suites remain green.
- Vulkan bootstrap and release game build remain green.
- Two warmed five-mode capture series complete at 1920x1080 with clean
  shutdown and identical entity signatures.
- Existing goldens and comparison thresholds are unchanged.
- Same-source cues and visual comparisons pass.
- Gameplay median/p95 improve materially over the 2048x2048 full-shadow
  capture; any remaining regression over the historical Windows Run 3/4
  authority is reported rather than hidden.
- The historical Windows-to-Apple text and antialiasing limitation remains
  documented as unresolved.
