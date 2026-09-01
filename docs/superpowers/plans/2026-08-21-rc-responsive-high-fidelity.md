# RC Responsive High-Fidelity Implementation Plan

Status: Complete

- [x] Record and auto-approve the confirmed design, acceptance criteria, and
  go/no-go threshold.
- [x] Add a deterministic one-frame-in-flight High launch/profile and preserve
  queue/fence/frame telemetry for before/after latency evidence.
- [x] Split RC geometry into opaque G-buffer and post-compose forward
  transparent phases while leaving the legacy order unchanged.
- [x] Add a High-only 64-to-16 angular prefilter compute pass and make material
  resolve consume its compact directional atlas; keep Low native at four.
- [x] Decouple and raise the High direct target to 3x field resolution, soften
  sub-cell blocker transitions, and prove cascade allocations are unchanged.
- [x] Extend C and Swift contract tests, synchronize vendored renderer/game
  shader artifacts, and run renderer plus game builds.
- [x] Capture deterministic legacy, RC final, final-indirect, direct-field,
  cascade, confidence, and projectile-shadow comparisons; collect median/p95
  and fence completion evidence against the acceptance gate.
- [x] Perform a final local code review, address material findings, and
  record the go/no-go decision in this plan.

## Verification evidence

- Renderer native build completed and CTest passed 19/19, including shader
  reflection plus live GPU device, window, and vertical smoke tests.
- Renderer `RadianceCascadesTests` passed 8/8. The added uniform-radiance
  conformance test verifies that 64-to-16 angular reduction preserves the
  integrated RGBA interval within a 0.001 tolerance.
- GPU-enabled game build completed. Targeted game RC tests passed 16/16 and
  the complete game test matrix passed 1,634 XCTest cases plus 3 Swift Testing
  cases with no failures.
- All ten renderer files mirrored into the pinned game vendor tree have
  matching SHA-256 hashes. The packaged prefilter SPIR-V artifact was used by
  the live Vulkan captures.
- Deterministic captures were recorded under
  `rc-responsive-high-fidelity`: legacy, RC final, final-indirect,
  direct-light, cascade zero, and confidence.

## Performance gate

The comparison below uses the same deterministic 147-frame fixed-step
`courser_frame` capture. Both tiers report one allowed frame in flight and a
peak of 115 projectiles.

| Tier | Median frame | p95 frame | Median fence | p95 fence |
| --- | ---: | ---: | ---: | ---: |
| RC High | 23.9 ms | 24.7 ms | 24.196 ms | 24.932 ms |
| RC Off | 8.8 ms | 9.4 ms | 8.848 ms | 9.549 ms |

The previous High capture clustered fence completion around 78 ms with three
frames in flight. The new one-frame queue therefore removes roughly 54 ms of
pipeline latency. Angular prefiltering also moves the prior roughly 26 ms High
median to 23.9 ms while retaining 64 transport directions and resolving 16
filtered directions.

The measured High-minus-Off median remains approximately 15.1 ms. This misses
the agreed 16.7 ms total-frame target and exceeds the 8-10 ms classic-RC
continuation threshold. RC Off proves that the remaining blocker is the RC
backend rather than a named non-RC pass.

## Review and decision

The final review found no material correctness defects after adding the
missing numerical energy-conformance test. Live captures confirm that forward
transparent projectiles and their authored-alpha shadows render after opaque
RC composition; individual shadows are translucent, while very dense overlap
still accumulates into a dark cluster as expected from alpha overdraw. The 3x
direct layer produces finer cone and blocker edges without changing cascade
atlas layouts.

**Decision: no-go for further global-resolution increases or incremental
classic directional-RC tuning.** Preserve this iteration as a responsive,
visually improved reference, then evaluate Holographic Radiance Cascades or
Split Radiance Cascades as an explicit replacement backend. Temporal history
is not approved as a shortcut for the remaining same-frame cost.
