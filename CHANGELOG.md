# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

0.13.0: the renderer's own backlog; nobody asked for these.
- **`BTBH_GPU_SCENE_RUNS=verify`**, a permanent debug mode: every draw that continues a same-material run (RFC 0011)
  checks that the run key covers what the passes read. CTest runs the smokes with runs in it.
- **SDF terrain updates** (Be The Bullet Hell v1's budget spikes, RFC 0007): chunk meshes go through the window's
  upload queue, and a budgeted update meshes one chunk at a time.
- **The Vulkan exit stall** (v1): a Vulkan device starts with a 32 MiB upload staging budget.
- **Descriptor pools after a load** (0.7.0's 25–28 ms frame): the prewarm lets SDL take its descriptor-set caches back.

With nothing set every frame draws byte for byte as 0.12.0's, and no shader changed. Three things do change with
nothing set, none of them in a frame: when an SDF terrain's meshes reach the GPU (in the next scene's copy pass, before
it draws, instead of in the update), the memory a Vulkan device keeps for uploads (up to 32 MiB), and a prewarm after
uploads, which now waits for the GPU once.

Tested on Windows (Vulkan, RTX 3070 Ti), on `feature/0.13.0-backlog`:
- `check.ps1` from a cold build directory passes 40/40: 0.12.0's 32 and the eight `*_runs_verify` runs. All 76 shaders
  build (SPIR-V and MSL), and the public headers compile clean under `/W4 /WX`.
- The Swift build is clean, and the full Swift suite runs 301 tests: 2 skipped, 0 failures. One source probe now
  counts the shadow pass's uniform pushes through `btbh_gpu_scene_run_push_vertex_uniforms` instead of
  `SDL_PushGPUVertexUniformData`.
- **Identity:** v0.12.0's references come from a build of the release checkout (`8a5c9e1d`), out of tree; the SDF
  terrain smoke's frames (new in this release) from the new smoke compiled against v0.12.0's header and core. Every
  image both builds write (the flags, font, scene target, height lift, prewarm, runs, RC tops, world detail, lit
  particle and SDF terrain smokes): 383 images and 18,354,048 pixels a mode; 0 differ, in each of four modes:
  - nothing set;
  - `BTBH_SMOKE_SHARED_MESHES=1`;
  - `BTBH_GPU_SCENE_RUNS=0`;
  - `BTBH_GPU_SCENE_RUNS=verify`, against the references with nothing set.

  Every smoke passes in each mode, the mesh queue smoke included. The runs smoke's four waterfront images exist only in
  the new build. The craters bench's 16 frames, each rendered after the update that switched its crater (so the queued
  meshes had to be there), are byte for byte v0.12.0's core's.
- **Run verify over the smokes** (each window's totals, 0 mismatches in all): the runs smoke 3,646 run draws checked,
  flags 72, world detail 771 (five windows), RC tops 422, SDF terrain 86, mesh queue 139, scene target 22, lazy
  creation 650. The height lift, lit particle, prewarm, font and model texture share smokes draw no run. With
  `roughness` dropped from the run key on purpose, the flags smoke logs `fragment uniform slot 0
  (BTBHGPUSceneFragmentUniforms): material_factors (+8)` and fails.
- **Shader artifacts:** no shader source changed; against v0.12.0's build 228 of 231 files are byte-identical, the
  other three the FSR 3 MSL files that differ between any two builds.
- **The header:** two macros (`BTBH_GPU_DEFAULT_UPLOAD_STAGING_BUDGET`, and the package-internal
  `BTBH_GPU_SCENE_HAS_RUN_VERIFY`); no struct, enum or function signature changed.

### Added
- **`BTBH_GPU_SCENE_RUNS=verify`** (read once a present, like `=0`): runs stay on, and every draw that continues a run,
  in the opaque, transparent, refraction, shadow (each map and face) and cascade-source passes, builds the uniforms,
  samplers, storage buffers, pipeline and stencil reference it would push and bind without the run and compares them
  with what the pass holds: each uniform block byte for byte at the size it would be pushed with. A checking draw
  pushes and binds nothing, so the frame is the frame with runs on.
  - A difference is logged with `SDL_Log` (the first 64 a window finds), as a line starting
    `BTBH_GPU_SCENE_RUNS=verify: mismatch` that names the pass, the batch, the uniform slot and the first field the
    difference touches, for example `fragment uniform slot 0 (BTBHGPUSceneFragmentUniforms): material_factors (+8)`.
    A window destroyed after it checked logs its totals.
  - Package-internal: the scene telemetry's `run_verify_draw_count` and `run_verify_mismatch_count` (a present's),
    and `btbh_gpu_window_scene_read_run_verify` (a window's life, renders into a target included, whose telemetry
    the window doesn't keep), with `BTBH_GPU_SCENE_HAS_RUN_VERIFY`.
  - Off, a push or bind costs one test more (the wrappers are forced inline): nothing measurable (Measured).
  - It started as the 0.10.1 investigation's temporary patch (the main pass only); the shadow and cascade-source passes
    and 0.11.0's and 0.12.0's state (the lift's sampler and block, the lit particles' samplers and block, the light
    factor and UV rect bits, flags2) are new.
- **CTest's `*_runs_verify`:** the eight smokes whose scenes have runs (runs, flags, world detail, RC tops, SDF
  terrain, mesh queue, scene target, lazy creation) run again with `BTBH_GPU_SCENE_RUNS=verify`; the core's mismatch
  log fails them (`FAIL_REGULAR_EXPRESSION`), and the runs and flags smokes also read the window's counts at their end
  and fail on a mismatch or on nothing checked (`Tests/btbh_gpu_smoke_runs_verify.h`). `check.ps1` and `check.sh` run
  them with the other tests. (The height lift, lit particle and prewarm smokes draw no run.)
- **The runs smoke's waterfront** (the 0.10.1 probe made coverage): v2's rainy quay (wet ground and paving, bollards,
  `SURFACE_WATER` tiles, kit pieces before and after them, a model whole and slot by slot, a kaiju's orthographic spot
  shadow with its occluder darkening, SSR with `SURFACE_REFLECTIVITY`, contact HBAO) with runs of each batch kind added
  since 0.10.0: lifted pieces on a heightfield, `INSTANCE_LIGHT_FACTOR` boxes (each instance its own light),
  `INSTANCE_UV_RECT` pieces (each its own atlas cell), lit particles, and beams with `FLAGS2_KEEP_CASCADES_LOOK` and
  without under additive cascades with `KEEP_HBAO`. Two frames of two mixes, own and shared buffers, runs on and off:
  the same bytes; presented, 74 geometry, 93 shadow and (with the cascades) 4 cascade-source draws continue a run.
- **The SDF terrain smoke:** `BTBH_SDF_TERRAIN_DUMP=<dir>` writes the frames that don't depend on the update budget's
  timing; `BTBH_SMOKE_BENCH=craters` times v1's craters (Measured), splitting each update with the package-internal
  `btbh_gpu_sdf_terrain_update_times` (the bricks, the meshing, the switch, the GPU vertices, the meshes' uploads and
  the mask's).
- **The stage bench** times the exit step by step (`BTBH_STAGE_BENCH_EXIT_EXTRA`, `BTBH_STAGE_BENCH_EXIT`), and
  `BTBH_STAGE_BENCH_STAGING_MB` sizes its staging mode's budget.
- **`BTBH_GPU_DEFAULT_UPLOAD_STAGING_BUDGET`** (32 MiB) and the environment variable
  **`BTBH_GPU_UPLOAD_STAGING_BUDGET=<bytes>`**, read when a device is made: the budget it starts with (0: none).

### Changed
- **SDF terrain: a chunk's mesh goes through the window's upload queue** (no API change). A switched chunk's custom
  mesh was updated, or made, with two transfer buffers and a command buffer of its own (0.13–0.17 ms each on Vulkan),
  after the update's budget had run out. Now the update copies its data into the queue (an update with
  `BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED`; a new chunk's mesh made with its buffers now and its data queued), and the
  window's next scene, a present or a render into a target, copies it in its one copy pass before it draws. A host
  updates the terrain before the scene that draws it, so that scene shows the new meshes with the new mask, as before
  (the mask is still uploaded in the update). When the queue can't take a chunk (`BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES`),
  it is uploaded at once as before. The dig mask's upload takes its transfer buffer from the staging pool when the
  device has a budget.
- **SDF terrain: a budgeted update without `run_jobs` meshes one chunk at a time**, checking the budget after each. A
  batch of up to 8 chunks sized by the running average of a chunk's time overran the budget by several chunks where
  they cost more than those before. With `run_jobs`, up to 8 still go out together. The chunks, their order and their
  meshes are the same.
- **A device made with `BTBH_GPU_BACKEND_VULKAN` starts with an upload staging budget of 32 MiB**
  (`btbh_gpu_device_set_upload_staging_budget`, 0.11.0), when no other device has the pool; Metal starts with none.
  Uploads take ranges of a few pooled transfer buffers (up to 32 MiB of host-visible memory, kept until the budget is
  lowered or the device shuts down) instead of one buffer each. Why: SDL 3.4 on Vulkan gives every transfer buffer an
  allocation of its own and frees a released one only in a submit that presents, or in `SDL_DestroyGPUDevice` (about
  0.1 ms each here), so every upload since the last presented frame waited for the exit (v1's `VULKAN_DestroyDevice`
  stall). `BTBH_GPU_UPLOAD_STAGING_BUDGET=0` or a budget of 0 restores 0.12.0's behaviour. Uploads and frames are the
  same to the byte.
- **The prewarm waits for the GPU once after uploads (Vulkan).** When the core submitted command buffers since the
  device's last presented frame (a level's uploads, renders into targets), `btbh_gpu_window_scene_prewarm` (and its
  `_render_to_target` form) ends with `SDL_WaitForGPUIdle`. SDL 3.4 hands every command buffer a descriptor-set cache
  from a pool and takes them back only in a submit that presents, or in a wait: a load's uploads emptied the pool, and
  the first frame after the load drew with an empty cache, making 15 descriptor pools in its draws (about 0.5 ms; in
  one of the three loads of a run 24 ms, the driver stalling in one of them). The core counts its submissions for this (package-internal macros over
  `SDL_SubmitGPUCommandBuffer` and `SDL_SubmitGPUCommandBufferAndAcquireFence`). With nothing submitted since the last
  presented frame, nothing waits.
  - The wait costs as long as the GPU still runs the load's uploads, and that time comes off the first frame: it moves
    the cost, for a small net gain, rather than adding 0.3–0.5 ms. Be The Bullet Hell v2's stage load (Deck, lockstep,
    0.12.0 → 0.13.0): the prewarm 23.2 → 46.0 ms, the first frame's present 34.7 → 7.1 ms and its submit 27.8 → 1.2 ms,
    the load frame 125.7 → 121.5 ms in all. In the renderer's stage bench, whose loads are small, the prewarm grows
    from 0.08 ms to 0.3–0.5 ms (Measured).

### Measured
Windows, RTX 3070 Ti, Vulkan, no debug device. Every number here was taken under the machine's perf lock, in three
blocks (11:39:34–11:39:52, 11:55:15–11:55:49 and 12:11:00–12:11:17), with other sessions free to run around them.
v0.12.0 is the release checkout built out of tree; a bench built against its header and core where the bench is new.
- **`BTBH_GPU_SCENE_RUNS` unset costs nothing.** The frame bench's CPU mode, its ring with runs (`ring+sorted+arena`,
  2,001 batches without the ring), three alternating processes a build: the frame without the ring encodes its geometry
  in 0.219, 0.232 and 0.222 ms against v0.12.0's 0.218, 0.221 and 0.221 (process medians), and a ring batch costs the
  same 0.09 µs. (The first build, whose wrappers weren't inlined, took 0.239–0.252 ms there.)
- **SDF terrain updates** (the SDF terrain smoke's `BTBH_SMOKE_BENCH=craters`: v1's 0.5 m cells, 32-cell chunks and
  smooth normals over 96 x 96 m, 12 craters of 13 m, some across chunk corners and some over earlier ones; a scene
  rendered after every update; three alternating processes a build, then two):

  | | v0.12.0 | 0.13.0 |
  | --- | --- | --- |
  | Updates for 12 craters, no budget | 40.5–42.2 ms (3.4–3.5 a crater) | 33.0–33.1 ms (2.75) |
  | Of it, the chunk meshes' uploads (57: 32 made, 25 updated)¹ | 7.3–8.5 ms | 0.4 ms |
  | Of it, the dig mask's 12 uploads¹ | 1.0–1.1 ms | 0.5–0.6 ms (from the pool) |
  | Slowest call with a 2,000 µs budget | 3.5–4.2 ms | 2.33–2.36 ms |
  | 90th percentile of those calls | 2.4–2.7 ms | 2.24–2.25 ms |
  | The renders after the unbudgeted updates (24) | 2.7–3.4 ms | 2.9 ms |

  ¹ v0.12.0's split comes from the branch before the fix: v0.12.0's upload path with 0.13.0's timers.

  - Where an unbudgeted crater's update goes (0.13.0's timers, the branch before the fix): bricks 1.05 ms, meshing
    1.6 ms, the switch 0.07, the GPU vertices 0.01, the meshes' uploads 0.6–0.7 and the mask 0.09. The renderer's
    part was the uploads: a fifth of the update, all of it after the budget.
  - The queue alone (no budget) gave 34.2–35.0 ms; the slowest budgeted call stayed at 4.06–4.13 ms until the meshing
    went one chunk at a time. That call's split: 232 bricks in 1.71 ms, then 5 chunks meshed in one batch, 2.37 ms.
  - v0.12.0 with a 16 MiB staging budget also got to 34.9–35.2 ms: its own-buffer meshes take pooled buffers since
    0.11.0. The queue saves the per-chunk submission on top, and needs no budget.
- **The exit** (the stage bench: v2's Deck-preset stage, 140 uploads a load, then its exit timed step by step):

  | | v0.12.0 | 0.13.0 |
  | --- | --- | --- |
  | After a stage's frames, nothing pending | 51.1–53.7 ms | 50.2–53.7 ms |
  | 500 textures, meshes and instance buffers made just before it (1,500 uploads, no frame after) | 245.9–246.2 ms | 82.1–82.9 ms |
  | Of it, `btbh_gpu_device_shutdown` | 232.4 ms | 70.1–70.6 ms |
  | Making those 1,500 uploads | 207–210 ms | 125–126 ms |

  - With 2,000 extra (6,000 uploads), the branch before the default took 850 ms without a budget and 136 ms with
    32 MiB.
    The stall grows with the uploads made since the last presented frame: about 0.13 ms each.
  - SDL instrumented (a scratch build, reverted): with the 1,500 uploads, `SDL_DestroyGPUDevice` found 2,029
    allocations (1,418 MB) and spent 182 ms in `vkFreeMemory` (0.09 ms each), 2 ms in SDL's region bookkeeping and
    36 ms in `vkDestroyDevice`; with a 128 MiB budget, 33 allocations, 16.5 ms and 31 ms.
  - What doesn't help (without a budget): destroying nothing before the window (247 ms), `SDL_WaitForGPUIdle` first
    (252 ms). A
    canvas-only present after the destroys moves 172–183 ms into that present (the exit stays 241–252 ms).
  - Budgets of 8, 32 and 128 MiB: 80.5, 77–79 and 68–70 ms.
- **The first frame after a load** (the stage bench, 4 loads a process, two processes a build):

  | | v0.12.0 | 0.13.0, no budget | 0.13.0 |
  | --- | --- | --- | --- |
  | Its scene present | 1.51–2.38 ms | 1.00–1.13 ms | 1.00–1.07 ms |
  | Of it, the opaque pass's encode (where SDL made the descriptor pools) | 0.90–1.72 ms | 0.59–0.65 ms | 0.59–0.63 ms |
  | Its canvas's submit (SDL freeing the load's upload buffers) | 14.0–15.9 ms | 13.5–14.9 ms | 2.0–6.0 ms |
  | The frame | 15.8–17.8 ms | 14.5–16.1 ms | 3.1–7.1 ms |
  | The prewarm | 0.08 ms | 0.33–0.48 ms | 0.28–0.45 ms |
  | The load's 48 kit meshes | 5.6–7.9 ms | 5.6–7.5 ms | 1.2–1.8 ms |

  - SDL instrumented: before the fix, the first frame after each load made 15 descriptor pools, 0.50–0.51 ms, and
    once in three loads 24.4 ms (one `vkCreateDescriptorPool` took 23.9 ms): the 0.7.0 frame. With the fix, none in
    6 loads. The first frames a device ever draws make theirs still (15 pools and 55–87 uniform buffers in each of
    the first 3–4 frames, 0.5–0.6 ms).
  - The lazy creation log can't show this: the pools are made inside SDL's draws, not in a call it times.

### Upgrading
- Nothing to do. A host that wants 0.12.0's uploads back sets `BTBH_GPU_UPLOAD_STAGING_BUDGET=0` or calls
  `btbh_gpu_device_set_upload_staging_budget(device, 0)`; a host that set its own budget keeps it.
- A host that calls the prewarm in the middle of play, right after uploads or renders into targets, now waits there for
  the GPU to finish them. After a load the prewarm takes longer by about the time the GPU still needs for the load's
  uploads (v2: 23 → 46 ms), and the first frame shorter by more (its present 34.7 → 7.1 ms).
- No public struct changed (bindings stay as they are). The package-internal scene telemetry struct, which the Swift
  layer reads, grew two fields at its end.

### Known
- `BTBH_GPU_SCENE_RUNS=verify` checks what goes through its helpers: the uniform blocks, samplers, storage buffers,
  stencil reference and pipeline a batch pushes and binds for its run. The vertex and index buffers (bound per
  geometry) and the cascade-source pass's pipeline (bound per geometry, before the run's check) aren't part of a run
  and aren't checked.
- The dig mask is still uploaded in the terrain's update (one submission, about 0.1 ms; half that from the staging
  pool). A host that updates the terrain while a presented scene waits for its canvas still gets the new mask in that
  frame with the old meshes, as in 0.12.0.
- The first three or four frames a device ever draws still each make their descriptor pools (15, about 0.5 ms): SDL
  makes a cache for each command buffer in flight.
- Not run here: Metal (`check.sh` on the Mac), the Steam Deck, the Vulkan validation layers, and v1 or v2 themselves.

## [0.12.0] — 2026-11-23

0.12.0: two additions to `BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO`, both asked for by Be The Bullet Hell v2
(0.11.0's two Known items about the flag):
- **A per-batch opt-out from its transparent routing**, in a new second byte of batch flags
  (`BTBHGPUSceneGeometryBatch.flags2`, `BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK`): v2's weapon beams and glows
  keep the cascades' point-light boost while its light overlays draw as without the cascades.
- **Its ambient part with contact AO and the SSAO:** with the flag, contact AO (`ssao_contact` 1) and the SSAO
  (`ssao_enabled` 1) under additive cascades darken what they darken without the cascades. With nothing glowing, a frame
  is the frame without the cascades. **This changes frames drawn with the flag** under additive cascades with contact
  AO (v2's setting since 0.11.0) or the SSAO.

Without the flag, and with it and the HBAO's `ssao_contact` 0 (0.9.0's path), every frame draws byte for byte as
0.11.0's. One shader changed: `production_model.frag.hlsl`, whose three radiance-cascades variants
(`production_model_rc*.frag`) compile anew; the plain variants and every other shader compile to 0.11.0's files.

Tested on Windows (Vulkan, RTX 3070 Ti), on `feature/0.12.0-keep-hbao`:
- `check.ps1` with the GPU smokes passes 32/32 (no new test executable); all 76 shaders build (SPIR-V and MSL), and
  the public headers compile clean under `/W4 /WX`.
- The Swift build is clean, and the full Swift suite runs 301 tests: 2 skipped, 0 failures.
- **Identity:** v0.11.0's references come from a build of the release checkout (`84cfff53`), out of tree. Every image
  both builds write (the flags, font, scene target, height lift, prewarm, runs, RC tops, world detail and lit particle
  smokes): 358 images and 17,714,048 pixels a mode; 0 differ, in each of three modes:
  - nothing set;
  - `BTBH_SMOKE_SHARED_MESHES=1`;
  - `BTBH_GPU_SCENE_RUNS=0`.

  The SDF terrain and mesh queue smokes pass in each mode. The flags smoke's 18 new images exist only in the new build.
  Its gallery's seven new unset frames were checked against the new smoke built against 0.11.0's header and core: 35
  gallery images, 0 differ, in each mode. Run on 0.11.0's core, the new smoke fails where it should: contact AO with
  `KEEP_HBAO` by day differs from the cascades off (233 pixels, up to 26 levels).
- **Shader artifacts:** against v0.11.0's build only `production_model_rc.frag`, `production_model_rc_2d_shadows.frag`
  and `production_model_rc_unshadowed.frag` differ (SPIR-V and MSL; their reflection is the same), with the three FSR 3
  MSL files that differ between any two builds.
- **The header:** the batch is 288 bytes, with `flags` at 253, `flags2` at 254 and `ssao_reserved[1]` at 255; batch bits
  0–3, 5 and 6 are taken (bit 4 RFC 0013 phase 2's, bit 7 free); flags2 bit 0 is taken; the 34 `HAS_` macros are each
  defined once.

Be The Bullet Hell v2 tested the candidate (957533cc) on its 0.11.0 adoption.
- **Nothing set:** 0 pixels differ from 0.11.0 by day, at night, in rain, on the Deck preset, at the map edge and at
  dusk. Two runs of each build match. `flags2` stays 0 from `Default`.
- **Its glow views**, which set `KEEP_HBAO` with contact AO, change only through the new ambient part.
  - At night, the lamp pools on cars and facades get darker: 5136 pixels by over 8 levels, up to 91.
  - At dusk, 5921 pixels change, 78 % of them brighter, because contact AO no longer darkens the cascades' bounce.
- **`KEEP_HBAO` with contact AO** under additive cascades with nothing glowing: 0 pixels differ from the cascades off,
  lamp pools included, with SSR and at dusk. 0.11.0 differed there by 5106 pixels, up to 94 levels.
- **`KEEP_CASCADES_LOOK`:**
  - Its flagged batches get the point-light boost back; the others stay as under `KEEP_HBAO`.
  - Without the cascades the flag changes nothing.
  - With its prewarm copying the flagged batches, the lazy creation log shows nothing after the first frame.
- **GPU** (cascades Low, stage 3 at night, medians):

  | Preset | 0.11.0 | 0.12.0 |
  | --- | --- | --- |
  | Deck, static | 1.637 ms | 1.638 ms |
  | Deck, fight | 1.692 ms | 1.691 ms |
  | High, static | 4.030 ms | 4.030 ms |
  | High, fight | 4.276 ms | 4.282 ms |

  On High the AO's part grows by 0.06 ms, as the HBAO now runs before the cascades, and the main pass shrinks by
  0.01 ms.

### Added
- **`BTBHGPUSceneGeometryBatch.flags2`**, the batch flags' second byte: byte 254 on 64-bit, right after `flags`, the
  old `ssao_reserved[0]`. The batch keeps its 288 bytes; `ssao_reserved` keeps one byte (255). The byte was in the run
  key already and stays there. Its bits are its own (they don't go in `flags`). An unknown bit fails the present and
  the prewarm with `BTBH_GPU_INVALID_ARGUMENT`. Batch bit 7 stays free, and bit 4 stays RFC 0013 phase 2's.
- **`BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK`** (flags2 bit 0) and
  **`BTBH_GPU_SCENE_HAS_BATCH_KEEP_CASCADES_LOOK`**. Under additive cascades with `KEEP_HBAO`, a transparent unlit batch
  with the bit draws as under the cascades without `KEEP_HBAO`: with the cascades' scene shader
  (`production_model_rc*.frag`), which adds the point lights, the dark scene's darkening, spot and glow, and unlit 2
  and 3's sun shadow, instead of the fast unlit shader. Its fragment block is the one it has there, too (the flag's
  scene-target bits cleared), so it draws byte for byte as without `KEEP_HBAO`.
  - Only a batch the fast unlit shader would draw is affected (unlit, in the transparent pass, no surface variant, no
    world detail texture, no table with `map_layers`), and only under additive cascades with `KEEP_HBAO`, outside the
    cascades' debug views. Elsewhere it changes nothing.
  - The batch takes the pipeline it takes under the cascades without `KEEP_HBAO`, and the prewarm makes it for a
    batch with the bit. No new shader.
- The flags smoke (`check_keep_cascades_look`): a beam (a long thin additive unlit box) by the neon, nothing glowing,
  additive cascades, no AO, no shadow and the sun's. With `KEEP_HBAO` the frame is the frame without the cascades; with
  the bit on the beam too it is the frame under the cascades without `KEEP_HBAO`, byte for byte, and the two differ
  (146 pixels, up to 12 levels). With contact AO and street lamps, and with the HBAO, the bit changes the beam's pixels
  alone (148 and 147). The bit changes nothing without the cascades, under cascades that aren't additive, under
  additive ones without `KEEP_HBAO`, in a debug view, on an opaque unlit box, on every batch of a street without a beam,
  or on the beam drawn lit. Prewarming the three flagged scenes in a fresh window made 34 pipelines; their renders then
  made 0.
- The refusals: flags2 bits 1 to 7, each with bit 0, at the present and at the prewarm; bit 0 alone is taken. The order
  test: flags2's offset and bit, the macro, and flags2 in the run key (61 fields over 288 bytes).
- Swift: nothing. The Swift layer sets no batch flags and doesn't expose `KEEP_HBAO`.

### Changed
- **`BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO`'s ambient part applies with contact AO and the SSAO** (asked for
  by Be The Bullet Hell v2). Under contact AO, v2's opaque lit surfaces in a street lamp's pool (a car's roof, a
  facade) came out up to 94 levels brighter than without the cascades, with the flag or without. The cause: the
  cascades' scene shader carries the street lamps' light in the emission target, and contact AO keeps the emission
  target's light undarkened. It does the same for an opaque unlit surface's colour (0.11.0's smoke: 237 pixels, up to
  26 levels). 0.9.0 to 0.11.0 applied the ambient part with the HBAO's `ssao_contact` 0 only.
  - **The SSAO** (`ssao_enabled` 1): the opaque pass writes the scene's own ambient targets (as with the HBAO), and the
    SSAO's resolve darkens the ambient light from them, as without the cascades (its mode 1; `ao_contact_strength` and
    `ao_direct_share` no longer apply there).
  - **Contact AO** (`ssao_enabled` 2, `ssao_contact` 1): the opaque pass writes the scene's own targets too, with the
    emission in the ambient target's rgb (the cascades' scene shader, `unlit_enabled` bit 10), which contact AO reads
    instead of the emission target. Contact AO reads no ambient light, and the emission target carries the albedo for
    the cascades' resolve. The HBAO's passes run after the opaque pass and before the cascades, over the scene the
    cascades then compose with their bounce light. So contact AO darkens the scene's own light as it does without the
    cascades, and the bounce light isn't darkened: without the flag, as with it in 0.11.0, contact AO comes after the
    cascades and darkens it with the rest. The HBAO's debug view (`ssao_debug_view`) stays after the cascades and shows
    the occlusion as before.
  - Screen-space reflections weigh each surface by its reflectivity's luminance there, as with the HBAO under the flag.
  - **No AO:** nothing darkens the scene, so a frame with nothing glowing already equals the frame without the cascades,
    with the flag or without. The flag changes the transparent pass alone there, as before.
  - Under the omni shadow a pixel may differ by a level now and then (the cascades' variant of the scene shader rounds
    that light apart from the plain one, as 0.10.0 noted): in the smoke, at most 1 pixel by 1 level a frame.
  - **This changes frames drawn with the flag under additive cascades with contact AO or the SSAO.** With nothing
    glowing they now match the cascades off. With something glowing, the bounce light near occluded corners is no
    longer darkened by contact AO.
  - The flags smoke (`check_keep_hbao_contact`): nothing glowing, additive cascades, the day street and a night street
    with two street lamps (over the metal box and before the building's front) and the neon, through no shadow, the
    sun's and the omni shadow. With the flag, contact AO and the SSAO render the frame without the cascades: 0 pixels
    differ (under the omni shadow at night 1 pixel by 1 level). Without it they differ (contact AO by day 233 pixels, up
    to 26 levels, the unlit box; at night 1,654 to 1,801, the lamps' pools up to 6 levels; the SSAO 1,544 to 2,331
    pixels, up to 6). No AO renders the frame without the cascades with the flag and without (under the omni shadow 1
    pixel by 1 level). On a street only the bounce light lights, contact AO and the SSAO with the flag render as no AO,
    byte for byte; without it they darken that light (950 and 663 pixels). Contact AO's debug view is the same with the
    flag, without it and without the cascades. With the reflections, contact AO and the SSAO under the flag render as no
    cascades with `SSR_SURFACE_REFLECTIVITY` (0 pixels). 0.11.0's checks: the SSAO case and the opaque unlit box by the
    neon now render the frame without the cascades too, and contact AO and the SSAO left the flag's "ignored" cases (no
    AO joined them).
  - The gallery's new unset frames (`lamps-contact`, `lamps-contact-additive`, `lamps-contact-additive-omni`,
    `lamps-ssao-additive`, `dark-bounce-contact-additive`, `beam-additive`, `beam-contact-additive`) match 0.11.0's
    core's to the byte.
- `btbh_gpu_scene.c`: the HBAO's three passes moved into `btbh_gpu_scene_encode_hbao`, which the old place calls as
  before.
- The Swift suite's source check of the SSAO's mode reads the new condition.

### Measured
Under the machine's perf lock (10:16:45 to 10:17:04), the flags smoke's bench at 1920x1080 on an RTX 3070 Ti
(Vulkan): the night street with its lamps and neon, additive cascades, contact AO (`BTBH_SMOKE_BENCH=contact`), and
the HBAO with SSR (`=hbao`). A frame takes about 1 ms there, so these are small numbers.
- **Off:** nothing to pay.
  - v0.11.0's core and shaders against this build's, three alternating processes of 20 turns, unset: median
    0.967 ms against 0.966 ms a frame (fastest 0.956 against 0.951).
  - The shaders alone (this core, 0.11.0's shaders in a second window, 30 turns): +0.003 ms median with contact AO
    unset, +0.004 ms with the HBAO and SSR unset. Within the turns' spread.
- **`KEEP_HBAO` with contact AO** against unset, this build: -0.011 ms median (0.955 against 0.966), the same in each
  of the three runs, and -0.010 ms against 0.11.0's `KEEP_HBAO` with contact AO (its transparent part alone). The
  extra work is a branch in the cascades' scene shader; the HBAO's passes only move.
- `KEEP_CASCADES_LOOK` costs a batch the scene shader instead of the fast unlit one; not measured.

### Upgrading
- Nothing to set: a host without `KEEP_HBAO`, or with it and the HBAO's `ssao_contact` 0, draws as before. Rebuild the
  scene shaders: with 0.11.0's `production_model_rc*.frag`, `KEEP_HBAO` with contact AO would keep the ambient light
  from contact AO instead of the emission. Rust hosts with bindgen regenerate the bindings: `ssao_reserved` (two bytes)
  becomes `flags2` and a one-byte `ssao_reserved`; `..Default::default()` leaves `flags2` 0. A host that left garbage
  in `ssao_reserved[0]` now gets `BTBH_GPU_INVALID_ARGUMENT`.
- v2: nothing to change for the lamp pools: `KEEP_HBAO` with contact AO now matches the cascades off with nothing
  glowing. Give the weapon beams' and glow sources' batches `flags2 = BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK`,
  and keep the light overlays without it. Prewarm with the flagged batches.

### Known
- **0.11.0's two Known items about `KEEP_HBAO` are settled**: the beams and glows keep the cascades' look with
  `KEEP_CASCADES_LOOK`, and lit surfaces in a street lamp's pool match the cascades off under contact AO with the flag.
- **`KEEP_CASCADES_LOOK` brings back everything the cascades add to a flagged batch**, a glow's white-out included.
  Be The Bullet Hell v2 flags only its weapon beams' own draws: on its whole effect emitter, the flag brought back the
  white-out of its city idle effects (the signs' neon halos and the power lines' surges) at a building's foot (up to
  180 levels). (Corrected in 0.13.0: this said an additive fire flipbook.)
- **macOS (Metal) and a real Steam Deck** aren't verified.

## [0.11.0] — 2026-11-23

0.11.0: everything in it was asked for by Be The Bullet Hell v2, and all of it is opt-in. With nothing set, every frame
draws byte for byte as 0.10.0's, and every shader that 0.10.0 had and 0.11.0 doesn't change compiles to the same files.
- **RFC 0012's phase 1:** a static batch lifted by a heightfield in the vertex stage
  (`BTBHGPUSceneGeometryBatch.height_lift`), so ground pieces and their overlays follow a slope smoothly instead of
  stepping cell by cell.
- **Queued uploads:** custom-mesh updates queued into the next scene (`BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED`). This grew
  into one upload queue a window, with queued or in-place instance buffer uploads (`btbh_gpu_instance_buffer_upload_ex`
  with `BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED` or `_NO_CYCLE`) and partial mesh updates
  (`btbh_gpu_custom_mesh_update_range`), so a city or kit chunk uploads only what changed, all of it in one copy pass
  a frame.
- **RFC 0013's phase 1:** a see-through surface lit per pixel (`BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE`) with a normal and
  thickness map, the soft and near fades and fog at its own depth, and a UV rect per instance
  (`BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT`). Phase 2 (a transmittance map in sun space) follows v2's captures.
- **A light factor per instance** (`BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR`): the instance's custom x scales its
  emission, its glass's lit panes and its lamp lens's glow, on screen and in the radiance cascades' source field, for
  v2's district blackouts.
- **From v2's night perf round:** the lazy creation log (`btbh_gpu_window_lazy_creation_report`,
  `BTBH_GPU_LOG_LAZY_CREATION=1`), which names what a prewarm missed; and the upload staging budget
  (`btbh_gpu_device_set_upload_staging_budget`). The budget keeps a level's uploads from leaving SDL hundreds of
  transfer buffers to free in the first frame after the load.
- **Fixes:**
  - `KEEP_HBAO`'s transparent part applies with any ambient occlusion.
  - A dropped frame that copied queued uploads is submitted, so they land.
  - Frame telemetry no longer hands SDL a fence while its frame may still run (a hidden or minimized window's
    frames), and its finish waits for every pending frame.

**Shaders:** 76, against 0.10.0's 71.
- New: `production_model_lift.vert` and `production_model_detail_table_lift.vert` (the lift);
  `production_particle_lit.frag`, `production_particle_lit_2d_shadows.frag` and
  `production_particle_lit_unshadowed.frag` (the lit particle); and the include `production_light_factor.hlsli`.
- Changed:
  - `production_model.vert` and the four built from it (the two lift shaders among them): the UV rect's test;
  - the six `production_model*.frag`, `production_model_unlit.frag` and `production_rc_material_source.frag`: the
    light factor.
- Rebuild them all with the core.

Tested on Windows (Vulkan), on the merged `release/0.11.0` (the code at `5f40c33a`; only this CHANGELOG follows it),
from a cold build:
- `check.ps1` with the GPU smokes passes 32/32. That is 0.10.0's 26 and:
  - the height lift's native test and smoke;
  - the mesh queue smoke (73 byte-for-byte checks, each rule it checks broken once on purpose and caught);
  - the particle's native test and the lit particle smoke;
  - the lazy creation smoke.

  All 76 shaders build (SPIR-V and MSL), and the public headers compile clean under `/W4 /WX`.
- The Swift build is clean, and the full Swift suite runs 301 tests: 2 skipped, 0 failures.
- **Identity with nothing set:** the images 0.11.0's first merge wrote (`fc2784ea`, `btbh-rel-0110-outs*`; those match
  0.10.0's candidate's to the pixel) against this build's. Every image that exists there: the eight image smokes' and
  the height lift smoke's 246 PNGs, and the prewarm smoke's 32 frames. 278 images and 16,219,008 pixels a mode;
  0 differ, in each of three modes:
  - nothing set;
  - `BTBH_SMOKE_SHARED_MESHES=1`;
  - `BTBH_GPU_SCENE_RUNS=0`.
- **Each branch's new images:**
  - against the light factor branch's tip (`17d0fc35`, which holds RFC 0013's phase 1 too): all of its images,
    including its flags smoke's 79 and its lit particle smoke's 60. 359 images and 17,739,648 pixels a mode; 0 differ,
    in each of the three modes;
  - against RFC 0013's own tip: the lit particle smoke's 60 images; 0 differ;
  - the queued uploads' and the prewarm log's branches add no images, and their outputs equal the references above.
- **Shader artifacts:** every SPIR-V, MSL and reflection file equals the light factor branch's build's. The exceptions
  are three FSR 3 MSL files, which differ between any two builds of the same source. Against `fc2784ea`'s build, only
  the 13 changed shaders listed above differ, and the lit particle's three are new.
- **The merged header:**
  - the batch is 288 bytes, with `world_detail_table` at 272 and `height_lift` at 280;
  - batch bits 0–3, 5 and 6 are taken (3 is the UV rect, 5 no spot shadow, 6 the light factor), and bit 4 is free;
  - post bits 0–2 are taken;
  - the lighting struct is 3,340 bytes, with byte 3,337 free (`directional_shadow_reserved`) and
    `spot_occluder_flags` at 3,338;
  - the 33 `HAS_` macros are each defined once.
- **The lazy creation smoke:**
  - frames with the log on, and frames prewarmed under a staging budget, equal the frames with nothing set;
  - under a budget, every upload path arrives whole: 25 rounds of 64 textures, and 25 rounds of 24 instance buffers
    and 16 meshes of their own, each uploaded before anything is read back (300 rounds in a stress run);
  - frame telemetry's fences: a hidden window's frame still running when the telemetry is turned off has finished by
    the time its fence is released (the code before the fix fails this), and 296 frames left unpolled keep the newest
    256 samples.

  The mesh queue smoke also passes with an 8 MiB staging budget forced on (a scratch run).

macOS (Metal), the Vulkan validation layers and a Steam Deck aren't verified.

Be The Bullet Hell v2 tested the candidate (4dec4eea) on an RTX 3070 Ti, every number under the machine's perf lock.
It ran on its main at e397c643, on 0.10.0 with its draw-order fix.
- **Nothing set:** 0 pixels differ from 0.10.0 in its six views (day, night, rain, the Deck preset, the map edge, the
  cascades at night), checked twice.
- **The height lift** on its street-level kit, the night's street wash and its ground marks (`KEEP_NORMALS` on the
  last two):
  - The CPU path's per-cell terraces are gone, with no seams and no z-fighting.
  - GPU main pass: +0.055 ms on the Deck preset, +0.14 ms on High.
  - CPU frame: mean 2.22 → 1.78 ms, p99 4.83 → 2.44 ms. Laying and uploading its marks: 0.70 → 0.06 ms.
- **Queued uploads:**
  - The city part's p99: 0.24 → 0.03 ms. The frame's p99: 2.30 → 1.89 ms. Instances sent: 2.00M → 1.40M.
  - A blackout wave's worst frame: 1.3 ms, against single 7–53 ms stalls with plain uploads.
  - 0 pixels differ.
- **Lit smoke**, one `LIT_PARTICLE` batch with a UV rect, six full-view cards. GPU cost against its fake (whose tent
  filter takes 16 gathers):

  | Preset | Fake | Lit |
  | --- | --- | --- |
  | Deck | +0.17 ms | +0.09 ms |
  | High | +0.37 ms | +0.31 ms |

  Its look is still being tuned.
- **The light factor:**
  - At 1 it draws as unflagged.
  - Half its city dark takes 2239 draws, against 2322 with its dark batches and 2237 with no blackout. Its dark keys
    can go.
- **`KEEP_HBAO` with contact AO:** the neon's overlays now match the cascades off. The pixels that differ fell from
  19 % (up to 203 levels) to 5.6 % (up to 94).
- **The lazy creation log and the staging budget:**
  - The log named its 566 ms miss: a skinned pipeline its own prewarm left out.
  - With a 128 MiB budget, stage 1's first present went from 925–1054 ms to 1.7 ms.
  - Stage 2's start frame went from 170–179 ms to 104–108 ms, and its present from 42 ms to 3 ms.

0.10.0's Known item on v2's frames is settled:
- Its 1-level differences are the recompiled shaders: 0.10.0's core with 0.9.0's shaders matches 0.9.0.
- Its rain glint came from v2's draw order, which changed from process to process. Runs on and off match.

### Added
- **`btbh_gpu_window_heightfield_create(window, heights, width, height, report)`**: a heightfield for the lift, width x
  height samples (2..`BTBH_GPU_HEIGHTFIELD_MAX_SIZE`, 4096), sample (i, j) at the texel centre. Kept as RGBA16F, the
  height and its slope along u and v, filtered bilinearly and clamped at the edges; the heights also stay on the CPU.
  Heights are halves (within 8..16 units they step by 1/128): a height that isn't finite or lies past ±32768 is refused,
  naming the sample. Only a lift takes one: every other texture slot refuses it with `BTBH_GPU_INVALID_ARGUMENT` (a
  batch's four, a world detail's three, a table's `map_layers`, the post LUT, the canvas's texture and target, the
  render target, the capture destination), and so does `btbh_gpu_texture_update_rgba8`. `btbh_gpu_texture_destroy`
  frees it.
- **`BTBHGPUSceneHeightLift`** (40 bytes: `heightfield`, `world_to_uv[4]`, `height_scale` (0 means 1), `height_offset`,
  `flags`, `reserved`) and **`BTBHGPUSceneGeometryBatch.height_lift`**, appended at 280 (the batch is 288 bytes). After
  the instance's matrix and the wind every vertex rises by `height_scale * h + height_offset`, h read at its flat world
  xz through `world_to_uv`; its normal and tangent tilt with the slope, unless
  **`BTBH_GPU_SCENE_HEIGHT_LIFT_KEEP_NORMALS`** (overlays that keep their shading). In every pass that draws a static
  batch: opaque, transparent, unlit (the fast unlit pipeline too, under additive cascades with `KEEP_HBAO` as well),
  additive, soft particles, refraction, with or without a world-detail table. FSR 3's previous position is lifted at its own xz (a static instance on a fixed field
  moves 0); the opaque front-to-back sort lifts a lifted batch's centres from the CPU heights; the radiance cascades'
  material sources take it unlifted (they project straight down). Several batches may share a lift; a lift with
  `heightfield` NULL draws as none.
  - Refused, naming the batch (`BTBH_GPU_INVALID_ARGUMENT`): a mapping that isn't finite or has a zero scale, a scale
    or offset that isn't finite, an unknown flag, `reserved` not 0, a heightfield that isn't one or is another
    window's; with a heightfield set, a batch that is animated, a billboard, dug (`dug_by`), or that would cast a
    shadow. **A lifted batch casts no shadow in 0.11.0**: it must be transparent, or set
    `BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW` (or have its `directional_shadow_visibility` byte 0) and leave
    `spot_shadow_visible` 0. It still takes shadows. Lifted casters are RFC 0012's phase 2, if a game asks.
  - Two new shader files, after their bases in the CMake list: **`production_model_lift.vert`** and
    **`production_model_detail_table_lift.vert`** (a lift with a world-detail table), with the include
    `production_height_lift.hlsli`. The core loads them beside the host's `production_model.vert`, for lifted batches
    only; without them (or with a file of that name that doesn't read a heightfield) a lifted batch fails the present,
    naming the file. Hosts that name their shaders add them only if they lift.
  - `BTBH_GPU_SCENE_HAS_HEIGHT_LIFT`.
- **`btbh_gpu_custom_mesh_update_ex(mesh, vertices, vertex_byte_count, indices, index_count, flags, report)`** and
  **`BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED`**: 0 is the plain update (unchanged; it now forwards to `_ex`). With the flag
  the data is copied at the call into the window's staging area, the mesh's counts and bounds change at once, and the
  next `btbh_gpu_window_scene_present_instances(_with_visibility)` or `btbh_gpu_window_scene_render_to_target` of the
  window copies every queued update in its own upload (the transfer and copy pass it already makes for the instances),
  before it draws: the data shows in the next scene that draws and every one after it.
  - Rules: a later update of a mesh replaces its queued one; a plain update made after a queued one always lands after
    it (for a mesh in the shared buffers whose queued update a frame waiting for the canvas copied, the plain update
    is recorded into that frame after its draws); `btbh_gpu_custom_mesh_destroy` drops the mesh's queued update; a
    mesh in the shared buffers must fit its ranges, a mesh of its own grows at the call; a scene that fails before it
    is kept leaves the queue for the next; a closed window drops its queue.
  - At most `BTBH_GPU_CUSTOM_MESH_QUEUE_MAX_BYTES` (256 MiB) queued at a time (replaced updates count until the next
    scene); past that the call fails and queues nothing. An unknown flag fails. `BTBH_GPU_HAS_QUEUED_MESH_UPDATES`.
- **`BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE`** (11; `BTBH_GPU_SCENE_SURFACE_COUNT` becomes 12): a see-through surface lit
  per pixel, for smoke, dust and steam (RFC 0013). Drawn in the transparent pass with **`production_particle_lit.frag`**
  (its **`_2d_shadows`** or **`_unshadowed`** sibling by the frame's shadows), beside the host's fragment shader.
  - Its light, each at the scale a lit surface takes it: the sky's ambient; the sun, its shadow through four plain
    comparison taps whatever the sun's filter, and the clouds; the fill and the spot (or omni) light with their
    shadows; the street lamps and the point lights, each skipped beyond its reach; the radiance cascades' light at its
    x and z, additive or not; its emission. A light's share is the albedo times the wrap
    `saturate((n.l + w) / (1 + w))` plus, for the sun and the point lights only, a transmission
    `x * exp(-thickness * z) * 4 pi * HG(y)` (Henyey-Greenstein's phase, tinted by the albedo).
  - Its normal and thickness: `normal_texture`, linear, on the albedo's texture coordinate (rg the tilt, OpenGL +Y,
    scaled by `normal_scale`; b the thickness). Without one it is flat and 1 thick.
  - `surface_parameters0`: the soft fade (as `SOFT_PARTICLE`'s, with the scene's distance in a slot of its own), the
    near fade's start and width, the wrap. `surface_parameters1`: the transmission, its asymmetry (clamped to
    -0.99..0.99), the optical depth at thickness 1, and 0.
  - Fog at its own depth: it writes the colour that the HDR composite's fog at the depth behind it turns into the puff
    fogged at its own depth over the background fogged at its own, for every layer (exact while the fog's opacity there
    stays below 1, as Be The Bullet Hell v2's always does). The distance pass runs on frames that draw one.
  - Straight alpha (`BTBH_GPU_SCENE_BLEND_ALPHA`) only. It casts no shadow, writes no motion, and is never a source of
    the radiance cascades' light, with an emission or without.
  - Refused, naming the batch (`BTBH_GPU_INVALID_ARGUMENT`, "surface_variant LIT_PARTICLE: ..."): opaque, another blend,
    unlit, animated, dug, a world detail with a texture, a table with `map_layers`, a lift with a heightfield,
    procedural detail, `surface_parameters1[3]` not 0. A present whose shader directory lacks the file (or has one of
    that name that isn't the lit particle's) fails, naming it.
  - `BTBH_GPU_SCENE_HAS_LIT_PARTICLE`.
- **`BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT`** (batch flag bit 3): the instance's four custom floats are a rectangle of
  its textures (the texture coordinate, after `uv_scale` and `uv_offset`, times custom x and y plus z and w), so one
  batch draws every cell of an atlas, a cell per instance. Every static vertex shader takes it, so it works in every
  pass: opaque or transparent, lit, unlit (the fast unlit shader too), soft and lit particles, lifted, with a
  world-detail table, and the radiance cascades' source pass. Refused, naming the batch: animated, `DISSOLVE`, `SHATTER`
  or `GLITTER` (they read the custom floats), a world detail or table seeded by a custom float. A present with the flag
  and a SPIR-V static vertex shader that doesn't read it fails, naming the file. `BTBH_GPU_SCENE_HAS_INSTANCE_UV_RECT`.
  Batch bit 4 stays free for RFC 0013's phase 2.
- **`BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR`** (batch flag bit 6): the instance's custom x (instance float 20),
  clamped to 0..1 (a NaN is 0) and taken to the nearest 1/4096, multiplies on that instance
  - the material's emission (the emission texture times the emission colour times `emission_energy`),
  - the glass detail's share of panes lit at night (pattern 5's `detail_parameters[1]`): as it falls the lit panes go
    out one by one, each at its full light until it does,
  - the lamp lens detail's glow (pattern 6),

  in every pass: the scene shaders, the fast unlit shader and the radiance cascades' source field. Nothing else
  changes: the albedo (an unlit batch's colour, on screen and as a source), the glass's panes, tint and reflection, the
  shading, the surface variants' own glows. At 1 an instance draws byte for byte as unflagged; at 0 as v2's Dark
  batches (`emission_energy` 0, the glass's share 0, the lens's pattern 0). Skinned batches take it. It reaches the
  shaders as bit 9 (512) of the fragment block's `unlit_enabled`: no pipeline is added.
  - Refused, naming the batch (`BTBH_GPU_INVALID_ARGUMENT`, "flags INSTANCE_LIGHT_FACTOR: ..."): with
    `INSTANCE_UV_RECT`, `DISSOLVE` or `GLITTER` (they read custom x), a world detail or table seeded by custom x
    (`seed_component` 1; v2's seed in custom w is free), a lit particle (its shader doesn't take it), a refracting
    batch (it draws no material).
  - Older scene shaders ignore it: the instance draws powered. `BTBH_GPU_SCENE_HAS_INSTANCE_LIGHT_FACTOR`.
- **`btbh_gpu_instance_buffer_upload_ex(buffer, byte_offset, bytes, byte_count, flags, report)`**: 0 is
  `btbh_gpu_instance_buffer_upload` (unchanged: a submission of its own that cycles the buffer, so the rest of it is
  undefined after a partial upload; it now forwards to `_ex`).
  - **`BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED`**: the bytes are copied at the call into the window's upload queue (the
    queued mesh updates' one) and the uploaded prefix grows at once; the next scene, present or render-to-target,
    writes them in place in the transfer and copy pass it makes for its instances, before it draws. The rest of the
    buffer keeps its data. Frames submitted before that scene, and a scene waiting for the canvas, draw the old data.
  - **`BTBH_GPU_INSTANCE_BUFFER_UPLOAD_NO_CYCLE`**: submitted at once but in place, so the rest of the buffer keeps its
    data; while a scene waits for the canvas it goes into that frame after its draws instead (that frame draws the old
    data). With `QUEUED` it changes nothing.
  - Rules: uploads of one buffer land in the order they were made, queued or not: a later upload's bytes win where it
    overlaps a queued one not yet copied (several queued in one frame included), and a queued one it covers is dropped;
    a `NO_CYCLE` upload after a queued one that the waiting frame copied lands after it; a plain upload cycles as always.
    `btbh_gpu_instance_buffer_destroy` drops the buffer's queued uploads (one a waiting frame copied still runs). The
    queue's cap counts mesh updates and instance uploads together. `BTBH_GPU_HAS_QUEUED_INSTANCE_UPLOADS`.
- **`btbh_gpu_custom_mesh_update_range(mesh, first_vertex, vertices, vertex_byte_count, first_index, indices,
  index_count, flags, report)`**: writes a range of a mesh's vertices and of its indices (either may be empty) in place,
  for a mesh with buffers of its own or in the shared buffers; the rest of the mesh keeps its data. The counts don't
  change and the range must lie within them; every index must be below the vertex count. The mesh's bounds grow to
  cover the new vertices and never shrink until a whole update. `flags`: 0 (submitted at once without cycling; into the
  waiting frame after its draws while one waits) or `BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED` (copied at the call, written
  by the next scene). Where a range overlaps queued updates of the mesh not yet copied, its data wins; a whole update
  replaces the queued ranges before it. `BTBH_GPU_HAS_PARTIAL_MESH_UPDATES`.
- **The lazy creation log** (v2's night perf round, request 4): `btbh_gpu_window_set_lazy_creation_log_enabled(window,
  enabled)` and `btbh_gpu_window_lazy_creation_report(window, entries, capacity, &count)`, which copies out the oldest
  entries since the last report (the window keeps `BTBH_GPU_LAZY_CREATION_WINDOW_CAPACITY`, 128; a gap in `sequence`
  shows a drop). `BTBH_GPU_LOG_LAZY_CREATION=1` records every window and prints each entry with `SDL_Log`, one line
  starting `BTBH_GPU_LAZY` (frame, call, kind, ms, prewarm, name). `BTBHGPULazyCreationEntry` (256 bytes):
  `frame_index` (the window's frames submitted before), `sequence`, `microseconds`, `kind`, `call` (scene present,
  render to a target, canvas present), `prewarm_covers`, `name[232]`. `BTBH_GPU_HAS_LAZY_CREATION_LOG`.
  - What it records: shaders (with their file read), graphics pipelines by their key (the vertex and fragment shader
    files, each colour target's format and blending, the depth-stencil format, depth test, stencil and cull mode),
    compute pipelines, samplers, textures (size, format, use), buffers and transfer buffers made or grown, each with
    its CPU time and where in the core; and the SDL calls that take 1 ms over their usual time: the scene's and the
    canvas's command buffer and upload map, the swapchain image, the frame's submit and a render to a target's.
  - `prewarm_covers` 1: made in the steps the present shares with `btbh_gpu_window_scene_prewarm` (or its
    render-to-target form), so a prewarm with the frame's shaders, settings, batches, render size and drawable size
    makes it; after a resize, only a prewarm after it. 0: nothing makes it ahead: the buffers that grow with a frame's
    instances, bones, motions, canvas vertices and the cascades' emitters, everything the canvas makes, the slow calls.
    SDL's own lazy work (descriptor pools, uniform buffers, its deferred frees) shows only as a slow call around it.
  - Only the presenting thread is recorded; uploads made outside a present (loading) aren't.
- **`btbh_gpu_device_set_upload_staging_budget(device, bytes)`** (request 5's fix): keeps up to `bytes` of upload
  buffers for reuse. Without it (0, the default) every upload makes its own transfer buffer and releases it, and SDL
  frees them all in the next submit that presents.
  - With it, uploads take ranges of at most 16 pooled buffers (4 MiB, or a power of two that holds the upload), reused
    once the GPU has finished every upload that read them (fences, never waited for).
  - Pooled: textures (made, loaded, updated, arrays, heightfields), the shared geometry buffers (models' primitives and
    `BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS` meshes), custom meshes with buffers of their own (made, updated, updated in
    part) and instance buffer uploads (plain and `NO_CYCLE`).
  - Not pooled: an upload recorded into a frame waiting for its canvas (a `NO_CYCLE` upload or a plain mesh update
    meanwhile) still makes its own; queued uploads are copied from the scene's own transfer buffer, as without a
    budget; an upload bigger than the budget, or one made while the pool is busy, makes its own as before.
  - Lowering the budget lets idle buffers go; the device's shutdown releases the rest.
    `BTBH_GPU_HAS_UPLOAD_STAGING_BUDGET`.

### Changed
- The custom-mesh queue is the window's upload queue: an entry per range (a mesh's vertices or indices, an instance
  buffer), and ranges that overlap carry the same bytes, so the order of their copies doesn't matter. A frame that
  copied queued updates or uploads is submitted even when it is dropped (the canvas refuses its submission, or the next
  scene is presented without one), so they land; before, such a frame was cancelled and its queued mesh updates lost.
  A host that never queues drops frames as before.
- `production_model.vert` carries the lift under `#if BTBH_HEIGHT_LIFT`; without it, it and every shader built from it
  compile to 0.10.0's SPIR-V, MSL and reflection byte for byte.
- RFC 0013: `production_model.vert` and the four files built from it read the UV rect's switch (a named word,
  `instance_uv_rect`, the first of the vertex block's reserved ones, through `production_temporal.hlsli`'s new
  `BTBH_TEMPORAL_UNIFORMS_STATIC`; the skinned shaders keep their block and their SPIR-V). An unflagged batch's texture
  coordinate is bit for bit what it was. `SOFT_PARTICLE` gains its header comment. The scene's shader check reads up to
  16 files (8 before).
- Tests (RFC 0013): `btbh_gpu_particle_test` (new: the phase, the wrap, the transmission, the fog's amount and its
  make-up over random stacks, every refusal); `btbh_gpu_lit_particle_smoke` (new: the light, the shadow, the fog, the
  cascades, the fades against CPU references, the UV rect on eight paths, the refusals, prewarm, old shaders;
  `BTBH_LIT_PARTICLE_DUMP`, and `BTBH_LIT_PARTICLE_OLD_SHADER_DIR` for an older release's static vertex shaders); the
  order test's variant, bit and macros; the phase 2 shader test's three particle shaders; the flags smoke's unknown
  batch flag is now bit 4; the Swift suite's check of the vertex uniforms' end; the frame bench's `smoke-off`,
  `smoke-soft` and `smoke-lit` modes. `tools/check.ps1` and `tools/check.sh` skip the new smoke with the
  other GPU smokes.
- Swift: `GPUSceneSurfaceVariant.litParticle` (11), classified from a `lit_particle` surface shader (`u_softFade`,
  `u_nearStart`, `u_nearFade`, `u_wrap`, `u_transmission`, `u_asymmetry`, `u_opticalDepth`), as `soft_particle` is. The
  Swift layer sets no batch flags, so the UV rect has no Swift API.
- `height_lift` is part of the same-material run key (RFC 0011): a run never spans two lifts.
- Tests: `btbh_gpu_height_lift_test` (new, the C copy: halves, slopes, the bilinear read, a plane's lift and normal,
  orthogonality through the tilt, a v2-style lattice, the refusals' reasons); `btbh_gpu_height_lift_smoke` (new: lifted
  batches against meshes lifted on the CPU through every pass, the table, a lifted unlit overlay under additive cascades
  with `KEEP_HBAO`, every refusal, prewarm, old shaders);
  `btbh_gpu_mesh_queue_smoke` (new: every queue rule against meshes made with the data, in render-to-target and
  present, and `BTBH_SMOKE_BENCH=mesh-queue`; then the queued and in-place instance uploads and the partial mesh
  updates, every rule above, and `BTBH_SMOKE_BENCH=upload-queue`); the order test's layouts, run key, lifted sort key,
  and the new flags and HAS_ macros; the phase 2
  shader test's lifted shaders; the frame bench's `lift`, `lift-keep` and `lift-flat` modes. `tools/check.ps1` and
  `tools/check.sh` skip the new smokes with the other GPU smokes (`check.sh` now skips the runs and RC tops smokes
  too).
- The light factor: `production_model.frag` (and the five files built from it), `production_model_unlit.frag` and
  `production_rc_material_source.frag` read it through a new include, `production_light_factor.hlsli` (in the CMake
  list's dependencies); the last two take custom x at location 5, and the source shader's inputs get explicit
  locations. An unflagged batch's emission, panes and lens glow are bit for bit what they were: each product with the
  factor stands where the value it scales stood.
- Tests (the light factor): the flags smoke's blackout street (glass, a lamp lens, a glow, a see-through unlit card on
  the fast unlit shader, the skinned quad, glows outside the view, unlit sources) through no cascades with the sun's
  shadow and without, cascades, additive cascades with the omni shadow and the HBAO, and with `KEEP_HBAO`; each piece
  alone; the refusals (present and prewarm); prewarm. Its unknown batch flag is now bit 7, and it loads
  `skinned-quad.gltf`. The particle test's refusals; the order test's bit and macro; the frame bench's `light-off`,
  `light` and `light-mixed` modes.
- Swift: nothing. The Swift layer sets no batch flags, so the light factor has no Swift API (as the UV rect).
- **`BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO`'s transparent part applies with any ambient occlusion** (asked
  for by Be The Bullet Hell v2, which draws with contact AO): under additive cascades with the flag, transparent unlit
  batches take the fast unlit shader, as without the cascades, with `ssao_contact` 1, `ssao_enabled` 1 or no ambient
  occlusion too. In 0.10.0 that needed the HBAO (`ssao_enabled` 2, `ssao_contact` 0), so with contact AO v2's overlays
  by its neon still took the point lights (+0.94 levels on 19 % of its pixels, up to 203, round a sign-lit building's
  base). The flag's other part (the scene's ambient targets, the bounce light's albedo, the reflections'
  reflectivity) still needs the HBAO; the header's doc says which part applies where. This changes frames only where
  the flag is set under additive cascades with contact AO, the SSAO or none, wherever a point light (or the dark scene,
  or unlit 2 or 3) meets a transparent unlit batch.
  - The flags smoke: additive cascades, nothing glowing, the overlays by the neon, with no shadow and the sun's. With
    the flag, contact AO and none render the frame without the cascades (0 pixels differ) and the SSAO changes the
    overlays; without it they are brighter. The gallery's new unset `neon-contact-additive`, `neon-ssao-additive` and
    `neon-no-ao-additive` match the frames of the core before the change, to the byte.
  - Not changed: an opaque unlit surface by point lights under additive cascades with contact AO draws as the cascades
    draw it, with the flag or without (in the smoke the unlit box's own pixels: 237, up to 26 levels).
- Every SDL texture, sampler, buffer and transfer buffer the core makes goes through a wrapper (macros in
  `btbh_gpu_internal.h`, the caller's `__func__` naming where), as shaders and pipelines already did; a present, a
  render to a target and a canvas present run inside the log's scope. With the log off that is a flag test a call and
  one atomic load a creation.
- `btbh_gpu_window_scene_prewarm`'s header names the first frame's other costs: the frees after a load, and the log.
- **Frame telemetry's fences** (found with the staging pool). SDL hands a released fence to the next submission at
  once, so a fence released while its frame still runs can read signalled before that submission has run. A presented
  frame's fence is also held by its swapchain, but a hidden or minimized window's frame was held only by the
  telemetry.
  - `btbh_gpu_window_release_frame_telemetry` (the telemetry turned off, the cascades leaving `AUTO`, the window
    destroyed) now waits for a frame still running before it releases its fence.
  - A full pending list (256 frames unpolled) drops its oldest sample, its fence long signalled, and keeps the newest.
    Before, the newest was dropped and its fence released at once.
  - `btbh_gpu_window_finish_frame_telemetry` waits 32 fences at a time: asked to wait for 256 at once, SDL 3.4 on
    Vulkan (an RTX 3070 Ti, driver 621.14) returned at once with the newest frames still running.
- Tests: `btbh_gpu_lazy_creation_smoke` (new: what cold frames log and prewarmed ones don't, frames with the log and
  the budget against frames without, the canvas, a render to a target, the report's refusals and pieces; under a
  budget, textures, instance buffers and meshes of their own uploaded through the pool, with queued uploads and uploads
  into a waiting frame among them; frame telemetry's fences on a hidden window and past 256 unpolled frames);
  `btbh_gpu_stage_bench` (new, a measurement: a stage switch at v2's Deck preset, each frame's
  CPU split with the SDL calls from the log). `tools/check.ps1` and `tools/check.sh` skip the new smoke with the other
  GPU smokes.

### Measured
Windows, RTX 3070 Ti, Vulkan, without a debug device. The machine's shared perf lock was held for the runs marked
*locked*; other sessions' `v2-game` processes (one to five) ran through all of them anyway, so the CPU times are up to
twice a quiet machine's.

- **Queued custom-mesh updates** (the mesh queue smoke's bench: 200 updates of a 64-vertex mesh a frame, against the
  same frames without updates; three runs, *locked*): a plain update costs 234–271 µs at the call (v2 measured about
  170; 156–182 µs here on a quieter run) and 238–275 µs with what it adds to the present. A queued one costs
  0.43–0.51 µs at the call and 4.3–5.3 µs in all (3.7–4.6 µs of it in the present's upload, where SDL makes two copies a
  mesh); in the shared buffers 3.4–3.6 µs. About 50 times cheaper: 200 moving marks cost about 1 ms a frame instead of
  48–55 ms.
- **Queued instance buffer uploads** (the mesh queue smoke's `upload-queue` bench: 60 chunks, instance buffers of 4 to
  64 KiB, 2,037 KiB in all, each drawn by a batch and all uploaded every frame; medians of 120 frames a mode, three
  runs, *locked*, two other sessions' `v2-game` processes running): whole chunks with the plain upload cost
  84–90 µs each at the call, and the frame 12.3–13.4 ms of CPU against 0.1 ms without uploads (the canvas after them
  waits about 120 µs an upload behind the 60 submissions). Queued, the same whole chunks cost 0.7–1.0 µs each at the
  call and 3.2–3.5 µs in the present's upload: the frame 0.35–0.39 ms. A quarter of each chunk queued (507 KiB a
  frame): 0.2 µs at the call, 1.8–2.1 µs in the present, the frame 0.23–0.25 ms. A quarter with `NO_CYCLE` costs what
  the plain upload does (82–92 µs at the call, 12.3–14.1 ms a frame): the submission is the cost, not the size. The
  GPU side of the copies wasn't measured.
- **Queued custom-mesh updates after the queue became the window's** (the `mesh-queue` bench, release/0.11.0's build
  and this one alternating, two runs each, *locked*): the same within the noise, 0.27–0.28 µs at the call and
  2.35–2.48 µs (before) against 2.36–2.41 µs (after) an update in all; in the shared buffers 1.72–1.81 against
  1.73–1.74 µs.
- **A frame without lifted batches** (the frame bench's CPU mode, 2,001 batches; `c5390f0b`'s core and shaders against
  this branch's, in alternating processes): the GPU does the same work (the same shaders, byte for byte, and the same
  pixels). On the CPU the quietest runs (14 pairs, not locked) put the geometry encode at +3–4 µs and the shadow encode
  at +2 µs a frame, about 2 ns a batch (the run key's one more field, the batch's 8 more bytes). The prepare cost +7 µs
  in a first version, which looked at the lift before the pipeline slot, and +0.7 to +4 µs after the fix (*locked*, 8
  and 16 pairs). Beside other sessions' processes the pairs scatter by up to ±0.2 ms a frame (*locked* medians: +13 µs
  geometry, +26 µs shadow encode over 16 pairs), so nothing under about 0.03 ms is resolved there.
- **Lifted batches** (the frame bench's `lift`, `lift-keep` and `lift-flat` modes: the 2,001 draws, 4.4 million vertices,
  lifted by a 193 × 145 heightfield, against the same draws unlifted; the pieces cast no shadow in all three; *locked*,
  three runs, 60 paired rounds): the opaque pass +0.043 ms on the GPU (90 % interval +0.033 to +0.051; the whole frame
  +0.035), about 0.001 ms per 100,000 lifted vertices, as the RFC estimated; with `KEEP_NORMALS` +0.041 ms (the fetch
  costs, not the tilt). On the CPU a lifted batch costs 3–7 ns more geometry encode in a run and 2 ns more prepare;
  with runs off (each batch binding its heightfield) 6–16 ns more encode.
- **The first frame of a new stage** (v2's request 5, 40–88 ms outside the CPU split; the stage bench at v2's Deck
  preset: a 1280x800 window, 2,000 kit draws, a stage load of about 140 uploads with textures up to 4096^2; six stage
  switches a run, *locked*). The first frame after the load, median (range):
  - nothing set: 28.4 ms (23.8–31.9). The scene present takes 2.1, all of it in its report's split; the canvas present
    takes 26.0, of which its submit 25.3. With the whole kit in the shared buffers, as v2 makes it: 26.5 (20.5–29.1),
    submit 24.2. The frames after it: 1–6 ms.
  - Where the submit goes (SDL timed inside, in a scratch build): SDL 3.4's Vulkan backend gives every transfer buffer
    a dedicated host-visible allocation, and frees the released ones only in a submit that presents. The first one
    after the load freed 136–146 of them (215–407 MB): 18–27 ms. Its defragmentation of one allocation took
    0.25–1.4 ms (moving up to 115 MB of the old stage's neighbours on the GPU), the queue submit 0.05–0.1 ms, the
    present about 0.5 ms. Not pipelines, residency or descriptor pools: the scene present after a prewarm made nothing.
  - with a staging budget of 128 MiB: 4.1 ms (2.9–8.6), submit 2.3; the next frames 1.4 and 1.2. The load made 2–12
    buffers to free instead of about 140, and its meshes took 3.6 ms instead of 12.9.
  - the host's way without a budget, waiting for the GPU (`SDL_WaitForGPUIdle`) and presenting one canvas frame after
    the load: 2.9 ms (1.2–15.7); that extra frame takes the 20 ms instead.
  - left with a budget: in a later frame, SDL frees the old stage's emptied 64–128 MB device memory blocks
    (0.4–17 ms, one a frame) and defragments (0.25–1.4 ms); a window resized at the switch remakes its swapchain in the
    first acquire (7.7 ms).
- **The staging budget on every path** (the stage bench as above, its default kit: half the pieces in the shared
  buffers, half with buffers of their own, plus 8 instance buffers; six switches a run, *locked*, on the merged
  release). The machine ran faster than for the entry above, so compare within each entry. The first frame after the
  load:
  - nothing set: 18.6 ms (16.8–47.6, two runs), of which 15.5 ms is the submit;
  - with a budget of 128 MiB: 4.1 ms (2.7–5.7), the submit 1.6–1.9 ms. Loading the meshes takes 1.3 ms instead of 6.3,
    and the instance buffers 0.2 instead of 0.7;
  - with the budget pooling only textures and the shared buffers (the prewarm log branch's build): 8.8 ms (7.5–9.7),
    the submit 6.0–6.5 ms. The meshes with buffers of their own and the instance buffers still left about 60 transfer
    buffers to free.
- **The log off** (the frame bench's CPU mode, 2,001 batches, four alternating processes each, *locked*): the scene
  present takes 0.640 ms against the base's 0.659 (the processes' medians 0.62–0.72 and 0.61–0.72): no difference.

RFC 0013's runs, on the same machine: the frame bench's Deck preset (960 x 600 upscaled to 1280 x 800, HBAO, FXAA, the
soft sun shadow, 2,001 kit draws). Every one held the perf lock (*locked*, 06:35:51-06:37:30 on 2026-11-23); one other
session's `v2-game` process ran through it anyway.

- **Lit smoke against Be The Bullet Hell v2's smoke today** (the bench's `smoke-soft` and `smoke-lit` modes, *locked*: 170
  puffs with discs 5.8 % of the screen's height; at least one layer over 11.2 % of the screen, three over 3.4 %, six over
  1.2 %, the deepest stack 14; 3.4 lights in reach of a puff, under 16 lamps, 10 point lights and the sun's shadow; three
  runs, 40 rounds a window, the windows rotated): the transparent pass, distance pass included, costs 0.067 ms without
  puffs, 0.110 ms with v2's unlit soft particles in 10 draws and 0.112 ms with one lit particle batch. Lit against soft:
  +0.002 ms (90 % interval -0.003 to +0.005; the 10th percentiles -0.001), the whole frame +0.005 ms (-0.012 to
  +0.021): not resolved. The lit shader's light costs about what the soft particles' full scene shader does, and it
  shades only the discs (a fragment whose alpha is 0 is discarded before its light). The Deck isn't measured.
- **Off** (mode none: no lit particle, no rect; *locked*): this branch's shaders against `fc2784ea`'s under this core,
  three runs: the opaque pass -0.002 ms (-0.004 to +0.001), the whole frame 0.000 ms (-0.003 to +0.004) for 2,001 draws
  and 4.4 million vertices, so the rect's test isn't measurable. On the CPU (`fc2784ea`'s bench, core and shaders
  against this branch's, 10 alternating processes each): prepare +5 us, geometry encode -8 us, shadow encode +1 us,
  the present +4 us a frame, each inside its pairs' spread (tens of microseconds beside the other process): nothing
  resolved.

The light factor's runs, on the same machine and bench (the Deck preset, 2,001 kit draws). All *locked*
(07:18:15-07:20:02 on 2026-11-23); one other session's `v2-game` process ran through it.

- **Per flagged batch** (the bench's `light` against `light-off`: every draw glowing, every fourth glass with half its
  panes lit, all 2,001 flagged at 1 in `light`; three runs, 40 rounds a window, the windows rotated): the opaque pass
  +0.0007 ms (90 % interval -0.0013 to +0.0040), the whole frame -0.0035 ms (-0.0078 to +0.0002). Not resolved: under
  2 ns a batch. `light-mixed` (1, 0.5 and 0 by instance) -0.003 ms (-0.007 to -0.001): dark glass skips its panes'
  light. On the CPU (8 alternating processes each) prepare +0.9 us and geometry encode +0.9 us a frame for the 2,001
  flags, inside the pairs' spread.
- **Off** (mode none): this branch's shaders against `acb817e5`'s under this core, three runs: the opaque pass +0.003 ms
  (-0.001 to +0.005; the 10th percentiles +0.001, -0.004 to +0.005), the whole frame -0.001 ms (-0.003 to +0.003). On
  the CPU (`acb817e5`'s bench, core and shaders against this branch's, 8 alternating processes): prepare +0.9 us,
  shadow encode +5 us, geometry encode +2 us, the present +15 us a frame, each inside its pairs' spread (about
  +-20 us): nothing resolved.

### Upgrading
- Nothing to set: hosts that don't lift or queue draw as before. Rust hosts with bindgen regenerate the bindings for
  the bigger batch (288 bytes) and the new functions; `..Default::default()` leaves `height_lift` null.
- To lift (v2): build the two lifted vertex shaders with the others; make one heightfield a stage (its samples on the
  lattice's points, `world_to_uv` mapping them to texel centres, as the header shows); point the ground pieces' and the
  overlays' batches at a lift (the overlays at their height over the ground with `KEEP_NORMALS`), give the ground
  pieces `BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW`, and stand their instances at height 0. Keep each lift alive and unmoved
  while a present that uses it runs. Bounds used for culling must span the lift.
- To queue (v2's conformed marks): `btbh_gpu_custom_mesh_update_ex(..., BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, ...)`.
- To upload part of a chunk (v2's city and kit chunks): `btbh_gpu_instance_buffer_upload_ex(chunk, first * 96,
  &records[first], count * 96, BTBH_GPU_INSTANCE_BUFFER_UPLOAD_QUEUED, &report)` for each changed range, every frame,
  instead of the whole chunk; a chunk that is a mesh: `btbh_gpu_custom_mesh_update_range(...,
  BTBH_GPU_CUSTOM_MESH_UPDATE_QUEUED, ...)`. Don't mix in plain (cycling) uploads of part of a buffer: they leave the
  rest undefined, as always.
- Swift: nothing changes; a `heightLift` option can follow when Be The Bullet Hell (v1) asks.
- RFC 0013: nothing to set; a host that draws no lit particle and sets no UV rect draws as before. Rebuild the five
  static vertex shaders with the others. Rust hosts with bindgen get the new variant and flag; `..Default::default()`
  leaves them off.
- To draw lit smoke (v2): build the three `production_particle_lit*.frag` files (`CMakeLists.txt`'s list); bake a
  normal and thickness atlas on the albedo's cells; make the smoke one batch of `BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE`,
  straight alpha, with `BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT` and each puff's cell in its custom floats; keep sorting the
  puffs far to near. The five tilts, the sprite turning, the emission's sky fill and the CPU light gathering can go.
- The light factor: nothing to set; a host without the flag draws as before. Rebuild the scene shaders (with the new
  include beside them). Rust hosts with bindgen get the new constant.
- To black out a district (v2): give the light-carrying kit batches `BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR` and
  every instance's light in custom x (1 powered, 0 dark; between them for the wave and the flicker); a blackout rewrites
  that float (0.11.0's queued uploads can carry it). `KitKey::Dark` and the dark `SurfaceLook` copy can go. Keep the
  world detail's seed out of custom x (`seed_component` 2 to 4).
- `KEEP_HBAO` with contact AO (v2): it now draws the transparent pass as without the cascades. Set it with additive
  cascades if the overlays should not take the point lights there.
- Swift: nothing changes; a `heightLift` option can follow when Be The Bullet Hell (v1) asks. Neither the lazy creation
  log nor the staging budget has a Swift API.
- The lazy creation log and the staging budget are off by default: nothing changes. Rust hosts regenerate the bindings
  for the new functions and types (no struct changed).
- v2, after a stage's first frames hitch: run with `BTBH_GPU_LOG_LAZY_CREATION=1` (or turn the log on per window and
  read the report). An entry with `prewarm=1` names a batch kind or setting the prewarm missed: prewarm with it. With
  `prewarm=0`: a buffer that grew (more instances than before), the canvas's (its pipelines are made by its first
  present and the first batch with a font atlas), or a slow SDL call. A slow `SDL_SubmitGPUCommandBuffer (frame)`
  after a load is SDL freeing what the load released: call `btbh_gpu_device_set_upload_staging_budget(device,
  128 << 20)` once after making the device (128 MiB lets a 4096^2 texture's 64 MiB share the pool), and destroy the old
  stage's resources a few frames before the new stage's first frame (a loading screen's), so that SDL frees their
  memory there.

### Known
- **The lift's pipelines are made on first use** unless the prewarm sees them. Give `btbh_gpu_window_scene_prewarm` a
  lifted batch (a template with a heightfield) of each kind drawn lifted.
- **Be The Bullet Hell v2's geometry encoding grew by 0.08 ms with the lift.** A lifted batch is drawn with its own
  pipeline, and its `height_lift` is part of the run key. Batches lifted with the same lift and material therefore
  keep their runs, but lifted and unlifted batches interleaved switch pipelines. Sort the lifted batches together.
- **`KEEP_HBAO` draws every transparent unlit batch as without the cascades**, the kaiju's beam and glows included:
  they lose the cascades' point-light boost. Under contact AO, opaque lit surfaces in a street lamp's pool still differ
  from the cascades off by up to 94 levels, with the flag or without: its HBAO part, the ambient light, applies to HBAO
  only.
- **The light factor at 0 leaves a glass pane drawn in its albedo**, the wall's, so drop dark cells' panes as before.
- **Random ~40 ms submits and 5–12 ms swapchain acquires** in a run's first frames: v2 sees them on 0.10.0 too. They
  are SDL's or the driver's.
- **macOS (Metal) and a real Steam Deck** aren't verified.

## [0.10.0] — 2026-11-23

This release has three parts for Be The Bullet Hell v2:
- cheaper draws (RFC 0011's phase 1);
- radiance-cascade sources with a top (RFC 0010);
- fixes and options from its findings against 0.9.0 and its kaiju's spot shadow.

The new features are opt-in, and the cheaper draws change nothing that is drawn. Unset, every image of this core's
smokes renders byte for byte as in 0.9.0. Be The Bullet Hell v2's frames differ in a few dozen pixels (see Known).
No struct grows, and no shader file is added. Radiance-cascade and scene shaders changed, so rebuild
the shaders with the C core.

RFC 0011's phase 1, asked for by Be The Bullet Hell v2: cheaper draws. A batch that differs from the one drawn before
it only in its mesh and instance range now binds and pushes nothing new (same-material runs), the frame's lighting is
filled once a pass instead of once a batch, and a creation flag puts custom meshes in the models' shared geometry
buffers, so that neighbouring draws share their vertex and index buffers. Every game gains where its batches run by
material, without changing anything; nothing it draws changes. No struct grows.

RFC 0010, asked for by Be The Bullet Hell v2: radiance-cascade sources with a top, so that street glows stay out of
the roofs they stand next to. With `BTBHGPUSceneRadianceCascades.source_tops` set, a ray piece of the cascades that
starts on a roof higher than a source's top passes over it. A glowing batch's top comes from its geometry; an
emitter's is `position_y + top_above`. Off, nothing changes, byte for byte. Neither struct grows (a byte and a float
from reserved fields); six radiance-cascade shaders changed: rebuild the shaders with the C core.

Be The Bullet Hell v2's three findings against 0.9.0, and its kaiju's spot shadow. `KEEP_HBAO` frames now match the
frame without the cascades under point lights too: the difference v2 saw wasn't the HBAO but the transparent pass,
whose unlit batches drew with another shader under the cascades. The cascades' debug views no longer draw the
transparent pass over them. Two opt-in additions: `BTBH_GPU_SCENE_POST_SSR_FACING_HITS`, which keeps the reflections
from smearing a column of each thin thing down the wet ground under a steep camera, and the spot occluder's flags with
`BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW`, for a kaiju shadow from the spot map that takes only the sun's light.
No struct grows (the new fields take reserved bits and a reserved byte); five scene shaders changed, so rebuild the
shaders with the C core.

Tested on Windows (Vulkan), the merged branch from a cold build:
- `check.ps1` with the GPU smokes passes 26/26. All 71 shaders build, and the public headers compile clean under
  `/W4 /WX`.
- The Swift build is clean, and the full Swift suite runs 301 tests: 2 skipped, 0 failures.
- Identity against `c5390f0b` (0.9.0's candidate with RFC 0011's phase 1): builds of it and of this branch wrote every
  image of the eight image smokes. 173 distinct images, 11,409,280 pixels; 0 differ, in each of three modes:
  - nothing set;
  - `BTBH_SMOKE_SHARED_MESHES=1`;
  - `BTBH_GPU_SCENE_RUNS=0`.
- `c5390f0b` was itself 0 pixels from 0.9.0's candidate over 153 images, in the same three modes.
- The merged build draws each branch's new images byte for byte as that branch did: RFC 0010's with tops on, and the
  findings' new flags.
- `c5390f0b`'s shaders under this core draw RFC 0010's gallery byte for byte.
- The SDF terrain smoke passes against its fixture.

macOS (Metal), the Vulkan validation layers and a Steam Deck aren't verified.

Be The Bullet Hell v2 tested the candidate (a89a4494) on its 0.9.0 adoption with a test patch, two rounds under the
machine's perf lock. The only code change it needed was the emitter's `reserved`, now `top_above`.
- **Unset:** its frames come out the same in every repeat. Against 0.9.0 they differ by a few dozen pixels (see
  Known). `SHARED_BUFFERS` on its kit libraries changes 0 pixels.
- **RFC 0011's CPU geometry encoding** (median per frame):

  | View | 0.9.0 | With runs | Runs and shared library meshes |
  | --- | --- | --- | --- |
  | Map edge, Deck preset | 0.845 ms | 0.471 ms | 0.155 ms |
  | Map edge, High at 1080p | 2.179 ms | 1.207 ms | 0.267 ms |
  | City, stage 3, Deck | 0.775 ms | 0.432 ms | 0.136 ms |

  The kit's share went from 0.69 ms to 0.08 ms, before its chunks share one instance buffer.
- **RFC 0010:** one cell of shielding is enough for its streets. A red source on a roof now lights its own roof. With
  its lamps out of the field, little street glow is left to shield (see Known).
- **`KEEP_HBAO`** with HBAO matches the cascades off, 0 pixels with its neon point lights on.
- **`SSR_FACING_HITS`** in rain removes the streaks under every actor at `ssr_thickness` 0.6, cleaner than its 0.1.
  GPU time at 1440p in rain:

  | Setting | GPU time |
  | --- | --- |
  | `ssr_thickness` 0.6 | 4.65–4.66 ms |
  | 0.6 with `SSR_FACING_HITS` | 4.71–4.72 ms |
  | 0.1 | 4.69–4.70 ms |

- **The kaiju's spot shadow** (`SUN_ONLY | ORTHOGRAPHIC`, its reach 2.5 times the map's depth):
  - It is as dark at dusk as before (0.22 of the light left), but tinted by the sky instead of brown: blue over red
    3.67, against 1.86.
  - Edges are crisp, with no acne. `RECEIVES_NO_SPOT_SHADOW` wasn't needed.

### Added
- **`BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS`** and **`btbh_gpu_window_custom_mesh_create_ex`** (the plain create's
  arguments and a `flags` word; 0 is the plain create, an unknown bit is refused): the mesh's vertices and indices go
  into the window's shared geometry buffers with the loaded models' primitives, instead of two buffers of its own.
  For meshes made once and kept: v2's kit, landmark and set-piece meshes.
  - `btbh_gpu_custom_mesh_update` writes in place and must fit the mesh's ranges: at most the vertex and index counts
    it was created with (none for a mesh made without indices). A bigger update fails with
    `BTBH_GPU_INVALID_ARGUMENT` and leaves the mesh as it was. An update shows in every frame submitted after it,
    a scene already presented but not yet submitted by the canvas included (a mesh with buffers of its own keeps the
    old data for that frame).
  - `btbh_gpu_custom_mesh_destroy` leaves a hole that is never reused. A shared buffer goes when every mesh and model
    in it is destroyed, or with the window. Meshes that change size or are remade (terrain chunks, per-stage meshes)
    should keep buffers of their own.
  - `BTBH_GPU_HAS_SHARED_MESH_BUFFERS`.
- `BTBH_GPU_SCENE_RUNS=0`, a measurement tool: every batch binds and pushes its whole state again.
- **`BTBHGPUSceneRadianceCascades.source_tops`** (RFC 0010, the byte that was `reserved[0]`): 1 gives the cascades'
  sources a top.
  - A batch the cascades take light from (unlit and no `rc_receiver`, or with an emission) has, in each field texel,
    the highest world height of its glowing fragments as its top. An emitter's top is `position_y + top_above`.
  - A ray piece passes over a source whose top lies lower than the height the piece starts from (the top of the
    occluder it starts in, or 0), less a twentieth of a field texel: none of its light, no dimming. Occluders are
    as before.
  - Steep faces read the field half a texel out from their wall, so facades keep the street's glow.
  - The cascades then read the field where its texels lie: without tops they read it half a texel toward +x and +z,
    as before, which set the two directions along each axis a texel apart and made the shield depend on the way a
    street runs. So turning tops on also moves the field's light half a texel toward -x and -z.
  - Other values, and with 1 an emitter's `top_above` that isn't finite or is below 0, turn the cascades off for that
    present (`BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION`), as the struct's other bad values do. With older
    radiance-cascade shaders, a present or prewarm with tops on fails with `BTBH_GPU_INVALID_ARGUMENT`, naming
    `production_hrc_trace.comp`.
- **`BTBHGPUSceneRadianceEmitter.top_above`** (RFC 0010, the four bytes that were `reserved`): with `source_tops` 1,
  how far the emitter's top stands above `position_y` (finite, at least 0); a large value lights every height, as
  without tops. Not read, nor checked, with `source_tops` 0.
- `BTBH_GPU_SCENE_HAS_RC_SOURCE_TOPS`.
- **`BTBH_GPU_SCENE_POST_SSR_FACING_HITS`** (post flags bit 2): the screen-space reflections take only hits a ray can
  make. v2 found that in the rain under a steep camera every actor (the kaiju, jets, enemies) left a noisy column of
  itself smeared down the wet street below it, which `ssr_thickness` 0.1 instead of 0.6 mostly hid. The depth buffer
  holds only the surfaces in front, so a ray that passed behind or beneath a thin thing within the thickness was taken
  for a hit on its visible face.
  - A hit on a surface that faces along the ray (its normal less than 90 degrees from the ray: the top of anything
    seen from above, for a ray climbing from the street) can't be one, so the ray marches on past it. The hits left
    fade as their surface turns edge on (within about 9 degrees) and as the ray passes farther behind it (from a
    quarter of `ssr_thickness` to all of it).
  - What stands on the ground keeps its reflection, and so do walls and the faces turned to the camera: a ray
    reflected from level ground keeps the camera's heading, so it meets them from the front.
  - It works with or without the cascades and with `SSR_SURFACE_REFLECTIVITY` or without; it changes nothing without
    SSR. A new bit rather than a change to `SSR_SURFACE_REFLECTIVITY`: the artefact is the trace's, with the cascades
    too, and 0.9.0's flag keeps its look. The SSAO resolve reads it from a uniform word added at the end of the post
    uniforms (`ssr_hit_options`).
  - In the smoke's steep street (a 67-degree camera, v2's SSR settings in the rain), the ground below a slab floating
    1.6 units up reflected it over 376 pixels; with the flag 68 remain, its true mirror image. The tall box standing
    on the ground keeps all 180 of its reflection's pixels. By day on the flat street, 316 of 2,801 reflecting
    pixels change, by at most 10 levels, along silhouettes.
  - `BTBH_GPU_SCENE_HAS_SSR_FACING_HITS`; Swift: `ScreenSpaceReflectionDescriptor.facingHits` (default false).
- **`BTBHGPUSceneLighting.spot_occluder_flags`** (byte 3,338, which was `directional_shadow_reserved[1]`; an unknown
  bit fails the present), for v2's kaiju shadow: it draws only the kaiju into the 1,024 spot map, fitted along the
  sun round its body with an orthographic matrix, and darkens with `spot_occluder_shadow_strength`.
  - **`BTBH_GPU_SCENE_SPOT_OCCLUDER_SUN_ONLY`**: the occluder's darkening takes only the sun's light (the primary
    directional light), not the sun, the fill and the sky together. The spot map then acts as a sharper sun shadow of
    the kaiju, which can leave the sun's map: taking all three, the shadow came out brown at dusk. Under cascades that
    aren't additive their light (their own field, sun and sky in one) is still darkened as before.
  - **`BTBH_GPU_SCENE_SPOT_OCCLUDER_ORTHOGRAPHIC`**: the spot matrix is orthographic. The reach is then the occluder's
    distance from the map's near plane in world units along the light, read linearly from the depth, instead of a
    perspective distance from the light; the darkening still eases out from 0.4 to 1 times it, and a reach of 2.5
    times the map's depth range never eases. v2 had set the reach to 10,000 to get the same. The spot shadow's slope
    bias also takes spot_direction for the light's direction.
  - `BTBH_GPU_SCENE_HAS_SPOT_OCCLUDER_FLAGS`.
- **`BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW`** (batch flags bit 5; bits 3 and 4 are left to RFC 0013's draft):
  the batch receives no spot or omni shadow: the spot light lights it unshadowed and the occluder's darkening takes
  none of its light. v2 offered it as an alternative to the sun-only darkening; both stay, because they do different
  things. With the sun-only darkening the kaiju shades itself like a sun shadow, at the spot map's resolution and with
  its acne; this flag takes the kaiju out of its own map as a receiver (it keeps its sun-map self-shadow if it stays
  in that map). What a batch casts is still `spot_shadow_visible`'s. `BTBH_GPU_SCENE_HAS_BATCH_RECEIVES_NO_SPOT_SHADOW`.
- Tests: `btbh_gpu_scene_flags_smoke` (since 0.9.0) gains, unset, 12 gallery scenes (neon point lights at night by an
  additive overlay and a refracting box with no, normal and additive cascades; the overlay under the cascades; the
  steep street's reflections with and without them; the kaiju's spot shadow with no, normal and additive cascades),
  and, set:
  - the debug views 1, 5, 6, 7, 9 and 10 under normal and additive cascades render the same with the overlay and
    without (the frame itself differs);
  - `KEEP_HBAO` at night with two point lights of reach 2.6 and energy 2.1 in the building's corners, the overlay and
    the refracting box: as no cascades, to the byte, through no shadow and the sun's (under the omni shadow, at most 1
    pixel by 1 level); without the flag the overlay is brighter (2,710–4,222 pixels differ, up to 22 levels);
  - `SSR_FACING_HITS` with no, normal and additive cascades: the slab's column goes, the tall box's reflection stays;
    without SSR it changes nothing;
  - the spot occluder: sun-only with no darkening, or without a sun, changes nothing; in the shadow it lies between
    the frame without the sun and the one without the darkening (1,461 pixels, none outside by more than a level),
    and is brighter than the classic one; under the cascades it differs from both. `RECEIVES_NO_SPOT_SHADOW` changes
    only the box's own pixels (1,097), lighter. Orthographic: reach 80 against 10,000 and reach 5 against no
    darkening are byte-identical, reach 12 eases; unset, an orthographic matrix's reach 5 eases nothing;
  - refusals: batch bits 3 and 6, post bit 3, spot occluder bit 2.
  - `BTBH_SMOKE_BENCH=facing` times `SSR_FACING_HITS` on the steep street at 1920x1080.
  - `btbh_gpu_scene_order` checks the new bits, the lighting's size and `spot_occluder_flags`'s offset.

### Fixed
- **`BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO` frames under point lights** (v2's finding (b)). With nothing
  glowing, the flag promises the frame without the cascades, but v2's night (its neon signs' point lights, reach 2.6,
  energy 2.1, at the walls' feet) came out up to 45–50 sRGB levels brighter there (in its full 1600x900 frames, 14,387
  pixels by more than 20 levels, at most 202).
  - The cause wasn't the HBAO: the HBAO resolve darkens the same ambient target in both frames, and the point lights'
    light goes to the colour target only, in both. It was the transparent pass. Without the cascades, a transparent
    unlit batch draws with the fast unlit shader (`production_model_unlit.frag`), which adds no point light. Under the
    cascades, it drew with the cascades' scene shader (`production_model_rc*.frag`), whose unlit path brightens the
    colour by the point lights. v2's additive night overlays (the spills, pools and street wash) by its neon lights
    therefore brightened. v2's frames without the neon and the overlays matched (9 pixels by 1 level).
  - With the flag, transparent unlit batches now take the fast unlit shader under the cascades too, so the frame
    matches the one without the cascades. **This changes `KEEP_HBAO` frames** wherever a point light, the dark scene
    (`unlit_darkness`) or unlit 2 or 3 meets a transparent unlit batch.
  - Unchanged, and reported to the coordinator instead: cascades on without the flag keep the scene shader for those
    batches, as before, so their overlays still take the point lights there. Cascades off keep the fast shader, which
    ignores the point lights, the dark scene and unlit 2 and 3's sun shadow for transparent unlit batches, although
    the header says the point lights brighten unlit surfaces. Which of the two is right is a look decision; neither
    default frame changed.
  - Not changed either: under the omni shadow, a pixel of the smoke's night now and then differs by a level between
    the cascades' variant of the scene shader and the plain one, with or without point lights. This is the two
    compiled variants rounding the same light apart, and it was already so in 0.9.0.

### Changed
- **The radiance cascades' debug views draw only the cascades** (v2's finding (a)): the transparent and refracting
  batches are no longer drawn over them. v2's mean "no emitter" light in its views (27.7/19.9/2.6) was mostly its
  additive night overlays. Debug-view output changes, frames without a debug view don't (`BTBHGPUSceneRadianceCascades
  DebugView`'s doc). The views still go through the tone mapping and the grading, as before.
- Shaders: `production_model.frag`, `production_model_2d_shadows.frag`, `production_model_rc.frag`,
  `production_model_rc_2d_shadows.frag` (the spot occluder's flags in `secondary_directional_shadow_controls.z`, unused
  before, and bit 8 of the fragment `unlit_enabled`) and `production_ssao_resolve.frag` (`ssr_hit_options`, a new
  last uniform word). The other 66 shaders are byte for byte c5390f0b's SPIR-V.
- **Same-material runs** (no API). In every pass that draws batches (opaque, transparent, refraction, the shadow maps,
  the radiance cascades' sources), a batch equal to the one the pass drew before it in every field but `kind`,
  `model`, `custom_mesh`, `material_slot`, `first_instance` and `instance_count` (byte for byte, padding aside)
  binds and pushes nothing but its vertex and index buffers before it draws: no uniforms, stencil reference,
  pipeline, storage buffers or samplers. A model's second and later primitives do the same. A skinned geometry never
  starts or continues a run.
- **The frame's lighting once a pass** (no API): the frame's part of the uniform blocks (lights, lamps, point lights,
  the sky's harmonics, clouds, shadow matrices) is filled once a pass and each batch writes its own fields over it.
  Every push carries the bytes it carried before.
- The geometry passes bind a vertex buffer only when it changes.
- The header says, beside `btbh_gpu_window_scene_present_instances`, how a host gains most: within each instance
  buffer, batches grouped by static vertex shader first (those with a world-detail table together, the others
  together: each switch changes the pipeline, and SDL then writes every descriptor set again), then by material, mesh
  after mesh; coplanar overlays after what they lie on, grouped the same way; the meshes drawn so made with the flag.
- The header's comments on `BTBHGPUSceneRadianceEmitter.position_y` and `height` say what the shaders do: the direct
  light's height, and a segment's width and the direct light's reach. Swift's `RadianceEmitter.height` comment too.
- RFC 0010's tests: the native test checks the two fields' offsets, the refusals and C copies of the shaders' math;
  `btbh_gpu_rc_tops_smoke` (new) renders a street of a two-storey and a one-storey house round a lamp head and
  emitters at v2's Low and High, turned four ways (`BTBH_RC_TOPS_OLD_SHADER_DIR` adds an earlier release's shaders).
  The frame bench gains the cascades (`+rc`, `+low`, `+tops`) and `tools/frame_bench_summary.py` a `--group`.
- Tests: `btbh_gpu_scene_order_test` checks the run key byte by byte; `btbh_gpu_scene_runs_smoke` (new) draws a
  street of kit pieces through every pass with and without shared meshes and runs, and the shared meshes' rules; the
  smokes take `BTBH_SMOKE_SHARED_MESHES=1`; the frame bench gains v2's ring (`ring` modes) and a CPU mode
  (`BTBH_FRAME_BENCH_CPU=1`, summarised by `tools/frame_bench_cpu.py`). Three Swift source-text checks follow the
  moved uniform code. `tools/check.ps1` skips the new smoke with the other GPU smokes.

### Measured
Windows, RTX 3070 Ti, Vulkan, without a debug device: the frame bench's new CPU mode, a frame like v2's map edge (943
draws of one material, then v2's ring as it measured it: 257 batches over distinct meshes from 8 chunks' persistent
instance buffers, walls with the world-detail table and glass without). The marginal geometry encode
(`cpu_geometry_encode_milliseconds`) a ring batch, µs, median of six alternating pairs of processes (one to five
`v2-game` processes ran beside them; contended frames dropped):

| Ring batches | 0.9.0 | 0.10.0 |
| --- | --- | --- |
| World detail on, wall and glass interleaved, distinct meshes (v2's "as it is", 1.31 µs on its machine) | 2.19 | 2.01 |
| the same, one mesh for every batch (v2's "one shared mesh", 0.64) | 1.49 | 1.25 |
| the same, the meshes in the shared buffers | – | 1.38 |
| World detail on, each chunk's walls then its glass, distinct meshes | 1.19 | 0.52 |
| the same, in the shared buffers | – | **0.14** |
| World detail off, interleaved, distinct meshes (v2: 0.76) | 1.45 | 1.26 |
| World detail off, sorted, in the shared buffers | – | **0.11** |

- The 943 batches before the ring: 0.58 → 0.14 µs a batch (runs; 0.40 with runs off).
- Runs take the sorted, shared ring from 0.45 to 0.14 µs a batch; the shared buffers from 0.52 to 0.14 (sorted) and
  2.01 to 1.38 (interleaved). The table/non-table interleave costs 1.24 µs a batch on top (a pipeline switch, about
  0.8 µs, on a material change): sorting by vertex shader removes it.
- 0.9.0 here costs 1.5–2 times v2's figures; scaled, the sorted shared ring would be about 0.1 µs a batch on v2's
  machine, some 0.02–0.03 ms for its 257 batches against the 0.5 ms it measured.
- Prepare (0.03 µs) and shadow encode (0.01 µs) a ring batch are unchanged. The GPU's time wasn't measurable beside
  the `v2-game` processes; the GPU gets the same draws and bytes.

RFC 0010, the smoke's street (a lamp head, its top at 0.6, beside a two-storey house with its roof at 0.735; v2's Low
field, 32^2 texels of a unit, and High, 64^2 of half a unit). The share of the lamp's light kept with tops, against the
same scene with nothing shielded, the same whichever way the street runs (to 0.1 %):

| The lamp, from the roof's edge | High: band keeping half | High: roof's interior (2.5–4 units in) | Low: band | Low: interior |
| --- | --- | --- | --- | --- |
| Half a cell | 1.5 units | 16 % | 0.4 units | 0 % |
| A cell (the middle of a two-cell street) | the whole roof | 78 % | 0.5 units | 75 % |
| A cell and a half | the whole roof | 97 % | – | – |

- The one-storey roof (0.385) and the street keep it all (99.4–100 %), and so do the facades facing the lamp
  (100.0 %, the four ways, both fields).
- Emitters half a cell out, `top_above` 0: 13–16 % in the two-storey roof's interior; a fire at a one-storey roof's
  height lights it fully with `top_above` 0.6.
- So tops keep a glow off the roofs beside it, not off the roofs across the street: the cascades share their ray
  pieces between heights (RFC 0010's implementation notes). The two-band field would; v2 found it not worth twice the
  cost.

RFC 0010's cost: the frame bench's new `+rc` modes (v2 at night: additive cascades over its High field, 256^2 texels
of half a unit, or its Low field, 128^2 of a unit; 200 glowing draws and 32 emitters; 960 x 600, the Deck preset; RTX
3070 Ti, Vulkan). The cascades' passes (the cost log's `rc_src_ms + rc_field_ms + rc_screen_ms`), windows interleaved in
one process; the 10th percentile of every split frame (another process on the GPU only adds time), with a 90 %
bootstrap interval of the difference:

| | High | Low |
| --- | --- | --- |
| Tops off | 2.88 ms | 0.65 ms |
| Tops on | +0.05 ms [−0.10, +0.25] | −0.00 ms [−0.14, +0.16] |
| 0.9.0's cascade shaders under this core | −0.21 ms [−0.43, +0.15] | – |

- No difference stands out of the noise, off or on, at either tier. The trace and finish gather the tops only where a
  source is. The spread between turns is large on this machine (the passes take one or two times their best), so a
  difference under about 0.2 ms (High) or 0.15 ms (Low) can't be told; v2's own frames would.
- Memory: the tops texture, 128 KiB on High and 32 KiB on Low; 4 bytes an emitter.
- Against c5390f0b's core in a process of its own: only an unbalanced run (two windows against five) was quiet, and
  measured the cascades at 1.62 ms (High) and 0.54 ms (Low). The comparison in one process above, which carries every
  core change but the shaders', doesn't show that gap, so it is put down to the processes' different load; a balanced
  base run in a quiet spell couldn't be had while Be The Bullet Hell v2's test used the GPU.

**`SSR_FACING_HITS`'s GPU cost.**
- Setup: `BTBH_SMOKE_BENCH=facing`, the steep street at 1920x1080, reflections without the cascades with
  `SSR_SURFACE_REFLECTIVITY`. Seven series of 30–40 interleaved turns of 20 frames, under the machine's perf lock.
  Four idle `v2-game` processes were open.
- The flag cost nothing measurable. Against unset, the series' fastest turns moved by −0.26 to +0.08 ms (median
  +0.01 ms) and their medians by −0.47 to +0.04 ms. In this scene the reflections themselves cost 0.23–0.48 ms
  against none.
- Unset, the new SSAO resolve and c5390f0b's are within noise of each other: the fastest turns differ by a median
  of −0.05 ms.
- A rejected hit marches on, so a frame whose thin things cover many reflecting pixels (v2's rain, where every wet
  pixel traces) may pay a little more than this scene shows.
- The other changes add a uniform branch each, or (`KEEP_HBAO`'s transparent pass) take the cheaper fast unlit
  shader.

### Upgrading
- Nothing to set: every game gets the runs and the once-a-pass lighting, and draws as before.
- Rust hosts with bindgen regenerate the bindings for `btbh_gpu_window_custom_mesh_create_ex` and the flag; no struct
  changed.
- To gain (v2):
  - Make the kit's, the landmarks' and the set pieces' meshes with `btbh_gpu_window_custom_mesh_create_ex(...,
    BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, ...)`; leave per-stage meshes as they are.
  - Within each chunk, submit the batches with a table first and those without after them (overlays after their
    ground), each group by material, mesh after mesh. The ring's chunks can share one instance buffer, with offsets.
- Swift: nothing changes; a `sharedBuffers:` option can follow when Be The Bullet Hell (v1) asks.
- RFC 0010 (v2): rebuild the shaders with the C core and regenerate the bindings:
  `BTBHGPUSceneRadianceEmitter.reserved` becomes `top_above` and `BTBHGPUSceneRadianceCascades.reserved` (one byte)
  `source_tops`, so `Emitter::raw`, which names `reserved: 0`, stops compiling until it names `top_above`. Then:
  - `radiance_cascades.source_tops = 1` in `Radiance::apply`; set `invalidate` for the frame it changes if the cut
    should be clean;
  - each emitter's `top_above`, as v2 answered: shots, beams, arcs, drones and XP 0 (`position_y` their own height);
    blasts about their radius; fires `position_y` the roof's top and `top_above` 0.6; roof gear 0.3 above its height;
  - the street glows that stay material sources (the kit's glow meshes, the resource buildings, roof gear, and the
    lamp heads if they stay) need nothing: their tops come from their geometry;
  - keep the occluder field as it is: 0 inside a column, the distance to the nearest inside texel outside (an exact
    signed distance that is 0 on the walls classifies the texels on them by direction).
  Zeroed structs keep `source_tops` 0 and draw as before.
- v2's findings and the kaiju shadow (rebuild all 71 shaders and regenerate the bindings: the lighting's
  `directional_shadow_reserved` shrinks to one byte, beside `spot_occluder_flags` and `spot_occluder_reserved`):
  - `KEEP_HBAO` needs nothing new. Its night frames with the neon lights now match the cascades off.
  - Rain with `SSR_SURFACE_REFLECTIVITY`: `post.flags |= BTBH_GPU_SCENE_POST_SSR_FACING_HITS as u8`, and `ssr_thickness`
    can go back toward 0.6.
  - The kaiju shadow: `lighting.spot_occluder_flags = (BTBH_GPU_SCENE_SPOT_OCCLUDER_SUN_ONLY |
    BTBH_GPU_SCENE_SPOT_OCCLUDER_ORTHOGRAPHIC) as u8`, a reach in world units from the map's near plane (or 2.5
    times its depth range for none), and the kaiju out of the sun's map if wanted. If its body shows the spot map's
    acne, `flags |= BTBH_GPU_SCENE_BATCH_RECEIVES_NO_SPOT_SHADOW as u8` on its batches.
  - Don't set the new bits with 0.9.0's shaders: nothing detects them, and they do nothing there.

### Known
- **Be The Bullet Hell v2's frames aren't byte-identical to 0.9.0's with nothing new set**, though every repeat of
  one build matches.
  - Most scenes differ by 11–27 pixels, by 1 level (4 at most): by day, at night, at the map edge, and at dusk on the
    Deck preset.
  - Rain differs by 92 pixels, and the night with its cascades on by 71. Of those, 5–6 pixels in one water glint
    differ by 57–61 levels.
  - This core's smokes show 0 pixels changed (above).
  - The cause isn't established. Five scene fragment shaders were recompiled for the findings (`KEEP_HBAO`'s routing,
    the spot occluder's flags), and a glint is very sensitive to rounding. A comparison of v2's frames with runs off,
    and with 0.9.0's shaders under this core, will tell.
- **With `source_tops`, rays that pass over a source take none of its light and aren't dimmed by it**, as designed.
  Where little street glow is left to shield, the indirect light gets brighter rather than darker. That is v2's night
  with its lamps out of the field: the indirect mean went from 1.36/0.73/0.24 to 1.54/0.90/0.23.
- **`KEEP_HBAO` is ignored with contact AO (`ssao_contact` 1)**, so the fix for transparent unlit batches doesn't
  apply there. Under additive cascades they still take the point lights: in v2's night with its neon, +0.94 L over 19 %
  of the pixels, up to 203 levels. Planned for 0.11.0.

## [0.9.0] — 2026-11-23

Four opt-in additions requested by Be The Bullet Hell v2: a batch that is no source of the radiance cascades' light
(`BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE`); two post flags (`BTBHGPUScenePost.flags`), one that keeps the horizon-based
ambient occlusion of no cascades under additive ones and one that gives the screen-space reflections each surface's
own reflectivity without the cascades; and `btbh_gpu_window_scene_prewarm_render_to_target`. Nothing changes until a
game sets them: unset, every frame renders byte for byte as in 0.8.0. No struct grows (the post's flags take its
last reserved byte), no shader file is added, and eight scene shaders changed: rebuild the shaders with the C core.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes (24/24, 71 shaders), the Swift build and the full Swift
suite (301 tests, 2 skipped, 0 failures). Unset, 0 pixels differ from 0.8.0's core and shaders (0dff164d): the new
flags smoke's gallery of 13 scenes (the HBAO without the cascades, with them and with additive ones through the sun's,
the omni and no shadow, its contact mode, the SSAO under additive cascades, the reflections without the cascades, with
them and with additive ones, the cascades' material sources; 332,800 pixels), the world detail smoke's gallery of 24
scenes through every scene shader variant and its other images (56 images, 1,245,184 pixels),
`btbh_gpu_scene_target_smoke`'s nine images (90,368 pixels), `btbh_gpu_font_smoke`'s 25 (6,549,120 pixels) and
`btbh_gpu_scene_prewarm_smoke`'s 16 frames and 16 renders into a target (1,843,200 pixels), each written by a build of
0.8.0 and by this one; and the world detail gallery and `btbh_gpu_sdf_terrain_smoke` with 0.8.0's shaders under this
core. macOS (Metal), the Vulkan validation layers and a Steam Deck aren't verified.

Be The Bullet Hell v2 tested it: its main at 728e3cf0 on 0.8.0 plus a test patch, 756 tests, and its text check and
44-screen UI tour clean.
- Unset, frames match 0.8.0's within its run-to-run noise. It saw 1-level differences once: 123 pixels with the
  cascades at night and 1 in rain. Two runs of 0.8.0 differ as much (122 and 30 pixels).
- The prewarm into a target takes the first present of its V1 cards' first offer from 12–14 ms to 1.1–1.2 ms, and that
  frame from 32.5/31.8 ms to 19.5/19.8 ms. With its lane scenes also made at load, the frame is 4.3/2.9 ms. Steady
  lanes cost 0.3–0.4 ms a present.
- `NO_RC_SOURCE` on its city glows (night, stage 3) takes the cascades' light from 3.57/2.24/1.35 to 1.88/1.54/1.30.
  The warm wash over roofs and streets goes, and the final frame's luma drops by 0.55.
- `KEEP_HBAO` with nothing feeding the cascades equals the frame without them within 9–13 pixels by 1 level, by day
  and at night, with its neon point lights off (see Known).
- `SSR_SURFACE_REFLECTIVITY` costs GPU time where the wet streets trace (preset High, GPU-bound, medians of three):
  - 1440p: +0.17 ms by day, +0.31 ms in rain;
  - 4K in rain: +0.81 ms, because every wet street pixel now traces.

  By day, glass facades reflect correctly.

### Added
- **`BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE`** (bit 2 of `BTBHGPUSceneGeometryBatch.flags`, beside `NO_SUN_SHADOW` and
  `KEEP_ORDER`): with the radiance cascades on, the batch isn't drawn into their source field, so its emission (and,
  unlit, its colour) lights nothing round it through them. v2 asked for it because hundreds of small city glows
  added up to an even warm wash over every street at night (its stage 3: a final-indirect mean of 27.7/19.9/2.6 of
  255 with no gameplay emitter at all).
  - Everything else stays: the batch is drawn, glows on screen with its own emission, is lit, takes the cascades'
    bounce light (lit, or unlit with `rc_receiver`) and casts its shadows as before.
  - The occlusion is unchanged: the cascades' occlusion is the host's (`occlusion_cascades`), which no batch draws
    into, so a batch blocks their light where that says so, with or without the flag.
  - An unlit batch with it is no source either (`rc_receiver` also makes it take the bounce light). Without the
    cascades, or on a batch that doesn't glow, it changes nothing.
  - With cascades that aren't additive, a flagged glow's emission on screen still reaches a few pixels round it
    (up to 6 measured) through the cascades' screen pass, which adds a surface's emission at its own resolution,
    as before. With additive ones its pixels are exactly the box's.
- **`BTBHGPUScenePost.flags`** (byte 427, the old `ssao_reserved[0]`: the struct keeps its 432 bytes). An unknown
  bit fails the present (and the prewarms) with `BTBH_GPU_INVALID_ARGUMENT`.
  - **`BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO`**: the horizon-based ambient occlusion (`ssao_enabled` 2,
    `ssao_contact` 0) under additive cascades darkens the ambient light of lit surfaces, as without the cascades,
    instead of three quarters of all their light but the emission. Without it, enabling the cascades with nothing
    glowing darkened v2's tuned night (stage 3: -1.4 sRGB levels on average, OKLab dL -0.56, 11.8 % of the pixels
    darker by more than 4 levels).
    - With nothing glowing, a frame is then the frame without the cascades, to the byte (reflections off).
    - Flipping the resolve's mode wasn't enough: under the cascades the opaque pass writes the materials (albedo,
      F0) where the HBAO's mode 1 reads the ambient light. With the flag, the cascades' scene shaders write the
      scene's own ambient targets (as without the cascades) and the albedo the bounce light lands on, times 1 -
      metalness, to the emission target, which the additive resolve reads it from instead
      (`production_hrc_resolve.comp`).
    - The bounce light is the same, but at the edges of metal surfaces (up to 2 levels within 2 pixels measured:
      the resolve samples at its own resolution and blended the albedo and the metalness apart). The bounce light
      isn't darkened by the occlusion (it isn't ambient light).
    - The reflections under it weigh each surface by its reflectivity's luminance, as with
      `SSR_SURFACE_REFLECTIVITY` (the cascades' targets kept its colour).
    - Ignored, rendering as without it: no cascades, cascades that aren't additive, `ssao_contact` 1 (the same with
      the cascades or without), `ssao_enabled` 0 or 1, and the cascades' debug views.
  - **`BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY`**: the screen-space reflections without the cascades weigh each
    surface by its own reflectivity, as they do with the cascades: its F0 (0.04 for a dielectric, 0.12 for glass, a
    metal's albedo). The opaque pass then writes the F0's luminance to the ambient target's alpha (a constant 1
    before, which nothing read) and marks unlit surfaces in the emission target's alpha.
    - A dielectric, glass and a grey metal reflect byte for byte as under additive cascades with nothing glowing
      (which read the F0 from their targets); a coloured metal reflects as bright, untinted (one channel).
    - Unset, the reflections without the cascades reflect nothing, as before (see Known); v2 read that as glass and
      metal reflecting too weakly. With the cascades on, or without the reflections, the flag changes nothing.
- **`btbh_gpu_window_scene_prewarm_render_to_target`**: `btbh_gpu_window_scene_prewarm` for
  `btbh_gpu_window_scene_render_to_target` into a target. v2's live weapon previews ("V1 cards") hitched 54–56 ms on
  the first render of a session while that call's own scene runtime started.
  - It makes what those renders would make the first time: their runtime, each batch's model pipelines for the
    settings, their shadow maps, the cascades' and SMAA's or FXAA's resources, their scene targets at the render
    size and their post chain at the target's size. The renders then make no shader, pipeline or target.
  - It runs the window prewarm's steps with what `render_to_target` does to the settings first (FSR 3 runs as FSR
    1; a render at the target's size or bigger is scaled bilinearly), in that call's runtime, swapped in as
    `render_to_target` swaps it: the three share those steps and can't drift apart. It draws nothing; the window's
    frames and their runtime are left alone; a render is the same to the byte with or without it.
  - The renders into every target of a window share one runtime: prewarm with the render size and target size they
    will use. A second call makes nothing.
  - Refusals: the window prewarm's, and a target `render_to_target` refuses (NULL, another window's, not a render
    target, an array), with `BTBH_GPU_INVALID_ARGUMENT`. The report gives the target's size and the upscaling the
    renders will run.
- `BTBH_GPU_SCENE_HAS_BATCH_NO_RC_SOURCE`, `BTBH_GPU_SCENE_HAS_ADDITIVE_CASCADES_KEEP_HBAO`,
  `BTBH_GPU_SCENE_HAS_SSR_SURFACE_REFLECTIVITY` and `BTBH_GPU_HAS_SCENE_RENDER_TO_TARGET_PREWARM`, defined with them.
- Swift: `ScreenSpaceReflectionDescriptor.surfaceReflectivity` (default false), which sets
  `SSR_SURFACE_REFLECTIVITY`. The batch flag, the HBAO flag and the prewarm have no Swift API (the Swift layer
  exposes neither batch flags, the HBAO nor additive cascades).
- Tests:
  - `btbh_gpu_scene_order` (no GPU): the flags' bits and the post's layout.
  - `btbh_gpu_scene_flags_smoke` (new; hidden windows, a debug device): the unset gallery above
    (`BTBH_SCENE_FLAGS_DUMP` and `_REFERENCE` compare two builds; it builds against 0.8.0's header). Set:
    - `NO_RC_SOURCE`, on and additive: a glow and an unlit source outside the view light the ground in view
      (15,000 pixels differ); flagged, they render byte for byte as the scene with the glow's emission at 0 and the
      source a receiver, and so does the scene with every batch flagged. In view, a flagged glow differs from no
      emission on its own pixels only, and from the unflagged one. Without the cascades it changes nothing.
    - `KEEP_HBAO`: with nothing glowing, additive cascades and the HBAO render byte for byte as no cascades through
      the sun's, the omni and no shadow (without it 3,571–4,115 pixels differ). With glowing boxes and an HBAO that
      darkens nothing, the bounce light is the same to the byte without metal, and within 2 levels and 2 pixels of
      the metal boxes. With the reflections it renders as no cascades with `SSR_SURFACE_REFLECTIVITY`. Ignored where
      it doesn't apply (4 cases).
    - `SSR_SURFACE_REFLECTIVITY`: unset reflections without the cascades render as none; set, they appear and match
      additive cascades with nothing glowing to the byte through the three shadow variants; a gold metal differs.
      With the cascades on, or without the reflections, it changes nothing.
    - Refusals: an unknown batch flag, unknown post flags (present and prewarm).
    - `BTBH_SMOKE_BENCH=1` (or `=ssr`, `=hbao`) times them (`BTBH_SMOKE_BENCH_ROUNDS`, and
      `BTBH_SCENE_FLAGS_BASELINE_SHADER_DIR` for 0.8.0's shaders in a second window).
  - `btbh_gpu_scene_prewarm_smoke`: the prewarm's sequence rendered into a target (FSR 1 into twice the render
    size). Cold, the first render makes 35 shaders and 30 pipelines and the effects' first 22 and 11; prewarmed for
    the target, the call makes exactly those, a second call nothing, no render anything, and every render matches
    the cold window's to the byte; the window's own first frame still makes its pipelines. The same with the
    cascades and an omni spot shadow (69 shaders, 55 pipelines). Its refusals. `BTBH_SMOKE_BENCH=target-cold` or
    `=target-prewarm` times it after four frames of the window; `BTBH_PREWARM_SMOKE_DUMP` and `_REFERENCE` cover
    the renders too.

### Changed
- Shaders: `production_model.frag` and its five variants write their outputs through two new bits of the fragment
  `unlit_enabled` (64: the reflectivity without the cascades; 128: the scene's own targets under additive cascades),
  `production_ssao_resolve.frag` reads the reflectivity from the ambient target's alpha when told to
  (`ssr_options.y`, reserved before), and `production_hrc_resolve.comp` the albedo from the emission target (a
  padding word renamed). No binding, uniform layout or file changed; the other 63 shaders are 0.8.0's SPIR-V.
- `btbh_gpu_window_scene_render_to_target` takes its settings, target check and runtime swap from the steps it now
  shares with its prewarm (no pixel changes).
- `tools/check.ps1` and `tools/check.sh` skip the new smoke with the other GPU smokes.

### Measured
Windows, RTX 3070 Ti (driver 621.14), Vulkan, without a debug device. Another session ran Be The Bullet Hell v2 on
the same GPU throughout (up to 100 % load): medians ran up to several times the fastest turns, and only the quietest
series is quoted for the flags' GPU cost.

**The render-to-target prewarm.** `BTBH_SMOKE_BENCH=target-cold` and `=target-prewarm`, seven runs of each,
alternating, in separate processes: the window presents four 1920x1080 frames first (its own runtime warm, as a
game's), then renders the sequence into a 960x540 target rendered at 480x270. CPU milliseconds of the call, median
(range):

| | Without the prewarm | With it |
| --- | --- | --- |
| The target prewarm call | – | 38.9 (27.3–51.8) |
| First render | 26.2 (21.9–47.2) | 2.0 (1.3–2.7) |
| Second render | 0.31 (0.23–0.37) | 0.27 (0.20–0.30) |
| First render with the effects (frame 6) | 18.1 (12.8–32.2) | 0.40 (0.37–0.55) |

- Cold, the first render makes 35 shaders and 30 pipelines (4.6–7.1 ms in the creation calls) and the effects' 22
  and 11 (6.2–10.0 ms); the prewarm makes the 57 and 41 together. The smoke's own sequence (a debug device, 160x90
  into 320x180) measured 51.7 against 2.3 ms for the first render.
- A first prewarmed render still costs 1–2 ms more than later ones, as the window prewarm's first frame does.

**The flags.** `BTBH_SMOKE_BENCH=1`, 1920x1080, 30 interleaved turns of 20 renders, fastest turn (median), ms a
frame, the quietest of seven series:

| | Unset | Flag set | 0.8.0's shaders, unset |
| --- | --- | --- | --- |
| Reflections without the cascades (`SSR_SURFACE_REFLECTIVITY`) | 0.441 (0.553) | 0.615 (0.747) | 0.448 (0.490) |
| Additive cascades, HBAO and reflections (`KEEP_HBAO`) | 1.163 (1.431) | 1.160 (1.446) | 1.134 (1.375) |

- `SSR_SURFACE_REFLECTIVITY` costs what the reflections cost once they run: +0.17 ms here (+0.17 to +0.57 ms in the
  other series). Unset they return before marching (their pass costs +0.07 ms against no reflections).
- `KEEP_HBAO` costs nothing measurable (-0.003 ms here, -0.50 to +0.17 in the contended series).
- Unset against 0.8.0's shaders: within the noise (-0.007 and +0.029 ms here, both signs in the other series).

**The HBAO under additive cascades**, in the smoke's street with nothing glowing (160x160): without the flag
-0.23 to -0.38 sRGB levels on average and 1.6–2.8 % of the pixels darker by more than 4 levels against no
cascades; with it, 0 pixels differ.

### Upgrading
- Nothing to set: a zeroed batch and post (`= {0}`, `..Default::default()`) render as in 0.8.0.
- Rebuild the shaders with the C core. 0.8.0's shaders under this core render unset frames byte for byte as these.
  With them, `SSR_SURFACE_REFLECTIVITY` changes nothing (the reflections still see every surface as unlit), but
  **don't set `KEEP_HBAO` with older shaders**: their cascades' variants write the materials where the occlusion
  then reads the ambient light, and it darkens by the albedo. Neither flag is detected (they add no named resource
  for the stale-shader check to find).
- Rust hosts with bindgen regenerate the bindings: `BTBHGPUScenePost.ssao_reserved` (one byte) becomes `flags`. A
  host that names every field sees it go; `..Default::default()` doesn't. A build script can gate on the four
  `HAS_` macros.
- To adopt them (v2):
  - `flags |= BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE` on the city's small glows (lit windows, signs, street lamps'
    lenses), leaving the gameplay emitters' and effects' batches as they are.
  - `post.flags |= BTBH_GPU_SCENE_POST_ADDITIVE_CASCADES_KEEP_HBAO` with `radiance_cascades.additive` 1 and the
    HBAO, where the cascades should leave the tuned look alone.
  - `post.flags |= BTBH_GPU_SCENE_POST_SSR_SURFACE_REFLECTIVITY` to A/B the reflections without the cascades.
  - `btbh_gpu_window_scene_prewarm_render_to_target` at load with a card's target, its render size and settings, and
    the batches the previews draw.
- Swift: `ScreenSpaceReflectionDescriptor(..., surfaceReflectivity: true)`; existing calls compile unchanged.

### Known
- **The reflections without the cascades have reflected nothing since they came** (e7def301): every lit surface
  writes 1 to the emission target's alpha, which the reflections take for unlit and skip. Kept unset, so frames
  stay as they were (Be The Bullet Hell v1 runs them so); `SSR_SURFACE_REFLECTIVITY` makes them reflect.
- `KEEP_HBAO`'s bounce light differs by up to 2 levels at the edges of metal surfaces (above).
- The SSAO (`ssao_enabled` 1) under additive cascades keeps the cascades' mode: `KEEP_HBAO` covers the HBAO only.
- **`KEEP_HBAO` doesn't match the frame without the cascades under strong point lights in occluded corners.**
  - Case: Be The Bullet Hell v2's sign lights at a wall's foot (reach 2.6, energy 2.1).
  - With the flag, those corners are up to 45–50 levels brighter than without the cascades: +0.8 luma over its night
    frame, +0.3 at dusk on the Deck preset.
  - It isn't established which of the two darkens the point lights' share. Under investigation for 0.10.0.
- **The radiance cascades' debug views draw the transparent pass over them**, so additive effects such as street
  washes and lamp pools read as cascade light there.
- **`SSR_SURFACE_REFLECTIVITY` in rain under a steep camera:** each actor leaves a noisy vertical streak of itself
  down the wet street below it. An `ssr_thickness` of about 0.1, down from 0.6, removes most of it.

## [0.8.0] — 2026-11-22

One opt-in addition requested by Be The Bullet Hell v2: the world-detail table chosen per vertex (phase 2 of
[RFC 0009](docs/rfc/0009-world-detail-table.md)), with the 2D array textures RFC 0001 drafted. Nothing changes until a
game sets it: a batch without a table, and every existing feature, renders byte for byte as in 0.7.0, and costs what
it cost there (Be The Bullet Hell v2 found the candidate's static draws dearer; see Measured). The batch grows from
272 to 280 bytes, the scene fragment shaders take one more sampler and uniform buffer, and a static batch with a
table draws with a new vertex shader, `production_model_detail_table.vert`: rebuild the shaders and regenerate the
bindings together with the C core.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes (23/23, 71 shaders), the Swift build and the full Swift
suite (301 tests, 2 skipped, 0 failures). `production_model.vert` and `production_animated_model.vert` compile to
0.7.0's SPIR-V. With no table, 0 pixels differ from 0.7.0's core and shaders (a40d908a): the
world detail smoke's gallery of 24 scenes through every scene shader variant (393,216 pixels; world details, values
in the joint slots and a skinned model among them), `btbh_gpu_scene_target_smoke`'s nine images (90,368 pixels),
`btbh_gpu_font_smoke`'s 25 (6,549,120 pixels) and `btbh_gpu_scene_prewarm_smoke`'s 16 frames (921,600 pixels), each
written by a build of 0.7.0 and by this one; and the gallery and `btbh_gpu_sdf_terrain_smoke` with 0.7.0's shaders
under this core. A table draws byte for byte as the same faces split into one draw per class with single packed
details, through the six scene shaders, and as the candidate (f96cd45d) drew it. macOS (Metal), the Vulkan
validation layers and a Steam Deck aren't verified.

Be The Bullet Hell v2 tested it (its main at dec1a3b7, 71 shaders built from this list, 751 tests, its UI tour of 43
screens clean), with the table on its merged distance models, its full per-class models (a constant index per mesh)
and its outskirts ring. Deck preset at 1280x800 on a desktop RTX, two interleaved passes:
- Frames at frame 900 against 0.7.0 are byte-identical without a table (Deck by day, High in the storm), with the
  full models on the table, with its resource marks and night pane tints, over its outskirts ring and in its
  render-to-target card previews. They differ only on the merged distance models, where the detail lands.
- GPU: the table costs what 0.7.0's split per-class draws cost by day (1.182–1.185 ms against 1.184–1.199 ms), and
  0.02 ms more in the storm's main pass; against merged models without distance detail, 0.05–0.07 ms more.
- CPU (a CPU-bound frame by day): 1,296 draws and 0.87–0.89 ms of geometry encoding against 1,739 and 1.38–1.40 ms
  split, so the frame is 0.14–0.24 ms shorter than split.
- Without a table, the total GPU time is within noise of 0.7.0's. The main pass alone measured about 0.02 ms more
  in all four of its pairs (see Known).

### Added
- **`BTBHGPUSceneGeometryBatch.world_detail_table`** (appended at byte 272): up to eight world details in one batch,
  chosen per vertex (`BTBHGPUSceneWorldDetailTable`, `BTBHGPUSceneWorldDetailTableEntry`, 24 and 36 bytes).
  - The table holds one 2D array texture (`map_layers`), the entries and their count (1..8), and one seed component
    for all of them. Each entry is `BTBHGPUSceneWorldDetail`'s eight floats (its bytes 24..55, so a host can copy
    them across) and the array layers of its normal and its packed ORM (the grey albedo in blue). 255
    (`BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER`) leaves that map out.
  - Each vertex's index is the first float of its joint slot (`BTBH_GPU_CUSTOM_MESH_DETAIL_INDEX_FLOAT`, byte 64),
    rounded and clamped to 0..255 (NaN is 255). A triangle takes `entries[index]`, projected and blended as a
    packed world detail. At or past the count (255 by convention), it has no detail. All three vertices of a
    triangle must carry the same index.
  - A skinned batch takes entry 0 everywhere (its joints are bones), and so does a model loaded without joints.
  - One draw per merged distance model instead of one per class: v2 measured +0.6–0.85 ms a frame on its Deck preset
    for the split. Its full models can use the same table with a constant index, so the macro maps load once.
  - The present refuses, with `BTBH_GPU_INVALID_ARGUMENT` naming the batch (and the entry): an entry count of 0 or
    above 8, NULL entries, a tile not finite and above 0, a scale not finite, an angle outside [0, 90), a layer
    neither 255 nor below the array's count, a seed component above 4, flags or reserved bytes set, `map_layers`
    that is 2D, of another window or destroyed, and a batch with both a table with `map_layers` and a
    `world_detail` with a texture (refused, not one winning; a `world_detail` without textures may stay).
  - A table without `map_layers`, or on a refracting batch, draws as none.
- **2D array textures** (RFC 0001's functions, unchanged): `btbh_gpu_window_texture_array_create_rgba8_ex` and
  `btbh_gpu_window_texture_array_load_ex`. 1..256 layers (`BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS`) of one size and
  colour space, each with its own mips. The report gives one layer's size and mips.
  - Refused, with the reason: 0 or more than 256 layers, NULL layers, a zero size, an unknown filter, wrap or
    colour space, more than 4 GiB, and (loading) a layer of another size than the first, naming its path. A device
    that can't sample a 2D array of the format: `BTBH_GPU_MISSING_CAPABILITY`.
  - Every 2D slot refuses an array: a batch's four textures and a world detail's three (naming the batch), the
    canvas's texture and target, the post LUT, the render-to-target target, the capture destination and
    `btbh_gpu_texture_update_rgba8`.
- The custom-mesh vertex is documented: `BTBH_GPU_CUSTOM_MESH_VERTEX_FLOATS` (24) and
  `BTBH_GPU_CUSTOM_MESH_DETAIL_INDEX_FLOAT` (16).
- `BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE` and `BTBH_GPU_HAS_TEXTURE_ARRAYS`, defined with them.
- Tests:
  - `btbh_gpu_scene_order` (no GPU): the batch's, the table's and the entry's layouts, the entry's floats at the
    detail's offsets minus 24.
  - `btbh_gpu_world_detail` (no GPU): the C copy of the vertex index, the entry choice and the 400-byte block, and
    every refusal of a table and an entry.
  - `btbh_gpu_world_detail_smoke`: merged against split, constant-index full models against single details, none
    and past the count against no detail, a skinned and a still model, through the six scene shaders (0 pixels
    differ); a merged mesh read back pixel by pixel against the C copy (the entry each pixel took, and the texel);
    arrays and their refusals; 21 refusals through the present; stale shaders. The unset gallery grows from 15 to
    24 scenes. The bench: `BTBH_SMOKE_BENCH=1`, or `=table` for this one only (`=vertices` for the unset vertex
    bench alone; `BTBH_SMOKE_BENCH_ROUNDS` up to 40 turns). The old-shader check adds a shader directory without
    `production_model_detail_table.vert`.
  - `btbh_gpu_frame_bench` (built, not run by ctest) with `tools/frame_bench_summary.py`: a frame like v2's city at
    its Deck preset (about 2,000 instanced draws of small kit pieces, 2.2 million triangles), one hidden window per
    shader directory and mode (no table, or a table with 85 %, none or all of the triangles on an entry), timed
    with the cost log's split frames. It builds against 0.7.0's header and library too.

### Changed
- Shaders: a static batch with a table draws with a new vertex shader, `production_model_detail_table.vert` (the
  source of `production_model.vert` with `BTBH_WORLD_DETAIL_TABLE`), which reads the joint slot's first float
  (location 5, after the tangent, so 0..4 stay) and passes the index in the flat `animated_model` value, as
  −1 − index. `production_model.vert`, which every other static batch draws with, and
  `production_animated_model.vert` are 0.7.0's (the same SPIR-V). `production_model.frag` and its five variants take
  the table's array after the world detail's three (15, 14 and 10 samplers) and its entries in a third uniform
  buffer; a batch without a table binds a 1x1 array. The single detail's code is unchanged. The unlit,
  radiance-cascade source and shadow shaders don't change.
- The static model pipelines describe six vertex attributes (five before); only the table's vertex shader reads the
  sixth. A static batch with a table has pipelines of its own, made with the new file from the directory of the
  host's static vertex shader; without that file such a present fails, naming it.
- A pass pushes a table's 400-byte block once for a run of batches that share it (SDL keeps it for the later draws),
  not once a batch.
- The stale-shader check: a table batch needs `world_detail_maps` in the scene fragment shader. And it now reads
  the variant the present draws with (`production_model_2d_shadows`, the unshadowed one, `production_model_rc*`)
  as well as the host's `production_model.frag`, each path once. 0.6.0 and 0.7.0 read only the latter, so a stale
  variant beside a current fragment shader went unnoticed for a dug or detailed batch.

### Measured
Windows, RTX 3070 Ti (driver 621.14), Vulkan. `BTBH_SMOKE_BENCH=table` with 0.7.0's shaders as the baseline: eight
coplanar screen-filling walls drawn with `LESS_EQUAL`, the heavy pixel (soft sun shadow, 40 lamps, 32 point lights)
and a light one; five runs of 10 or 30 interleaved turns of 20 frames, the fastest turn of each configuration.
Another session benchmarked Be The Bullet Hell v2 on the same GPU throughout (up to 99 % load, 88 °C): medians ran up
to three times the fastest turns, and the heavy pass at 1920x1080 is lost in that noise (−0.14 to +0.20 ms either
way). Per screen of detailed surface:

| | Light, 1920x1080 | Light, 960x600 | Heavy, 960x600 |
| --- | --- | --- | --- |
| Single packed detail (0.6.0's) | +0.053 to +0.056 ms | +0.015 ms | −0.004 to +0.014 ms |
| Table, one entry | +0.053 to +0.060 ms | +0.014 to +0.015 ms | +0.005 to +0.014 ms |
| Table, two entries in alternate 17-pixel strips | +0.054 to +0.063 ms | +0.015 ms | +0.006 to +0.014 ms |
| Table against the single detail | −0.001 to +0.007 ms | −0.001 to 0.000 ms | 0.000 to +0.010 ms |
| No table, against 0.7.0's shaders | −0.008 to −0.001 ms | 0.000 ms | +0.003 to +0.008 ms |

- A table costs what the single packed detail costs, within the bench's noise; two entries in one wave cost the
  same. RFC 0009 estimated 0.04–0.05 ms a 1080p screen and the packed detail plus a few percent.
- **CPU:** 200 merged batches with one table take 0.08–0.11 ms to prepare and encode; 1,000 split ones with single
  details 0.48–0.66 ms (the present call: 0.13–0.18 against 0.53–0.73 ms). The split's 800 extra batches cost
  0.40–0.55 ms, 0.5–0.7 µs each: v2 measured +0.6–0.85 ms a frame for its split.
- **Unset, many vertices** (the candidate, f96cd45d): 20,000 small boxes in one instanced draw (480,000 vertices)
  at 1920x1080 took 0.016–0.023 ms more than with 0.7.0's shaders on a 0.52 ms frame. That was the sixth vertex
  attribute, which only a table batch's vertex shader reads now (below).
- **Memory:** v2's sixteen 512² maps with mips are 21.3 MiB as one array, as they are as sixteen 2D textures.

**After Be The Bullet Hell v2 tested the candidate (f96cd45d).** v2's stage 3 at its Deck preset (1280x800, RTX 3070
Ti, GPU median, ms; "merged": the merged distance models without a detail):

| Scene | No detail, 0.7.0 | No detail, candidate | Table | Split, 0.7.0 | Merged |
| --- | --- | --- | --- | --- | --- |
| Day | 1.33 | 1.40 | 1.49 | 1.44 | 1.36 |
| Storm | 1.30 | 1.37 | 1.44 | 1.40 | 1.31 |

Without a table the candidate cost 0.06–0.07 ms more (about 5 %), in the opaque pass (0.91–0.92 → 0.98–0.99 ms), and
the table more than the split. `btbh_gpu_frame_bench` builds a frame like it: 2,000 instanced draws of small kit
pieces, 2.2 million triangles at 960x600, FSR 1 to 1280x800, HBAO, FXAA, the soft sun shadow, pattern 7. Hidden
windows of one process take turns, shuffled every round and rotated over the windows between runs; each turn is
paired with the reference's turn of its round, and rounds another process slowed are dropped (another session ran
v2 and a game on this GPU on and off). The opaque pass against 0.7.0's shaders, median of the pairs (36 runs):

| Static vertex shader reads location 5 | Index in an output of its own | Fragment table code | Opaque pass | Frame |
| --- | --- | --- | --- | --- |
| yes | yes | yes (the candidate) | +0.014 ms | +0.020 ms |
| no | yes | yes | +0.000 ms | +0.010 ms |
| yes, packed into `animated_model` | no | yes | +0.027 ms | +0.035 ms |
| no | no | yes (this release) | −0.001 ms | −0.001 ms |
| no | yes | no | −0.003 ms | +0.003 ms |

- **The cause is the sixth vertex attribute**: every configuration that reads it pays and none that doesn't. The
  index's own flat output and the fragment shader's table code cost nothing measurable, and 0.7.0's shaders under
  this core cost what they cost under 0.7.0's core (+0.001 ms), so the core's per-draw changes don't either. The
  driver gives the scene shaders 0.7.0's registers (64 vertex, 80 fragment).
- **This release without a table, against 0.7.0** (the same frame): in one process (32 runs, configurations rotated
  over the windows, 80 of 128 rounds kept), −0.008 ms on the opaque pass (10th percentile −0.014) and −0.017 ms
  (−0.013) on the frame; the candidate measured +0.021 (+0.023) and +0.023 (+0.023) in the same runs, and a second
  copy of 0.7.0's shaders +0.003 (+0.002). Against 0.7.0's core and shaders in alternating processes: +0.009 and
  +0.013 ms on the opaque pass (10th percentile +0.013 and +0.012; two sets of 20 and 16 pairs), where the candidate
  measured +0.033 (+0.029); 0.7.0's shaders under both cores, +0.002. Separate processes scatter by up to 0.03 ms; the
  two methods differ by about 0.02 ms on what remains, and both put the candidate's extra cost back.
- **No samples for none, measured and not kept.** Branching on the flat index so that a triangle at or past the count
  (or a layer of 255) samples nothing saved nothing with 85 % of the triangles on 255 (+0.007 ms) and cost
  +0.029–0.035 ms where every triangle samples; returning early on the index alone saved 0.003–0.011 ms and cost
  0.013–0.019 ms. The candidate's table cost the same on both meshes: those samples aren't where a table's cost is.
  The table samples and selects, as before.
- **The table, before and after:** pushing its block once a run instead of once a batch took its cost from +0.126 to
  +0.090 ms (85 % of the triangles on 255 or all of them alike). Against no table, the candidate's core and shaders
  against this release's: with 85 % of the triangles on 255, +0.148 → +0.109 ms on the opaque pass and +0.151 → +0.109
  ms on the frame; with every triangle on an entry, +0.147 → +0.107 and +0.144 → +0.104 (16 alternating pairs, each
  against 0.7.0's shaders without a table in its own process). Where the rest goes isn't established; the table's
  vertex shader reads the sixth attribute (0.014–0.027 ms above), and with every triangle on 255 the table still costs
  +0.089 ms.
- Not measured: a Steam Deck (on AMD every vertex output costs too, and this release's static vertex shader passes
  on what 0.7.0's did), v2's own frames with this release.

### Upgrading
- Nothing to set: a zeroed batch has no table.
- Rebuild the shaders with the C core. 0.7.0's shaders still work under this core and draw everything as before
  (the extra attribute, sampler and uniform buffer go unread); with them a table batch fails the present with the
  shader's path. This release's shaders don't work under an older core.
- **A new shader file: `production_model_detail_table.vert`** (in `CMakeLists.txt`'s list after
  `production_model.vert`; SPIR-V and MSL, a metallib on Apple). The present loads it from the directory of the
  static vertex shader it is given, for a static batch with a table only: a host that draws a table needs it beside
  `production_model.vert`; one that doesn't can leave it out. Hosts that name their shaders add it to the list
  (CHIMERA's and Tyrian3D's come from `CMakeLists.txt`, and v2 builds that list). The candidate's shaders (f96cd45d)
  have no such file: under this core their table batches fail the present with its path; rebuild them.
- The batch is 280 bytes: Rust hosts with bindgen regenerate the bindings (`..Default::default()` leaves the table
  NULL); a build script can gate on `BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE`.
- To adopt it (v2): load the sixteen macro maps as one array (`btbh_gpu_window_texture_array_load_ex`, linear,
  repeat, mips), fill eight entries, write each kit vertex's entry into `joints[0]` (255 for the untextured
  materials), and set `world_detail_table` on the merged distance models (and on the full models, with a constant
  index), leaving `world_detail` NULL. Then the eight 2D detail sets and `BTBH_LOD1_SPLIT` can go. See RFC 0009,
  "Implementation notes (phase 2)".
- Swift: no API.

### Known
- Without a table, Be The Bullet Hell v2's main pass measured about 0.02 ms more than with 0.7.0 in each of its four
  pairs (0.72–0.73 ms against 0.74–0.77 ms by day), though its total GPU time stays within noise. This release's own
  bench put the residual within ±0.013 ms, with both signs. Its cause isn't established: the vertex shaders are
  0.7.0's SPIR-V, so what differs is the fragment shaders' unused table path and their bindings (a third uniform
  buffer, a one-layer fallback array).

## [0.7.0] — 2026-11-20

Two opt-in additions requested by Be The Bullet Hell v2: `btbh_gpu_window_scene_prewarm`, which makes the
scene's pipelines, shaders and targets at load, and pattern 7 of the procedural surface detail, the facade's
weather without its grain (phase 1 of [RFC 0009](docs/rfc/0009-world-detail-table.md)). Nothing changes until a
game uses them: every other pattern, and a zeroed batch, renders byte for byte as in 0.6.0. The scene fragment
shaders changed: rebuild the shaders together with the C core.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes (23/23), the Swift build and the full Swift suite
(301 tests, 2 skipped, 0 failures), on the two features together. With pattern 7 unused, 0 pixels differ from 0.6.0's core and shaders: a
gallery of 15 scenes, patterns 0–6 among them, through every scene shader variant (245,760 pixels), and
`btbh_gpu_scene_target_smoke`'s nine images (90,368 pixels), each written by a build of 0.6.0 and by this one.
Pattern 7 renders byte for byte as pattern 3 with x and y at 0 through the six scene shaders, dry and soaked
(393,216 pixels). The prewarm's smoke shows that its frames make nothing and match a cold window's to the byte.
macOS (Metal) isn't verified.

Be The Bullet Hell v2 tested it (bindgen regenerated, all 70 shaders rebuilt):
- The same v2 source against 0.6.0 and this release gives byte-identical frames at frame 900 in six scenes (day,
  dusk, storm and night, the Deck preset by day, Low in the storm), with the prewarm on or off and pattern 7 off.
- Its storm stage on High (about 2,120 batches): the prewarm takes 1.88 s the first run after a shader build, then
  23–36 ms. Frame 0's present goes from 93–121 ms to 67–89 ms, of which the scene's preparation from 22.8 to
  0.3 ms.
- Pattern 7 on Low and the Deck preset: nothing measurable dry, about 0.01 ms in the rain. The street darkening and
  the wet walls are subtle from its classic camera but visible.

### Added
- **Pattern 7, `BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER`**: a host sets `detail_parameters` to `{ 0, 0, 0, 7 }`
  (Be The Bullet Hell v2's `[0, 0, 0, 7]`). It is pattern 3's weather without the triplanar noise, the stains and
  the bump:
  - every face darkens toward the street: × 0.8 at the flat world height 0, rising to × 1 at a quarter of a render
    unit;
  - with `BTBHGPUSceneLighting.surface_wetness`, steep faces turn a shade darker and semi-gloss (at 1: × 0.85 and a
    roughness of 0.3 on vertical walls). Level faces only darken.
  - It renders byte for byte as pattern 3 with x and y at 0, whatever pattern 3's z, without the noise's 40 hashes
    a pixel.
  - x, y and z mean nothing yet and must be 0. The present fails with `BTBH_GPU_INVALID_ARGUMENT` for pattern 7
    with any of them set, and for any pattern above 7.
  - For v2: on facades where the world detail replaces the grain, and as `SurfaceLook::flat()`'s facade on Low and
    the Deck preset, which lost the darkening and the wet walls with the grain.
- **Names for the patterns**: `BTBH_GPU_SCENE_DETAIL_NONE`, `_GROUND`, `_GRASS`, `_FACADE`, `_PAVING`, `_GLASS`,
  `_LAMP_LENS` (0–6, as before) and `_FACADE_WEATHER` (7). `BTBH_GPU_SCENE_HAS_FACADE_WEATHER`, defined with them.
- Swift: `SurfaceDetail.Pattern.facadeWeather` and `SurfaceDetail.facadeWeather`. It needs nothing but its
  pattern and goes native as `{ 0, 0, 0, 7 }`, whatever its other fields say.
- Tests:
  - `btbh_gpu_scene_order` (no GPU): which detail parameters the present accepts, pattern 7 and the rounding
    included.
  - `btbh_gpu_world_detail_smoke`: pattern 7 against pattern 3 with x = y = 0 (z 6 and 0.37) through the six scene
    fragment shaders, dry and soaked (0 pixels differ); its refusals; under 0.6.0's shaders it draws as no pattern;
    and its bench (`BTBH_SMOKE_BENCH=1`, or `=weather` for this one only). The unset gallery grows from 8 to 15
    scenes: patterns 0–6 together through the six shaders, soaked in the rain at night, and once dry by day.
- **`btbh_gpu_window_scene_prewarm`** (asked for by Be The Bullet Hell v2, whose runs stalled for 22–32 ms about
  0.3 s in): makes at load what a window's scene presents would make the first time they draw. A host calls it
  before its first frame with the shader paths, render size, lighting, post settings and batches the frames will
  present. Those frames then make no shader, pipeline or target.
  - It makes each batch's model pipelines for those settings (the sun's, the omni or no shadow; the radiance
    cascades or not; FSR 3's and the SSAO/SSR outputs or not; skinned or not; per blend, depth compare, cull mode
    and stencil use; the fast unlit shader). Also the shadow maps and their pipelines, the radiance cascades' and
    FSR 3's pipelines and resources, SMAA's or FXAA's, the scene's targets, the post chain's pipelines, the
    distance pass and the refraction copy.
  - It runs the present's own steps, so the two can't drift apart. It draws nothing and leaves every frame's
    state alone (FSR 3's history and jitter, the retained shadow maps, the cascades' history): the frames are the
    same to the byte with or without it.
  - The batches are taken as the present takes them, without their instances. A second call with the same
    arguments makes nothing. Settings change the pipelines: call it once per quality preset the game uses, and
    again with the batches a level adds.
  - It refuses what the present refuses, and a call while a scene waits for its canvas, with
    `BTBH_GPU_INVALID_ARGUMENT`. A missing shader fails it as it would fail the present, with the path.
  - It covers the window's frames, at its size now. A resize or another render size makes the targets anew at the
    next present (the pipelines stay). `btbh_gpu_window_scene_render_to_target` keeps its own.
- `BTBH_GPU_HAS_SCENE_PREWARM`, defined with it.
- GPU smoke `btbh_gpu_scene_prewarm_smoke` (hidden windows, a debug device). A scene like v2's (FSR 3, the sun's
  shadow, SSAO, SSR, bloom, fog, vignette, a skinned unit, an x-ray stencil wall) draws its effects from frame 6:
  a decal, an additive unlit flash, a premultiplied soft particle, the x-ray ghost and a refraction strip.
  - Cold, its first frame makes 35 shaders and 40 pipelines, and frame 6 makes 22 shaders and 11 pipelines.
    Prewarmed, the window makes exactly those in the call, and none of its 16 frames makes any. Each frame matches
    the cold window's to the byte.
  - The same with the radiance cascades and an omni spot shadow (69 shaders, 65 pipelines).
  - A second prewarm makes nothing. A prewarm for FSR 1 makes what FSR 1 needs, and the FSR 1 frame makes nothing.
  - The refusals: each argument, a scene waiting for its canvas, a missing shader.
  - `BTBH_SMOKE_BENCH=cold` or `=prewarm` times one sequence at 1080p. `BTBH_PREWARM_SMOKE_DUMP` and `_REFERENCE`
    compare the cold frames of two builds.
- Internal: the renderer makes every shader and pipeline through `btbh_gpu_create_shader`,
  `btbh_gpu_create_graphics_pipeline` and `btbh_gpu_create_compute_pipeline`, which count them and the time they
  take (`btbh_gpu_creation_stats`).

### Changed
- Shaders: `production_model.frag` and its five variants take pattern 7. Pattern 3's street darkening moved into a
  function both patterns call. No binding, uniform or shader file changed. The unlit, radiance-cascade source and
  shadow shaders don't change.
- Header comments: `detail_parameters` documents every pattern (it named 0–3), and `surface_wetness` names the
  facade patterns too (it named 1 and 4, though the shader wets pattern 3's walls as well).
- The present's setup, model pipeline and target steps are three functions now, which the prewarm shares. A frame
  with a refracting batch makes the refraction copy with its other targets, before its passes. No pixel changes:
  the smoke's 16 FSR 3 frames and the world detail smoke's eight gallery scenes match 0.6.0's core to the byte.

### Measured
**Pattern 7.** Windows, RTX 3070 Ti, `BTBH_SMOKE_BENCH=weather`: the world detail's eight coplanar screen-filling
walls drawn with `LESS_EQUAL`, 10 interleaved turns of 20 frames, the fastest turn of each of two runs. Per screen of
facade:
- Pattern 7: under 0.01 ms dry, within the bench's noise at 1920x1080 and 0.001–0.002 ms at 960x600. Soaked (the
  wet walls take the face normal from derivatives): 0.012–0.025 ms at 1920x1080 and up to 0.007 ms at 960x600.
  RFC 0009 estimated under 0.01 ms.
- Pattern 3 (x 0.07, y 0, z 6): 0.17–0.20 ms at 1920x1080 and 0.046–0.060 ms at 960x600. Pattern 7 saves
  0.165–0.18 ms and 0.045–0.056 ms of it.
- Unset, against 0.6.0's shaders: nothing measurable (within ±0.011 ms at 1920x1080, either sign).
- A Steam Deck isn't measured.

**The prewarm.** Windows, RTX 3070 Ti (driver 621.14), Vulkan, the driver's shader cache warm. `BTBH_SMOKE_BENCH` at
1920x1080 rendered at 960x540 with FSR 3, six runs of each, alternating, in separate processes. CPU milliseconds of
the present call, median (range):

| | Without the prewarm | With it |
| --- | --- | --- |
| The prewarm call | – | 45.4 (43.4–46.7) |
| First present | 41.8 (37.3–55.5) | 5.4 (4.9–5.6) |
| Second present | 2.1 (1.8–2.3) | 2.2 (1.9–2.5) |
| First present with the effects (frame 6) | 10.9 (10.5–15.6) | 0.34 (0.24–0.40) |
| Later presents | 0.3 | 0.3 |

- The cold first present is what it makes. The creation calls for its 35 shaders and 40 pipelines take 14–20 ms.
  Reading the shader files and creating the targets, FSR 3's resources and the shadow maps take most of the rest.
  A quieter series measured 25–31 ms, 10–11 of them in the creation calls.
- The effects' first frame makes 22 shaders and 11 pipelines (5–8 ms in the calls) and reads their files.
- The first two prewarmed frames still cost 5 and 2 ms more than later ones (cold frames pay the same): the GPU
  API's first use of render passes, descriptor pools and uniform buffers, which only drawing makes.
- The first run after the shaders were built (a debug device) took 1.9 s for the first frame's creations and 3.0
  s with the cascades: the driver compiled them. Later runs take 10–20 ms: its shader cache.
- Nothing persists between runs but the driver's cache. SDL 3.4 makes its Vulkan pipelines without a pipeline
  cache and has no API for one.
- A Steam Deck isn't measured.

### Upgrading
- Nothing to set: zeroed batches keep pattern 0, and a host that never calls the prewarm presents as before.
- Rebuild the shaders with the C core. 0.6.0's shaders still work under this core, but draw a pattern-7 batch as
  no pattern, without its weather, and nothing detects that (pattern 7 adds no named resource to check for). This
  release's shaders under 0.6.0's core draw as before, and that core refuses pattern 7.
- No struct changed. Rust hosts with bindgen get the new function and constants when they regenerate the bindings.
  A build script can gate on `BTBH_GPU_HAS_SCENE_PREWARM` and `BTBH_GPU_SCENE_HAS_FACADE_WEATHER`.
- To adopt pattern 7 (v2): `detail: [0.0, 0.0, 0.0, 7.0]` where `kit_surface` replaces the grain, and for
  `SurfaceLook::flat()`'s facade. See RFC 0009, "Compatibility".
- To adopt the prewarm (v2): after loading the models and textures, call it once per quality preset with the
  arguments of the first present, and with batches for everything a stage will draw: each blend, depth compare,
  cull mode and stencil use, skinned or not, the effects and the refraction strips. No instance data is needed.
- Swift: `SurfaceDetail.facadeWeather` on a material. A `switch` over `SurfaceDetail.Pattern` without a `default`
  needs the new case. The prewarm has no Swift API yet.

### Known
- The prewarm can't remove what SDL makes while drawing. In v2's storm stage (about 2,100 draws) one frame between
  the 6th and the 10th still takes 25–28 ms, 24–25 of them in encoding the geometry at an unchanged draw count:
  most likely SDL creating a command buffer's descriptor pools on first use. Not fixed in this release.
- v2's first presented frame after frame 0 takes 1.3–1.5 s off-screen with or without the prewarm (the scene's
  encoding is 19 ms of it): the window's first canvas and swapchain present. Not investigated yet.

## [0.6.0] — 2026-11-20

An opt-in addition for Be The Bullet Hell v2: the world detail, phase 1 of
[RFC 0008](docs/rfc/0008-world-projected-detail.md). Nothing changes until a game sets it: a zeroed batch renders
byte for byte as in 0.5.1. The scene fragment shaders changed (three more textures and a bigger second uniform
buffer): rebuild the shaders together with the C core.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes (22/22), the Swift build and the full Swift suite (301
tests, 2 skipped, 0 failures). With nothing set, 0 pixels differ from 0.5.1's core and shaders: a gallery of eight
scenes through every scene shader variant (131,072 pixels) and `btbh_gpu_scene_target_smoke`'s nine images (an FSR
3 frame among them, 90,368 pixels), each written by a build of 0.5.1 and by this one. macOS (Metal) isn't
verified.

Be The Bullet Hell v2 (Rust, bindgen) tested it on its stage 3 by day, on a desktop RTX, with all 70 shaders
rebuilt and its macro sets bound per class (packed, the lot's index as the seed): no errors, nothing broken. GPU
medians of two passes, in ms:

| Preset | 0.5.1 | 0.6.0, unset | Detail | Detail and the facade grain |
| --- | --- | --- | --- | --- |
| Deck | 1.301 / 1.300 | 1.290 / 1.296 | 1.322 / 1.340 | 1.327 / 1.331 |
| Low | 1.606 / 1.601 | 1.612 / 1.619 | 1.691 / 1.706 | 1.702 / 1.694 |
| Medium | 2.690 / 2.665 | 2.663 / 2.658 | 2.651 / 2.651 | 2.779 / 2.765 |
| High | 3.036 / 3.030 | 3.036 / 3.034 | 2.932 / 2.953 | 3.210 / 3.208 |

Unset costs nothing measurable in its frame. The detail costs 0.03–0.05 ms on the Deck preset and 0.08–0.09 ms
on Low, and is cheaper than the triplanar facade grain it replaces on Medium and High. A Steam Deck isn't
measured yet.

### Added
- **`BTBHGPUSceneGeometryBatch.world_detail`** (appended at offset 264: the batch grows from 264 to 272 bytes on
  64-bit) pointing at a **`BTBHGPUSceneWorldDetail`** (64 bytes): a second texture set for a batch, projected from
  its flat world position instead of read through TEXCOORD_0. NULL draws as before. One detail can serve any
  number of batches; the renderer reads it during the present call only.
  - Up to three textures: albedo, normal, ORM. The albedo's rgb times `albedo_scale` multiplies the albedo. The
    normal's tilt (OpenGL +Y, xy read, z rebuilt) times `normal_scale` is added to the first set's. The ORM's r
    multiplies the occlusion, its g times `roughness_scale` the roughness. Metalness and emission don't change.
  - `BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE`: a grey albedo in the ORM's blue (linear). Two textures and
    two samples instead of three. v2's layout.
  - The projection uses the face's own normal, so a face never switches halfway across. Level faces (within
    `level_degrees` of flat, 18 by default) are projected straight down: u along +x, v to the north (-z), every
    `level_tile`. Other faces run along the face: u to the right seen from the front, v up the face (the height
    on walls, the distance up the slope on pitched faces), every `wall_tile`. The image is upright on walls and
    has its top to the north on roofs.
  - It uses the position before `world_curvature` bends it, so the pattern stays put as the camera moves. It is
    for static geometry: on anything that moves (skinned units, wind, billboards) it slides.
  - `seed_component` (1..4) names an instance custom float holding a whole number, such as a lot's index. It
    offsets the pattern, so neighbouring buildings don't share their streaks. Give all of a building's
    instances the same one. 0 shares one pattern for all.
  - Zeroed scales mean 1 and a zeroed angle 18 degrees. The tiles have no default.
  - Load the textures with `BTBH_GPU_TEXTURE_WRAP_REPEAT`.
  - A detail whose three textures are NULL, or one on a refracting batch, draws as without one.
- **Refusals**: the present fails with `BTBH_GPU_INVALID_ARGUMENT`, naming the batch, for a tile that isn't
  finite and above 0, a scale that isn't finite, `level_degrees` outside [0, 90), `seed_component` above 4, an
  unknown flag, `reserved` not 0, `ALBEDO_IN_ORM_BLUE` with an albedo texture or without an ORM texture, or a
  texture of another window.
- `BTBH_GPU_SCENE_HAS_WORLD_DETAIL`, defined with it.
- Native test `btbh_gpu_world_detail` (no GPU): the projection's C copy (`src/gpu/btbh_gpu_world_detail.h`) against
  the RFC's examples, the seed's hash, the uniforms and the refusals. `btbh_gpu_scene_order` checks the layout.
- GPU smoke `btbh_gpu_world_detail_smoke`:
  - unset: the gallery byte for byte the same with `world_detail` NULL, with a detail without textures, and with
    an earlier release's shaders (`BTBH_WORLD_DETAIL_BASELINE_SHADER_DIR`); `BTBH_WORLD_DETAIL_DUMP` and
    `BTBH_WORLD_DETAIL_REFERENCE` compare builds of two releases;
  - every pixel of roofs, the ground, an underside, walls, a 45 degree wall and a 30 degree pitch, read back
    against the C copy (all match);
  - two seeds giving two offsets, packed against separate (byte for byte), a neutral detail, the refusals and
    old shaders.

### Changed
- Shaders: `production_model.frag` and its five variants take three more textures after the dig mask (t11–t13,
  t10–t12 or t6–t8). Their second fragment uniform buffer grows from 32 to 80 bytes (`BatchExtension`, was
  `DigMask`). They include a new file, `production_world_detail.hlsli`. No shader file is new or renamed. The
  unlit, radiance-cascade source and shadow shaders don't change.
- A batch drawing a world detail never takes the fast unlit pipeline (its shader samples none).
- Shaders from before 0.6.0 would draw a detailed batch without its detail, without a word. So a present with
  one checks that the scene's fragment shader names `world_detail_normal_texture` (SPIR-V; Metal libraries
  aren't checked). Otherwise it fails with `BTBH_GPU_INVALID_ARGUMENT` and the shader's path. Without a detail,
  0.5.x shaders still draw as before under this core.

### Measured
Windows, RTX 3070 Ti, `BTBH_SMOKE_BENCH=1`: eight coplanar screen-filling layers drawn with `LESS_EQUAL`, 512²
maps with mips, 10 interleaved turns of 20 frames, the fastest turn of each of two runs. Per screen of detailed
surface:
- Packed (two samples): 0.042 ms at 1920x1080 and 0.005 ms at 960x600 under a heavy pixel (soft sun shadow, 40
  lamps, 32 point lights); 0.045 ms and 0.012 ms under a light one. Three maps: 0.027–0.054 ms and 0.008–0.012
  ms. RFC 0008 estimated 0.02–0.04 ms at 1080p.
- Unset, against 0.5.1's shaders: nothing measurable under the light pixel. Under the heavy one, 0.03–0.04 ms a
  1080p screen (about 2 %) and 0.005 ms at 960x600 (about 1 %): the price of the longer path's registers. The
  Deck's figure (and the register count on its GPU) is still to measure.
- The CPU pushes 48 more bytes per batch and binds three more textures (the same fallbacks for every batch
  without a detail).

### Upgrading
- Nothing to set: zeroed batches (`= {0}`, `..Default::default()`, Swift's `init()`) have `world_detail` NULL.
- Rebuild the shaders with the C core. 0.5.x shaders still draw under this core as long as no batch has a
  detail. 0.6.0's shaders don't work under an older core: its pipelines declare three samplers too few.
- Rust hosts with bindgen (Be The Bullet Hell v2, CHIMERA): regenerate the bindings for the bigger batch. A build
  script can gate on `BTBH_GPU_SCENE_HAS_WORLD_DETAIL`.
- To adopt it (v2): load each macro set with `WRAP_REPEAT` (packed: the normal, and the ORM with the albedo in its
  blue, linear), keep one `BTBHGPUSceneWorldDetail` per class with `ALBEDO_IN_ORM_BLUE`, the class's tiles and
  scales of 1.25, set `world_detail` on the class's batches, and write each building's lot index into the custom
  float `seed_component` names (v2: 4, float 23 of the instance record). See RFC 0008, "Compatibility".
- Swift: no API yet. The layer builds batches zeroed, so nothing changes.

## [0.5.1] — 2026-11-12

0.5.0 plus a fix for its destructible terrain, found by Be The Bullet Hell. No API changed and no shader changed:
rebuilding the C core is enough. A host without a terrain draws as in 0.5.0.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes (20/20), the Swift build and the full Swift suite (301
tests, 2 skipped, 0 failures). Be The Bullet Hell: 5709 tests, 0 failures. In its captures, a bare crater and 19
overlapping boss craters show clean lips, with no floor over the bowls and no holes. `update` costs what it did:
about 8.7 ms of work per 13 m crater, against 8 before. macOS (Metal) isn't verified.

### Fixed
- Destructible terrain: a host's floor no longer shows as a sawtooth along the top of a crater's bowl wall (found
  by Be The Bullet Hell). Where the patch's surface drops across a cell column, as it does where a bowl cuts its
  rim, the dig mask held the surface's height on the column's high side only. Floor between that height and the
  wall was kept and stuck out over the bowl, cut off at the column's edge. Each texel now holds the lowest point of
  the patch's top surface over its column, counting a wall that goes on into the chunk below.
  - The same columns are masked as before; only their heights are lower. A zeroed batch, and a dug batch away
    from any patch, draw as in 0.5.0. The shaders are unchanged: rebuilding the C core is enough.
  - `Tests/Fixtures/sdf_terrain/crater.png` changes in 544 pixels, all along the bowl's inner lip: the reference
    had the sawtooth (light ground teeth) in it, and the bowl's wall now shows there.
  - New tests: the GPU smoke renders Be The Bullet Hell's crater from its gameplay camera (no floor pixel over
    the bowl, no background), and the native test checks the mask over four walls, including one that crosses a
    chunk's floor.

## [0.5.0] — 2026-11-12

Opt-in additions for 0.4.0's games:
- destructible terrain from SDF edits, phases 1 to 3 of [RFC 0007](docs/rfc/0007-destructible-terrain.md) (asked
  for by Be The Bullet Hell and CHIMERA on #3), and a Swift layer for it (asked for by Be The Bullet Hell);
- three font features, asked for by CHIMERA after it tested 0.4.0's small-text loader;
- two scene features for CHIMERA: batches that cast no sun shadow, and a front-to-back opaque pass.

Nothing changes until a game uses them: a zeroed `BTBHGPUFontDesc.flags` bakes what 0.4.0 bakes, byte for byte,
and the canvas draws what it drew; a zeroed batch, a zeroed `BTBHGPUScenePost` and a host with no terrain render
as in 0.4.0 (in Swift: a platform with no terrain set, whatever its materials say). One shader is new
(`production_canvas_coverage.frag`). The scene, unlit, shadow and radiance-cascade material-source shaders changed
for the terrain (a dig-mask branch that a zeroed batch never takes): rebuild the shaders together with the C core.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes (20/20), the Swift build and the full Swift suite (301
tests, 2 skipped, 0 failures).
- Be The Bullet Hell, with nothing set (`83e05749`, before CHIMERA's notes and the Swift terrain): 5701 tests, 0
  failures, and no frame time change it could measure. With craters, through the Swift layer (`a9b39f28`): 5709
  tests, 0 failures. It used one terrain per city: 0.5 m cells, 10,400 chunks of 32 cells, and a heightfield base
  at its floor tiles' tops. Crater and collapse edits dug through its floor tiles. With craters on against off,
  interleaved, it ran at 77.7 / 77.2 fps (boss), 104.6 / 104.3 (stress) and 103.3 / 101.8 (mid), which is within
  noise. After nine 13 m craters it held 5.7 MB of meshes and 20.7 MB of mask.
- CHIMERA, on `751ddb01` (this release's C core): 996 workspace tests, both against it and against 0.3.0. With
  nothing set, 0 pixels differ from 0.4.0. It turned on the kerning pairs, R8 atlases, `SNAP_X_HEIGHT`,
  `NO_SUN_SHADOW`, and its ground on `KEEP_ORDER` drawn after the scenery. It has no terrain, so the dig mask and
  the refusal of old shaders aren't exercised there.
- macOS (Metal) isn't verified.

### Added
- **`BTBHGPUSdfTerrain`** (`include/btbh_gpu/btbh_gpu_sdf_terrain.h`, included at the end of `btbh_gpu.h`;
  `BTBH_GPU_HAS_SDF_TERRAIN`): the ground before any damage (a heightfield, or the host's batch function) and an
  ordered list of edits: sphere, ellipsoid, rounded box, capsule, rounded cylinder and torus shapes that ADD,
  SUBTRACT, INTERSECT (within the edit's bounds) or PAINT, with a smooth-min radius and a material.
  - `btbh_gpu_sdf_terrain_create` needs no window; `add_edit`, `set_edit` (non-destructive) and `remove_edit`;
    `update` with a budget in microseconds and an optional `run_jobs` callback (the same results either way).
  - Queries from the edit list, exact whatever the budget left pending: `distance`, `height`, `raycast`,
    `sample_height_deltas`, and `take_changes` for the rectangles whose look changed.
  - Sparse 8³ bricks of int8 distances only where edits reach; chunk meshes from surface nets with smooth or
    faceted normals, vertex colours from `material_colors` and watertight seams. `chunk_count`, `chunk` (a custom
    mesh to draw as an ordinary batch) and `chunk_geometry` (its triangles on the CPU, also without a window).
    Triangles are clockwise seen from outside: front faces for `BTBH_GPU_SCENE_CULL_BACK` under a camera built as
    the Swift layer builds it.
  - The edit cap (`max_edits`, 4096 by default): `edit_cap` freeze folds the oldest edits into the base, reject
    makes `add_edit` return 0.
  - `btbh_gpu_window_sdf_terrain_attach` / `btbh_gpu_sdf_terrain_detach`: the GPU side (the chunk meshes and the
    dig mask) on a window. Destroying the window detaches its terrains.
- **`BTBHGPUSceneGeometryBatch.dug_by`** (appended): ground that a terrain may dig through. Where the terrain's
  patch has taken over (a crater, a mound's foot, paint), the batch's fragments that don't lie more than a tenth
  of a cell under the patch's top surface are discarded at their flat world position (so `world_curvature`
  keeps the hole under its patch), in the colour, shadow and radiance-cascade passes. The mask is one R16F
  texel per cell column over the terrain's bounds, made on the first patch; until then the batch draws exactly
  as with NULL. A dug batch takes the shadow pipeline with a fragment stage while the mask exists. Shaders from
  before 0.5.0 would draw a dug batch undug without a word, so a present with a dug batch checks that the scene's
  fragment shader samples the mask (SPIR-V names it; Metal libraries aren't checked) and otherwise fails with
  `BTBH_GPU_INVALID_ARGUMENT` and the shader's path. Without `dug_by`, older shaders still draw as before.
- **Swift: `GPUSdfTerrain`** (BTBHRendererGPU, asked for by Be The Bullet Hell): the terrain without the C API.
  - `GPUSdfTerrain(GPUSdfTerrainDescriptor)`: bounds, cell size, chunk cells, `maxEdits` and `editCap`, smooth or
    faceted `normals`, a `.flat(height:)` or `.heightfield(heights:width:depth:cellSize:)` base with its surface
    and subsurface materials and depth, up to eight `materialColors` (sRGB like every Swift colour; linearised
    for the C core), and `concurrentJobs` (`run_jobs` over `DispatchQueue.concurrentPerform`). A flat base is a
    one-cell heightfield. Destroyed on deinit. It needs no window, so its edits and queries work headless.
  - `addEdit(_:) -> GPUSdfEditID?` (nil for an invalid edit, or at the cap with `.reject`), `setEdit(_:to:)` and
    `removeEdit(_:)` (false for an id that was removed or frozen), `update(budgetMicroseconds:)` returning a
    `GPUSdfTerrainReport` (with `isSettled`), `height(x:z:fromY:) -> Float?` (nil where the C call gives
    -INFINITY), `raycast(origin:direction:maxDistance:) -> RenderVec3?`, `distance(at:)`, and `takeChanges()` as
    `RenderRect`s (x and width along world x, y and height along world z). `GPUSdfEdit` takes the six shapes and
    four operations with a `RenderQuaternion` rotation.
  - `GPUPlatform.setSdfTerrain(_:)` attaches one to the window and detaches the one before; nil detaches it, and
    closing the window does too (attach it again after reopening). `GPUPlatform.sdfTerrain` and
    `GPUSdfTerrain.isAttached` say which. While one is attached, every scene frame draws each chunk that has a mesh
    as an ordinary custom-mesh batch on one identity instance, with `GPUSdfTerrain.material`
    (`MaterialDescriptor.default` unless set: white albedo times the vertex colour, back faces culled, casting and
    taking shadows, bent by the world curvature like everything else). Chunks are culled like persistent ranges:
    drawn in view, and into the sun's shadow map only when their shadow can reach the view. A frame with no other
    draw still draws nothing.
  - FSR 3: the reactive mask is written only by the transparent pass, and `reactive_mode` only chooses how a
    transparent batch writes it, so an opaque chunk can't mark it. For `reactiveFrameCount` scene frames (4) after
    a chunk's mesh changes, the chunk is drawn once more in the transparent pass, unlit, black, additive and with
    `lessEqual` depth: it adds exactly nothing to the colour, and its alpha (1) times `BTBH_FSR3_REACTIVE_SCALE`
    goes into the mask. Only while FSR 3 runs; 0 turns it off.
- **Swift: `MaterialDescriptor.dugByTerrain`** and **`ModelMaterialPatch.dugByTerrain`** (BTBHRenderer, next to
  `castsShadow`): the material's batches get `dug_by` set to the platform's attached terrain, however they are
  submitted (models, instances, persistent range lists, transform streams, meshes). nil in a patch keeps the
  imported material's (false). With no terrain attached, or one without a patch yet, they draw as before. Be The
  Bullet Hell's floor tiles take `castsShadow: false, dugByTerrain: true` in their material patch.
- Swift GPU tests `GPUSdfTerrainTests`: the handle without a window (heights, rays and distances before any
  update and after the edits are removed, changes reported once the meshes are ready, `setEdit` keeping the id,
  refused descriptors and edits, a heightfield base, both caps, and the same bricks, mesh and mask bytes with
  `concurrentJobs` and a budget); chunks drawn through the platform (a mound changes the picture; a dug floor
  with no terrain, a terrain not attached, one attached without edits, one detached and one whose edits were
  removed all give the plain picture byte for byte; attached again, the mound frame byte for byte; closing the
  window detaches); a `dugByTerrain` floor over a crater, drawn as instances, a persistent range list and transform
  streams: the scorched floor shows and no background shows through, while without the flag the floor covers it
  and far from it nothing changes; and the second draw of fresh chunks under FSR 3 (counted in batches) for
  exactly `reactiveFrameCount` frames, never without FSR 3, never with 0, and (forced into a frame without FSR 3,
  where pixels compare) leaving the picture byte for byte as it was. `BTBH_SDF_TERRAIN_PNG_DIR=<dir>` writes the
  frames.
- **All kerning pairs in one call** (CHIMERA made about 16k `btbh_gpu_font_kerning` calls per font):
  `btbh_gpu_font_kerning_pairs(font, pairs, capacity)` copies the font's nonzero pairs, sorted by left then
  right, as `BTBHGPUFontKerningPair { left, right, advance }`, with the advances `btbh_gpu_font_kerning` returns,
  and returns how many the font has whatever the capacity (`NULL, 0` sizes the array). 0 for a font from
  `btbh_gpu_window_font_load`. `BTBH_GPU_HAS_FONT_KERNING_PAIRS` is defined with it.
- **`BTBHGPUFontDesc.flags`**, which had to be 0 in 0.4.0, now holds `BTBH_GPU_FONT_FLAG_*`. Unknown bits are
  still refused. `BTBH_GPU_HAS_FONT_FLAGS` is defined with it.
- **One-channel font atlas**: `BTBH_GPU_FONT_FLAG_R8_ATLAS` bakes the atlas as R8 coverage instead of RGBA8
  (white, coverage in alpha): a quarter of the memory at the same size (at 12 px with `oversample_x` 3, 128 KiB
  becomes 32 KiB). Glyphs, metrics and kerning are the same. The canvas draws a batch whose texture has one
  channel as white x coverage x the vertex colour, with a new shader, `production_canvas_coverage.frag`, which
  it loads from the directory of the textured fragment shader the first time a batch needs it. In the font
  smoke test the R8 rows match the RGBA8 rows pixel for pixel. Elsewhere (a scene material, a readback) the
  atlas is an R8 texture, and `btbh_gpu_texture_update_rgba8` refuses it.
- **Snapped x-height**: `BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT` rasterises with a vertical scale of
  scale x round(x) / x, x being the x-height in pixels, so on a whole-pixel baseline the top of flat lowercase
  ends on a pixel edge instead of leaving a grey row (a cheap stand-in for vertical hinting at 720p). The
  horizontal scale, advances and kerning don't change. `btbh_gpu_font_metrics` reports the scale used:
  `x_height` is a whole number, and ascent, descent, line gap and cap height are scaled to match (`pixel_size`
  stays as asked). In Titillium Web at 16 px the top row of 'z' and 'u' goes from 26% ink to 100%. It only puts
  the x-height's top edge on the grid: a horizontal stroke elsewhere or of a fractional thickness (the bar of
  'e', the cross of 't') still straddles a pixel edge, round tops still overshoot a little, capitals and
  ascenders aren't snapped, and rounding changes the size (10 px Titillium gets about 9% shorter).
  `BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT_DOWN` (asked for by CHIMERA) rounds down instead, so a face never grows
  taller than asked. In Titillium Web, ascent - descent at 16 px is 15.2 px with either flag; at 12 px it is
  9.1 px rounded down and 12.2 px rounded to the nearest. Under one pixel of x-height it bakes as without the
  flag; the two snap flags together are refused.
- Native test `btbh_gpu_font_smoke`: the pairs against `btbh_gpu_font_kerning`; the R8 atlas against the RGBA8
  one (its alpha texel for texel, a quarter of the bytes, the same drawn text); a present that needs the
  coverage shader and can't find it fails with the path; the snapped metrics; and the top row of
  "xxx zzz uuu vvv nnn ooo eee" with and without the snap. New PNGs: `text-<size>px-ex-r8.png`,
  `text-<size>px-ex-snap.png`, `probe-<size>px-ex-vs-snap.png` and `text-extras.png` / `text-extras-x4.png`.
- **Batches that cast no sun shadow**: `BTBHGPUSceneGeometryBatch.flags` (a byte that was `ssao_reserved[0]`)
  with `BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW`. Such a batch is drawn and shadowed as usual but stays out of both directional shadow maps (the primary sun's and the secondary's), for
  decals, ground overlays and small props whose shadow isn't worth the shadow pass. Every scene call takes it.
  The `_with_visibility` calls' `directional_shadow_visibility` bytes already did this per batch, through those
  calls only; a batch casts when both let it. The spot (or omni) light's shadow stays with
  `spot_shadow_visible`, as before: 0 already casts none there, so a batch that should cast no shadow at all sets
  the flag and leaves `spot_shadow_visible` 0. A shadow map kept over several frames shows a change of the flag
  at its next update. An unknown bit fails the present with `BTBH_GPU_INVALID_ARGUMENT`.
- **Front-to-back opaque pass**: `BTBHGPUScenePost.opaque_sort` (a byte that was `ssao_reserved[0]`, so the
  struct keeps its size) with `BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK`. The opaque pass then draws its batches
  nearest first, so the depth test throws hidden pixels away before they are shaded. The key is the view depth
  (clip w; clip z for an orthographic view) of the batch's centre: the mean, over its instances, of the instance
  transform applied to the centre of the model's or custom mesh's bounds. The sort is stable. It moves only a
  batch whose picture can't depend on what was drawn before it: opaque (not transparent, the alpha blend mode,
  not refracting), visible, `depth_compare` LESS, the stencil off, transient instances, bounds known. Every other
  opaque batch keeps its place and nothing is moved across it: depth compares other than LESS (`LESS_EQUAL`
  overlays, `ALWAYS`), stencil writers and testers, persistent instance buffers (their positions live on the
  GPU), custom meshes with a position that isn't finite, and batches with `BTBH_GPU_SCENE_BATCH_KEEP_ORDER`, a
  second new flag for an order the renderer can't see (a coplanar overlay drawn with LESS, the ground left
  deliberately after the units). Transparent and refracting batches, the shadow maps and the radiance cascades'
  sources draw in the order given, as before. The picture stays the same except where two opaque surfaces have
  exactly the same depth (with LESS the first drawn shows). An unknown value fails the present. One key stands
  for a whole batch, so an instanced batch spread over the view (trees across a map, tiled ground) sorts by a
  mean that lands mid-scene; such batches gain little, and the header says so.
- **`BTBH_GPU_SCENE_OPAQUE_SORT_FRONT_TO_BACK_LESS_EQUAL`**: the same sort, also moving batches drawn with
  `LESS_EQUAL`, for hosts that draw everything with it (as Tyrian3D's batch setup does, and CHIMERA after it);
  under plain `FRONT_TO_BACK` nothing of theirs moves. With `LESS_EQUAL` the last drawn of two surfaces at the
  same depth shows, so a coplanar overlay (a decal, a road on the ground, a selection ring) needs `KEEP_ORDER`
  after what it covers.
- Custom meshes measure their vertices' bounds on create and update, for that key.
- `BTBH_GPU_SCENE_HAS_BATCH_FLAGS` and `BTBH_GPU_SCENE_HAS_OPAQUE_SORT`, defined when these exist, for hosts that
  build against several releases.
- Native test `btbh_gpu_scene_order` (no GPU): which batches sort and which keep their place, the depth they sort
  by (bounds, instance mean, perspective and orthographic, behind the camera), stability, and the two structs'
  sizes and offsets. In `btbh_gpu_scene_target_smoke` (GPU), read back from a render target: a cube's shadow on a
  slab gone with the cube's flag (those texels then match a render without shadows) while the cube is still drawn,
  and kept when the slab has the flag; the sort flipping a depth tie between two copies of a slab (so it does
  reorder), not flipping it when one of them is pinned, writes the stencil, or draws with `LESS_EQUAL` or
  `ALWAYS`, and flipping a `LESS_EQUAL` tie under `FRONT_TO_BACK_LESS_EQUAL` unless one is pinned; a layered
  scene with see-through cubes byte for byte the same sorted; and the refusals. `btbh_gpu_sdf_terrain_smoke`:
  a dug batch with a fragment shader that doesn't name the mask fails the present with its path, and a plain
  batch still draws. `btbh_gpu_font_smoke`: the rounded-down metrics at seven sizes.
  `BTBH_SMOKE_BENCH=1` prints the timings below.

### Changed
- `btbh_gpu_custom_mesh_update` keeps its buffers when the new data fits and otherwise makes them a quarter
  bigger than needed (grow-only), instead of reallocating for every size change. Nothing renders differently.
- Shaders: `production_model*.frag`, `production_model_unlit.frag` and `production_rc_material_source.frag` take
  one more texture (after their others) and a second fragment uniform buffer; `production_shadow.frag` takes a
  texture and a uniform buffer; `production_shadow.vert` and `production_animated_shadow.vert` pass the world
  position on. Shader file names are unchanged.

### Fixed
- Under FSR 3, the relief from brightness (an unlit material with a negative `normal_scale`) sampled its four
  albedo taps without the negative mip bias the albedo itself uses, so it read a blurrier mip and looked flatter
  than at native resolution. The taps now take the same bias. Without FSR 3 the bias is 0 and nothing changes.
  Found by `testFSR3PicksTextureDetailForTheOutputSize`; it dates from the tyrian3d merge (`1021d8b1`).
- Swift GPU tests: the 10 cases that failed on 0.3.0 and 0.4.0 (stale source checks, and three that ignored
  `BTBH_GPU_SHADER_DIR`) check today's renderer; the suite passes.

### Measured
Windows, RTX 3070 Ti, 1920x1080 renders to a target with a heavy pixel (soft sun shadow, 40 lamps, 32 point
lights), `BTBH_SMOKE_BENCH=1`. SDL 3.4 has no GPU timestamps, so a sample is the wall time of 20 frames rendered
back to back and one wait for the GPU, over 20; the configurations took 8 turns each. The machine was shared
(another game held the GPU at 100 %), so turns varied up to fivefold; the fastest turn of each of two runs is
quoted.
- 48 screen-filling slabs given far to near (the worst case): 125 / 133 ms a frame as given, 2.3 / 2.2 ms sorted;
  the nearest slab alone takes 1.1 / 2.0 ms.
- A ground slab and 1024 small units in 64 batches, seen from 45 degrees up, the ground given first: 6.3 / 7.3 ms;
  sorted 5.9 / 5.8 ms (the ground's centre lands among the units, so it moves only part-way back); the ground
  given last, sorted or not (`KEEP_ORDER`), 5.2 / 5.2 ms. The ground alone takes 2.4 / 3.1 ms, the units alone
  4.1 / 4.2 ms.
- In CHIMERA (the main pass's 10th percentile, 770 frames a side, A/B in one run), with `KEEP_ORDER` on the
  ground and everything lying on it: `FRONT_TO_BACK_LESS_EQUAL` alone gained 0.002 ms with 132 units and
  0.019 ms in its town. Scenery drawn before the pinned ground, which `KEEP_ORDER` allows, gained 0.049 and
  0.072 ms; both together, 0.044 and 0.092 ms. A game that already draws its ground last gains little from the
  sort itself.
- CPU (Ryzen 9 9950X, as contended), `btbh_gpu_scene_opaque_order` alone, 200 calls averaged: 17 µs for 256
  batches of 16 instances, 57-200 µs for 2048 batches of one, 0.23-0.31 ms for 1024 batches of 64 (one pass over
  the instances, then the sort). A custom mesh update of 100 000 vertices spends 0.35-0.38 ms on its bounds. The
  default order costs nothing new.
- Swift, same CPU: while a terrain is attached, the scene backend reads every chunk once a frame
  (`btbh_gpu_sdf_terrain_chunk`, for the meshes and their revisions): 14-24 µs a frame for 2116 chunks (Be The
  Bullet Hell's 1440 m map at 0.5 m cells and 64-cell chunks; `testChunkWalkAtBTBHScale`, three runs). The cost
  follows the chunk count: 32-cell chunks would make 16 562 of them on that map (two layers instead of one).

### Not added
- A depth pre-pass. It would draw every opaque batch twice (double the vertex work and draw calls, skinned units
  most of all), need depth-only pipelines for every vertex path (skinning, billboards, world curvature, the
  dissolve and shatter surfaces) and alpha-tested ones, and an `EQUAL` main pass that only holds if both passes
  produce bit-identical depth. Stencil users and depth-compare overrides would have to stay out of it. With the
  sort, early depth already drops most hidden pixels; a pre-pass is worth measuring again once the GPU can be
  timed per pass.

### Known
- `btbh_gpu_sdf_terrain_update` can overrun its budget, because a chunk's mesh build and upload don't split.
  Be The Bullet Hell measured up to 3.8 ms in one frame with 32-cell chunks (5.6 ms with 64-cell ones), and
  about 8 ms of work in all per 13 m crater. Meshing only the bricks near active ones (RFC 0007's next step), or
  splitting the upload, would remove the spikes.

### Upgrading
- Nothing to set: zeroed batches (`= {0}`, `..Default::default()`, Swift's `init()`) have `dug_by` NULL.
  Rebuild the shaders with the C core.
- To adopt the terrain, see RFC 0007 ("Compatibility") and its implementation notes: a base, `dug_by` on the
  host's ground batches, one patch batch per chunk with a mesh, `update` once a frame.
- Swift hosts: create a `GPUSdfTerrain`, call `update` once a frame and `GPUPlatform.setSdfTerrain(terrain)`; the
  backend draws the chunks. Mark the ground with `dugByTerrain: true` in its `ModelMaterialPatch` (or
  `MaterialDescriptor`), next to `castsShadow: false`. Before destroying the window or the terrain nothing needs
  detaching; `setSdfTerrain(nil)` at the end of a level lets the terrain go. Existing calls compile unchanged:
  the new initialiser parameters have defaults.
- Nothing changes unless a game sets `BTBHGPUFontDesc.flags` or calls `btbh_gpu_font_kerning_pairs`.
- A game that bakes R8 atlases must ship `production_canvas_coverage.frag` (`.spv` or `.metallib`) next to
  `production_canvas_textured.frag`. It is in the renderer's shader list in `CMakeLists.txt`, so build scripts
  that read that list pick it up.
- Nothing to set: a zeroed batch and post (`= {0}`, `..Default::default()`) draw as in 0.4.0. A host that names
  every field sees `ssao_reserved` shrink by one byte in both structs.
- To keep a hand-made "units before the ground" order and still sort the units, give the ground `KEEP_ORDER`
  and leave it last.
- `FRONT_TO_BACK` moves only `LESS` batches. A host whose batches start from Tyrian3D's setup draws with
  `LESS_EQUAL`, so turning it on alone moves nothing: use `FRONT_TO_BACK_LESS_EQUAL`, with `KEEP_ORDER` on
  coplanar overlays.
- A dug batch needs the 0.5.0 scene shaders; with older ones the present fails and names the shader.
- SwiftPM hosts: 0.5.0 adds a C file to the package (`btbh_gpu_sdf_terrain_bridge.c`). After checking the
  renderer out from 0.5 back to an older release, SwiftPM may keep its old build plan and fail with
  "couldn't build ... btbh_gpu_sdf_terrain_bridge.c.o because of missing inputs". Deleting `.build/*.yaml` and
  `.build/release/description.json` fixes it (found by Be The Bullet Hell).
- Swift: no wrapper for the batch flags or the sort. Its draws already keep out of the sun's shadow maps
  (`castsSunShadow`, a material's `castsShadow`) through the `_with_visibility` bytes.

## [0.4.0] — 2026-11-12

0.3.0 plus three opt-in features: models that share external textures and V-Sync through `GPUPlatform` (asked for
by Be The Bullet Hell), and readable small text (RFC 0004, option A, asked for by CHIMERA). Nothing changes until
a game uses them. No shader changed.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes (17/17), the Swift build and the Swift GPU tests.
- Be The Bullet Hell: 5699 tests, 0 failures, plus its debug and release builds and a real start. It shared the
  textures of every army body and boss, and its night army capture matched the old build, emission glow included.
  It toggled V-Sync between frames from its Options row, and each switch took the mode asked for.
- CHIMERA: 951 workspace tests against 0.3.0 and the candidate, then 206 against the final one, with no failures.
  On the HUD at 720, 900 and 1440, spacing became even, sharpness was unchanged, small rows at 720 read better,
  frame time was the same, and the font atlases got smaller (31.9 vs 48.8 MB at 1440).
- macOS (Metal) isn't verified, as for 0.2.0.

### Added
- **Models share external textures** (asked for by Be The Bullet Hell, whose 37 army models each uploaded
  their own copy of the same `textures/mc_*.png`): `BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES` for
  `btbh_gpu_window_model_load_ex`. Models loaded with it share one texture per external image (a glTF image
  with a `uri`) and format, through a window-level cache keyed by the image's normalised resolved path: an sRGB
  albedo or emission texture never shares with a linear normal or ORM one. A composed ORM texture is shared by
  its pair of external images, so it is composed once. Embedded (buffer-view) images stay each model's own.
  The path key is lexical (`\` and `/` alike, `.` and `name/..` folded); spellings that differ otherwise
  (letter case, links) load twice, as without the flag. Without the flag nothing changes.
- Ownership, now in the header: a model's textures (`BTBHGPUModelMaterialInfo`) belong to the model. A shared
  one is counted per material slot that holds it; the last model destroyed frees it, the window's destroy frees
  what is left, and `btbh_gpu_texture_destroy` on it does nothing (so a host can't free it twice). Any other
  model texture is the model's alone, as before.
- `BTBH_GPU_MODEL_HAS_SHARED_EXTERNAL_TEXTURES`, defined when the flag exists, for hosts that build against
  several releases.
- Swift: `ModelImportFlags.shareExternalTextures`, and `ModelMaterialPatch.emissionTexture`, which replaces
  an imported material's emission texture as `albedoTexture`, `normalTexture` and `ormTexture` replace theirs.
  It wins over `clearEmissionTexture`. Load it with `colorSpace: .sRGB` to match the importer. Existing calls
  compile unchanged.
- **Readable small text** (RFC 0004, option A): `btbh_gpu_window_font_load_ex`
  bakes a font from a `BTBHGPUFontDesc` with stb_truetype's packer, oversampled (`oversample_x`, `oversample_y`,
  up to 8) and prefiltered. `BTBHGPUFontGlyph` then has fractional offsets and advances and UVs that undo the
  oversampling, so a glyph drawn on the 1/`oversample_x` pixel grid stays sharp and spacing stays even. Its atlas
  is always linear. `snap_advances` rounds advances and kerning to whole pixels. `coverage_gamma` (0.25 to 4,
  0 = 1) is applied once to the atlas while baking: 1.2-1.4 thickens light-on-dark text, and the canvas shader
  doesn't change. An invalid descriptor returns NULL with `BTBH_GPU_INVALID_ARGUMENT` and the reason in the
  report.
- `btbh_gpu_font_metrics`: ascent, descent, line gap and size in pixels (for a plain font, what its report says),
  and x-height and cap height (the tops of 'x' and 'H', unrounded), so a host can pick a size that puts the
  x-height on a whole pixel.
- `btbh_gpu_font_kerning(font, left, right)`: the font's kerning in pixels (GPOS pairs or its kern table),
  precomputed between the baked glyphs. 0 for fonts from `btbh_gpu_window_font_load`.
- `BTBH_GPU_HAS_FONT_LOAD_EX`, defined when these exist, for hosts that build against several releases.
- Native test `btbh_gpu_font_smoke` (GPU): `_ex`'s validation, its metrics against the plain loader (within a
  pixel), kerning, the coverage curve, and a line of text at 10, 12 and 16 px with and without `_ex`, read back
  into PNGs (`font-smoke/` in the build directory). Its face is Titillium Web SemiBold (OFL), added under
  `Tests/Fixtures` with its licence. `check.ps1 -SkipGpuTests` and `SKIP_GPU_TESTS=1 check.sh` skip it.
- **V-Sync through `GPUPlatform`** (asked for by Be The Bullet Hell, which opens its window through it):
  `GPUPlatform.setPresentMode(_:)` switches the present mode between frames without reopening the window
  (`.vsync` for on, `.automatic` for off), and `GPUPlatform.presentMode` says what the window took. While a
  chosen mode resolves to V-Sync, a `.displayRefresh` frame-rate policy stops throttling and leaves the pacing
  to the swapchain, so a whole-hertz throttle can't miss a vblank on a panel that is a little faster; `.fixed`
  caps still apply. Until a game calls it, pacing is as before.

### Upgrading
- Nothing changes unless a model is loaded with `BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES` or a patch sets
  `emissionTexture`.
- Nothing changes unless a game calls `btbh_gpu_window_font_load_ex`: `btbh_gpu_window_font_load` is byte for
  byte the same. No shader changed.
- Swift: no font wrapper yet (Be The Bullet Hell doesn't need one).

## [0.3.0] — 2026-11-11

0.2.1 plus a choosable present mode (V-Sync), asked for by CHIMERA's Graphics tab. New API, opt-in: a zeroed
`BTBHGPUWindowDescriptor` takes the same present mode as before, and nothing else changed. No shader changed.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes, the Swift build, and CHIMERA's build and on-screen
test at 60 Hz. In a running mission, V-Sync toggled on and off without recreating the window, and the report
followed. On capped the frame rate at the refresh. Off left 7 scenes with 0 differing pixels against 0.2.1, at
the same frame rate within noise. macOS (Metal) isn't verified, as for 0.2.0.

### Added
- **Choosable present mode**: `BTBHGPUWindowDescriptor.present_mode` (a `BTBHGPUPresentMode` in a byte that
  was `reserved`, so the struct keeps its size). `BTBH_GPU_PRESENT_MODE_INVALID` (0) takes the first of
  IMMEDIATE, MAILBOX and VSYNC that the window supports, as before. `BTBH_GPU_PRESENT_MODE_VSYNC`,
  `_IMMEDIATE` or `_MAILBOX` asks for that mode, and takes VSYNC, which every window supports, where the window
  lacks it. Any other value fails `btbh_gpu_window_create`. `BTBHGPUWindowReport.present_mode` says what was
  taken.
- `btbh_gpu_window_set_present_mode(window, mode, report)`: switches the mode at run time, without recreating
  the window, by the same rules (0 is the default choice again). The swapchain keeps its SDR composition.
  Call it between frames: with a frame pending (a scene presented, its canvas not yet) it changes nothing and
  returns `BTBH_GPU_INVALID_ARGUMENT`. Switching rebuilds the swapchain and may wait for the GPU to finish
  the frames in flight; a request that resolves to the mode in use does nothing. Before the claim it sets the
  mode the claim asks for.
- `BTBH_GPU_WINDOW_HAS_PRESENT_MODE`, defined when both exist, for hosts that build against several releases.
- Swift: `GPUPresentMode` (`.automatic`, `.vsync`, `.immediate`, `.mailbox`), `GPUWindowProbe`'s
  `presentMode:` parameter (defaults to `.automatic`) and `GPUWindowProbe.setPresentMode(_:)`. Existing calls
  compile unchanged. `GPUPlatform` and `WindowDescriptor` don't take a present mode yet.

### Upgrading
- Nothing to set: a zeroed descriptor, `= {0}` or `..Default::default()`, presents as in 0.2.1.
- For a V-Sync option, V-Sync on is `BTBH_GPU_PRESENT_MODE_VSYNC`, and V-Sync off is 0 (the fastest mode the
  window has). Asking for IMMEDIATE instead gives VSYNC on a window that has only MAILBOX besides it.

## [0.2.1] — 2026-11-11

0.2.0 plus a skinning fix found by CHIMERA. No API changed, but the skinned vertex shaders and the model
loader did: rebuild the shaders together with the C core.

Tested on Windows (Vulkan): `check.ps1` with the GPU smokes, the Swift build, and CHIMERA's build and on-screen
A/B (its original infantry no longer shard at 175 units). macOS (Metal) isn't verified, as for 0.2.0.

### Fixed
- **Skinned vertices away from the world origin** were thrown towards or away from it. A vertex blended from
  several bones moved by (Σw − 1) times its instance's translation, along the line through the world origin.
  At 175 units from the origin that is about 1 unit, enough for long shards on CHIMERA's infantry. Present
  since 0.1.0. Two causes, both fixed:
  - The skinned vertex shaders took the skinned point's w from the blended bone matrix, which makes it the
    weights' sum rather than 1, and the instance matrix then scaled its translation by that sum. They now
    take the point with w = 1. This covers `production_animated_model.vert` (the scene draw, its previous
    pose for motion vectors, and the radiance-cascade source variant
    `production_rc_animated_material_source.vert`) and `production_animated_shadow.vert`.
  - Loading quantised each bone weight to a 255th on its own, so a multi-bone vertex's weights added up to
    254–257 255ths. They now add up to exactly 255: each share is rounded down, and the 255ths left over go to
    the largest remainders. Weights that already added up to 255 (every single-bone vertex among them) pack
    exactly as before.

### Known
- A rigid mesh on a node inside a skin's skeleton (a soldier's gun under its hand) is drawn where the file
  puts it at rest; it doesn't follow its node through the pose. Following it is a feature for a later release.
  Until then, bind such meshes into the skin offline (CHIMERA does so with `asset-tool bind-skin`).

### Upgrading
- All three games: bump to `v0.2.1` and rebuild the shaders. There is nothing to set.
- Skinned models with vertices blended from several bones change where they were wrong. Where such a vertex's
  weights didn't add up to 255, one or two of them change by a 255th, and the vertex comes back to where its
  bones put it: by up to 2/255 of its instance's distance from the world origin.
- Every other skinned vertex moves only by float rounding: its instance's translation is added in a different
  order. A/B captures show scattered single edge pixels on skinned meshes, more of them further from the
  origin, and nothing visible side by side. CHIMERA, whose skins all add up to 255 already, measured this at
  1920 × 1080: 28 pixels differ near the origin and 318 at 175 units out (8 mechs), with no shape change.
  Static meshes are unchanged.

## [0.2.0] — 2026-11-10

Tyrian3D's last four `tyrian3d` commits, ported to master as opt-in features: with every new field zeroed
a scene looks as it did in 0.1.0. After this release the `tyrian3d` branch is retired and all three games
build from tags on `master`.

It also contains 0.1.1 (a scene into a render target, per-batch reactive mask, `upscale_mode` in the
report; RFC 0006), merged from `master`.

Tested on Windows (Vulkan) with all three games:
- CHIMERA is byte-identical to 0.1.0.
- Be The Bullet Hell builds unchanged from `e95b392` and keeps its look.
- Tyrian3D matches `tyrian3d` @ `89266d2` with the settings below.

macOS (Metal), including the new HBAO metallibs, isn't verified yet. Its check is pending in issue #1 and
didn't hold up this release.

### Added
- **Soft sun shadows** (from `f7c3773`): `BTBHGPUSceneLighting.directional_shadow_filter`
  (`BTBHGPUSceneShadowFilter`). `BTBH_GPU_SCENE_SHADOW_FILTER_TENT` (0) is the filter as before;
  `BTBH_GPU_SCENE_SHADOW_FILTER_SOFT` (1) is percentage-closer soft shadows: a blocker search, a penumbra
  0.035 world units wide per unit of the blockers' height, times `directional_shadow_softness` (at least
  0.75 texel), and 4 to 32 comparison taps against the receiver's own plane. With the soft filter,
  `unlit_enabled` 3 is a plain four-tap lookup for see-through layers that are drawn many times. With the
  tent filter, 3 behaves like 2. As before, unlit batches in the transparent pass with no surface variant
  take the fast unlit pipeline, which has no shadows.
- `btbh_gpu_window_scene_set_directional_shadow_size`: sets the primary directional shadow map's size
  (256–8192 each way; 2048 × 2048 by default), for a light-space region that isn't square.
- **Horizon-based ambient occlusion** (from `beaf727`): `BTBHGPUScenePost.ssao_enabled` 2. It runs at half
  resolution and uses 4 directions of 4 steps. A depth-aware denoise and a depth-aware upsample follow, and
  the result is laid over the opaque scene before the transparent draws.
  - New post fields: `ssao_radius`, `ssao_intensity`, `ssao_power` and `ssao_bias`. A field left at 0 takes
    its default (1, 1, 1, 0.03).
  - `ssao_contact` darkens all the light but the emission.
  - `ssao_debug_view` shows the occlusion alone, in grey.
  - Per-batch `ssao_mode` (`BTBHGPUSceneSSAOMode`: full, receive only, cast only, none).
  - Procedural pixel water never takes it.
  - Its ambient mode subtracts light through the blend, so it needs the floating-point scene colour. On
    devices that fall back to 8-bit colour, only the contact mode and the radiance-cascade mode darken.
  - The SSAO of `ssao_enabled` 1 (with `ao_*` and the SSR in its resolve) is unchanged. With HBAO on,
    screen-space reflections still run through that resolve.
- **Point-light ceiling** (from `89266d2`): `BTBHGPUSceneLighting.point_light_ceiling` (luminance, finite;
  0 means none). The light that point lights add to a surface eases out towards the room the surface has left under
  the ceiling. Dark ground still flashes bright, and pale ground under a stream of shots doesn't burn white.

### Changed
- `PIXEL_WATER`'s mask is smooth (from `96486ab`): it is interpolated between its texels and cut at a half,
  with an edge about a screen pixel soft. Shores no longer run in the game's pixel steps. Only Tyrian3D uses
  the surface.
- The scene shader's samplers changed: the primary directional map is bound again at t8 through a
  comparison sampler, and the omni map moved to t9. Rebuild the shaders together with the C core.
- The tent filter reads the primary map's size from the texture, so it is unchanged at the default 2048.
- New appended fields (`point_light_ceiling`, `directional_shadow_filter`, `ssao_mode`, the `ssao_*` post
  fields) carry reserved padding. Zero-initialise the structs, as before.

### Upgrading
- **Be The Bullet Hell** (from `e95b392`):
  - Bump the submodule to `v0.2.0` and rebuild the shaders.
  - There is nothing to set. The look stays the same, because 0.1.0's fixes only change zeroed fields and the
    log contrast is already in `e95b392`.
  - Text: the font atlas filters linearly since 0.1.0.
    - Text drawn at whole pixels is unchanged.
    - A pixel font drawn at half a pixel (centred labels, outlines) gets a soft fringe.
    - Round text positions and outline offsets to whole pixels to keep it sharp.
  - `build-gpu-game` stages an explicit shader list. Add `production_hbao*.spv` to it before using
    `ssao_enabled` 2.
  - On macOS, FSR 3 falls back to FSR 1.
- **CHIMERA** (from `v0.1.0` or `v0.1.1`): point `renderer.pin` at a `v0.2.0` checkout and rebuild the shaders. There is
  nothing to set.
- **Tyrian3D** (from `tyrian3d` @ `89266d2`):
  - Bump the submodule to `v0.2.0`.
  - In its CMake, change `shaders/gpu_spike` to `shaders/gpu` (the source path and its `-I`).
  - To keep its look, set:
    - `lighting->directional_shadow_filter = BTBH_GPU_SCENE_SHADOW_FILTER_SOFT`;
    - `post->ssao_enabled = 2` instead of 1, keeping its `ssao_*` values and the per-batch `ssao_mode`.
  - `point_light_ceiling`, `set_directional_shadow_size` and `unlit_enabled` 3 keep their names.
  - Two looks from 0.1.0 differ from `tyrian3d` and are retuned in the game rather than kept as options:
    - Bloom: `bloom_knee` and `bloom_radius` are gone. Retune with master's `bloom_intensity`,
      `bloom_threshold` (in exposed units, so the old threshold times the exposure), `bloom_soft_knee` and
      `bloom_scatter`. Master blends its mip levels, where `tyrian3d` added all six, so the intensity is
      about 6.4 times the old one (Tyrian3D: 0.35 became 2.25, with knee 0.2 → soft knee 0.16, radius
      1.2 → scatter 0.75).
    - Radiance cascades (`1917761`): a source is as opaque as it is bright against the light behind it, so
      rays pass the soft edges of additive glows. The light around explosions is slightly brighter.

## [0.1.1] — 2026-11-09

0.1.0 plus three opt-in additions requested by CHIMERA ([RFC 0006](docs/rfc/0006-scene-render-to-target.md)).
C code only: no shader, uniform or G-buffer changed. A host that doesn't call the new function or set the
new field renders exactly as with 0.1.0. Additive, so a patch release under the pre-1.0 rule in
`COORDINATION.md`.

### Added
- **A scene into a render target, apart from the window's frame** (`btbh_gpu_window_scene_render_to_target`,
  marker `BTBH_GPU_SCENE_HAS_RENDER_TO_TARGET`). It is meant for a picture inside the frame, such as a unit
  portrait. It takes the arguments of `btbh_gpu_window_scene_present_instances_with_visibility`, plus a render
  target of the window (`btbh_gpu_window_render_target_create`, any size).
  - The scene renders at its own render size. When it is smaller than the target, it is enlarged with
    FSR 1 or Lanczos (as `upscale_mode` asks). At the target's size or bigger it is scaled bilinearly without
    sharpening, so a render twice the target's size is halved exactly.
  - It runs on a second scene runtime the window keeps for these scenes. That runtime has its own targets,
    shadow maps and pipelines, so the frame's targets keep their size.
  - It is never temporal: FSR 3 runs as FSR 1 and `post`'s `temporal_*` fields are ignored. The window's
    FSR 3 history, previous view-projection, bone palettes, retained shadow maps and radiance-cascade history
    are neither read nor written. The next frame reprojects as if the portrait had not been drawn. Each call
    also starts afresh itself: its shadow maps are redrawn whatever the shadow update periods, and it keeps no
    cascade history from the last portrait.
  - Call it before the frame's scene present or after its canvas present. While a scene waits for its
    canvas, it is refused and the waiting frame is kept.
- **Per-batch reactive mask** (`BTBHGPUSceneGeometryBatch.reactive_mode`). `BTBH_GPU_SCENE_REACTIVE_NONE`
  keeps a transparent batch out of FSR 3's reactive mask, so it is accumulated like the opaque scene. This
  suits ground detail layers drawn over their base, which made the whole ground "reactive". The default
  (`BTBH_GPU_SCENE_REACTIVE_SCALED`, 0) is as before: alpha times `temporal_reactive_scale`.
- **What upscaling ran** (`BTBHGPUSceneReport.upscale_mode`). It is `post->upscale_mode`, or
  `BTBH_GPU_SCENE_UPSCALE_FSR1` where FSR 3 was asked for and is unavailable (the device, a failed encode, or a
  scene rendered to a target). Before, a fallback was only a log line.

### Notes
- The first scene rendered to a target creates its runtime: pipelines (a one-time hitch of about 10–20 ms
  CPU in the smoke) and, with shadows, its own shadow maps (about 21 MB with the directional and spot maps).
- `BTBH_GPU_COST_LOG`: a scene rendered to a target is no frame of its own. It counts in the frame it is
  drawn for, and it doesn't split the GPU timing.
- Known, unchanged: with `temporal_reactive_scale` 0, transparent batches leave the shading's ambient term in
  the reactive mask. The shader writes the mask only for a positive scale. `REACTIVE_NONE` does not have this
  problem.
- Not in this release: motion vectors for meshes updated in place (cloth, decals). The vertex shader would
  need each vertex's previous position, which is a vertex-layout change (RFC 0006, open questions).
- Merged into `release/v0.2.0` (the v0.2.0 candidate) on 2026-11-10.

## [0.1.0] — 2026-11-01 (first tagged release)

The first version under semver. The public C header `include/btbh_gpu/btbh_gpu.h` is the contract:
anything it declares is covered by semver from here on; the Swift package (`BTBHRenderer`,
`BTBHRendererGPU`) wraps it. This is what the SDL3 GPU (SDL_GPU) renderer offers today.

### Device, window, input and gamepad
- Device creation with capability validation (`btbh_gpu_device_create`, `btbh_gpu_validate_capabilities`, `BTBH_GPU_REQUIRED_FEATURES`).
- Windows with VSync/immediate/mailbox present modes, resize, minimize/restore; resizable windows open at a share of the display, centred, and keep running while minimized.
- Frame loop: `btbh_gpu_window_begin_frame`, `btbh_gpu_window_poll_events`, `btbh_gpu_window_present_clear`.
- Keyboard and mouse queries (`btbh_gpu_window_key_down/_pressed`, `btbh_gpu_window_mouse_button_*`), cursor visibility and warping, clipboard text.
- Gamepad snapshots and rumble (`btbh_gpu_window_gamepad_snapshot`, `btbh_gpu_window_gamepad_rumble` via `SDL_RumbleGamepad`).
- Native open/save file dialogs (`btbh_gpu_window_show_file_dialog`).
- Asynchronous GPU frame telemetry and per-pass CPU timings in `BTBHGPUSceneReport`; opt-in GPU cost log (GPU time and triangles per pass, per model).

### Textures and fonts
- Texture loading with nearest/linear/trilinear filtering, clamp/repeat/mirror wrap and sRGB/linear colour space (`btbh_gpu_window_texture_load_ex`).
- Off-screen render targets (`btbh_gpu_window_render_target_create`).
- TrueType font atlases rasterized on the GPU path, with glyph metrics (`btbh_gpu_window_font_load`, `btbh_gpu_font_glyph`, `btbh_gpu_font_atlas`).

### Models, materials, skinning and animation
- glTF model loading (cgltf) with relative material textures; Assimp import on Windows and Linux (`btbh_gpu_window_model_load_ex`).
- Model introspection: material slots, material info, mesh info, positions and indices (`btbh_gpu_model_material_info`, `btbh_gpu_model_mesh_copy_*`).
- PBR materials: albedo, normal, ORM and emission textures and factors; per-batch material overrides and patches.
- Per-batch blend (alpha/additive/multiplied/premultiplied), billboard, cull, depth-compare, stencil and alpha-cutoff state.
- Materials can stay out of the shadow maps; model patches can fade a model and pick their depth test.
- GPU skinning with animation libraries and poses (`btbh_gpu_animation_library_load`, `btbh_gpu_animation_pose_create`, `btbh_gpu_animation_pose_set_samples`).
- Geometry arena: model primitives share a few vertex and index buffers.

### Custom meshes and instancing
- Custom meshes, created and updated at runtime (`btbh_gpu_window_custom_mesh_create`, `btbh_gpu_custom_mesh_update`).
- Instance buffers and transform streams (`btbh_gpu_window_instance_buffer_create`, `btbh_gpu_pack_transform_stream_instances`).
- Persistent instance ranges, lazily uploaded, with adjacent persistent batches consolidated.
- LOD: persistent ranges small on screen draw a simplified model.
- Frustum and camera-ray queries; authored cluster culling for spot shadows.

### Scene lighting and shadows
- Direct PBR shading with ambient, procedural sky/ground/sun environment and SH irradiance (`BTBHGPUSceneLighting`).
- Primary and secondary directional lights with soft shadows; the primary shadow can cover a sphere around the camera target.
- Spot light with shadow, or an omni (cube) shadow (`spot_shadow_is_omni`); a near-occluder shadow share (`spot_occluder_shadow_strength/_reach`).
- Street lamps: up to `BTBH_GPU_SCENE_MAX_LAMPS` (40) per-pixel point lights per scene.
- Cloud shadows on the sun (`cloud_*`), surface wetness (`surface_wetness`), rain ripples (`rain`) and lit night windows (`window_glow`).
- Shadow-pass specialization and budgeting; scenes without shadows skip the shadow resources entirely.

### Surfaces
- Surface variants (`BTBHGPUSceneSurfaceVariant`): glitter (static and animated), water, dissolve, shatter, soft particle, rim light.
- Procedural surface detail (`detail_parameters`): ground, grass, facade, paving, glass and lamp-lens patterns with albedo variation and bump.
- Water with world-space waves; foam only on the highest crests.
- Render-only world curvature (`world_curvature`): small-planet and hyperbolic looks.
- Screen-space refraction (`BTBH_GPU_SCENE_SURFACE_REFRACT` = 10, `BTBH_GPU_SCENE_HAS_REFRACTION`): batches that show the scene
  behind them through a flowing displacement with dispersion, chromatic lines and a normal push for shockwaves, in a pass
  of their own after the transparent one; frames without one pay nothing (RFC 0005, requested by CHIMERA).

### Radiance cascades (global illumination)
- Holographic radiance cascades (`BTBHGPUSceneRadianceCascades`) with low/high/auto tiers, typed emitters (disk, rectangle, segment) and debug views.
- Status and per-pass telemetry, with a fallback reason when RC turns itself off (`BTBHGPUSceneRadianceCascadesStatus`).

### Post chain (`BTBHGPUScenePost`)
- SSAO (radius, intensity, contact strength, direct share) and screen-space reflections (`ssr_*`), resolved before effects.
- HDR composite: distance fog with world-edge fade, soft-threshold bloom, vignette.
- Tone mapping (Reinhard, PBR Neutral, ACES), exposure, log-space contrast, saturation, 3D LUT grading.
- Contrast-adaptive sharpening (CAS) and tilt-shift.
- Anti-aliasing: SMAA (medium/high) or FXAA.
- Upscaling: linear, Lanczos, FSR 1 (EASU + RCAS), FSR 3.1 temporal (`BTBH_GPU_SCENE_UPSCALE_FSR3`, with motion vectors, reactive mask and texture LOD bias; falls back to FSR 1).
- Horizontal mirror (`mirror_horizontal`).

### Canvas / 2D
- Canvas2D batches (`BTBHGPUCanvasPass`) with alpha/additive/multiplied/premultiplied blend modes, presented with the scene in one frame (`btbh_gpu_window_canvas_present`).

### Readback and capture
- Presented-frame readback (`btbh_gpu_window_request_presented_frame_readback`, `btbh_gpu_window_readback_presented_frame`); PNGs are written opaque (`btbh_gpu_write_png_rgba8`).
- Scenes render into sub-rectangles; a finished scene can be copied or captured aside (`btbh_gpu_window_copy_pending_scene_texture`, `btbh_gpu_window_capture_pending_scene_texture`).

### Platforms and build
- Windows and Linux on Vulkan (SPIR-V); macOS on Metal (metallib). There is no Direct3D backend.
- HLSL shaders in `shaders/gpu`, compiled by SDL_shadercross; pinned SDL3, SDL_shadercross and Assimp in `external/`.
- SwiftPM package (`Package.swift`, `Sources/`) and CMake; `BTBH_RENDERER_BUILD_ROOT` overrides the native build directory; native tests are opt-in (`BTBH_BUILD_GPU_NATIVE_TESTS`).

### From the tyrian3d branch (merged for v0.1.0)

The branch up to `92b3b35`. Its later commits (`96486ab`, `f7c3773`, `beaf727`, `89266d2`: smooth pixel-water
mask, PCSS sun shadows, HBAO SSAO, point-light ceiling) are planned for v0.2.0 as opt-in features.

- Raw RGBA8 textures from memory, region updates for streaming, and access to the SDL window (`btbh_gpu_window_texture_create_rgba8_ex`, `btbh_gpu_texture_update_rgba8`, `btbh_gpu_window_sdl_window`).
- Unlit surfaces that receive the primary directional shadow (`unlit_enabled = 2`); public per-batch shadow visibility (`btbh_gpu_window_scene_present_instances_with_visibility`).
- Glints surface for water drawn in textures (`BTBH_GPU_SCENE_SURFACE_GLINTS`).
- Small point lights without shadows for flashes, up to 32 (`point_light_count`, `point_lights`).
- Relief from albedo brightness for unlit surfaces under point lights (negative `normal_scale`).
- Procedural pixel water surface (`BTBH_GPU_SCENE_SURFACE_PIXEL_WATER`).
- Radiance cascades for pixel art: unlit receivers (`rc_receiver`) and an additive mode (`additive`).
- Dark scenes: unlit surfaces darkened, lit by the spot light, brightest pixels glowing (`unlit_darkness`, `unlit_glow_threshold`).
- Font atlases filtered linearly (text at any size).
- Mip caps: `generate_mipmaps` n > 1 limits a texture to at most n levels, for atlases whose cells would bleed.

### Changed for consumers of tyrian3d
- Surface variants renumbered: `GLINTS` 6 → 8, `PIXEL_WATER` 7 → 9 (master already had `SOFT_PARTICLE` = 6, `RIM` = 7); `BTBH_GPU_SCENE_SURFACE_COUNT` = 10.
- The tyrian3d bloom (`bloom_knee`, `bloom_radius` and its own bloom down/up/composite shaders) was dropped in favour of master's HDR-composite bloom: `bloom_enabled`, `bloom_intensity`, `bloom_threshold` (in exposed units), `bloom_soft_knee` (0..1), `bloom_scatter` (0..1).
- Only the primary directional shadow map is 2048; the secondary stays 512 (master's split), instead of tyrian3d's global 2048.
- Font atlases are now linear for all consumers.

### Fixed for zero-initialised hosts
- `BTBHGPUScenePost`'s vignette fields are only validated when `vignette_enabled` is set; before, a zeroed struct
  failed every scene present (`vignette_smoothness` had to be positive). Found by Tyrian3D on the Mac.
- `BTBHGPUScenePost.ao_radius` no longer has to be positive: zero (a host that never set the AO controls) keeps
  the previous fixed AO values (radius 1, intensity 0.4, contact 0.75, direct share 0.2). Before, every scene
  present from a zeroed `BTBHGPUScenePost` failed validation.

### Notes for every consumer
- **Changed look for hosts from before 2026-10-16** (added after the release): `contrast` scales in log space around
  mid grey 0.18 since `0722f5f`, so black stays black and darks no longer clip. With `contrast` above 1, darks and
  mid-tones come out lighter than with the old linear curve around 0.5 (CHIMERA's estuary at 1.12: shaded ground and
  the abyss). CHIMERA kept the new look. A switch back (`contrast_mode`) exists on branch `fix/contrast-mode` and
  ships only if a game asks for it.
- `BTBH_GPU_PHASE2_REQUIRED_FEATURES` is now `BTBH_GPU_REQUIRED_FEATURES`; the old name stays as a deprecated alias.
- FSR 3 is not available on Metal yet: six of its compute passes produce MSL whose bindings collide with SDL_GPU's
  Metal convention, so they are not built as metallibs and `BTBH_GPU_SCENE_UPSCALE_FSR3` falls back to FSR 1 on macOS
  (with a log line). Windows and Linux are unchanged. A proper binding fix is planned for v0.2.0.
- macOS builds no longer need `CMAKE_OSX_DEPLOYMENT_TARGET` to link SDL_shadercross, and `tools/check.sh` uses Unix
  Makefiles when Ninja is not installed.
- Hosts that zero-initialise the scene structs get every feature added since their last update switched off.
- The scene shader's uniform block grew (street lamps, clouds, point lights): rebuild the shaders together with
  the C core; mixing shader and C versions is not supported.

## History before v0.1.0

### 2025-02 – 2026-07: r3d upstream (Bigfoot71 and contributors)
- 2025-02: r3d starts as a raylib OpenGL deferred renderer: IBL, bloom, SSAO, soft shadows (dir/spot/omni), FXAA, instancing, forward pass, billboards, particles.
- 2025-03 – 04: PCSS shadows, AgX tonemapping, animated sprites, physically based bloom.
- 2025-07 – 08: GPU skinning and model animations, depth of field, fog, SSR, sprite shadows.
- 2025-11 – 12: decals, primitive instancing, complete SSR and skinning, draw-call grouping, SSIL, à-trous SSAO denoiser.
- 2026-01 – 03: procedural skyboxes, reflection probes, SMAA, custom instance buffers, animation state trees.
- 2026-04 – 07: custom instance attribute layouts, volumetric fog, handle pools, temp allocator.

### 2026-07: the fork and the SDL_GPU renderer
- 07-18: fork design for Be The Bullet Hell; package foundation, POD types and generational handles.
- 07-19 – 07-24: backend-neutral renderer seams: math, platform lifecycle, mesh/model streams, material descriptors, animation tree graphs, presentation viewport.
- 07-26: SDL_GPU vertical spike, GPU resource lifecycle, Phase 2 deterministic scene runtime with shadow maps (Windows gate closed).
- 07-27: production package: textures, glTF models and animations, Assimp on Windows, instancing, custom meshes, GPU skinning, font atlases, Canvas2D, frame capture/readback.
- 07-28: persistent instance buffers, PBR material channels, file dialogs, direct PBR, procedural IBL, directional shadows.
- 07-29 – 07-31: Windows Vulkan visual parity (SSAO, SMAA), FXAA, shadow specialization and budgets, pass profiling; Vulkan gameplay migration finalized.

### 2026-08 – 09: Metal and radiance cascades
- 08-02: production Metal renderer parity on macOS (adds Lanczos upscale, CAS, tilt-shift and LUT stages).
- 09-01: production radiance cascades renderer (classic and holographic).
- 09-03 – 09-05: lighting polish, omni shadows in the RC path, Metal RC stabilized, faster debug asset imports.

### 2026-10: one renderer, then features
- 10-01: r3d/OpenGL renderer, GPU spikes and classic radiance cascades removed; holographic RC kept, `shaders/gpu_spike` became `shaders/gpu` (~81k lines deleted).
- 10-02 – 10-03: horizontal mirror; HDR composite (fog, bloom, vignette); procedural surface detail; shadow-excluded materials.
- 10-05: softer shadows, window sizing, sub-rectangle scenes, soft particles, world-space water waves, continuous shatter collapse.
- 10-07: paving detail and the spot light's near-occluder shadow share.
- 10-10: Linux build and run (Vulkan, X11/Wayland); LOD for small persistent ranges.
- 10-13 – 10-14: FSR 1, rim-light surface variant, FSR 3.1 temporal upscaling with motion vectors.
- 10-15 – 10-18: world curvature, wetness, omni street lamps (32, then 40), GPU cost log, glass and lamp-lens detail, cloud shadows, SSR and tunable AO, geometry arena, rain ripples, gamepad rumble.
- 10-19 – 10-22: opaque presented-frame PNGs; water foam on the highest crests (HEAD `e95b392`).
- 10: tyrian3d branch (raw textures, pixel-art lighting, glints, pixel water) merged for v0.1.0.
