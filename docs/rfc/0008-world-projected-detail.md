# RFC 0008: World-projected detail

- **Status:** Accepted on 2026-11-20 (phase 1 for 0.6.0; phase 2 later). See "Decisions" and "Phases"
- **Requested by:** Be The Bullet Hell v2 (its coordinator session, 2026-11-20). The spec is
  `Be-The-Bullet-Hell/v2/assets/kit/textures/macro/macro.json`, made by `v2/tools/kit/textures_macro.py`
- **Affects:** Be The Bullet Hell v2 (adopts it). Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D:
  nothing, it is opt-in. They rebuild the scene shaders with the C core, as for 0.5.0
- **Issue:** none. The games answered through their sessions: v2 accepted every point, CHIMERA has no objection
  (2026-11-20)
- **Depends on:** nothing. Phase 2 (compressed textures) doesn't depend on phase 1, and phase 1 doesn't depend on
  it

## Need

Be The Bullet Hell v2 ("v2" below) is a Rust rewrite of Be The Bullet Hell. It uses the renderer through bindgen
(`v2/crates/btbh-gpu-sys`, adapted from CHIMERA's), and `v2/renderer.pin` pins it to `../../btbh-renderer-v0.5.1`.
`CONSUMERS.md` lists it as the fourth consumer.

v2 draws its city from a building kit (`v2/crates/v2-render/src/kit.rs`, `scene.rs`):

- **One set per batch today.** Every kit surface class (`glb::SurfaceClass`: brick, concrete, glass, asphalt,
  paving, grass, roof, signs) is drawn as its own batch. Each batch binds one albedo, normal and ORM set through
  TEXCOORD_0 (`kit_surface`, `TextureSet`).
  - The kit bakes TEXCOORD_0 at one unit per cell, in the piece's frame. It is a box projection split at 18°, and
    the GLBs carry MikkTSpace tangents.
  - The fine sets repeat every half cell (`assets/kit/textures/textures.json`).
  - Pieces are instanced: one mesh, many placements, quarter turns (`KitCity::place_cell`).
- **The fine sets don't read from the classic camera.** At 43 to 64 pixels a cell, the fine sets sit at mip 4 to 5
  and their relief averages out. v2 made a coarse second set per class (`textures_macro.py`) for what still reads
  there:
  - panel joints a storey apart, rain streaks under the storey lines, repairs and sealed cracks;
  - ponding on flat roofs, weathered fields on tile roofs, mowing stripes on lawns.

  There are 8 classes of 3 maps each, 512², procedural.
- **They must tile in world space.** They repeat every 2 × 1.4 cells on walls (four storeys) and 4 × 4 cells on the
  ground. Pieces sit half a cell apart, so piece UVs can't make a pattern that runs on across pieces and
  buildings: it has to come from the world position. And neighbouring buildings shouldn't share their streaks,
  so each building needs its own offset.
- **The distance models could take it too.** v2 draws its distance models (`<file>_lod1.glb`) untextured today. A
  world projection needs no UVs, so they could take this set.

What v2 asked for, condensed:

1. An optional second texture set per batch (albedo, normal, ORM). Unset means no extra texture reads.
2. The world projection of `macro.json`, with separate tile sizes for level faces and walls. A split by face
   normal at 18°, not a blended triplanar.
3. A per-instance offset read from the instance's spare custom floats, filled per building.
4. The blend: albedo multiplied (×1.25), the normal's tilt added to the first set's, occlusion multiplied,
   roughness × g / 0.8.
5. Later, BC1/BC4/BC5 textures. The API takes RGBA8 only. v2's fine sets are about 29 MiB with mips, and the
   macro sets would double that.

Their estimate is 0.05 ms or less on a desktop GPU and 0.1–0.2 ms on the Steam Deck. They would keep it to
Medium and High if it costs more than 0.1 ms on the Deck. Nothing is blocked on it.

`macro.json`, as the renderer must read it:

| Field | What it says | For the renderer |
| --- | --- | --- |
| `projection` | Faces within 18° of level: u = x / tile_u, v = north / tile_v (north = −z in glTF), tangent +x, bitangent north. Other faces: T = normalize(cross(up, N)), u = dot(P, T) / tile_u, v = height / tile_v, bitangent up. Add a per-lot offset to u and v | Section 2. v grows toward the image's top (OpenGL), so the sampled t is −v. On faces that aren't level, v runs up the face: height on vertical walls, the distance up the slope on pitched faces (Decisions) |
| `normal` | Tangent space in the projection's frame, linear, OpenGL +Y; add its tilt to the fine normal's | xy read, z rebuilt, as the first set's |
| `orm` | Linear. R occlusion (multiply); G a roughness factor stored × 0.8 (0.8 keeps it); B 0 | `roughness_scale` = 1.25. B will carry the albedo (Decisions) |
| `albedo` | An sRGB grey multiplier with a linear mean of 0.8; scale by 1.25 and multiply | `albedo_scale` = 1.25. A UNORM texture can't hold a multiplier above 1. It moves into the ORM's B, linear (Decisions) |
| `tile_cells` | Walls (brick, tiles, concrete, glass) 2 × 1.4; ground (asphalt, paving, grass, roof) 4 × 4. One pair per class, for both projections | `level_tile` and `wall_tile`, in world units (v2: 1 unit = 1 cell) |
| `relief_cells` | The relief height the generator bakes into the normals (`textures.py` `normal_map`) | Not needed: the renderer takes a `normal_scale` |
| `materials` | Which kit materials take which class | v2's. The fine `brick` class holds the `tiles` material, which has its own macro class; v2 splits it into a ninth draw class (Decisions) |

## Proposal

Three parts:

1. **`BTBHGPUSceneWorldDetail`.** A plain struct the host fills and keeps (up to three textures, two tile sizes,
   three scales, the seed's source, a flag for the packed layout), and a pointer to it appended to
   `BTBHGPUSceneGeometryBatch`: `world_detail`. NULL draws as before.
2. **The scene fragment shader.** A uniform branch projects the flat world position by the face normal, samples
   the maps and blends them into the first set. That is two samples in the packed layout v2 uses (the grey albedo
   in the ORM's blue), three with a separate albedo. A batch without a world detail takes no extra sample.
3. **Phase 2: block-compressed textures.** BC1, BC4, BC5 and BC7 from data the host compressed offline, as raw
   blocks or a `.dds` file, with its mips. It is independent of 1 and 2.

### Where this differs from the request

v2 accepted every point below on 2026-11-20 (Decisions).

- **The offset is one seed, not two floats.** It is one custom float, a whole number (a lot's index), which the
  shader hashes into a 2D offset.
  - It leaves three of the four floats free.
  - A single free float exists whatever surface variant a building takes later: DISSOLVE reads x, y and z,
    SHATTER reads y, z and w, GLITTER reads x. A free pair doesn't (section 2, "The seed").
  - A host that wants exact offsets can't have them. v2 doesn't need them.
- **Pitched faces: v runs up the face.** `macro.json` says v = height and bitangent up on every face that isn't
  level.
  - On a sloped face "bitangent up" isn't in the face's plane, so the normal map can't use it. The shader uses
    B = cross(N, T), which is up on vertical walls and runs up the slope on a pitched face.
  - v = dot(P, B) follows it. On vertical walls that is the height, as `macro.json` says. On a pitched face it is
    the distance up the slope, so the image isn't stretched.
  - v = height would have stretched it by 1 / sin(pitch), twice at 30°, which v2 says would show.
  - The cost is a seam where a pitched face meets a wall. In v2 the eaves already are a seam between two macro
    classes (roof tiles and walls).
- **The image's top.** `macro.json`'s v and its normals' +Y both grow toward the image's top. The renderer
  samples with row 0 at the top, so the texture coordinate is (u, −v).
  - Walls show the image upright: the rain streaks, which the generator draws fading down the rows, run down.
  - Level faces show the image's top to the north.
  - Sampled at +v, every wall would be upside down and every normal's green would point the wrong way.
- **Scales are fields.** The ×1.25 on the albedo and the / 0.8 on the roughness are `albedo_scale` and
  `roughness_scale`, not constants in the shader.
- **Normals need nothing for BC5.** The request assumes BC5 normals need a flag or a swizzle. They don't:
  `production_model.frag` already reads only the normal map's xy and rebuilds z
  (`mapped_normal.z = sqrt(1 - dot(xy, xy))`, after `normal_scale`). A one-channel albedo (BC4) does need one
  (phase 2).
- **A packed layout.** v2's albedo is grey and its ORM's blue is unused, so both fit in one texture: two samples
  instead of three, and a third less cost.
  - `BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE` ships in phase 1, and v2's baker writes its sets that way.
  - A separate `albedo_texture` stays for a host whose detail albedo has colour.

### 1. API (phase 1)

```c
/* A second texture set for a batch, projected from the batch's flat world position (before world_curvature
 * bends it) instead of read through the mesh's TEXCOORD_0 (RFC 0008, since 0.6.0). It multiplies the albedo,
 * adds its normal's tilt to the first set's, and scales the occlusion and roughness. The host owns it; the
 * renderer reads it during the present call only. A batch with world_detail NULL, or one whose three textures
 * are NULL, draws exactly as before.
 *
 * The projection, by the face's own normal N (from the position's screen derivatives, on the side of the vertex
 * normal):
 * - Level faces (N within level_degrees of straight up or down): u = x / level_tile[0], v = -z / level_tile[1]
 *   (north), seen from above; a face that looks down takes v = +z (it is seen from below). Tangent +x.
 * - Other faces: T = normalize(cross(up, N)) (to the right seen from the front), B = cross(N, T) (up the face),
 *   u = dot(P, T) / wall_tile[0], v = dot(P, B) / wall_tile[1]: the height on vertical walls, the distance up
 *   the slope on pitched faces (not stretched).
 * - The frame is (T, cross(N, T), N). The image's top lies toward +v (as an OpenGL normal map's +Y): upright on
 *   walls, up the slope on pitched faces, to the north on level faces. The texture coordinate is (u, -v) plus the
 *   seed's offset.
 * Wrap the textures with BTBH_GPU_TEXTURE_WRAP_REPEAT. */
typedef struct BTBHGPUSceneWorldDetail {
    /* Its rgb times albedo_scale multiplies the albedo; alpha unused. NULL: no albedo change. */
    BTBHGPUTexture *albedo_texture;
    /* Tangent space in the frame above, OpenGL +Y, linear: xy are read and z is rebuilt (two channels are
     * enough). Its tilt times normal_scale is added to the first set's. NULL: no tilt. */
    BTBHGPUTexture *normal_texture;
    /* Linear. r multiplies the occlusion; g times roughness_scale multiplies the roughness; b unused, or the
     * grey albedo multiplier with BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE. NULL: neither changes. */
    BTBHGPUTexture *orm_texture;
    /* World units one repeat of the textures covers, along u and v; each must be above 0. */
    float level_tile[2];
    float wall_tile[2];
    float albedo_scale;        /* 0 means 1. v2: 1.25 (a linear mean of 0.8 to 1) */
    float normal_scale;        /* 0 means 1 */
    float roughness_scale;     /* 0 means 1. v2: 1.25 (its g of 0.8 keeps the roughness) */
    float level_degrees;       /* 0 means 18; below 90 */
    /* Which of the instance's four custom floats seeds an offset of the pattern: 0 none (every instance shares
     * it), 1..4 x..w. The seed is rounded to a whole number (exact up to 2^24); different whole numbers give
     * unrelated offsets in [0, 1) of a repeat along u and v. Give every instance of a building the same seed
     * (its lot's index). */
    uint8_t seed_component;
    uint8_t flags;             /* BTBH_GPU_SCENE_WORLD_DETAIL_* bits; 0 */
    uint8_t reserved[2];       /* 0 */
} BTBHGPUSceneWorldDetail;

enum {
    /* The grey albedo multiplier is the ORM texture's blue channel, linear like the rest of it (times
     * albedo_scale, as albedo_texture's rgb would be): the set is two textures and two samples. albedo_texture
     * must be NULL and orm_texture set. Be The Bullet Hell v2's layout. */
    BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE = 1u << 0
};

/* In BTBHGPUSceneGeometryBatch, appended after dug_by: */
    /* NULL: drawn as before. Otherwise the batch's second texture set (BTBHGPUSceneWorldDetail, since 0.6.0).
     * Several batches may point at one. */
    const BTBHGPUSceneWorldDetail *world_detail;

/* Defined when BTBHGPUSceneGeometryBatch.world_detail exists (since 0.6.0). */
#define BTBH_GPU_SCENE_HAS_WORLD_DETAIL 1
```

**Zero values.**
- **The batch:** `world_detail` NULL draws as before. So does a detail whose three textures are all NULL.
- **The detail:** inside it, 0 means the default for every scale, and for the level angle. A host can zero it,
  set the textures and the tiles, and get a plain multiply. The tiles have no default: a world unit means
  something different in each game.

**Refusals.** These fail the present with `BTBH_GPU_INVALID_ARGUMENT` and the batch's index, like the other batch
checks:
- a tile that isn't finite and above 0;
- `level_degrees` outside [0, 90);
- `seed_component` above 4;
- an unknown `flags` bit, or `reserved` not 0;
- `ALBEDO_IN_ORM_BLUE` with an albedo texture, or without an ORM texture.

`REFRACT` batches ignore it, because they sample no material.

**Lifetime.** The textures are the host's and must outlive the frame, as every batch texture must. The struct
itself is read during the call only, so a host can keep one per class (v2: eight) and point any number of batches
at it.

### 2. The projection, settled

- **Flat world space.** The vertex shader passes the world position before `btbh_world_bend`. The bend is around
  a centre that moves with the camera (`world_curvature.yz`), so a projection in bent space would slide as the
  camera moves. The detail uses the flat position, as lighting and the dig mask (RFC 0007) do. v2's TopDown camera
  bends by 1/160.
- **The face normal decides.**
  - The split and the frame use the face's own normal, `cross(ddx(P), ddy(P))`, not the interpolated vertex
    normal. It is constant over a triangle, so a smooth-shaded face never switches projection halfway across.
    The procedural detail does the same, for the same reason: some imported meshes carry vertex normals almost
    in the surface plane.
  - The face normal is turned to the side of the vertex normal. Then the tilt is added in the same frame as the
    first set's, and a back face flips it with the rest of the normal.
  - Where that dot product is near zero, the face normal is turned toward the camera instead.
- **No seams in the mip choice.** All four pixels of a 2×2 quad belong to one triangle (helper pixels extrapolate
  it), so they take the same projection and the texture coordinate's derivatives are right. The split costs no
  seam lines, which a per-pixel triplanar choice would.
- **Level faces** (|N.y| ≥ cos `level_degrees`).
  - They are projected straight down: u = x, v = −z (north). Two nearly level faces at different slopes still
    share one pattern across their edge: the ground runs on across tiles and pieces.
  - The price is a stretch along a level face's slope of at most 1 / cos 18°, 5 %.
  - A face that looks down (an awning's underside, a bridge's soffit) takes v = +z. Its frame stays right-handed
    (T × B = N), so its relief isn't inverted. The camera rarely sees these.
  - The frame is +x made orthogonal to N, and B = cross(N, T): north on a flat roof.
- **Walls and pitched faces** (everything else).
  - T = normalize(N.z, 0, −N.x) = normalize(cross(up, N)). Its length is at least sin 18° (0.31), so it never
    degenerates. B = cross(N, T), a unit vector in the face.
  - u = dot(P, T) and v = dot(P, B).
  - On a vertical wall B is up and v is the height, as `macro.json` says. On v2's 45° facades T runs along the
    facade: a blended triplanar would show two copies there, the split shows one.
  - On a pitched face (between 18° and vertical), B runs up the slope and v is the distance along it. The image
    keeps its proportions: a tile of 2 × 1.4 world units on a 30° roof covers 1.4 units up the slope, not 2.8.
  - Where a pitched face meets a vertical wall, both keep u (same T when they face the same way), but v jumps:
    the wall's v is its height, the roof's is its distance up the slope. In v2 the eaves already are a class
    seam (roof tiles against walls).
- **Image orientation.** The texture coordinate is (u + offset.x, −v + offset.y). The image's top lies toward +v,
  as in `macro.json`, and T is the image's right.
  - A wall seen from the front shows the image upright and unmirrored.
  - A pitched face shows it unmirrored with its top up the slope: the generator's streaks on tile roofs run
    down the slope, as its comment intends ("v runs up a pitched face").
  - A flat roof seen from above, north up on the screen, shows it unmirrored with its top to the north.
  - The green of a normal texel then tilts toward B, as an OpenGL +Y map's should.
- **The seed.**
  - `round(custom[seed_component − 1])` goes through a PCG hash. Its two 16-bit halves are the offset in repeats,
    in [0, 1), added to u and v on every face of the instance.
  - Rounding first makes it exact. The instance's custom floats reach the fragment shader interpolated
    (`instance_custom`, TEXCOORD5, not `nointerpolation`), and a GPU that interpolates with perspective may
    round a constant by an ulp. A raw-bits hash would then change from pixel to pixel; a whole number survives.
    No existing output changes.
- **Which custom floats are spare.** The record is 24 floats: a 4×4 model matrix, a colour, and four custom
  floats (`InstanceData` in `production_model.vert.hlsl`). The renderer reads the custom floats only for three
  surface variants:

  | Variant | Reads | Free |
  | --- | --- | --- |
  | `DISSOLVE` (3) | x time, y seed, z duration | w |
  | `SHATTER` (4) | y time, z seed, w scale | x |
  | `GLITTER` (5) | x level | y, z, w |
  | any other | nothing | all four |

  What the games put there:
  - **v2:** zero on every kit instance (`instance_trs`). It uses no variant that reads them (RIM, WATER,
    REFRACT), so all four are free. It will put the lot's index in w (Decisions).
  - **Be The Bullet Hell v1:** fills all four on city buildings (x the destructable level, y the shatter time, z
    a seed, w the shatter's scale; `CityRenderer.swift`). Its dissolving enemy ghosts use x, y and z.
  - **CHIMERA:** writes none in production (`batch.rs` sets only the colour); a dissolve test sets x.

  The offset never collides with a variant: the host picks the component.

### 3. The blend

In `production_model.frag`, inside one branch on a uniform (the batch has a world detail):

```hlsl
// After the first set's four samples and before apply_surface, so derivatives are valid (SHATTER's
// apply_surface can discard) and a surface variant that replaces the albedo, normal or ORM (water) wins.
// Packed (v2): two samples, normal and ORM; the albedo is detail_orm.b. Otherwise a third sample's rgb.
albedo_sample.rgb *= (packed ? detail_orm.bbb : detail_albedo.rgb) * albedo_scale;
orm.r *= detail_orm.r;                                         // occlusion
orm.g *= detail_orm.g * roughness_scale;                       // before saturate, wetness and material_factors
float3 m = detail_normal.xyz * 2 - 1;  m.xy *= normal_scale;  m.z = sqrt(1 - saturate(dot(m.xy, m.xy)));
world_gradient = -(m.x * T + m.y * B) / max(m.z, 1e-3);        // a surface gradient, as the procedural detail's
...
normal = stable_normalize(normal - detail_gradient - world_gradient, normal);
```

- **The tilt.** The two sets' tilts add as surface gradients (Mikkelsen's framework, which the procedural detail
  and the rain already use). For a flat first set this is exactly tangent-space normal mapping in the projection's
  frame. Otherwise the two tilts add, as `macro.json` asks.
- **Albedo, occlusion, roughness.** These are multiplied in before the procedural detail's tints, its wetness and
  `material_factors`, so they act on the first set's values as those do. Metalness and emission are untouched.
- **Every sample** (two or three) uses `SampleBias(..., procedural_controls.w)`, the bias the first set's samples use: FSR 3's
  negative bias, 0 otherwise. 0.5.0 fixed the relief-from-brightness taps for the same reason. Without it, under
  FSR 3 the detail would read the render size's mip and look softer than the first set.

### 4. Phase 2: compressed textures

```c
#define BTBH_GPU_FEATURE_TEXTURE_BC (UINT64_C(1) << 9u)   /* reported in capabilities.features; never required */

typedef enum BTBHGPUCompressedFormat {
    BTBH_GPU_COMPRESSED_FORMAT_INVALID = 0,
    BTBH_GPU_COMPRESSED_FORMAT_BC1 = 1,   /* RGB (1-bit alpha), 8 bytes a 4x4 block; sRGB or linear */
    BTBH_GPU_COMPRESSED_FORMAT_BC4 = 2,   /* one channel, 8 bytes a block; linear only. In a colour slot it is
                                           * read as (r, r, r, 1) */
    BTBH_GPU_COMPRESSED_FORMAT_BC5 = 3,   /* two channels, 16 bytes a block; linear only. Normal maps (z is
                                           * rebuilt), two-channel ORM maps (b reads 0: no metalness) */
    BTBH_GPU_COMPRESSED_FORMAT_BC7 = 4    /* RGBA, 16 bytes a block; sRGB or linear */
} BTBHGPUCompressedFormat;

/* Blocks the host compressed offline, with every mip level it wants (none are generated): the levels back to
 * back, largest first, each ceil(w / 4) x ceil(h / 4) blocks, rows of blocks top first. byte_count must be
 * their total. SRGB only with BC1 and BC7. BTBH_GPU_MISSING_CAPABILITY when the device lacks BC
 * (BTBH_GPU_FEATURE_TEXTURE_BC). */
BTBHGPUTexture *btbh_gpu_window_texture_create_compressed(
    BTBHGPUWindow *window,
    BTBHGPUCompressedFormat format,
    BTBHGPUTextureColorSpace color_space,
    uint32_t width,
    uint32_t height,
    uint32_t mip_count,
    const void *blocks,
    uint64_t byte_count,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
);
/* A .dds file and its mips: a DX10 header with DXGI BC1, BC4, BC5 or BC7 (_UNORM or _UNORM_SRGB), or a legacy
 * header with DXT1, ATI1/BC4U or ATI2/BC5U (linear). */
BTBHGPUTexture *btbh_gpu_window_texture_load_dds(
    BTBHGPUWindow *window,
    const char *path,
    BTBHGPUTextureFilter filter,
    BTBHGPUTextureWrap wrap,
    BTBHGPUTextureReport *report
);

/* Defined when the two functions above exist. */
#define BTBH_GPU_HAS_COMPRESSED_TEXTURES 1
```

- **Mips come from the host.** Compressing at load is not proposed. It would need an encoder in the core, cost
  seconds of load time for BC7, and give worse blocks than an offline tool. `SDL_GenerateMipmapsForGPUTexture`
  can't fill a BC texture either: it renders into each level, and a BC texture can't be a render target (SDL's
  Metal backend refuses `COLOR_TARGET` for every BC format).
- **Raw blocks are the base.** Any host with its own container can use them: Rust has the `ddsfile` and `ktx2`
  crates. DDS is the one container the core reads. It is a fixed header with mips back to back, and every BC
  tool writes it: `texconv`, `compressonatorcli`, `nvcompress`, and the `intel_tex_2` crate behind a few lines of
  Rust. KTX2 is left out: Basis and Zstd supercompression would need decoders, and plain KTX2 adds nothing over
  DDS here.
- **sRGB.** BC1, BC2, BC3 and BC7 have sRGB variants in SDL, Vulkan and Metal. BC4 and BC5 don't: they hold data.
  A grey albedo in BC4 is stored linear. For a multiplier between about 0.6 and 1.0 that is no loss: linear 8-bit
  steps there are about half the size of sRGB's.
- **Which formats the shaders read.**
  - Normal maps: BC5 (or BC1, or BC7) needs no change, since only xy are read.
  - ORM: an ORM in BC5 has no metalness (b reads 0), so a metal ORM needs BC1 or BC7.
  - A one-channel texture (BC4, or an R8 texture) in a colour slot reads (r, 0, 0, 1), which shows red. SDL's GPU
    API has no texture swizzle (`SDL_GPUTextureCreateInfo` has none). So the scene sets a bit per colour slot
    (albedo, emission, world-detail albedo) in the batch's second uniform block, from the bound texture's format,
    and the shader reads `.rrr` and alpha 1.
  - The model, unlit and radiance-cascade material-source fragment shaders read that bit. A one-channel texture
    in a normal or ORM slot fails the present.
  - The canvas already sends an R8 texture through `production_canvas_coverage.frag`; BC4 joins that test.
- **Device support.**
  - Desktop Vulkan on Windows and Linux, and RADV on the Steam Deck, sample BC on every GPU the games target.
  - Metal: SDL's backend allows BC where `supportsBCTextureCompression` is true (macOS 11 and later). That
    includes Apple Silicon and Intel Macs. Be The Bullet Hell needs macOS 12.
  - SDL 3.4.2 (`external/SDL`) doesn't ask for Vulkan's `textureCompressionBC` feature when it creates the
    device. It enables that only through `SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER`, which also resets
    SDL's own Vulkan 1.1–1.3 feature requests.
  - `SDL_GPUTextureSupportsFormat` asks the physical device, so it can say yes on a logical device that lacks
    the feature. Phase 2 starts by settling this with the validation layers on. If sampling a BC image needs the
    feature, the device is created with it where supported, and that path gets its own test.
  - `BTBH_GPU_FEATURE_TEXTURE_BC` is set only when both the format query and the feature agree.
- **Memory.** A 512² map with its mips is 1.33 MiB in RGBA8, 0.33 MiB in BC5 or BC7 and 0.17 MiB in BC1 or BC4.
  - v2's fine sets (21 maps) go from 28 MiB to about 6 MiB.
  - v2's packed macro sets (16 maps) go from 21 MiB to about 5 MiB: the normal in BC5, the ORM with its albedo in
    BC7 (linear). BC1's 5:6:5 endpoints are too coarse for three unrelated channels.

## Minimal example

```c
/* v2's concrete class (macro.json: tile_cells 2 x 1.4, the same for both projections), packed: the ORM's blue
 * holds the grey albedo multiplier, linear. */
BTBHGPUTextureReport tr;
BTBHGPUSceneWorldDetail concrete = {0};
concrete.normal_texture = btbh_gpu_window_texture_load_ex(window, "macro/concrete_normal.png", 1,
    BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &tr);
concrete.orm_texture = btbh_gpu_window_texture_load_ex(window, "macro/concrete_orm.png", 1,
    BTBH_GPU_TEXTURE_FILTER_TRILINEAR, BTBH_GPU_TEXTURE_WRAP_REPEAT, BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR, &tr);
concrete.flags = BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE;
concrete.level_tile[0] = concrete.wall_tile[0] = 2.0f;
concrete.level_tile[1] = concrete.wall_tile[1] = 1.4f;
concrete.albedo_scale = 1.25f;
concrete.roughness_scale = 1.25f;
concrete.seed_component = 4;                    /* custom.w */

/* Every instance of a building's pieces carries its lot's index in custom.w (float 23 of the record). */
instance[23] = (float)lot_index;

/* Each batch of the concrete class, as today, plus one pointer. */
batch.world_detail = &concrete;                 /* NULL on everything else */
```

In v2's Rust this is `world_detail: set.map_or(std::ptr::null(), |d| d as *const _)` in `batch_of`.
- The eight descriptors, one per macro class, live in `KitLibrary` next to `textures`.
- The nine fine draw classes point at them: `brick` and the split-off `tiles` share the fine brick set and take
  the brick and tiles details.
- `KitPiece` gains the lot's index, from v2-city's `KitInstance`, and `place_cell` writes it into float 23.

## Implementation

### Phase 1

- **Header.** The struct, the flag, the appended pointer and `BTBH_GPU_SCENE_HAS_WORLD_DETAIL`. The version
  becomes 0.6.0.
- **`src/gpu/btbh_gpu_scene.c`.**
  - Validation, with the refusals above. Many batches point at one detail, so it is checked once per distinct
    pointer a frame.
  - The uniforms: section "Bindings" below.
  - Three more sampler bindings after the dig mask: the detail's textures, or the existing fallbacks for the
    ones it lacks (never sampled then). A packed detail binds the fallback in the albedo slot.
  - Every model fragment shader is created with three more samplers: 14, 13 and 9 instead of 11, 10 and 6.
  - `btbh_gpu_scene_uses_fast_unlit_pipeline` returns false for a batch with a world detail, so
    `production_model_unlit.frag` stays as it is.
  - A present whose batches have a world detail checks that the scene's SPIR-V fragment shader names
    `world_detail_normal_texture`, as 0.5.0 checks for `dig_mask_texture`. A shader from before 0.6.0 would bind
    and draw without a word, ignoring the set. Metal libraries aren't checked.
- **Shaders.**

  | Shader | Changes? | Why |
  | --- | --- | --- |
  | `production_model.frag` (and the five files that include it: `_2d_shadows`, `_unshadowed`, `_rc`, `_rc_2d_shadows`, `_rc_unshadowed`) | Yes | The bindings, the uniforms and the branch. The radiance-cascade variants write the detailed albedo and normal into their targets with no extra work |
  | New `production_world_detail.hlsli` | New, included | The projection, the frame and the seed's hash, kept apart so a C copy of the same math can be tested natively |
  | `production_model_unlit.frag` | No | A batch with a world detail never takes the fast unlit pipeline |
  | `production_rc_material_source.frag` | No | Lit surfaces inject only their emission. Unlit ones inject their albedo, and the detail's multiplier averages 1 (v2's: 0.8 × 1.25), so the injected light is right on average |
  | `production_shadow*`, `production_animated_shadow.vert` | No | The detail changes neither coverage nor depth |
  | `production_model.vert`, `production_animated_model.vert` | No | The fragment already receives `world_position` (flat) and `instance_custom` |

  No shader file is new, so CHIMERA's and Tyrian3D's shader lists (read from `CMakeLists.txt`) and v2's 14 named
  shaders stay as they are.
- **Bindings.**
  - SDL's GPU API allows 16 samplers and 4 uniform buffers per stage (`MAX_TEXTURE_SAMPLERS_PER_STAGE`,
    `MAX_UNIFORM_BUFFERS_PER_STAGE` in `external/SDL/src/gpu/SDL_sysgpu.h`).
  - 0.5.0 put the dig mask after each variant's own textures (t10, t9 or t5) and its placement in a second
    fragment uniform buffer (b1, space3), pushed for every batch.
  - The detail's textures follow the mask:

  | Variant | Today | With the detail | Free |
  | --- | --- | --- | --- |
  | Directional and omni shadows | t0–t9, mask t10 | t11–t13 | 2 |
  | Directional shadows only (`_2d_shadows`) | t0–t8, mask t9 | t10–t12 | 3 |
  | Unshadowed | t0–t4, mask t5 | t6–t8 | 7 |

  - RFC 0001's two control textures, if it goes ahead in the same shader, would take the full variant to 16:
    the limit. The albedo slot stays declared even though v2 packs its albedo, for hosts with a coloured one. If
    the slots run out, it is the one to drop.
  - The uniforms join b1 rather than a third buffer: one push per batch, as now, of 80 bytes instead of 32.

  ```hlsl
  cbuffer BatchExtension : register(b1, space3)   // was DigMask; same slot
  {
      float4 dig_mask_origin;
      float4 dig_mask_scale;
      float4 world_detail_tiles;      // 1 / level_tile (u, v), 1 / wall_tile (u, v)
      float4 world_detail_controls;   // albedo scale, normal scale, roughness scale, cos(level angle)
      uint4  world_detail_mode;       // x: maps (1 albedo, 2 normal, 4 ORM, 8 albedo in ORM blue; 0 none);
                                      // y: seed component (0 none, 1..4); zw: 0 (phase 2: one-channel slots)
  };
  ```

  The unlit, radiance-cascade source and shadow shaders keep their 32-byte block. A longer push than a shader
  declares is harmless: it reads only what it declares.
- **Keeping "unset" free: a uniform branch, not pipeline variants.**
  - `world_detail_mode.x == 0` skips the whole path. The value is the same for every pixel of a draw, so no
    texture is fetched and no wave diverges.
  - Variants would double every model pipeline array (`[color targets][blend][depth][transparent][cull][stencil]
    [fast unlit]`, times the shadow and skinning sets) and add five shader files to every game's list.
  - 0.5.0 chose the branch for the dig mask for the same reasons.
  - The branch has one cost that must be measured, not assumed. The compiler reserves registers for the longest
    path, so the detail can lower occupancy for every pixel of every batch, set or not. On the Deck's RDNA 2 GPU
    that is the risk (Cost). If it shows, the fallback is a `BTBH_WORLD_DETAIL` define and variants for the full
    shadow set only.
- **Skinned, animated, billboards, vertex-animated surfaces.** The animated vertex shader feeds the same fragment
  shader, so the detail works on skinned batches. On anything that moves it slides, though: skinned units,
  billboards, wind, a dissolve's drift, a shatter's collapse. It is meant for static geometry. An object-space
  mode could follow if a game asks (Alternatives).
- **SPIR-V and Metal.** The shaders go through shadercross as today. The branch, derivatives in uniform control
  flow, and `SampleBias` are plain HLSL, SPIR-V and MSL. The Metal path isn't verified, as for 0.5.0.

### Cost

These are estimates, to measure in the prototype. v2's packed layout (two samples) is the main path. The
three-sample layout (a separate albedo texture) is in parentheses.

| Item | Estimate |
| --- | --- |
| Per pixel, with a detail | 2 trilinear samples (3) and about 40 ALU instructions (derivatives, the frame, the hash, the gradient) |
| Per pixel, without | One uniform compare, plus whatever the longer path costs in registers (to measure) |
| Desktop GPU (RTX 3070 Ti, the bench machine), 1080p | 0.02–0.04 ms (0.03–0.05 ms) |
| Same, 1440p | 0.03–0.06 ms (0.05–0.09 ms) |
| Steam Deck preset (960 × 600) on a Deck | 0.04–0.12 ms (0.06–0.19 ms) |
| CPU per batch | One pointer, 48 more bytes pushed. SDL's Vulkan backend rewrites a fragment descriptor set only when a binding changes, so the new samplers cost three more writes on those draws only |
| GPU memory, RGBA8 with mips | 1.33 MiB a map: v2's 16 packed maps 21 MiB (24 maps: 32 MiB) |
| GPU memory, phase 2 | about 5 MiB for v2's 16 packed maps (BC5 normal, BC7 ORM with albedo); about 7 MiB for 24 maps (BC5 normal, BC5 ORM, BC4 albedo) |

**How the GPU rows were reached.**
- The Deck has 8 RDNA 2 compute units at 1.0–1.6 GHz, each with four texture filter units. Trilinear runs at half
  rate, so the Deck filters 16–26 billion trilinear samples a second.
- At 960 × 600, 0.5–1 million kit fragments a frame (screen share times overdraw, guessed) take 1–2 million
  samples packed: 0.04–0.12 ms. Three maps take 1.5–3 million: 0.06–0.19 ms.
- The RTX 3070 Ti filters about 170 billion trilinear samples a second (192 units at 1.77 GHz). At 1080p it shades
  1.5–3 million kit fragments: 3–6 million samples packed, 0.02–0.04 ms.
- These are texture-bound costs. The real cost is lower when the shader's heavy ALU (soft shadows, 40 lamps, 32
  point lights) hides the fetches. It is higher if the longer path lowers occupancy.
- Packed, the Deck's upper estimate lands just above v2's 0.1 ms line and its lower one well under it. The
  measurement decides whether the detail stays on Low and the Deck.

**What the Deck number hinges on**, in order:
1. **Occupancy.** The register count of `production_model.frag` on gfx1033 (the Deck's GPU) before and after,
   with the detail unset. Radeon GPU Analyzer reads it offline from the SPIR-V, without a Deck.
2. **The kit's screen share and overdraw** at the classic camera, and whether its opaque batches are drawn front
   to back (`opaque_sort`).
3. **What the detail replaces.**
   - On High, v2 still runs the procedural facade grain on textured kit surfaces, for its tint
     (`SurfaceLook::detailed`, pattern 3). That is a triplanar fbm: about 40 hashes a pixel, against two
     fetches here (packed).
   - v2's Low and Deck looks already drop the grain as "the dear part".
   - So the detail in place of the grain may cost nothing, or save time. v2 should measure that pair, not the
     detail on top of everything.
4. **Phase 2's formats**, later. BC lowers bandwidth and cache misses, not the filter rate. At mips 1 to 3, where
   the classic camera reads these maps, the textures mostly stay in cache anyway.

A real Deck number needs a real Deck. v2's Deck figures so far are its Deck preset measured on the desktop GPU,
and that scales by neither the Deck's texture rate nor its occupancy. The real numbers will come from the owner's
Deck test (Decisions). Until then, the rule "Medium and High only above 0.1 ms on the Deck" can only be applied
to the estimate above and the analyser's register count.

**How to measure it.**
- **Bench.** SDL 3.4 has no GPU timestamps. The bench therefore times 20 frames back to back, as in
  `BTBH_SMOKE_BENCH` (CHANGELOG 0.5.0, "Measured"), and makes the detail's share large: eight coplanar
  screen-filling layers drawn with `LESS_EQUAL` so every layer shades every pixel, with the heavy pixel of the
  opaque-sort bench.
  - Four configurations: no detail, packed (v2's), three maps, and no detail against 0.5.1's shaders (the
    branch's own cost).
  - At 1920 × 1080 and 960 × 600, in interleaved turns, quoting the fastest turn.
  - The difference over eight is the cost per screen of detailed surface.
- **Registers.** Radeon GPU Analyzer on `production_model.frag.spv` (all five variants) for gfx1033 and gfx1030:
  VGPRs and waves per SIMD, before and after.
- **v2.** An A/B of its kit with and without the detail, and with the detail in place of the grain, from its frame
  telemetry. Then the same on the owner's Deck.

### Phase 2

- **2a.** `btbh_gpu_window_texture_create_compressed` and `btbh_gpu_window_texture_load_dds` in
  `src/gpu/btbh_gpu_texture.c`, `BTBH_GPU_FEATURE_TEXTURE_BC` in `btbh_gpu_device.c`, and the Vulkan feature
  question settled.
  - Upload: one `SDL_UploadToGPUTexture` per level, sizes in texels; SDL works out the block rows from the
    format.
  - The texture has `SAMPLER` usage only.
  - No shader changes: BC1, BC5 and BC7 work in every slot that already reads RGBA8.
- **2b.** The one-channel bit for BC4 (and R8) in colour slots: the model, unlit and radiance-cascade source
  shaders, plus the canvas's coverage test.
- **Model loading.** It is unchanged: imported glTF textures stay RGBA8. A loader for `MSFT_texture_dds` or KTX2
  in glTF is a later request.

## Compatibility

- **Versioning:** minor (0.6.0): additive and opt-in.
  - The batch grows by one pointer, from 264 to 272 bytes on 64-bit, and `world_detail` sits at offset 264. Every
    game compiles the C core from the checkout whose header it builds against, so they agree on the size, as for
    0.5.0's `dug_by`.
  - The scene shaders change bindings and must be rebuilt with the C core.
    - Old shaders with the new core still draw: the three extra bindings go unused. Only a batch with a world
      detail is refused, by the name check.
    - New shaders with an old core don't work: the pipelines would be created with too few samplers.
  - Phase 2 is additive too: two functions, an enum, a feature bit. 2b changes three shaders.
  - **Byte-identical when unset** is tested, not assumed: the shader is rebuilt, and a compiler may schedule the
    unset path differently. 0.5.0 showed the way: CHIMERA found 0 pixels different from 0.4.0 with nothing set.
- **Be The Bullet Hell v2 (Rust, bindgen):**
  - Nothing changes until it opts in. bindgen generates `BTBHGPUSceneWorldDetail` with `Default`, and `batch_of`
    ends in `..Default::default()`, so `world_detail` is null.
  - To adopt it:
    - bump `renderer.pin` to the 0.6.0 checkout;
    - rebake the macro sets packed (the albedo, linear, in the ORM's blue) and update `macro.json`'s projection
      text (v up the face);
    - split `tiles` out of the fine `brick` class into a ninth draw class;
    - keep eight descriptors in `KitLibrary`, flagged `ALBEDO_IN_ORM_BLUE`;
    - set `world_detail` on the kit's class batches, lod1 included if it wants;
    - write each building's lot index into float 23 (custom w) of its pieces' instances.
  - One choice is still its own: whether the detail replaces the facade grain.
  - A side note from reading its loader. `kit.rs` `load_tile` scales the fine albedo by 1.25 in linear light and
    clamps at 1 (`(lin * gain).min(1.0)`). Every texel brighter than the set's mean is clipped, so the fine sets
    lose their bright half (brick's 95th percentile is 1.068 of the mean).
    - The detail avoids that with `albedo_scale`.
    - For the fine sets, v2 can load the albedo unscaled and put the 1.25 in the instance colour, which is a
      float and may exceed 1.
    - Or it can ask for a first-set scale. That is not part of this RFC.
- **Be The Bullet Hell (v1, Swift):**
  - Nothing changes. The Swift layer builds batches with `BTBHGPUSceneGeometryBatch()`, which zeroes the new
    pointer.
  - Its city buildings use all four custom floats (shatter and level), so a v1 adoption would need a free float,
    or SHATTER's spare x when the building isn't shattering.
  - A Swift `MaterialDescriptor.worldDetail` can follow when it asks. The layer would keep the descriptors alive
    across the present call, as it keeps its native batch array.
- **CHIMERA (Rust, bindgen):** nothing changes. `default_batch()` ends in `..Default::default()`. Its build script
  can gate on `BTBH_GPU_SCENE_HAS_WORLD_DETAIL` as it gates on `BTBH_GPU_SCENE_HAS_OPAQUE_SORT`. It writes no
  custom floats in production.
- **Tyrian3D (C):** nothing changes. `init_batch` memsets the batch. It is pinned at v0.2.1.

## Platforms and testing

- **Native tests (no GPU, `tools/check.ps1` and `check.sh`):**
  - `btbh_gpu_scene_order_test`: `offsetof(world_detail) == 264` and `dug_by` still at 256. `sizeof
    (BTBHGPUSceneWorldDetail) == 64` on 64-bit.
  - A C copy of the projection, kept in step with `production_world_detail.hlsli`:
    - the up and down faces, four walls, a 45° wall, a 30° pitch, and faces at 17° and 19°;
    - T × B = N on each;
    - the image's top up on walls, up the slope on the pitch, and north on roofs;
    - on the pitch, a world unit up the slope moves v by 1 / `wall_tile[1]`, the same as a unit of height on a
      wall: no stretch;
    - the seed's offsets stable and spread.
  - The refusals: a zero tile, NaN, `seed_component` 5, an unknown flag, packed with an albedo texture or without
    an ORM texture.
- **GPU smoke** (`btbh_gpu_scene_target_smoke`, or a new `btbh_gpu_world_detail_smoke`; Windows/Vulkan):
  - **Unset:** the existing scenes render byte for byte the same with `world_detail` NULL, with a detail whose
    textures are all NULL, and against 0.5.1's references.
  - **Neutral:** a flat normal and an ORM of (1, 0.8, 0.8) packed, with both scales 1.25, render within 1/255 of
    no detail. So does the same with a separate albedo of 0.8.
  - **Packed against separate:** the same grey multiplier in the ORM's blue and in an albedo texture renders the
    same, within 1/255.
  - **Orientation, with a reference image** (`Tests/Fixtures/world_detail/orientation.png`). A test texture (an
    arrow and an "F") on a box with four walls, one 45° wall, a 30° pitched face and a flat roof, over ground.
    The readback also checks it numerically:
    - on every wall the arrow points up and the F reads unmirrored from outside;
    - on the pitched face the arrow points up the slope and is as long as on the walls (no stretch);
    - on the roof and the ground the arrow points north.
  - **Continuity:** two touching boxes with the same seed continue the pattern across their seam; with different
    seeds they don't.
  - **Curvature:** with `world_curvature` > 0, two camera positions put the same texels on the same face.
  - **FSR 3:** a high-frequency detail at a 67 % render scale picks the output size's mip, as
    `testFSR3PicksTextureDetailForTheOutputSize` does for the first set.
  - **Old shaders:** a batch with a world detail and 0.5.1's `production_model.frag` fails the present with the
    shader's path; without the detail, it draws.
  - **Skinned:** an animated batch with a detail draws.
- **`BTBH_SMOKE_BENCH=1`**: the bench above (prints only).
- **Phase 2 smoke:**
  - BC1, BC4, BC5 and BC7 textures (from a small checked-in DDS per format) match their RGBA8 decode within the
    format's error;
  - sRGB BC1 and BC7 match the sRGB RGBA8 load;
  - a BC5 normal map lights like its RGBA8 source;
  - a BC4 albedo reads grey, not red;
  - a device without BC returns `BTBH_GPU_MISSING_CAPABILITY`.

  These run with the Vulkan validation layers on.
- **Platforms:**
  - Windows/Vulkan is where the smokes run.
  - Linux and the Deck (RADV) through Be The Bullet Hell when it runs there.
  - macOS/Metal isn't verified, as for 0.5.0.
- **Games:**
  - CHIMERA: 0 pixels different with nothing set.
  - Be The Bullet Hell: its Swift suite.
  - v2: its kit captures with and without the detail, and its A/B timings.

## Alternatives

| Alternative | Why not (now) |
| --- | --- |
| **The host bakes a second UV set** (TEXCOORD_1) | Pieces are instanced with quarter turns and half-cell offsets, so a UV baked into a piece mesh can't continue across pieces or buildings. Only the instance transform knows where a piece is, which makes this the world projection done the hard way. The custom-mesh vertex (96 bytes) has no second UV either, so the vertex layout and shaders would change too |
| **A full blended triplanar** | Three samples per map (nine) where a split needs three. On v2's box kit the face normals are axis-aligned, so the blend weights are 0 or 1 anyway. On its 45° facades a blend shows two half-strength copies of the pattern, which the split doesn't. The procedural facade grain blends this way and pays for it |
| **A decal or a second pass** (each detailed surface drawn again, multiplied) | Every detailed batch drawn twice: double the vertex work and draws. The scene is forward-shaded, so a second pass can only change the final colour. It can't tilt the normal or change the roughness and occlusion that the lighting reads |
| **The procedural surface detail** (`detail_parameters`, `apply_surface_detail`) | It exists in C, but its patterns are fixed in the shader: grain and stains for ground, grass, facade and paving; glass panes; a lamp's lens. Each new look is new shader code, and it can't show authored features such as joints a storey apart or streaks under storey lines. The facade pattern is the expensive one (three triplanar fbm). The world detail is the data-driven counterpart, and it may replace the grain |
| **The set's fields inline in the batch** | About 60 bytes and 11 fields added to every batch and every literal, for a set that eight descriptors cover. Future fields (an object-space mode, anisotropy) would touch the batch again. The pointer follows RFC 0007's `dug_by` |
| **An opaque handle** (`btbh_gpu_window_world_detail_create` / `destroy`) | It would validate once, but adds create and destroy calls and a lifetime to manage. Checking a 64-byte struct once per distinct pointer a frame is cheap |
| **Pipeline variants instead of a branch** | Double the pipeline arrays and five more shader files in every game's list. Kept as the fallback if the unset path's register cost shows on the Deck |
| **A two-float offset** (as asked) | Two of the four custom floats, and no free pair exists under DISSOLVE or SHATTER. A seed of one whole number per building gives the same "neighbours differ" |
| **One texture array for all classes** (a layer per class) | A batch could then carry several classes (brick and tiles in one mesh). But v2 already draws per class and splits `tiles` into its own draw class. Arrays need every layer the same size and format, and RFC 0001's terrain arrays are a different binding. Later, if a game needs mixed classes in one draw |
| **v = height on pitched faces** (`macro.json` as first written) | It joins a wall at the eaves, but stretches the image along the slope by 1 / sin(pitch), twice at 30°. v2 chose v up the face: its eaves are a class seam anyway |
| **Object-space projection** | The detail would follow moving objects, but restart on every piece, which is the opposite of what v2 needs. Possible later as a mode bit |
| **Compressing at load** (phase 2) | An encoder in the core, slow loads (BC7 especially), worse quality than offline tools, and nothing gained over shipping DDS |
| **KTX2** (phase 2) | Its point is Basis or Zstd supercompression, which needs decoders. Plain KTX2 is DDS with more parsing. A host that wants it can use the raw-block call |

## Decisions (v2, 2026-11-20)

v2 accepted every point in "Where this differs from the request": one seed float, the (u, −v) coordinate,
cross(N, T) on slopes, BC5 normals with no flag, and phase 2 later. Its answers to the open questions:

1. **The seed.** One whole number per building, its lot's index, in whichever custom float `seed_component`
   names. v2 uses w and fills it per instance. It doesn't need exact offsets.
2. **Pitched faces: v runs up the face.** No stretch, a seam at the eaves.
   - In v2, pitched roofs are a different macro class (roof tiles) from the walls, so the eaves already are a seam.
   - A 2× stretch at 30° would show.
   - The projection above says so everywhere: on faces that aren't level, v = dot(P, cross(N, T)). That is the
     height on vertical walls, as before.
   - v2 updates `macro.json`'s projection text to match.
3. **Packed: two samples.** The grey albedo goes in the ORM's blue channel, linear.
   - `BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE` ships in phase 1, unconditionally.
   - v2's baker writes the sets that way once this RFC is accepted.
   - The three-texture layout stays for hosts whose detail albedo has colour.
4. **`tiles` becomes a ninth draw class.** v2 splits it out of the fine `brick` class, so pitched roofs get the
   tiles macro set and brick keeps its own. Nothing changes in the renderer: it is one more batch, with its own
   `world_detail`.
5. **BC waits.** It waits until v2's tools write DDS. Phase 1 is what v2 needs; phase 2 stays in this RFC, later
   and independent.
6. **Deck numbers.** Real ones come from the owner's Deck test. So far only the Deck preset was measured, on the
   desktop GPU.

Still open, for the implementation:

- **The unset path's register cost** on gfx1033 (the Deck's GPU), from Radeon GPU Analyzer and the bench. If
  occupancy drops for batches without a detail, the branch becomes pipeline variants for the full shadow set
  (Implementation, "Keeping unset free").
- **Low and the Deck.** Whether the detail stays on Low and the Deck preset, against v2's 0.1 ms line. The packed
  estimate straddles it (0.04–0.12 ms); the owner's Deck test decides. v2 may also measure it in place of the
  facade grain.
- **Phase 2 only:** whether sampling BC images on Vulkan needs `textureCompressionBC` enabled on the device, which
  SDL 3.4.2 doesn't ask for by default.

## Phases

1. **Phase 1, the world detail (0.6.0).**
   - The struct, the pointer, the packed flag, the branch in `production_model.frag`, the tests and the bench.
   - It works on today's RGBA8 textures (`btbh_gpu_window_texture_load_ex` with `WRAP_REPEAT`). v2 rebakes its
     PNGs packed and loads them as they are.
2. **Phase 2, compressed textures (later, its own release).**
   - 2a: raw BC blocks and DDS, the capability bit and the Vulkan feature. No shader change.
   - 2b: one-channel colour slots (BC4 and R8). Three shaders change.
   - It comes when v2's asset tools write DDS (Decisions, 5).

**Can phase 2 come first, or separately?** Separately, yes; first isn't needed.
- Neither phase uses the other. Phase 1 is complete on RGBA8, and phase 2 serves every texture slot, not only the
  detail.
- The order is phase 1 now and phase 2 when v2's tools write DDS, since the encoder lives in the game's tools,
  not the renderer.
- Nothing about memory forces it: v2's textures stay near 50 MiB with the packed sets, which is not a problem on
  any target. BC's gains are load time (no PNG decode, no GPU mip generation) and some cache.
