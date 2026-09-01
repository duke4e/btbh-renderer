# RC Content Emitters Plan

Status: implemented and verified on Windows

Date: 2026-08-23

Design:
`docs/superpowers/specs/2026-08-23-rc-content-emitters-design.md`

1. [x] Add failing tests for city, enemy-projectile, and environmental source
   authoring, stable identity, lifecycle filtering, and class assignment.
2. [x] Derive deterministic window/lamp sources from applied city objects and
   filter destructed owners through the existing Game obstacle lifecycle.
3. [x] Add active enemy projectile sources using shared presentation colors.
4. [x] Add environmental hazard snapshots from authoritative CombatWorld arrays,
   including a segment representation for sweeping beams.
5. [x] Extend the deterministic radiance showcase and telemetry with source-class
   counts, then run tests, Release build, captures, and no-readback profiling.

## Verification record

- All 16 focused gameplay-light tests pass, including deterministic city
  identity/destruction, projectile identity/color, environmental class/shape,
  finite energy bounds, and 512-source count preservation.
- The deterministic radiance scenario reports 1,340 logical sources: 1,321
  world, 10 pickup, and 9 transient. Directional draw expansion now processes
  only actual spot emitters; its native unit contract passes.
- The normal visual scenario is the beauty-authority check and preserves
  material separation with restrained city windows and a readable warm lamp:
  `hrc-high-city-only-3/game.png`. The intentionally dense many-light stress
  evidence is `hrc-high-composed-balanced/game.png`, with its transport-only
  inspection in `hrc-high-final-indirect-balanced/game.png`.
- The warmed 240-frame 1280x720 no-readback High run with the same 1,340-source
  logical set recorded 3.7 ms median, 4.0 ms p95, and 4.731 ms worst. Release
  Vulkan bootstrap and live HRC telemetry reported the HRC backend active with
  no fallback and valid phased temporal history.
