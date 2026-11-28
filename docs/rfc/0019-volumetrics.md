# RFC 0019: Lit volumetrics: one froxel grid for fog, fog of war and local volumes, lit by the scene's own lights

- **Status:** Draft (2026-11-28). v2 answered its 13 questions on 2026-11-28, each as recommended (see "Decisions
  (v2)"); CHIMERA's answers are pending
- **Requested by:** the owner, through Be The Bullet Hell v2's coordinator (2026-11-28): recent SIGGRAPH volumetric
  work as one shared feature for both games, for fog and mist, weapons and fog of war, "and surely more places". The
  owner confirmed the source to build on: IO Interactive's Smolder (SIGGRAPH 2026). The Volumetric Gaussian Primitives
  paper (SIGGRAPH 2025) is a reference only
- **Affects:** Be The Bullet Hell v2 and CHIMERA adopt it, opt-in. Be The Bullet Hell (v1, Swift) and Tyrian3D have
  nothing to change. A window that never calls `btbh_gpu_window_scene_set_volumes` makes no new pass, pipeline or
  texture and draws byte for byte as before. Every game rebuilds the shaders with the C core, as every release:
  - four files change their SPIR-V and MSL but not their pictures: `production_particle_lit.frag`, its
    `_2d_shadows` and `_unshadowed` siblings, and `production_model_unlit.frag`. Each gains a slot and a branch that is
    not taken without volumes;
  - three shaders are new, with two includes (listed under "Phase 1").
- **Issue:** none yet. Both games answer the open questions at the end through their sessions
- **Depends on:**
  - Phase 1 depends on nothing.
  - Phase 2 draws into RFC 0013 phase 2's particle transmittance map (0.14.0) and reads RFC 0010's cascade field.
  - It works with RFC 0014's views (0.16.0), and with RFC 0015's frame thread (accepted), whose rule ("copy at the
    call") the setter follows.
  - If CHIMERA agrees, it supersedes RFC 0002 part A (analytic height fog). Part B (water) is untouched.
  - RFC 0017 (HDR output) has no commit yet on `rfc/0017-hdr-output`. Volumes write scene-linear light before the tone
    map, so they don't wait for it.
  - RFC 0018 (distortion) could later add heat haze around fire volumes.
- **Sources:**
  - **This repository:** master `c20c4d0b` (v0.16.1); every line reference below is that commit's. Short names:
    `scene.c` for `src/gpu/btbh_gpu_scene.c`, `internal.h` for `src/gpu/btbh_gpu_internal.h`, `btbh_gpu.h` for
    `include/btbh_gpu/btbh_gpu.h`.
  - **SDL:** 3.4.2's `SDL_gpu.h`.
  - **v2:** BTBH `6da975c4b`, paths under `v2/`.
  - **CHIMERA:** RTS `c1091ded`, paths under `RTS/`, read only.
  - **Smolder:**
    - A. Mueller, "Smolder: Real-Time Volumetric Effect Rendering in Glacier and 007 First Light", Advances in Real-Time
      Rendering in Games, Part II, SIGGRAPH 2026: <https://advances.realtimerendering.com/s2026/index.html>.
    - The slides with speaker notes: <https://advances.realtimerendering.com/s2026/content/AlexanderMueller_Smolder_Siggraph26.pdf>.
  - **Volumetric Gaussian Primitives:**
    - J. Condor, S. Speierer, L. Bode, A. Božič, S. Green, P. Didyk, A. Jarabo, "Don't Splat your Gaussians:
      Volumetric Ray-Traced Primitives for Modeling and Rendering Scattering and Emissive Media", ACM TOG 44(1), 2025,
      presented at SIGGRAPH 2025: <https://doi.org/10.1145/3711853>, <https://arxiv.org/abs/2405.15425>.
    - Project page: <https://arcanous98.github.io/projectPages/gaussianVolumes.html>.
    - Code: <https://github.com/facebookresearch/volumetric_primitives>.
    - SIGGRAPH 2025 session list: <https://www.kesen.realtimerendering.com/sig2025.html>.
  - **Background:**
    - B. Wronski, "Volumetric Fog", SIGGRAPH 2014 Advances:
      <https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf>.
    - S. Hillaire, "Physically Based and Unified Volumetric Rendering in Frostbite", SIGGRAPH 2015 Advances:
      <https://advances.realtimerendering.com/s2015/index.html>.
    - Unreal's heterogeneous volumes:
      <https://dev.epicgames.com/documentation/en-us/unreal-engine/heterogeneous-volumes-in-unreal-engine>.
    - Unity's 6-way lit smoke: <https://unity.com/blog/engine-platform/realistic-smoke-with-6-way-lighting-in-vfx-graph>.

## Need

### The request

- **What the owner wants:** lit volumes, integrated with the lighting, transparency and fog, shared by both games.
- **Named uses:** fog and mist, weapons, fog of war.

### Be The Bullet Hell v2

You play a kaiju destroying cities, top-down, in a bullet hell.

**Scale.** One world unit is one cell and a storey is 0.35 units (RFC 0010, "Need").

**What v2 wants volumes for:**
- **Biome atmosphere:** jungle morning mist, a monsoon flood's rain haze, desert dust storms, ice blizzards, night fog in
  lamp light.
- **Weapons and hazards:**
  - the Gas Leak mutator's clouds: "gas in the streets; fire or an explosion sets off a 3-cell explosion that spreads
    through the cloud" (`v2/docs/mutators.md:29`). Today they are flat green tiles;
  - smoke from fires and wrecks;
  - a flamethrower's and plasma's volume, explosions lit from inside, steam vents, collapse dust, the kaiju's breath.
- **Fog of war,** on in every run since the owner's play-tests (`v2/crates/v2-game/src/fog.rs:1-4`). The request asks
  for "a real volume over the unrevealed city that the kaiju's presence clears". Egypt's twist is the khamsin, "dust
  that settles back" (`v2/docs/night-round-2026-11-24.md:95`, `:369`).
- **Lighting:** the sun with its shadow, the 40 street lamps, and the radiance cascades (RC) field.

**Readability.** The game sets every source's density itself, because it gates enemy salience.

**Its fog of war today** (`v2/crates/v2-render/src/fog.rs:1-59`) is render-side only:
- A reveal value per cell lives in a texture, whose alpha is the fog's density, with a 32-cell margin.
- It is drawn as five see-through sheets from 0.25 to 4.6 cells up, to the tallest roof's height. Each sheet reads the
  texture where the view's ray meets the ground. That way the clear ground on screen is exactly the revealed ground,
  and a roof takes only the sheets above it.
- The reveal uploads at most every 0.1 s.
- It costs the GPU 0.005-0.03 ms at the Deck preset.
- The city, props, lamp pools and enemy bodies take the fog. What the fight draws after it doesn't: shots, weapons,
  hazards, telegraphs, blasts and pickups.
- Enemy salience was measured with it off and on (`fog.rs:26-35`).

**Budgets** (v2's perf round, `v2/docs/perf-2026-11-27.md:33-37`, `:139-182`; this PC: RTX 3070 Ti):
- **4K High:** render scale 1, with FSR 3 as the anti-aliasing. GPU 11.8-15.1 ms, of which post is 4.0-4.6 ms (mostly
  FSR 3 at full 4K).
- **The Deck preset:** the "Steam Machine's stand-in", 1280 x 800 at scale 0.75 (960 x 600 rendered), FSR 1, low
  shadows.
  - GPU 1.2-1.5 ms in play and 1.9 ms on the title.
  - The coordinator puts the whole frame at about 1.6-2.2 ms.
  - Nothing has run on the device itself yet.

### CHIMERA

An RTS after StarCraft II's co-op missions.

**Camera and map** (`RTS/crates/chimera-platform/src/camera.rs:40-61`):
- 28° lens, 56° pitch, 32 m of ground across the screen.
- The wheel zooms 0.6-1.6x, and "Wide zoom" reaches 142 m across. Strategic zoom shows the whole map, tilting to 78°.
- Maps are about 192 m square (`RTS/docs/v2/GDD.md:399`), in metres, on height levels 3.2 m apart. Abyss floors lie
  about 16 m down.

**Its fog of war today:**
- **The simulation:** 1 m cells, updated at 10 Hz. Each team holds a "visible now" bit and an "ever seen" bit per cell
  (`RTS/crates/chimera-sim/src/vision.rs:1-27`). Vision decides targeting and is lockstep state.
- **How it is drawn** (`RTS/crates/chimera-render/src/fog.rs:1-17`): a CPU mesh, a vertex a metre, re-uploaded at
  30 Hz.
  - Clear where units see. A cool dusk at 0.48 over ground seen before. Near-black at 0.8, with a drifting cloud, over
    ground never seen.
  - Drawn with `DEPTH_ALWAYS` and never steeper than the eye's line, so "every surface in the fog dims alike", cliff
    faces included.
  - It costs about 0.12 ms (`RTS/docs/v2/napredak/piksel.md:106`) and has no motion vectors under FSR 3.
- **The look the docs ask for:** SC2's dark teal: unexplored ground dark, but not dead black
  (`RTS/docs/v2/istrazivanje/mape-sc2.md:378`).

**Wanted, not built yet:**
- height fog, already asked for in RFC 0002 (`RTS/docs/v2/GDD.md:452`);
- low mist over the canal; mist, spray and volumetric light by the waterfall (`RTS/docs/v2/sadrzaj/misija-rez.md:748-751`);
- the Eclipse's *Shroud*, an 8 m dome of darkness (`RTS/docs/v2/sadrzaj/kimera-prva.md:306`);
- the Wandering Storm mutator, 9 m, today drawn as rings only;
- mutators in its catalogue: Fog Banks, Smoke Spite, the Gas Thrower and Gas Pods
  (`RTS/docs/v2/istrazivanje/mutatori.md:452`, `:653`, `:772`, `:774`).

**Faked today:** three abyss mist sheets, and transparent "puff" quads for lava steam and waterfall mist.

**Readability:** "Smoke thin and soon gone: a battle's units must stay readable through it"
(`RTS/crates/chimera-render/src/vfx.rs:641`).

**Platform and budget:**
- Windows and Vulkan only, with no handheld target.
- At 1440p its frame is about 7.5 ms on this PC, of which "Atmosphere" is 0.9 ms (`RTS/docs/v2/napredak/piksel.md:39-41`).
- CHIMERA uses neither RC nor lit particles.

### What the two games share

| Use | Game | Shape of the medium |
| --- | --- | --- |
| Fog of war | both | A 2D field the game writes per cell (1 cell, 1 m) over the map, as deep as the tallest roof (v2) or the abyss (CHIMERA). Read along the eye's ray so that cliffs and roofs dim alike. Its own colour first, lit later |
| Height and ground mist, abyss and canal mist, morning mist | both | Thickening below a height, with drifting noise |
| Rain haze, blizzard, dust storm, khamsin | v2 | Uniform or field-shaped, noisy, wind-driven |
| Smoke, steam, dust, gas clouds | both | Many small soft shapes or a street-shaped field, lit by the sun, the sky and the lamps |
| Explosions, flamethrower, plasma, breath | v2 (CHIMERA's blasts too) | Small emissive shapes, lit from inside, fast |
| Night fog in lamp light | both | Any of the above lit by the lamps and point lights (and, for v2, RC) |

## What exists today

### Fog

- **The distance fog and the world-edge fade** live in the HDR composite (`shaders/gpu/production_hdr_composite.frag.hlsl:40-72`).
  - The opacity is `1 - exp(-((view depth - start) * density)^2)`, capped at `fog_maximum_opacity`, lerped toward one
    colour.
  - It is applied at the depth of the opaque surface behind each pixel. Transparent pixels write no depth.
  - The background is skipped (`:45-48`). The scene pass clears it to the fog colour instead (`scene.c:11935-11948`).
- **The settings** are `BTBHGPUScenePost`'s `fog_*` fields (`btbh_gpu.h:1681-1708`). The composite runs when fog, bloom or
  the vignette is on (`scene.c:10301-10305`). Its uniforms are at `scene.c:253-261` and `:13467-13509`. With two views
  it runs per view through `production_post_view.vert` (`:13511-13535`).
- **Lit particles fog themselves at their own depth** (`shaders/gpu/production_particle_lit.frag.hlsl:507-525`, `:661-678`).
  They use a made-up colour that the composite's fog at the opaque depth then turns into the right one. That trick
  works only because the fog is a lerp toward a constant.
- **Height fog doesn't exist.** RFC 0002 part A, a draft, proposes it analytically in the composite.

### Lights a volume could take

All of these are in `BTBHGPUSceneLighting` (3,340 bytes, one reserved byte left at 3,339:
`Tests/btbh_gpu_scene_order_test.c:182-184`):

- **The sun:** direction, colour and energy. Its shadow is one map: 2048² by default, 256-8192 through
  `btbh_gpu_window_scene_set_directional_shadow_size` (`btbh_gpu.h:2641-2648`; `scene.c:28`).
  - It isn't cascaded. v2 fits the matrix to the ground its camera sees (RFC 0014, "Need").
  - `set_shadow_update_periods` redraws it every n frames, and receivers then read the retained matrix
    (`btbh_gpu.h:2649-2674`).
  - The sun's light is dimmed by the cloud layer's shadow (`btbh_cloud_light`).
- **The fill:** a secondary directional light with a 512² map.
- **The spot or omni light:** one at a time, with a 1024² map or a 6 x 512² cube (`scene.c:29-31`).
- **Street lamps:** `lamp_*`, up to 40, each lit through the BRDF (`btbh_gpu.h:1510-1517`). v2 uses all 40 at night, per
  view.
- **Point lights:** up to 32, shadowless, eased under `point_light_ceiling` (`btbh_gpu.h:1529-1537`). Explosions' flashes
  use them; CHIMERA adds one per blast, at most 32.
- **The sky:** nine SH terms and a procedural sky.
- **The lit particle shader holds a ready copy of all of this for a point in space** (`production_particle_lit.frag.hlsl`):
  - the sun through four comparison taps (`:272-302`), the fill (`:333`), the spot and omni (`:358`);
  - the lamps (`:423`), the point lights (`:450`), RC at the point's x and z (`:482-505`);
  - the sky's SH (`:191`), the clouds (`:217`), the particle transmittance map (`:607`).
  - A volume's lighting is the same list with a phase function in place of the wrap.

### Shadows that suit volumes

- **The particle transmittance map** (RFC 0013 phase 2, 0.14.0; `btbh_gpu.h:1595-1610`):
  - one RGBA16F map in sun space, at half or a quarter of the sun map's size;
  - it holds the optical depth τ, τ times depth, and the nearest depth, blended order-independently;
  - receivers read it as a slab: `T = exp(-τ · saturate((d - front) / (back - front)))`;
  - it is drawn every frame, after the shadow maps, with the matrix the receivers read (`scene.c:11809-11835`);
  - with `particle_shadow` 2 the opaque surfaces read it too, so a smoke column shadows the street.
- That is a cheap transmittance function in light space, the role Smolder's moment shadow maps play (below).

### The radiance cascades

- **The field is 2D.** It is a four-quadrant atlas of the light arriving at each texel from four directions, at street
  level (RFC 0010, "What the code does today").
- **Lit particles read it at their x and z**, scaled by `particle_cascade_origin.w`.
- **It is off on frames of two views** (`btbh_gpu.h:3000-3002`), and so is FSR 3.

### Transparency, particles, refraction

The frame's order (`scene.c:12106-12440`, RFC 0013 "The transparent pass"):

1. The opaque pass.
2. HBAO, RC and the SSAO/SSR resolve.
3. The distance pass, which writes R32F at the render size when soft or lit particles or refraction need it
   (`:12350-12398`).
4. The refraction copy (`:12399-12425`).
5. The transparent pass, which writes the colour and FSR 3's reactive mask (`:12426-12438`).
6. The refraction pass.
7. Bloom (`:13384-13466`), on the image before fog.
8. The HDR composite.

What each kind of transparent batch can read:
- **Lit particles** have their own shader with ten of SDL's sixteen sampler slots.
- **The fast unlit shader** that draws most unlit transparent batches uses three slots
  (`production_model_unlit.frag.hlsl:1-7`).
- **The full scene shader** uses all sixteen. `SOFT_PARTICLE` and refracting batches draw with it (RFC 0013, "What the
  last slot costs").

### Post, FSR 3, HDR

- **Bloom** reads the image after the transparent pass and before the composite.
- **FSR 3** reads depth, the opaque pass's motion vectors and the R8 reactive mask (`scene.c:4793-4840`, `:13779`).
  Transparent batches write `alpha · temporal_reactive_scale` into the mask (CHIMERA 0.3).
- **At the Deck preset v2 runs FSR 1.** No temporal filter runs at all there.
- **Everything before the tone map is scene-linear RGBA16F**, which is where RFC 0017's HDR output will start.

### Views

- **`btbh_gpu_window_scene_present_views`** (0.16.0) draws two cameras, each clipped to a half-plane.
- **Per view:** the opaque and transparent passes, the screen-space passes, the distance pass and the composite.
- **Shared, or per view:** a view's own sun and spot maps, and its own transmittance map (`scene.c:11782-11808`).
- **Per view's lighting:** lamps, point lights and the spot.

### Compute, formats and slots

- **Compute shaders:** RC and FSR 3 already run as compute, at `[numthreads(8, 8, 1)]`. They are found beside the post
  shaders (`scene.c:7640`).
- **No shader in the renderer uses wave intrinsics or `groupshared`** (grep of `shaders/gpu/`).
- **Formats:** SDL 3.4.2 guarantees R16G16B16A16_FLOAT for sampling, colour targets and storage on every backend
  (`SDL_gpu.h`, `SDL_GPUTextureFormat`'s notes). 3D textures exist (`SDL_GPU_TEXTURETYPE_3D`). Whether a 3D texture
  takes storage use is a query (`SDL_GPUTextureSupportsFormat`).

### Cost log, prewarm, lazy creation

- **The cost log** (`BTBH_GPU_COST_LOG=1`) splits the GPU frame into groups (`internal.h:621-639`): upload, shadow,
  main, rc_source, rc_field, rc_screen, ssao, transparent, post, ui, and the two view-1 groups. Pass frames cut ssao and
  post into steps (`internal.h:653-678`).
- **The prewarm makes everything a frame needs.** The lazy creation log names what a frame made that a prewarm would
  have (`btbh_gpu.h:3089-3169`). v2's rule: no pipeline after frame 1.

## The two sources

### Smolder (IO Interactive, SIGGRAPH 2026)

**What I could read:**
- The course page lists the talk: Alexander Mueller, Advances in Real-Time Rendering in Games, Part II, 21 July 2026,
  with a PDF and a PPTX.
- I read the PDF's text: the speaker notes for all 113 pages, extracted with `pdftotext`.
- I read the performance table on page 107, which is an image.
- I did not inspect the other slides' images (diagrams, screenshots) or the 265 MB PPTX.
- No video is listed, and I found no IOI blog post. Secondary pages (fan wikis, a benchmark site) add nothing technical.

Everything below is from the notes unless marked as inference.

**What it is.** A volumetric effects framework for Glacier, shipped in *007 First Light*. It is meant to be generic and
fully integrated with the lighting, transparency and volumetric fog, so that volumes are ordinary VFX. Glacier already
had froxel fog. Smolder adds local volumes on top of it.

- **The abandoned first attempt** was GPU particles as density spheres, software-rasterized in compute into moment-based
  OIT at several mip scales. It failed:
  - moderately sized particles each covered hundreds to thousands of pixels, and a large effect had hundreds of
    thousands of them;
  - the result was dotty or blurry, with depth artefacts at the mip seams;
  - shadows cost too much, and effects were hard to author.
- **The shipped representation:** effects simulated offline (EmberGen, Houdini), exported as OpenVDB and packed into a
  one-level sparse grid.
  - Bricks are 4³, stored as 5³ with duplicated borders. Voxels are 4 bits, with a min and max per brick.
  - The brick index is 16 bits, and an occupancy bitmask is walked for empty space.
  - Each asset carries a tiny prebaked GI-occlusion grid.
  - Animated frame sequences, over 450 of them. NanoVDB was rejected for its register pressure.
- **Rendering:** a per-pixel compute raymarch in 8x4 or 8x8 groups, heavy on wave intrinsics.
  - Each group builds ray segments from up to four overlapping volume bounds, sorted in-wave (a ballot rank sort).
  - The step follows the voxel size, and the march exits early when transmittance reaches about 0.
- **Lighting:**
  - Sun cascades (3 covering 25 m, a static scrolling one over 200 m, a low-res global map).
  - A new clustered light list with per-use bitmasks, culled again by the segments (7-8 lights per cluster in their
    example). Projector textures.
  - **Volumetric shadows** are MBOIT-style moments: 4 x 16-bit, from 64² to 1k², omni lights in one octahedral map.
    - Opaque, transparent and froxel fog all sample them, with one bilinear tap and an early-out on the 0th moment.
      This costs about 30 % of a normal shadow lookup.
    - Beer shadow maps were tried and self-shadowed too poorly.
    - Every light gets volume shadows by default. Lights that touch no volume are culled, leaving 10-20, rendered in
      async compute, which the speaker calls about free on consoles.
  - **GI** comes from DDGI probes, sampled at one position per volume, two directions, times the baked occlusion.
  - One generic phase function. No multiple-scattering approximation is mentioned.
  - **Colour and emission** are listed as future work.
- **Integration as shipped,** a "hacky" single integration depth:
  1. The froxel fog is applied to opaque as before.
  2. OIT moments are generated.
  3. Smolder marches and keeps a depth where it becomes fairly opaque, jittered by blue noise.
  4. Fog up to that depth is taken out of the background, Smolder's transmittance is applied, and the fog is put back.
     Smolder's light is attenuated by fog and transparency at that depth.
  5. Its moments are added to the OIT moments before the resolve.

  Their ideal, unified transmittance across all three systems (as they believe Remedy did in Alan Wake 2) was not built.
- **Temporal:** no filter of its own; per-pixel blue noise and the frame's TAA or upscaler.
  - FSR 3 needed Smolder in its reactive mask; without it the ghosting was heavy.
  - DLSS needed strictly per-pixel noise.
- **Resolution:** half and quarter resolution were tried and dropped: poor upsampling, ghosting, and broken per-pixel
  noise. Instead a wave drops its lighting rate to half, then a quarter, once transmittance across the wave falls under
  a threshold, sharing samples through LDS. This saved about 50 % in heavy smoke.
- **Cost:** at settings equivalent to the PS5's, without the async shadow cost, scaling almost exactly linearly with
  the pixel count.
  - The table on page 107, three scenes each:

    | GPU, resolution | Scenes 1 / 2 / 3 |
    | --- | --- |
    | RTX 3060, 1080p | 1.23 / 1.08 / 3.46 ms |
    | RX 6700 XT, 1080p | 0.78 / 0.64 / 1.97 ms |
    | RTX 5060 Ti, 1080p | 0.64 / 0.63 / 1.82 ms |
    | RTX 5060 Ti, 4K | 2.41 / 2.01 / 7.16 ms |

  - Memory reached a few hundred MB for resident animations. A half-resolution mip from the packer cuts it by
    80-85 % on Series S and the lowest PC settings.
  - It shipped on Steam Deck and Series S, but the talk gives no timings for them.

**What fits this renderer:**
- the layering: a froxel grid for the big media, local volumes on top, and transparency taking both at its own depth;
- lighting through the frame's own lights and shadow maps;
- one light-space transmittance map read by opaque, transparent and fog alike, which this renderer already has;
- GI from the existing probe field at a coarse rate;
- the reactive mask for FSR 3;
- a lighting rate lowered where little shows through.

**What doesn't fit, as built:**
- **The asset pipeline.** VDB simulation and an offline packer. Neither game has one, and Smolder's memory is
  hundreds of MB.
- **Wave intrinsics everywhere.** SDL GPU doesn't report subgroup support, and the MSL path is untested here.
  `groupshared` does the same jobs portably.
- **No temporal filter of its own.** That relies on a temporal AA: v2's Deck preset runs FSR 1, which accumulates
  nothing, so per-pixel noise would stay noise there.
- **Per-pixel cost at 4K native** (v2's High). By analogy with the 5060 Ti row (a GPU of about the 3070 Ti's
  class; my inference), 2-7 ms. That is too much for the base layer, which must also draw a fog of war over most of the
  screen in every run.

### Volumetric Gaussian Primitives (Condor et al., TOG 2025, SIGGRAPH 2025)

I read the paper from its PDF, linked from the project page. I found no other SIGGRAPH 2025 paper of that title; this is
the one the name points to.

- **The primitive:** an un-normalized kernel (a Gaussian truncated at 3σ, or Epanechnikov) with one cross-section,
  emission and phase.
  - Extinction is `Σ σᵢ Kᵢ(x)`, so overlapping primitives add.
  - Transmittance along a ray is closed-form (erf for the Gaussian).
- **Rendering:** a volumetric path tracer on OptiX hardware ray tracing, over ellipsoid shells meshed as icospheres.
  Segments come from re-casting at each hit: no sorting, exact order.
  - Lit results take minutes: a cloud of 2.6k-16.2k primitives at 1024 spp on an RTX 3080 took 11.7-26.3 minutes.
  - The real-time numbers are emission-only radiance fields: 28-53 fps on an RTX A6000.
- **Assets** are fitted from VDB grids by optimization, from 15 minutes to several hours. They are compact: 107-639
  KB against a 480 MB VDB.
- **Limits:** a degenerate BVH under big overlapping kernels, lost sharpness, density-only variation.

**Fit (my inference):** poor as a base. It needs hardware ray tracing, which SDL GPU doesn't expose, its lighting is
offline, and its assets come from an optimizer, not an artist.

**What this RFC takes from it:**
- the Gaussian kernel as a volume shape that adds where volumes overlap;
- its closed-form line integral, for phase 2's sun-space optical depth and the analytic tier;
- the argument that a few analytic primitives can stand in for a heavy grid.

Smolder's own failed first attempt (analytic spheres, rasterized) is the warning against going further down that road.

### Techniques considered

| Technique | Strength | Cost and limits here | Used |
| --- | --- | --- | --- |
| Froxel grid (Wronski 2014, Hillaire 2015): a camera-aligned 3D grid, lit per froxel, integrated front to back, applied with one 3D lookup | Resolution-independent cost; big and soft media; many lights; one lookup per opaque or transparent pixel; its own temporal filter | Low xy resolution, so halos at thin silhouettes and soft local detail. Wronski's 160x90x64 grid cost about 1.1 ms (the notes don't name the platform; the game shipped on 2013 consoles, about a Deck's class) | Phase 1, as the base layer Smolder sits on |
| Smolder's per-pixel raymarch of local volumes | Crisp close-up detail; exact segments | Cost scales with pixels (2-7 ms at 4K on a 5060 Ti); needs a temporal AA; VDB assets | Phase 3, as hero volumes at High, procedural first |
| Analytic media in the composite (RFC 0002 A) | About free; exact per pixel | Unlit; height fog and ground-read fields only | Phase 1's ANALYTIC tier and fallback |
| Particles lit as volumes (RFC 0013's lit particles; Frostbite's particle voxelization) | Already built; high frequency; cheap | Billboards, not volumes. Voxelizing them into the grid needs image atomics, which SDL GPU lacks for floats | Kept as the high-frequency layer, inside the volumes (`IN_VOLUMES`) |
| 6-way lit flipbooks (Unity) | Lit-sprite cost, baked lighting | An asset pipeline per effect; flat up close | A game's choice, as lit particles |
| Volumetric Gaussian primitives | Compact, exact, adds on overlap | Hardware RT, offline lighting, fitted assets | The Gaussian shape and its closed form only |
| Sheets, as today (v2's five, CHIMERA's lid) | Cheap and tuned | No light, no volume, CPU work, no motion vectors | Replaced, if the games agree |
| A 3D fog-of-war texture | Any shape | Both games' vision is 2D; 3D uploads cost (v2's texture update submits its own command buffer) | No: a 2D field and a height profile |

### Choice

**Smolder's architecture, built bottom-up for these budgets.**
- **Phase 1** is the layer Smolder assumes, which this renderer lacks: a froxel grid.
  - It holds the global and height fog, two game-written fields (fog of war, gas) and up to 128 analytic local volumes.
  - It is lit by the scene's own sun and shadow map, sky, lamps and point lights, with emission.
  - It is applied to the opaque image before the transparent pass, and transparent batches can take it at their own
    depth.
- **Phase 2** adds Smolder's shared light-space transmittance (the particle transmittance map, drawn by volumes too), its
  coarse GI (RC) and the remaining lights.
- **Phase 3** is Smolder's core, per-pixel raymarched hero volumes, for High, once phase 1's numbers are in.

## Proposal

### The pipeline

Every frame that has volumes (steps 2 and 3 run once per view):

1. **Shadow group, unchanged.** The sun, fill and spot maps, and the particle transmittance map. From phase 2 the
   volumes flagged `SUN_SHADOW` draw into that map too.
2. **Volume group (new):** right after the shadow maps, before the opaque pass. It reads no frame target.
   - **Froxel pass** (compute, `production_volume_froxel.comp`). Per froxel:
     1. a jittered sample position;
     2. the density of every medium there: global, height, the fields, and the local volumes whose froxel range holds
        it;
     3. their scattering-weighted albedo, phase and emission;
     4. the lighting;
     5. the temporal blend.
     It writes in-scattered light per unit length and extinction into an RGBA16F 3D texture.
   - **Integrate pass** (compute, `production_volume_integrate.comp`). Front to back along each column, energy-conserving
     (Hillaire 2015). It writes in-scatter and transmittance, `(S, T)`, per slice, plus the local volumes' own opacity
     (R16F, for the reactive mask).
3. **Apply pass (new),** after the distance pass and before the refraction copy and the transparent pass.
   - A fullscreen draw (`production_volume_apply.frag`; `production_post_view.vert` with views) reads depth and fetches
     `(S, T)` trilinearly at that depth.
   - It blends `dst · T + S` over the opaque image (colour `ONE`, `SRC_ALPHA`, with T in alpha).
   - It writes the reactive mask from the local opacity.
   - The background takes the grid's whole column.
4. **Transparent pass.** An unflagged transparent batch draws over the volumes, as v2's fight effects draw over its
   fog of war today. A batch with `BTBH_GPU_SCENE_BATCH_FLAGS2_IN_VOLUMES` fetches `(S, T)` at its own depth.
   - Alpha blending outputs `T·X + S` with its alpha.
   - Additive blending outputs `T·X`.
   - The result is exact for any number of layers drawn back to front: the light between the camera and the puff is
     added, and what it hides is taken away.
5. **Bloom, the HDR composite and the rest, unchanged.** Bloom sees the volumes' light, so lamp halos and fire cores
   bloom. The distance fog still applies in the composite at the opaque depth (see "Integration").

Under the ANALYTIC tier, steps 2 and 4's fetch are replaced by a closed-form evaluation per pixel (below).

### The media

All positions and lengths are world (render) units; densities are extinction per unit.

- **The global medium:**
  - a uniform density, plus an exponential height fog `height_density · exp(-height_falloff · (y - height_base))`, with
    one albedo, phase g and emission;
  - its sun-ward transmittance is closed-form, so it self-shadows the sun's light at no cost.
- **Two fields** (`fields[2]`), each a 2D texture the game writes, over a world x/z rectangle:
  - **The texture:** an RGBA8 texture made with `btbh_gpu_window_texture_create_rgba8_ex` and updated with
    `btbh_gpu_texture_update_rgba8`. One channel is read, so one texture can carry the fog of war in one channel and the
    gas in another. `INVERT` reads `1 - texel`, so a "revealed" texture works as is. A NULL texture with a density is a
    uniform field inside the rectangle: a mist slab or a blizzard with noise.
  - **The height profile:** from `bottom` to `top`, fading over `top_softness` above, nothing below. By default it is
    even through the slab. `profile_exponent` moves the density toward the top (`h^k`), so the fog of war can read as a
    lid over the blocks, as v2's sheets do, rather than a haze that thins on the towers.
  - **Where the texture is read:**
    - **`ALONG_VIEW`:** where the eye's ray meets the plane `y = anchor_height`. Every sample along a pixel's ray takes
      that one value, so the clear ground on screen is exactly the revealed ground, and a roof takes only the fog above
      it. This is what v2's parallax sheets and CHIMERA's lid each built by hand. The froxel grid's density still follows
      `y`, so a roof takes less than the street.
    - **Otherwise:** at the sample's own x and z, for a real volume such as a dust storm that walls off where it ends.
  - **Noise:** 3D noise at the sample's own position, offset by the host (`noise_offset`: wind, drift). The renderer
    keeps no time.
- **Local volumes** (`volumes`, up to 128): an ellipsoid, a box or a Gaussian, at a centre, with three half-axis
  vectors (any orientation and scale).
  - **Shapes:**
    - `ELLIPSOID` fades over `edge_softness` toward its shell.
    - `BOX` fades over the same share per axis.
    - `GAUSSIAN` is `exp(-4.5 r²)` inside the shell, which is 3σ (the VGP kernel).
  - **Density and colour:** the core's density, albedo, HG phase g, and emission with `emission_falloff` (0 even, 1
    strongest at the core and none at the shell: an explosion lit from inside).
  - **Noise:** strength, frequency and offset.
  - **`light_scale`** scales the light it scatters, not its emission. Readability without touching opacity.
  - **Emission** is the radiance an opaque, black core shows by itself (the source term is `density · emission`), so
    a thin volume glows less than a thick one.
  - Overlapping volumes add, as VGP's kernels do. Albedo and g are weighted by each one's scattering.
  - **Flags:**
    - `UNLIT`: no light, emission only;
    - `NO_LOCAL_LIGHTS`: sun, sky and RC only;
    - `CLEARS`: inside its shape it removes the global medium and the fields, its own density only shaping it. The
      kaiju's presence, a vent parting mist, a breath through fog;
    - `SUN_SHADOW`: phase 2;
    - `HERO`: phase 3.
  - **Culling:** the CPU computes each volume's froxel box per view from its bounding box. The froxel pass tests the
    box first, so a froxel pays only for the volumes that reach it.

### Lighting

Per froxel, at its jittered sample, once for the mixture: scattering-weighted albedo and g, then `light_scale`.

| Light | Phase 1 | Phase 2 |
| --- | --- | --- |
| Sun | Its energy and colour × the sun map's visibility (one comparison tap at LOW and MEDIUM, four at HIGH; the jitter and the temporal blend do the filtering) × the cloud layer's shadow × the height fog's closed-form sun-ward transmittance × HG(g) | × the particle transmittance map's slab T (volumes' self-shadow, lit smoke's shadow) |
| Sky | The SH's first band (the sky's mean), × `sky_scale` | Unchanged |
| Lamps (40), point lights (32) | Each in reach: its falloff as the lit particle's × HG(g), × `local_light_scale`; the point lights eased under `point_light_ceiling` | Unchanged |
| Fill | – | Its colour × its 512 map, one tap |
| Spot, omni | – | Its cone and range × its map (one gather, one cube tap) |
| RC | – | The four quadrants at the froxel's x and z, averaged (isotropic), × `cascade_scale`, fading with height over `cascade_height` |
| Emission | `density · emission` (with its falloff and noise) | Unchanged |

- **`UNLIT` media** take emission only. Their colour is exactly the game's, as the sheets' is today.
- **The lighting code is the lit particle shader's.** It moves into `production_volume_light.hlsli`, which both include.
  The lit particle's pictures must not change (proven byte for byte, "Platforms and testing").
- **Multiple scattering** isn't modelled beyond the sky term, as in Smolder.

### Integration

- **Opaque surfaces and the background.** The apply pass fogs them before the transparent pass, as Smolder's froxel fog
  is applied before its blur, refraction and OIT. The background takes the column to `far_distance`.
- **Transparent batches.**
  - Unflagged ones draw over the volumes. That is v2's layering today (shots, telegraphs, discs, pickups unfogged) and
    the readable default.
  - `IN_VOLUMES` batches take the volumes at their own depth, as above.
  - **Phase 1 honours the flag** where the batch draws with the lit particle shader (a free slot: t10, t9 and t5 in the
    three variants) or the fast unlit shader (t3). Under the full scene shader it does nothing (no slot is free; RFC 0013
    "What the last slot costs"). Phase 2 frees one there, for `SOFT_PARTICLE` and refraction.
- **Lit particles** keep their distance-fog trick and add the volumes' `T` and `S` at their own depth. That is v2's lit
  smoke inside gas, or in a dust storm.
- **Refraction.** The copy is taken after the apply, so a refracting batch displaces the fogged scene. Its own depth
  takes no volume in phase 1 (full scene shader).
- **SSR, HBAO and SSAO** run before the apply and are unchanged: wet streets don't reflect the fog.
- **The distance fog stays** in the composite after the transparent pass, at the opaque depth.
  - With volumes in front of a far, fogged surface the order is slightly wrong: the far haze is laid over the volume's
    light instead of under it.
  - Hosts that adopt volumes should move their haze into the lit global medium (open questions).
- **Bloom** takes the volumes' light. A fire core or a lamp halo blooms, and fog in front of a lamp head dims its bloom,
  as it should.
- **FSR 3.**
  - The apply writes `local opacity · reactive_scale` into the reactive mask, with `MAX` blending, before the
    transparent pass loads it.
  - Only local volumes count. The fog of war and the height fog are static over the ground, so they keep FSR 3's
    accumulation.
  - Volumes have no motion vectors of their own. The ground's motion under them is right, which CHIMERA's sheet lacks
    today.
- **FSR 1, SMAA, FXAA:** nothing to do. The grid is smooth and noise-free when sampled.
- **HDR output (RFC 0017).** Volumes are scene-linear light before the tone map. Emission above 1 carries into bloom and
  will carry into an HDR output unchanged.
- **World curvature** (`post.world_curvature`) bends vertices only. Volumes are placed in the drawn space, as the
  composite's fog is. Neither game uses it.

### Temporal stability

- **Each view keeps a history:** its last froxels' lit radiance per unit extinction, plus the extinction it was lit at.
- **Each frame:**
  - the froxel's sample is jittered within the froxel along a Halton sequence on the window's frame count (the
    renderer's own, independent of FSR 3's jitter);
  - it is reprojected through the view's previous view-projection;
  - then blended: `lit = lerp(history, current, temporal_blend)`, at 0.1 by default.
- **Density and emission are never accumulated.** They are evaluated fresh each frame, so an explosion growing over
  0.3 s has no trail.
- **The light's history is dropped** where the extinction changed by more than half, or the history falls outside the
  last grid.
- **This is the one place the design departs from Smolder,** which keeps no temporal filter of its own. The grid must
  be stable at the Deck preset, where FSR 1 accumulates nothing.
- **A view starts a new history** on `post->temporal_reset`, on its own `BTBH_GPU_SCENE_VIEW_TEMPORAL_RESET`, when its
  grid's size or quality changes, after a frame without volumes, and always with `BTBH_GPU_SCENE_VOLUMES_NO_TEMPORAL`
  (captures and tests).

### Views

- **Each view gets its own grid and history.** They are camera-aligned and cover its whole frame in phase 1; phase 2
  fits them to its half.
- **Each view uses its own lamps, point lights, spot, sun map and transmittance map,** as its geometry does
  (`BTBHGPUSceneView.lighting`).
- **The apply runs clipped per view,** as the composite's views pipeline does.
- **Media and volumes are the frame's,** shared.
- **With two views RC is off,** so phase 2's RC light is too.

Two grids cost twice the froxel passes. At the Deck preset that is about 0.02 ms (below).

### Quality tiers and budgets

The grid's x count is fixed by the tier, and y follows the render aspect (`round(x · height / width)`). A host may
override x and the depth count.

| Tier | Grid (16:9) | Sun taps | Noise octaves | Meant for |
| --- | --- | --- | --- | --- |
| `ANALYTIC` | none: the apply evaluates the global medium and the `ALONG_VIEW` fields in closed form per pixel; local volumes and non-`ALONG_VIEW` fields aren't drawn | none (unshadowed) | 1, at the anchor point | The fallback on a device without 3D RGBA16F storage (reported); a host's floor where only the fog of war and the height fog must show |
| `LOW` | 64 x 36 x 32 (64 x 40 x 32 at the Deck's 16:10) | 1 | 1 | v2's Deck preset and Low; CHIMERA's Low and Medium |
| `MEDIUM` | 96 x 54 x 48 | 1 | 2 | v2's Medium; CHIMERA's High |
| `HIGH` | 160 x 90 x 64 | 4 | 3 | v2's High at 4K; CHIMERA's Ultra |
| `HIGH` + hero (phase 3) | as HIGH, plus flagged volumes raymarched per pixel | 4 | 3 | v2's High, close-up effects |

**GPU targets,** in this PC's cost log for one view. These are estimates to be measured in phase 1, not measurements:

| Tier, setting | Froxel + integrate | Apply | Target total | Share of that setting's GPU frame |
| --- | --- | --- | --- | --- |
| `ANALYTIC`, 1440p | – | about 0.02 ms (in the apply's place) | ≤ 0.03 ms | under 1 % of a 7.5 ms frame |
| `LOW`, v2 Deck (960 x 600 rendered) | about 0.02-0.03 ms (82k froxels) | about 0.02 ms | **≤ 0.10 ms** | about 5-7 % of 1.4-2.2 ms |
| `MEDIUM`, 1080p-1440p | about 0.05-0.08 ms (249k froxels) | about 0.05-0.08 ms | **≤ 0.25 ms** | about 3 % of CHIMERA's frame |
| `HIGH`, v2 4K native + FSR 3 | about 0.15-0.25 ms (922k froxels) | about 0.2-0.3 ms (8.3 Mpx, bandwidth-bound) | **≤ 0.6 ms** | about 4-5 % of 12-15 ms |
| Hero volumes (phase 3), 4K | – | per pixel, covered pixels only | **≤ +1.0 ms** with hero volumes on a quarter of the screen | – |

**How the targets are hit:**
- **Grid cost is independent of resolution,** fixed per tier.
- **Depth slices sit between `near_distance` and `far_distance`.** For a top-down camera the game sets them to bracket
  its ground, so the 32-64 slices land where the action is. `slice_curve` blends linear and exponential spacing.
- **Empty froxels skip the lighting.** Volumes are culled per froxel by their CPU-computed boxes.
- **One shadow tap, smoothed over time,** in place of a filter.
- **The apply is one depth read, one trilinear 3D fetch and one blend.** It discards where T is about 1 and S about 0,
  so a frame mostly free of volumes writes little. At 4K it is bandwidth-bound, which is why it may later fold into the
  SSAO resolve (phase 2) where that runs.
- **Hero volumes (phase 3)** march only covered pixels and drop their lighting rate in dense smoke (Smolder's trick, via
  `groupshared`).

**On the real device:**
- The Deck preset is a stand-in run on the 3070 Ti, so a real device's numbers are a multiple of these.
- My inference only, from FLOPS and bandwidth: a Steam Deck's GPU is roughly 7-13 times slower, putting `LOW` at
  roughly 0.3-0.7 ms there.
- A faster "Steam Machine" GPU would be less.
- The measurement is open question v2-3.

**Memory per view** (three RGBA16F 3D textures and one R16F, ping-pong history included): `LOW` 2 MiB, `MEDIUM` 6.5 MiB,
`HIGH` 24 MiB. A second view doubles it. Add a 16 KiB storage buffer for the volumes and its transfer buffer, made
once.

### API

Additive. No existing struct grows. One new bit in `BTBHGPUSceneGeometryBatch.flags2` (byte 254, bit 1). Sizes and
offsets are for 64-bit, checked with `ctypes` against the C layout rules; `BTBHGPUSceneVolume` has no pointer, so it is
the same on 32-bit.

```c
#define BTBH_GPU_SCENE_MAX_VOLUMES 128u
#define BTBH_GPU_SCENE_VOLUME_FIELD_COUNT 2u

/* BTBHGPUSceneVolumes.quality: what the window's scene presents draw. */
typedef enum BTBHGPUSceneVolumeQuality {
    BTBH_GPU_SCENE_VOLUME_QUALITY_OFF = 0,       /* nothing (as without the call) */
    BTBH_GPU_SCENE_VOLUME_QUALITY_ANALYTIC = 1,  /* closed form per pixel: the global medium, ALONG_VIEW fields; no shadow */
    BTBH_GPU_SCENE_VOLUME_QUALITY_LOW = 2,       /* the grid, 64 wide, 32 deep */
    BTBH_GPU_SCENE_VOLUME_QUALITY_MEDIUM = 3,    /* 96 wide, 48 deep */
    BTBH_GPU_SCENE_VOLUME_QUALITY_HIGH = 4,      /* 160 wide, 64 deep */
    BTBH_GPU_SCENE_VOLUME_QUALITY_COUNT = 5
} BTBHGPUSceneVolumeQuality;

/* BTBHGPUSceneVolumes.flags. */
enum {
    /* Each frame afresh: no history (captures, tests, a scripted shot). */
    BTBH_GPU_SCENE_VOLUMES_NO_TEMPORAL = 1u << 0,
    /* The global medium takes no light: its emission only. */
    BTBH_GPU_SCENE_VOLUMES_GLOBAL_UNLIT = 1u << 1
};

/* BTBHGPUSceneVolume.shape. */
typedef enum BTBHGPUSceneVolumeShape {
    BTBH_GPU_SCENE_VOLUME_ELLIPSOID = 0,  /* full inside, fading over edge_softness toward the shell */
    BTBH_GPU_SCENE_VOLUME_BOX = 1,        /* full inside, fading over edge_softness toward each face */
    BTBH_GPU_SCENE_VOLUME_GAUSSIAN = 2,   /* exp(-4.5 r^2) inside the shell (r = 1 is 3 sigma); edge_softness unused */
    BTBH_GPU_SCENE_VOLUME_SHAPE_COUNT = 3
} BTBHGPUSceneVolumeShape;

/* BTBHGPUSceneVolume.flags. */
enum {
    BTBH_GPU_SCENE_VOLUME_UNLIT = 1u << 0,            /* no light: its emission only */
    BTBH_GPU_SCENE_VOLUME_NO_LOCAL_LIGHTS = 1u << 1,  /* the sun, the sky (and RC, phase 2) only */
    /* Inside its shape the global medium and the fields thin by the shape's weight (1 - w); its density, albedo and
       emission are ignored, its noise shapes the clearing. */
    BTBH_GPU_SCENE_VOLUME_CLEARS = 1u << 2,
    /* Phase 2: casts its sun-ward optical depth into the particle transmittance map (with particle_shadow set). Phase 1
       accepts it and draws nothing more. */
    BTBH_GPU_SCENE_VOLUME_SUN_SHADOW = 1u << 3,
    /* Phase 3: raymarched per pixel at QUALITY_HIGH. Phase 1 accepts it and draws the volume in the grid, as every
       tier below HIGH will. */
    BTBH_GPU_SCENE_VOLUME_HERO = 1u << 4
};

/* A local volume. 128 bytes, no pointer. A volume of density 0 is skipped (not refused). */
typedef struct BTBHGPUSceneVolume {
    float center[3];          /*   0: world position */
    float density;            /*  12: extinction per render unit at the core, >= 0 */
    float axis_x[3];          /*  16: half-axis vectors (direction times half-extent), world units; with density > 0  */
    float edge_softness;      /*  28:   the three must span space (determinant not ~0); 0..1, the share of the     */
    float axis_y[3];          /*  32:   half-extent over which ELLIPSOID and BOX fade out (0: a hard edge)         */
    float noise_strength;     /*  44: 0..1: how much the noise carves the density (0: none) */
    float axis_z[3];          /*  48 */
    float noise_frequency;    /*  60: noise cells per render unit (0: one cell across the shortest half-axis) */
    float albedo[3];          /*  64: single-scattering albedo, linear, 0..1 */
    float phase_g;            /*  76: Henyey-Greenstein asymmetry, -0.95..0.95 (0 isotropic) */
    float emission[3];        /*  80: linear radiance an opaque, black core shows (energy folded in), >= 0 */
    float emission_falloff;   /*  92: 0..1: 0 even, 1 strongest at the core and none at the shell */
    float noise_offset[3];    /*  96: where the noise is read from, moved by the host (wind, rise, churn) */
    float light_scale;        /* 108: scales the light it scatters, not its emission (0: 1) */
    uint8_t shape;            /* 112: BTBHGPUSceneVolumeShape */
    uint8_t flags;            /* 113: BTBH_GPU_SCENE_VOLUME_* */
    uint16_t reserved16;      /* 114: 0 */
    uint32_t source;          /* 116: phase 3 (a 3D density texture's index); 0 */
    uint32_t reserved[2];     /* 120: 0 */
} BTBHGPUSceneVolume;

/* BTBHGPUSceneVolumeField.flags. */
enum {
    /* Read where the eye's ray meets y = anchor_height, so every sample along a pixel's ray takes one value (fog of war:
       the clear ground on screen is the revealed ground; cliffs and roofs dim alike). Else at the sample's own x, z. */
    BTBH_GPU_SCENE_VOLUME_FIELD_ALONG_VIEW = 1u << 0,
    BTBH_GPU_SCENE_VOLUME_FIELD_INVERT = 1u << 1,           /* 1 - texel (a "revealed" texture as it is) */
    BTBH_GPU_SCENE_VOLUME_FIELD_UNLIT = 1u << 2,            /* its emission only: the game's own colour */
    BTBH_GPU_SCENE_VOLUME_FIELD_NO_LOCAL_LIGHTS = 1u << 3   /* the sun, the sky (and RC, phase 2) only */
};

/* A medium the game shapes with a 2D texture over the world. 104 bytes on 64-bit. Zeroed (texture NULL, density 0): no
   field. */
typedef struct BTBHGPUSceneVolumeField {
    /* An RGBA8 texture of this window (btbh_gpu_window_texture_create_rgba8_ex, linear colour space; arrays and
       heightfields refused), borrowed: keep it alive while the setting names it. NULL with a density: 1 everywhere inside
       world_rect (all four 0: everywhere), a mist slab or a blizzard. */
    BTBHGPUTexture *texture;  /*   0 */
    float world_rect[4];      /*   8: min x, min z, max x, max z the texture spans (texel centres inset half a texel);
                                      0 outside it */
    float bottom;             /*  24: world y the medium starts at */
    float top;                /*  28: world y it reaches full density at, > bottom */
    float top_softness;       /*  32: render units above top over which it fades out (0: a hard top) */
    float density;            /*  36: extinction per render unit at a texel of 1, at top, >= 0 */
    /* How the density climbs from bottom to top: density * h^profile_exponent, h = (y - bottom) / (top - bottom);
       0 even through the slab (a real volume, thinning only above top), 1-2 top-heavy (a lid, like v2's sheets), 0..8 */
    float profile_exponent;   /*  40 */
    float albedo[3];          /*  44 */
    float phase_g;            /*  56 */
    float emission[3];        /*  60: an UNLIT field's colour, a lit one's glow */
    float anchor_height;      /*  72: ALONG_VIEW's plane (world y) */
    float noise_offset[3];    /*  76 */
    float noise_strength;     /*  88: 0..1 */
    float noise_frequency;    /*  92: cells per render unit (0: 1) */
    uint8_t channel;          /*  96: 0..3, the texel's r, g, b or a */
    uint8_t flags;            /*  97: BTBH_GPU_SCENE_VOLUME_FIELD_* */
    uint8_t reserved[6];      /*  98: 0 */
} BTBHGPUSceneVolumeField;

/* What the window's scene presents draw as volumes (btbh_gpu_window_scene_set_volumes). 312 bytes on 64-bit. Zeroed:
   QUALITY_OFF. Zero means the default noted for every field below. */
typedef struct BTBHGPUSceneVolumes {
    uint8_t quality;            /*   0: BTBHGPUSceneVolumeQuality */
    uint8_t flags;              /*   1: BTBH_GPU_SCENE_VOLUMES_* */
    uint16_t grid_width;        /*   2: froxels across (16..256; 0: the quality's) */
    uint16_t grid_depth;        /*   4: slices (8..128; 0: the quality's) */
    uint16_t reserved16;        /*   6: 0 */
    float near_distance;        /*   8: view depth the grid starts at (0: the camera's near plane) */
    float far_distance;         /*  12: view depth it ends at, > near (required from LOW up); surfaces past it take the
                                       whole column */
    float slice_curve;          /*  16: 0 linear .. 1 exponential slice spacing */
    float temporal_blend;       /*  20: the new frame's share of the light (0: 0.1; 1: no history) */
    float maximum_opacity;      /*  24: the most any pixel loses to the volumes, 0..1 (0: 1), in-scatter scaled with it */
    float reactive_scale;       /*  28: FSR 3's reactive mask from local volumes' opacity (0: none) */
    float global_albedo[3];     /*  32 */
    float global_density;       /*  44: uniform extinction everywhere in the grid */
    float global_emission[3];   /*  48 */
    float global_phase_g;       /*  60 */
    float height_density;       /*  64: extra extinction at height_base, growing below it */
    float height_base;          /*  68: world y */
    float height_falloff;       /*  72: 1 / render units (> 0 with height_density) */
    float sky_scale;            /*  76: the sky's share (0: 1) */
    float local_light_scale;    /*  80: the lamps', point lights' (and spot's) share (0: 1) */
    float cascade_scale;        /*  84: phase 2: RC's share (0: none); phase 1 validates and ignores it */
    float cascade_height;       /*  88: phase 2: render units over which RC's light fades upward (0: 1) */
    uint32_t volume_count;      /*  92: 0..BTBH_GPU_SCENE_MAX_VOLUMES */
    const BTBHGPUSceneVolume *volumes;                          /*  96: volume_count of them, copied at the call */
    BTBHGPUSceneVolumeField fields[BTBH_GPU_SCENE_VOLUME_FIELD_COUNT]; /* 104, 208 */
} BTBHGPUSceneVolumes;

/* What the last frame's volumes ran (btbh_gpu_window_scene_volume_report). 32 bytes. */
typedef struct BTBHGPUSceneVolumeReport {
    uint8_t quality;             /*  0: what ran: the asked quality, ANALYTIC on a fallback, OFF when none drew */
    uint8_t fallback_reason;     /*  1: BTBH_GPU_SCENE_VOLUME_FALLBACK_* */
    uint16_t grid_width;         /*  2 */
    uint16_t grid_height;        /*  4 */
    uint16_t grid_depth;         /*  6 */
    uint32_t view_count;         /*  8 */
    uint32_t volumes_drawn;      /* 12: local volumes that reached some view's grid */
    uint32_t volumes_culled;     /* 16: outside every grid, density 0, or not drawn at ANALYTIC */
    uint32_t history_reset;      /* 20: 1 when a view started a new history this frame */
    double cpu_encode_milliseconds; /* 24 */
} BTBHGPUSceneVolumeReport;

enum {
    BTBH_GPU_SCENE_VOLUME_FALLBACK_NONE = 0,
    BTBH_GPU_SCENE_VOLUME_FALLBACK_DEVICE = 1,            /* no 3D RGBA16F storage texture: ANALYTIC instead */
    BTBH_GPU_SCENE_VOLUME_FALLBACK_RENDER_TO_TARGET = 2,  /* renders into targets draw no volumes (phase 1) */
    BTBH_GPU_SCENE_VOLUME_FALLBACK_DEBUG_VIEW = 3         /* RC's debug views skip them, as they skip the composite */
};

/* Sets the volumes the window's following scene presents (present_instances, _with_visibility, present_views) and
   prewarms draw. It copies the struct and its volumes at the call; field textures are borrowed by handle. The setting
   stays until the next call; NULL (or QUALITY_OFF) turns volumes off, freeing nothing until the window is destroyed.
   Call it between frames, before btbh_gpu_window_scene_prewarm(_views) for each setting a preset uses. Renders into
   targets ignore it (phase 1). Returns BTBH_GPU_OK, or BTBH_GPU_INVALID_ARGUMENT naming the field or the volume's index
   and changing nothing (below). */
BTBHGPUResult btbh_gpu_window_scene_set_volumes(BTBHGPUWindow *window, const BTBHGPUSceneVolumes *volumes);

/* The last scene present's volumes. NULL window or report: BTBH_GPU_INVALID_ARGUMENT. */
BTBHGPUResult btbh_gpu_window_scene_volume_report(BTBHGPUWindow *window, BTBHGPUSceneVolumeReport *report);

/* BTBHGPUSceneGeometryBatch.flags2, byte 254, bit 1 (bit 0 is KEEP_CASCADES_LOOK): */
    /* A transparent batch takes the volumes in front of it at its own depth (alpha: T * colour + S; additive:
       T * colour). Without it a transparent batch draws over them. Honoured where the batch draws with the lit
       particle shader or the fast unlit shader (phase 1); elsewhere it draws as without it (phase 2 frees a slot in the
       full scene shader). Changes nothing on frames without volumes. Refused on a batch drawn in the opaque pass. */
    BTBH_GPU_SCENE_BATCH_FLAGS2_IN_VOLUMES = 1u << 1

/* Defined when btbh_gpu_window_scene_set_volumes, btbh_gpu_window_scene_volume_report, their structs and
   BTBH_GPU_SCENE_BATCH_FLAGS2_IN_VOLUMES exist. */
#define BTBH_GPU_SCENE_HAS_VOLUMES 1
```

**Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, "GPU scene volumes: ..." naming the field, or the volume or field by
index; the window's setting is left as it was):
- **The frame's settings:**
  - `quality` at or past `COUNT`, or a flag bit the renderer doesn't know;
  - a `reserved` field or `source` that isn't 0;
  - from `LOW` up, `far_distance` ≤ `near_distance`;
  - a `grid_width` outside 16-256 or a `grid_depth` outside 8-128 when not 0;
  - `slice_curve`, `temporal_blend` or `maximum_opacity` outside 0..1;
  - `height_falloff` ≤ 0 with `height_density` > 0;
  - a negative density, emission, scale or reactive scale;
  - any value that isn't finite.
- **The volumes:**
  - `volume_count` above 128, or `volumes` NULL with a count;
  - a `shape` at or past `COUNT`;
  - an albedo outside 0..1, `phase_g` outside -0.95..0.95, `edge_softness`, `noise_strength` or `emission_falloff`
    outside 0..1;
  - with density > 0 (or `CLEARS`), half-axes that don't span space.
- **The fields:**
  - a texture that isn't an RGBA8 2D texture of this window (an array, a heightfield, a render target, another window's);
  - a field with a texture or density whose `world_rect` is empty (min ≥ max, unless all four are 0 with a NULL
    texture);
  - `top` ≤ `bottom`, or a `profile_exponent` outside 0..8;
  - `channel` above 3.
- **At the present** (the present fails, naming the path, as for the particle shadow's shaders): a volume shader missing
  beside the post shaders, or (with `IN_VOLUMES` batches) a lit particle or fast unlit shader of an older release that
  doesn't name the volume texture. Metal libraries aren't checked.

**Zero values and opt-in:**
- **No call, or `NULL`, or `QUALITY_OFF`:** no pass, pipeline, texture or buffer is made, and every frame is byte for
  byte as before.
- **The four changed shaders** bind a fallback for their new slot and skip its branch.
- **A batch's `IN_VOLUMES` bit** changes nothing on a frame without volumes.

**The prewarm.**
- `btbh_gpu_window_scene_prewarm` and `_views` read the window's setting. They make the volume pipelines (two compute,
  the apply plain and per view, with and without the reactive target), the grids for its quality and view count, and
  the volumes' buffer.
- Changing `volume_count`, the media or the field textures makes nothing.
- A frame that needs what no prewarm made logs it with `prewarm_covers` 1.

**The cost log.**
- **A new group `volume`:** the froxel and integrate passes after the shadow maps, plus the apply before the transparent
  pass. Both pieces add to it.
- It is printed only on frames that drew volumes, as the view-1 groups are, so every existing line keeps its format.
- Its passes are labelled "BTBH Volume Froxels", "BTBH Volume Integrate" and "BTBH Volume Apply (Views)" for captures.

## Minimal example

```c
#if defined(BTBH_GPU_SCENE_HAS_VOLUMES)
/* Once per setting, before the prewarm; again whenever something moves (a few hundred bytes, copied). */
BTBHGPUSceneVolume blast;
memset(&blast, 0, sizeof(blast));
blast.center[0] = x; blast.center[1] = 0.6f; blast.center[2] = z;
blast.density = 3.0f;
blast.axis_x[0] = 1.2f; blast.axis_y[1] = 1.0f; blast.axis_z[2] = 1.2f;   /* grown by the game over 0.4 s */
blast.albedo[0] = 0.30f; blast.albedo[1] = 0.28f; blast.albedo[2] = 0.26f;
blast.emission[0] = 6.0f; blast.emission[1] = 2.4f; blast.emission[2] = 0.6f;
blast.emission_falloff = 1.0f;
blast.noise_strength = 0.5f;
blast.noise_offset[1] = -rise;                     /* the host's time: the churn rises */
blast.shape = BTBH_GPU_SCENE_VOLUME_GAUSSIAN;      /* plus a point light at its centre, as today */

BTBHGPUSceneVolumes volumes;
memset(&volumes, 0, sizeof(volumes));
volumes.quality = BTBH_GPU_SCENE_VOLUME_QUALITY_LOW;
volumes.near_distance = 2.0f;
volumes.far_distance = 90.0f;                      /* brackets the ground the camera sees */
volumes.height_density = 0.05f; volumes.height_base = 0.0f; volumes.height_falloff = 1.5f;   /* ground mist */
volumes.global_albedo[0] = volumes.global_albedo[1] = volumes.global_albedo[2] = 0.9f;
/* The fog of war: the game's reveal texture, alpha = fog, one texel a cell, 32 cells of margin. */
volumes.fields[0].texture = reveal;
volumes.fields[0].world_rect[0] = -32.0f; volumes.fields[0].world_rect[1] = -32.0f;
volumes.fields[0].world_rect[2] = map_w + 32.0f; volumes.fields[0].world_rect[3] = map_h + 32.0f;
volumes.fields[0].bottom = -1.0f; volumes.fields[0].top = 4.6f; volumes.fields[0].top_softness = 0.6f;
volumes.fields[0].density = 0.63f;                 /* profile_exponent 0: even through the slab */
volumes.fields[0].emission[0] = 0.02f; volumes.fields[0].emission[1] = 0.025f; volumes.fields[0].emission[2] = 0.04f;
volumes.fields[0].channel = 3;
volumes.fields[0].flags = BTBH_GPU_SCENE_VOLUME_FIELD_ALONG_VIEW | BTBH_GPU_SCENE_VOLUME_FIELD_UNLIT;
volumes.volumes = &blast;
volumes.volume_count = 1u;
btbh_gpu_window_scene_set_volumes(window, &volumes);
/* lit smoke batches: batch.flags2 |= BTBH_GPU_SCENE_BATCH_FLAGS2_IN_VOLUMES; then prewarm and present as before */
#endif
```

With an even slab at density 0.63, a near-vertical ray over the street crosses about 4.9 cells of fog (4.6 plus half the
softness). The street keeps `exp(-0.63 · 4.9) ≈ 0.046` of its light, a 5-storey roof (1.75 cells) about 0.14, and a
12-storey tower's roof (4.2) about 0.64. v2's sheets keep 0.045, 0.09 and 0.2 (`v2/crates/v2-render/src/fog.rs:51-56`):
- **The street matches.**
- **The roofs come out lighter,** because a real volume thins with height while the sheets put most of their alpha in
  a 0.8 lid at 4.6. A `profile_exponent` of about 1-2 moves the density up toward that look.
- **Matching the sheets exactly is one tuning pass** (open question v2-1).

## Implementation

### Phase 1: the grid, the analytic tier, transparency (one release)

**Renderer:**
- **The API and its contract test.** Static asserts on the layouts and every refusal above, in
  `Tests/btbh_gpu_scene_order_test.c` and a new `Tests/btbh_gpu_volumes_test.c`.
- **The setter's copy into the window,** taken by the next present. Under RFC 0015's frame thread it is part of what a
  frame copies at its submit.
- **The CPU side per frame and view.** The tier's grid size, the slice depths, and each volume's inverse axes and froxel
  box. The volumes that reach the view go into a storage buffer in the frame's upload.
- **`production_volume.hlsli`:** shapes, noise, fields with their profile and `ALONG_VIEW` read, the height fog and its
  sun-ward closed form, the slice mapping, and the analytic integrals (the height fog's, a slab's chord).
- **`production_volume_light.hlsli`:** the lit particle's light functions, moved, with HG in place of the wrap.
  `production_particle_lit*.frag` include it unchanged in effect.
- **`production_volume_froxel.comp`** at `[numthreads(4, 4, 4)]` and **`production_volume_integrate.comp`** at
  `[numthreads(8, 8, 1)]`, looping over the slices. No wave intrinsics.
- **`production_volume_apply.frag`:** grid or analytic, with the cap and the reactive output. Plain and with
  `production_post_view.vert`.
- **The present's steps.** The volume group after the shadow split. The apply after the SSAO/SSR resolve and the
  distance pass, reading depth as the composite does, with the reactive target written first and the transparent pass
  loading it.
- **The changed shaders.** The lit particle variants (t10, t9 and t5) and `production_model_unlit.frag` (t3) gain:
  - the integrated grid;
  - a uniform block of their own: the grid's mapping, alpha or additive, and at `ANALYTIC` the media's closed-form
    parameters;
  - the `IN_VOLUMES` branch.
- **Views:** a grid and history per view, and the apply per view clipped.
- **The prewarm, the lazy creation log, the cost log's `volume` group and the report.**
- **The fallback:** `SDL_GPUTextureSupportsFormat(R16G16B16A16_FLOAT, 3D, SAMPLER | COMPUTE_STORAGE_WRITE)` false runs
  `ANALYTIC` and says so.
- **The CMake shader list:** the three new shaders (`production_volume_froxel.comp`, `production_volume_integrate.comp`,
  `production_volume_apply.frag`). The two includes are read by them.

**Estimate:** 9-13 days, measurements and identity runs included. v2's adoption: 2-3 days (the fog of war's field,
presets, prewarm). CHIMERA's: 2-3 days, after moving its pin from 0.14.2.

### Phase 2: shared shadows, the remaining lights, the last slot

- **`SUN_SHADOW` volumes draw into the particle transmittance map.** A quad over each volume's sun-space footprint carries
  its optical depth along the sun ray: closed-form for `GAUSSIAN` (VGP's line integral), the chord times the edge falloff
  for `ELLIPSOID` and `BOX`.
  - With `particle_shadow` set, volumes then self-shadow (the grid reads the map), shadow lit particles, and with readers
    2 shadow the street.
  - The map is drawn on frames with a casting volume even without a flagged batch.
  - This is Smolder's shared light-space transmittance on the renderer's existing slab model. Moment maps (4 x 16-bit,
    as Smolder) only if the slab shows its limits.
- **The rest of the lights.** RC in-scatter (`cascade_scale`, `cascade_height`), the fill, and the spot or omni light
  with its map.
- **A free slot in the full scene shader,** for `IN_VOLUMES` on `SOFT_PARTICLE` and refraction (RFC 0013's options: the
  sun map's comparison binding on `GatherCmp`, or the world detail's three maps as one array).
- **Fields that follow the ground:** an optional heightfield (RFC 0012's `BTBHGPUSceneHeightLift` heightfield) as
  `ALONG_VIEW`'s surface, for v2's Height terrain hills.
- **Grids fitted to each view's half.** The apply folded into the SSAO resolve where that runs, if 4K's numbers ask for
  it.
- **If asked:**
  - a stencil exemption (pixels with given stencil bits take a share of the volumes) for enemy salience;
  - volumes in renders into targets.
- **Estimate:** 6-9 days.

### Phase 3: hero volumes (Smolder's core), at High

- **What it draws:** volumes flagged `HERO`, raymarched per pixel at the render size at `HIGH`. Below `HIGH` they stay in
  the grid. Procedural (the same shapes with stronger noise and domain warping) first.
- **Optional density sources:** small dense 3D textures, through a new 3D texture call, as flipbook frames (`source`).
- **Per-tile segments.** Each 8x8 tile gathers up to four hero bounds in `groupshared` and sorts them there. The step
  follows the source's voxel size or the shape's.
- **Lighting rate.** Lighting at full rate, dropping to 2x2 then 4x4 shared samples in a tile once the tile's
  transmittance falls below a threshold (Smolder's 50 % saving).
- **Lighting:** the same lights, with phase 2's transmittance map as their shadow.
- **Noise and temporal:** per-pixel blue-noise start offsets only under FSR 3, and no temporal filter of their own.
- **Compositing:** Smolder's single integration depth, inside the grid. The grid's fog up to that depth comes out of the
  background and goes back over the hero's light.
- **Estimate:** 10-15 days, after phase 1's 4K numbers.

### Phase 4, only if a game asks

- Smolder's sparse 4³-brick flipbooks from OpenVDB, with a packer under `tools/`;
- moment shadow maps for the lamps and point lights;
- injecting lit particles into the grid.

## Platforms and testing

- **Windows, Vulkan (RTX 3070 Ti), on every phase:**
  - **Identity without volumes:** `check.ps1`, and the four modes (plain, shared meshes, no runs, runs verified), 421
    images a mode, 0 px different against 0.16.1. Also through `present_views` with one view.
  - **This proves the four changed shaders** draw as before with the new slot's fallback.
- **A new readback smoke** (`Tests/btbh_gpu_volumes_smoke.c`). Its scenes:
  - a height-fog bank at dusk with a row of lamps (halos), and a sun shaft through a gap (the shadow map);
  - an `ALONG_VIEW` field revealing a disc, compared per pixel at `ANALYTIC` with a C copy of the closed form (as
    `btbh_gpu_particle.h` copies the lit particle's), and at `LOW` within a tolerance;
  - an emissive Gaussian with a point light, and a `CLEARS` volume in a fog;
  - lit particles with and without `IN_VOLUMES` in front of, inside and behind a volume;
  - two views split.
- **Checks in the smoke:**
  - temporal stability: a still camera for 60 frames, frame-to-frame RMS under a threshold after 10, and a growing volume
    leaving no trail;
  - the reactive mask under FSR 3;
  - the prewarm leaving the lazy creation log empty after frame 1 for each tier;
  - the cost log's `volume` group.
- **Contract test:** layouts and refusals.
- **macOS, Metal:**
  - the smoke on Apple silicon (compute with 3D storage, a comparison sampler in compute);
  - Be The Bullet Hell v1's references unchanged (it sets nothing).
- **Linux, Vulkan (Deck):** when v2 first runs there. v2's Deck-preset measurement on the device itself (open question
  v2-3).
- **The games:**
  - v2: before and after its fog of war (salience numbers, the readability pass), the Deck preset and 4K High under
    the perf lock, and its golden hashes with volumes off.
  - CHIMERA: before and after its fog of war and abyss, at its presets and at strategic zoom.

## Cost

| Item | Off | On |
| --- | --- | --- |
| GPU | Nothing | Per tier, above: ≤ 0.10 ms at the Deck preset, ≤ 0.25 at Medium, ≤ 0.6 at 4K High (estimates to measure) |
| CPU per frame | Nothing (the setting's test) | Under 0.02 ms: per view, each volume's box (128 at most), the buffer's upload (16 KiB at most) |
| CPU per call | – | The copy: 312 bytes + 128 per volume |
| Memory | Nothing | Per view 2 / 6.5 / 24 MiB (LOW / MEDIUM / HIGH), the 16 KiB buffer; ANALYTIC none |
| Shaders | The four changed files: one uniform test, a fallback binding | One 3D fetch per opaque pixel (the apply), per `IN_VOLUMES` fragment |
| Pipelines | None | 2 compute and 2-4 graphics, all made by the prewarm |

## Risks

- **Froxel halos.** A thin silhouette in front of a dense volume takes a froxel's blur (15 px at the Deck preset, 24 at
  4K). Softened by the jitter. This is the price of the base layer, and hero volumes in phase 3 are the answer up close.
- **Ghosting.** The grid's light history plus FSR 3's at 4K. Mitigations:
  - fresh density and emission every frame;
  - history dropped on change;
  - local volumes in the reactive mask;
  - `temporal_blend` and `NO_TEMPORAL` as escape hatches.
- **Banding in thin height fog** across 32 slices at `LOW`. Mitigated by `slice_curve`, `near` and `far` bracketing the
  ground, and the jitter.
- **The fog of war's anchor plane on hills.** `ALONG_VIEW` reads at one height, so on v2's Height terrain the ground and
  the plane part by the hill's height. Phase 2's heightfield anchor fixes it. Flat stages are exact.
- **The sun map covers only what v2 fits it to.** A volume outside the map is taken as lit, and a tall column may leave
  its depth range (clamped).
- **The four changed shaders.** Their SPIR-V changes, and the identity runs must prove their pictures don't.
- **4K apply bandwidth.** About 0.2-0.3 ms at 8.3 Mpx. The folding into the SSAO resolve is held back for that.
- **Metal.** Compute on 3D storage textures and comparison sampling in compute are new for this renderer there.
- **The real device.** The budgets are this PC's. The Steam Machine's numbers aren't known until v2 runs on it.
- **Readability.**
  - Lit fog can lower enemy salience. The game keeps every density, plus `maximum_opacity` and `light_scale`.
  - Night lamps lighting an unrevealed fog of war would show the city's lights. Both games choose `UNLIT` or
    `NO_LOCAL_LIGHTS` there.
  - CHIMERA must keep passing no light of a unit its player can't see.
- **Determinism.** The renderer feeds nothing back. The noise is a pure function of position and the host's offset;
  only the light's history depends on the frame sequence (`NO_TEMPORAL` for exact captures).

## Compatibility

- **Versioning:** minor, additive, opt-in. New functions, structs, enums, a macro and one free bit of `flags2`. No struct
  grows.
- **Be The Bullet Hell (v1, Swift):** nothing. The Swift layer doesn't expose volumes (it can later).
- **Tyrian3D:** nothing. It rebuilds the shaders.
- **CHIMERA:** nothing until it adopts. Adopting means moving `renderer.pin` from 0.14.2 to the release, adding the three
  new shader names to its list of 14, and setting the fields.
- **v2:** nothing until it adopts. Its shader list is read from `CMakeLists.txt`.

## Alternatives

In short; the table under "Techniques considered" gives the reasons.

- **VGP as the base:** needs hardware ray tracing and offline lighting.
- **Smolder's per-pixel march alone:** cost at 4K native, and noise at the Deck preset without a temporal AA.
- **The analytic height fog alone (RFC 0002 A):** unlit, no local volumes. It stays as the `ANALYTIC` tier.
- **A 3D fog-of-war texture:** both games' vision is 2D.
- **Putting volumes in `BTBHGPUScenePost`:** no room left without growing it. A setter keeps the existing structs and
  frames untouched, and suits the frame thread's copy at the call.
- **New present functions with a volumes argument:** five variants to double.

## Open questions for v2

Each has a recommended answer; v2's session answers through the coordinator.

1. **Fog of war.**
   - Should field 0 replace the five sheets? It would be `ALONG_VIEW` at street level, reading the reveal texture's
     alpha, with bottom -1, top 4.6 cells, softness about 0.6 and density about 0.63.
   - An even slab, which fogs the towers' tops less than today, or a lid with `profile_exponent` 1-2, like the sheets?
   - Unlit with today's `FogLook` colour, or lit?
   - **Recommended:** replace the sheets. Start `UNLIT` with today's colour, and a `profile_exponent` tuned so that the
     street, roof and tower values match the sheets (0.045, 0.09, 0.2) and the salience numbers (`fog.rs:26-35`) carry
     over. Then try the even slab, and lit with `NO_LOCAL_LIGHTS` at night, in a play-test.
2. **The khamsin.**
   - Dust in the fog of war's own field (dust-coloured, lit, wind-driven noise, the game closing it behind the kaiju), or
     a second field?
   - **Recommended:** Egypt's field 0 turns dust-coloured and lit with drifting noise. A second, non-`ALONG_VIEW` field
     only if the owner wants dust in revealed ground too.
3. **Tiers per preset, and the device.**
   - **Recommended:** High = `HIGH`, Medium = `MEDIUM`, Low and the Deck preset = `LOW`, with `ANALYTIC` as the fallback.
     Never `OFF` in a run, since the fog of war is always on.
   - Hold the Deck preset to ≤ 0.10 ms on this PC.
   - Can v2 measure the Deck preset on the Steam Machine (or a Steam Deck) once phase 1 lands? The budgets above can't be
     trusted on the device until it does.
4. **Effects over or in the volumes.**
   - **Recommended:**
     - shots, telegraphs, discs and pickups stay over (unflagged), as today's layering;
     - the lit smoke and the flamethrower and plasma puffs take `IN_VOLUMES`;
     - the soft-particle effects, under the full scene shader, wait for phase 2.
5. **The Gas Leak.**
   - **Recommended:**
     - **The gas:** field 1 from a channel of the same RGBA8 texture, set per street cell by the game, green-tinted,
       bottom 0, top about 0.5 cells, with a little emission at night;
     - **The chain explosion:** emissive Gaussian volumes plus point lights, the game clearing the cells it burns.
6. **Weapons and hazards.** The plan is:
   - flamethrower, plasma and breath as chains of 4-8 emissive ellipsoids along the stream;
   - explosions as an emissive Gaussian with `emission_falloff` 1 and a point light, grown over 0.3-0.5 s;
   - steam vents and collapse dust as noisy ellipsoids;
   - fire and wreck smoke as lit particles with `IN_VOLUMES`, or volumes where they read better.

   Is 128 volumes a frame enough at v2's peak (a late run, Air Raid)? **Recommended:** keep 128, and v2 counts its peak.
7. **Biome atmosphere.** The renderer ships no presets.
   - **Recommended,** for v2 to set per biome and time of day:
     - jungle morning mist: height fog plus a uniform mist field (NULL texture, slab 0-0.8 cells, noise);
     - monsoon haze: the global density, lit;
     - desert dust storm: a world field with noise;
     - blizzard: a uniform field with fast noise plus the global density;
     - night fog: height fog lit by the 40 lamps.
8. **Salience.**
   - Is per-source density plus `maximum_opacity` and `light_scale` enough?
   - **Recommended:** yes for phase 1. Measure with the readability pass, and ask for phase 2's stencil exemption (enemy
     bodies taking a share of the volumes) only if the numbers drop.
9. **Split screen.**
   - Each view gets its own grid, about +0.02 ms at the Deck preset. RC is off in split frames, as today.
   - **Recommended:** accept.
10. **FSR 3 at 4K.**
    - **Recommended:** `reactive_scale` 0.3 from local volumes only, `temporal_blend` 0.1. Tune in the play-test.
11. **The distance haze.**
    - **Recommended:** where volumes run, turn the composite's distance fog off and use `global_density` (lit by sun and
      sky) instead, which avoids its misordering over near volumes.
12. **Hero volumes (phase 3)** for the breath and close explosions at High.
    - **Recommended:** decide after phase 1's 4K numbers.
13. **Renders into targets** (the V1 cards' pictures).
    - **Recommended:** no volumes there.

## Decisions (v2, 2026-11-28)

v2 took the recommendation on all 13 questions:

1. **Fog of war:** one renderer field replaces the five sheets, unlit with today's colour, the profile matched to 0.045
   / 0.09 / 0.2. v2 re-measures salience before it switches over, and tries a lit night version in a play-test.
2. **Khamsin:** Egypt's fog field turns dust-coloured and lit, with wind noise. No second field for now.
3. **Tiers:** High = HIGH, Medium = MEDIUM, Low and Deck = LOW, never off in a run. There is no Steam Deck to measure
   on: v2 deploys a bench build to the Steam Machine once phase 1 lands and the owner starts it, so the device number is
   the Machine at the Deck preset (and at its own), not a real Deck.
4. **Over or in:** shots, telegraphs, discs and pickups draw over the volumes; lit smoke and flamethrower and plasma
   puffs opt in; soft particles wait for phase 2.
5. **Gas Leak:** a second channel of the same texture. Until then v2's stopgap is a soft low haze of smoke sprites.
6. **128 volumes a frame.** v2 ranks its sources (nearest the kaiju, largest and newest first) and culls past 128
   itself, and counts its peak in a late stage-5 fight with a heavy build.
7. **Biome atmosphere** is v2's, per biome and time of day; the renderer ships no presets. v2's biomes (jungle and mud,
   desert, ice, monsoon flood) move their mist, haze, storms and blizzard onto volumes when phase 1 lands.
8. **Salience:** per-source density, an opacity cap and a light scale for phase 1; a stencil exemption for enemy bodies
   only if salience drops.
9. **Split screen:** a grid per view, no radiance cascades in split frames.
10. **FSR 3:** reactive scale 0.3 from local volumes, history blend 0.1, tuned in the play-test.
11. **Distance haze:** the composite's distance fog is off where volumes run, the lit global fog instead; v2 checks the
    far city's haze still reads the same in captures.
12. **Hero volumes:** decided after phase 1's 4K numbers.
13. **No volumes in renders into targets.**

Order (v2): HDR output (0.17.0), the distortion pass (RFC 0018), then this RFC's phase 1.

## Open questions for CHIMERA

Each has a recommended answer; CHIMERA's session answers through the coordinator.

1. **Fog of war in the renderer.**
   - The plan is two fields from one RGBA8 texture CHIMERA writes at 10 Hz with its eased values, 1 m texels:
     - "never seen" in one channel: near-black teal, 0.8, drifting noise;
     - "seen, not in sight" in another: dusk, 0.48.
   - Both `ALONG_VIEW`, `UNLIT`, with a slab from the abyss floor (about -20 m) to above the highest level.
   - The 500 m frame round the map: a world rect with margin texels at "never seen", or keep the frame mesh.
   - **Recommended:** yes. It replaces the CPU sheet and its 30 Hz upload (about 0.12 ms GPU). Every line of sight then
     dims alike by construction, as the lid does, and FSR 3 sees the ground's motion under the fog.
2. **Effects and lights in the fog.**
   - Should the renderer dim them?
   - **Recommended:** no. Keep `Fog::clear` and the "only what the player sees" rule game-side, since vision is
     simulation state. Effects stay unflagged.
3. **Height fog.**
   - The global medium's height fog (`height_density`, `height_base`, `height_falloff`, per-biome colour), in place of
     RFC 0002 part A, at every preset (`ANALYTIC` included).
   - Drop the three abyss sheets.
   - Canal mist as a few box volumes.
   - **Recommended:** yes. Mark RFC 0002 A superseded and keep its part B (water).
4. **Local volumes.**
   - Wanted: the *Shroud* (an 8 m dark ellipsoid, albedo about 0.05, high density, 5 s), the Wandering Storm (9 m,
     moving), lava steam, waterfall mist with sun shafts, the Ichor Font's steam, and Fog Banks and Smoke Spite once
     built. Combat smoke stays sprites.
   - **Recommended:** yes. Expect under 20 a view.
5. **Presets.**
   - **Recommended:** Low and Medium = `LOW`, High = `MEDIUM`, Ultra = `HIGH`.
     - Once the fog of war is the renderer's, quality is never `OFF` in a match. `ANALYTIC` is the floor, on a device
       without 3D storage: fog of war and height fog, but no local volumes, so the *Shroud* wouldn't show. That is why
       Low takes `LOW`, at ≤ 0.10 ms.
     - The "Atmosphere" toggle then zeroes the lit media and noise the game sets, not the quality.
   - At strategic zoom `far_distance` follows the zoom, and the cost doesn't grow (the grid is fixed).
6. **Sight-blocking clouds** (fog tides, Fog Banks, Smoke Spite).
   - **Recommended:** the simulation decides and the renderer only draws them. A spectator's switch of team means the game
     rewrites the field.
7. **Light regions (RFC 0003).**
   - **Recommended:** volumes take the frame's one lighting. A per-region tint of the volumes waits for RFC 0003.
8. **Lamps.**
   - Volumes take `lamp_*` and the point lights, so CHIMERA's night structure lights should move to `lamp_*` (as the RFC
     index's note and `CONSUMERS.md` already ask) to light the fog.
   - **Recommended:** move them. The fog of war's fields stay `UNLIT` (question 1), so no light shows through them.
9. **Adopting.**
   - It means a pin past 0.16.1 and three more shader names in its list.
   - **Recommended:** adopt with phase 1's release, at the fog of war and the abyss first.
