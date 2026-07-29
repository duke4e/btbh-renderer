# Shadow performance isolation design

Status: approved

## Goal

Determine whether the post-directional-shadow steady-state frame-time
movement is caused by renderer source or by run-to-run Windows GPU/driver
variance before changing production rendering behavior.

## Evidence and boundary

The retained pre-shadow control used game commit `c31c593e` with renderer
commit `7553484`. The current lazy-shadow A/B pair used game commit
`9fa9f1f9` with renderer commit `3830811`. Between the two game commits, the
only runtime dependency change is the renderer gitlink; the other game
changes are the audit inventory and build script.

Current CPU-submit telemetry is flat against the control. The observed
movement is in presented-frame and fence-completion time. Exact old
unshadowed fragment SPIR-V and lazy removal of all shadow-only resources have
already failed to remove the movement, so neither shader text nor shadow
resource residency alone is a supported root cause.

## Experiment

Create archive-based diagnostic source trees for the exact historical game
and renderer commits under an ignored diagnostic directory. Do not switch
the active repository or submodule away from
`codex/renderer-phase-0-1`, and do not create a Git worktree.

Reuse the renderer's existing external submodule directories only after
verifying their commits match the historical renderer gitlinks. Build the
historical native library, shader artifacts, and release game executable in
the diagnostic tree. Run a fresh five-mode capture with unchanged seed,
resolution, manifest, goldens, and thresholds. Then run a new five-mode
current-source control in the same session.

Compare:

- historical source, historical capture, and current source entity
  signatures;
- presented-frame median/p95;
- CPU-submit median/p95;
- fence-completion median/p95;
- clean shutdown, output dimensions, global SSIM, and cue SSIM.

## Decision

If the fresh historical build remains more than 5% faster than the
same-session current control in a stable mode, treat the regression as
source-linked and add the smallest practical GPU phase timing probe before
changing behavior.

If the gap does not reproduce, classify the prior movement as environmental
variance and retain the raw evidence. Do not claim a production performance
fix.

## Same-session result and encode probe

The first archive control and immediately following current run reproduced
the source-linked gap. Inspection then found that the no-shadow primitive
loop still zeroed the full enlarged fragment-uniform struct and populated
both shadow matrices even though only the common prefix was uploaded. A real
Vulkan RED probe observed four shadow-tail populations for four no-shadow
primitives.

A narrow candidate zeroed only the uploaded prefix and populated the tail
only when shadows were active. High-resolution scene timing disproved that
hypothesis: no-shadow scene encode is only 0.04–0.09 ms in the editor modes,
far below their frame-time movement. The candidate and its counter were
removed completely.

Package-internal scene-encode and canvas-present observations remain,
buffered and emitted only when the existing performance log is enabled. They
do not change the public C or Swift API. The exact `7553484` archive control
was rebuilt with the same observations. A paired current-then-historical
Vulkan run produced identical 13196 entity signatures, clean shutdown, and
1920x1080 output for all five modes. Direct current-to-historical comparison
passed every image and cue at 0.98 without changing authority, goldens, or
thresholds; global SSIM ranged from 0.999751967 to 0.999954490.

The paired medians isolate the source-linked work:

| Mode | Historical frame | Current frame | Historical scene | Current scene | Historical canvas | Current canvas |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| game | 34.7356 ms | 35.9952 ms | 3.1184 ms | 4.3145 ms | 0.6846 ms | 0.6322 ms |
| combat-lab | 35.2688 ms | 38.1968 ms | 3.0201 ms | 4.3736 ms | 0.7335 ms | 0.7627 ms |
| particle-editor | 3.6497 ms | 4.0992 ms | 0.0393 ms | 0.0476 ms | 0.7436 ms | 0.8002 ms |
| city-block-editor | 1.8211 ms | 2.0125 ms | 0.0390 ms | 0.0372 ms | 0.3722 ms | 0.3595 ms |
| unified-city | 2.2056 ms | 2.7519 ms | 0.0829 ms | 0.0926 ms | 0.5399 ms | 0.6000 ms |

Game and combat each author two `castsShadow: true` directional lights. Their
new shadow-map passes account for 1.20–1.35 ms of the paired median movement;
canvas-present and CPU-submit medians are flat. The editor scenes author
shadows off, and both scene and canvas CPU movement is at most 0.06 ms. Their
remaining movement tracks fence-completion variance, not new CPU encode
work. Frame p95 remained 7.32%, 8.40%, 17.70%, 34.43%, and 11.96% above the
paired archive run respectively, with correspondingly noisy fence p95s.

This diagnoses rather than removes the greater-than-5% movement. The shadowed
cost is expected new feature work and is a candidate for later shadow-pass
culling or update-rate optimization. The editor p95 movement is not
actionable as a scene or canvas regression from this evidence.

Both paired captures still differ from the approved legacy game authority at
the already documented 13185-versus-13196 entity signature. That is not a
renderer visual mismatch and remains unresolved.

No experiment may update a golden, lower a threshold, change renderer
selection, or claim the historical Windows-to-Apple text/antialiasing issue
is resolved.
