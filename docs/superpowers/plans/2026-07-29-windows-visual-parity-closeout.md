# Windows Visual Parity Closeout Plan

Status: Approved

**Goal:** Close Game, Combat, and Unified visual parity gaps against the
unchanged legacy authority before resuming performance work.

**Architecture:** Repair the proven GPU canvas fan defect, then isolate the
Game/Combat animated surface mismatch at the lowest proven renderer boundary.
Preserve public renderer contracts, goldens, thresholds, and normal interactive
timing.

## Task 1: Record the disproven animation-clock hypothesis

- Retain the focused Game/Combat comparison showing no material improvement.
- Keep the experimental game source and test changes reverted.
- Do not revisit this boundary without contradictory capture evidence.

## Task 2: Close the Unified control-panel fan

- Add a focused Vulkan readback test that samples the missing top segment of a
  rounded rectangle and observe it fail.
- Close the rounded-rectangle boundary before triangle-list fan expansion.
- Re-run the focused test and the full renderer suite.
- Capture Unified City once and require the control-panel cue to pass.

## Task 3: Prove and repair Game/Combat surface parity

- Compare the animated normal, material, normalized-weight, and blended-pose
  paths with focused probes.
- Add a regression test at the first proven divergent boundary.
- Capture Game and Combat and require improvement against the unchanged
  authority.

## Task 4: Regression and visual closeout

- Run focused renderer and game tests.
- Run full renderer `swift test`.
- Run full game `swift test` and `swift build -c release`.
- Run the API audit and `git diff --check`.
- Run two warmed five-mode capture series.
- Require unchanged entity signatures, clean shutdown, 1920x1080 output, and
  all unchanged visual gates.
- Update parity documentation with exact commits and results.

## Task 5: Resume performance phase

- Resume shadow draw-call consolidation only after visual closeout.
- Re-run the Windows 10-run A/B benchmark on the target FXAA/no-SSAO path and
  compare median/p95 to Run 3/4.
- Keep SMAA and SSAO measurements diagnostic-only; do not treat their cost as a
  release blocker because FXAA and Radiance Cascades are their approved
  replacements.
- Diagnose any remaining regression above 5% before Windows `.exe` closeout.

## Task 6: Replace the ambient pass

- Integrate Radiance Cascades as separately scoped follow-up work after the
  current renderer migration is visually and functionally closed.
- Establish dedicated visual and performance acceptance for that pass instead
  of weakening the current parity thresholds or goldens.
