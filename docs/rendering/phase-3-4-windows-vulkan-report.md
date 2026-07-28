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
  `d91f13acdab3cf7d8e4381787f654153ce4f610f`
- Renderer implementation source:
  `346adfb0e22ffe3efb6d29e56083adddd7437bf9`
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

Material-slot filtering prepares only the primitives each batch draws;
compared with the immediately preceding `d347d9b` two-run means, this reduced
game median from 43.971 ms to 35.941 ms and combat median from 43.205 ms to
35.975 ms. Renderer `346adfb` additionally retains the packed Swift instance
and batch buffers, native batch conversion storage, and camera vectors across
frames, while releasing that storage on shutdown. The scene contents are
still rebuilt each frame and the renderer still records a large number of
per-primitive pipeline/sampler/buffer bindings and draw calls. The remaining
submission overhead is therefore still above the legacy gate. The final
captures retain identical workloads across both GPU runs, confirming
same-source reproducibility. The documented legacy-to-current
entity-signature drift is small relative to these regressions, but cannot be
strictly excluded from the legacy comparison until a matching-workload gate
is available.

### Scene-submission scratch A/B

The retained-buffer change was verified with two warmed five-mode captures,
followed by a same-session A-B-A control because comparison with the older
closeout runs initially showed editor deltas above the 5% diagnostic
threshold:

- New before: `.build/render-baselines/windows-gpu-346adfb-scratch-run2`
- Old control: `.build/render-baselines/windows-gpu-adf9484-control-now`
- New after: `.build/render-baselines/windows-gpu-346adfb-scratch-run3`

The old `adf9484` control itself moved from the earlier 1.561 / 2.018 / 3.911
ms two-run editor medians to 2.040 / 2.437 / 4.230 ms for city, unified, and
particle respectively. That reproduced the apparent editor slowdown without
the new code and identifies host/run drift as the source of the historical
comparison delta.

Using the old control between the two adjacent new runs gives:

| Mode | Old control median / p95 ms | New A-B-A mean median / p95 ms | Median delta | p95 delta |
| --- | ---: | ---: | ---: | ---: |
| Game | 36.422 / 42.931 | 34.262 / 40.929 | -5.93% | -4.66% |
| Combat Lab | 36.781 / 45.294 | 35.083 / 42.094 | -4.62% | -7.07% |
| Particle editor | 4.230 / 4.699 | 4.353 / 4.921 | +2.90% | +4.73% |
| City-block editor | 2.040 / 2.397 | 2.035 / 2.544 | -0.23% | +6.12% |
| Unified city | 2.437 / 2.727 | 2.365 / 2.676 | -2.97% | -1.86% |

The city p95 delta is 0.147 ms on a roughly 2 ms frame and is not stable:
the post-control new run was only +2.24% versus the immediately preceding
control. It is recorded as short-frame/run-order noise rather than a stable
regression. No other editor mean exceeds the 5% gate, while the heavy game
and combat workloads improve.

Both retained new runs passed all 19 unchanged visual/cue comparisons, kept
the five entity signatures identical to the reference manifest, produced
1920×1080 PNGs, emitted exactly one `CAPTURED` and `FINISHED` marker per mode,
and left no game, supervisor, or comparator process running. No golden or
threshold was changed.

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
3. Replace the remaining per-frame scene-content rebuild and per-primitive
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
