# HRC Material Response Plan

Status: implemented and verified

Date: 2026-08-23

Design:
`docs/superpowers/specs/2026-08-23-hrc-material-response-design.md`

1. [x] Add focused failing shader-contract tests for BRDF, AO, emission, and
   unlit behavior across HRC/classic resolves and contact AO.
2. [x] Encode the unlit mask in the RC material G-buffer and bypass lighting in
   both resolve paths.
3. [x] Add and run a Vulkan HRC material pixel test.
4. [x] Mirror shared shaders into the game Vendor copy and rebuild production
   shader artifacts.
5. [x] Capture final-indirect output, inspect it, run a no-readback profile, and
   close roadmap item 5.

Verification evidence:

- 15 focused RC contract tests, 7 emitter-budget tests, and the hardware Vulkan
  HRC material pixel test pass.
- Production shader compile/reflection, full Release game build, and hardware
  Vulkan bootstrap pass.
- Deterministic 1920x1080 composed and final-indirect captures are stored under
  `rc-material-response` in the task visualization directory.
- The uncapped 1280x720 radiance profile completed 240 frames at 3.500 ms median,
  3.900 ms p95, and 280.596 FPS average without readback.
- Canonical and game-Vendor copies of the four changed build/shader files have
  identical SHA-256 hashes.
