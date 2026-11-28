# RFC 0020: A cover by facing (snow, sand, mud) on the batches that ask, and still ice on the water

- **Status:** Accepted on 2026-11-28: v2 answered all 17 questions, each as recommended (see "Decisions"). Cover
  phase 1 and the still ice ship in one release after the distortion pass (RFC 0018), before volumetrics phase 1.
  Both implemented on `feature/0.18.0-cover-ice` (from `feature/0.18.0-distortion`) for 0.18.0, not yet released, in
  shader variants by decision 14; where it departs from this text, see "Phase 1 as built"
- **Requested by:** Be The Bullet Hell v2, through its biomes and effects agents after the owner's seventeenth play-test
  (2026-11-28). The biomes agent's renderer requests 1 and 2 (`v2/docs/seventeenth-playtest-2026-11-28.md`, "Biomes",
  on branch `p23-biomes`): "A cover by facing: snow or sand on every up-facing surface of the kit (ledges, sills,
  cornices, pitched roofs by slope), with a noisy edge" and "Frozen water: an ice surface (cracks, frost) for the water
  batches". The effects agent's (the same file on main, "Effects (p23-fx2)", open questions): lit, normal-mapped ground
  decals, and a decal batch whose instances pick an atlas cell with the GPU height lift. Rain streaks are the biomes
  agent's request 3
- **Affects:**
  - Be The Bullet Hell v2 adopts both (opt-in).
  - Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D change nothing. A window that never sets a cover and draws no
    `ICE` batch draws byte for byte as the release before.
  - Six shader files change their SPIR-V and MSL but not their pictures: `production_model.frag` and the five that
    include it (`_2d_shadows`, `_unshadowed`, `_rc`, `_rc_2d_shadows`, `_rc_unshadowed`). Two includes are new. No
    shader file that is loaded is new, so the shader list in `CMakeLists.txt` that CHIMERA, Tyrian3D and v2 read is
    unchanged.
- **Issue:** none; v2 answers through its session
- **Depends on:** nothing. It only has to be ordered with:
  - **RFC 0018 (distortion, accepted).** v2 asked for these right after it. Nothing is shared.
  - **RFC 0019 (volumetrics, accepted for phase 1).** These go before it if they are small (v2's order). 0019 takes
    `flags2` bit 1 (`IN_VOLUMES`), so the cover takes bits 2 and 3 whichever lands first. 0019's phase 2 frees a sampler
    in the full scene shader; a textured cover or a melt mask (later, if asked) would want one too.
  - **RFC 0017 (HDR, accepted).** Nothing shared: the cover and the ice are surface properties, lit before the tone map.
  - **RFC 0014's views, RFC 0015's frame thread, RFC 0011's runs, RFC 0012's lift.** Covered under "What exists today"
    and "Proposal".
- **Sources:**
  - Line references are master `73dcb985` (v0.16.1 plus accepted RFC docs; its code is v0.16.1's).
  - Short names: `scene.c` for `src/gpu/btbh_gpu_scene.c`; `btbh_gpu.h` for `include/btbh_gpu/btbh_gpu.h`;
    `model.frag` for `shaders/gpu/production_model.frag.hlsl`; SDL is `external/SDL` at `release-3.4.2`.
  - v2: main `c4a9cfc40` (the water, the kit, the scars), and the biomes on branch `p23-biomes` at `a5b35c9d2` (not yet
    on main), paths under `v2/`. Read only.
  - Every GPU cost here is an estimate, scaled from RFC 0009's measured cost of the facade's procedural grain (pattern
    3: about 700 ALU instructions a pixel, +0.17-0.18 ms a 1080p screen and +0.05 ms at 960 x 600 on this PC's RTX
    3070 Ti, `0009-world-detail-table.md:684-700`) by instruction count. None is measured.

## Need

### What the player should see

The owner will judge the ice biome by these two. The biomes agent: "Snow is held at V 0.21 by the palette rule, so by
night and at dusk the ice reads mostly through its snowfall, its white roofs and its frozen water; a cover by facing
(request 1) would help it most."

| | Today (v2's workaround) | With this RFC |
| --- | --- | --- |
| **Ice** | Roof and ground colour groups painted toward snow, whole: ledges, sills, cornices and cars stay bare; a pitched roof is as white as a flat one; it reads as grey concrete | Snow on every face that turns up, by its slope, with a noisy edge: on the ledges and sills, thinning on steep roofs; slush on the roads |
| **Desert** | Sand painted over the kerbs, roads and squares | Sand dusted on whatever faces up, thinning softly at its edge |
| **Jungle and mud** | Mud where the lawns were; moss painted on walls and roofs | Mud on the ground with glossy puddles; moss on the tops of things |
| **Monsoon flood** | Wet streets (wetness 1, the rain's rings) | Wet mud on the ground; in phase 2 a flood line up the walls |
| **Frozen water** | v1's water maps with the ripples at 0.08, no drift or glints, a pale tint, roughness at least 0.5: matt and pale | Still ice: plates parted by cracks that catch the light, frost in patches, glossy between, reflecting the city |

### The asks and their workarounds

1. **A cover layer** on the kit and the terrain: snow, sand or mud on every up-facing surface by facing, with a noisy
   edge and a height or amount control. "This is the one that matters most for the look." Workaround: whole roof and
   ground materials recoloured.
2. **A frozen-water surface:** flat and glossy with cracks, no ripples. Workaround: the water's ripples, drift and
   glints off, tinted pale.
3. Later: rain streaks drawn by the renderer: depth-tested, sized in pixels, lit by the lamps at night. Workaround: 360
   instanced strips.
4. Later: lit, normal-mapped ground decals, so cracks catch light on their edges. Workaround: plain dark decals.
5. Later: one ground-decal batch whose instances each pick an atlas cell (as lit particles' `uv_rect`, RFC 0013), with
   the GPU height lift (RFC 0012). Workaround: three textures and three draws.

v2 wants estimates for 1 and 2. Items 3-5 wait for volumetrics unless one falls out of 1 or of 0019 for free ("Items
3-5", below).

### The budget

- **4K on the TV:** High is FSR 3 Quality, a 2560 x 1440 render. Its GPU frame is about 8.2 ms in the boss fight (v2's
  cost log; 7.5-7.7 ms in the biome stages).
- **The Deck preset:** 960 x 600 rendered, FSR 1 to 1280 x 800, Low's shadows (a 1024 sun map), HBAO
  (`v2/crates/v2-game/src/graphics.rs:1-23`). Its whole GPU frame is about 1.6-2.2 ms (v2's cost log on this PC:
  1.2-1.5 ms in play; the biome stages 1.19-1.34 ms).
- v2's camera shows about 33.3 cells across: 77 render pixels a cell at 4K High, 29 at the Deck preset (RFC 0018).

### The palette rule

v2 keeps every painted colour within its lands' rule: V at most 0.40, S at most 0.50, and a distance from every gameplay
colour under the four visions (`biomes.rs`'s test). The renderer draws the cover's colour as given; v2's test must take
the layers' colours, and a cover changes how much of the frame takes them (question 6).

## What exists today

### The scene fragment shader's material path

One function shades every batch the full scene shader draws: opaque and transparent, lit and unlit, static, skinned,
lifted and with a table. They differ only in the vertex shader and in the six wrappers' `#define`s
(`production_model_unshadowed.frag.hlsl:1-2`, `_rc.frag.hlsl:1-4` and so on). `shade_fragment` (`model.frag:2342`):

| Step | Where | What a cover needs from it |
| --- | --- | --- |
| The position's derivatives, before any discard | `:2354-2355` (`btbh_position_dx`, `_dy`) | The noise's footprint, so its fine octave fades before it aliases |
| The material's samples (albedo, emission, normal, ORM) | `:2386-2410` | |
| The world detail or its table | `:2411-2422` (`apply_world_detail`, `:2244-2293`; `_table`, `:2304-2340`) | The table's entry index per triangle (`btbh_world_detail_table_unpack`) |
| The surface variant | `:2423-2429` (`apply_surface`, `:294-...`: water `:344-397`, pixel water `:537-613`, glints `:614-...`) | Where still ice goes: a case that replaces the normal map and ORM, as water's does |
| The procedural detail and the rain's rings | `:2430-2431` | |
| The only discard (cut-off, dig mask, coverage cut) | `:2433-2436` | Nothing after it may take a derivative |
| The normal: mapped, the details' tilts, turned to the side seen | `:2437-2461` | **The cover goes here**, with the vertex normal and the shading normal both at hand |
| Occlusion, roughness (with the wetness, `:2205-2237`), metalness, F0 | `:2464-2476` | The cover's blend comes before F0 is taken |
| Unlit (`:2479-...`) or lit shading, then the targets | `fragment_output` `:1787-1811`; `rc_material_output` `:1838-1876`; `rc_scene_targets` `:1890-...` | Everything downstream sees the covered values |

### What the targets carry, and who reads them

- **`ambient_specular.a` is the roughness** and `normal.a` the occlusion (`model.frag:1801-1808`); the `_rc` variants
  write albedo, F0 and roughness to the cascades' targets (`:1858-1873`).
- **SSR** skips a pixel at or above `ssr_max_roughness` and fades from half of it
  (`production_ssao_resolve.frag.hlsl:397-404`, `:531`). v2 sets 0.35 (`v2/crates/v2-game/src/visuals.rs:921`). So snow
  must be rough (never reflects) and ice glossy (reflects); today's frozen water at 0.5 reflects nothing.
- **HBAO and SSAO** read the normal target. The radiance cascades' bounce light reads the albedo there.
- A cover applied before these outputs reaches every reader with no change to them.

### What is free

| | State | For this RFC |
| --- | --- | --- |
| `BTBHGPUSceneGeometryBatch.flags2` | Bit 0 `KEEP_CASCADES_LOOK` (`btbh_gpu.h:1346-1362`); RFC 0019 takes bit 1; unknown bits refused (`scene.c:5395`); in the run key (`scene.c:5715`) | Bits 2 and 3: a batch's choice of layer, with no struct change and runs intact |
| `BTBHGPUSceneWorldDetailTableEntry.flags` | Must be 0 (`btbh_gpu.h:1078`; refused, `btbh_gpu_world_detail.h:44`, `:206-208`) | Phase 2: an entry's own choice, for v2's merged distance models |
| `BatchExtension.world_detail_mode.zw`, `WorldDetailTable.world_detail_table_layers[].zw` | 0 (`model.frag:126-128`, `:139-140`) | The batch's and the entries' choice reach the shader with no new push |
| Fragment samplers | SDL allows 16 a stage (`external/SDL/src/gpu/SDL_sysgpu.h:29`). The full variant binds all 16 (t0-t15, `model.frag:26-111`; `scene.c:3909-3919`); `_2d_shadows` 15; `_unshadowed` 10 | **None in the full variant.** A cover texture would need one freed first (RFC 0013, "What the last slot costs"). Hence a procedural cover |
| Fragment uniform buffers | SDL allows 4 (`SDL_sysgpu.h:32`). The scene shader uses b0 `MaterialPBR` (3,152 bytes, `scene.c:111-159`), b1 `BatchExtension` (80, `:170-175`), b2 `WorldDetailTable` | **b3 is free:** the cover's 128 bytes, pushed once a pass, with no change to `MaterialPBR`'s layout |

### Pipelines

- Model pipelines are keyed by state: `[colour targets][blend][depth][transparent][cull][stencil][fast unlit]` per
  shadow set, skinning, table and lift (`scene.c:3438-3522`).
- The surface variant isn't in the key, except `LIT_PARTICLE` (its own shader) and `REFRACT` (its own phase).
  `WATER`, `PIXEL_WATER` and `GLINTS` are cases of `apply_surface` on the pipeline the same batch has without them.
- The fast unlit shader draws transparent unlit batches with no variant and no world detail (`scene.c:1608-1622`). It
  has its own pipelines and shader.

### v2's water, roofs, ground and decals today

| What | Where | How |
| --- | --- | --- |
| The rivers and canals | `v2/crates/v2-render/src/scene.rs:2042-2055` (and the outskirts' ring, `:3242-3255`) | A lit material: v1's foam map as the albedo, its normal map drifting by a UV offset, roughness by the mood, receive-only AO, no shadow, a tint per instance (`WaterLook`, `:1172-1188`). `SURFACE_WATER` only when the maps are missing (`:1927`) |
| The ice biome's water | `p23-biomes`: `v2/crates/v2-game/src/biomes.rs`, `mood`'s frozen branch | `water_normal` x 0.08, `water_flow` 0, roughness at least 0.5, a paler tint, no glints, frost on the glass |
| Frozen gameplay cells | `scene.rs:3286-3301` | An ice tile over the water: lit, roughness 0.35 |
| Roofs, walls, the ground | `v2/crates/v2-render/src/kitruns.rs:81-125` | Lit. Full models: a draw per surface class (roof, tiles, glass, paving, grass...) with its own world detail (`scene.rs` `kit_surface`). Distance models (the Deck preset's in play): merged, one table entry per vertex. Street-level pieces lifted by the Height terrain |
| A biome's look | `p23-biomes`: `v2/crates/v2-data/src/biomes.rs` (`Paint`) | A colour group moves toward the biome's colour: whole roofs and grounds |
| Ground decals | `v2/crates/v2-render/src/scars.rs:458-459` | Unlit 2, alpha-blended, three textures (burn, crack, line) and three draws |

### The rules to keep

- **Runs (RFC 0011):** v2 draws its kit through same-material runs. A choice per batch or per material keeps them; per
  instance would too (instance data), but the instance's custom floats are taken (the light factor, UV rects, seeds).
- **Views (RFC 0014):** anything world-space is the same in both views.
- **Prewarm and lazy creation:** nothing made after frame 1 (`btbh_gpu.h:2572-2640`; the lazy creation log,
  `:3090-3169`).
- **Identity:** a game that doesn't opt in draws byte for byte as before.
- **Pipeline count:** new shader variants multiply pipelines.

## Designs considered

### Where a cover is set

| | Per frame | Per batch | Runs | Struct growth | Verdict |
| --- | --- | --- | --- | --- | --- |
| (a) Recolour the materials whole (today) | | The material | Intact | None | No facing: ledges bare, pitched roofs as white as flat ones |
| (b) A pointer on the batch to a cover struct, as `world_detail` | Host-owned, read at the present | Any number of covers | The pointer joins the run key | The batch grows 288 → 296 bytes: every host's layout | More than v2 needs, and an 80-byte push becomes 144 per run |
| (c) An amount per instance (a custom float) | | Per instance | Intact | None | The custom floats are taken. A question for later (question 15) |
| **(d) Two layers on the window, two bits of `flags2` per batch; in phase 2 an entry's choice in a table (recommended)** | A setter, copied at the call, kept until the next | A layer by bits | `flags2` is in the run key already | None | **Recommended** |

**Why two layers.** Each biome needs one or two covers at once: ice, snow and slush; jungle, mud and moss; desert, sand;
flood, mud. A layer is 64 bytes, and the shader reads only the batch's.

### Where it is drawn

| | For | Against | Verdict |
| --- | --- | --- | --- |
| **(1) In the scene fragment shader, a uniform branch after the normal is composed (recommended)** | Every reader (the lighting, SSR, AO, the cascades) sees it; no pass, no pipeline; the opaque, unshadowed, lifted, table and skinned paths all go through it | The branch may cost registers for every pixel, set or not (Risks) | **Recommended** |
| (2) A screen-space pass over the targets | One place | This renderer lights in the opaque pass, so a later albedo change would need relighting; without the cascades the targets carry no albedo | Rejected |
| (3) `#if BTBH_COVER` variants | Free when off | Doubles every model pipeline array and adds six files to every game's list | The fallback if the registers show |
| (4) A texture for the cover's look (a snow albedo and normal) | Authored look | The full variant binds SDL's 16 samplers | Procedural noise instead. A texture after RFC 0019's phase 2 frees a slot, only if asked |

### Which normal says "up"

- **The vertex normal** (interpolated, turned to the side seen) follows the mesh: a roof's slope, a ledge's top.
- **The face normal** (from derivatives) is flat per triangle, and can't be taken after the discard.
- **The shading normal** (with the normal map and the details' tilts) catches snow in the roof tiles' ridges. On a
  brick wall it would sprinkle the bricks' tops.

The cover reads the vertex normal, leaning toward the shading normal by `normal_influence` (0: the vertex normal alone).

### Frozen water

| | What | Verdict |
| --- | --- | --- |
| (a) v2's own textures: an ice albedo, normal and ORM on its water batches | 0 renderer days; v2 generates its water maps already (`tools/look/water_maps.py`) | A fallback. A tiling texture repeats its cracks along a river; no plate tilt |
| (b) Parameters on `WATER` (as RFC 0002 part B proposes) | v2 doesn't draw its kit water with `WATER` (only when its maps are missing); ice shares nothing with waves | Rejected |
| (c) Parameters on `PIXEL_WATER` | Its mask is in the ORM's blue, for pixel art | Rejected |
| (d) An `ICE` pattern of the cover | Its Voronoi on every covered pixel of the city would cost three times the cover; ties ice to the facing | Rejected |
| **(e) A surface variant `ICE` (recommended)** | A case of `apply_surface` like water's, on the pipeline the water batch has today. World-space plates (they never repeat), bevelled cracks, a tilt per plate, frost | **Recommended** |

## Proposal

### The cover

```c
/* A cover laid by facing (RFC 0020, asked for by Be The Bullet Hell v2 for its biomes): snow, sand or mud on whatever
 * faces up, with a noisy edge, on the batches that ask for it. The window keeps two layers
 * (btbh_gpu_window_scene_set_cover); a batch takes one by BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 or _COVER_1. Where it
 * lies, it replaces the surface's albedo, roughness, metalness, occlusion and normal by its own, before the lighting,
 * so the reflections, the ambient occlusion and the radiance cascades see it as they see the surface. It is worked out
 * from the world position and the normal alone: no texture, no pass, no pipeline. A window that never sets a layer, or
 * a batch without the bits, draws byte for byte as before. */

/* How many layers a window keeps. */
#define BTBH_GPU_SCENE_COVER_LAYER_COUNT 2u

/* BTBHGPUSceneCoverLayer.pattern: the cover's edge and relief. */
typedef enum BTBHGPUSceneCoverPattern {
    BTBH_GPU_SCENE_COVER_SNOW = 0,   /* a crisp edge, soft lumps; roughness 0 means 0.85 */
    BTBH_GPU_SCENE_COVER_SAND = 1,   /* a soft edge that thins into a dusting, a fine grain; roughness 0 means 0.95 */
    /* a blotchy edge, glossy puddles where the noise is low (glossier with BTBHGPUSceneLighting.surface_wetness);
       roughness 0 means 0.55 */
    BTBH_GPU_SCENE_COVER_MUD = 2,
    BTBH_GPU_SCENE_COVER_PATTERN_COUNT = 3
} BTBHGPUSceneCoverPattern;

/* BTBHGPUSceneCoverLayer.flags. */
enum {
    /* One octave of noise and no relief (bump is ignored): about half the cost, for a Deck-class preset. */
    BTBH_GPU_SCENE_COVER_LAYER_COARSE = 1u << 0
};

/* One cover layer. 64 bytes, no pointer (the same on 32-bit): color 0, amount 12, slope_degrees 16, edge_degrees 20,
 * edge_noise_degrees 24, noise_scale 28, roughness 32, bump 36, normal_influence 40, base_height 44, base_fade 48,
 * wind 52, pattern 60, flags 61, reserved 62. A zeroed layer lays nothing (its amount is 0). Slopes are measured from
 * level: 0 a face looking straight up, 90 a wall, 180 a face looking straight down. */
typedef struct BTBHGPUSceneCoverLayer {
    float color[3];             /* linear albedo, each 0..1 */
    /* 0..1. 0: nothing (the layer is off). Above it, faces up to amount x slope_degrees from level take it: patches on
       level faces near 0.1, every level face by about 0.3, steep roofs near 1. */
    float amount;
    float slope_degrees;        /* the steepest slope it holds at amount 1, 0..90; 0 means 60 */
    float edge_degrees;         /* the slopes over which it thins out past that, 0..90; 0 means 8 */
    /* How far the noise moves the edge either way, 0..90 degrees of slope. 0: a clean line, and no noise is worked out
       (the cheapest). On level faces under a thin cover it is what breaks the cover into patches. */
    float edge_noise_degrees;
    float noise_scale;          /* noise cells per world unit, 0 or more; 0 means 1 */
    float roughness;            /* 0..1; 0: the pattern's */
    float bump;                 /* 0..1: its own relief's strength (0: flat, and no gradient is worked out) */
    /* 0..1: how far the surface's normal map, world detail and procedural detail tilt the slope the cover reads (snow
       caught in roof tiles' ridges). 0: the vertex normal alone. */
    float normal_influence;
    /* Phase 2; refused unless 0 before it. Below base_height (world y, before world_curvature), every face takes the
       layer, walls and undersides too, fading out over base_fade above it and wavering with the noise: mud splashed up
       a wall's foot, a flood's line, sand drifted against walls. base_fade 0: no band. */
    float base_height;
    float base_fade;            /* world units, 0 or more */
    /* Phase 2; refused unless 0 before it. The wind, x and z, its length 0..1: faces turned into it hold the layer up
       to 30 x its length degrees steeper (snow plastered on windward walls), and SAND's ripples lie across it. */
    float wind[2];
    uint8_t pattern;            /* BTBHGPUSceneCoverPattern */
    uint8_t flags;              /* BTBH_GPU_SCENE_COVER_LAYER_* bits */
    uint8_t reserved[2];        /* 0 */
} BTBHGPUSceneCoverLayer;

/* The window's cover (btbh_gpu_window_scene_set_cover). 136 bytes, no pointer: layers 0 and 64, flags 128, reserved
 * 132. */
typedef struct BTBHGPUSceneCover {
    BTBHGPUSceneCoverLayer layers[BTBH_GPU_SCENE_COVER_LAYER_COUNT];
    uint32_t flags;             /* 0 */
    uint32_t reserved;          /* 0 */
} BTBHGPUSceneCover;

/* Sets the cover the window's following scene presents draw (present_instances, _with_visibility, present_views,
 * render_to_target and its views). It is copied at the call and kept until the next call; NULL clears it.
 * - A layer whose amount is 0 is off: the batches that take it draw as without the bit, to the byte.
 * - Prewarms need nothing for it: a covered batch draws with the pipeline it has without the cover.
 * - Needs this release's scene fragment shaders (they name the cover's block, cover_color_roughness): a present that
 *   draws a covered batch with older ones fails with BTBH_GPU_INVALID_ARGUMENT, naming the shader.
 * - Refused with BTBH_GPU_INVALID_ARGUMENT, keeping the setting it had:
 *   - a NULL window; the cover's flags or reserved not 0;
 *   - in a layer: a field that isn't finite; a colour, amount, roughness, bump or normal_influence outside 0..1;
 *     slope_degrees, edge_degrees or edge_noise_degrees outside 0..90; noise_scale below 0; a pattern at or past
 *     BTBH_GPU_SCENE_COVER_PATTERN_COUNT; an unknown flag; reserved not 0; and, until phase 2, base_height, base_fade
 *     or wind not 0 (from phase 2: base_fade below 0, or wind longer than 1).
 *   report may be NULL. Otherwise only its result and message are written; the message names the layer and the field.
 * Call it on the thread that presents, between presents. */
BTBHGPUResult btbh_gpu_window_scene_set_cover(
    BTBHGPUWindow *window,
    const BTBHGPUSceneCover *cover,
    BTBHGPUSceneReport *report
);

/* BTBHGPUSceneGeometryBatch.flags2, bits 2 and 3 (bit 0 is KEEP_CASCADES_LOOK; bit 1 is RFC 0019's IN_VOLUMES): */
    /* The batch takes the window's first (COVER_0) or second (COVER_1) cover layer where its faces turn up. Every pass
     * that draws it with the scene's fragment shader takes it: opaque and transparent, lit and unlit, static, skinned
     * and lifted, with or without a world detail or a table. The cover is laid in the world: on anything that moves its
     * noise slides, and it stays on whatever faces up. Without a layer set, or with that layer's amount 0, the bit
     * changes nothing. Refused (the present fails with BTBH_GPU_INVALID_ARGUMENT, naming the batch): both bits; a
     * LIT_PARTICLE or REFRACT batch (their shaders draw no material); a batch the fast unlit shader draws (unlit,
     * transparent, no surface variant, no world detail with a texture, no table with map_layers). Adds no pipeline. */
    BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 = 1u << 2,
    BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1 = 1u << 3

/* BTBHGPUSceneWorldDetailTableEntry.flags (phase 2; refused unless 0 before it, as today), at most one: what the
 * entry's triangles take instead of their batch's choice, where the table draws (map_layers set). 0: the batch's. */
enum {
    BTBH_GPU_SCENE_WORLD_DETAIL_ENTRY_NO_COVER = 1u << 0,
    BTBH_GPU_SCENE_WORLD_DETAIL_ENTRY_COVER_0 = 1u << 1,
    BTBH_GPU_SCENE_WORLD_DETAIL_ENTRY_COVER_1 = 1u << 2
};

/* Defined when btbh_gpu_window_scene_set_cover, BTBHGPUSceneCover, BTBHGPUSceneCoverLayer, BTBHGPUSceneCoverPattern,
   BTBH_GPU_SCENE_COVER_LAYER_COARSE and BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 and _1 exist. */
#define BTBH_GPU_SCENE_HAS_COVER 1
/* Phase 2: defined when base_height, base_fade and wind are honoured, MUD has its wet halo and a table entry's flags
   choose its cover. */
#define BTBH_GPU_SCENE_HAS_COVER_PHASE_2 1
```

`pattern` and `flags` are bytes, as `BTBHGPUSceneWorldDetail`'s `seed_component` and `flags` are, so the layout never
depends on an enum's size. No existing struct changes size.

### What a layer does

For a fragment of a batch that takes layer L, in `shade_fragment` after the normal is composed and turned to the side
seen (`model.frag:2461`), before the occlusion, roughness, metalness and F0 are taken (`:2464-2476`). Angles in degrees:

```text
n_v  = the vertex normal, turned to the side seen
n_s  = the shading normal (normal map, world detail's and procedural detail's tilts), turned the same way
n_f  = normalize(lerp(n_v, n_s, normal_influence))
θ    = acos(n_f.y)                         0 level, 90 a wall, 180 facing down
N    = 0 when edge_noise is 0. Otherwise value noise over the flat world (x, z) * noise_scale, on the procedural
       detail's integer hash (stable at a city's coordinates): two octaves, the second at 2.03x and half as strong,
       fading out where its cells shrink under two pixels; scaled to -1..1. COARSE: the first octave alone
θmax = amount * slope + edge_noise * N
       phase 2, the wind: + 30 * |wind| * max(0, -dot(normalize(n_f.xz), normalize(wind))) * length(n_f.xz)
       phase 2, below base_height:
         θmax = lerp(θmax, 180 + edge, saturate((base_height + 0.5 * base_fade * N - y) / base_fade))
c    = saturate((θmax - θ) / edge), then the pattern's edge:
         SNOW smoothstep(0.3, 0.7, c); SAND c; MUD smoothstep(0.15, 0.55, c)

albedo.rgb = lerp(albedo.rgb, color * (1 + grain * N), c)      grain: SNOW 0.04, SAND 0.10, MUD 0.15
albedo.a   = max(albedo.a, c)                                   a see-through batch: the cover is opaque
roughness  = lerp(roughness, r, c)                              r: the layer's, or the pattern's
             MUD: r = lerp(r, 0.12, puddle * (0.5 + 0.5 * wetness)), puddle = smoothstep(0.2, -0.4, N),
             wetness = BTBHGPUSceneLighting.surface_wetness
metalness  = lerp(metalness, 0, c);  occlusion = lerp(occlusion, 1, c);  glass's F0 (detail pattern 5) eases to 0.04
normal     = normalize(lerp(n_s, n_c, c)),  n_c = normalize(n_v - bump * k * (∇N on the face's plane))
             k: SNOW the first octave's gradient x 0.5; SAND the second's x 0.35; MUD both x 0.6
             bump 0 or COARSE: n_c = n_v
phase 2, MUD's wet halo: within one edge's width past where c reaches 0, the uncovered surface darkens by up to 0.3
             and its roughness falls by up to 0.4
```

- **Amount.** `θmax` grows with it: at small amounts only level faces take the cover, in patches where the noise lifts
  `θmax` past 0; by about 0.3 every level face; near 1 slopes up to `slope_degrees`. v2 can raise it over a stage (snow
  settling).
- **Edge.** `edge_degrees` is how wide the fade is in slope; the noise moves where it falls. On a ledge the cover ends
  where the moulding turns down; on a pitched roof it thins by slope.
- **Height (phase 2).** The base band covers every face below a height. Snow's level is the amount.
- **The noise's scale.** 1 cell a cell by default: patches about 77 px across at 4K High; the second octave's edge
  detail about 38 px.

| Pattern | Its edge | Its relief | Its roughness | Its grain |
| --- | --- | --- | --- | --- |
| SNOW | Crisp (smoothstep over the middle) | Soft lumps: the first octave | 0.85 | ±4 % |
| SAND | Soft: thins into a dusting | A fine grain: the second octave; ripples across the wind in phase 2 | 0.95 | ±10 % |
| MUD | Blotchy | Both octaves | 0.55, puddles 0.12 (glossier when wet) | ±15 %; a wet halo in phase 2 |

### What the cover changes, and what it doesn't

| Reader | With the cover |
| --- | --- |
| The lit colour: sun, fill, spot, lamps, point lights, the sky, IBL | The cover's albedo, roughness and normal: a snowy ledge lights as snow, in the same shadows |
| SSR | Roughness in `ambient_specular.a`: snow (0.85) never reflects under v2's 0.35; MUD's puddles (0.12) do; F0 0.04 |
| HBAO, SSAO | The normal target carries the cover's normal and occlusion |
| The radiance cascades | The `_rc` variants write the covered albedo, normal and roughness, so bounce light off snow is white |
| The cascades' material sources (`production_rc_material_source.frag`) | Not covered: an unlit covered batch injects its own colour. Lit batches inject only their emission, which the cover leaves |
| The emission | Unchanged: a glowing roof glows through snow (v2's lit panes are on walls) |
| Shadows, the dig mask, coverage, depth, motion vectors | Unchanged: the cover changes no geometry |
| Transparency | A see-through covered batch takes the cover opaque where it lies (snow on a glass roof) |
| Unlit batches (1, 2, 3) | Their colour is the covered albedo; unlit 2 and 3 still darken it in the sun's shadow |

### How it reaches the shader

- **The layers:** a new `cbuffer SurfaceCover : register(b3, space3)` in `model.frag`, four `float4` a layer (128
  bytes): colour and roughness; `amount * slope`, `1 / edge`, the noise's reach and scale (radians, cells); bump,
  normal influence, the base band (phase 2); the wind (phase 2), the pattern, the flags. Pushed before the first covered
  batch of each pass, so a frame without a covered batch pushes nothing new. The scene fragment shaders are created with
  four uniform buffers instead of three (`scene.c:3919`).
- **The batch's choice:** `BatchExtension.world_detail_mode.z` (0 today), pushed per batch already
  (`scene.c:12996-13020`): bits 0-1 the layer (0 none, 1 the first, 2 the second), 0 when that layer's amount is 0;
  phase 2, bit 2: its table's entries choose.
- **An entry's choice (phase 2):** `WorldDetailTable.world_detail_table_layers[i].z` (0 today): 0 the batch's, 1 none,
  2 the first, 3 the second.
- **The branch:** `if (world_detail_mode.z != 0u)`, a uniform: a batch without a cover runs none of it.

### Still ice

```c
/* BTBHGPUSceneSurfaceVariant, after LIT_PARTICLE: */
    /* Still ice: frozen water (RFC 0020). Plates in a Voronoi pattern over the flat world x and z (it never repeats),
     * parted by bevelled cracks that catch the light, each plate tilted a little so its reflection breaks from its
     * neighbours', a finer craze of hairline cracks, and frost in patches. The colour is the surface's (its albedo
     * texture, colour and instance colour); the cracks and the frost lift it. It replaces the normal map and the ORM
     * texture, as WATER does (occlusion 1, metalness 0); the emission stays. surface_time is unused: the ice is still.
     * Lit or unlit, opaque or see-through; on anything that moves its pattern slides.
     * surface_parameters0: x a plate's size (world units across; 0 means 1.2), y a crack's width (world units; 0 means
     *   0.025), z the cracks' and the frost's lift (their albedo is the surface's times 1 + z, at most 1; 0 means 0.5),
     *   w a plate's largest tilt (radians; 0 means 0.035).
     * surface_parameters1: x the frost, 0..1 (the share of the surface it covers; 0: none), y a frost patch's size
     *   (world units; 0 means 3), z the clear ice's roughness, 0..1 (0 means 0.06), w the cracks' and the frost's
     *   roughness, 0..1 (0 means 0.6).
     * A parameter that isn't finite or is below 0, or one of the 0..1 above outside it, fails the present with
     * BTBH_GPU_INVALID_ARGUMENT, naming the batch. Needs this release's scene fragment shaders: an older one draws it
     * as NONE, so a present that draws it with a shader that doesn't name the cover's block (cover_color_roughness, of
     * the same release) fails, naming the shader. Adds no pipeline. */
    BTBH_GPU_SCENE_SURFACE_ICE = 12,
    BTBH_GPU_SCENE_SURFACE_COUNT = 13

/* Defined when BTBH_GPU_SCENE_SURFACE_ICE exists. */
#define BTBH_GPU_SCENE_HAS_SURFACE_ICE 1
```

**What it does** (a case of `apply_surface`, `model.frag:294`, after water's):

```text
p        = world (x, z) / plate                              a Voronoi grid, jitter 0.85, the integer hash
F1, F2   = the distances to the two nearest seeds; id the nearest's cell; e the direction from it to the second
d        = (F2 - F1) / 2 * plate                             about the distance to the plate's border, world units
crack    = 1 - smoothstep(0, width, d), times saturate(2 * width / footprint)
           (thin at a distance, it fades out rather than alias)
fine     = the same at plate / 2.6, half the width, 0.6 as strong; skipped where its width is under a pixel
frost    = smoothstep(1 - frost - 0.1, 1 - frost + 0.1, two octaves of value noise over (x, z) / frost size), and at
           least 0.7 x crack (frost gathers in the cracks)
k        = max(crack, fine, frost)
albedo   = albedo * lerp(1, min(1 + lift, 1 / max(albedo)), k)
roughness= lerp(clear, frost roughness, k);  metalness 0;  occlusion 1
normal   = up, tilted by the plate's own tilt (hash of id, up to `tilt`); within twice the crack's width of a crack it
           leans up to 0.35 rad toward the crack's line along e (the bevel); turned into the vertex's tangent frame
```

- **Flat and still:** no time, no waves; the batch's own normal map and ORM are ignored, as `WATER` ignores them.
- **Glossy:** clear ice at 0.06 is below half of v2's `ssr_max_roughness`, so the plates reflect the city at full
  weight; cracks and frost at 0.6 don't.
- **At v2's scale:** a 1.2-cell plate is about 92 px across at 4K High and 35 at the Deck preset. A 0.025-cell crack is
  about 1.9 px at 4K High and fades at the Deck preset's 0.7 px, where the plates' tilt and the frost still read.
- **With the cover:** an `ICE` batch may take a cover bit too (snow drifted on the ice), but its frost does that on its
  own (question 12).

### Zero values and identity

- **Both structs are new;** a zeroed layer lays nothing; a zeroed `ICE` batch's parameters take their defaults.
- **No call, NULL, every layer's amount 0, or no batch with a bit:** `world_detail_mode.z` is 0 for every batch, no
  cover block is pushed, and every frame is byte for byte as before.
- **The bits are new** (unknown bits were refused); so are the entry flags (refused unless 0) and the variant's value.
- **The six scene shaders** gain a uniform block and a branch that isn't taken; their pictures must not change
  (verified, "Platforms and testing").

## Minimal example

```c
/* The ice biome, set at the stage's start (and each frame, if the snow settles as the stage goes on). */
BTBHGPUSceneCover cover;
SDL_zero(cover);
/* Layer 0, snow: the kit's roofs, ledges, sills and props, the lawns, the squares, the hills. */
BTBHGPUSceneCoverLayer *snow = &cover.layers[0];
snow->color[0] = 0.034f; snow->color[1] = 0.036f; snow->color[2] = 0.041f;   /* v2's palette rule sets it */
snow->amount = 0.8f;
snow->slope_degrees = 55.0f;
snow->edge_degrees = 6.0f;
snow->edge_noise_degrees = 14.0f;
snow->noise_scale = 1.6f;
snow->bump = 0.4f;
snow->pattern = BTBH_GPU_SCENE_COVER_SNOW;
/* Layer 1, slush: the roads. */
BTBHGPUSceneCoverLayer *slush = &cover.layers[1];
slush->color[0] = 0.030f; slush->color[1] = 0.031f; slush->color[2] = 0.033f;
slush->amount = 0.6f;
slush->edge_noise_degrees = 20.0f;
slush->roughness = 0.3f;
slush->pattern = BTBH_GPU_SCENE_COVER_MUD;
/* On the Deck preset: snow->flags = slush->flags = BTBH_GPU_SCENE_COVER_LAYER_COARSE; */

BTBHGPUSceneReport report;
if (btbh_gpu_window_scene_set_cover(window, &cover, &report) != BTBH_GPU_OK) {
    /* report.message names the layer and the field */
}

/* Each frame, as the kit's materials are made: */
roof.flags2 |= BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0;
road.flags2 |= BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1;
/* The rivers: still ice instead of the water maps. */
river.normal_texture = NULL;
river.surface_variant = BTBH_GPU_SCENE_SURFACE_ICE;
river.surface_parameters1[0] = 0.35f;    /* frost on a third of it */
/* ... the present, as every frame. Another biome: btbh_gpu_window_scene_set_cover(window, NULL, NULL), or its own. */
```

## Implementation

### The cover, phase 1: the ice biome's look

| File | Change |
| --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | The two structs, the pattern enum, the layer flag, the setter, the two `flags2` bits, the entry flags (refused until phase 2), the macros |
| `src/gpu/btbh_gpu_cover.h` (new) | The CPU reference: a layer's validation and packing into the block, the facing, the noise on the procedural detail's hash, the patterns' edges, so the tests can check the pictures |
| `src/gpu/btbh_gpu_window.c` | The window's setting (136 bytes) and the setter |
| `shaders/gpu/production_cover.hlsli` (new) | The noise with its gradient, the facing, the three patterns, the blend |
| `shaders/gpu/production_model.frag.hlsl` | The `SurfaceCover` block at b3, the call after `:2461`, F0 after it. The five wrappers change with it |
| `src/gpu/btbh_gpu_scene.c` | Below |
| `src/gpu/btbh_gpu_world_detail.h` | `world_detail_mode.z` from the batch's bits and the frame's layers |

In `scene.c`:
- **Validation** (`btbh_gpu_scene_validate_world_details`, `:5481`): both bits; with `LIT_PARTICLE`, `REFRACT` or on a
  batch `btbh_gpu_scene_uses_fast_unlit_pipeline` takes; `flags2`'s known bits (`:5395`).
- **The push:** the 128-byte block before the first covered batch of each pass (opaque, transparent, each view's,
  renders into targets); a fourth fragment uniform buffer for the scene shaders (`:3919`); the run verifier's fourth
  fragment slot (`BTBH_GPU_SCENE_RUN_VERIFY_FRAGMENT_SLOTS`, `:5760`).
- **The shader check** (`:7504-7510`): `has_cover` when the file names `cover_color_roughness`; a frame that draws a
  covered or `ICE` batch with a scene fragment shader without it fails, naming the shader.
- **Debug:** `BTBH_DEBUG_COVER=1` draws each covered pixel's c in place of its colour (red the first layer, green the
  second), for tuning.

**Estimate: 4-4.5 days.**

| Part | Days |
| --- | --- |
| Header, the window's setting and setter, the CPU reference | 1 |
| Shaders: the noise, facing, patterns and blend, tuned by eye on v2's ice, desert and jungle captures | 1.5 |
| `scene.c`: validation, the push, the shader check, the run verifier, the debug view | 0.5 |
| Tests: native, the GPU smoke against the reference, the identity runs, the bench | 1-1.5 |
| CHANGELOG, docs, the Mac's check | 0.25 |

### The cover, phase 2: bands, wind, halo, table entries

| Item | Days |
| --- | --- |
| The base band (walls below a height: mud and flood lines, sand at the walls' feet) | 0.25 |
| The wind: windward faces, SAND's ripples across it | 0.5 |
| MUD's wet halo | 0.25 |
| A table entry's choice (the entry flags, the table block's z; v2's merged distance models: snow on roofs, slush on roads in one batch) | 0.5 |
| Tests, docs | 0.25-0.5 |
| **Total** | **1.5-2** |

**The whole cover is 5.5-6.5 days**, more than about 5, hence the split: phase 1 has everything the ice biome's first
look needs, and the header is whole from the start (phase 2's fields refused until it lands, so nothing is silently
ignored).

### Still ice

| File | Change |
| --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | The variant, its comment, the macro |
| `shaders/gpu/production_ice.hlsli` (new) | The plates, cracks, bevel, tilt and frost |
| `shaders/gpu/production_model.frag.hlsl` | The case in `apply_surface` |
| `src/gpu/btbh_gpu_scene.c` | The parameters' validation; the shader check (above) |
| A CPU reference in `btbh_gpu_cover.h` | The Voronoi's cracks, for the test |

**Estimate: 2.5-3 days.**

| Part | Days |
| --- | --- |
| Header and validation | 0.5 |
| The shader, tuned by eye on a frozen river at 4K High and the Deck preset | 1-1.25 |
| Tests: native, the GPU smoke, identity | 0.75-1 |
| CHANGELOG, docs, the Mac's check | 0.25 |

### Together

- **Cover phase 1 and ice in one release: about 6-7 days** (one release's docs and identity runs instead of two). That
  is the size of RFC 0018's phase 1 (5-7 days), so they fit between it and volumetrics, as v2 asked.
- **Cover phase 2** after a play-test of phase 1: 1.5-2 days, before or after volumetrics as v2 prefers.
- **No new cost step.** The cover and the ice are inside the opaque and transparent passes; the cost log's groups and
  steps are unchanged.

## Items 3-5: what falls out, and what they would cost

| Item | Falls out for free? | What it would cost |
| --- | --- | --- |
| **5. One decal batch, an atlas cell per instance, the GPU lift** | **Yes, today.** `INSTANCE_UV_RECT` (0.11.0) works with `height_lift` on a static batch: every static vertex shader takes the UV rect, the lift's included, and so does the fast unlit shader (`btbh_gpu.h:1272-1282`, `:1230-1242`; nothing in `btbh_gpu_particle_uv_rect_problem` refuses a lift). The burns, cracks and lines can share one atlas and one draw now | 0 renderer days. v2 packs the atlas (cells with a gutter, as its smoke's) and sets `uv_rect` and `lift: 2` (`KEEP_NORMALS`) on one decal material |
| **4. Lit, normal-mapped ground decals** | **Mostly, today.** A lit (unlit 0), see-through batch with a normal map draws with the full scene shader: the sun and its shadow, the lamps and point lights, the normal map's bevel catching them. With the UV rect the normal map takes the same cell; lifted without `KEEP_NORMALS` its normal follows the hills. What it doesn't get: the HBAO is laid over the opaque scene before the transparent pass, so a decal by a wall's foot is a shade brighter than the ground round it; and it costs the full shader per decal pixel instead of the fast unlit one (about 0.02-0.06 ms at 4K High for 2-8 % of the frame) | 0 renderer days for this form. A decal that tilts the ground's own lighting without painting it (a normal-only decal) needs decals in the opaque pass's shader with an atlas, so a sampler the full variant hasn't got: 5-8 days, after RFC 0019's phase 2 frees one. Nothing from the cover helps |
| **3. Rain streaks drawn by the renderer** | **No.** Partly today: v2's strips as `LIT_PARTICLE` (RFC 0013) are lit per pixel by the lamps and point lights, depth-tested and soft against the ground; v2 sizes them by distance on the CPU. Not from the cover, nor from distortion. Volumetrics helps one part: where a frame's volumes have a medium where it rains (the flood's rain haze), the streaks can take their light from the froxel grid's lit radiance (its history) instead of a light loop of their own | A streak pass: placed on the GPU in a box round the camera, wrapped by time, pixel-sized and anti-aliased, stretched by the fall and the camera's motion, depth-tested with a soft fade, a reactive mask under FSR 3: 3-4 days; about 1 day less after volumetrics phase 1, its lighting read from the grid. The same pass could draw the ice biome's snowfall as flakes |

## Cost

This PC, the RTX 3070 Ti: 4K High is a 2560 x 1440 render (1.78 1080p screens); the Deck preset 960 x 600. The cover's
share of the frame is taken as 85 % (the kit, the ground, the props), with 1.1 fragments a pixel; the ice's as 8 % (the
rivers and canals). "Deck-class" is about 1/13 of this PC's ALU rate (RFC 0009).

| Part | Instructions a pixel | 4K High | Deck preset, this PC | Deck-class APU |
| --- | --- | --- | --- | --- |
| **Nothing set, no bit, or amount 0** | A uniform compare | **0** (registers: Risks) | **0** | **0** |
| Cover with noise and relief | about 230 | about 0.10 ms (1.2 % of 8.2 ms) | about 0.016 ms (about 1 % of 1.2-1.5 ms) | about 0.2 ms |
| Cover, `COARSE` | about 120 | about 0.05 ms | about 0.008 ms | about 0.1 ms |
| Cover without noise (a clean line) | about 65 | about 0.03 ms | about 0.004 ms | about 0.05 ms |
| `ICE` on the water | about 650 | about 0.02 ms (0.01-0.03) | about 0.004 ms | about 0.05 ms |
| **Pipelines added** | | **0** | **0** | |
| **Shader files added** (loaded) | | **0**; 2 includes | | |
| Memory | | 136 bytes on the window | | |
| CPU | | The copy at the call; one 128-byte push a pass with a covered batch; a few bit tests a batch: under 0.01 ms | | |

- **How the instructions were counted.** One value-noise octave on the integer hash: four 15-instruction hashes and the
  blend, about 75; its gradient about 10 more. The facing (`acos`, the lerp of normals) about 35; the blend and the
  relief's normal about 30. The ice: a 3 x 3 Voronoi of about 26 instructions a cell, twice (the fine craze), the
  frost's two octaves, the bevel.
- **Scaling.** Pattern 3, about 700 instructions, measured +0.175 ms a 1080p screen and +0.05 ms at 960 x 600; the
  rows above scale that by instructions, screens and share.
- **Against today:** v2's surface grain (pattern 3) costs it 0.26-0.28 ms on High; the full cover is about a third of
  that. The ice replaces the water maps' two fetches.

## Risks

| Risk | What then |
| --- | --- |
| The branch costs registers in every pixel of the full shader, cover or not (0.6.0's world detail: +0.03-0.04 ms a heavy 1080p screen, nothing under a light pixel; RFC 0008) | Measured with the bench (the world-detail smoke's heavy and light pixel) and the driver's register count (`VK_KHR_pipeline_executable_properties`, as RFC 0009 did). If it shows: keep the noise's live state small; failing that, `#if BTBH_COVER` variants for the shadowed sets only (more pipelines, question 14) |
| The six shaders' pictures change with the branch (a driver contracting differently round it) | The identity runs catch it; the code moves until they pass, as for 0.6.0 to 0.12.0's branches |
| The noise shimmers at a distance or under FSR 1 | Its second octave fades by footprint; `COARSE` has none; `BTBH_DEBUG_COVER` shows it |
| Snow on things that move (falling debris, a tumbling car) stays on top and its noise slides | By design; v2 leaves the bit off the collapse's draws (question 4) |
| A cover on every up-facing surface makes the frame brighter, and the bodies less salient | v2's salience pass on the ice biome with the cover, and its palette test with the layers' colours (question 6) |
| The cascades' material sources ignore the cover (an unlit covered batch injects its own colour) | Only matters for unlit covered batches; v2's kit is lit |
| Metal | Unverified until the Mac session's `check.sh` builds the metallibs and runs the smoke |
| Shared numbers with RFC 0019 (`flags2` bits, uniform slots) | Bits 2-3 are this RFC's whatever lands first. If 0019's phase 2 wants a uniform block in the full scene shader, it shares b3 (room to 4 KiB) |

## Compatibility

- **Versioning:** minor (pre-1.0: "additive → minor or patch"), the release after the distortion pass's, by v2's order.
  - New: two structs, an enum, a layer flag, a function, two `flags2` bits, three entry flags (refused until phase 2),
    a surface variant and three macros.
  - No struct changes size; no default changes.
  - `BTBH_GPU_SCENE_SURFACE_COUNT` becomes 13.
- **Be The Bullet Hell v2 (Rust, bindgen):**
  - Regenerate the bindings. The new enums are written by v2 and never read back, so `rustified_enum` is harmless.
  - Its `Material` gains a cover choice that sets the `flags2` bits (`v2/crates/v2-platform/src/gpu.rs:3182` sets
    `flags2` today); its own run and order keys must take it.
  - Its biome code calls the setter at a stage's start (NULL in the temperate city), and swaps its water material for
    `ICE` in the ice biome.
- **Be The Bullet Hell (v1, Swift), CHIMERA (Rust), Tyrian3D (C):** nothing to change. They rebuild their shaders with
  the C core, as every release. The Swift layer maps its own variant enum (`GPUResourceBackend.swift:30-41`), which
  needs no `ICE`.

## Platforms and testing

- **Native:**
  - **`btbh_gpu_scene_order_test`:** `BTBHGPUSceneCoverLayer` is 64 bytes and `BTBHGPUSceneCover` 136 with the offsets
    above; the bits' and entry flags' values; `BTBH_GPU_SCENE_SURFACE_ICE` 12.
  - **The public header test:** the header alone as C11 and C++17.
  - **New `btbh_gpu_cover_test`:** every refusal of the setter, and that a refused call keeps the setting; the batch
    refusals (both bits, `LIT_PARTICLE`, `REFRACT`, fast unlit); the `ICE` parameters' refusals; on the CPU reference,
    the facing at known slopes (c 1, the edge, 0), the noise's range and its integer hash against the shader's, the
    Voronoi's crack distance at known points.
- **GPU smoke, Windows/Vulkan (new `btbh_gpu_scene_cover_smoke`, after `btbh_gpu_world_detail_smoke.c`):**
  - **Identity.** The flags, prewarm, world-detail, scene-target, runs, lit-particle, views and (if landed) distortion
    smokes' images, byte for byte against the release before, in the four modes (plain, shared meshes, no runs, runs
    verified), three ways: no call; a cover set and no batch with a bit; a bit on every batch that may take one and
    every layer at amount 0. And no pipeline made after frame 1 in each.
  - **The facing, read back pixel by pixel.** A fan of unlit facets at 0, 15, 30, 45, 60, 75, 90, 120 and 180 degrees
    from level, a ledge and a sill on a wall, each pattern, with the noise off and on, under three cameras: each pixel's
    colour against the CPU reference's c at the point its ray hits (within 2 levels in 99.5 % of the pixels, 6
    everywhere). `COARSE` the same.
  - **The targets.** Lit, with SSR on: a level covered facet reflects nothing under `ssr_max_roughness` 0.35, MUD's
    puddles do; the HBAO over a covered step against an uncovered one.
  - **Paths.** The same facet static, skinned, lifted, with a world detail and with a table; opaque and see-through;
    each shadow set and under the cascades; through `present_views` with a split; rendered into a target.
  - **Ice.** An unlit level quad: each pixel's lift where the CPU reference puts a crack (within the tolerances
    above); lit with SSR on, the plates reflect and the cracks and frost don't; at the Deck preset's size no crack
    aliases (a 30-frame camera pan, its frame-to-frame difference within the plates' own).
  - **Refusals and old shaders:** each refusal fails the present naming the batch; a scene shader from 0.16.1 with a
    covered or `ICE` batch fails naming the shader.
  - **The bench** (`BTBH_SMOKE_BENCH=1`): screen-filling covered walls and floors under the heavy and light pixel: the
    cover off against 0.16.1's shaders (the registers' cost), full, `COARSE`, without noise; the ice.
- **The cost:** `BTBH_GPU_COST_LOG=1` at 4K High and the Deck preset in v2's ice stage, cover and ice on and off. Under
  the machine's perf lock.
- **macOS/Metal:** the Mac session's `check.sh` builds the metallibs, and the smoke runs there.
- **Linux and the Deck:** through v2.
- **Games:** v2's ice, desert, jungle and flood stages by day and night at 4K High and the Deck preset, its salience
  pass and its palette test; CHIMERA 0 px different; Be The Bullet Hell's Swift suite.

## Open questions for v2

Each has the renderer's recommendation; v2 answers per number.

1. **Two layers on the window, chosen per batch.** The window keeps two cover layers (`btbh_gpu_window_scene_set_cover`)
   and each batch takes one by two `flags2` bits, so v2's same-material runs stay whole. Is two enough: ice (snow and
   slush), jungle (mud and moss), desert (sand), flood (mud)? Recommended: yes.
2. **Procedural, no texture.** The cover's look is the renderer's three patterns (snow, sand, mud) from noise. The full
   scene shader has no free sampler for a snow texture. Recommended: accept. A textured cover only if the play-test
   asks, after volumetrics' phase 2 frees a slot.
3. **Which batches take it.** Recommended: the kit's roofs and walls (their ledges, sills and cornices take it by
   facing), the street-level pieces, the props (cars, roof gear), the Height terrain, the outskirts and the far hills.
   Not the units, the kaiju, effects, decals, marks or the water (the ice has its own frost).
4. **Things that move.** The cover is laid in the world: on falling debris or a collapsing piece it stays on whatever
   faces up, and its noise slides. Recommended: leave the bit off the collapse's and the wrecks' draws.
5. **Which normal says "up".** By default the vertex normal (`normal_influence` 0). Recommended: start there, and try
   0.4 on textured roof tiles so snow catches in their ridges.
6. **Colour and readability.** The palette rule (V at most 0.40, the distance from the gameplay colours) should take the
   layers' colours, and a cover on every up-facing surface brightens much of the frame. Recommended: v2's palette test
   takes the layers' colours; the salience pass runs on the ice biome with the cover. Start the snow at the roofs'
   paint (V 0.21) and see whether snow on ledges only lets it rise.
7. **Snow settling over a stage.** The amount can be set each frame (136 bytes a call). Recommended: try 0.3 rising to
   0.9 over the ice stage; constant in the other biomes.
8. **The Deck preset.** `COARSE` (one octave, no relief): about 0.008 ms on this PC, about 0.1 ms on a Deck-class APU,
   against about 0.016 and 0.2 ms in full. Recommended: `COARSE` on the Deck preset and Low, full on Medium and High.
9. **The split.** Phase 1: the layers, the facing, the noise, the three patterns, `COARSE`. Phase 2 (1.5-2 days): the
   base band (walls below a height: mud splashes, the flood's line, sand at the walls' feet), the wind (snow on windward
   walls, sand's ripples), mud's wet halo, and a table entry's choice. Without phase 2, the Deck preset's merged
   distance models take one layer per batch, so their roads take the roofs' snow, not slush. Recommended: accept that
   for phase 1, and decide phase 2 after the play-test.
10. **The emission under a cover.** It is left as it is: a glowing roof glows through snow. v2's lit panes are on walls.
    Recommended: accept.
11. **Still ice on the rivers.** A new surface variant, `ICE`, on v2's water batches in the ice biome instead of v1's
    water maps. Its colour is the batch's (v2's pale tint). It gives world-space plates, bevelled cracks, a tilt per
    plate and frost in patches, glossy (0.06) so SSR reflects the city on the plates. Recommended: yes. Start at a plate
    of 1.2 cells, cracks 0.025, lift 0.5, tilt 0.035, frost 0.35 with patches of 3 cells, roughness 0.06 and 0.6. Use it
    on the frozen gameplay cells too (`scene.rs:3286`).
12. **Snow on the ice.** The ice's frost already patches it. Recommended: no cover bit on the water.
13. **Ice's reflections.** Glossy ice reflects the buildings, brighter and busier than today's matt water, and the
    bullets cross it. Recommended: v2 checks readability over a frozen river; raise the clear roughness up to 0.15 if it
    is busy.
14. **If the cover's branch costs every pixel.** If the bench shows the off case costing registers (as 0.6.0's world
    detail did, 0.03-0.04 ms a heavy 1080p screen), the fallback is shader variants for the cover: more pipelines, made
    by the prewarm. Recommended: accept that fallback if needed.
15. **An amount per instance.** A custom float could scale the cover per instance (a building whose snow a fire melted,
    a fresh wreck). The custom floats are busy (the light factor, seeds, UV rects). Recommended: not now; ask after the
    play-test.
16. **The order.** Recommended: cover phase 1 and the ice in one release (about 6-7 days), after the distortion pass and
    before volumetrics phase 1.
17. **Items 3-5.** Recommended:
    - **Item 5 now, in v2:** it works today (`INSTANCE_UV_RECT` with `height_lift` on one decal batch, one atlas).
    - **Item 4:** try lit decals on the cracks now: a lit see-through batch with a normal map works today, at the full
      shader's cost and with no HBAO under it. A normal-only decal would be 5-8 days, after volumetrics' phase 2.
    - **Item 3** after volumetrics (3-4 days, about one less with its lighting from the froxel grid). Meanwhile, the
      strips as `LIT_PARTICLE` are lit by the lamps.

## Decisions (v2, 2026-11-28)

v2 took the recommendation on all 17 questions:

1. **Two layers a window** are enough.
2. **Procedural, no texture;** a textured cover only if the play-test asks.
3. **Which batches:** the kit's roofs and walls, street pieces, props, the Height terrain, the outskirts and far hills.
   Not units, the kaiju, effects, decals, marks or water.
4. **The bit stays off collapse and wreck draws.**
5. **`normal_influence` 0 to start;** v2 tries 0.4 on textured roof tiles.
6. **Readability:** v2 runs its palette test on the layers' colours and the salience pass on the ice biome with the
   cover on. Snow starts at V 0.21; v2 tries brighter snow on ledges only.
7. **Settling** 0.3 to 0.9 over an ice stage, constant elsewhere.
8. **`COARSE` on the Deck preset and Low,** full on Medium and High.
9. **Phase 1 as split;** phase 2 decided after the play-test.
10. **Emission through snow** is accepted.
11. **`ICE`** on the water batches in the ice biome and on the frozen gameplay cells, with the starting values.
12. **No cover on water.**
13. **Ice reflections:** v2 checks readability over a frozen river with bullets crossing; clear-ice roughness up to 0.15
    if it is busy.
14. **The shader-variant fallback** is accepted if the branch costs when the cover is off.
15. **No amount per instance** now.
16. **Cover phase 1 and the ice in one release,** after distortion and before volumetrics phase 1.
17. **Items 3-5:** v2 does 5 now (one decal batch with `INSTANCE_UV_RECT` and the height lift), tries 4 now (lit
    see-through normal-mapped cracks), and draws its rain strips as lit particles until 3.

## Phase 1 as built (feature/0.18.0-cover-ice)

Where the implementation departs from the text above, and why. Cover phase 1 and still ice are built together on
`feature/0.18.0-cover-ice` (from `feature/0.18.0-distortion`, with its review's fixes merged).

1. **Shader variants: question 14's fallback, taken.** As first built, with the branch in the six scene fragment
   shaders, the cover off cost the main pass +0.0014 ms at the Deck preset (paired by round, 10th to 90th percentile
   -0.017 to +0.051) and +0.014 ms at 4K High (-0.044 to +0.126); whole frames 1.321 to 1.320 ms and 5.248 to 5.270
   ms. That is no clear cost, but not a provable zero either, and the upper percentiles reached +0.05 and +0.13 ms. By
   decision 14 the cover and the ice moved behind `#if BTBH_COVER` (0 by default) into six new files:
   `production_model_cover.frag` and its `_2d_shadows`, `_unshadowed`, `_rc`, `_rc_2d_shadows` and `_rc_unshadowed`
   siblings. Each is a few lines that set `BTBH_COVER 1` and include `production_model.frag.hlsl`.
   - **Without the cover, the shaders are the old ones to the byte.** All 90 shaders built before compile to
     0.18.0-distortion's SPIR-V byte for byte, the six scene shaders included, and those keep three uniform buffers.
     The cover variants have four. 96 shaders.
   - **Which shader draws which batch:** a batch whose bit's layer is on, or a still-ice batch, draws with the cover
     variant of the shader the frame's shadows and cascades pick. Every other batch draws with the shaders and
     pipelines it had, a batch whose layer is off included. So "a layer at amount 0 changes nothing" holds by
     construction.
   - **Pipelines:** the cover variants have their own sets (`BTBHGPUSceneCoverPipelines`: static, skinned, with a
     table, lifted, lifted with a table, by shadow set; never fast unlit). The first batch that draws one allocates
     them. So the "Pipelines added: 0" row becomes: none for a host that covers nothing, and one more for each state a
     covered or icy batch draws in.
   - **Prewarm:** `btbh_gpu_window_scene_prewarm` makes both pipelines for a batch with a cover bit (its layer may be on
     or off by the frame), and only the cover variant's for still ice. So a host that turns snow on mid-stage
     prewarms with the bits set. The lazy creation log names a cover pipeline a prewarm missed (marked
     `prewarm_covers` 1).
   - **The shader check** reads the cover variant the frame draws with, not the host's fragment shader. A covered or
     still-ice batch whose cover shader is missing (shaders from before 0.18.0) or doesn't name `cover_color_roughness`
     fails the present with `BTBH_GPU_INVALID_ARGUMENT`, naming the file. A prewarm also checks batches whose bit's
     layer is off.
   - **Measured with the variants** (item 9): bits on every kit piece and the ground with the cover off cost the main
     pass -0.002 ms at the Deck preset (-0.017 to +0.012) and +0.001 ms at 4K High (-0.033 to +0.033) against
     0.18.0-distortion's shaders. A batch without a bit draws with the same bytes.
2. **A face past the cover's reach does no cover work.** The first build cost more than the estimate: +0.43 ms on the
   main pass at 4K High with noise and relief and +0.31 with `COARSE`, against about 0.10 and 0.05 estimated; +0.086
   and +0.062 at the Deck preset. The shader worked out the noise on every pixel of a covered batch, walls included.
   Now a fragment whose slope is at or past amount x slope plus the noise's reach returns before the noise, because c
   is 0 there whatever the noise. (`BTBH_DEBUG_COVER` draws such a fragment black, as c 0.) The CPU reference does
   the same, and the native test checks that the surface comes back unchanged past the reach. That cut the 4K cost by
   about 0.1 ms and most of the Deck preset's (item 9).
3. **The noise's reach grows with the amount** up to 0.25 (`edge_noise_degrees x min(1, 4 x amount)`). Without that,
   a thin cover (amount 0.01) still laid patches up to the noise's reach, while amount 0 lays nothing.
4. **No table-entry flags yet.** The text put them in phase 1's header, refused until phase 2. They aren't in it:
   phase 2 adds them, additively. Bit 2 of `world_detail_mode.z` says the batch is see-through (its alpha rises to c),
   not the table's choice; phase 2 takes another bit.
5. **Still ice:**
   - **Its cracks' anti-aliasing:** a crack is drawn at `max(width, half a pixel's footprint)` and loses strength in
     step (`width / that`), so a crack thinner than a pixel keeps its share of the pixel and fades evenly. The text
     multiplied by `saturate(2 x width / footprint)` at the true width. The bevel follows the same rule, and the craze
     is skipped where its width is under half a pixel.
   - **Its roughness is its own:** the batch's roughness factor doesn't scale it.
   - **Its normal** is a surface gradient added with the details' gradients, so it needs no tangent frame.
   - **Its code** is in `production_cover.hlsli` (there is no `production_ice.hlsli`), and its CPU reference is in
     `btbh_gpu_cover.h`.
6. **Where it lives:** the setter and the window's setting are in `scene.c`, not `window.c`. `scene.c` sets
   `world_detail_mode.z` from `btbh_cover_batch_mode`; `btbh_gpu_world_detail.h` is unchanged.
7. **The identity runs' third way** is `BTBH_SMOKE_COVER=1` in the smokes' presents (`Tests/btbh_gpu_smoke_present.h`):
   a cover whose layers lay nothing (amount 0, every other field set) and `COVER_0` on every batch that may take it.
8. **The smoke** (`btbh_gpu_scene_cover_smoke`):
   - **What it draws:** eight facets from 0 to 120 degrees under one camera. It checks each pattern, `COARSE`, the
     second layer, `normal_influence` and the debug view, each pixel against the CPU reference. It also checks the
     three shadow sets (unlit facets through each cover variant), the targets the screen-space passes read, the
     cascades' albedo target, SSR under rough snow, see-through batches, the ice unlit and lit, runs and the run
     verifier, views, a render into a target, prewarm, the refusals, and old and missing cover shaders.
   - **Accuracy:** the tolerance is 3 levels in 99.5 % of the pixels and 8 everywhere. The patterns land within 1
     level everywhere, the debug view within 3, and the unlit ice within 3 except 2 pixels of 38,354.
   - **Not covered:** skinned, lifted and table batches under the cover (their cover pipelines are picked as the plain
     ones are), and the Deck-size camera pan for the cracks' aliasing.
9. **The cost, measured.** The frame bench (`Tests/btbh_gpu_frame_bench.c`) gained the modes `cover-off` (a river
   across the city, v2's water material), `cover-bits` (`COVER_0` on every kit piece and the ground, no layer set),
   `cover-full` (that and a snow layer at v2's starting values: amount 0.8, slope 55, edge 6, noise 14 degrees at 1.6
   cells a unit, bump 0.4), `cover-coarse` (the same, `COARSE`) and `cover-ice` (the river as still ice, frost 0.35).
   The runs below are on this PC's RTX 3070 Ti under the machine's perf lock: five windows interleaved over 80 rounds
   of 8 frames, at 09:54 to 09:56. They give medians with the 10th and 90th percentiles, and each mode's main pass
   paired by round against `cover-off` on 0.18.0-distortion's shaders.

   | | Deck preset (960 x 600 to 1280 x 800, FSR 1) | 4K High (2560 x 1440 to 3840 x 2160, FSR 3) |
   | --- | --- | --- |
   | Frame, cover off (0.18.0-distortion's shaders) | 1.396 ms (1.334-1.433) | 5.264 (5.223-5.373) |
   | Frame, bits, cover off / full / `COARSE` / ice | 1.395 / 1.415 / 1.389 / 1.407 | 5.262 / 5.636 / 5.510 / 5.283 |
   | Main pass against cover off: bits | -0.002 ms (-0.017..+0.012) | +0.001 (-0.033..+0.033) |
   | Main pass: full | +0.018 (-0.002..+0.128) | +0.330 (+0.297..+0.398) |
   | Main pass: `COARSE` | -0.011 (-0.024..+0.087) | +0.231 (+0.203..+0.266) |
   | Main pass: ice on the river | +0.007 (-0.008..+0.022) | +0.016 (-0.009..+0.063) |

   So with nothing covered, or the bits set and the cover off, the cover costs nothing: the bytes are the same. At the
   Deck preset the full cover costs about 0.02 ms (within noise) and `COARSE` nothing measurable. At 4K High it costs
   0.33 ms full and 0.23 `COARSE`, about 4 % and 3 % of v2's 7.5-8.2 ms frame. That is three to five times the
   estimate (0.10 and 0.05), with every roof, ledge and street of the bench's city covered. A 4K pixel costs about
   three times a Deck pixel here; why is not established (the Deck numbers are near the noise). The ice costs about
   0.01-0.02 ms, as estimated. The first build (the branch,
   no early-out; 09:21 to 09:23, the same bench) measured the main pass at +0.086 / +0.062 ms (Deck, full / `COARSE`)
   and +0.432 / +0.309 (4K), and the ice at +0.003 and +0.027. Three runs with a sixth window (`cover-off` on this
   branch's shaders, the same bytes) were not usable: partway through each, some windows' main pass slowed by up to
   half, at both sizes (at 4K six windows' targets come near the card's 8 GB). The five-window run above is steady.
10. **What doesn't change:** the cost log's groups and steps (the cover and the ice are inside the opaque and
    transparent passes), and the scene shaders' bytes for every batch that takes neither.
