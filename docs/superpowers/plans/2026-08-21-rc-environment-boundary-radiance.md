# RC Environment Boundary Radiance Plan

Status: completed

Date: 2026-08-21

Design:
`docs/superpowers/specs/2026-08-21-rc-environment-boundary-radiance-design.md`

## Implementation steps

1. Extend the RC runtime trace contract with ambient and two directional
   boundary-radiance inputs from `BTBHGPUSceneLighting`.
2. Enable those inputs only for the farthest cascade and add unobstructed
   boundary radiance after interval tracing.
3. Add source-contract tests and verify shader reflection, native/Swift tests,
   and the live GPU path.
4. Sync the renderer into the game, rebuild, capture the same deterministic
   frame, compare it with legacy and the previous RC capture, then tune from
   visual evidence.
