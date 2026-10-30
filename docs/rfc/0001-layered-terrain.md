# RFC 0001: Layered terrain in one pass

- **Status:** Draft
- **Requested by:** CHIMERA (`RTS/docs/v2/izgled-mapa.md` §3 and §9.1, branch `v2-izgled`)
- **Affects:** CHIMERA (adopts it). Be The Bullet Hell and Tyrian3D: nothing, it is opt-in (see Compatibility)
- **Issue:** to be opened

## Need

CHIMERA's ground has seven materials (sand, grit, salt grass, salt crust, wet sand, tracks, flagstones),
each with its own tiling and rotation. The renderer draws one texture set per batch, so the ground today is an
opaque base plus six transparent copies of the same vertices whose alpha is the layer's share at each vertex
(0.5 m apart). That costs:

- overdraw: every layer redraws its triangles, in the scene and in every shadow cascade;
- soft, linear transitions between vertices 0.5 m apart, never "stone pokes through the sand";
- no blending by the textures' own height.

The ground carries the game (wet shallows, the stone causeway, tracks to objectives), so it must read sharply
at RTS distance.

## Proposal

A terrain is a normal scene batch (`BTBHGPUSceneGeometryBatch`, a model or custom mesh, so instancing, shadows,
visibility, radiance cascades and FSR 3 motion vectors keep working) with one new optional pointer. When it is
`NULL` (as in every zeroed batch) nothing changes.

```c
#define BTBH_GPU_SCENE_TERRAIN_MAX_LAYERS 8u

/* Texture arrays: layer_count same-size RGBA8 layers sampled as one texture (a 2D array). */
BTBHGPUTexture *btbh_gpu_window_texture_array_create_rgba8_ex(
    BTBHGPUWindow *window,
    const uint8_t *const *layer_pixels,   /* layer_count pointers, each width * height * 4 bytes */
    uint32_t width,
    uint32_t height,
    uint32_t layer_count,
    uint8_t generate_mipmaps,             /* as btbh_gpu_window_texture_create_rgba8_ex */
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureColorSpace color_space,
    BTBHGPUTextureReport *report
);
/* The same from image files (PNG/JPG/...), one per layer; all must share a size. */
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

typedef struct BTBHGPUSceneTerrain {
    /* Arrays with one layer per ground material, all the same layer count (1..8):
     * albedo (sRGB; alpha = the material's height, 0..1, for the blend), normal (tangent space),
     * ORM (occlusion, roughness, metalness). NULL normal/ORM: flat and the batch's factors. */
    BTBHGPUTexture *albedo_layers;
    BTBHGPUTexture *normal_layers;
    BTBHGPUTexture *orm_layers;
    uint32_t layer_count;
    /* Weights in world XZ: control_textures[0].rgba = layers 0-3, [1].rgba = layers 4-7 (NULL when
     * layer_count <= 4). Linear RGBA8, usually made with btbh_gpu_window_texture_create_rgba8_ex and
     * streamed with btbh_gpu_texture_update_rgba8. control_rect: min x, min z, max x, max z it covers. */
    BTBHGPUTexture *control_textures[2];
    float control_rect[4];
    /* Per layer: x, y world units per texture repeat; z rotation (radians); w unused. */
    float layer_tiling[BTBH_GPU_SCENE_TERRAIN_MAX_LAYERS][4];
    /* Height blend sharpness: 0 blends by the weights alone (linear, like today's overlays); larger
     * values let the higher material win over a band of that many height units (0.05..0.3 typical). */
    float height_blend;
    /* 1: the weights may sum to less than 1 and the remainder shows the batch's own albedo/normal/ORM
     * (a base material); 0: they are renormalised. */
    uint8_t base_material;
    uint8_t reserved[3];
} BTBHGPUSceneTerrain;

/* In BTBHGPUSceneGeometryBatch, appended: */
    const BTBHGPUSceneTerrain *terrain;   /* NULL: an ordinary batch */
```

The host keeps each `BTBHGPUSceneTerrain` alive for the duration of the present call, like the batches.
`BTBH_GPU_FEATURE_*` already covers what is needed (2D array textures are core in SDL_GPU on Vulkan and
Metal); no new capability flag.

## Minimal example

```c
const char *albedo[] = { "sand.png", "grit.png", "grass.png", "flagstones.png" };
BTBHGPUTexture *albedo_layers = btbh_gpu_window_texture_array_load_ex(
    window, albedo, 4, 1, BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT,
    BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB, &texture_report);
BTBHGPUTexture *control = btbh_gpu_window_texture_create_rgba8_ex(
    window, weights, 512, 512, 0, BTBH_GPU_TEXTURE_FILTER_LINEAR, BTBH_GPU_TEXTURE_WRAP_CLAMP,
    BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &texture_report);   /* 0.5 m per texel over 256 m */

BTBHGPUSceneTerrain terrain = {0};
terrain.albedo_layers = albedo_layers;
terrain.layer_count = 4;
terrain.control_textures[0] = control;
terrain.control_rect[2] = terrain.control_rect[3] = 256.0f;
for (uint32_t i = 0; i < 4; ++i) { terrain.layer_tiling[i][0] = terrain.layer_tiling[i][1] = 4.0f; }
terrain.layer_tiling[3][2] = 0.0f;    /* flagstones stay on the map's axes */
terrain.height_blend = 0.12f;

ground_batch.terrain = &terrain;      /* the ground mesh: one opaque batch instead of seven */
```

## Implementation

- **Shader.** No new monolith: `production_model.frag.hlsl` gains a `BTBH_TERRAIN` define (as it already has
  `BTBH_DIRECTIONAL_SHADOWS` and friends for the unshadowed variant) that replaces the material sampling with
  the terrain blend and leaves every lighting path (sun, spot, lamps, point lights, clouds, wetness, rain,
  SSAO/SSR/RC outputs, FSR 3 velocity) as is. Two new shader files: `production_model_terrain.frag` and
  `production_model_terrain_unshadowed.frag`. Layers bind as `Texture2DArray` at t0/t2/t3 in the terrain
  variant; the control textures take two further slots (t9, t10).
- **Blend.** Weights from the control textures (bilinear, world XZ); per layer UV from `layer_tiling`; height
  blend as `w_i * max(h_i + w_i - (max_j(h_j + w_j) - height_blend), 0)` normalised. Only layers with weight
  above 1/255 at the pixel are sampled (a branch per layer), so a pixel usually reads 1-3 layers.
- **Normals.** Tangent-space normals per layer blended with the same weights (whiteout), then through the
  mesh's tangent frame; a flat terrain mesh without tangents gets a world-aligned frame.
- **Uniforms.** `terrain_control_rect`, `layer_tiling[8]`, `terrain_controls` (count, height blend, base flag)
  in the scene fragment uniform block: 10 float4. Zero for ordinary batches.
- **Pipelines.** A terrain batch picks the terrain fragment shader; the vertex and all shadow passes are the
  ordinary ones (the terrain casts and receives shadows once, not seven times).
- **Cost.** Expected at 1600x900 on CHIMERA's map: fewer vertices (one copy instead of seven) and far less
  overdraw; the fragment cost per ground pixel rises (1-3 layer samples x 3 arrays). To be measured against
  `chimera-game --profile 900` before and after; the RFC is accepted only if the ground frame time does not
  rise.
- **Memory.** 7 layers x 1024^2 x 3 arrays with mips ≈ 112 MB at RGBA8; CHIMERA may halve normal/ORM.

## Compatibility

- **Versioning:** minor (additive, opt-in). New functions, a new struct, a pointer appended to
  `BTBHGPUSceneGeometryBatch` (zero = `NULL` = today), two new shader files.
- **Be The Bullet Hell:** nothing to change; the Swift layer zero-initialises batches, so `terrain` is
  `NULL`. Its look must not change: its smoke scene and reference images confirm it. The scene uniform block
  grows by 10 float4 (it is already ~2 KB with lamps and point lights); measured on BTBH's heaviest scene.
- **Tyrian3D:** nothing to change; stage the two new shader files only if it wants terrain.
- **CHIMERA:** regenerate the bindings (automatic, bindgen), add the two shaders to its fixed list in
  `crates/chimera-platform/src/gpu.rs`, build the arrays and control texture in `site.rs`, replace the six
  overlays with one batch.

## Platforms and testing

- Windows/Vulkan: CHIMERA's map, before/after captures and `--profile`.
- macOS/Metal: the native tests plus a new contract test that renders a 4-layer terrain into a readback
  (also run on Windows by `tools/check.ps1`).
- A native test: texture array creation (sizes, layer count, mismatched layers rejected), terrain batch
  validation (layer_count 1..8, control textures present for the counts used, finite tiling).

## Alternatives

- **Keep the overlays** and only add height blending to them: still seven passes and seven shadow casts.
- **Virtual texturing / baked ground texture:** sharp and cheap to sample but a large bake per map and no
  streaming of ground changes (creep, tide) without re-baking regions. Possible later for far LOD.
- **Vertex-colour weights instead of a control texture:** ties the transition sharpness to the 0.5 m mesh.
- **A separate terrain API outside the batch:** would duplicate instancing, shadows, RC and FSR plumbing.

## Open questions

- 8 layers enough? CHIMERA uses 7 today; a third control texture would allow 12 (its first wish was 8-12).
- Triplanar sampling for steep slopes (cliffs are separate meshes in CHIMERA, so probably not needed now).
- Should `layer_tiling.w` become a per-layer height bias or normal strength?
