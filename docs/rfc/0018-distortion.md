# RFC 0018: Screen-space distortion: rings, discs and strips that bend the scene, applied in the HDR composite

- **Status:** Accepted on 2026-11-28: v2 answered all 13 questions, each as recommended (see "Decisions"). Phase 1
  implemented on `feature/0.18.0-distortion` (from `release/0.17.0`, after HDR output's phase 1) for 0.18.0, not yet
  released; where it departs from this text, see "Phase 1 as built"
- **Requested by:** Be The Bullet Hell v2. Its coordinator asked after the owner's seventeenth play-test (2026-11-28):
  "A screen-space distortion pass: v2 draws distortion sources (rings, blobs, strips with a strength/offset) into a
  buffer and post applies the offset." It went to the renderer with HDR and the lit smoke
  (`v2/docs/handoff-2026-11-25.md`, updated 2026-11-28)
- **Affects:**
  - Be The Bullet Hell v2 adopts it (opt-in).
  - Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D change nothing. A frame without a source drawn is byte for byte
    0.16.1's.
  - The release adds new shader files. `production_hdr_composite.frag.hlsl` gains `#if` blocks only, and its compiled
    SPIR-V and MSL must stay byte-identical (checked).
- **Issue:** none; v2 answers through its session
- **Depends on:** nothing. It only has to be ordered with:
  - **RFC 0017 (HDR output, accepted, phase 1 next).** HDR starts after the composite, so its gain reads the displaced
    picture and neither RFC changes the other. Its phase 1 took no cost step, so this one's is 20 (as built).
  - **RFC 0014's scene views (0.16.0) and 0.16.1's margin by the line.** They are the base for the split.
  - **RFC 0015 phase 1b (the frame thread).** When it lands, the sources set for a frame travel with `frame_submit`'s
    copy of it.
- **Sources:**
  - Line references are master `0e6b6d12` (v0.16.1 plus docs; its code is `c20c4d0b`'s, which RFC 0017 cites).
  - Short names: `scene.c` and `internal.h` for `src/gpu/btbh_gpu_*`; `btbh_gpu.h` for `include/btbh_gpu/btbh_gpu.h`;
    shaders under `shaders/gpu/`; SDL is `external/SDL` at `release-3.4.2`.
  - v2's numbers: `v2/docs/perf-2026-11-27.md` (the cost log's medians on this PC's RTX 3070 Ti) and the presets in
    `v2/crates/v2-game/src/graphics.rs:1-23`.
  - Every cost here is an estimate from traffic at about 430 GB/s effective (as RFC 0017 counts). None is measured.

## Need

What the player should see:

- **Shockwave rings:** a big blast or a kaiju's stomp sends a ring across the ground. Inside its band the street, the
  buildings and the units bend outward for about half a second.
- **Heat haze:** the air over fires, along beams and over the coming desert biome's sand shimmers.
- **Lensing orbs:** round energy orbs bend what lies behind them, like a lens.
- **Never the HUD.** The sources are short-lived and v2 gates them, so gameplay stays readable.

The budget:

- **4K on the TV:** High is FSR 3 Quality, a 2560 x 1440 render. Its GPU frame is 8.2 ms in the boss fight at that
  scale, 12-15 ms at full scale.
- **The Steam Deck preset:** 1280 x 800 output from a 960 x 600 render, with FSR 1 and FXAA. Its whole GPU frame is
  about 1.6-2.2 ms (v2's cost log on this PC: 1.2-1.5 ms in play, 1.9 ms on the title).

So the pass must be cheap with sources and cost nothing without them.

## What exists today

### Refraction (RFC 0005) can already draw all three

REFRACT batches can draw all three today:
- a ring mesh with outward normals and `surface_parameters1.z` makes CHIMERA's shockwave;
- a sphere does the same as a lens;
- the flow noise does haze.

What that costs and where it falls short:

| Part | Where | For distortion |
| --- | --- | --- |
| The surface variant | `btbh_gpu.h:666-678` | A mesh per shape, a batch or an instance per effect. Strength is per batch; per instance only through the colour's alpha |
| A copy of the scene | after the transparent pass, a blit at the render size into RGBA16F (`scene.c:12404-12426`, made at `:2933-2975`), on frames with a refracting batch (`:10314-10319`) | 59 MB of traffic at 4K High (0.14 ms) on every frame a source shows |
| Its pass | the third geometry phase (`:12096-12115`): the batches sample the copy at a displaced position (`:12959-12962`; `refract_fragment`, `production_model.frag.hlsl:2941-3023`) | Overlapping sources don't add up. Each reads the copy made before the first, so the later one's blend replaces the earlier one's |
| Its reactive mask | `saturate(length(offset) / 6)` in render pixels, times the frame's reactive scale (`production_model.frag.hlsl:3020-3022`, `:3047-3056`). The pass loads the transparent pass's mask (`scene.c:12434-12437`), blended with max (`:1979-1984`) | The rule to reuse |
| At the split line | "A refracting surface near the line may sample the other view's pixels" (`btbh_gpu.h:2993`) | Not acceptable for a ring that crosses the line |

### The post chain

In the present's order (`scene.c:13379-14007`). Everything is at the render size until the upscaler:

| Pass | Where | Picture |
| --- | --- | --- |
| Bloom, down and up | `:13384-13470` | HDR, from the composite's input |
| **HDR composite:** fog (it reads depth), bloom added, vignette | `:13471-13552` (views: `:13512-13537`); `production_hdr_composite.frag.hlsl:40-101` | HDR into `post_pong`, RGBA16F (`scene.c:5004-5017`) |
| LUT and grading | `:13553-13574` | HDR in, 0-1 out |
| CAS | `:13575-13597` | |
| Tilt-shift with the output, or the output alone (tone map, sRGB, dither) | `:13598-13644` | 8-bit, in the swapchain format |
| FXAA or SMAA | `:13645-13742` | 8-bit |
| Mirror | `:13747-13770` | |
| FSR 3 | `:13771-13920` | The tone-mapped picture, depth, motion vectors (RG16F) and the reactive mask (R8, `:4820-4829`) |
| FSR 1 (EASU, RCAS) or Lanczos | `:13921-14007` | Output size |

- **When the composite runs:** when fog, bloom or the vignette is on (`:10301-10305`). v2's lighting defaults to bloom 0.6
  (`v2/crates/v2-platform/src/light.rs:494`), so v2's frames run it.
- **Scene views:**
  - Each view's composite is a clipped draw (`production_post_view.vert.hlsl`; `scene.c:9715-9757`).
  - The halves are widened by a quarter of a render pixel (`btbh_gpu.h:2921-2931`).
  - The reflections keep their reads `0.5 + |nx| + |ny|` render pixels inside the view's edge, and fade within 24
    (`production_ssao_resolve.frag.hlsl:94-106`, `:532-541`). n is the line's unit normal in render pixels.
- **Uniform blocks:** SDL's Vulkan backend binds each one with a 4 KiB range (`MAX_UBO_SECTION_SIZE`,
  `external/SDL/src/gpu/vulkan/SDL_gpu_vulkan.c:71`, `:5183`), whatever `UNIFORM_BUFFER_SIZE` says (32 KiB,
  `external/SDL/src/gpu/SDL_sysgpu.h:35`).
- **The rules this must keep:**
  - the cost log's groups and pass steps (`internal.h:623-683`);
  - the prewarm (`btbh_gpu.h:2572-2617`) and the lazy creation log (`:3090-3169`): nothing made after the first frame;
  - unknown `BTBHGPUScenePost.flags` bits are refused (`:1771-1774`).

## Designs considered

### How v2 gives the sources

| | What v2 builds | What the renderer adds | GPU with sources | Identity | Verdict |
| --- | --- | --- | --- | --- | --- |
| **(a) REFRACT batches, as they are** | Meshes (a ring with outward normals, a sphere); a batch or instances per effect | Nothing | The copy (0.14 ms at 4K High) plus fill at about 20 noise lookups a pixel | Free | Works today. But sources don't add, reads cross the split line, and every frame with one pays the copy |
| **(b) A surface variant drawing into an offset field**, with its own fragment shader (as `LIT_PARTICLE`'s) | Meshes or particle quads; the shape from their UVs or an offset texture | A geometry phase into the field. Pipelines keyed by each batch's state and the field's format. A full-size field if it is to be depth-tested | Fill only | New files | Phase 2 at most, if v2 wants authored offset sprites (question 1) |
| **(c) An analytic source list (recommended)** | 64 bytes a source, a frame: centre, radius, width, strength | Instanced draws into a half-size field (one per 60 sources, per view); a variant of the composite | Fill at a quarter of the pixels | New files; a frame without sources runs 0.16.1's passes | **Recommended** |

**Why (c):**
- v2's sources are geometric (a ring's radius and width, an orb's radius), short-lived and numerous. A struct is less to
  build each frame than a mesh and an instance buffer.
- The shapes are exact at any size.
- Nothing that exists changes.

The cost is that the renderer owns the shapes: a new look is a new shape or mode.

**World positions, not screen positions.**
- Each view of a frame projects the sources with its own camera and clips them to its half.
- A `FLAT` ring lies on the ground in perspective.
- Screen positions would have v2 project every source, and split them by view itself.

### Where the offset is applied

| | Bloom | Fog | FSR 3 | HDR (0017) | Extra GPU | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| **(1) Inside the HDR composite (recommended)** | Read at the displaced point: a highlight and its glow move together | At the displaced point's depth | The reactive mask is written in the same pass | Its gain reads the displaced composite | One read of the field a pixel; no new full-screen pass | **Recommended** |
| (2) A pass before bloom | Made from the displaced picture: exact | At the undisplaced depth: a displaced edge takes its neighbour's fog | As (1) | As (1) | A full-screen HDR read and write: 0.14 ms at 4K High, 0.02 at the Deck preset (59 and 9 MB) | Rejected: it gains only the outer part of a glow (below) |
| (3) After the transparent pass, as refraction | Made from it | As (2) | As REFRACT | As (1) | A copy | Design (a) above |
| (4) After the tone map, before the anti-aliasing | Already added: moves with the picture, as in (1) | Already applied: moves with the picture | Needs the reactive mask too | Bends RFC 0017's extended values in the tail | A variant of the output pass | No gain over (1), and an 8-bit picture to filter in SDR |
| (5) After the upscaler | As (4) | As (4) | Never sees it | Bends the RGBA16F frame | At the output size: 8.3 Mpx at 4K, 2.2 times the render's. A pass after each of the four upscale paths and the mirror | Rejected |

**(1) in detail.** Bloom is made from the undisplaced picture and read at the displaced point. Inside a source a
highlight and its glow move together. A glow that spreads past a source's edge keeps its outer part where it was, by at
most the displacement (6-20 px at 1080). (2) would fix that for 0.14 ms at 4K.

**Before the anti-aliasing and the upscaler.**
- FXAA, SMAA, CAS and FSR 1 see the displaced picture as any other: nothing to change.
- **FSR 3** sees content move without motion. A pixel inside a ring shows the scene from `d` away, which its motion
  vector doesn't describe. So the history reprojects wrong, and a growing ring or a boiling haze would leave a trail.
  The composite marks such pixels reactive, by REFRACT's rule, so FSR 3 keeps less history there.
- **Displaced motion vectors** (a pixel takes the motion of the point it shows) would help a lens over a scrolling
  scene, but not a changing field. The composite can't read and write the velocity target in one pass, so it would
  need a copy. Not in phase 1.

### The field

| | Channels | Size | Memory at 4K High / the Deck preset | Verdict |
| --- | --- | --- | --- | --- |
| **Half the render size each way, RGBA16F (recommended)** | rg the summed displacement in render pixels; b each source's chroma times its displacement's length, summed; a the lengths summed | ceil(W / 2) x ceil(H / 2) | 7.4 MB / 1.15 MB | **Recommended** |
| Full size, RG16F | No chroma per source | W x H | 14.7 / 2.3 MB | Twice the memory and four times the fill, for nothing: the fields are smooth |
| Full size, RGBA16F | As above | W x H | 29.5 / 4.6 MB | The same, twice over |

**Half size is enough:**
- v2's camera shows about 33.3 cells across the target: 77 render pixels a cell at 4K High, 29 at the Deck preset.
- A ring band of 0.15 cells is still 2 field texels at the Deck preset.
- Haze cells are larger than that.

**RGBA16F** is a colour target and blendable on every SDL GPU backend, which matters because sources add with an
additive blend.

## Proposal

```c
/* Screen-space distortion (RFC 0018, asked for by Be The Bullet Hell v2): rings, discs and strips placed in the world for
 * one frame bend the scene behind them. They are drawn into an offset field at half the render size, and the HDR
 * composite shows the scene displaced by it, before the grading, the tone map, the anti-aliasing and the upscaling.
 * The canvas (the UI) is never displaced. A frame that draws no source is the same to the byte as without this. */

/* The most sources btbh_gpu_window_scene_set_distortion takes for one frame. */
#define BTBH_GPU_SCENE_MAX_DISTORTION_SOURCES 240u

/* BTBHGPUSceneDistortionSource.shape. r is a point's distance from position in the shape's plane (world units); for a
 * strip, from the segment between position and end. */
typedef enum BTBHGPUSceneDistortionShape {
    BTBH_GPU_SCENE_DISTORTION_RING = 0,    /* a band `width` wide whose middle lies `radius` from position */
    BTBH_GPU_SCENE_DISTORTION_DISC = 1,    /* a disc of `radius`: a blob, a lens */
    BTBH_GPU_SCENE_DISTORTION_STRIP = 2,   /* a capsule reaching `radius` each side of the segment position-end */
    BTBH_GPU_SCENE_DISTORTION_SHAPE_COUNT = 3
} BTBHGPUSceneDistortionShape;

/* BTBHGPUSceneDistortionSource.mode. */
typedef enum BTBHGPUSceneDistortionMode {
    /* The scene moves along the shape's outward direction on screen (away from position, or from the strip's segment).
     * strength above 0 pushes it out (a disc magnifies, a ring's band bulges outward); below 0 pulls it in (a disc
     * pinches). */
    BTBH_GPU_SCENE_DISTORTION_PUSH = 0,
    /* Heat haze: the scene shimmers by an animated noise; the shape is only its envelope. */
    BTBH_GPU_SCENE_DISTORTION_HAZE = 1,
    BTBH_GPU_SCENE_DISTORTION_MODE_COUNT = 2
} BTBHGPUSceneDistortionMode;

/* BTBHGPUSceneDistortionSource.flags. */
enum {
    /* The shape lies in the world's x-z plane at its height: a ring on the ground, an ellipse under a tilted camera.
     * Without it the shape faces the camera: a ring or a disc is a circle on screen, and a strip is a ribbon turned
     * about its segment toward the camera. */
    BTBH_GPU_SCENE_DISTORTION_FLAT = 1u << 0
};

/* One distortion source for one frame. 64 bytes on 64-bit (no pointers): position 0, radius 12, end 16, width 28,
 * strength 32, chroma 36, haze_scale 40, haze_speed 44, shape 48, mode 52, flags 56, reserved 60. A zeroed source draws
 * nothing (its radius and strength are 0). */
typedef struct BTBHGPUSceneDistortionSource {
    float position[3];   /* world: the centre (RING, DISC), or the strip's first end */
    float radius;        /* world units: RING to the band's middle, DISC its radius, STRIP its half-width. 0: nothing */
    float end[3];        /* world: the strip's second end. 0 for RING and DISC */
    float width;         /* world units: RING the band's whole width (0: nothing). 0 for DISC and STRIP */
    /* The largest displacement, in pixels of a frame 1080 pixels high: strength / 1080 of the frame's height, so it
     * reads the same at any resolution and render scale (12: 12 px at 1080p, 24 at 2160, 8.9 on the Deck's 800 lines).
     * PUSH: signed, as above. HAZE: the largest along each axis (the sign is ignored). 0: nothing. A PUSH source is
     * clamped where its picture would fold over itself (RFC 0018, "What a source does"). */
    float strength;
    /* 0..1: red displaced by 1 + 0.35 * chroma times the displacement, blue by 1 - 0.35 * chroma, so colours part along
     * the bend (as REFRACT's dispersion). 0: none. */
    float chroma;
    float haze_scale;    /* HAZE: noise cells per world unit (0: 1). 0 for PUSH */
    float haze_speed;    /* HAZE: cells per second the noise drifts up the screen (0: still). 0 for PUSH */
    uint32_t shape;      /* BTBHGPUSceneDistortionShape */
    uint32_t mode;       /* BTBHGPUSceneDistortionMode */
    uint32_t flags;      /* BTBH_GPU_SCENE_DISTORTION_* bits; 0: facing the camera */
    uint32_t reserved;   /* 0 */
} BTBHGPUSceneDistortionSource;

/* BTBHGPUScenePost.flags, appended after BTBH_GPU_SCENE_POST_SSR_FACING_HITS: */
    /* The frame draws the distortion sources set for it (btbh_gpu_window_scene_set_distortion). Without it they are
     * dropped undrawn: a preset's or a player's switch. With it, the prewarm also makes the distortion's pipelines and
     * field. With it and no source, the frame is as without it, to the byte. */
    BTBH_GPU_SCENE_POST_DISTORTION = 1u << 3

/* BTBHGPUSceneReport: the byte after upscale_mode, which was upscale_reserved[0] (the report keeps its size). */
    uint8_t upscale_mode;
    uint8_t distortion_sources_drawn;   /* the sources the frame drew in any view, after skipping and culling;
                                           0: no distortion pass ran */
    uint8_t upscale_reserved[2];

/* Sets the distortion sources of the window's next scene present (btbh_gpu_window_scene_present_instances,
 * _with_visibility or btbh_gpu_window_scene_present_views). They are copied at the call. That present takes them and
 * forgets them, drawn or not: the one after draws none unless this is called again.
 * - A second call before the present replaces the first; source_count 0 clears them. A refused present leaves them set.
 * - They are drawn only when the present's post has BTBH_GPU_SCENE_POST_DISTORTION, and not in the radiance cascades'
 *   debug views. Renders into targets and prewarms neither draw nor take them.
 * - A source is skipped (not drawn, not counted) when its radius, its strength, or a ring's width is 0. In each view it
 *   is drawn when its bounds meet the view's frustum: projected with the view's camera and clipped to its half.
 * - time_seconds is the haze's clock: any steadily growing seconds. Each source's drift is wrapped on the CPU, so the
 *   clock may grow without bound.
 * - Needs production_distortion.vert, production_distortion.frag, production_hdr_composite_distort.frag and
 *   production_hdr_composite_distort_reactive.frag beside the present's post fragment shader. A frame that draws a
 *   source without them fails with BTBH_GPU_INVALID_ARGUMENT, naming the file; so does a prewarm with the flag.
 * - Refused with BTBH_GPU_INVALID_ARGUMENT, and nothing is kept (the next present draws none):
 *   - a NULL window; sources NULL with a source_count above 0; a source_count above BTBH_GPU_SCENE_MAX_DISTORTION_SOURCES;
 *   - time_seconds not finite;
 *   - a source with a field that isn't finite; radius, width or haze_scale below 0; chroma outside 0..1; an unknown
 *     shape, mode or flag; reserved not 0; a field its shape or mode doesn't use that isn't 0 (end for RING and DISC,
 *     width for DISC and STRIP, haze_scale and haze_speed for PUSH).
 *   report may be NULL. Otherwise only its result and message are written; the message names the source and the field.
 * Call it on the thread that presents, at any time between presents. */
BTBHGPUResult btbh_gpu_window_scene_set_distortion(
    BTBHGPUWindow *window,
    const BTBHGPUSceneDistortionSource *sources,
    uint32_t source_count,
    float time_seconds,
    BTBHGPUSceneReport *report
);

/* Defined when btbh_gpu_window_scene_set_distortion, BTBHGPUSceneDistortionSource, its enums,
 * BTBH_GPU_SCENE_POST_DISTORTION and BTBHGPUSceneReport.distortion_sources_drawn exist. */
#define BTBH_GPU_SCENE_HAS_DISTORTION 1
```

`shape` and `mode` are `uint32_t` in the struct, not the enum types, so the layout never depends on an enum's size and
bindgen sees plain integers.

### What a source does

**The shape's plane and its axes:**
- A facing shape lies in the camera's plane through `position`: u along the camera's right, v along its up.
- A `FLAT` shape lies in the x-z plane at `position`'s height:
  - u is the camera's right, laid flat;
  - v is the camera's up laid flat (its forward when that is too short), so +v runs up the screen.
- A strip's band:
  - facing: across its segment and the view direction;
  - `FLAT`: across the segment, horizontal.

**The profiles.** Let `x = (r - radius) / (width / 2)` for a ring and `u = r / radius` for a disc or a strip. A source
does nothing where `|x| >= 1` or `u >= 1`.

| Shape | PUSH: the displacement along n | HAZE: the envelope |
| --- | --- | --- |
| RING | `strength * (1 - x^2)^2 * min(1, 2r / width)` | `(1 - x^2)^2 * min(1, 2r / width)` |
| DISC, STRIP | `strength * 3.4939 * u * (1 - u^2)^2`: 0 at the centre and the edge, 1 at u = 0.447 | `(1 - u^2)^2` |

- **n** is the screen direction in which r grows (its gradient on screen):
  - radial, for a facing ring or disc;
  - the ellipse's normal, for a `FLAT` one;
  - straight out from the segment, for a strip.
- **Rings:** `min(1, 2r / width)` keeps a newborn ring, one whose radius is still under half its width, from making a
  seam at its centre.
- **HAZE:** the displacement is `strength * envelope * (N_x, N_y)` on screen, x right and y up. Each N is a value
  noise in -1..1 over the shape's plane:
  - `haze_scale` cells per world unit;
  - drifting up the screen at `haze_speed` cells per second;
  - two octaves, the second at twice the frequency, drifting 1.7 times as fast and turned, so it doesn't read as a
    scrolling texture;
  - an integer hash with a period of 256 cells, so the CPU reference reproduces it exactly.
- **The fold clamp** (as built: "Phase 1 as built", 10): a PUSH source's `|strength|` is clamped so that the Jacobian of
  `p -> p - k d(p)` stays positive over the whole shape for every colour (red's `k = 1 + 0.35 chroma`), from the profile's
  steepest slopes and the shape's projection where it is thinnest and most slanted on screen.
  - Without chroma that is about 0.26 of a facing disc's or strip's radius on screen and 0.29 of a ring's width (0.32
    of the radius pulling a disc in); less with chroma and for a flat shape under a tilted camera.
  - Past it the picture would mirror over itself.
- **Sources add.** The sum is clamped to 64 pixels at 1080 lines, so a pile-up never tears the frame.

**What the composite does with it** (the distort variant, at each render pixel p):
- **The read:** p shows the scene at `p - d(p)`, d the field read bilinearly at p.
- **Chroma:** where the field's chroma is above 0, red is read at `p - d * (1 + 0.35 c)` and blue at
  `p - d * (1 - 0.35 c)`. c is the field's chroma channel divided by its length channel (the average of the sources'
  chroma, weighted by their displacement). Elsewhere there is one read.
- **Fog and bloom** are read at the displaced point (green's); **the vignette** at p. A read outside the frame clamps
  to its edge.
- **The reactive mask, under FSR 3:** `min(1, |d| / 6)` times `temporal_reactive_scale`, written with blend max over the
  transparent pass's mask, d the displacement the pixel shows (summed and clamped) in render pixels (REFRACT's rule;
  as built, "Phase 1 as built", 11). The motion vectors stay the scene's.
- **Scene views:**
  - Within 8 render pixels of a view's edge the displacement fades to nothing at `0.5 + 2(|nx| + |ny|)` pixels from it
    (2.5 for an upright or level line, 3.3 on a diagonal). Nearer than that, the half-size field's bilinear read could
    take the other view's texels.
  - A displaced read never comes nearer the edge than the reflections' margin, `0.5 + |nx| + |ny|`; it slides along
    the line instead.
  - The edge is the line widened by a quarter pixel, as 0.16.1 defines it.

**The composite on a frame with sources:**
- It runs whether or not fog, bloom and the vignette are on; their terms stay as set.
- With two views, each view's composite is the clipped variant. Two views never run FSR 3 (RFC 0014 phase 1), so that
  variant has no reactive sibling.

**Zero values and identity:**
- The struct is new, and a zeroed source draws nothing.
- The flag is a new bit. A present without it, or with it and no source drawn, runs 0.16.1's passes and pipelines
  (no field, no source pass, the plain composite) and draws the same image to the byte.
- The report's new byte is 0 then.

## Minimal example

```c
/* Each frame, from v2's effects: */
BTBHGPUSceneDistortionSource sources[3];
SDL_zeroa(sources);

/* A stomp's shockwave on the ground: 0.6 s, its radius 0 -> 9 cells, fading out. */
const float t = stomp_age / 0.6f;
sources[0].position[0] = stomp_x; sources[0].position[1] = ground_y; sources[0].position[2] = stomp_z;
sources[0].radius = 9.0f * t;
sources[0].width = 1.5f;
sources[0].strength = 16.0f * (1.0f - t);          /* px at 1080 lines */
sources[0].chroma = 0.3f;
sources[0].shape = BTBH_GPU_SCENE_DISTORTION_RING;
sources[0].flags = BTBH_GPU_SCENE_DISTORTION_FLAT;

/* Heat haze over a fire: a facing disc above it. */
sources[1].position[0] = fire_x; sources[1].position[1] = fire_y + 1.5f; sources[1].position[2] = fire_z;
sources[1].radius = 2.0f;
sources[1].strength = 2.5f;
sources[1].haze_scale = 1.5f;
sources[1].haze_speed = 0.8f;
sources[1].shape = BTBH_GPU_SCENE_DISTORTION_DISC;
sources[1].mode = BTBH_GPU_SCENE_DISTORTION_HAZE;

/* An energy orb's lens. */
sources[2].position[0] = orb_x; sources[2].position[1] = orb_y; sources[2].position[2] = orb_z;
sources[2].radius = orb_radius * 1.6f;
sources[2].strength = 8.0f;
sources[2].chroma = 0.5f;
sources[2].shape = BTBH_GPU_SCENE_DISTORTION_DISC;

BTBHGPUSceneReport report;
if (btbh_gpu_window_scene_set_distortion(window, sources, 3, game_seconds, &report) != BTBH_GPU_OK) {
    /* report.message names the source and the field */
}
post.flags |= BTBH_GPU_SCENE_POST_DISTORTION;     /* also in the post given to the prewarm at load */
/* ... btbh_gpu_window_scene_present_instances_with_visibility(...) or present_views(...), as every frame */
```

## Implementation

### Phase 1

| File | Change |
| --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | The struct, its two enums and flag, the post flag, the report's byte, the function, the macro |
| `src/gpu/btbh_gpu_distortion.h` (new) | The CPU reference: the profiles, the fold clamp, the noise and its hash, a source's bounds. The renderer uses it to validate and cull, and the tests to check the pictures |
| `src/gpu/btbh_gpu_window.c` | The pending sources on the window (a copy of up to 240, the clock) and the setter |
| `shaders/gpu/production_distortion.hlsli` (new) | The profiles and the noise, as the CPU reference has them |
| `production_distortion.vert.hlsl` (new) | A quad a source: six vertices from `SV_VertexID`, the source from `SV_InstanceID` in a block of 60. Facing or flat, bounded with a texel to spare, projected with the view's matrix (FSR 3's jitter included, as the scene's). The fold clamp. `SV_ClipDistance0` from the view's plane, 1 when it is zero (as the scene's vertex shaders) |
| `production_distortion.frag.hlsl` (new) | The profile or the haze, into the field |
| `production_hdr_composite.frag.hlsl` | `#if BTBH_DISTORTION` blocks; its compiled bytes stay 0.16.1's |
| `production_hdr_composite_distort.frag.hlsl`, `production_hdr_composite_distort_reactive.frag.hlsl` (new) | Two-line wrappers (`#define BTBH_DISTORTION 1` or `2`, then the `#include`). The blocks they turn on read the field at t3 and grow the uniform block by three float4 (the render size, the sum's clamp, the reactive scale, the view's plane in UV). With 2 the shader also writes the reactive mask (`SV_Target1`) |
| `src/gpu/btbh_gpu_internal.h` | `BTBH_GPU_COST_STEP_DISTORTION` |
| `src/gpu/btbh_gpu_scene.c` | Below |
| `CMakeLists.txt` | The four shaders in the list that CHIMERA, Tyrian3D and v2 read |

In `scene.c`:
- **Setup.** `distortion_active` is true when:
  - the post has the flag;
  - sources are set;
  - no debug view is on;
  - at least one source is left after skipping and culling. Each view tests each source's bounding sphere against
    its six planes on the CPU.
- **The field.** It is made on the first frame that draws a source, or by a prewarm with the flag. It is made again at
  a new render size and released with the runtime, as the refraction copy is (`:2933-2975`, `:16125`).
- **The source pass** runs at the start of the post chain (`:13379`), before bloom:
  - one render pass that clears the field;
  - per view: a block of 128 bytes (its matrix, its camera's right and up, the render size, its plane) and up to 60
    sources (3,840 bytes), so 3,968 bytes a push, under the 4 KiB range;
  - one instanced draw per 60 sources a view: four for 240;
  - additive blend (one, one) on all four channels, and no depth.
- **The composite.** On such a frame the distort variant takes the plain one's place.
  - Under FSR 3 it is the reactive variant, with the reactive texture as a second target (loaded, blended with max).
    That needs a two-target sibling of `btbh_gpu_scene_render_fullscreen` (`:8652`).
  - With views, it is the variant over `production_post_view.vert`, as `:13512-13537` draws the plain one.
- **The prewarm** (`btbh_gpu_scene_prewarm_internal`, `:14274`): with the flag, it makes the source pipeline, the
  composite variants the settings use, and the field. The lazy creation log marks them `prewarm_covers` 1.
- **The report:** `distortion_sources_drawn`.
- **The cost log:** the step `DISTORTION` (20, the next free one: RFC 0017's phase 1 took none; `COUNT` 21), in the post
  group, before
  bloom. The composite keeps `COMPOSITE`. A frame without sources prints no distortion step.
- **Debug:** `BTBH_DEBUG_DISTORTION=1` shows the field instead of the frame (red and green `0.5 + d / 32 px`, blue the
  chroma), for tuning.

**Estimate: 5-7 days.**

| Part | Days |
| --- | --- |
| Header, setter, CPU reference | 1 |
| Shaders | 1.5 |
| `scene.c`: the pass, field, variants, views, reactive target, prewarm, report, cost step | 1.5-2 |
| Tests: native, the GPU smoke with the CPU reference, the identity runs | 1.5-2 |
| CHANGELOG, docs, the Mac's check | 0.5 |

### Phase 2, only if v2 asks

| Item | Estimate |
| --- | --- |
| `DEPTH_TESTED`: a source hidden where the opaque scene stands nearer than its own surface (question 8) | 1 day |
| An offset sprite: a textured source, or design (b)'s surface variant (question 1) | 2-3 days |
| Displaced motion vectors under FSR 3, if trails show (question 4) | 1-2 days |
| A full-size field, if thin bands look soft | 0.5 day |
| Sources in renders into targets | 1 day |

### Cost

This PC, the RTX 3070 Ti. 4K High is a 2560 x 1440 render (FSR 3 Quality) with a 1280 x 720 field; the Deck preset is
a 960 x 600 render with a 480 x 300 field. "Typical" is 2 rings, 6 haze discs and 4 orbs, covering about 15 % of the
field; "worst" is a frame-wide haze with 20 overlapping rings.

| Part | 4K High | Deck preset |
| --- | --- | --- |
| **No source drawn, or the flag off** | **0** | **0** |
| Source pass, typical: the clear and the fill | about 0.02 ms | about 0.01 ms |
| Source pass, worst: 2.5 layers over the whole field | about 0.09 ms | about 0.02 ms |
| Composite: the field's read, the displaced reads, chroma's two reads where set; under FSR 3 the reactive write (7.4 MB) | +0.03-0.05 ms | +0.01 ms (FSR 1: no reactive) |
| **Total, typical / worst** | **about 0.06 / 0.15 ms: 0.7-1.8 % of the 8.2 ms frame** | **about 0.02 / 0.03 ms: 1-2 % of 1.6-2.2 ms** |
| Memory | 7.4 MB (16.6 MB at a native 4K render) | 1.15 MB |
| CPU | the copy at the call (up to 15 KB); culling up to 240 spheres a view; up to four pushes and draws a view: under 0.02 ms | the same |

- **For comparison:** REFRACT's copy alone is 0.14 ms at 4K High.
- **On real Deck-class hardware** (about 88 GB/s, five to seven times less than this PC's effective bandwidth), expect
  about 0.1 ms typical and 0.2 ms worst. v2's cost log on the Machine measures it.

### Risks

| Risk | What then |
| --- | --- |
| The plain composite's bytes change with the `#if` blocks | Caught by the hash check against 0.16.1's build; the variant then becomes a copy instead of a wrapper |
| FSR 3 trails behind a growing ring or in haze | `BTBH_DEBUG_MOTION_VECTORS=2` on the High preset at 4K. Then a lower threshold than REFRACT's 6 px, or phase 2's displaced motion vectors |
| Thin bands (under 2 field texels) lose their profile | A full-size field (phase 2): four times the memory and fill |
| The glow past a source's edge keeps its undisplaced part | Design (2), for 0.14 ms at 4K, if v2 sees it |
| Bullets inside a source are displaced | By design; v2 gates the sources (question 3) |
| Metal | Unverified until the Mac session's `check.sh` builds the metallibs and runs the smoke |
| Numbers shared with RFC 0017 and RFC 0019 (the lit smoke, drafted beside this one): the cost step, the post flag bit | Settled at merge, in landing order |

## Compatibility

- **Versioning:** minor (pre-1.0: "additive → minor or patch"), the release after 0.17.0's HDR by v2's order.
  - New: a struct, two enums, a flag, a function and a macro; a post flag bit; a reserved report byte given a meaning.
  - No struct changes size and no default changes.
  - New shader files; the plain composite's compiled bytes are unchanged.
- **Be The Bullet Hell v2 (Rust, bindgen):**
  - Regenerate the bindings. `upscale_reserved` becomes two bytes after `distortion_sources_drawn`; a literal that names
    it needs the new length.
  - The new enums are written by v2 and never read back, so `rustified_enum` is harmless.
  - Its effects code fills the sources each frame and sets the flag in the presets that want it.
  - Its shader list takes the four new files from `CMakeLists.txt`.
- **Be The Bullet Hell (v1, Swift), CHIMERA (Rust), Tyrian3D (C):** nothing to change. They rebuild their shaders with the
  C core, as every release; they never set the flag, so they never load the new files.

## Platforms and testing

- **Native:**
  - **`btbh_gpu_scene_order_test`:** `BTBHGPUSceneDistortionSource` is 64 bytes with the offsets above;
    `distortion_sources_drawn` sits at `upscale_mode` + 1, and the report keeps its size; the flag's value.
  - **The public header test:** the header alone as C11 and C++17.
  - **New `btbh_gpu_distortion_test`:**
    - every refusal of the setter, and that a refused call leaves nothing set;
    - on the CPU reference: each profile is 0 at its edges and C1 there, and peaks at 1 where stated;
    - at the fold clamp, `p -> p - d(p)` is monotonic along n on a dense line, for each shape facing and flat;
    - the noise stays in -1..1 with a period of 256;
    - the culling bounds.
- **GPU smoke, Windows/Vulkan (new `btbh_gpu_scene_distortion_smoke`):**
  - **Identity.** The flags, prewarm, world-detail, scene-target, runs, lit-particle and views smokes' images, byte for
    byte against 0.16.1's build, three ways:
    - no call;
    - the flag with no source;
    - the flag with sources that are all skipped (strength 0) or culled (outside the frustum).

    `distortion_sources_drawn` must be 0 in each. The plain composite's SPIR-V and MSL hashes must equal 0.16.1's.
  - **Each source against the CPU reference.**
    - The scene: an unlit plane facing the camera with a smooth test pattern (soft-edged 32 px checks, colours within
      0.1-0.8 so the tone map stays nearly linear); a 512 x 512 render at the output size; no anti-aliasing, no
      upscale; fog, bloom, the vignette and the LUT off.
    - The cases: RING, DISC and STRIP, each in PUSH and HAZE, facing and `FLAT`; a negative DISC; a RING with chroma
      0.6; a RING past the fold clamp.
    - The expected picture: the frame without sources, resampled on the CPU at `p - d_ref(p)`. `d_ref` is the CPU
      reference evaluated at the field's texel centres and read bilinearly, as the GPU reads it; with chroma, red and
      blue are resampled at their own offsets.
    - To pass: within 2 levels of 255 in 99.5 % of the pixels, and 6 everywhere. The pictures are kept as PNGs for
      review.
  - **Views.** A vertical split, with a ring across the line:
    - Each half must equal its own one-view render (joined on the CPU) farther than 8 render pixels from the line.
    - With view 0 seeing a red ground and view 1 a blue one, no pixel of view 0's half takes any blue.
  - **FSR 3.**
    - The reactive mask (`BTBH_DEBUG_MOTION_VECTORS=1` shows FSR 3's inputs) holds `min(1, |d| / 6)` inside the ring and
      the transparent pass's values elsewhere.
    - A ring growing over 30 frames under a panning camera: no NaN; its trail is checked by eye with
      `BTBH_DEBUG_MOTION_VECTORS=2`.
  - **Prewarm.** With the flag in the prewarm, the first frame with sources makes nothing (the lazy creation log is
    empty). Without it, the log names the pipelines and the field, with `prewarm_covers` 1.
  - **Refusals:** a missing shader file fails the frame that draws a source, and the prewarm with the flag.
- **The cost:** `BTBH_GPU_COST_LOG=1 BTBH_GPU_COST_PASSES=1` at 4K High and at the Deck preset, with no source, one ring,
  the typical set and the worst case. Under the machine's perf lock.
- **macOS/Metal:** the Mac session's `check.sh` builds the metallibs, and the smoke runs there. FSR 3's part is Vulkan
  only until Metal has FSR 3.
- **Linux and the Deck:** through v2.
- **Games:**
  - v2's bench, by stage, with the effects on and off, and its look at a stomp, a fire and an orb under each preset;
  - CHIMERA, 0 pixels different;
  - Be The Bullet Hell, its Swift suite.

## Open questions for v2

Each has the renderer's recommendation; v2 answers per number.

1. **Shapes, not meshes.** The sources are analytic rings, discs and strips set each frame (64 bytes each), not batches
   with meshes or particle quads. Recommended: yes. Does v2 need an authored shape soon (a sprite of offsets on its
   particle quads)? Recommended: not now. It would be phase 2.
2. **Strength in pixels of a 1080-line frame,** as REFRACT's, clamped where the picture would fold. Recommended: yes.
   The alternative is a fraction of the shape (1 = the fold limit), which grows with the shape on screen. Suggested
   starting values: rings 10-20, orbs 6-12, haze 1.5-3.
3. **Everything in the scene bends, the UI never.** Inside a source, opaque surfaces, transparent effects, refraction,
   bloom's glow and bullets are displaced; the canvas is drawn after. Recommended: accept it, keep sources short and off
   the player's own lane. If bullets must stay crisp inside a source, that needs a mask the bullets' batches write (a
   stencil bit, or a target in the transparent pass): phase 2 at the earliest.
4. **FSR 3:** displaced pixels are marked reactive by REFRACT's rule (fully at 6 render pixels of displacement, times
   `temporal_reactive_scale`, which v2 sets to 1), and the motion vectors stay the scene's. Recommended: yes. v2 then
   looks at a stomp's ring and a fire's haze under FSR 3 (High at 4K, and Medium) for trails, with
   `BTBH_DEBUG_MOTION_VECTORS=2`.
5. **The Deck preset:** about 0.02 ms on this PC, about 0.1 ms on a real Deck-class APU. Recommended: on in every preset,
   with the same sources.
6. **A player's switch.** Without `BTBH_GPU_SCENE_POST_DISTORTION` the sources are dropped undrawn, so a Graphics or
   Accessibility row ("Screen distortion: on / off") needs nothing more from the renderer. Recommended: add one, on by
   default, for players sensitive to motion.
7. **The desert's haze.** Recommended: a few large `FLAT` HAZE discs round the camera's target (strength 1-2,
   `haze_scale` 0.5) that follow it, not a frame-wide mode. Does the desert want the haze stronger far away (up the
   screen)? That would be a flag later.
8. **Depth.** Phase 1 bends everything in a source's footprint, including what stands in front of it (a tower in front
   of a fire's haze). Recommended: accept that for the top-down camera. A `DEPTH_TESTED` flag (the source hidden where
   the opaque scene is nearer) comes in phase 2 if the play-test shows it.
9. **Chroma per source.** Recommended: on rings (0.2-0.4) and orbs (0.4-0.6), off for haze, where it would shimmer in
   colours.
10. **240 sources a frame.** Recommended: enough, if v2 puts a lens on the big orbs only, not on every orb bullet. Is it
    enough?
11. **The split.** The displacement fades within 8 render pixels of the line (to nothing 2.5-3.3 pixels from the view's
    edge), and no read crosses it. v2's divider covers 2 render pixels or more each side. Recommended: accept.
12. **The haze's clock.** Recommended: v2's game time, so the haze stops with the pause. Any growing seconds work.
13. **The order.** Recommended: after HDR's phase 1, as v2 asked. Its cost step is then 21 (as built: 20, HDR took
    none).

## Decisions (v2, 2026-11-28)

v2 took the recommendation on all 13 questions:

1. **Shapes set each frame;** no authored offset sprites now (phase 2 only if asked).
2. **Strength in pixels of a 1080-line frame,** clamped at the fold. Starting values rings 10-20, orbs 6-12, haze
   1.5-3, tuned by v2 in a play-test.
3. **Everything in the scene bends, the UI never.** v2 keeps sources short-lived and off the kaiju's own lane, and judges
   bullet readability inside rings in a play-test before asking for a bullet mask.
4. **FSR 3: reactive by refraction's rule.** v2 checks a stomp's ring and a fire's haze for trails at 4K High and
   Medium with `BTBH_DEBUG_MOTION_VECTORS=2`.
5. **On in every preset,** with the same sources.
6. **A "Screen distortion: on / off" row** in v2's options, on by default.
7. **The desert's haze:** a few large flat HAZE discs following the camera's target; no stronger-with-distance flag for
   now.
8. **Not depth-tested in phase 1;** `DEPTH_TESTED` only if a play-test shows towers bending in front of a fire.
9. **Chroma** on for rings (0.2-0.4) and orbs (0.4-0.6), off for haze.
10. **240 sources are enough:** v2 lenses only big orbs and portals, not orb bullets, and ranks and culls its rings and
    hazes.
11. **The split's 8-pixel fade** is accepted.
12. **The haze's clock** is v2's game time, so it stops on pause.
13. **After HDR output's phase 1.**

Order (v2): HDR output (0.17.0), this RFC's phase 1, volumetrics phase 1 (RFC 0019), then half-resolution SSR and the
rest of v2's list.

## Phase 1 as built (feature/0.18.0-distortion)

Where the implementation departs from the text above, and why.

1. **The cost step is 20, not 21.** RFC 0017's phase 1 added no step (its encode is in the UI group), so
   `BTBH_GPU_COST_STEP_DISTORTION` takes the next free value (`BTBH_GPU_COST_STEP_COUNT` 21). A pass frame prints
   `distortion_ms` only when it drew the source pass, so lines without distortion keep 0.17.0's fields.
2. **The haze's second octave drifts three of its cells for each of the first's** (1.5 times as fast in world units),
   not 1.7 times: with a whole number of cells both octaves repeat when the drift wraps at the lattice's 256 cells, so
   a growing clock never jumps.
3. **The CPU works out each source's plane per view** (its origin and unit axes, the fold clamp in render pixels, the
   wrapped drift: `btbh_distortion_pack` in `src/gpu/btbh_gpu_distortion.h`), so the vertex shader only places a quad.
   The pending sources and their records live on the window and its runtime; the setter is in `scene.c` (not
   `window.c`).
4. **The outward direction** is r's gradient in the plane carried to the screen by the plane coordinates' derivatives
   (`ddx`, `ddy` of the local point, which are smooth), not r's own derivatives, which kink at a disc's centre and a
   strip's axis.
5. **The identity runs' third way** is `BTBH_SMOKE_DISTORTION=1` in the smokes' presents
   (`Tests/btbh_gpu_smoke_present.h`): the flag and two sources that draw nothing, one skipped and one culled, on every
   frame of every smoke that presents through it.
6. **"A refused present leaves them set"** is precise as: a present refused before it begins (its arguments, batches,
   views or cameras invalid, or a scene waiting for its canvas) leaves them set; one that fails later (a missing shader
   file included) has taken them.
7. **The flags smoke's unknown post bit** moves from bit 3 (now `BTBH_GPU_SCENE_POST_DISTORTION`, which a render into a
   target takes and draws nothing with) to bit 4.
8. **The frame bench** (`Tests/btbh_gpu_frame_bench.c`) gained the modes `dist-none`, `dist-off`, `dist-typical` and
   `dist-worst` and the preset `4k` (v2's 4K High: 3840 x 2160 from 2560 x 1440 with FSR 3) for the cost below.
9. **The cost, measured** (the frame bench on this PC's RTX 3070 Ti, under the machine's perf lock, four windows
   interleaved over 120 rounds of 8 frames: `dist-none` with v2's bloom, `dist-off` the flag and no source,
   `dist-typical` 15 % of the frame, `dist-worst` 91 % at 1.8 layers; medians with the 10th and 90th percentiles):

   | | Deck preset (960 x 600 to 1280 x 800, FSR 1) | 4K High (2560 x 1440 to 3840 x 2160, FSR 3) |
   | --- | --- | --- |
   | Frame, no source / flag alone | 1.432 ms (1.396-1.482) / 1.432 (1.395-1.517) | 5.475 (5.429-5.694) / 5.476 (5.425-5.704) |
   | Frame, typical / worst | 1.438 (1.407-1.500) / 1.451 (1.415-1.512) | 5.495 (5.437-5.724) / 5.469 (5.421-5.698) |
   | `post` group, none / typical / worst | 0.279 / 0.287 / 0.298 ms | 2.510 / 2.521 / 2.507 |
   | Source pass (`distortion_ms`), typical / worst | 0.000 / 0.010 ms (floor noise, about +-0.02) | 0.010 (-0.012-0.034) / 0.051 (0.027-0.075) |
   | Composite (`composite_ms`), none / typical / worst | 0.005 / 0.005 / 0.013 ms | 0.072 / 0.080 / 0.105 |

   So the distortion costs about 0.01 ms typical and 0.02 ms worst at the Deck preset here (the frames' and the post
   group's medians), and about 0.02 ms typical and 0.08 ms worst at 4K (the source pass and the composite's steps; the
   whole frames' medians move within their noise there), under the estimates above (0.02 / 0.03 and 0.06 / 0.15). With
   no source it costs nothing measurable, and its frames are 0.17.0's to the byte.

The review of `8e036913` (an independent pass) found five things, fixed in the next commit:

10. **The fold clamp now holds over the whole shape for every colour.** The first clamp measured the shape's shorter
    screen axis at its centre and ignored chroma: a flat disc under a 45 degree camera folded along its long axis (the
    tangential stretch grows with the ellipse's aspect), red's 1 + 0.35 chroma passed the limit for v2's chroma, and
    the far side of a large flat shape is thinner. With the projection locally A (singular values a >= b) and the
    displacement m(r) along r's gradient on screen, the Jacobian's determinant is det(A) (1 - k m' / l)(1 - k (m / r)
    l^3 / (a b)^2), l in [b, a]: one-to-one while k max(m') < b and k max(m / r) < b^2 / a (a pull: k max(-m') < b).
    `btbh_distortion_pack` takes a and b at the shape's centre and at its quad's corners and edges (the thinnest and most
    slanted), the profile's slopes analytically (a disc, a strip) or sampled over the band (a ring, a newborn one too),
    k = 1 + 0.35 chroma, at 0.9 of the limit. `btbh_gpu_distortion_test` checks the determinant at every pixel of 84
    cases (each shape and a large one, facing and flat under a 45 degree camera, pushed and pulled, chroma 0, 0.6 and 1,
    each colour), and that 2.5 times the clamp folds a flat disc.
11. **The reactive mask** takes the length of the displacement the pixel shows (summed, clamped), as refraction does,
    not the sum of each source's length: sources that cancel aren't reactive.
12. **Culling by the view's half:** each view culls a source by its frustum and by its half of the frame (the clip
    line as a world plane through the camera, a sphere test), so a source in view A's frustum that lands wholly on B's
    side is neither drawn nor counted, and a frame whose sources all land nowhere is 0.17.0's. A PUSH source whose
    centre is behind the camera isn't drawn either (its clamp has no scale there).
13. **A pixel with no displacement reads itself.** The composite moved every read off the line to the reflections'
    margin, the pixel's own too, so on a split frame with a source the column each side of the line blended with its
    neighbour; now only a displaced read is kept inside.
14. **Two clock notes in the header:** `time_seconds` is a float, so past about 36 hours (2^17 s) its steps pass a
    60 Hz frame and the haze moves in steps (a game that runs longer restarts its clock); and the drift is time times
    `haze_speed`, so a speed changed between frames moves the pattern at once.

After the fixes the checks pass as before (identity in every mode, 0 px different), the fold check's lowest determinant
is 0.010 over 2.2 million pixel checks, and the smoke's band by the line and its source past the line are byte for byte
the frames without them. The cost in 9 was measured before the fixes and not again: they change no pass's work on the
GPU (the clamp and the culling run in the pack, 57 microseconds of CPU for 240 sources in two views; the composite only
skips the keep-inside for undisplaced pixels).

