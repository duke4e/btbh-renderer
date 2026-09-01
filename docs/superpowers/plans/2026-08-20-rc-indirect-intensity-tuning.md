# Radiance Cascades Indirect Intensity Tuning Plan

Status: superseded by `2026-08-20-rc-cinematic-presence.md`

Date: 2026-08-20

## Goal and acceptance

Implement the approved RC intensity control without coupling visual energy to
the Low/High quality tier. Balanced `0.60` is the source-compatible Swift and
game default; RC itself remains off unless explicitly selected.

## Implementation

1. Extend the native C descriptor and validation with finite `0.0...2.0`
   intensity, then feed that value directly to the existing resolve uniform for
   all active tiers. Add focused validation and source-contract tests.
2. Extend the Swift renderer descriptor, native projection, and validation while
   preserving existing initializer call sites through a default argument. Test
   default and explicit values.
3. Parse and validate the game environment value, project it into the renderer
   descriptor, and add Subtle/Balanced/Strong selection to the interactive CMD
   launcher plus its optional third argument.
4. Synchronize canonical changes into `Vendor/BTBHRenderer`, run native and
   Swift renderer tests, game configuration tests, GPU build/bootstrap, CMD
   contract checks, and one independent review.

## Verification

- Canonical renderer: 19/19 native CTest tests and 73/73 XCTest tests passed,
  plus the Swift Testing lifecycle test.
- Real Vulkan RC regression passed, including invalid-configuration local
  fallback and same-tier recovery on the next valid frame.
- Game: GPU build and High/Strong Vulkan bootstrap passed; 1612/1612 XCTest
  tests and all 3 Swift Testing tests passed.
- Canonical/vendor parity passed for all intensity and fallback files,
  `git diff --check` passed in both repositories, and the independent re-review
  returned no findings.

## Invariants and risk

- The intensity is applied once in resolve; confidence and receiver response
  remain unchanged.
- No tier switch may change light energy for the same configured intensity.
- Invalid environment input must never reach the renderer precondition path.
- Native Metal execution remains part of the existing Phase 5 Mac acceptance
  gate; this tuning change must at least compile its shared HLSL and stage the
  existing Metal shader set.
