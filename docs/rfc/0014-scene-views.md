# RFC 0014: Scene views: two cameras in one present, each clipped to a half-plane of the frame

- **Status:** Accepted on 2026-11-27: v2 answered all 12 questions through its session (see "Decisions"). Phase 1 is
  next, after 0.15.0
- **Requested by:** Be The Bullet Hell v2 (its session, 2026-11-26: local co-op's split screen, after the owner's fifteenth
  play-test on a 4K TV: "when the screen splits, the fps drops drastically")
- **Affects:** Be The Bullet Hell v2 (adopts it). Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D: nothing to
  change; it is opt-in. They rebuild the shaders with the C core, as every release. The two scene vertex shader sources
  (`production_model.vert`, `production_animated_model.vert`, so five built files) and six screen-space fragment shaders
  change their SPIR-V and MSL; their pictures should not change (to be proven byte for byte, see "Platforms and testing")
- **Issue:** none. v2 answered through its session on 2026-11-27
- **Depends on:** nothing. 0.15.0 (released 2026-11-27, `6f4267a1`: `btbh_gpu_window_scene_render_to_target_view`
  with a scissor rect and kept per-view runtimes, a gamepad roster, the shadow-period setter in `btbh_gpu.h`) is the
  step before it and the A/B baseline for every measurement here. v2 adopted it: its split's second view renders at
  0.67 inside player 2's rect, a 13.1 ms frame at 4K High against 10.2-10.4 ms for one view. Written against master `aa8b1146` (v0.14.2); every
  line reference is that commit's (0.15.0 moved `scene.c`'s lines after about 12,500). `src/gpu/btbh_gpu_scene.c` is "`scene.c`" below

## Need

v2's split screen, from its request and `v2/docs/coop.md` ("The camera", "Drawing the split", "The split's cost"):

- **One view** while both kaijus fit on the screen with a 20 % margin.
- **Past that, a Voronoi split.** The divider goes through the screen's middle, perpendicular to the line between the
  kaijus as the screen shows it. It rotates as they move round each other. Both cameras keep one angle, one lens and
  one height; they differ only by where they stand (`v2-game/src/split.rs:1-13`, coop.md "The camera").
- **How it is drawn on 0.14.2** (`v2-game/src/split_view.rs`):
  1. the second view renders through `btbh_gpu_window_scene_render_to_target` into a target (the window's second
     runtime: its own targets and shadow maps, never temporal);
  2. the frame's own present draws the first view over the whole screen;
  3. the canvas lays the second view over player 2's half as a textured polygon;
  4. the divider (5 px at 1080 lines, scaled with the height: 10 px at 4K) and the HUD go on top.
- **What each view gets** (`v2-game/src/main.rs:8742-8762`): the same lighting, except its own sun shadow matrix
  fitted to its ground, no kaiju spot map in the second view (the first kaiju's crisp map would black it out), and by
  night its own 40 street lamps.
- **Culling:** v2 keeps a thing when either view's frustum sees it (`Frustum::either`). Both calls draw the same
  batches.

What v2 measured (RTX 3070 Ti, 3840 x 2160, High, coop.md "The split's cost"):

| | Frame |
| --- | --- |
| One shared view | 10.5 ms |
| Split, the whole second view (0.14.2) | 18.9 ms (+82 %) |
| Split, the second view capped at 1080p pixels, FSR 1 at 0.75 (now) | 13.5 ms (+29 %) |

| One part off the whole second view | Saves |
| --- | --- |
| Its shadow pass | 0.7 ms |
| Its SSR | 1.0 ms |
| Its SSAO (v2's is the HBAO, `ssao_enabled` 2) | 2.0 ms |
| Half its size | 5.5 ms |

At the Deck preset (1280 x 800, CPU-bound on that PC) the split adds CPU: the second call takes 1.3 ms; the main present
goes from 1.3-1.4 to 2.8-2.9 ms, its draws from 911 to 1516; the scene build +0.4 ms.

v2's ask: one present that draws N (2) views of the same batches and instances into one frame, each clipped to its own
screen half-plane, with one shared shadow pass, per-view opaque and transparent passes, post once over the composed
frame, FSR 3 with a history per view (or an honest FSR 1 fallback), shared preparation and upload, and per-view
visibility. The goal: two views for about one view's fragment work plus one extra geometry pass.

## What the present does today

`btbh_gpu_window_scene_present_instances_internal` (`scene.c:9253-12394`) draws one camera. Its steps, in order, and
what each reads of the view:

| Step | Where | Reads the view? | With two views |
| --- | --- | --- | --- |
| Validation, world details | `:9323-9380` | No | Once |
| Frame setup: cascades, FSR 3 prepare, shadow resources, SMAA/FXAA | `btbh_gpu_scene_prepare_frame`, `:8681-8958` | No (FSR 3 only for the window's frame, `:8766-8776`) | Once |
| Jitter and previous view-projection | `:9507-9586` | Yes: the requested matrix, the runtime's `temporal_previous_view_projection` (`:533`) | Jitter shared; previous matrix per view |
| Bones, dig bindings, shader checks | `:9600-9676` | No | Once |
| Opaque sort | `:9687-9711`; key `btbh_gpu_scene_batch_view_depth` (`:5986`), order `:6104` | Yes: view depth per batch | Per view |
| Model pipelines | `:9713-9733` | No | Once (the same pipelines serve both views) |
| Upload: instances, bones, motions, queued meshes | `:9745-9946` | No | Once |
| Shadow maps: sun (2048 by default, `:28`), fill (512), spot (1024) or omni (6 x 512) | `:9960-10105`; `btbh_gpu_scene_render_directional_shadow` `:7771` | The light's matrix only. The camera's right and up orient billboard casters (`:7832-7841`, `production_shadow.vert.hlsl:86-88`) | Per distinct matrix (below) |
| Retained maps and update periods | `:9984-10003`; matrices kept at `:10091-10097`; receivers read the kept matrix, `:10913-10925` | No | Per map |
| Particle transmittance (RFC 0013 phase 2) | `:10106-10126`, `:7667` | No: sun space, puffs face the sun (`production_particle_transmittance.vert.hlsl:2`) | Per sun map |
| Targets | `:10148-10179` | No | Once |
| Opaque pass, motion vectors in the sixth target | `:10345-11581` | Yes: view-projection, camera, previous matrix, jitter (vertex block `:72-106`), camera position (fragment block `:109`) | Per view, clipped |
| Decals and scars | ordinary batches in the opaque pass (`LESS_EQUAL`, `KEEP_ORDER`); no renderer subsystem | As the opaque pass | Per view |
| HBAO (and contact AO) | `:10354-10392`, before the cascades `:11608-11640`; `btbh_gpu_scene_encode_hbao` `:8595-8673` | Yes: inverse matrix, projection scale; taps reach a tenth of the view's height (`production_hbao.frag.hlsl:44`, `:175`) | Per view, clipped; taps kept to their side |
| Radiance cascades: emitters, material sources, field passes | `:11641-11758`; sources `:8213` (camera right/up for billboards) | The field is world space, placed by the host's first occlusion cascade (`btbh_gpu_radiance_cascades.c:3951-3961`) | Once, if the field covers both views |
| Radiance cascades: resolve, reconstruct, compose | `btbh_gpu_radiance_cascades.c:3648-3845` | Yes: inverse matrix (`:3664-3668`, `:3756-3760`) | Per view (compute: per view's box) |
| SSAO, SSR resolve | `:10393-10497` | Yes: `BTBHGPUPostUniforms` inverse matrix and matrix (`:204-216`); SSR marches in screen space (`production_ssao_resolve.frag.hlsl:330-338`) | Per view, clipped; rays stop at the seam |
| Scene distance (soft and lit particles, refraction) | `:10498-10532` | Yes: inverse matrix, camera position | Per view, clipped |
| Refraction copy | `:10533-10556` | No | Once |
| Transparent and additive pass, reactive mask | `:10557-11581`; mask scale `:11195-11200`; lit particles' frame block `:7361` (inverse matrix) | Yes | Per view, clipped |
| Refraction pass | third phase of the loop, `:10342-10344` | Yes; samples the copy at a displaced screen position | Per view, clipped; displaced samples may cross the seam |
| Bloom | `:11775-11848` | No | Once; bleeds across the seam |
| HDR composite: fog, the world-edge fade, bloom add, vignette | `:11849-11902`; `production_hdr_composite.frag.hlsl:35-48` | Yes: inverse matrix for the fog's depth | Per view, clipped (vignette is screen-centred, the same in both) |
| LUT, CAS, tilt-shift, tonemap output | `:11904-11986` | No | Once (tilt-shift's band is fixed to the screen) |
| FXAA, SMAA | `:11987-12078` | No | Once |
| Mirror | `:12083-12105` | No | Once |
| FSR 3 | `:12106-12245` | Depth, motion, reactive mask; camera near, far and lens from `post->temporal_*` | One context (phase 2) |
| FSR 1, Lanczos | `:12246-12326` | No | Once |
| Temporal bookkeeping | `:12350-12363` | Yes: keeps the requested matrix | Per view |

- **Clouds and cloud shadows** have no pass. The scene fragment shaders take them from the lighting
  (`cloud_controls`, `cloud_shape`, filled at `:10878-10884`), in world space: shared.
- **Height fog** doesn't exist in the renderer (RFC 0002 is a draft). The fog is the composite's distance fog plus the
  world-edge fade.
- **The lit smoke** is lit particles in the transparent pass. Per view: its frame block. Per sun map: the transmittance
  map.
- **The scene uses no viewport or scissor today.** The only `SDL_SetGPUScissor` is the canvas's
  (`btbh_gpu_canvas.c:1689`).

## Clipping a view to a half-plane

A half-plane in output pixels is linear in clip space. With the output `W x H`, `x_ndc = 2x/W - 1` and
`y_ndc = 1 - 2y/H` (SDL's convention on both backends: +y up in NDC, pixel rows down). Then
`clip[0]*x + clip[1]*y + clip[2] >= 0` becomes `a*x_c + b*y_c + c*w_c >= 0` with:

- `a = clip[0] * W / 2`
- `b = -clip[1] * H / 2`
- `c = clip[0] * W / 2 + clip[1] * H / 2 + clip[2]`

It holds at any render size: NDC doesn't depend on it.

### The options

| | Exact on a diagonal | Shades the other half? | Interactions | Verdict |
| --- | --- | --- | --- | --- |
| **`SV_ClipDistance`** | Yes: the rasterizer never makes the other half's fragments | No | None: no pipeline state, works the same for opaque, transparent, refraction and full-screen passes | **Recommended** |
| Stencil mask | Yes | No (early stencil) | Every model pipeline's stencil state doubles; x-ray (`NOT_EQUAL_KEEP`) can't also test a view bit | Rejected |
| Depth pre-fill | Yes for one view | No | Can't hold two views' real depth in one buffer; `ALWAYS` batches and the transparent pass ignore it | Rejected |
| Fragment `discard` | Yes | Partly: rasterized, launched, and before 0.14.2's rule, all of it | Breaks 0.14.2's "no derivative after a discard" | Rejected |
| Scissor boxes alone | Only axis-aligned splits | Yes, inside the box | None | Not enough; boxes only bound compute dispatches |

**`SV_ClipDistance` is available everywhere the renderer runs.**

- **shadercross carries it.** Compiled in `%LOCALAPPDATA%\btbh-rfc0014-scratch` with the release build's
  `shadercross.exe` (the `CMakeLists.txt:454-485` command lines):
  - a minimal vertex shader writing `SV_ClipDistance0`: SPIR-V has `OpCapability ClipDistance` and the `ClipDistance`
    builtin; MSL 2.4 has `float gl_ClipDistance [[clip_distance]] [1]` plus SPIRV-Cross's `gl_ClipDistance_0
    [[user(clip0)]]` copy (its `enable_clip_distance_user_varying` default, `spirv_msl.hpp:414`);
  - `production_model.vert` with the output added under `#if !BTBH_RC_SOURCE_PROJECTION`: the same in it,
    `_detail_table` and `_lift` (360 bytes more SPIR-V); `production_rc_material_source.vert` stays without it;
  - a clipped full-screen post vertex shader (one uniform block): the same, and its reflection says one uniform buffer.

  None was run on a GPU, and no metallib was made (no Mac here).
- **SDL 3.4.2's Vulkan device enables `shaderClipDistance`.** It is an opt-out feature, on unless
  `SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN` is false (`external/SDL/src/gpu/vulkan/SDL_gpu_vulkan.c:12471`).
  A physical device without it is rejected (`:12035`). The renderer doesn't set the property (`btbh_gpu_device.c:25-73`).
  So every Vulkan device the renderer has ever run on has it. RADV (the Deck) supports it.
- **Metal** needs nothing enabled: `[[clip_distance]]` is a vertex output. SDL's Metal backend doesn't touch it.
  Unverified until the Mac session's `check.sh` builds the metallib and runs the smoke.
- **D3D12:** the renderer creates only Vulkan and Metal devices (`btbh_gpu_device.c:50-73`). `SV_ClipDistance` is native
  in DXIL anyway.

**Where the plane goes.** `BTBHGPUSceneUniforms.surface_controls` y, z and w are free: the vertex shaders read only x
(`production_model.vert.hlsl:142,160,283`, `production_animated_model.vert.hlsl:126,206`; the core writes only
`[0]`, `scene.c:8370`, `:11104`). The plane `(a, b, c)` goes there. All zero means the whole frame, and the shader then
writes 1, so nothing is clipped and the one-view path keeps its bytes. The RC source projection's builds leave it out.

**Why not discard.** HLSL's `discard` is `OpKill` here (0.14.2's CHANGELOG: SDL doesn't enable demote). A clip at the top
of the shader puts every derivative after a kill, which 0.14.2 just removed: quads on the seam would get undefined
derivatives. A clip at the end shades the whole other half first: it saves nothing of the 7.3 ms. Either way the
fragments are rasterized and launched.

**Why not stencil.** The pipelines bake three stencil variants (`scene.c:66-70`, `:1387`). Testing a view bit doubles
them all. `NOT_EQUAL_KEEP` (v2's x-ray) needs "low bits differ and the view bit is set", which one compare op can't
express. It also costs a mask draw per view.

**Why not a depth pre-fill.** Filling the other half at depth 0 works for the first view. The second view then needs its
own half back at 1 and the first half blocked, which overwrites the first view's real depth. SSAO, SSR, the fog and
FSR 3 read that depth later.

**Scissors.** A diagonal half's bounding box covers 60-100 % of the screen: 0.78 of the width at 45 degrees on 16:9. Clip
distances are exact, so geometry needs no scissor. Boxes bound only the compute dispatches (phase 3).

**The seam.** The two views' clipped edges are the same line computed from different triangles, so a pixel centre on it
could fall in neither. The renderer widens each half by half a render pixel (`clip[2]` grows by
`0.5 * |(clip[0], clip[1])| * output/render`). Pixels in the overlap are drawn by both views, and the shared depth test
picks the nearer surface. The band is one pixel, under v2's divider.

## One shadow pass for two views

**What the renderer has.** No cascaded shadow maps. Three maps (`scene.c:484-486`, made at `:3951-4030`): the sun's
(2048 x 2048 by default, settable with `btbh_gpu_window_scene_set_directional_shadow_size`), the fill's (512) and the
spot's (1024, or the omni's six 512 faces). The host gives every matrix (`BTBHGPUSceneLighting`,
`btbh_gpu.h:1398-1401`). v2's "cascades" are two maps:

- the sun map, fitted by v2 to a sphere round the ground in view (`ground_in_view`, `v2-game/src/main.rs:10764-10773`;
  `sun_shadow_view_projection`, `v2-platform/src/camera.rs:173-187`);
- the kaiju's crisp spot map (`SPOT_OCCLUDER_SUN_ONLY | ORTHOGRAPHIC`).

**The sizes in v2's numbers** (estimates from v2's classic camera, not measured):

- The lens is 45 degrees at a 45 degree pitch, 33.3 cells across at the target (`v2-render/src/camera.rs:65-75`). The
  ground in view is a trapezoid 24 cells wide near and 57 far, 32 deep. `ground_in_view` gives a sphere of about 38
  cells. So a view's sun map spans about 76 cells: about 27 texels a cell at 2048 (High).
- With the kaijus 60 cells apart (v2's bench, `BTBH_COOP_GAP=60`), R is about 6-10 cells and the cameras stand about
  40-48 cells apart.

| Fit | Map | Cells a texel (High) | Linear density against one view's |
| --- | --- | --- | --- |
| One view (today) | 2048 x 2048 over 76 | 0.037 | 1 |
| One map, v2's sphere round both views, 60 cells apart | 2048 x 2048 over about 120 | 0.059 | 0.63 |
| One map, a rectangle turned along the views (same texels, about 2573 x 1629) | over 120 x 76 | 0.047 | 0.80 |
| One map, sphere, kaijus 100 cells apart | over about 160 | 0.078 | 0.48 |
| One map, rectangle, kaijus 100 cells apart | about 2967 x 1414 over 160 x 76 | 0.054 | 0.69 |
| One map per view | 2 x 2048 x 2048 | 0.037 | 1 |

A union fit loses density without bound as the players part. The rectangle needs v2 to turn the map's right vector
along the line between the views (its matrix fixes it today) and a fixed non-square size (a size change makes the map
anew).

**The cost** against v2's 0.7 ms (one 2048 sun map's pass at 4K High):

- **The update periods matter more than the fit.** v2 doesn't call `btbh_gpu_window_scene_set_shadow_update_periods`
  (nothing in `v2/crates` does), and the period defaults to 1 (`scene.c:1046-1053`): its maps redraw every frame.
  coop.md says "every second frame"; the code says otherwise.
- **Maps take turns.** The update loop gives each active map its own phase by ordinal (`scene.c:9975-10000`). Two sun
  maps at period 2 alternate: one is drawn each frame.

| Choice | GPU each frame, against today's split (0.7 + 0.7) | Texels | Lag |
| --- | --- | --- | --- |
| One map over both views, period 1 | about 0.8-0.9 ms (the union's casters): saves about 0.5-0.6 | 0.48-0.80 x | None |
| One map per view, period 1 | 1.4 ms: saves nothing | 1 x | None |
| **One map per view, period 2, taking turns** | **about 0.7 ms: saves 0.7** | **1 x** | A map is at most a frame old |
| A shared far map with a crisp map per view | about 0.8-0.9 ms plus a spot map per kaiju (small: its casters are a kaiju) | Sun 0.48-0.80 x; each kaiju crisp | None |

The "shared far, per-view near" row is what v2 has today in spirit: the sun map is the far one, the kaiju's spot map
the near crisp one. It needs a spot map per view.

**Recommendation: let the host choose by the matrices it passes.**

- Each view may carry its own lighting (see Proposal). Views whose sun matrices are equal byte for byte share one map;
  otherwise each draws its own.
- The same holds for the spot map. So the "far shared, near per view" row is available too.
- The map's casters are the call's `directional_shadow_visibility` when shared, the view's `shadow_visibility` when its
  own.
- v2 shares the map while the views are near (the split opening: both cameras stand in the middle) and passes each
  view's own fit once they part. With period 2 the sun's pass costs about one map a frame.
- Billboard casters face view 0's camera in a shared map and their own view's in an own map. v2's views share one
  angle, so they agree.
- **Memory a second view adds:** 16 MiB (a 2048 sun map, D32), 4 MiB (a 1024 spot map), and 8 MiB with the
  transmittance map at half size.

## Screen-space passes over a composed frame

Three kinds, by what they read:

**Per pixel, the view's matrices** (fog, distance, the HDR composite): they need the right view's matrix at each pixel
and nothing else. Each runs as **one draw per view in one render pass**: production_post.vert's full-screen triangle,
clipped to the view's half by a sibling `production_post_view.vert` (`SV_ClipDistance` from a vertex uniform block).
Each draw pushes its view's block as today. Their fragment shaders don't change.

**Neighbourhoods, the view's matrices** (SSAO, its denoise, SSR, HBAO, its denoise and upsample): the same clipped draws.
They also sample round the pixel, so a tap must stay on its own side: the seam is treated as the screen's edge.

| Pass | Reaches across the seam today | Rule |
| --- | --- | --- |
| HBAO (`production_hbao.frag`) | Up to a tenth of the view's height: 216 px at 4K (`:44`, `:175`) | A tap on the other side is skipped, as a background tap is (`:177-180`) |
| HBAO denoise, upsample | The 4 x 4 pattern; the depth-aware upsample's taps | Taps on the other side get no weight |
| SSAO (`ssao_enabled` 1), its denoise | Its kernel; the bilateral blur | As the HBAO |
| SSR (`production_ssao_resolve.frag`) | A ray's whole reach | The march stops where it stops at the screen's edge (`:336-339`) |

The plane rides free uniform words, in UV units so that any resolution can test it:
`BTBHGPUSceneHBAOUniforms.view` y, z, w (`scene.c:227`; only x is read, `production_hbao.hlsli:22`) and
`BTBHGPUPostUniforms.ssr_hit_options` y, z, w (`scene.c:215-216`). All zero disables the test.

**No view at all** (bloom, LUT, CAS, tonemap, vignette, tilt-shift, FXAA, SMAA, FSR 1, Lanczos, mirror): once over the
composed frame, unchanged.

- **Bloom** bleeds across the seam by its widest level (`BTBH_GPU_SCENE_BLOOM_LEVELS` 6: up to 1/64 of the frame). A fire
  near the seam glows a little into the other half. Phase 3 can keep its taps to their side, scaling the plane to each
  level; a texel that straddles the seam stays mixed.
- **FXAA, SMAA, CAS, EASU** reach a few pixels: they soften the seam, which suits it.
- **Tilt-shift** keeps one focus band fixed to the screen. v2 doesn't use it.
- **The vignette** is centred on the screen, as v2's composite gives it today (both of its targets are full-screen).

**Compute passes with the view's matrices** (the cascades' resolve and reconstruct, `btbh_gpu_radiance_cascades.c:3732-3789`)
can't be clipped. They run per view over the half's bounding box, with an early return for the other side's pixels
(phase 3).

**Refraction** samples a displaced screen position. Near the seam it can read the other view's copy, as far as the
displacement goes. Accepted in phase 1, under the divider. Phase 3 can clamp it: the scene fragment block has three
reserved words (`lamp_controls` z and w, `surface_wetness` w; `scene.c:136-144`).

## FSR 3 with two views

**What a temporal frame keeps** (`scene.c:9531-9586`, `:12350-12363`; `btbh_gpu_fsr3.h`):

- one jitter sequence;
- one previous view-projection;
- one history at the output size: two RGBA16F upscaled images, the output, the locks, and render-size luma histories.

At 4K one RGBA16F output-size image is 66 MB.

**Motion vectors.** The opaque pass writes each pixel's motion from the vertex block's previous matrix (`scene.c:97`,
`:10688-10692`). Per-view passes push each view's own previous matrix. So every pixel's motion is relative to its own
view's previous frame.

**The jitter** is added in clip space (`:9566-9574`). Both views take the same offset, so one sequence serves both.

**The seam moves and turns.** A pixel near it may reproject to where the other view stood last frame. FSR's disocclusion
test compares depths, and v2's two views are the same camera moved: the ground lies at the same depth in both. So FSR
would take the other view's history: a smear along the swept band.

**Recommendation: one context over the composed frame, not one per view.**

- **Per-view contexts don't fix the seam.** Context A would still accumulate B's half, and when the seam moves into it, A
  reprojects B's content with A's motion: the same smear. They double the output-size memory (about 200 MB at 4K) and
  FSR 3's GPU time (a full-resolution pass each).
- **One context, with the seam marked reactive.** A small pass after the transparent passes writes 1 into the reactive
  mask (blend max) where a pixel's history comes from another view:
  - it reads the motion vector and finds the pixel's previous position;
  - it tests that position against the previous frame's planes;
  - where the owner differs from the pixel's view, FSR takes this frame's colour there.

  This frame's planes and the previous frame's are kept per view.
- **Opening and closing.** When the split opens, both cameras stand in the middle (coop.md). A view that opens continues
  view 0's history: its previous matrix is view 0's. Two views whose previous matrices are equal count as one, so
  neither frame marks anything.
- **Cuts.** `post->temporal_reset` resets the whole history, as today. A view's `TEMPORAL_RESET` flag (a player respawned
  elsewhere) marks only its half reactive and takes its current matrix as the previous one.
- **FSR 3 needs one projection.** It reads the camera's near, far and lens from `post->temporal_*` (`:12120-12124`). The
  views must share them. The renderer compares each view's projection scale (as at `:9390-9394`) with view 0's. If they
  differ, the frame runs FSR 1.

**The honest fallback.** FSR 1 when:

- the device lacks FSR 3 (as today);
- the views' projections differ;
- phase 2 hasn't shipped (phase 1 runs FSR 1 for every frame with two views).

`report.upscale_mode` says FSR 1 (`btbh_gpu.h:1774-1779`), as `render_to_target` does since 0.1.1. A frame on FSR 1 ends
the temporal run (`scene.c:12351`), so the next FSR 3 frame starts a new history.

## The CPU side

**Shared once:** validation, world details, setup, bones, dig bindings, shader checks, model pipelines, upload, targets,
post encode, submit (the steps marked "Once" above).

**Per view:**

- the opaque sort over its visible batches (an order buffer per view; `opaque_order`, `scene.c:556-559`);
- the frame part of both uniform blocks, filled once per view per pass (RFC 0011 phase 1c, `:10625-11001`);
- the batch loop with its visibility.

RFC 0011's run state resets at each view: the first batch of a view pushes everything.

**Where v2's Deck cost goes.** Today the split encodes about 3,032 draws in two calls. Both carry the union of both
views' frustums: 1,516 in the main present (911 for the shared view) and the same 1,516 again in the second call, inside
its 1.3 ms.

- **With per-view visibility alone** (each view culled by its own whole frustum): view 0 about 911, view 1 about 911,
  about 1,800 draws in one call. The views' frustums overlap, so this is more than the union. The second call's fixed
  cost goes.
- **With each view culled by its frustum and its half-plane:** each view draws about its half, roughly 911 plus what
  straddles the seam. The half-plane is a world plane through the camera: `VP^T * (a, b, 0, c)`, with `(a, b, c)` the
  clip-space plane above. v2 can add it to its frustum test as a fifth plane.

v2's own numbers give the marginal draw cost: (2.9 - 1.4) ms / (1,516 - 911) draws, about 2.5 us a draw on the Deck
preset, submission included. Estimates, not measured:

| Deck preset, present calls | CPU |
| --- | --- |
| Shared view | 1.4 ms |
| Split on 0.14.2 (and 0.15.0: its scissor saves GPU, not CPU) | 2.9 + 1.3 = 4.2 ms |
| Views, each culled by its whole frustum (about 1,800 draws) | about 3.5-3.8 ms |
| **Views, each culled by its frustum and half-plane (about 1,000-1,100 draws)** | **about 1.8-2.0 ms** |

The draws left are the per-draw cost RFC 0011 already works on. Draw lists (its phase 2) and drawing both views in one
draw (Alternatives) are the next levers, if the Deck still needs them.

## Proposal

```c
/* The most views btbh_gpu_window_scene_present_views draws (RFC 0014). */
#define BTBH_GPU_SCENE_MAX_VIEWS 2

/* BTBHGPUSceneView.flags. */
enum {
    /* A cut in this view alone (a player put elsewhere): under FSR 3 its pixels take no history this frame, and its
     * previous view-projection is this frame's. Without FSR 3 it does nothing. */
    BTBH_GPU_SCENE_VIEW_TEMPORAL_RESET = 1u << 0
};

/* One view of a scene presented by btbh_gpu_window_scene_present_views (RFC 0014): a camera and the part of the frame it
 * covers. 144 bytes on 64-bit: view_projection 0, right 64, up 76, position 88, clip 100, visibility 112,
 * shadow_visibility 120, lighting 128, flags 136, reserved 140. */
typedef struct BTBHGPUSceneView {
    float view_projection[16];
    float right[3];
    float up[3];
    float position[3];
    /* The view's half of the output, in output pixels (x right, y down from the top-left corner, before
     * post->mirror_horizontal): a pixel centre (x, y) is the view's where clip[0] * x + clip[1] * y + clip[2] >= 0.
     * All zero: the whole frame (one view only). With two views the halves should cover the frame once: each is widened
     * by half a render pixel, so a pixel on the line is drawn by both and the nearer surface shows. */
    float clip[3];
    /* Per batch, 0 or 1: drawn in this view (with the call's scene_visibility: both must let it in). NULL: every batch
     * the call's scene_visibility lets in. */
    const uint8_t *visibility;
    /* Per batch, 0 or 1: cast into this view's own sun and spot maps, when it has them (see lighting). NULL: the call's
     * directional_shadow_visibility. A batch with BTBH_GPU_SCENE_BATCH_NO_SUN_SHADOW still casts into no sun map. */
    const uint8_t *shadow_visibility;
    /* NULL: the call's lighting. Otherwise read only for:
     * - directional_shadow_view_projection (the sun map's matrix: equal byte for byte to an earlier view's, it shares
     *   that view's map; else the view draws its own);
     * - the spot light: spot_* but spot_shadow_is_omni, spot_occluder_*, its shadow (the map shared or own by the same
     *   rule);
     * - lamp_count, lamp_core_radius, lamp_position_reach, lamp_color_energy;
     * - point_light_count, point_lights.
     * Every other field is the call's: the view's isn't read. */
    const BTBHGPUSceneLighting *lighting;
    uint32_t flags;      /* BTBH_GPU_SCENE_VIEW_* bits; 0 as before */
    uint32_t reserved;   /* 0 */
} BTBHGPUSceneView;

/* What one view drew (btbh_gpu_window_scene_present_views' view_reports). */
typedef struct BTBHGPUSceneViewReport {
    uint32_t primitive_draw_count;   /* its draws in the opaque, transparent and refraction passes */
    uint32_t instance_count;         /* the instances those draws drew (after the host's culling and visibility) */
    uint32_t sun_map;                /* the view whose sun map it read (itself, or an earlier view with the same
                                        matrix); UINT32_MAX without the sun's shadow */
    uint32_t spot_map;               /* the same for the spot map */
    double cpu_opaque_order_milliseconds;
    double cpu_geometry_encode_milliseconds;
} BTBHGPUSceneViewReport;

/* Presents one frame of up to BTBH_GPU_SCENE_MAX_VIEWS views of the same batches and instances, each clipped to its half
 * of the frame (RFC 0014). Arguments as btbh_gpu_window_scene_present_instances_with_visibility, the camera replaced
 * by views. With one view whose clip is zero and whose visibility, shadow_visibility and lighting are NULL, the frame
 * is that call's with the view's camera, byte for byte.
 * - Shared: preparation, the upload, the shadow maps whose matrices the views share, the radiance cascades' field
 *   (phase 3), bloom, grading, anti-aliasing and upscaling.
 * - Per view: the opaque sort; the opaque, transparent and refraction passes; the screen-space passes that read the
 *   camera (SSAO, HBAO, SSR, the distance, the fog), each keeping its samples to its own half; its previous
 *   view-projection.
 * - Needs this release's scene vertex shaders and production_post_view.vert beside the host's post vertex shader.
 *   With older SPIR-V vertex shaders (no clip distance), two views fail with BTBH_GPU_INVALID_ARGUMENT naming the
 *   path; Metal libraries aren't checked.
 * - FSR 3: one history over the frame (phase 2); FSR 1 where the views' projections differ (report.upscale_mode).
 * - The window's frame only: it waits for the canvas as a present does. */
BTBHGPUResult btbh_gpu_window_scene_present_views(
    BTBHGPUWindow *window,
    const char *vertex_shader_path,
    const char *animated_vertex_shader_path,
    const char *fast_shadow_vertex_shader_path,
    const char *fast_shadow_fragment_shader_path,
    const char *shadow_vertex_shader_path,
    const char *animated_shadow_vertex_shader_path,
    const char *shadow_fragment_shader_path,
    const char *fragment_shader_path,
    const char *unshadowed_fragment_shader_path,
    const char *post_vertex_shader_path,
    const char *post_fragment_shader_path,
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const float *instances,
    uint32_t transient_instance_count,
    uint32_t logical_instance_count,
    const BTBHGPUSceneGeometryBatch *batches,
    const uint8_t *scene_visibility,
    const uint8_t *directional_shadow_visibility,
    uint32_t batch_count,
    uint8_t clear_red,
    uint8_t clear_green,
    uint8_t clear_blue,
    uint8_t clear_alpha,
    BTBHGPUSceneViewReport *view_reports,   /* view_count entries, or NULL */
    BTBHGPUSceneReport *report
);

/* btbh_gpu_window_scene_prewarm for btbh_gpu_window_scene_present_views: also makes the clipped post pipelines, the
 * views' own shadow maps (by their lighting, as the present decides) and their transmittance maps. The views'
 * matrices are read only to decide which maps they share. */
BTBHGPUResult btbh_gpu_window_scene_prewarm_views(
    BTBHGPUWindow *window,
    /* the eleven shader paths, as btbh_gpu_window_scene_prewarm */
    BTBHGPUShaderFormat shader_format,
    uint32_t render_width,
    uint32_t render_height,
    const BTBHGPUSceneView *views,
    uint32_t view_count,
    const BTBHGPUSceneLighting *lighting,
    const BTBHGPUScenePost *post,
    const BTBHGPUSceneGeometryBatch *batches,
    uint32_t batch_count,
    BTBHGPUSceneReport *report
);

/* Defined when btbh_gpu_window_scene_present_views and its structs exist. */
#define BTBH_GPU_SCENE_HAS_VIEWS 1
```

Where this differs from v2's proposal:

- **Per-view visibility, shadow visibility and lighting in the view.** v2 already builds a second lighting
  (`main.rs:8743-8762`). The pointer takes it as it is and reads only the parts that may differ.
- **A per-view report, out of `BTBHGPUSceneReport`.** The main report keeps its layout and its whole-frame meaning.
  - Per view: draws, which maps it read, its sort and encode CPU time.
  - GPU time per view isn't available: SDL's GPU API has no timestamp queries. Under `BTBH_GPU_COST_LOG` each view's
    geometry goes to a render pass of its own so the log can split them (a measurement mode, as the log's command
    buffer splits already are).
- **A cut per view** (`TEMPORAL_RESET`).
- **No `render_to_target` form.** Views compose the window's frame; a picture in a target stays one view.

**Zero values.** A zeroed view with a valid camera is the whole frame, every batch the call lets in, the call's
lighting. One such view is today's present.

**Relation to the old calls.** `btbh_gpu_window_scene_present_instances` and `_with_visibility` become one-view calls of
the same internal function. Nothing changes for their callers.

**Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, naming the view; the window left as it was):

- `views` NULL;
- `view_count` 0 or above `BTBH_GPU_SCENE_MAX_VIEWS`;
- a view's matrices or position not finite, or its view-projection not invertible (as `:9341-9347`, `:9395-9406`);
- `clip` not finite, or `clip[0]` and `clip[1]` both 0 with two views;
- a visibility byte above 1;
- unknown flags, or `reserved` not 0;
- two views with SPIR-V vertex shaders lacking `ClipDistance`, or without `production_post_view.vert`;
- every refusal of `_with_visibility`.

**Not refused, reported:**

- **The radiance cascades with two views** in phase 1 and 2: off for that frame. The report gives a new appended fallback
  reason, `BTBH_GPU_SCENE_RC_FALLBACK_VIEWS` (8, after `INVALID_CONFIGURATION`, `btbh_gpu.h:746`).
- **FSR 3 with two views** before phase 2, or with differing projections: FSR 1 (`upscale_mode`).

## Minimal example

```c
/* Player 2's half: where (p - centre) . dir > 0 (v2's Split::dir, screen pixels, y down). */
const float cx = 0.5f * (float)width, cy = 0.5f * (float)height;
BTBHGPUSceneView views[2];
SDL_zeroa(views);
for (int v = 0; v < 2; ++v) {
    const float sign = v == 0 ? -1.0f : 1.0f;
    memcpy(views[v].view_projection, cams[v].view_projection, sizeof(views[v].view_projection));
    memcpy(views[v].right, cams[v].right, sizeof(views[v].right));
    memcpy(views[v].up, cams[v].up, sizeof(views[v].up));
    memcpy(views[v].position, cams[v].position, sizeof(views[v].position));
    views[v].clip[0] = sign * dir_x;
    views[v].clip[1] = sign * dir_y;
    views[v].clip[2] = -sign * (dir_x * cx + dir_y * cy);
    views[v].visibility = view_visible[v];          /* culled by its frustum and its half-plane */
    views[v].shadow_visibility = view_casters[v];
}
views[1].lighting = &lighting2;                      /* its sun fit, lamps, its kaiju's spot map */

BTBHGPUSceneViewReport per_view[2];
BTBHGPUSceneReport report;
if (btbh_gpu_window_scene_present_views(window, /* the eleven paths */ ..., format, render_w, render_h,
        views, 2, &lighting, &post, instances, n, n, batches, NULL, shared_casters, batch_count,
        10, 11, 13, 255, per_view, &report) != BTBH_GPU_OK) {
    /* report.message */
}
/* The canvas draws the divider and the HUD over the frame, as now. */
```

In v2, `split_view.rs` is the one module that changes: `SplitTarget::render` and `composite` go, and the frame's present
takes both cameras.

## Implementation

### Phase 1: views in one present (FSR 1)

| File | Change |
| --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | The structs, the constant, the flag, the two functions, the macro, `BTBH_GPU_SCENE_RC_FALLBACK_VIEWS` |
| `shaders/gpu/production_model.vert.hlsl`, `production_animated_model.vert.hlsl` | `SV_ClipDistance0` from `surface_controls.yzw` (1 when all zero), outside `BTBH_RC_SOURCE_PROJECTION`. Five built files carry it |
| `shaders/gpu/production_post_view.vert.hlsl` (new) | `production_post.vert`'s triangle, clipped by a vertex uniform block |
| `production_hbao.frag`, `_hbao_denoise.frag`, `_hbao_resolve.frag` | Taps on the other side skipped (plane in `hbao_view.yzw`) |
| `production_ssao.frag`, `_ssao_denoise.frag`, `_ssao_resolve.frag` | The same (plane in `ssr_hit_options.yzw`); SSR stops at the seam |
| `src/gpu/btbh_gpu_scene.c` | Below |
| `CMakeLists.txt` | The new shader in the list (CHIMERA, Tyrian3D and v2 read it from there) |

In `scene.c`:

- The internal present takes `views` and `view_count`; the old entry points pass one view (`:12396-12548`).
- The frame part's fill (`:10625-11001`) becomes a function of the view and its lighting.
- Each phase's render pass (`:10572-10579`) holds every view in turn: the view's frame part, its sort or visibility,
  its own sun and spot map bindings (sampler slots 5, 7 and 8, `:11392-11408`; `btbh_gpu_scene_bind_lit_particle`'s),
  run state reset.
- **Shadow maps by slot.** Sun and spot maps are kept per slot, shared by equal matrices. Each slot has its own retained
  matrix and ready flag (today `[3]`, `:513-521`), and its own transmittance map. The update loop (`:9977-10105`) counts
  them in its ordinals, so they take turns.
- **Per-view screen passes as clipped draws** in one render pass each:
  - a variant of `btbh_gpu_scene_render_fullscreen` (`:8114-8211`) takes N uniform blocks and planes;
  - the pipelines are made with `production_post_view.vert`, lazily, and by the prewarm: SSAO x 3, HBAO x 3,
    distance, composite.
- **Per view kept in the runtime:** the previous view-projection, the opaque order, the planes.
- The cascades stay off with two views (the fallback reason).
- The seam's half-pixel overlap is added to `clip[2]` when the plane is converted.

**Cost, phase 1.** Estimates against v2's 4K High numbers (none measured):

| Part | Against the shared view's 10.5 ms |
| --- | --- |
| Pixels (opaque, transparent, HBAO, SSR, post) | About 0: each pixel is shaded by one view |
| View 1's geometry (vertices, draws on the GPU) | +0.1-0.3 ms culled by half-planes; +0.4-0.7 ms by whole frustums (v2's 8.4 ms less 7.3 of pixels less 0.7 of shadow) |
| Shadows | 0 with own maps taking turns at period 2 (one map a frame, as the shared view draws today); +0.7 ms at period 1; +0.1-0.3 ms for one shared map |
| Extra clipped draws, tap tests | +0.02-0.05 ms |
| **Frame** | **About 11-12 ms (+5-15 %), against 13.5 now and 18.9 whole**, both halves at the full render size |

Uncertain: whether two halves cost the pixels of one shared view. They should (same pixels, similar depth complexity),
but a fight in each half may draw more transparent effects than one fight.

**Measure:**
- **v2's bench:** `BTBH_COOP_SPLIT=1 BTBH_COOP_GAP=60`, its 4K High and Deck rows, against 0.15.0's split. Its
  `BTBH_SPLIT_LOOK` loses meaning (no second view to strip).
- **The cost log** (`BTBH_GPU_COST_LOG`) per group.
- **The frame bench:** one view through `present_views` against `_with_visibility` (the clip output's cost).

### Phase 2: FSR 3 over two views

- **The seam pass:** a full-screen pass after the refraction pass, blend max into the reactive mask (new
  `production_view_seam.frag`). It reads the velocity target and this and the previous frame's planes, and writes 1
  where a pixel's history belongs to another view, or to a view with `TEMPORAL_RESET`.
- The views open from and close into view 0's history (equal previous matrices count as one view).
- FSR 3 runs when the views' projection scales match (`:9390-9394`); otherwise FSR 1, reported.
- **Cost:** the seam pass, one read and one write per render pixel: about 0.05-0.1 ms at 4K. v2's "High, FSR 3 as its
  anti-aliasing" row (shared 14.33 ms, split now 17.72) should land near 15-16 ms.

### Phase 3: what phase 1 leaves at the seam

- **The radiance cascades with two views.**
  - Shared: emitters, sources and field passes, in one field the host places over both views.
  - Per view: resolve and reconstruct, dispatched over each half's bounding box with an early return; the compose as
    clipped draws.
  - v2 has them off in every preset (`PRESET_RC`, `v2-game/src/graphics.rs:288`), so this waits for a game that needs
    them.
- **Bloom's taps kept to their side**, if the bleed shows.
- **Refraction's displaced samples clamped** at the seam (three reserved words in the fragment block).

### Phase 4, only if the Deck needs it

- **Both views in one draw per batch:** instanced over the views, the view picked from the instance index. It halves
  draws only for batches both views see, which are few when the players are apart. It conflicts with RFC 0011's runs
  (the vertex shader would need each draw's first instance and count).
- **Or RFC 0011 phase 2's draw lists for the views.**

Each phase is opt-in and shippable alone. Phase 1 alone answers v2's first request but FSR 3; phase 2 its FSR 3.

### Risks

- **`SV_ClipDistance` is compiled, not run.**
  - Vulkan: SDL requires the feature at device creation, so it exists. But the first run is the smoke.
  - Metal: unverified until the Mac's `check.sh` and a run. Where a platform turns out to lack it, two views are
    refused there rather than drawn wrong.
- **Byte for byte for one view.** The five vertex shaders change. The expectation is identical images (the shader writes
  1 when the plane is zero); the identity runs prove it or not, as RFC 0013's UV rect did.
- **The present grows.** It is 3,100 lines in one function. Phase 1 must lift the frame-part fill and the pass bodies
  into functions first. The identity runs and `BTBH_GPU_SCENE_RUNS=verify` guard that refactor.
- **Seam artefacts** within a few pixels (FXAA, SMAA, EASU, the one-pixel overlap) sit under the divider. Bloom's and
  refraction's reach farther (phase 3).
- **v2's shadows look different** if it shares one map over parted views: 0.48-0.80 x the texels.
- **Retained maps at period 2:** moving casters' shadows update at half the frame rate. Today v2's maps redraw every
  frame.

## Compatibility

- **Versioning:** minor (pre-1.0: "additive → minor or patch"). New functions, structs, a constant, a flag, an enum
  value appended, a new shader file. No struct grows. No default changes.
- **Shaders.** Changed files must keep every image:
  - two vertex sources (five built files);
  - six screen-space fragment shaders.

  The seam pass (phase 2) is a new file.
- **Be The Bullet Hell v2 (Rust, bindgen):** regenerated bindings. `split_view.rs` changes; `Scene` takes a second camera
  and its lighting. Its shader list gains `production_post_view.vert` from `CMakeLists.txt`.
- **Be The Bullet Hell (v1, Swift), CHIMERA (Rust), Tyrian3D (C):** nothing to change. They rebuild their shaders.
  CHIMERA loads 14 named `.spv` files and needs no new one unless it draws views. Tyrian3D's metallib gains the clip
  output.

## Platforms and testing

- **Native** (`btbh_gpu_scene_order_test`): `BTBHGPUSceneView` 144 bytes with the offsets above;
  `BTBHGPUSceneViewReport` 32; the fallback reason's value; the plane conversion (pixel to clip space, the overlap) on
  the CPU.
- **GPU smoke, Windows/Vulkan** (new `btbh_gpu_scene_views_smoke`):
  - **Identity.** One view through `present_views` against `_with_visibility`: the flags, prewarm, world detail,
    scene-target, runs and lit-particle smokes' images, byte for byte, in the four identity modes (plain, shared meshes,
    runs off, runs verify). And the same images against 0.15.0's build, as RFC 0011 did against 0.9.0.
  - **A vertical split.** Two views with `clip` (1, 0, -W/2) and (-1, 0, W/2) against two `_with_visibility` frames of
    each camera, joined on the CPU at x = W/2.
    - Equal, outside a band, for passes without taps: no AO, no SSR, no bloom; fog and tonemap on.
    - With the HBAO and SSR on: equal beyond the HBAO's reach of the seam. Inside it, no tap from the other half: a
      checker of two depths on either side shows no darkening across.
  - **A diagonal split.** Every pixel belongs to exactly one view's colour or to the overlap band; none shows the clear
    colour.
  - **Shadows.** Equal sun matrices: one map drawn (telemetry). Different: two. At period 2 they take turns (one map a
    frame). The report's `sun_map` and `spot_map`.
  - **Refusals and fallbacks:** each refusal; the cascades' fallback reason; FSR 3 to FSR 1 (phase 1).
  - **Phase 2:** a seam turning over 20 frames under FSR 3. The motion-error debug view (`BTBH_DEBUG_MOTION_VECTORS=2`)
    shows no history from the other view. A view's cut marks its half alone.
- **macOS/Metal:** the Mac session's `check.sh` builds the metallibs, with the clip output. The smoke runs there with
  them; FSR 3's phase is Vulkan only until Metal has FSR 3.
- **Linux and the Deck:** through v2 (RADV has `shaderClipDistance`).
- **Games:** v2's split bench and its look pass's salience per half (coop.md "Readability"); CHIMERA 0 pixels
  different; Be The Bullet Hell its Swift suite.

## Alternatives

| Alternative | Why not |
| --- | --- |
| **Two calls, as now (0.14.2), with 0.15.0's scissor and kept runtimes** | The interim. The scissor saves the second view's pixels outside its half's box (most of the screen on a diagonal). The kept runtime saves its shadow redraws. It still prepares, uploads and encodes the scene twice (1.3 ms on the Deck preset) and runs post twice |
| **One shadow map over both views, always** | Density falls with distance without bound (0.48 x at 100 cells). The proposal allows it by passing equal matrices |
| **Per-view FSR 3 contexts** | Don't remove the seam's smear; double about 200 MB at 4K and FSR's GPU time |
| **Per-pixel view selection in the screen passes** (both views' matrices in each block, one draw) | Grows five uniform layouts and their shaders for every game. Clipped draws keep the blocks and change only the tap tests. It would suit compute passes, which phase 3 handles by boxes |
| **Stencil mask, depth pre-fill, discard, scissor alone** | See "Clipping a view to a half-plane" |
| **Viewports** (`SDL_SetGPUViewport` per view) | Only axis-aligned splits; v2's turns |
| **Both views in one draw** (instanced views) | Phase 4 at most: conflicts with RFC 0011's runs, needs every scene vertex shader to pick a view, and gains little while the views see different things |

## Decisions

Be The Bullet Hell v2 answered all 12 questions through its session on 2026-11-27, each as recommended or close to it:

1. **Shadows:** one sun matrix for both views while they are near (the split opening), each view's own fit once they
   part, the two maps taking turns. v2 measures both.
2. **Update periods:** v2 had never set them, so its maps redrew every frame. It now sets the sun's period to 2 while
   split (`btbh_gpu_window_scene_set_shadow_update_periods`, public since 0.15.0). The shadow group went 0.215 to
   0.183 ms at 4K High; a moving shadow lags its caster by one step every second frame (2-3 px walking).
3. **Culling:** v2 culls each view by its frustum plus its half-plane (`VP^T * (a, b, 0, c)`).
4. **What differs per view:** the sun's matrix, the spot light with its map (each kaiju its own shadow), the lamps and
   the point lights, as `BTBHGPUSceneView.lighting` proposes. Exposure, clouds and fog density are shared (one sun, one
   sky); v2's fog-of-war reveal is world-space. **Every fog term that depends on the camera (distance fog, height fog
   from the eye) reads each view's camera.**
5. **Anti-aliasing until phase 2:** SMAA medium on split frames.
6. **One projection:** both cameras always share the lens, near, far, aspect and angle; only the target and position
   differ. The FSR 1 fallback for differing projections stays as a guard.
7. **The seam:** v2's divider covers at least 2 render pixels each side at every preset, while it fades in too. A little
   bloom across the seam is fine for phase 1; v2 looks at bloom by night before phase 3.
8. **Radiance cascades:** off with two views, reported (`BTBH_GPU_SCENE_RC_FALLBACK_VIEWS`), until a game needs them.
9. **Two views only.** `reserved` and a later RFC carry more.
10. **The second view's size:** v2 drops its second view's render scale and cap when it moves to views; both halves
    render at the frame's render scale.
11. **The report:** per view, the draws, **the instances after culling** (`instance_count`, added to
    `BTBHGPUSceneViewReport` above) and the CPU encode; GPU per view through `BTBH_GPU_COST_LOG`.
12. **Opening:** a view that opens continues view 0's temporal history.

0.15.0 also settled what the interim gives: with the rect and v2's margin of 96 px, no seam shows; reflections that
leave the rect find nothing (0.15.0's Known), which views in one present remove.
