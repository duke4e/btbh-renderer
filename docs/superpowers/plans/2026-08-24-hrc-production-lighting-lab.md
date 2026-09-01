# HRC Production Lighting Lab Implementation Plan

Status: automatically approved; implementation complete

Date: 2026-08-24

Design:
`docs/superpowers/specs/2026-08-24-hrc-production-lighting-lab-design.md`

1. [x] Add focused game tests for production presets, finite class-control
   bounds, neutral forwarding, transformed emitters, solo, freeze, reset, and
   temporal invalidation.
2. [x] Add class tuning and retained scratch/frozen emitter state to the F9
   lighting lab without changing the renderer ABI or steady-state neutral path.
3. [x] Split the F9 layout into Lighting/System pages and render the new class,
   preset, solo, freeze, telemetry, and slider controls at supported sizes.
4. [x] Add an interactive Windows lighting-lab launcher that selects live,
   normal-reference, or radiance-stress gameplay with HRC High/Balanced.
5. [x] Run focused and broader RC tests, Release Vulkan build/smoke, deterministic
   preset captures, and a no-readback performance profile; record the visual
   judgment and remaining post-chain follow-up.

## Verification record

- 43 Radiance Cascades tests and all 11 CameraProfile/launcher tests pass on
  Windows.
- The production Release build completed and the bootstrap probe selected
  hardware Vulkan at 1280x720.
- Same-frame paused captures cover Readable, Balanced, Neon, both menu pages,
  and Hero solo. Balanced is selected at launch; Hero solo reports one source
  and preserves the player cone.
- The seeded `radiance` stress profile completed 298 measured frames at 59.419
  FPS (16.830 ms average, 17.000 ms p95, 18.462 ms worst). HRC stayed active at
  1280x720 with valid temporal history and no fallback or error marker.
- Baseline capture mode was absent and the profile log contains no readback or
  baseline-capture marker.

Visual judgment: the authoring surface is clear and functionally authoritative
across both gameplay emitters and the renderer-managed player cone. Preset
differences are intentionally controlled in a calm scene; the class selector
and solo mode are the useful tools for the next dense-pickup/projectile tuning
pass. Proper exposure and multi-resolution bloom remain separate post-chain
work rather than hidden gains in this tool.
