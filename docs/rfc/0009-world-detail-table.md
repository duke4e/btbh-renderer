# RFC 0009: A world-detail table chosen per vertex; the facade's weather without its grain

- **Status:** Phase 1 accepted on 2026-11-20 (for 0.7.0; the owner left the decision to the coordinator). Phase 2
  awaits v2's answers to questions 1–5.
- **Requested by:** Be The Bullet Hell v2 (its session, 2026-11-20, after it adopted 0.6.0; `CONSUMERS.md`,
  "Requested")
- **Affects:** Be The Bullet Hell v2 (adopts it). Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D: nothing, it is
  opt-in. They rebuild the scene shaders with the C core, as for 0.6.0
- **Issue:** none. v2 answered questions 6–8 (phase 1) through its session on 2026-11-20 (see "Decisions")
- **Depends on:** RFC 0008 phase 1 (0.6.0). Phase 2 brings the texture-array functions RFC 0001 drafted, unchanged

## Need

Be The Bullet Hell v2 ("v2" below) adopted 0.6.0's world detail (RFC 0008 phase 1) at BTBH `ed6edb8d`. It binds one
macro set per kit class, packed (the normal, and the ORM with the grey albedo in its blue), two samples each. Its
next two requests, verbatim:

1. "A detail chosen per vertex: a small table of up to 8 details per batch indexed by a vertex attribute, so merged
   distance models keep detail without extra draws (splitting them by class cost +0.6–0.85 ms frame on the Deck
   preset, from encode and draw count)."
2. "The facade's street-level darkening and wall wetness as a cheap mode without the triplanar noise, since
   replacing the grain with the detail drops both today."

v2's test of 0.6.0 (`CHANGELOG.md:26-31`): desktop RTX, GPU medians of two passes, in ms.

| Preset | 0.6.0, unset | Detail | Detail and the facade grain |
| --- | --- | --- | --- |
| Deck | 1.290 / 1.296 | 1.322 / 1.340 | 1.327 / 1.331 |
| Low | 1.612 / 1.619 | 1.691 / 1.706 | 1.702 / 1.694 |
| Medium | 2.663 / 2.658 | 2.651 / 2.651 | 2.779 / 2.765 |
| High | 3.036 / 3.034 | 2.932 / 2.953 | 3.210 / 3.208 |

### 1. Merged distance models

What v2 draws far off, from its code at `ed6edb8d` (v2 paths below are under `Be-The-Bullet-Hell/v2/crates/`):

- **The distance models are merged by default.** Each kit file has a `<file>_lod1.glb` (`v2-render/src/kit.rs:325-333`).
  It is split with `surface_class` (`v2-render/src/glb.rs:95-104`), which knows only glass, paving, grass and signs.
  Brick, tiles, concrete, roof, asphalt and the untextured materials all land in one `Plain` build: one draw per node.
- **That draw has no world detail.** It takes the facade's or the ground's procedural pattern and nothing else
  (`v2-render/src/scene.rs:897-904`). Only the separate classes go through `kit_surface`, which attaches a detail.
- **The split costs draws.** `BTBH_LOD1_SPLIT=1` splits the distance models by full class (`kit.rs:583-587`), so each
  class takes its detail. That is the +0.6–0.85 ms.
- **On the Deck, lod1 is nearly everything.** A chunk switches to lod1 under 30 pixels a cell: "the gameplay
  camera's far rows at 1080p, nearly all of the Steam Deck's 960 x 600" (`kit.rs:225-229`).

RFC 0008 left one texture array for all classes for "Later, if a game needs mixed classes in one draw"
(`0008-world-projected-detail.md:788`). A game does now.

### 2. The facade grain, taken apart

Pattern 3 of the procedural surface detail (`detail_parameters`, `shaders/gpu/production_model.frag.hlsl`) does five
things. Two need noise, and one needs the grain:

| Part | Lines | What it does | Noise |
| --- | --- | --- | --- |
| Triplanar grain | 1920-1929 | `height` from three `btbh_detail_fbm`, weighted by the face normal to the fourth power | 3 fbm × 3 octaves × 4 hashes = 36 hashes |
| Stains and tint | 1946-1947 | a broad noise, then `tint = 1 + x × (2 height + stain)` | 4 hashes |
| Street darkening | 1957-1961 | `tint *= lerp(0.8, 1, saturate(y × 4))`: 0.8 at y = 0, 1 from 0.25 render units up (the comment says "a fifth"; the code ramps over a quarter) | none |
| Wet walls | 2050-2071 | gated on patterns 1, 3, 4 (line 2058). On pattern 3: `wall_wet = wetness × 0.5 × (1 − level)`, the albedo × lerp(1, 0.85), the roughness toward 0.3 | none |
| Wet bump | 1971-1972 | the grain's bump × up to 3.5 when wet | needs the grain |

- The hash (`shaders/gpu/production_surface.hlsli:68-78`) is four 32-bit integer multiplies and three xor-shifts.
  Forty of them a pixel, in ten noise lookups, are the "dear part" (v2's words, `v2-render/src/scene.rs:251-252`).
- The darkening uses the flat world height and needs nothing from x, y or z. The wetness needs the face normal from
  the position's derivatives, which the shader already takes for the soft sun shadow, the world detail and the grain.
- v2 replaces the grain with the detail by setting `detail` to `[0; 4]` (`v2-render/src/scene.rs:542-552`). Pattern 0
  returns at line 1880 and fails the wetness gate at line 2058, so the darkening and the wetness go with it.
- **The grain's cost, measured.** "Detail and the facade grain" against "Detail" in v2's table: 0.11–0.13 ms on
  Medium and 0.26–0.28 ms on High, on a desktop RTX.
- **Low and the Deck lost them earlier.** Those presets set `surface_detail: false` (`v2-game/src/graphics.rs:194`),
  so `SurfaceLook::flat()` makes the facade `[0; 4]` (`v2-render/src/scene.rs:251-255`). Their walls have had no
  darkening or wetness since before 0.6.0. That is also why their two right-hand columns are equal.

## What the code allows

### The vertex

Models and custom meshes share one 96-byte vertex (`src/gpu/btbh_gpu_scene.c:15-20`,
`src/gpu/btbh_gpu_custom_mesh.c:9-12`, `BTBHGPUModelVertex` in `src/gpu/btbh_gpu_model.c:38-46`). The header doesn't
document it; v2 copies it into its own `Vertex` (`v2-platform/src/gpu.rs:68-80`).

| Offset | Field | Static pipeline (locations 0–4) | Animated pipeline (0–6) |
| --- | --- | --- | --- |
| 0 | position, float3 | read | read |
| 12 | normal, float3 | read | read |
| 24 | uv, float2 | read | read |
| 32 | tangent, float4 | read | read |
| 48 | colour, float4 | read | read |
| 64 | joints, float4 | **not read** | bones |
| 80 | weights, float4 | **not read** | bone weights |

- **Bytes 64–95 are free on static draws.** The static model pipeline binds five attributes, the animated one seven
  (`btbh_gpu_scene.c:1396-1434`).
- **Custom meshes are never skinned.** Animation needs a model batch (`btbh_gpu_scene.c:4439-4449`), so a custom mesh's
  joint slot is never read.
- **Who writes there today:**
  - The glTF loader zeroes the vertex and sets weight 0 to 1; joints come only from `JOINTS_0`
    (`btbh_gpu_model.c:1736`, `1797-1830`). An unskinned primitive has joints (0, 0, 0, 0).
  - The SDF terrain (RFC 0007) writes its layer weights into bytes 64–95 of its chunks, for RFC 0001's phase 4
    (`src/gpu/btbh_gpu_sdf_terrain.c:2887-2888`, `include/btbh_gpu/btbh_gpu_sdf_terrain.h:197-198`).
  - v2's kit meshes leave `joints` at its `Default`, zero (`v2-render/src/glb.rs:360-367`).

The candidates for a small per-vertex index:

| Carrier | Verdict |
| --- | --- |
| The colour's alpha | No. It multiplies the albedo's alpha, which the alpha cut-off and blending read, and it is interpolated |
| A new attribute (a second stream, or a wider vertex) | No. A new custom-mesh call and layout, while 32 bytes of every static vertex sit unused |
| The instance record (a custom float) | No. It is per instance; a merged mesh mixes classes inside one instance. v2 also uses w for the seed |
| **joints.x (byte 64)** | **Yes.** Present in every vertex, read by no static draw, written by v2's own mesh code |

**Flat interpolation is available.** The model shaders already pass `nointerpolation float animated_model`
(`shaders/gpu/production_model.vert.hlsl:67`, `production_model.frag.hlsl:177`). A `nointerpolation uint` goes through
shadercross to SPIR-V and MSL today (`production_rc_direct_light.frag.hlsl:13`). Which vertex gives a flat value
differs between graphics APIs, so all three vertices of a triangle must carry the same index.

### Binding eight details

SDL's GPU API allows 16 samplers per stage (`external/SDL/src/gpu/SDL_sysgpu.h:29`). The scene fragment shader takes
14, 13 or 9 (`btbh_gpu_scene.c:3078-3085`):

| Variant | Today | Free |
| --- | --- | --- |
| Directional and omni shadows | t0–t9, dig mask t10, world detail t11–t13 | 2 |
| Directional shadows only (`_2d_shadows`) | t0–t8, mask t9, detail t10–t12 | 3 |
| Unshadowed | t0–t4, mask t5, detail t6–t8 | 7 |

Eight packed details are 16 textures. The options:

| Option | Verdict |
| --- | --- |
| 16 separate textures | No: 30 samplers in the full variant |
| An atlas (eight tiles in one 2D texture) | No. Tiles bleed into each other at the mips the distance models read; that needs gutters and a capped LOD. `frac` breaks the mip choice at every repeat unless the shader uses `SampleGrad`. The atlas can't use the sampler's repeat |
| **A `Texture2DArray`** | **Yes.** Each layer repeats and has its own mips. The layer is just a third coordinate |
| Descriptor arrays with `NonUniformResourceIndex` | Not possible: SDL's GPU API has no descriptor arrays |

**SDL supports 2D arrays.** `SDL_GPU_TEXTURETYPE_2D_ARRAY` (`external/SDL/include/SDL3/SDL_gpu.h:924`) with
`layer_count_or_depth` (`:1777`); uploads and copies name a layer (`:1450`, `:1471`). Array textures can't be depth
targets or multisampled (`external/SDL/src/gpu/SDL_gpu.c:1344-1354`), neither of which matters here.
`SDL_GenerateMipmapsForGPUTexture` needs `SAMPLER | COLOR_TARGET` usage (`SDL_gpu.c:3046`), as for today's 2D
textures. Its Vulkan backend loops over the layers (`external/SDL/src/gpu/vulkan/SDL_gpu_vulkan.c:9071`); Metal's
`generateMipmapsForTexture` covers every slice. The renderer creates no array texture yet: `BTBHGPUTexture` has no
layer count (`src/gpu/btbh_gpu_internal.h:125-140`) and every texture is `SDL_GPU_TEXTURETYPE_2D`
(`src/gpu/btbh_gpu_texture.c:200`, `334`, `808`).

**One array or two.** v2's normal and ORM maps are both linear RGBA8 of one size. One array can hold both kinds of
layer: each entry names its normal's layer and its ORM's layer. That takes one sampler, so the full variant
reaches 15 of 16. Two arrays (normals, ORMs) take two and fill it to 16. One array is proposed. Its cost comes with
RFC 0008 phase 2: one array has one format, so the normals would be BC7 like the ORMs, not BC5 (open question 2).

### The entries' data: a uniform block

Per entry: two tiles, three scales, the level angle and two layers. Eight entries are 400 bytes as `float4`s and
`uint4`s, well inside a uniform buffer. SDL allows 4 uniform buffers per fragment stage (`SDL_sysgpu.h:32`); the scene
uses 2 (`btbh_gpu_scene.c:1358`).

- **A third buffer, not a bigger second one.** The second buffer (`BatchExtension`, 80 bytes) is pushed for every
  batch (`btbh_gpu_scene.c:8789-8796`). The table's 400 bytes go in a third, pushed only for batches that have a
  table.
- **Unpushed is harmless.** SDL's Vulkan backend gives a slot that was never pushed a buffer when the pipeline binds
  (`SDL_gpu_vulkan.c:7991`). Only a batch with a table reads it.
- **A storage buffer** would allow many more entries. It needs a copy pass and a per-frame buffer. Not for eight.

### A per-pixel index in one draw

The index is constant on a triangle but changes from triangle to triangle within a draw.

- **No `NonUniformResourceIndex`.** That is for picking a descriptor per pixel. Here every pixel of the draw uses one
  array texture, one sampler and one uniform block. Only the layer coordinate and an offset into the block vary.
- **No divergence.** The shader doesn't branch on the index. It clamps the index for its reads, samples, and swaps
  the result for "no change" with a select where the index is past the count.
- **Derivatives stay valid.** A 2×2 quad always belongs to one triangle (helper pixels extrapolate it), and a
  triangle to one entry. So the coordinate's derivatives inside a quad come from one entry's tiles. The mip choice is
  right and class boundaries draw no seam line, only the change of pattern. RFC 0008 relies on the same fact for its
  face split.
- **Reading the entry.** A per-pixel index into a uniform array becomes vector loads on AMD instead of scalar ones:
  three 16-byte loads from a cached 400-byte block.
- **Two layers in one wave** cost what two textures cost: the texture unit takes a layer per lane.

## Proposal

Two phases, smallest first:

1. **Phase 1: pattern 7, the facade's weather.** A new value of `detail_parameters[3]`: pattern 3's street darkening
   and wet walls, without the noise, the stains or the bump. Names for all eight patterns. No struct changes.
2. **Phase 2: the world-detail table.** Two texture-array functions (RFC 0001's drafts), a table of up to eight
   entries over one array, a pointer appended to the batch, and the vertex's joints.x as the entry's index.

### Where this differs from the request

- **The "vertex attribute" is the one already there.** joints.x of the 96-byte vertex, read by the static pipeline
  as a sixth attribute. No new layout and no new custom-mesh call.
- **Packed only.** A table's ORM carries the grey albedo in its blue, v2's layout. There is no coloured albedo in a
  table: it would need another array and the full variant's last sampler.
- **One array for both kinds of map.** Each entry names a normal layer and an ORM layer. Every layer has one size and
  format.
- **An index past the count means "no detail".** A merged draw holds untextured materials (metal, awnings, trim) next
  to the classes. 255 is the documented "none".
- **The seed is the table's.** One `seed_component` for all entries; v2 uses w for every class.
- **The procedural pattern stays per batch.** A merged draw keeps one pattern. In v2 a piece is either ground
  (pattern 1 or 4) or a building (pattern 3), so its merged draw has one (`scene.rs:897-901`; Decisions, 6).
  Glass (5), paving (4), grass (2) and signs (6) have their own patterns and stay separate draws, as today.
- **Request 2 is a pattern, not a flag.** Pattern 7 draws as pattern 3 with x and y at 0, without the noise's cost
  (Alternatives).

### Phase 1: pattern 7

```c
/* In BTBHGPUSceneGeometryBatch, the comment above detail_parameters becomes: */
    /* Procedural surface detail: x albedo variation, y bump height (render units), z noise cells per render
     * unit, w the pattern (BTBH_GPU_SCENE_DETAIL_*). A zeroed batch has none. */
    float detail_parameters[4];

/* BTBHGPUSceneGeometryBatch.detail_parameters[3]. Values 0..6 are as before; their names and value 7 are new in
 * 0.7.0. */
enum {
    BTBH_GPU_SCENE_DETAIL_NONE = 0,
    BTBH_GPU_SCENE_DETAIL_GROUND = 1,      /* grain, stains, wet puddles, rain rings */
    BTBH_GPU_SCENE_DETAIL_GRASS = 2,       /* grain, stains, a warm-to-cool drift */
    BTBH_GPU_SCENE_DETAIL_FACADE = 3,      /* triplanar grain, stains, street darkening, wet walls */
    BTBH_GPU_SCENE_DETAIL_PAVING = 4,      /* slabs, joints, grain, wet puddles, rain rings */
    BTBH_GPU_SCENE_DETAIL_GLASS = 5,       /* panes; y the share lit at night, z panes per render unit */
    BTBH_GPU_SCENE_DETAIL_LAMP_LENS = 6,   /* its colour shines with BTBHGPUSceneLighting.window_glow */
    /* Pattern 3's weather without its grain: every face darkens toward the street (0.8 at y = 0 to 1 at 0.25
     * render units, flat world height) and, with BTBHGPUSceneLighting.surface_wetness, steep faces turn darker and
     * semi-gloss. No noise, no stains, no bump. x, y and z must be 0. It draws as pattern 3 with x and y at 0. */
    BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER = 7
};

/* Defined when BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER exists (since 0.7.0). */
#define BTBH_GPU_SCENE_HAS_FACADE_WEATHER 1
```

- **Zero values.** Nothing new in any struct. A batch draws as before unless it sets 7.
- **Refusals.** Today the present refuses a pattern outside [0, 6] and, for patterns 1–6, z not above 0
  (`btbh_gpu_scene.c:4512-4520`). It now accepts up to 7. For 7 it refuses x, y or z not 0, with
  `BTBH_GPU_INVALID_ARGUMENT`. That keeps them free to carry the darkening's height and depth later without changing
  a look.
- **What z is.** For pattern 7, nothing yet: it must be 0, as x and y must. `[0, 0, 0, 7]` draws all of the weather,
  since the darkening reads only the flat world height and the wetness only `surface_wetness` and the face normal.
  On pattern 3, z is the noise cells per render unit and scales only the noise (Decisions, "What z is").
- **The header's comments catch up.** The batch comment lists patterns 0–3 only (`include/btbh_gpu/btbh_gpu.h:924-928`).
  `surface_wetness` says patterns 1 and 4 (`:1056-1058`), but the shader wets 1, 3 and 4. Both are corrected.
- **The shader.**

  ```hlsl
  // Pattern 3's street darkening, shared with pattern 7 (the same function, so the same bytes).
  float btbh_street_darkening(float height)
  {
      return lerp(0.8f, 1.0f, saturate(height * 4.0f));
  }

  // apply_surface_detail, after pattern 6's early return (line 1888) and before any noise:
  if (pattern == 7u)
  {
      albedo.rgb *= btbh_street_darkening(input.world_position.y);
      return float3(0.0f, 0.0f, 0.0f);
  }
  // Line 1960 becomes: tint *= btbh_street_darkening(input.world_position.y);

  // apply_surface_wetness: the gate (line 2058) takes 7, and the wall branch (line 2065) is
  // `if (pattern == 3u || pattern == 7u)`.
  ```

- **Why it equals pattern 3 with x and y at 0.** There, `tint = 1 + 0 × (2 height + stain) = 1` exactly, times the
  darkening; the albedo is multiplied by that. The bump is `height × 0 × wet_bump = 0`, so its gradient is 0. The
  wetness branch is the same code. Pattern 7 computes the same values without the 40 hashes. The test below checks
  it byte for byte.
- **v2 can see the look on 0.6.0 today.** `[0, 0, 6, 3]` (z must stay above 0 for pattern 3) draws the same pixels,
  at the grain's cost. Any z above 0 does: with x and y at 0, z reaches no pixel.
- As every pattern, it isn't drawn on batches that take the fast unlit pipeline (`btbh_gpu_scene.c:1153-1166`).

### Phase 2: the world-detail table

#### Texture arrays

The two functions RFC 0001 drafted, with its signatures. RFC 0001 then uses them as they are.

```c
/* A 2D array texture: layer_count layers of one size and colour space, each with its own mips, sampled through the
 * slots that take an array (BTBHGPUSceneWorldDetailTable.map_layers, since 0.8.0). Every other texture slot (batch,
 * world detail, canvas, LUT, render target) refuses one, and btbh_gpu_texture_update_rgba8 refuses it.
 * btbh_gpu_texture_destroy frees it. BTBHGPUTextureReport's width, height and mip_count describe one layer.
 * layer_count: 1..BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS. generate_mipmaps as btbh_gpu_window_texture_create_rgba8_ex. */
#define BTBH_GPU_TEXTURE_ARRAY_MAX_LAYERS 256u

BTBHGPUTexture *btbh_gpu_window_texture_array_create_rgba8_ex(
    BTBHGPUWindow *window,
    const uint8_t *const *layer_pixels,   /* layer_count pointers, each width * height * 4 bytes, row 0 at the top */
    uint32_t width,
    uint32_t height,
    uint32_t layer_count,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
);
/* The same from image files (PNG, JPG, ...), one per layer, in order. Every image must have the first one's size:
 * otherwise BTBH_GPU_INVALID_ARGUMENT, naming the path. */
BTBHGPUTexture *btbh_gpu_window_texture_array_load_ex(
    BTBHGPUWindow *window,
    const char *const *layer_paths,
    uint32_t layer_count,
    uint8_t generate_mipmaps,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
);

/* Defined when the two functions above exist (since 0.8.0). */
#define BTBH_GPU_HAS_TEXTURE_ARRAYS 1
```

- 256 is Vulkan's guaranteed minimum of `maxImageArrayLayers`; Metal allows 2048.
- No feature bit, as RFC 0001 says: RGBA8 2D arrays are core on Vulkan and Metal. The call still asks
  `SDL_GPUTextureSupportsFormat` for `SDL_GPU_TEXTURETYPE_2D_ARRAY` and returns `BTBH_GPU_MISSING_CAPABILITY` if not.
- The handle stays `BTBHGPUTexture`, as in RFC 0001, so destroy and reports work unchanged. The checks are at run
  time (Alternatives).

#### The table

```c
#define BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES 8u
/* An entry's normal_layer or orm_layer: that map is absent. Also the vertex index that means "no detail". */
#define BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER 255u

/* One entry of a world-detail table: the eight floats of BTBHGPUSceneWorldDetail (same names, order and meaning,
 * same zero values), then the layers of the table's array it samples. */
typedef struct BTBHGPUSceneWorldDetailTableEntry {
    float level_tile[2];       /* finite and above 0 */
    float wall_tile[2];        /* finite and above 0 */
    float albedo_scale;        /* 0 means 1 */
    float normal_scale;        /* 0 means 1 */
    float roughness_scale;     /* 0 means 1 */
    float level_degrees;       /* 0 means 18; below 90 */
    /* Layers of map_layers: the normal (OpenGL +Y, xy read, z rebuilt) and the ORM (r occlusion, g roughness
     * factor, b the grey albedo multiplier, linear). BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER: that map is absent. */
    uint8_t normal_layer;
    uint8_t orm_layer;
    uint8_t flags;             /* 0 */
    uint8_t reserved;          /* 0 */
} BTBHGPUSceneWorldDetailTableEntry;

/* Several world details in one batch, chosen per vertex (RFC 0009, since 0.8.0). Each vertex's index is the first
 * float of its joint slot (BTBH_GPU_CUSTOM_MESH_DETAIL_INDEX_FLOAT), rounded: entries[index], projected and blended
 * as BTBHGPUSceneWorldDetail with ALBEDO_IN_ORM_BLUE. An index at or past entry_count (255 by convention) leaves
 * the triangle without a detail. All three vertices of a triangle must carry the same index. Meant for custom
 * meshes: a skinned batch takes entry 0 everywhere (its joints are bones), and so does a model loaded without
 * joints. The host owns it; the renderer reads it, and the entries, during the present call only. */
typedef struct BTBHGPUSceneWorldDetailTable {
    /* A 2D array texture (btbh_gpu_window_texture_array_*), linear, WRAP_REPEAT, holding every entry's maps.
     * NULL: the batch draws as without a table. */
    BTBHGPUTexture *map_layers;
    const BTBHGPUSceneWorldDetailTableEntry *entries;
    uint32_t entry_count;      /* 1..BTBH_GPU_SCENE_WORLD_DETAIL_TABLE_MAX_ENTRIES */
    uint8_t seed_component;    /* as BTBHGPUSceneWorldDetail.seed_component, for every entry */
    uint8_t flags;             /* 0 */
    uint8_t reserved[2];       /* 0 */
} BTBHGPUSceneWorldDetailTable;

/* In BTBHGPUSceneGeometryBatch, appended after world_detail: */
    /* NULL: drawn as before. Otherwise a world detail chosen per vertex (BTBHGPUSceneWorldDetailTable, RFC 0009,
     * since 0.8.0). A batch can't have both this and a world_detail with a texture. */
    const BTBHGPUSceneWorldDetailTable *world_detail_table;

/* The custom-mesh vertex, documented (it is the model vertex too): 24 floats, 96 bytes. */
#define BTBH_GPU_CUSTOM_MESH_VERTEX_FLOATS 24u   /* position 0-2, normal 3-5, uv 6-7, tangent 8-11, colour 12-15,
                                                  * joints 16-19, weights 20-23 */
#define BTBH_GPU_CUSTOM_MESH_DETAIL_INDEX_FLOAT 16u

/* Defined when world_detail_table and its structs exist (since 0.8.0). */
#define BTBH_GPU_SCENE_HAS_WORLD_DETAIL_TABLE 1
```

Layouts on 64-bit. `BTBHGPUSceneWorldDetailTableEntry`, 36 bytes, aligned to 4:

| Offset | Field | Same as in `BTBHGPUSceneWorldDetail` |
| --- | --- | --- |
| 0 | `level_tile[2]` | offset 24 |
| 8 | `wall_tile[2]` | 32 |
| 16 | `albedo_scale` | 40 |
| 20 | `normal_scale` | 44 |
| 24 | `roughness_scale` | 48 |
| 28 | `level_degrees` | 52 |
| 32 | `normal_layer` | (new) |
| 33 | `orm_layer` | (new) |
| 34 | `flags` | (new) |
| 35 | `reserved` | (new) |

`BTBHGPUSceneWorldDetailTable`, 24 bytes:

| Offset | Field |
| --- | --- |
| 0 | `map_layers` |
| 8 | `entries` |
| 16 | `entry_count` |
| 20 | `seed_component` |
| 21 | `flags` |
| 22 | `reserved[2]` |

The batch grows from 272 to 280 bytes; `world_detail_table` sits at 272. The entry's first 32 bytes are
`BTBHGPUSceneWorldDetail`'s bytes 24–55, so a host can copy them across.

**Zero values.**
- **The batch:** `world_detail_table` NULL draws as before. So does a table whose `map_layers` is NULL.
- **An entry:** scales and angle as in RFC 0008. The tiles have no default. A zeroed entry samples layer 0 for both
  maps, which is a mistake but harmless. A map that should be absent says so with 255.

**Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, naming the batch and the fault, as 0.6.0's detail checks do):
- `entry_count` 0 or above 8, or `entries` NULL, with `map_layers` set;
- an entry's tile not finite and above 0, a scale not finite, `level_degrees` outside [0, 90), a layer neither 255
  nor below the array's layer count, `flags` or `reserved` not 0;
- `seed_component` above 4, the table's `flags` or `reserved` not 0;
- `map_layers` not an array texture, or of another window or destroyed;
- an array texture in any 2D slot, the batch's four and the world detail's three included;
- a batch with both a table whose `map_layers` is set and a world detail that has a texture.

A table on a refracting batch is ignored, as a world detail is. Checks run per batch, skipping a table that is the
same pointer as the previous batch's, as 0.6.0 does for details.

## Minimal example

```c
/* Phase 1: a building's facade with the world detail instead of the grain, keeping its weather. */
batch.detail_parameters[3] = (float)BTBH_GPU_SCENE_DETAIL_FACADE_WEATHER;   /* x, y, z stay 0 */

/* Phase 2: v2's eight macro classes in one array, 512² each, linear: class k's normal at layer 2k, its ORM (the
 * grey albedo in blue) at 2k + 1. */
const char *maps[16] = { "macro/brick_normal.png", "macro/brick_orm.png", /* ... 14 more ... */ };
BTBHGPUTextureReport tr;
BTBHGPUTexture *macro = btbh_gpu_window_texture_array_load_ex(window, maps, 16, 1,
    BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &tr);

BTBHGPUSceneWorldDetailTableEntry entries[8] = {0};
for (uint32_t k = 0; k < 8; ++k) {
    entries[k].normal_layer = (uint8_t)(2 * k);
    entries[k].orm_layer = (uint8_t)(2 * k + 1);
    entries[k].albedo_scale = entries[k].roughness_scale = 1.25f;
    /* macro.json's tile_cells: walls 2 x 1.4, ground 4 x 4, the same pair for both projections */
    entries[k].level_tile[0] = entries[k].wall_tile[0] = tile_cells[k][0];
    entries[k].level_tile[1] = entries[k].wall_tile[1] = tile_cells[k][1];
}
BTBHGPUSceneWorldDetailTable table = {0};
table.map_layers = macro;
table.entries = entries;
table.entry_count = 8;
table.seed_component = 4;                       /* custom.w, the lot's index, as with RFC 0008 */

/* The merged distance model: each vertex names its class, 255 for the untextured materials. */
vertex[BTBH_GPU_CUSTOM_MESH_DETAIL_INDEX_FLOAT] = (float)entry_of_class;

batch.world_detail_table = &table;              /* world_detail stays NULL */
```

In v2's Rust:
- The parser pushes each vertex with `..Default::default()` (`glb.rs:360-367`). It sets
  `joints: [entry as f32, 0.0, 0.0, 0.0]` there instead. The distance models are split with `surface_class`, but the
  full `classify(material)` is at hand (`kit.rs:330`) to give the entry.
- Each primitive's vertices are appended on their own (`glb.rs:346`), and `Build::merged` only concatenates
  (`glb.rs:142-152`), so no vertex is shared between materials.
- `KitLibrary` keeps the array and the eight entries in place of eight boxed details.
- `batch_of` gains `world_detail_table: table.map_or(std::ptr::null(), |t| t as *const _)`.
- With every kit mesh carrying its index, the per-class draws can use the same table. Then the maps are loaded once,
  as one array, and the eight 2D sets go away (Cost, memory).

## Implementation

### Phase 1

| File | Change |
| --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | The enum, the macro, the two comments |
| `src/gpu/btbh_gpu_scene.c` | Validation accepts 7 with x, y, z at 0 (`:4512-4520`) |
| `shaders/gpu/production_model.frag.hlsl` (and its five variants, which include it) | `btbh_street_darkening`, the pattern-7 branch, the wetness gate |
| `Sources/` (Swift) | Nothing. A `SurfaceDetail.Pattern.facadeWeather` can follow when v1 asks; `isEnabled` would then allow x = y = 0 for it, as for glass and the lamp (`Sources/BTBHRenderer/RenderResources.swift:274-279`) |

- No shader file is new. The unlit, radiance-cascade source and shadow shaders don't use the detail.
- **A stale shader can't be detected.** Pattern 7 adds no named resource for the SPIR-V name check to find. With
  0.6.0's shaders, a pattern-7 batch falls into the ground branch with x and y at 0: no darkening, no wetness, one fbm
  paid. The CHANGELOG says to rebuild the shaders with the core, as for every release.

### Phase 2

- **Header.** The two array functions, the two structs, the macros, the appended pointer, the vertex layout comment.
- **`src/gpu/btbh_gpu_internal.h`.** `BTBHGPUTexture` gains `uint32_t layer_count` (0 for a 2D texture).
- **`src/gpu/btbh_gpu_texture.c`.** Array create and load: `SDL_GPU_TEXTURETYPE_2D_ARRAY`, one
  `SDL_UploadToGPUTexture` per layer in one copy pass (`SDL_GPUTextureRegion.layer`), then one
  `SDL_GenerateMipmapsForGPUTexture`. Usage `SAMPLER | COLOR_TARGET` when mips are generated, as for 2D.
- **Every texture check** refuses an array in a 2D slot: the scene batch's four, the world detail's three, the
  canvas batch and pass target, the post LUT, the capture destination, the render-to-target target, and
  `texture_update_rgba8`. Vulkan's validation layers would catch a mismatched view type, but the core must refuse it
  first: without the layers, a 2D-array view in a 2D slot is undefined. An array is never a render target: its
  `COLOR_TARGET` usage is for mip generation only, as for today's mipped textures.
- **`src/gpu/btbh_gpu_scene.c`.**
  - Validation, with the refusals above.
  - **The static vertex input.** One more attribute: location 5, `FLOAT`, offset 64 (`:1396-1434`). The animated
    pipeline is unchanged.
  - **Samplers.** One more after the world detail's three: 15, 14 and 10 instead of 14, 13 and 9 (`:3078-3085`,
    `:8758-8800`). A batch without a table binds a 1×1, one-layer fallback array: the slot's view type must be an
    array even when nothing samples it.
  - **Uniform buffers.** The fragment shader is created with 3 (`:1358`). A table batch pushes its 400 bytes into
    slot 2.
  - `btbh_gpu_scene_uses_fast_unlit_pipeline` (`:1153-1166`) returns false for a batch with an active table.
  - **The name check** (`:5643-5705`): a present with an active table needs the scene fragment shader to name
    `world_detail_maps`. Otherwise it fails with the shader's path, as 0.6.0 does for `world_detail_normal_texture`.
- **`src/gpu/btbh_gpu_world_detail.h`.** The entry's checks and packing next to the detail's, reusing its frame, hash
  and coordinate for the native tests.
- **Shaders.**

  | Shader | Changes? | Why |
  | --- | --- | --- |
  | `production_model.vert` | Yes | Input `float detail_index : TEXCOORD7`, declared after the tangent. Output `nointerpolation uint detail_index : TEXCOORD11`, rounded and clamped to 0–255. Both inside `#if !BTBH_RC_SOURCE_PROJECTION` |
  | `production_animated_model.vert` | Yes | Writes `detail_index = 0` (its joints are bones) |
  | `production_model.frag` (and its five variants) | Yes | The array at t14/t13/t9, the `WorldDetailTable` block, the table path |
  | `production_rc_material_source.vert` | No | It includes `production_model.vert` with `BTBH_RC_SOURCE_PROJECTION`, so the guard keeps its input and output. The RC pipelines (`src/gpu/btbh_gpu_radiance_cascades.c:988-1046`) are unchanged |
  | `production_model_unlit.frag`, shadow shaders | No | A table batch never takes the fast unlit pipeline. The table changes no coverage or depth |

  - **Vertex locations.** shadercross compacts the static shader's inputs, so the tangent sits at location 4
    (`docs/superpowers/specs/2026-07-28-production-direct-pbr-design.md:42-47`). Declared after it, the index
    takes location 5 and locations 0–4 don't move. That keeps 0.6.0's static vertex shader working under the new core:
    the pipeline's sixth attribute just goes unread.
  - No shader file is new or renamed. CHIMERA's and Tyrian3D's shader lists (read from `CMakeLists.txt`) and v2's
    named shaders stay as they are.
- **The fragment shader.**

  ```hlsl
  Texture2DArray<float4> world_detail_maps : register(t14, space2);   // t13 (_2d_shadows), t9 (unshadowed)
  SamplerState world_detail_maps_sampler : register(s14, space2);

  cbuffer WorldDetailTable : register(b2, space3)
  {
      float4 world_detail_table_tiles[8];      // per entry: 1 / level_tile (u, v), 1 / wall_tile (u, v)
      float4 world_detail_table_controls[8];   // per entry: albedo, normal, roughness scales, cos(level angle)
      uint4  world_detail_table_layers[8];     // per entry: x normal layer, y ORM layer (255 none); zw 0
      uint4  world_detail_table_count;         // x the entry count; yzw 0
  };

  // In shade_fragment (line 2187), the branch stays uniform. world_detail_mode.x gains a bit, 16: "the table".
  // world_detail_mode.zw stay free for RFC 0008 phase 2b.
  if (world_detail_mode.x != 0u)
  {
      world_gradient = (world_detail_mode.x & btbh_world_detail_map_table) != 0u
          ? apply_world_detail_table(input, lod_bias, albedo_sample, orm)
          : apply_world_detail(input, lod_bias, albedo_sample, orm);
  }

  // apply_world_detail_table: the entry at min(index, 7) gives the tiles, controls and layers; the frame, the
  // seed's offset and the coordinate are RFC 0008's. Two SampleBias at float3(coordinate, layer). Then, with
  // on = index < count and the layer not 255, each change is chosen by a select: orm.r *= on ? r : 1, and so on.
  ```

  The table path and the single path stay one function each so the single path's code doesn't change. Whether that
  or one shared function keeps fewer registers is for the implementation to measure.

### Cost

These are estimates, to measure in each phase's bench.

**Phase 1.**

| Item | Estimate |
| --- | --- |
| Pattern 7, per pixel | About 20 ALU instructions (a lerp, the face normal shared with the shader's other uses, the wet branch). No texture, no hash |
| Pattern 3, per pixel | 40 hashes (160 integer multiplies) in 10 noise lookups: several hundred instructions |
| Desktop GPU | Pattern 7 under 0.01 ms. In v2's frames pattern 3 costs 0.11–0.13 ms on Medium and 0.26–0.28 ms on High (Need) |
| Steam Deck | Pattern 7 about 0.01 ms for 0.2–0.5 million facade pixels at 960 × 600. Pattern 3 likely near a millisecond: the Deck has about 1/13 of an RTX 3070 Ti's ALU rate and 28 % of 1080p's pixels. That holds if v2's RTX is like the bench's 3070 Ti and High's 0.27 ms was at about 1080p |
| Unset | Nothing measurable expected: one more compare on a uniform in a function that already branches on the pattern |

- On Medium and High, v2 can drop the grain and keep the weather for about 0.1–0.3 ms less on its desktop.
- On Low and the Deck, v2 can set pattern 7 on its facades and get back the darkening and the wet walls, for about
  0.01 ms.

**Phase 2.**

| Item | Estimate |
| --- | --- |
| Per pixel, with a table | RFC 0008's packed cost (2 trilinear samples, about 40 ALU), plus about 10 ALU and three 16-byte loads for the entry |
| Desktop GPU, per screen of detailed surface | 0.04–0.05 ms at 1080p, 0.005–0.012 ms at 960 × 600: 0.6.0's measured packed cost, plus a few percent |
| Steam Deck, per screen | 0.04–0.12 ms (RFC 0008's estimate for the packed detail), plus a few percent |
| v2's Deck preset | Nearly every kit pixel gains the detail, since the merged distance models have none today. That is the split's GPU work without its +0.6–0.85 ms: about 0.01–0.02 ms on the desktop (a 960 × 600 screen with some overdraw) and 0.04–0.12 ms on a Deck (RFC 0008's estimate for all kit fragments) |
| CPU per table batch | One 400-byte uniform push. The array binding changes only when the table's array does (v2: never). Validation once per run of batches sharing a table |
| Unset, GPU | The static vertex shader reads 4 more bytes and writes one flat value. The bytes come from a cache line the static pass already reads (96-byte stride: neighbouring vertices cover every 64-byte line). The fragment shader's longer branch may cost registers again (0.6.0: +0.03–0.04 ms a heavy 1080p screen, nothing under the light pixel) |
| Unset, CPU | One more sampler binding per draw (the same fallback every time) |
| GPU memory | 16 maps of 512² with mips: 21.3 MiB in one array, the same as v2's 16 2D maps. Keeping both the 2D sets and the array doubles that to 42.7 MiB |

**How to measure it.**
- **Phase 1 bench** (`BTBH_SMOKE_BENCH=1`, as 0.6.0): eight coplanar screen-filling walls drawn with `LESS_EQUAL`,
  pattern 3 (x 0.07, y 0) against pattern 7, under the heavy and the light pixel, at 1920 × 1080 and 960 × 600.
- **Phase 2 bench:**
  - per screen: each layer one mesh whose triangles alternate between two entries, against the single packed detail;
  - CPU: 200 merged batches against 1,000 split ones, the encode time;
  - unset: against 0.7.0's shaders, under the heavy pixel, and with many small instances for the extra attribute.
- **Registers:** Radeon GPU Analyzer on `production_model.frag.spv` and `production_model.vert.spv` for gfx1033 (the
  Deck) and gfx1030, before and after. It wasn't installed for 0.6.0.
- **v2:** its frame telemetry with the table on the merged distance models, against `BTBH_LOD1_SPLIT=1` and against
  no detail at a distance. Then the owner's Deck.

## Compatibility

- **Versioning.** Both phases are minor and opt-in: an enum value appended, new functions, new structs, a pointer
  appended to the batch. Phase 1 is 0.7.0, with the pipeline pre-warm, and phase 2 0.8.0 (Decisions, 8).
  - Phase 1 changes no struct. Phase 2 grows the batch from 272 to 280 bytes. Every game compiles the C core from the
    checkout whose header it builds against, so they agree on the size, as for 0.5.0 and 0.6.0.
  - The scene shaders change in both phases and must be rebuilt with the C core.
    - Phase 1: old shaders under the new core draw every pattern but 7 as before; 7 draws without its weather.
    - Phase 1's shaders under 0.6.0's core draw as before; that core refuses pattern 7.
    - Phase 2: old shaders under the new core draw as before: the extra sampler, uniform buffer and vertex attribute
      go unread. A batch with a table is refused by the name check.
    - Phase 2's shaders under an older core don't work: too few samplers, uniform buffers and vertex attributes.
  - **Byte-identical when unset** is tested, not assumed, as for 0.6.0.
- **Be The Bullet Hell v2 (Rust, bindgen):**
  - Nothing changes until it opts in. bindgen generates the new structs with `Default`, and `batch_of` ends in
    `..Default::default()`, so `world_detail_table` is null.
  - Phase 1: where `kit_surface` replaces the grain, `detail: [0.0, 0.0, 0.0, 7.0]` instead of `[0.0; 4]`
    (`scene.rs:548`). On Low and the Deck, `SurfaceLook::flat()` sets the facade to `[0.0, 0.0, 0.0, 7.0]` too
    (Decisions, 8).
  - Phase 2: the array and the entries in `KitLibrary`; each kit vertex's entry in `joints[0]`; `world_detail_table` on
    the merged distance models (and, if it wants, on every kit draw). `BTBH_LOD1_SPLIT` can go.
- **Be The Bullet Hell (v1, Swift):**
  - Nothing changes. The Swift layer builds batches zeroed. Its `SurfaceDetail.Pattern` has no case 7, so it never
    sends it.
  - Its static draws bind one more vertex attribute. Its custom meshes' joint slots are read and then unused.
- **CHIMERA (Rust, bindgen):** nothing changes. `default_batch()` ends in `..Default::default()`. It regenerates the
  bindings for the bigger batch (phase 2). No shader file is added to its list of 14.
- **Tyrian3D (C):** nothing changes. `init_batch` memsets the batch. It is pinned at v0.2.1.
- **RFC 0001 (Draft):** phase 2 ships its array functions with its signatures. Its terrain shaders are separate files
  with their own bindings, so the slot count here doesn't touch them.
- **RFC 0007's SDF terrain:** its chunks carry layer weights in bytes 64–95. A table on a chunk's batch would read
  layer 0's weight as the index. Tables aren't meant for terrain chunks; the header says so.

## Platforms and testing

- **Native tests** (no GPU, `tools/check.ps1` and `check.sh`):
  - Phase 1: pattern 7 accepted with x, y, z at 0; refused with any of them set; 8 refused.
  - Phase 2, `btbh_gpu_scene_order_test`: `offsetof(world_detail_table) == 272`, `sizeof(BTBHGPUSceneGeometryBatch) ==
    280`, `world_detail` still at 264, `sizeof(BTBHGPUSceneWorldDetailTable) == 24`, `sizeof(...Entry) == 36`, and the
    entry's floats at the detail's offsets minus 24.
  - Phase 2: every refusal above. The entry's packing into the 400-byte block.
- **GPU smoke, phase 1** (`btbh_gpu_world_detail_smoke` or a new one; Windows/Vulkan):
  - **Equal:** pattern 7 renders byte for byte as pattern 3 with x and y at 0, dry and at `surface_wetness` 1, through
    every scene shader variant. A difference from FMA contraction would be reported, not hidden.
  - **Unset:** the gallery byte for byte as 0.6.0's core and shaders.
- **GPU smoke, phase 2:**
  - **One entry against the single detail:** a table of one entry, every index 0, the same two maps as layers, renders
    byte for byte as RFC 0008's packed detail with the same maps as 2D textures. Both mip chains come from the same
    SDL blits.
  - **Merged against split:** one mesh of two boxes with indices 0 and 1 renders byte for byte as two batches with the
    matching single details. That covers the seam between classes (no line) and the per-entry tiles and scales.
  - **None:** an index of 255, or past the count, renders byte for byte as no detail.
  - **Skinned:** a table on an animated batch draws, with entry 0.
  - **Arrays:** creation and load (sizes, a layer of another size refused, 0 and 257 layers refused, mips per layer);
    an array as a batch albedo refused; a 2D texture as `map_layers` refused; `texture_update_rgba8` on an array
    refused.
  - **Old shaders:** a table batch with 0.7.0's shaders fails with the shader's path; without one, they draw.
  - **Unset:** the gallery and `btbh_gpu_scene_target_smoke`'s images byte for byte as the release before, with
    `world_detail_table` NULL, with a table whose `map_layers` is NULL, and with the release before's shaders.
  - All with the Vulkan validation layers on: a view-type mismatch shows there.
- **Platforms:**
  - Windows/Vulkan is where the smokes run.
  - Linux and the Deck (RADV) through v2 when it runs there.
  - macOS/Metal: the shaders change, so the Mac session runs `tools/check.sh` (metallib) on the candidate before each
    tag. `Texture2DArray` and a flat `uint` go through shadercross to MSL as other shaders' do; not run on Metal yet.
- **Games:** CHIMERA 0 pixels different with nothing set; Be The Bullet Hell its Swift suite; v2 its kit captures and
  A/B timings, near and far.

## Alternatives

| Alternative | Why not (now) |
| --- | --- |
| **Split by class** (v2's `BTBH_LOD1_SPLIT=1`) | +0.6–0.85 ms a frame on the Deck preset, measured. The table keeps one draw |
| **The index in the colour's alpha** | The alpha multiplies the albedo's alpha, which the cut-off and blending read. It is interpolated |
| **A new vertex attribute or stream** | A new custom-mesh call and layout, while the static vertex's bytes 64–95 go unread |
| **The instance record** | Per instance, not per triangle. A merged mesh mixes classes within one instance |
| **`SV_PrimitiveID` and a per-triangle class buffer** | A buffer per mesh, a storage-buffer binding and a new API. Reading the primitive ID in a fragment shader needs the geometry (or tessellation) capability in SPIR-V |
| **16 separate textures** | 30 samplers in the full variant, which allows 16 |
| **An atlas** | Bleeds between tiles at the mips the distance models read. Needs gutters, a capped LOD and `SampleGrad`, and loses the sampler's repeat |
| **Two arrays (normals, ORMs)** | Clearer, and lets the normals be BC5 while the ORMs are BC7 (RFC 0008 phase 2). But it fills the full variant to 16 of 16. Open question 2 |
| **A table of `BTBHGPUSceneWorldDetail`s** (the existing pointer plus a count) | Their textures are 2D. Eight of them still need 16 samplers, or the renderer copying them into hidden arrays: twice the memory, a cache keyed by texture pointers, and copies whenever one changes |
| **Extending `BTBHGPUSceneWorldDetail`** (array fields in it) | One struct whose texture fields mean 2D or array by context. The table is a separate struct and pointer, so RFC 0008's stays as it is |
| **A storage buffer for the entries** | A copy pass and a buffer per frame, for 400 bytes. Possible later if a table grows past what a uniform block holds |
| **A distinct handle type for arrays** (`BTBHGPUTextureArray`) | Type-safe at compile time, also in Rust. But it needs its own destroy and report, and RFC 0001 drafted `BTBHGPUTexture`. The run-time check costs one compare per slot |
| **A procedural pattern per entry** | Patterns are per batch and branch on a uniform. Per entry they would branch per pixel, around derivatives |
| **Request 2: pattern 3 skipping its noise when x and y are 0** | No API at all, and the same pixels. But it is a hidden rule: the cost depends on two values, and z must stay above 0 though unused. It is how v2 can see the look on 0.6.0 today |
| **Request 2: a batch flag** | The batch flags change passes (`NO_SUN_SHADOW`, `KEEP_ORDER`). The darkening and the wetness belong to pattern 3; a flag beside pattern 3 would apply them twice |
| **Request 2: a flag on the world detail** | It ties the weather to the detail. Low and the Deck want the weather with no detail too |

## Open questions for v2

1. **How many entries?** Eight fits v2's eight macro classes and a 400-byte uniform block. A merged distance model
   holds at most five of them (brick, tiles, concrete, roof, asphalt). Is eight enough for every merged draw you
   plan, or should the block be sized for 16?
2. **One array, one size, one format.** All normal and ORM layers share one array, so they need one size, one colour
   space and one mip count. Are all sixteen maps 512² linear RGBA8, and will they stay so? When BC comes (RFC 0008
   phase 2), one array means one format: BC7 for the normals too, not BC5. Is that acceptable, or do you want two
   arrays at the price of the full shader's last sampler slot?
3. **The index in `joints[0]`.** Is `joints` free on every kit and prop mesh you build? Your GLB parser leaves it at
   zero and shares no vertex between materials (`glb.rs:346-367`). Does any other mesh you would merge share a vertex
   between triangles of two classes? Such a vertex would have to be split.
4. **Which draws merge.** From your code, the distance models merge brick, tiles, concrete, roof, asphalt and the
   untextured materials into one `Plain` draw per node by default (`kit.rs:325-333`), and `BTBH_LOD1_SPLIT=1` is the
   split you measured. Is that the merge you mean? Will the full models (textured, one draw per class) move to the
   table too, so the macro maps are loaded once as one array instead of twice (21 MiB more)?
5. **Untextured triangles in a merged draw** (metal, awnings, plain trim): no detail (index 255), or one of the
   classes?
Questions 6–8, on phase 1, are answered: see "Decisions".

## Decisions (v2, 2026-11-20)

v2 answered questions 6–8, the ones on phase 1. The owner left the decision to the coordinator, who accepted
phase 1 for 0.7.0 on these answers. Questions 1–5, on phase 2, are still open.

6. **Mixed merged draws: few, and pattern 7 at street level is fine as drafted.**
   - Ground pieces never merge with walls. `road/`, `plaza/`, `water/`, `bridge/`, `rubble/` and `shared/marking`
     are pieces of their own (`KitMesh::ground`) with patterns 1 or 4, never 3 or 7.
   - A building piece's merged lod1 draw is walls, roofs and trim.
   - A few ground floors carry flat yard surfaces at street level in their `Plain` draw: gardens' soil and gravel,
     the gas station's forecourt trim, house yards, the substation yard. They take pattern 3 today and darken to
     0.8 on Medium and High. On Low and the Deck they would start to darken too, which reads as a contact shade and
     is acceptable.
   - No gate on the face normal. Pattern 7 stays byte for byte pattern 3 with x = y = 0.
7. **Wet relief: the darker, glossier walls are enough.**
   - From the classic camera, the storm's wet look comes from wet ground, puddles, SSR, and v2's rain and fog.
   - Wetness strengthening the world detail's tilt can come later as an option, at the lowest priority. Phase 1
     doesn't wait for it.
8. **Order: phase 1 first and on its own, as 0.7.0 with the pipeline pre-warm.**
   - v2 will set `SurfaceLook::flat()`'s facade and the grain's replacement to `[0, 0, 0, 7]`. That gives Low and
     the Deck the street darkening and the wet walls back, for about 0.01 ms.
   - Phase 2 can follow as 0.8.0. Nothing in v2 waits on it but detail on the merged distance models.

**What z is** (settled with answer 8):
- `[0, 0, 0, 7]` is pattern 7 as drafted, with all of its weather. Pattern 7 reads none of x, y and z.
- For pattern 7, z means nothing yet and must be 0, as x and y must. A pattern-7 batch with any of them set is
  refused (Phase 1, "Refusals"). They stay free to carry the darkening's height and depth later.
- On pattern 3, z is the noise cells per render unit. It scales only the noise: the grain, the stains and the
  footprint they fade with. With x and y at 0 no noise reaches a pixel, so pattern 3 draws the same at any z above
  0. Pattern 3 refuses z = 0. That is the only reason the 0.6.0 preview reads `[0, 0, 6, 3]`: 6 stands for any
  positive number. The draft never meant pattern 7 to take it.
- So "byte for byte pattern 3 with x = y = 0" compares `[0, 0, 0, 7]` with `[0, 0, z, 3]` for z above 0, at more
  than one z.

Still open:

- **Phase 2:** questions 1–5.
- **Phase 1, for the implementation:** whether the compilers keep pattern 7 byte for byte pattern 3 (FMA
  contraction). The test reports a difference rather than hiding it.

## Phases

1. **Phase 1, the facade's weather (0.7.0).** Pattern 7, the pattern names, the two header comments corrected,
   validation, the equality test and the bench. Small: one shader function and one check. It doesn't wait for the
   answers on phase 2.
2. **Phase 2, the world-detail table (0.8.0; Decisions, 8).** Texture arrays (RFC 0001's functions), the
   table and its entries, the static vertex's index, one more sampler and a third uniform buffer, the refusals, the
   tests and the bench. It starts once v2 has answered questions 1–5.

**Can phase 2 come first, or alone?** Alone, yes: neither phase uses the other. First isn't needed: phase 1 is
smaller, and on its own it gives Low and the Deck their walls' weather back.
