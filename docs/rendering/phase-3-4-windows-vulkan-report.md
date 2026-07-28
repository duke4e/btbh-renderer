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
  `ec6d3671f7fbb2740af5068288f5eb9a2b496280`
- Latest captured renderer source:
  `2620c64d5b1b5f82ceb2dc3c0eff81857f967441`
- Initial final-telemetry capture source:
  game `c5e8d16c67d243cff1722bee4fa1464207feb277`,
  renderer `4c8c92cffee2ceea59da1ec9805ea5fa032d97e9`
- Renderer implementation source (the later renderer commits are report-only):
  `fcdf7de61f74d2da03864a941a9ce9a8012de9e2`
- Branch: `codex/renderer-phase-0-1`
- CPU: AMD Ryzen 9 9950X
- GPU: NVIDIA GeForce RTX 3070 Ti
- OS: Windows 10.0.19045
- Backend bootstrap: `requested=vulkan actual=vulkan hardware=1`
- Release executable: `City Generator (r3d)/.build/release/Main.exe`
- Resolution: 1920×1080
- Seed: 424242

The current-head telemetry evidence directories are:

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

1. Port the used PBR/IBL, shadow, stencil/cull, transparency, and post-chain
   behavior instead of the current simplified forward scene pass.
2. Group the remaining per-frame scene-content rebuild and per-primitive
   bindings/draw calls. Persistent semantic instance buffers now bind and
   reuse native GPU storage directly, but geometry/material submission is
   not yet retained as a grouped render graph.
3. Re-run the strict platform-local legacy visual gate on matching current
   entity signatures without changing the retained authority to force a pass.

After those Windows items pass, the remaining platform gate is native
macOS/Metal. Only then should a later change make GPU the default and remove
the legacy OpenGL implementation.
