# Radiance Cascades Dedicated Light Sources Plan

Status: complete

Date: 2026-08-20

## Goal

Replace screen-derived RC seed energy with explicit world-space point emitters
and integrate the first dynamic emitter in gameplay.

## Steps

1. Add Swift and native source descriptors, bounded validation, projection, and
   focused contract tests.
2. Extend RC compute uniforms with inverse view-projection and up to 16 sources;
   remove scene/ambient sampling from the seed pass.
3. Pass the existing scene inverse matrix into RC encode and update shader
   reflection/runtime contracts.
4. Author and update a dedicated player RC source in the game while preserving
   the live F9 controls.
5. Run focused renderer/game tests, full suites, GPU build/runtime smoke, diff
   checks, and independent review.

## Verification

- Public descriptor, source-only shader contract, native validation, shader
  reflection, and std140 offsets are covered by focused tests.
- A real GPU readback proves that a red dedicated point source produces
  dominant red `finalIndirect` on an otherwise unlit receiver.
- Renderer suite: 209 tests, one interactive file-dialog smoke skipped, no
  failures after updating the RC auxiliary-target contract assertion.
- Game suite: 1,625 XCTest cases plus three Swift Testing checks, no failures.
- The debug GPU game build completed and launched a responsive
  `Be The Bullet Hell` window with the high-tier player emitter enabled.

## Risks and invariants

- Keep C/HLSL uniform layouts byte-identical and 16-byte aligned.
- Bound shader work to 16 sources per RC pixel.
- Preserve the current dirty renderer and game worktrees without unrelated
  cleanup.
- Do not silently restore scene-color seeding when there are no sources.
