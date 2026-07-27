# Phase 3–4 Windows Vulkan integration report

Date: 2026-07-28

## Result

The production GPU bundle builds and runs the real release `Main.exe` through
SDL_GPU/Vulkan on Windows. Game, Combat Lab, particle editor, city-block
editor, and unified-city all completed two independent warmed capture series
without unsupported-operation diagnostics or unclean shutdowns.

This is a functional Windows/Vulkan integration pass, not a completed visual
or performance parity claim. The retained legacy renderer remains the default
and rollback path. The overall migration must not make the GPU backend the
default until the open Windows parity items below and the native macOS/Metal
gate are closed.

## Source and host

- Game source:
  `3b77065094c3bc307ff0b792d1d64a7f2446985b`
- Renderer source:
  `ca6ece5cc50b53586e09bea8618993d8a1b88507`
- Branch: `codex/renderer-phase-0-1`
- CPU: AMD Ryzen 9 9950X
- GPU: NVIDIA GeForce RTX 3070 Ti
- OS: Windows 10.0.19045
- Backend bootstrap: `requested=vulkan actual=vulkan hardware=1`
- Release executable: `City Generator (r3d)/.build/release/Main.exe`
- Resolution: 1920×1080
- Seed: 424242

The final evidence directories are:

- `.build/render-baselines/windows-gpu-ca6ece5-final-run1`
- `.build/render-baselines/windows-gpu-ca6ece5-final-run2`

All ten child processes emitted `CAPTURED` and `FINISHED`, exited with status
0, and produced PNG, QOI, cue, metrics, entity, and manifest artifacts. Every
PNG header reports 1920×1080. No final-run log contains an unsupported,
fallback, fatal, or error diagnostic.

## Reproducibility and visual cue gate

Entity signatures are byte-identical between Run 1 and Run 2 for every mode:

| Mode | Canonical entity signature |
| --- | --- |
| Game / Combat Lab | projectiles=13196, projectile_proxies=0, beams=1416, fields=35218, effects=15078, enemies=0, hostile_projectiles=0, active_hazards=35315 |
| Particle editor | particles=51 |
| City-block editor | scene_objects=64 |
| Unified city | assets=154, districts=9, road_meshes=3 |

Run 2 passed the unchanged Run 1 manifest and every required cue:

| Mode | Run 2 to Run 1 global SSIM | Result |
| --- | ---: | --- |
| Game | 0.999961898 | Pass |
| Combat Lab | 0.999988185 | Pass |
| Particle editor | 0.999914365 | Pass |
| City-block editor | 0.999916946 | Pass |
| Unified city | 0.999893473 | Pass |

No golden or threshold was changed.

The same-host GPU reproducibility result does not establish legacy visual
parity. Manual A/B inspection against the retained legacy authority still
shows material/lighting, shadow, stencil/cull, bloom, tilt-shift, and other
post-chain differences. Imported GLB base-color and emission textures now
survive identity material patches, but the production path does not yet
reproduce the legacy PBR/IBL and full post-processing result.

The approved legacy manifest also carries the older deterministic game entity
signature (`projectiles=13185`, `fields=35224`, `effects=15077`, and related
counts), while current source produces the signature above. The strict
comparator therefore correctly fails at the entity precondition before an
image score; this was not bypassed and the authority was not rewritten.

The historical Windows-to-Apple SSIM issue at font, text, and antialiased
edges remains documented and unresolved. This report does not claim that the
cross-platform issue was fixed.

## Performance

Each entry contains 120 presented-frame samples. The final measurements are:

| Mode | Run 1 median / p95 / worst ms | Run 2 median / p95 / worst ms |
| --- | ---: | ---: |
| Game | 35.301 / 44.329 / 49.599 | 36.582 / 43.963 / 54.614 |
| Combat Lab | 37.077 / 45.250 / 56.990 | 34.874 / 40.673 / 54.855 |
| Particle editor | 3.943 / 4.591 / 4.910 | 3.878 / 4.549 / 12.913 |
| City-block editor | 1.589 / 2.032 / 2.682 | 1.532 / 1.871 / 12.700 |
| Unified city | 2.068 / 2.803 / 15.742 | 1.968 / 2.220 / 3.239 |

Two-run means compared with the retained Windows Run 3/4 legacy means:

| Mode | GPU mean median / p95 ms | Median delta | p95 delta |
| --- | ---: | ---: | ---: |
| Game | 35.941 / 44.146 | +17.46% | +18.96% |
| Combat Lab | 35.975 / 42.962 | +19.52% | +15.62% |
| Particle editor | 3.911 / 4.570 | +121.00% | +72.38% |
| City-block editor | 1.561 / 1.952 | +205.70% | +171.27% |
| Unified city | 2.018 / 2.511 | +286.91% | +231.55% |

These regressions exceed the 5% diagnostic threshold. The heavy game and
combat paths show real absolute mean regressions of 5.344 ms and 5.876 ms
median. The editor ratios are amplified by the sub-millisecond legacy
baseline but also remain real absolute regressions.

The current production path materializes Swift batch, instance, material, and
bone arrays each frame, then records a large number of per-primitive
pipeline/sampler/buffer bindings and draw calls. Material-slot filtering now
prepares only the primitives each batch draws; compared with the immediately
preceding `d347d9b` two-run means, this reduced game median from 43.971 ms to
35.941 ms and combat median from 43.205 ms to 35.975 ms. The remaining
submission overhead is still above the legacy gate. The final captures retain
identical workloads across both GPU runs, confirming same-source
reproducibility. The documented legacy-to-current entity-signature drift is
small relative to these regressions, but cannot be strictly excluded from the
legacy comparison until a matching-workload gate is available.

CPU-submit and asynchronous fence-completion distributions are not yet exposed
by the production capture schema. Adding them by synchronously waiting on a
fence would perturb the measurement, so this report records that missing
instrumentation as an open acceptance item instead of fabricating values from
whole-frame time.

## Remaining Windows work

The following work remains before Windows visual/performance parity can be
called complete:

1. Port the used PBR/IBL, shadow, stencil/cull, transparency, and post-chain
   behavior instead of the current simplified forward scene pass.
2. Preserve the imported metallic/roughness and normal-map channels in the
   production material representation.
3. Replace per-frame Swift/native scene materialization and per-primitive
   submission with persistent, grouped GPU batches.
4. Add non-perturbing CPU-submit and fence-completion telemetry to the capture
   report.
5. Implement and exercise the production GPU file-dialog path; automated tool
   captures do not open a dialog.
6. Re-run the strict platform-local legacy visual gate on matching current
   entity signatures without changing the retained authority to force a pass.

After those Windows items pass, the remaining platform gate is native
macOS/Metal. Only then should a later change make GPU the default and remove
the legacy OpenGL implementation.
