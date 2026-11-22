# RFC 0011: Cheaper draws: shared mesh buffers and same-material runs; draw lists

- **Status:** Phase 1 accepted on 2026-11-22 for 0.10.0 (the owner left the decision to the coordinator). Phase 2
  waits for phase 1's numbers. See "Decisions"
- **Requested by:** Be The Bullet Hell v2 (its session, 2026-11-22, after the owner's sixth play-test)
- **Affects:** Be The Bullet Hell v2 (adopts it). Phase 1's encode changes reach every game without an API and draw
  byte for byte as before: Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D change nothing and gain where their
  batches run by material. Phase 2 is opt-in
- **Issue:** none. v2 answered all five questions through its session on 2026-11-22 (see "Decisions")
- **Depends on:** nothing. Written against 0.8.0's layout (`feature/0.8.0-detail-table`, unreleased): the batch is
  280 bytes there, `world_detail_table` at 272. Phase 1 changes no struct. Phase 2 appends a pointer at 280 (at 288
  if RFC 0012's `height_lift` is appended first)

## Need

v2's request, verbatim:

> "v2's new outskirts ring adds about 285 draws at a map edge (+0.6–1.5 ms renderer CPU geometry encode). One draw
> call covering many meshes with one material would remove most of them."

The ring, from v2's code (paths under `Be-The-Bullet-Hell/v2/crates/`, BTBH `dec1a3b7`; the commit that made it is
`361be68d`):

- **Built once a stage, never damaged.** 32-cell chunks, one persistent instance buffer each, "a draw per (chunk,
  mesh, surface)" (`v2-render/src/outskirts.rs:6-14`).
- **Distance models only.** Flat facades, a third of the triangles, one surface each. Two variants a piece at most:
  293 distinct meshes, 2,637 chunk ranges on stage 3. No sun shadow of its own.
- **Darker with distance.** The instance colour fades the ring; each building's lot seeds its world detail through
  the instance's custom w.
- **Kit pieces are custom meshes** (`btbh_gpu_window_custom_mesh_create`, `v2-platform/src/gpu.rs:931`), made by
  v2's own GLB parser. Props (trees, lamps) are models (`btbh_gpu_window_model_load_ex`, `:1074`).
- **The cost** (commit `361be68d`): +285 draws at a map edge (900 → 1,185), +0.6–1.5 ms renderer CPU. The Deck
  preset's frame there went from 3.2–3.3 to 3.6–4.1 ms.

## What a draw costs today

### The main pass, per batch and geometry

`src/gpu/btbh_gpu_scene.c:8371-9097` (v0.7.0 lines; 0.8.0 adds the table's bindings):

| Step | Size | Skipped when |
| --- | --- | --- |
| Pipeline lookup and bind | – | the pipeline is the one bound |
| Stencil reference | – | never |
| Vertex uniforms (`BTBHGPUSceneUniforms`), filled and pushed | 416 bytes | never |
| Fragment uniforms (`BTBHGPUSceneFragmentUniforms`), filled and pushed | about 3.1 KB: 40 lamps, 64 point-light rows, the sky's harmonics, three shadow matrices, and about 100 bytes of material | never |
| Batch extension (dig mask, world detail), pushed | 80 bytes | never |
| Instance and motion storage buffers | 2 bindings | never called less |
| Fragment samplers | 13–14 bindings (15 on 0.8.0) | never called less; SDL rewrites the descriptor set only when one changed |
| Vertex buffer | 1 binding | SDL skips the same buffer and offset |
| Index buffer | 1 binding | the renderer skips the one bound |
| The draw | – | – |

- **The material is a small part of the fragment block.** About 3 KB of it is the frame's lighting, the same for
  every batch, refilled on the CPU and pushed again each time.
- **A model with several primitives** repeats every step per primitive, though its primitives share the batch's
  material.
- **The shadow passes and the RC material-source pass** repeat a shorter version per batch (`:5869-6075`,
  `:6190-6470`).

### Measured, and the gap

- **The bench** (RFC 0009 phase 2, `BTBH_SMOKE_BENCH=table`): 1,000 split batches against 200 merged ones took
  0.40–0.55 ms more to prepare and encode: 0.5–0.7 µs a batch on the bench machine.
- **v2's figure** is 0.6–1.5 ms for 285 draws: 2–5 µs a draw. The difference isn't explained yet. Three things in
  SDL 3.4's Vulkan backend grow with what v2 draws and the bench didn't:
  - **Buffer tracking.** Every bind of a buffer the command buffer hasn't seen recently scans the list of buffers
    it used so far, from the end (`TRACK_RESOURCE`, `external/SDL/src/gpu/vulkan/SDL_gpu_vulkan.c:2376-2391`;
    vertex binds `:8014-8022`, every index bind `:8038`). A custom mesh owns its vertex and index buffers
    (`src/gpu/btbh_gpu_internal.h:186-203`, created at `src/gpu/btbh_gpu_custom_mesh.c:181`), so v2's hundreds of
    kit meshes are hundreds of pairs of buffers. Model primitives already share a few arena buffers.
  - **Uniform blocks.** SDL's uniform buffers are 32 KiB (`external/SDL/src/gpu/SDL_sysgpu.h:35`). It takes a new
    one when fewer than 4 KiB plus the push remain (`SDL_gpu_vulkan.c:7703`), and then writes a new descriptor set.
    At about 3.3 KB a fragment push (aligned), every ninth draw does that.
  - **Prepare.** Each batch is checked, counted and keyed before encoding; a ring range is a batch.
- Open question 1 asks v2 for the split by report field, with and without the ring.

## Options

| | Phase 1: shared buffers and same-material runs | Phase 2: draw lists (multi-draw indirect) | Host-side merge (a mesh builder) |
| --- | --- | --- | --- |
| What it is | Static custom meshes in the models' geometry arena; a batch that matches the previous one but for its geometry and instances binds and pushes nothing new | One batch draws many (mesh, instance range) entries with one material, through `SDL_DrawGPUIndexedPrimitivesIndirect` | Meshes sharing a material concatenated into one mesh, each part transformed into place |
| CPU per range | One draw call; estimate 0.1–0.2 µs | 20 bytes written and a few checks; one call per (chunk, material, arena block) | None: the range is gone |
| Prepare per range | As today (it is still a batch) | A few checks per entry | None |
| GPU | Unchanged | The same vertices and pixels; 20 bytes read per command. True multi-draw where the device has `multiDrawIndirect` (`SDL_gpu_vulkan.c:12293-12295`), else SDL loops (`:5438-5458`); Metal loops (`SDL_gpu_metal.m:2930-2938`) | The same pixels; each instance's vertices stored again, so vertex memory grows by the instance count |
| Culling | The host's, per chunk, as today | The host's, per list (a chunk and material). Per entry needs a new list each frame | Per merged mesh only: a chunk is culled whole, or the host makes more, smaller meshes |
| Shadows | The shadow passes take the same runs | Each pass issues the same commands | One draw per merged mesh per pass |
| Picking | The renderer has none; v2 picks on its cells. Nothing changes | Nothing changes | The renderer loses which piece a triangle came from (no renderer picking uses it) |
| Instance data | Kept | Kept | Lost: the fade colour moves into vertex colours; the lot seed (custom w) has no per-vertex slot (joints.x is 0.8.0's table index) |
| Memory | Arena blocks (64 MiB vertices, 32 MiB indices); holes aren't reused | An indirect buffer of 20 bytes a command, per frame | A piece of 100–500 vertices costs 9.6–48 KB again per instance: 96–480 MiB for ten thousand |
| API | One creation flag | An entry struct, a list struct, a batch pointer | One function; or none, since v2 parses its GLBs and can merge in Rust today |
| Who gains | Every game, without changing code (runs); v2 with the flag | Hosts that build lists | The host that merges |

- **The host-side merge doesn't fit the ring.** Its pieces are instanced (293 meshes over 2,637 ranges), and their
  instance colour and seed carry the fade and the world detail. Baking them would multiply the vertex memory and lose
  both. It fits unique static geometry, which v2 already bakes (the ring's water, the hills).
- **Phase 1 first.** It costs no GPU time, keeps every image byte for byte, and serves every game. It takes the
  per-range cost close to one SDL draw call when ranges run by material and their meshes share buffers.
- **Phase 2 after phase 1's numbers.** It removes what phase 1 leaves: the prepare per batch and the calls. It needs
  an API and the arena (phase 1a) anyway.

## Proposal

### Phase 1a: custom meshes in the shared geometry arena

```c
enum {
    /* The mesh's vertices and indices live in the window's shared geometry buffers, with the models' primitives,
     * instead of buffers of its own (RFC 0011, since 0.10.0). Batches of such meshes then bind their buffers once
     * for a run. btbh_gpu_custom_mesh_update must fit the vertex and index counts the mesh was created with;
     * a bigger one fails with BTBH_GPU_INVALID_ARGUMENT. The arena doesn't reuse holes: a buffer is freed when
     * every mesh and model in it is destroyed. For meshes made once and kept (a level's kit). */
    BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS = 1u << 0
};

/* btbh_gpu_window_custom_mesh_create with flags (BTBH_GPU_CUSTOM_MESH_*). 0 is
 * btbh_gpu_window_custom_mesh_create. An unknown bit fails with BTBH_GPU_INVALID_ARGUMENT. */
BTBHGPUCustomMesh *btbh_gpu_window_custom_mesh_create_ex(
    BTBHGPUWindow *window,
    const void *vertices,
    uint32_t vertex_byte_count,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    uint32_t flags,
    BTBHGPUCustomMeshReport *report
);

/* Defined when btbh_gpu_window_custom_mesh_create_ex and BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS exist (since 0.10.0). */
#define BTBH_GPU_HAS_SHARED_MESH_BUFFERS 1
```

- **The arena is the models'.** `btbh_gpu_geometry_arena_upload` (`src/gpu/btbh_gpu_model.c:1245-1314`) places
  data in 64 MiB vertex and 32 MiB index blocks (`:1195-1196`). A shared mesh keeps its block, `vertex_base` and
  `first_index`, as a model primitive does (`src/gpu/btbh_gpu_internal.h:26-40`). The scene draws it the same way.
- **Updates in place.** An update that fits writes into the mesh's range of the block, as an update writes into its
  own buffer today. SDL orders the copy after the earlier frames' draws.
- **Without the flag** nothing changes: the mesh owns its buffers and grows on update, as since 0.5.0.

### Phase 1b: same-material runs (no API)

In every pass that encodes batches (main, transparent, shadows, RC material sources):

- **The key.** A batch matches the one drawn before it in the pass when everything but its geometry and instance
  range is equal: the pipeline it selects, its material fields, textures, detail and table, its instance buffer, its
  dig binding, whether it carries motions, and the fast-unlit choice. The fields that may differ are `kind`,
  `model`, `custom_mesh`, `material_slot`, `first_instance` and `instance_count`. A skinned batch never matches
  (its bone offset changes the vertex uniforms).
- **A matching batch** pushes no uniforms and binds no samplers or storage buffers: SDL keeps them for the pass's
  later draws. 0.8.0's table block, pushed once for a run of batches sharing it (`a44746f6`), already relies on that.
  It binds the vertex and index buffer only if they changed, and draws.
- **A model's second and later primitives** count as matching: they share the batch's material.
- **The comparison** copies the batch's 280 bytes with the six fields cleared and compares them with the previous
  batch's copy: tens of nanoseconds.
- **The picture doesn't change.** The GPU gets the same state for every draw. The tests compare byte for byte.

### Phase 1c: the frame's lighting filled once a pass (no API)

- The fragment block's frame part (lights, lamps, point lights, harmonics, sky, clouds, shadows) is filled once a
  pass. Each batch writes only its own fields (`material_factors` through `surface_parameters1`, the reactive scale
  in `procedural_controls.z`, `detail_parameters`) into a copy, then pushes it.
- The push stays per batch when the material changes. Splitting the frame part into its own uniform buffer would
  shrink it to about 100 bytes, but changes every scene fragment shader and takes the last of SDL's four slots
  (0.8.0 uses three): see Alternatives.

### Phase 2: draw lists (only if phase 1 isn't enough)

```c
/* One entry of a draw list: a geometry and its instances, drawn with the batch's material (RFC 0011). */
typedef struct BTBHGPUSceneDrawListEntry {
    BTBHGPUModel *model;              /* a model, or NULL. Not skinned */
    BTBHGPUCustomMesh *custom_mesh;   /* or a custom mesh made with BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS; one of the
                                       * two is set */
    int32_t material_slot;            /* a model's primitives, as BTBHGPUSceneGeometryBatch.material_slot */
    uint32_t first_instance;          /* in the batch's instance buffer, or its transient instances */
    uint32_t instance_count;
    uint32_t reserved;                /* 0 */
} BTBHGPUSceneDrawListEntry;

typedef struct BTBHGPUSceneDrawList {
    const BTBHGPUSceneDrawListEntry *entries;
    uint32_t entry_count;             /* 1 or more */
    uint32_t flags;                   /* 0 */
} BTBHGPUSceneDrawList;

/* In BTBHGPUSceneGeometryBatch, appended after world_detail_table: */
    /* NULL: drawn as before. Otherwise the batch draws every entry's geometry and instances with its own material,
     * in one indirect draw per pair of arena buffers (RFC 0011). The batch's kind, model, custom_mesh,
     * first_instance, instance_count and animation fields must then be zero. The host owns the list; the renderer
     * reads it during the present call only. */
    const BTBHGPUSceneDrawList *draw_list;

/* Defined when BTBHGPUSceneGeometryBatch.draw_list and its structs exist. */
#define BTBH_GPU_SCENE_HAS_DRAW_LISTS 1
```

Layouts on 64-bit: `BTBHGPUSceneDrawListEntry` 32 bytes (`model` 0, `custom_mesh` 8, `material_slot` 16,
`first_instance` 20, `instance_count` 24, `reserved` 28); `BTBHGPUSceneDrawList` 16 bytes (`entries` 0,
`entry_count` 8, `flags` 12). The batch grows from 280 to 288 bytes; `draw_list` sits at 280.

- **Encoding.** The renderer writes one `SDL_GPUIndexedIndirectDrawCommand` (20 bytes) per entry and primitive into a
  per-frame indirect buffer (`SDL_GPU_BUFFERUSAGE_INDIRECT`), uploaded in the frame's copy pass with the transient
  instances. Entries are grouped by arena block pair, and each group is one `SDL_DrawGPUIndexedPrimitivesIndirect`.
- **Instances.** Each command carries its entry's `first_instance`. The vertex shaders index `instances[]` by
  `SV_InstanceID`, which includes the base instance on Vulkan and Metal, as the direct draws already rely on
  (`batch->first_instance` is passed to every draw today). SDL enables `drawIndirectFirstInstance` by default
  (`SDL_gpu_vulkan.c:12472`); the renderer doesn't turn it off.
- **Every pass reuses the commands**: main, shadows, spot and omni faces, RC sources. The fast shadow path reads
  instances as a vertex stream bound at `first_instance` today (`btbh_gpu_scene.c:6015-6032`); for a list it binds
  the stream at 0 and lets each command's base instance pick, as both APIs apply it to instance-rate attributes.
- **Unchanged per batch:** the opaque sort's one key (over every entry's instances), `_with_visibility`'s bytes, the
  reactive mask, the stencil.
- **Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, naming the batch and the entry): no entries; both or neither geometry
  set; a skinned model; a custom mesh without shared buffers; a geometry of another window; a range past the
  instances; `reserved` or `flags` set; the batch's own geometry or instance fields set.

## Minimal example

```c
/* Phase 1: the kit's meshes in the shared buffers, made once. */
BTBHGPUCustomMesh *piece = btbh_gpu_window_custom_mesh_create_ex(window, vertices, vertex_bytes, vertex_count,
    indices, index_count, BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS, &mesh_report);

/* Each chunk's ranges, grouped by material: every batch the same but for custom_mesh and the range. */
for (uint32_t r = 0; r < chunk->range_count; ++r) {
    BTBHGPUSceneGeometryBatch b = chunk->material_batch[chunk->ranges[r].surface];   /* the material, once */
    b.kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH;
    b.custom_mesh = chunk->ranges[r].mesh;
    b.instance_buffer = chunk->instances;
    b.first_instance = chunk->ranges[r].first;
    b.instance_count = chunk->ranges[r].count;
    batches[batch_count++] = b;
}

/* Phase 2: one batch per chunk and material. */
BTBHGPUSceneDrawList list = { entries, entry_count, 0 };
BTBHGPUSceneGeometryBatch b = chunk->material_batch[surface];
b.instance_buffer = chunk->instances;
b.draw_list = &list;                  /* kind, model, custom_mesh, first_instance, instance_count stay 0 */
```

In v2's Rust: `Gpu::mesh` (`v2-platform/src/gpu.rs:925`) gains a variant that passes
`BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS`, for the kit library; the ring's and the map's chunk draws are sorted by
(material, mesh) before `batch_of`.

## Implementation

### Phase 1

| File | Change |
| --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | `btbh_gpu_window_custom_mesh_create_ex`, the flag, the macro |
| `src/gpu/btbh_gpu_custom_mesh.c` | Shared meshes: arena upload and release (the model arena's functions made shared), updates in place, the size refusal |
| `src/gpu/btbh_gpu_model.c` | `btbh_gpu_geometry_arena_upload` and the block release reachable from the custom meshes |
| `src/gpu/btbh_gpu_internal.h` | `BTBHGPUCustomMesh` gains its blocks, `vertex_base` and `first_index` |
| `src/gpu/btbh_gpu_scene.c` | Draw a shared mesh by `vertex_base` and `first_index`; the run key and the skips in the main, transparent, shadow and RC source loops; the frame part of the fragment block filled once a pass |
| `Sources/` (Swift) | Nothing. A `CustomMesh(..., sharedBuffers: true)` can follow when v1 asks |

- No shader changes, no new shader file.
- The opaque sort and the bounds already handle custom meshes; a shared mesh keeps its bounds.

### Phase 2

The header's structs, the pointer and the macro; the per-frame indirect buffer and its upload; the grouping by block
pair; the indirect draws in each pass; the refusals. No shader changes.

### Cost

| Item | Estimate (desktop CPU of the bench machine) |
| --- | --- |
| Phase 1, a range in a run | One draw call, no binds: about 0.1–0.2 µs, against 0.5–0.7 µs a batch today |
| Phase 1, a batch that starts a run | As today, minus the frame part's fill |
| Phase 1, GPU | None |
| Phase 1, memory | Shared meshes in 64/32 MiB blocks instead of buffers sized to each mesh |
| Phase 2, per entry | 20 bytes written, a few checks |
| Phase 2, per list | One indirect draw per arena block pair and pass |
| Phase 2, GPU | 20 bytes read per command; otherwise the same work |

For v2's 285 ranges at the edge, phase 1 would take the encode from the bench's 0.14–0.2 ms to about 0.03–0.06 ms, if
the ranges run by material. Whether it also removes the gap between the bench and v2's 2–5 µs a draw depends on
where that goes (open question 1); the shared buffers remove the tracking scans for the kit.

**How to measure it.**
- **Frame bench** (`btbh_gpu_frame_bench`, RFC 0009 phase 2): 2,000 draws of 32 kit pieces. Configurations: today;
  shared buffers; runs (draws sorted by material); both. The report's `cpu_prepare_milliseconds` and
  `cpu_geometry_encode_milliseconds`, and the cost log's split frames for the GPU (expected unchanged).
- **A many-meshes case**: 600 distinct small meshes, so SDL's tracking list is as long as v2's.
- **v2:** the map edge, with and without the ring, before and after.

## Compatibility

- **Versioning.** Phase 1 is minor (0.10.0) for the new function and flag; its encode changes are a patch-level
  change that keeps every image. Phase 2 is minor: a pointer appended to the batch (280 → 288 bytes), new structs.
- **Be The Bullet Hell v2 (Rust, bindgen):** nothing changes until it opts in. Phase 1: the flag on its kit meshes,
  and its batches sorted by material. Phase 2: regenerated bindings (`..Default::default()` leaves `draw_list` null),
  then lists per chunk and material.
- **Be The Bullet Hell (v1, Swift):** nothing to change. Its custom meshes keep their buffers. Its model batches with
  several primitives, and any batches it submits in material runs, encode faster.
- **CHIMERA (Rust, bindgen):** nothing to change. Its dynamic custom meshes keep their own buffers and grow on update
  as before. Phase 2's bigger batch needs regenerated bindings.
- **Tyrian3D (C):** nothing to change. `init_batch` memsets the batch. Pinned at v0.2.1.
- **Shaders:** none change in either phase.

## Platforms and testing

- **Native:** `btbh_gpu_scene_order_test` (phase 2): `offsetof(draw_list) == 280`, the batch 288 bytes, the entry 32,
  the list 16. The run key: which fields break a run (each material field, the instance buffer, the dig binding,
  motion, skinning) and which don't (the six).
- **GPU smoke (Windows/Vulkan):**
  - **Byte for byte:** the world detail smoke's gallery, `btbh_gpu_scene_target_smoke`, the prewarm and SDF terrain
    smokes, with phase 1's encode against 0.8.0's (runs on and off; shared meshes against own buffers).
  - **Shared meshes:** create, update in place, an update that doesn't fit refused, destroy and the block freed when
    empty; two shared meshes in one batch run draw as two meshes with their own buffers.
  - **Phase 2:** a list of N entries against N batches, byte for byte, through the six scene shaders, the shadows
    and the RC sources; the refusals.
- **Bench:** as above (`BTBH_SMOKE_BENCH`, the frame bench).
- **Platforms:** Windows/Vulkan runs the smokes. Phase 2's indirect draws on Metal (SDL loops per command) are
  checked by the Mac session's `check.sh` run; Linux and the Deck (RADV has `multiDrawIndirect`) through v2.
- **Games:** v2's edge timings; CHIMERA 0 pixels different; Be The Bullet Hell its Swift suite.

## Alternatives

| Alternative | Why not (now) |
| --- | --- |
| **The host-side merge** (a mesh builder concatenating meshes with transforms) | The ring's pieces are instanced; baking multiplies vertex memory by the instance count and loses the instance colour (fade) and custom w (the lot seed). v2 can merge unique static geometry in Rust already, without a renderer API |
| **Draw lists first** | They need the shared buffers anyway, add an API, and only remove what phase 1 leaves. Phase 1's numbers decide |
| **All custom meshes in the arena** (no flag) | CHIMERA's and others' dynamic meshes grow and are remade; the arena doesn't reuse holes, so blocks would stay alive. The flag keeps that a choice |
| **The frame's lighting in its own uniform buffer** | Pushed once a pass, it would cut each material change's push from 3.1 KB to about 100 bytes and the descriptor-set churn with it. But every scene fragment shader changes its bindings, and it takes the last of SDL's four uniform slots (0.8.0 uses three for the material, the extension and the table). Worth an RFC of its own if phase 1 shows the pushes are what's left |
| **GPU culling** (a compute pass writing the indirect commands from per-entry bounds) | The frustum test on the GPU would save the host's culling too, but needs storage buffers written by compute, per-entry bounds and a pass. CHIMERA's note (§9.6, `docs/rfc/README.md`) asked for culling in the renderer; this would be the place, with a second game asking |
| **Vertex pulling** (one draw, the vertex shader fetching any mesh's vertices from storage) | New vertex shaders for every path, and the 0.8.0 lesson: one more vertex input cost about 2 % on many small draws |
| **A patched SDL** (a hash set for buffer tracking) | The renderer builds against stock SDL 3.4.2 (`external/SDL`); the shared buffers avoid the scan without a fork |

## Open questions for v2

1. **Where the 0.6–1.5 ms go.** Please send the scene report's `cpu_prepare_milliseconds`,
   `cpu_shadow_encode_milliseconds`, `cpu_geometry_encode_milliseconds`, `primitive_draw_count` and
   `radiance_cascades.cpu_encode_milliseconds` at a map edge, with and without the ring (`BTBH_OUTSKIRTS=0`). The
   bench says 0.5–0.7 µs a batch; your figure is 2–5 µs a draw.
2. **Material runs.** Can you submit the ring's and the map's chunk draws grouped by material (surface textures,
   detail table, pattern, flags), mesh after mesh within a group? Phase 1's runs need that order.
3. **Mesh lifetimes.** Are the kit's custom meshes made once a run (or a stage), never resized, and destroyed
   together? The shared buffers don't reuse holes, and an update must fit.
4. **Phase 2.** If phase 1 brings the edge within your budget, do you still want draw lists? If so, is a list per
   chunk and material enough, or do you need to drop single ranges per frame?
5. **Baking.** Is there static geometry with one instance per mesh (the moat's banks, the boulevard, the far boxes)
   that you would rather merge into chunk meshes in Rust? That needs nothing from the renderer.

All five are answered: see "Decisions".

## Decisions (v2, 2026-11-22)

v2 answered all five questions on 2026-11-22, measured on 0.8.0 at BTBH `59af919b`: the stage 3 map edge at the Deck
preset (1280 x 800) on a desktop RTX, medians over frames 300 and later of two or three interleaved runs. The owner
left the decision to the coordinator, who accepted phase 1 for 0.10.0. Phase 2 waits for phase 1's numbers (answer 4).

1. **Where the ring's cost goes.** The ring adds 257 batches.
   - Geometry encode grows by 0.506 ms: 1.97 µs a batch (2.09 µs on High at 1080p). Prepare grows by 0.13 µs a batch
     and shadow encode by 0.04 µs.
   - The present's wall clock grows by 0.8–0.9 ms, against 0.58 ms in the timed fields: 0.25–0.35 ms of the extra is
     in the submission (queue submit, present, the driver), which no encode field shows.
   - The cost a batch drifts with the machine's load: 1.31–1.67 µs.
   - Marginal geometry encode a ring batch:

     | | World detail on | World detail off |
     | --- | --- | --- |
     | As it is | 1.31 µs | 0.76 µs |
     | One shared mesh for every ring batch | 0.64 µs | 0.39 µs |

   - **The split.** Distinct buffers a batch cost 0.4–0.7 µs: SDL's buffer tracking ("Measured, and the gap"), which
     phase 1a removes. Table batches interleaved with batches without a table cost 0.25–0.55 µs a batch: each switch
     alternates the static vertex shader between `production_model.vert` and `production_model_detail_table.vert`
     (RFC 0009's follow-up), so the pipeline, and, as v2 reads it, breaks the table block's run. What remains, about
     0.4 µs, matches the bench's 0.5–0.7 µs; phase 1b and 1c take it on.
   - So the gap between the bench and v2's 2–5 µs a draw is explained: the bench's split batches shared a few
     meshes and none of them alternated with a table batch.
2. **Material runs: yes.** v2 will stable-sort each chunk's batches by a material key and keep coplanar overlays after
   their ground.
   - Runs can't span chunks while each chunk has its own instance buffer: the instance buffer is part of the run key.
   - The ring is static and can share one instance buffer, with offsets, across its chunks. The map's chunks rewrite
     their buffers on damage, so they keep one each.
3. **Mesh lifetimes: as phase 1a needs.** Kit, landmark and set-piece meshes are made once a process, never updated,
   and destroyed at window close: they take the flag.
   - Per-stage meshes (the kit's water, terrain chunks, the ring's water) stay out of the shared buffers.
   - v2 doesn't destroy its per-stage meshes yet; that is v2's leak, which v2 will fix. v2 asked whether a destroy call
     exists: `btbh_gpu_custom_mesh_destroy` does, as the coordinator answered.
4. **Phase 2: phase 1's numbers decide.**
   - The ring costs about 0.5 ms on the desktop, probably two to three times that on the Deck, so v2 will likely still
     want lists after phase 1.
   - One list per (chunk, LOD, material) is enough. Lists are rebuilt on a LOD switch or damage; no range is dropped
     per frame.
5. **Baking: v2's own.** v2 merges unique static geometry in Rust and needs no API: the ring's ground goes from about
   54 draws to about 6. The 167 building draws stay instanced.

**What the answers mean for phase 1** (the coordinator's reading):
- **Table and non-table batches interleave within a piece.** A building piece's walls draw with the world-detail table
  and its glass and signs without (RFC 0009, decision 4), one after the other. Each switch changes the static vertex
  shader (`production_model.vert` against `production_model_detail_table.vert`), so the pipeline: SDL then writes all
  four descriptor sets again at the next draw, and two batches on either side of the switch never share a run key.
  A material sort alone keeps that interleave wherever the glass must follow its wall.
- **Phase 1 deals with it.** It either keeps runs intact across the switch, or documents the order a host sorts to so
  that the switch happens once a chunk rather than once a piece; and it measures the interleave before and after.
  "Implementation notes (phase 1)" says which it does.
- **The run key holds the instance buffer** (answer 2): a run never spans two instance buffers. The ring gains most
  if it shares one.
- **The arena's rules can be strict** (answer 3): v2's flagged meshes are never updated, so an update that doesn't fit
  the mesh's ranges can be refused, and a destroy may leave a hole.

Nothing is open for phase 1.

## Phases

1. **Phase 1, shared buffers and runs (0.10.0; Decisions).** The creation flag, the run key and skips in every pass,
   the frame part filled once a pass, the table/non-table interleave, the tests and the bench. No shader change.
2. **Phase 2, draw lists (after 0.10.0, only if phase 1 isn't enough).** The structs, the pointer, the indirect
   buffer, the grouping, the refusals. It waits for phase 1's numbers on v2's edge (Decisions, 4: one list per
   chunk, LOD and material would do).
