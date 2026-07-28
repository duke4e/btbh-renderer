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

- Latest captured game source:
  `3926f6b749f8f71c7d42acb4c759e2a119e384c4`
- Latest captured renderer source:
  `755348465ae75bed1cb5a46f4c710d5c76f09c61`
- Initial final-telemetry capture source:
  game `c5e8d16c67d243cff1722bee4fa1464207feb277`,
  renderer `4c8c92cffee2ceea59da1ec9805ea5fa032d97e9`
- Direct-PBR renderer implementation source:
  `5af2603e8f5b8066d3574d9abe46f731fc227099`
- Procedural-IBL and material-AO implementation source:
  `755348465ae75bed1cb5a46f4c710d5c76f09c61`
- Branch: `codex/renderer-phase-0-1`
- CPU: AMD Ryzen 9 9950X
- GPU: NVIDIA GeForce RTX 3070 Ti
- OS: Windows 10.0.19045
- Backend bootstrap: `requested=vulkan actual=vulkan hardware=1`
- Release executable: `City Generator (r3d)/.build/release/Main.exe`
- Release executable size: 53,422,080 bytes
- Release executable SHA-256:
  `1719067D7604621D400BBD5477CC1B26EC4061564B4C8462E9C1EF4C0C6C9753`
- Resolution: 1920×1080
- Seed: 424242

The pre-direct-PBR final-telemetry evidence directories were:

- `.build/render-baselines/windows-gpu-4c8c92c-final10-gpu-A`
- `.build/render-baselines/windows-gpu-4c8c92c-final10-gpu-B`

All ten child processes emitted `CAPTURED` and `FINISHED`, exited with status
0, and produced PNG, QOI, cue, metrics, entity, and manifest artifacts. Every
PNG header reports 1920×1080. No telemetry-run log contains an unsupported,
fallback, fatal, or error diagnostic.

The closeout commits after `a28832a3` change the capture report/test, renderer
gitlink, audit inventory, and this report; they do not change the captured
game workload.

The first recapture shell after a usage-limit interruption omitted both
`BTBH_ENABLE_GPU_BACKEND=1` and `BTBH_RENDER_BACKEND=gpu`. It therefore
rebuilt and exercised the legacy-only executable. Its isolated
`windows-gpu-4c8c92c-final10-A` and
`windows-gpu-4c8c92c-final10-current-A` artifacts were retained as
diagnostic evidence but are excluded from the GPU benchmark. The absence of
GPU telemetry and the exact legacy entity signature proved the environment
error. The accepted A/B runs used an explicit GPU feature flag, Vulkan
selection, native library path, shader staging, and Visual Studio linker
environment. A post-capture release bootstrap independently reported
`requested=vulkan actual=vulkan hardware=1`.

## Reproducibility and visual cue gate

Entity signatures are byte-identical between Run 1 and Run 2 for every mode:

| Mode | Canonical entity signature |
| --- | --- |
| Game / Combat Lab | projectiles=13196, projectile_proxies=0, beams=1416, fields=35218, effects=15078, enemies=0, hostile_projectiles=0, active_hazards=35315 |
| Particle editor | particles=51 |
| City-block editor | scene_objects=64 |
| Unified city | assets=154, districts=9, road_meshes=3 |

Run B passed the unchanged Run A manifest and every required cue:

| Mode | Run B to Run A global SSIM | Result |
| --- | ---: | --- |
| Game | 0.999807654 | Pass |
| Combat Lab | 0.999856126 | Pass |
| Particle editor | 0.999875400 | Pass |
| City-block editor | 0.999938133 | Pass |
| Unified city | 0.999901147 | Pass |

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

Each entry contains 120 presented-frame samples. The telemetry closeout
measurements are:

| Mode | Run A median / p95 / worst ms | Run B median / p95 / worst ms |
| --- | ---: | ---: |
| Game | 31.300 / 37.472 / 49.331 | 32.258 / 39.173 / 52.004 |
| Combat Lab | 31.969 / 38.177 / 48.818 | 32.516 / 39.349 / 48.214 |
| Particle editor | 3.962 / 4.340 / 5.205 | 3.985 / 4.514 / 5.053 |
| City-block editor | 1.822 / 2.037 / 3.580 | 1.882 / 1.918 / 2.329 |
| Unified city | 2.241 / 2.591 / 3.136 | 2.334 / 2.609 / 3.035 |

Two-run means compared with the retained Windows Run 3/4 legacy means:

| Mode | GPU mean median / p95 ms | Median delta | p95 delta |
| --- | ---: | ---: | ---: |
| Game | 31.779 / 38.323 | +3.86% | +3.27% |
| Combat Lab | 32.242 / 38.763 | +7.12% | +4.32% |
| Particle editor | 3.973 / 4.427 | +124.53% | +66.98% |
| City-block editor | 1.852 / 1.978 | +262.78% | +174.86% |
| Unified city | 2.288 / 2.600 | +338.64% | +243.23% |

Game is now below the 5% median/p95 diagnostic threshold. Combat median
remains 7.12% (+2.143 ms) above the legacy mean while its p95 is below the
threshold. The editor ratios are amplified by the sub-millisecond legacy
baseline, but remain real absolute mean regressions of 1.342–2.204 ms median
and 1.258–1.843 ms p95.

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

### CPU-submit and fence-completion telemetry

Renderer `fcdf7de` adds opt-in telemetry only when
`BTBH_RENDER_PERF_LOG=1`. The normal production path still calls
`SDL_SubmitGPUCommandBuffer` and does not acquire or poll fences. The telemetry
path measures the CPU duration of
`SDL_SubmitGPUCommandBufferAndAcquireFence`, then observes completion with
non-blocking `SDL_QueryGPUFence` calls after presentation. It never waits in
the measured frame path. A controlled wait/drain occurs only while closing
the window, after the measured loop, so final samples are not silently
dropped.

The capture report selects the final 120 telemetry samples. Heavy modes
produced frame serials 1–292 and editor modes 1–152; therefore the selected
tail maps exactly to the 120 measured frames after warm-up, capture, and
recovery. Every mode reports 120 CPU-submit and 120 fence-completion samples:

| Mode | CPU submit A median / p95 / worst ms | CPU submit B median / p95 / worst ms | Fence completion A median / p95 / worst ms | Fence completion B median / p95 / worst ms |
| --- | ---: | ---: | ---: | ---: |
| Game | 0.109800 / 0.127400 / 3.697300 | 0.110350 / 0.125300 / 1.707800 | 31.469350 / 37.943700 / 66.901000 | 32.445050 / 39.742600 / 69.212700 |
| Combat Lab | 0.113550 / 0.124300 / 9.594700 | 0.113050 / 0.123500 / 10.079000 | 32.124200 / 40.588000 / 68.528400 | 32.653850 / 40.258200 / 69.251100 |
| Particle editor | 0.352250 / 0.396100 / 0.433100 | 0.353100 / 0.397100 / 0.475500 | 0.354800 / 0.399000 / 0.435900 | 0.355850 / 0.399900 / 0.478200 |
| City-block editor | 0.052300 / 0.068300 / 0.078200 | 0.053000 / 0.061400 / 0.077700 | 1.882900 / 2.098200 / 42.537200 | 1.941000 / 1.985100 / 39.803000 |
| Unified city | 0.155850 / 0.180300 / 0.273900 | 0.160150 / 0.191800 / 0.241900 | 0.173250 / 2.466100 / 46.008400 | 0.170350 / 2.624700 / 47.622000 |

These are observed fence-completion latencies, not native GPU timestamp
durations. The current same-build A/B presented-frame medians and p95 values
remain within 5% except city-block p95, whose B-to-A movement is -5.81% but
only -0.118 ms on a roughly 2 ms frame. It is recorded as short-frame
driver/scheduler variation, not a code delta. Because host state and
instrumentation differ from earlier runs, those movements are recorded as
run drift rather than credited as an optimization.

### Persistent instance-buffer binding closeout

Renderer commits `de5553bb3e316ac24bdf5c96688fe33832d34cdb` and
`2620c64d5b1b5f82ceb2dc3c0eff81857f967441` complete the first production
persistent-instance binding slice:

- semantic instance records are packed once as a 96-byte matrix, normalized
  color, and four-custom-float record;
- model and mesh batches bind the persistent SDL_GPU storage buffer directly
  instead of copying those records into the transient scene buffer each
  frame;
- unchanged semantic arrays reuse the existing upload, while a changed array
  triggers one new upload;
- mixed persistent and transient batches remain supported;
- a conflicting attempt to change one persistent handle twice in one frame
  fails before any upload;
- native validation accepts only a contiguous uploaded prefix, so an upload
  with a hole cannot expose uninitialized records to a draw.

The first real-game capture attempt at renderer `de5553b` found that the game
uses the same semantic lazy-upload contract as the legacy backend. The GPU
backend had incorrectly required a prior explicit `uploadInstances` call and
failed closed before the first game frame. The isolated
`.build/render-baselines/windows-gpu-de5553b-persistent-A` directory is
retained as diagnostic evidence. Renderer `2620c64` implements lazy upload
and unchanged-array reuse, and adds regression coverage for first upload,
reuse, changed data, same-frame conflict, mixed batches, upload holes, and
zero-transient persistent presentation.

The final Windows evidence directories are:

- `.build/render-baselines/windows-gpu-2620c64-persistent-A`
- `.build/render-baselines/windows-gpu-2620c64-persistent-B`
- `.build/render-baselines/windows-gpu-2620c64-persistent-C`

All fifteen child processes emitted exactly one `CAPTURED` and `FINISHED`
marker, exited with status 0, and produced 1920x1080 PNGs. Every series
contains 1,040 raw telemetry lines and 120 final CPU-submit and
fence-completion samples per mode. Entity JSON is byte-identical across A,
B, and C for all five modes. All five global comparisons and all fourteen
cue comparisons pass the unchanged reference manifest in every series; the
lowest global SSIM is 0.999817384. No log contains a scene failure,
unsupported operation, fallback, fatal, or error diagnostic. No golden or
threshold changed.

Run A followed a full release rebuild and showed isolated combat/city p95
movement. A same-session C control returned city to B-level timing and moved
combat below both A and B, diagnosing run-order/driver scheduling rather than
a stable code regression. B and C are therefore the steady-state performance
pair:

| Mode | Run B median / p95 / worst ms | Run C median / p95 / worst ms |
| --- | ---: | ---: |
| Game | 31.553 / 38.775 / 65.322 | 29.531 / 37.874 / 63.847 |
| Combat Lab | 33.723 / 39.943 / 70.390 | 31.467 / 37.158 / 65.816 |
| Particle editor | 3.853 / 4.160 / 4.529 | 3.980 / 4.576 / 5.421 |
| City-block editor | 1.835 / 2.035 / 2.286 | 1.830 / 2.084 / 2.489 |
| Unified city | 2.230 / 2.356 / 3.078 | 2.121 / 2.298 / 2.449 |

The B/C means compared with the immediately preceding `4c8c92c` GPU
closeout are:

| Mode | B/C mean median / p95 ms | Median delta | p95 delta |
| --- | ---: | ---: | ---: |
| Game | 30.542 / 38.325 | -3.89% | +0.01% |
| Combat Lab | 32.595 / 38.551 | +1.09% | -0.55% |
| Particle editor | 3.917 / 4.368 | -1.42% | -1.32% |
| City-block editor | 1.832 / 2.059 | -1.07% | +4.13% |
| Unified city | 2.176 / 2.327 | -4.90% | -10.49% |

No mean regression against that same-backend closeout exceeds 5%. The B/C
means versus the retained Windows Run 3/4 legacy authority remain:

| Mode | B/C mean median / p95 ms | Median delta | p95 delta |
| --- | ---: | ---: | ---: |
| Game | 30.542 / 38.325 | -0.18% | +3.28% |
| Combat Lab | 32.595 / 38.551 | +8.29% | +3.75% |
| Particle editor | 3.917 / 4.368 | +121.40% | +64.77% |
| City-block editor | 1.832 / 2.059 | +258.55% | +186.00% |
| Unified city | 2.176 / 2.327 | +316.77% | +207.44% |

Combat median remains 2.496 ms above the legacy mean. Editor percentages are
again dominated by the sub-millisecond legacy baseline, but their absolute
GPU overhead remains open. The persistent binding slice removes redundant
instance record packing/upload work; it does not remove the remaining
per-frame scene rebuild, per-primitive bindings/draw calls, simplified
lighting, or missing post chain. The historical Windows-to-Apple text and
antialiasing SSIM issue remains unresolved and is not claimed fixed here.

### Production material-channel preservation

Renderer commit `cc1d470` closes the representation half of the imported
PBR-material gap:

- glTF normal textures and normal scale now survive model import, native
  ownership, the C ABI, Swift scene preparation, model material patches, and
  scene-batch validation;
- the glTF metallic-roughness texture plus occlusion, roughness, and
  metalness factors survive the same path;
- actual glTF materials retain glTF's implicit roughness/metalness defaults,
  while the synthetic slot-zero material retains the engine defaults;
- standalone `MaterialDescriptor` values and normal/ORM texture overrides
  propagate without discarding imported scalar factors;
- normal and ORM textures are released with the owning model, including
  partial-import failure paths.

The final source passes 13/13 native CTests, 116 GPU-enabled XCTest cases
plus one Swift Testing check on real Vulkan, and 65 base XCTest cases plus
one Swift Testing check without the GPU test target. An independent review
found and closed two importer semantic gaps—occlusion strength and implicit
glTF metalness—and its affected-boundary re-review has no remaining
HIGH/MEDIUM finding.

This slice deliberately does not sample the new channels in the simplified
forward shader. It is prerequisite data/lifetime plumbing for the remaining
PBR/IBL work, not a claim of PBR visual parity, and it does not replace or
modify any golden or threshold.

### Production direct-PBR closeout

Renderer implementation commit `5af2603` replaces the simplified
albedo-times-Lambert model path with the first production direct-PBR slice:

- static and animated vertices carry world position, inverse-transpose
  normals, forward-transformed tangents, and determinant-corrected tangent
  handedness;
- the fragment path samples albedo, emission, normal, and metallic-roughness
  channels, applies normal scale plus material scalar factors, and evaluates
  Burley diffuse, GGX/Smith specular, and Schlick Fresnel for the two
  directional lights;
- camera position is transported through the neutral C ABI, and a neutral
  normal fallback texture preserves models without a normal map;
- unlit and zero-light behavior remain compatible with the retained scene
  contract;
- ambient occlusion remains transported but is deliberately not applied
  until the ambient/IBL path and separate glTF occlusion-texture semantics
  are implemented.

Independent review found one medium TBN issue: the initial shader revision
forward-transformed normals and did not correct tangent handedness for
mirrored transforms. Both static and animated shaders now use an
inverse-transpose-equivalent cofactor transform, forward-transform tangents,
and carry the instance/skin determinant sign. A real Vulkan regression with
a slanted normal under non-uniform scale would fail the old transform. The
focused re-review found no remaining HIGH/MEDIUM issue.

Fresh renderer verification passed 13/13 native CTests, 123 GPU-enabled
XCTest cases with one expected interactive-dialog skip plus one Swift Testing
check on real Vulkan, and 65 base XCTest cases plus one Swift Testing check.
The pinned game then passed 1,514 XCTest cases plus three Swift Testing checks,
the render API audit, a release build, and a hardware Vulkan bootstrap:
`requested=vulkan actual=vulkan hardware=1`.

The direct-PBR Windows evidence directories are:

- `.build/render-baselines/windows-gpu-ba7cbed-direct-pbr-A`
- `.build/render-baselines/windows-gpu-ba7cbed-direct-pbr-B`

All ten child processes emitted exactly one `CAPTURED` and `FINISHED` marker,
exited with status 0, and produced PNG, QOI, cue, metrics, entity, and manifest
artifacts. Every PNG is 1920x1080. Entity JSON is byte-identical between A and
B for all five modes, and no log contains an unsupported, fallback, fatal, or
error diagnostic.

Run B passed the unchanged Run A manifest and all required cues:

| Mode | B-to-A global SSIM | Result |
| --- | ---: | --- |
| Game | 0.999888891 | Pass |
| Combat Lab | 0.999914495 | Pass |
| Particle editor | 0.999939191 | Pass |
| City-block editor | 0.999900315 | Pass |
| Unified city | 0.999976497 | Pass |

No golden or threshold changed. The strict retained legacy manifest still
fails at its older game entity signature (`projectiles=13185` versus the
current `13196`) before image scoring, so the authority was neither bypassed
nor rewritten. The capture scenes also do not isolate every normal/ORM
response; that semantic proof comes from the real Vulkan shader regression.

The presented-frame measurements are:

| Mode | Run A median / p95 / worst ms | Run B median / p95 / worst ms |
| --- | ---: | ---: |
| Game | 31.637 / 37.783 / 50.944 | 31.167 / 38.482 / 52.860 |
| Combat Lab | 30.824 / 36.886 / 50.232 | 30.935 / 38.205 / 50.530 |
| Particle editor | 3.777 / 4.272 / 4.723 | 3.777 / 4.155 / 4.646 |
| City-block editor | 1.825 / 2.053 / 2.702 | 1.838 / 2.095 / 2.540 |
| Unified city | 2.131 / 2.257 / 2.474 | 2.151 / 2.516 / 2.682 |

The two-run means versus the retained Windows Run 3/4 legacy means are:

| Mode | Direct-PBR mean median / p95 ms | Median delta | p95 delta | Absolute median / p95 delta |
| --- | ---: | ---: | ---: | ---: |
| Game | 31.402 / 38.133 | +2.63% | +2.75% | +0.804 / +1.022 ms |
| Combat Lab | 30.880 / 37.546 | +2.59% | +1.04% | +0.780 / +0.388 ms |
| Particle editor | 3.777 / 4.213 | +113.45% | +58.94% | +2.008 / +1.562 ms |
| City-block editor | 1.831 / 2.074 | +258.71% | +188.24% | +1.320 / +1.354 ms |
| Unified city | 2.141 / 2.386 | +310.59% | +215.02% | +1.620 / +1.629 ms |

Game and Combat Lab remain below the 5% median/p95 diagnostic threshold. The
editor ratios remain dominated by the sub-millisecond legacy baseline; their
absolute fixed GPU overhead remains open and is not hidden by rounding.

Against the immediately preceding `4c8c92c` GPU A/B closeout, mean
median/p95 movements are `-1.19%/-0.50%` for Game,
`-4.23%/-3.14%` for Combat Lab, `-4.94%/-4.82%` for the particle editor,
`-1.12%/+4.87%` for the city-block editor, and `-6.39%/-8.22%` for unified
city. No same-backend regression exceeds 5%; the only movements over 5% are
unified-city improvements.

Manual inspection confirms that same-source A/B output is stable but does not
establish legacy parity. The current path still lacks the legacy ambient/IBL,
AO, shadow, stencil/cull, transparency, bloom, tilt-shift, and full post-chain
result. A cross-series comparison with the preceding GPU closeout passed the
stationary modes; the dynamic Game world cue scored 0.979288724, just below
0.98, while its HUD cue passed. The difference is concentrated in the
frame-phased central projectile/effect field, not a changed entity signature,
and the adjacent same-source A/B gate passes at 0.999888891. This diagnostic
is recorded rather than hidden by a threshold change.

The historical Windows-to-Apple SSIM issue at font, text, and antialiased
edges remains unresolved and is not claimed fixed.

### Production procedural IBL and material AO closeout

Renderer commit `7553484` closes the ambient-lighting slice used by the
current game:

- procedural skies are projected once at ambient-map creation into nine
  real spherical-harmonic diffuse-irradiance coefficients using cubemap
  solid-angle weights and normalized cosine convolution;
- the production fragment pass evaluates that irradiance at the material
  normal and samples the same procedural-sky equation at the reflection
  direction for view-dependent environment specular;
- rough reflections blend toward the low-frequency environment, use an
  analytic environment-BRDF approximation, and apply Lagarde-style specular
  occlusion;
- packed `ORM.r` and the material occlusion factor attenuate indirect
  diffuse/specular light without attenuating direct lights or emission;
- the authored procedural-sun direction is negated exactly as in the legacy
  sky setup, and a zero direction disables the sun consistently;
- `.invalid` ambient maps retain ambient-color fallback, while stale nonzero
  handles retain the renderer's fail-closed resource behavior.

The neutral C scene-lighting payload and the production shader cbuffer carry
the SH coefficients and bounded procedural-sky parameters without adding a
fifth texture sampler. Independent review found three medium issues in the
first revision: legacy sun orientation, a stale-handle specification
mismatch, and a constant-sky Vulkan test that did not prove view dependence.
All three were fixed. The focused re-review found no remaining HIGH/MEDIUM
issue.

Fresh verification passed:

- the native `/W4 /WX` build and 13/13 CTests;
- 127 GPU-enabled XCTest cases with one expected interactive-dialog skip,
  plus the Windows Swift Testing ABI/lifecycle check;
- 65 base XCTest cases plus the same Swift Testing check;
- 1,514 game XCTest cases plus three Swift Testing checks;
- the render API audit and `git diff --check`;
- a GPU-enabled release build and hardware Vulkan bootstrap:
  `requested=vulkan actual=vulkan hardware=1 claimed=1`.

The production-IBL evidence directories are:

- `.build/render-baselines/windows-gpu-7553484-procedural-ibl-A`
- `.build/render-baselines/windows-gpu-7553484-procedural-ibl-B`
- `.build/render-baselines/windows-gpu-7553484-procedural-ibl-C-diagnostic`

The required A/B pair contains ten successful child processes. Every launch
emitted exactly one `CAPTURED` and `FINISHED` marker, exited with status 0,
and produced a 1920x1080 PNG plus QOI, cue, metrics, entity, and manifest
artifacts. Entity JSON is byte-identical between A and B for all five modes,
and no log contains an unsupported, fallback, fatal, or error diagnostic.

Run B passed the unchanged Run A manifest and every required cue:

| Mode | B-to-A global SSIM | Result |
| --- | ---: | --- |
| Game | 0.999904663 | Pass |
| Combat Lab | 0.999871325 | Pass |
| Particle editor | 0.999893736 | Pass |
| City-block editor | 0.999975691 | Pass |
| Unified city | 0.999890705 | Pass |

Run A also passed the preceding direct-PBR Run B manifest. Its lowest
cross-series global score was `0.999356196` in Game, confirming that the
bounded ambient change preserves the existing captured cues. Manual
inspection shows intact HUD/editor layouts and the expected small lighting
shift without a missing scene or gross artifact. No golden or threshold
changed. The strict retained legacy manifest still stops at the documented
older game entity signature (`projectiles=13185` versus current `13196`), so
that authority was not bypassed or rewritten.

The required A/B presented-frame measurements are:

| Mode | Run A median / p95 / worst ms | Run B median / p95 / worst ms |
| --- | ---: | ---: |
| Game | 29.829 / 35.422 / 51.350 | 30.103 / 36.989 / 65.774 |
| Combat Lab | 32.536 / 39.270 / 48.708 | 30.780 / 38.262 / 46.074 |
| Particle editor | 3.379 / 3.812 / 4.037 | 3.475 / 3.962 / 5.047 |
| City-block editor | 1.438 / 1.493 / 2.091 | 1.448 / 1.667 / 1.870 |
| Unified city | 1.823 / 2.154 / 2.598 | 1.813 / 1.933 / 2.425 |

The A/B Combat median mean is `31.658 ms`, or approximately `+5.18%`
against the retained Windows Run 3/4 legacy mean. This barely crossed the
diagnostic threshold because Run A alone measured `32.536 ms`; Run B measured
`30.780 ms`. The additional same-source C control measured `30.236 ms`
median and `38.479 ms` p95, diagnosing run-order/driver scheduling variance
rather than a stable IBL regression.

B and C are therefore the steady-state diagnostic pair:

| Mode | B/C mean median / p95 ms | Delta vs direct-PBR median / p95 |
| --- | ---: | ---: |
| Game | 30.539 / 37.486 | -2.75% / -1.70% |
| Combat Lab | 30.508 / 38.371 | -1.20% / +2.20% |
| Particle editor | 3.430 / 4.022 | -9.19% / -4.54% |
| City-block editor | 1.445 / 1.564 | -21.08% / -24.59% |
| Unified city | 1.822 / 1.947 | -14.90% / -18.40% |

No stable same-backend median or p95 regression exceeds 5%. Against the
retained Windows Run 3/4 legacy means, the B/C Game deltas are approximately
`-0.19%/+1.01%` and Combat Lab deltas are `+1.36%/+3.26%`. The remaining
large editor ratios still reflect the known fixed GPU overhead against
sub-millisecond legacy baselines; their absolute B/C median overhead is
approximately `1.66 ms` for particle, `0.93 ms` for city-block, and `1.30 ms`
for unified city.

This slice is deliberately narrower than complete legacy environment parity.
It implements the game's procedural ambient-map path and material AO, not
arbitrary imported cubemaps, native prefiltered environment textures, exact
legacy BRDF-LUT convolution, reflection probes, or screen-space AO. Shadows,
stencil/cull/transparency parity, bloom, tilt-shift, and the rest of the post
chain remain open. The historical Windows-to-Apple text/antialiasing SSIM
issue also remains unresolved and is not claimed fixed.

### Production directional-shadow closeout

Renderer commits `e6bbd3d`, `b932084`, and `c74f57f` add the production
directional-shadow slice and its benchmark-driven disabled-shadow
specialization:

- up to two sorted shadow-casting directional lights own snapped,
  camera-aware 2048x2048 D32 projections;
- opaque static, animated, persistent-instance, transient-instance, custom
  mesh, and billboard geometry share the production transform contract in
  depth-only passes;
- each matching direct-light contribution receives deterministic 3x3 PCF,
  while ambient/IBL, emission, and the other directional light remain
  independent;
- the disabled-shadow path uses separate four-sampler static and animated
  pipelines, compile-time removes the PCF resources, and uploads only the
  common pre-shadow fragment-uniform prefix;
- the public backend-neutral Swift API is unchanged. Only the package-private
  C bridge and shader-path plumbing grew.

Independent implementation and performance-specialization reviews found no
remaining HIGH/MEDIUM issue. Fresh verification passed:

- the native `/W4 /WX` build and 13/13 CTests;
- 129 GPU-enabled renderer XCTest cases with one expected gated
  interactive-dialog skip, plus the Windows Swift Testing ABI/lifecycle
  check;
- the real Vulkan shadow enabled/disabled receiver-caster regression and the
  real no-shadow four-sampler Vulkan probe;
- 1,514 game XCTest cases plus three Swift Testing checks;
- the GPU-enabled release build and hardware Vulkan bootstrap:
  `requested=vulkan actual=vulkan hardware=1 claimed=1`;
- the render API audit and `git diff --check`.

The final evidence directories are:

- `.build/render-baselines/windows-gpu-c74f57f-directional-shadows-final-A`
- `.build/render-baselines/windows-gpu-c74f57f-directional-shadows-final-B`

The required A/B pair contains ten successful child processes. Every launch
emitted exactly one `CAPTURED` and `FINISHED` marker, exited with status 0,
and produced a 1920x1080 PNG plus QOI, cue, metrics, entity, and manifest
artifacts. Entity JSON is byte-identical between A and B for all five modes.
No log contains an error, fatal, fallback, unsupported, failed, or assertion
diagnostic.

Run A passed the unchanged same-environment pre-shadow manifest, with global
SSIM from `0.999796782` through `0.999959388`. Run B passed the unchanged Run
A manifest and every required cue:

| Mode | B-to-A global SSIM | Result |
| --- | ---: | --- |
| Game | 0.999829846 | Pass |
| Combat Lab | 0.999935872 | Pass |
| Particle editor | 0.999974101 | Pass |
| City-block editor | 0.999943658 | Pass |
| Unified city | 0.999905567 | Pass |

Manual inspection confirms intact game and Combat Lab worlds, HUD, tool
panel, particle field, and editor layouts. No golden or threshold changed.
The strict retained legacy authority still stops at the documented older Game
entity signature (`projectiles=13185` versus current `13196`), so it was not
bypassed or rewritten.

The final presented-frame measurements are:

| Mode | Run A median / p95 / worst ms | Run B median / p95 / worst ms |
| --- | ---: | ---: |
| Game | 31.882 / 39.217 / 52.018 | 33.077 / 40.188 / 50.111 |
| Combat Lab | 31.963 / 39.242 / 48.855 | 33.317 / 39.459 / 52.004 |
| Particle editor | 4.102 / 4.403 / 4.821 | 4.091 / 4.725 / 5.292 |
| City-block editor | 2.058 / 2.105 / 2.374 | 2.047 / 2.372 / 3.145 |
| Unified city | 2.435 / 2.585 / 2.815 | 2.397 / 2.561 / 3.069 |

The A/B means are `32.480/39.702 ms` for Game, `32.640/39.350 ms`
for Combat Lab, `4.096/4.564 ms` for the particle editor,
`2.052/2.238 ms` for the city-block editor, and `2.416/2.573 ms` for
unified city. Against the retained Windows Run 3/4 legacy means, the
median/p95 deltas are respectively `+6.15%/+6.98%`,
`+8.44%/+5.90%`, `+131.57%/+72.17%`, `+301.99%/+211.10%`, and
`+363.33%/+239.71%`. The editor ratios remain amplified by the
sub-millisecond legacy baseline, but are not hidden.

Two interleaved builds of the immediately preceding `7553484` renderer
provided a same-machine control pair. Relative to that pair, the final
median/p95 movements are `+9.96%/+10.74%` for Game,
`+2.93%/+4.11%` for Combat Lab, `+8.38%/+4.56%` for the particle
editor, `+18.19%/+23.62%` for the city-block editor, and
`+15.35%/+13.18%` for unified city.

The above-five-percent movements were diagnosed rather than waived. CPU
submission stayed effectively flat. An additional c74 diagnostic replaced
only the four-sampler unshadowed fragment SPIR-V with the exact pre-shadow
`7553484` artifact; its timings remained at the c74 level. This rules out the
specialized fragment program and trimmed uniform prefix as the cause. Game
and Combat Lab enable real shadows, so their fence cost includes the new
2048x2048 opaque depth pass and PCF receiver work. The no-shadow editors issue
no shadow draw pass, but the current runtime still eagerly creates both D32
maps, the shadow sampler, both shadow pipelines, and both shadow-capable model
pipeline families. Their unchanged CPU-submit and old-fragment diagnostic
isolate the remaining movement to that new GPU runtime/resource-residency
layer. Lazy creation and release of shadow-only resources is therefore a
recorded performance follow-up; this report does not claim that regression
was removed.

This slice deliberately covers opaque directional shadows only. It does not
implement cascades, spot/omnidirectional shadows, alpha-tested casters,
transparent casters, or arbitrary legacy shadow filters. Stencil/cull,
transparency, bloom, tilt-shift, and the rest of the post chain remain open.
The historical Windows-to-Apple text and antialiasing SSIM issue also remains
documented and unresolved.

### Production GPU file-dialog closeout

Renderer commit `9403e5a` replaces the GPU platform placeholder with the
production SDL 3 file-dialog path:

- the public Swift contract remains SDL-free while a neutral C ABI carries
  open/save kind, title, exact filter names and patterns, multiple-selection
  policy, and optional initial location;
- native code copies every caller-owned filter string and retains the SDL
  properties and filter array until the one-shot callback returns;
- Swift retains its completion bridge until native ownership completes,
  including synchronous SDL callbacks, and releases it immediately when
  native launch setup fails;
- cancel maps to `.success([])`, selected paths become file URLs, a nil SDL
  file list becomes an exact backend failure, and save-plus-multiple is
  rejected before native launch;
- the dialog callback context does not retain the GPU window, so callback
  teardown does not depend on renderer shutdown.

The normal full suite remains non-interactive. The opt-in Windows Vulkan
smoke opened the real OS dialog, cancelled it through the UI, observed the
production callback, asserted `.success([])`, and shut the renderer down
cleanly. Final verification passed 13/13 native CTests, 122 GPU-enabled
XCTest cases with the one gated smoke skipped in the normal run plus one
Swift Testing check, and 65 base XCTest cases plus one Swift Testing check.
The gated smoke also passed separately when enabled. Independent ownership
and race review found no HIGH/MEDIUM issue.

This slice changes no renderer selection, visual golden, threshold, shader,
or performance authority.

## Remaining Windows work

The following work remains before Windows visual/performance parity can be
called complete:

1. Lazily create and release shadow-only GPU resources for scenes with active
   directional shadows, then port stencil/cull, transparency, and post-chain
   behavior.
2. Group the remaining per-frame scene-content rebuild and per-primitive
   bindings/draw calls. Persistent semantic instance buffers now bind and
   reuse native GPU storage directly, but geometry/material submission is
   not yet retained as a grouped render graph.
3. Re-run the strict platform-local legacy visual gate on matching current
   entity signatures without changing the retained authority to force a pass.

After those Windows items pass, the remaining platform gate is native
macOS/Metal. Only then should a later change make GPU the default and remove
the legacy OpenGL implementation.
