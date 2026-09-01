# HRC Windows Quality Presets Plan

Status: implemented and verified on Windows; native Mac acceptance deferred

Date: 2026-08-23

Design:
`docs/superpowers/specs/2026-08-23-hrc-windows-quality-presets-design.md`

1. [x] Add failing native tests that lock Low 1x, High 2x, distinct working
   sets, and the existing boundary-gated Auto hysteresis contract.
2. [x] Select the HRC linear scale from the effective tier and preserve resource
   generation/history invalidation behavior.
3. [x] Make HRC the implicit backend for explicitly enabled RC tiers, keep Off
   and explicit Classic behavior, and update focused game tests.
4. [x] Update the Windows launcher and F9 labeling to expose the locked presets
   without a hidden Classic default.
5. [x] Run native/game tests, Release Vulkan bootstrap, live telemetry, repeated
   no-readback Low/High profiles, and deterministic Low/High capture pairs.
6. [x] Record measurements and visual judgment, mark roadmap item 7 complete,
   and leave native Mac acceptance explicitly deferred.

## Windows verification record

- Native HRC tests pass with Low at 1x/quarter-resolution indirect and High at
  2x/full-resolution indirect; Low has the smaller transport working set.
- The focused Swift RC/HRC suite passes 34/34, including the explicit Classic
  to production-HRC menu transition and boundary-gated Auto behavior.
- Release Vulkan bootstrap reports hardware Vulkan, HRC active with no fallback,
  valid temporal history, and the expected Low 320x184 / High 1280x720 telemetry
  for the 1280x720 runtime.
- Two warmed 240-frame no-readback runs per tier remain below 16.67 ms: Low
  median/p95 3.6/4.1 and 3.7/4.1 ms; High 4.0/4.5 and 3.7/4.1 ms.
- Deterministic composed and final-indirect capture pairs were retained in the
  Windows verification artifact archive.
  High resolves finer gradients; Low is visibly softer/coarser while retaining
  source color, occlusion, silhouettes, UI, and bounded indirect light.
