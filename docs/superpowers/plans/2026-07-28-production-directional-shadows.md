# Production Directional Shadows Implementation Plan

Status: approved automatically on 2026-07-28.

## 1. Lock the lighting projection contract

- Extend `GPUSceneLightingProbe` with two directional shadow projections.
- Preserve `castsShadow`, range, and softness while selecting sorted lights.
- Add focused Swift tests for slot association and fail-closed inputs.
- Add a camera-aware helper implementing the approved legacy-compatible
  snapped orthographic matrix.

## 2. Add production shadow shaders and reflection checks

- Add static and animated depth-only vertex shaders with production transform
  parity.
- Register SPIR-V/Metal artifacts in CMake.
- Update reflection assertions for both shadow shaders and the expanded model
  fragment contract.
- Build once to establish the intended RED failures before native support is
  complete.

## 3. Implement native shadow resources and passes

- Extend the internal C lighting ABI with two matrices and shadow controls.
- Create/release two sampled D32 maps, a sampler, and static/animated shadow
  pipelines.
- Render opaque static/animated batches into enabled maps before the scene
  pass.
- Bind both maps to fragment slots four and five.
- Preserve partial-initialization cleanup and existing error reporting.

## 4. Apply shadow visibility in direct PBR

- Extend the fragment uniform buffer with both matrices and controls.
- Implement bounds rejection, legacy constant/slope bias, and deterministic
  3 by 3 PCF.
- Multiply each visibility only into its matching directional contribution.
- Add a real Vulkan enabled/disabled receiver/caster regression test.

## 5. Verify and close Windows

- Run native `/W4 /WX`, all CTests, focused then full renderer Swift tests,
  full game Swift tests, release build, API audit, and diff checks.
- Request one independent final code review and resolve all high/medium
  findings.
- Run two warmed five-mode Windows A/B capture series; inspect output, entity
  signatures, shutdown, cue parity, and timing regression.
- Update the Windows report without changing goldens or thresholds.
- Commit and push the renderer branch, then update, commit, and push the game
  submodule gitlink.

