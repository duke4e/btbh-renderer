# Windows Renderer Performance Profile Plan

**Design:** `docs/superpowers/specs/2026-07-30-windows-renderer-performance-profile-design.md`

**Status:** Approved

## 1. Establish the final-source comparison

- Verify renderer and game commits, branch, submodule pin, and existing dirt.
- Capture two warmed legacy and two warmed Vulkan Game/Combat series at
  1920x1080 with seed 424242.
- Confirm clean shutdown, matching entity signatures, output dimensions, and
  the expected Vulkan driver identity.
- Summarize frame, scene-encode, canvas, submit, and coarse fence distributions.

## 2. Make the Vulkan frame inspectable

- Add diagnostic labels in `src/gpu/btbh_gpu_scene.c` around uploads, each
  shadow map, scene geometry, opaque/transparent boundaries, SSAO stages, post,
  and AA stages.
- Add diagnostic labels in `src/gpu/btbh_gpu_canvas.c` around canvas upload and
  each target pass.
- Keep labels behavior-neutral and available in normal debug-tool captures.
- Add source-level tests or scans for stable marker coverage, then run native
  renderer tests and `git diff --check`.

## 3. Capture equivalent warmed frames

- Launch the deterministic Vulkan and legacy workloads through RenderDoc.
- Capture a settled frame after resource and pipeline warm-up.
- Record API, event hierarchy, pass durations where supported, draw/dispatch
  counts, render-target dimensions/formats, attachment loads/stores, transfer
  volume, and synchronization/present boundaries.
- Save capture paths and screenshots/event tables as diagnostic evidence, not
  goldens.

## 4. Rank and isolate the largest deltas

- Build one table mapping legacy and Vulkan work into transfer, shadows,
  geometry, transparency, post, AA, canvas, and present.
- Rank costs by absolute time and regression contribution.
- Form one falsifiable hypothesis for the highest-ranked unnecessary cost.
- Run a single-factor diagnostic configuration and compare two warmed
  120-sample series with the full configuration.
- Repeat only after accepting or rejecting the previous hypothesis.

## 5. Repair measured inefficiencies

- Add a focused failing test for any stable behavior or scheduling rule that
  can be tested automatically.
- Implement the narrowest complete fix without reducing approved visual
  quality.
- Run focused native and Swift tests, the release Vulkan build/bootstrap,
  render API audit, and `git diff --check`.
- Retain the repair only if focused performance evidence moves in the predicted
  direction.

## 6. Close the Windows profiling iteration

- Capture two final warmed Vulkan Game/Combat series.
- Reconfirm deterministic entities, output dimensions, visual cues, clean
  shutdown, and unchanged goldens/thresholds.
- Compare median/p95 against both the pre-fix final-source Vulkan baseline and
  historical Windows Run 3/4.
- Update the Windows parity/performance report with measured attribution,
  retained fixes, remaining costs, and the still-open Apple parity statement.
- Request an independent review before committing any medium or risky renderer
  change.
