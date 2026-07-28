# Lazy Directional-Shadow Resources Implementation Plan

Status: approved automatically on 2026-07-28.

## 1. Lock the observable lifecycle contract

- Add a package-internal readiness query without changing the public C scene
  report.
- Extend the real Vulkan probe report.
- Prove the current eager behavior RED for a no-shadow scene and preserve a
  shadow-enabled readiness assertion.

## 2. Split base and shadow runtime creation

- Make initial scene setup create only unshadowed model pipeline families and
  common fallback resources.
- Move six-sampler model pipelines, both depth pipelines, two D32 maps, and
  the sampler into one idempotent shadow-resource initializer.
- Invoke it before command-buffer acquisition only when either validated
  directional shadow slot is enabled.
- Keep complete-runtime cleanup for every partial failure and normal
  shutdown.

## 3. Verify behavior and regression

- Run the focused real Vulkan lifecycle and receiver/caster tests.
- Run native `/W4 /WX`, all CTests, and full renderer tests.
- Rebuild the game release executable, run full game tests, API audit, and
  diff checks.
- Run warmed Windows visual/performance captures without changing goldens or
  thresholds, comparing no-shadow editors with the interleaved `7553484`
  controls.

## 4. Review and close

- Request one independent correctness/lifecycle review and resolve every
  HIGH/MEDIUM finding.
- Record exact visual, lifecycle, and performance evidence in the Windows
  report.
- Commit and push the renderer branch first, then pin, commit, and push the
  game submodule on `codex/renderer-phase-0-1`.
