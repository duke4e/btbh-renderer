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

- Captured game source:
  `c5e8d16c67d243cff1722bee4fa1464207feb277`
- Captured renderer source:
  `4c8c92cffee2ceea59da1ec9805ea5fa032d97e9`
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

## Remaining Windows work

The following work remains before Windows visual/performance parity can be
called complete:

1. Port the used PBR/IBL, shadow, stencil/cull, transparency, and post-chain
   behavior instead of the current simplified forward scene pass.
2. Preserve the imported metallic/roughness and normal-map channels in the
   production material representation.
3. Replace the remaining per-frame scene-content rebuild and per-primitive
   submission with persistent, grouped GPU batches.
4. Implement and exercise the production GPU file-dialog path; automated tool
   captures do not open a dialog.
5. Re-run the strict platform-local legacy visual gate on matching current
   entity signatures without changing the retained authority to force a pass.

After those Windows items pass, the remaining platform gate is native
macOS/Metal. Only then should a later change make GPU the default and remove
the legacy OpenGL implementation.
