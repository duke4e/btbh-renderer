# RFC 0011: Cheaper draws: shared mesh buffers and same-material runs; draw lists

- **Status:** Phase 1 accepted on 2026-11-22 (the owner left the decision to the coordinator) and released in
  v0.10.0. Phase 2 waits for phase 1's numbers: v2's geometry encoding fell to 0.14–0.27 ms with phase 1. See
  "Decisions"
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

## Implementation notes (phase 1, 2026-11-22)

Phase 1 is implemented for 0.10.0 on `feature/0.10.0-cheaper-draws`, cut from 0.9.0's candidate (`d077b5e6`) with
the drafts of RFCs 0010–0012:
- the header: `BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS`, `btbh_gpu_window_custom_mesh_create_ex`,
  `BTBH_GPU_HAS_SHARED_MESH_BUFFERS`, and a note on runs and the order a host submits in, beside
  `btbh_gpu_window_scene_present_instances`;
- `src/gpu/btbh_gpu_custom_mesh.c` (shared meshes), `src/gpu/btbh_gpu_model.c` (the arena's upload, copy and release
  shared with the custom meshes), `src/gpu/btbh_gpu_internal.h`;
- `src/gpu/btbh_gpu_scene.c`: the run key and its skips in every pass that encodes batches, the frame's uniforms once
  a pass, the shared meshes' ranges in the draws;
- the tests: `btbh_gpu_scene_order_test` (the key), `btbh_gpu_scene_runs_smoke` (new), `BTBH_SMOKE_SHARED_MESHES` in
  the smokes, the frame bench's ring and `tools/frame_bench_cpu.py`.

No struct of the public header grows and no shader changes. The version macros stay at 0.8.0's, as on 0.9.0's
candidate, until the release commit.

**The shared meshes' rules** (the text said less):
- `btbh_gpu_window_custom_mesh_create` is `_ex` with no flags and unchanged. An unknown bit is refused
  (`BTBH_GPU_INVALID_ARGUMENT`, "Unknown GPU custom mesh flags"); the data is checked as before.
- **Placement.** As a model primitive's: the vertices (96-byte aligned, so `vertex_base` counts vertices) and the
  indices (4-byte aligned) are appended to the arena's open block, the last one; a range that doesn't fit opens a new
  block, and one bigger than a block (64 MiB, 32 MiB) gets a block of its own size. Each range is one copy submission,
  as for the primitives. A mesh without indices takes no index range.
- **Updates** write in place, vertices and indices in one submission, without cycling (the block's other ranges are
  other meshes' and models'). They must fit the ranges the mesh was created with: at most its vertex count and its
  index count, none for a mesh made without indices. A bigger one is refused, naming both counts, and leaves the mesh
  as it was. A smaller one draws its own counts; a later update may grow back to the created size. Because they
  don't cycle, an update shows in every frame submitted after it, a scene already presented but not yet submitted by
  its canvas included; a mesh with buffers of its own keeps the old data for that frame (SDL cycles its buffer).
- **Destroy** gives the ranges back to their blocks. The holes are never reused (the arena only appends to its last
  block); a block's buffer is released with its last range, a mesh's or a model primitive's, or at window close. A
  mesh that outlives its window can be destroyed after it; an update of it is refused.
- Decided from v2's answer 3: the strict rules cost v2 nothing (its flagged meshes are never updated) and keep a
  dynamic mesh from pinning blocks by accident. Per-stage meshes stay without the flag.

**The run key** (as drafted, more exactly):
- `btbh_gpu_scene_batch_continues_run(previous, batch)`: true when 53 of the batch's 59 fields are equal, byte for
  byte, each compared on its own; the six that may differ are `kind`, `model`, `custom_mesh`, `material_slot`,
  `first_instance` and `instance_count`. The animation fields are in the key (the text didn't say). Padding (13 bytes
  on 64-bit) isn't compared, so a host that doesn't zero it (Rust's struct literals, Swift) still runs. Floats compare
  by their bits: −0 and +0 break a run (conservatively), the same NaN doesn't. Pointers compare as pointers: two
  world details or tables with equal contents but different addresses break a run.
- Why it is enough: everything a pass binds or pushes for a batch, apart from its vertex and index buffers and the
  draw, is a function of those fields (and of what they point at, which a present doesn't change: the dig mask comes
  from `dug_by`, the pipeline from the material and the stencil fields), of the pass and of whether the geometry is
  skinned. The passes require both geometries unskinned: a skinned one never starts or continues a run.
- "The batch before" is the last geometry the pass drew. A batch the pass skips (invisible, the other phase, not a
  caster of this map, not a source) changes nothing. Each pass starts without one: the opaque, transparent and
  refraction passes, every shadow map and omni face, the cascades' sources.
- A geometry that continues its run binds and pushes nothing but its vertex and index buffers, each only when it
  changed, and draws. In the geometry passes that skips the vertex and fragment uniforms, the extension block, the
  table's block, the stencil reference, the pipeline's lookup and bind, the storage buffers and the samplers' array
  and bind; in the shadow passes the slow path's uniforms, the pipeline, the dig mask and the storage buffers; among
  the cascades' sources the uniforms, the storage buffers, the dig mask and the samplers.
- A model's second and later primitives continue their batch's run (unskinned).
- The fast shadow path binds the instances as a vertex stream at `first_instance`, so every range still binds its
  stream. Binding it at 0 and passing `first_instance` to the draw would remove that, but changes every game's fast
  shadow draws, and Metal's handling of a base instance there is unverified: not done. v2's ring casts no sun shadow.
- `BTBH_GPU_SCENE_RUNS=0` (a measurement tool, read once a present) turns runs off. The package-internal telemetry
  counts the draws that continued a run per kind of pass (`geometry_run_draw_count`, `shadow_run_draw_count`,
  `rc_source_run_draw_count`); the pipeline bind and skip counts mean what they meant.
- `BTBH_GPU_SCENE_RUNS=verify` (0.13.0, a debug mode, read once a present) keeps runs on and checks the run key
  against what the passes read. Each pass routes the pushes and binds of a batch's state through small helpers
  (`btbh_gpu_scene_run_*` in `btbh_gpu_scene.c`), which are the plain SDL calls when the mode is off. A draw that
  starts a run records what it pushes and binds: the last block in each uniform slot, the vertex and fragment
  samplers, the vertex storage buffers and the stencil reference. A draw that continues one runs the same code with
  the helpers comparing instead: each block byte for byte at the size it would be pushed with, the bindings, the
  stencil reference and the pipeline it would bind against the one bound. It leaves what the pass caches (the
  pipeline bound, the table, lift and particle blocks pushed, the run's batch) alone, and pushes and binds nothing, so
  the frame is the frame with runs on. A difference is logged through SDL_Log, naming the pass, the batch, the uniform
  slot and the first field it touches (the first 64 per window), and counted: per present in the telemetry's
  `run_verify_draw_count` and `run_verify_mismatch_count`, and over the window's life, renders into a target included,
  by `btbh_gpu_window_scene_read_run_verify`. CTest runs the smokes with runs again in the mode
  (`*_runs_verify`). The runs smoke's waterfront (0.13.0) has runs of every kind of batch added since 0.10.0. Dropping
  `roughness` from the key on purpose makes the flags smoke log `material_factors (+8)` and fail.

**The frame's uniforms once a pass** (phase 1c, as drafted): at the start of each geometry pass the frame's part of
both uniform blocks is filled; each batch writes its own fields over it (the material, the bone offsets, the motion
flag, the reactive scale, the detail) before it pushes them, so every push carries the bytes it carried before. The
shadow passes' slow uniforms and the cascades' sources' vertex uniforms do the same. The geometry passes bind a vertex
buffer only when it changes.

**The table/non-table interleave** (Decisions):
- The table's block didn't break with the interleave: 0.8.0's `pushed_table` already outlived a batch without a table
  (such a batch never touches slot 2), so v2's walls pushed their table once a table, not once a switch. What a switch
  costs is the pipeline: SDL then writes all four descriptor sets at the next draw.
- Runs can't be kept across the switch: on either side of it the batches differ in their material (the table, and in
  v2 the textures), so they bind and push anew whatever the renderer does. Drawing a batch without a table through the
  table's vertex shader would avoid the pipeline switch, but costs the sixth attribute on the GPU (RFC 0009's
  follow-up) and isn't guaranteed to rasterise to the same bits on every driver; the renderer reordering the batches
  would change the order the host chose (overlays, depth ties). Neither is done.
- So phase 1 documents the order instead (the header, beside the present): within each instance buffer, by static
  vertex shader first (table batches together, the others together), then by material, mesh after mesh, coplanar
  overlays after what they lie on and grouped the same way; and measures the interleave both ways (below).

**Tests**
- **Native** (`btbh_gpu_scene_order_test`): the flag's value and macro; the run key with every byte of a batch flipped
  in turn, both ways round: the bytes of the six fields and the padding (45 of 280) keep a run, the other 235 break
  it; the fields listed cover the batch, so a field appended later fails the test until it is listed; the six changed
  at once keep it and one more field breaks it; −0 against +0 breaks it, the same NaN doesn't.
- **GPU, `btbh_gpu_scene_runs_smoke`** (new, the 25th test):
  - A street of kit pieces: ten distinct meshes (one without indices) in two chunks with a persistent instance buffer
    each, the first chunk's walls (a table) and glass (none) interleaved piece by piece, the second's sorted; a model
    of two primitives on two nodes in two batches around a skinned model; runs of emissive and unlit pieces, coplanar
    LESS_EQUAL decals, a stencil writer and two skippers, three boxes the middle one KEEP_ORDER, transparent pieces.
    Through no shadow, the sun's soft shadow, the sun's and the omni shadow, the cascades, the front-to-back sort.
  - Each scene with the meshes in buffers of their own and in the shared buffers, runs on and off: the same bytes,
    each twice. Presented to the window, the telemetry counts 24 (25 sorted) geometry draws that continued a run, 20
    in the sun's map and 140 over the sun's and the omni's faces, 3 among the cascades' sources, and none with runs
    off.
  - The shared meshes: unknown flags (two), invalid data (three) and no report refused; two meshes and then a model
    share one vertex and one index block (bases 0, 24 and 30); updates of 25 vertices, of 42 indices and with indices
    for a mesh made without refused, the mesh unchanged and drawing so; an update to three faces and back to six draws
    as a mesh of its own made with that data; the blocks' buffers stay after the meshes are destroyed and go with the
    model; a mesh outlives its window, its update is refused and it is destroyed after.
- `BTBH_SMOKE_SHARED_MESHES=1` (`Tests/btbh_gpu_smoke_meshes.h`): the flags, prewarm, scene-target, SDF terrain and
  world-detail smokes make their custom meshes (those they never update) in the shared buffers.

**Byte for byte against 0.9.0** (`d077b5e6`, the candidate a game is testing):
- A build of it with this branch's smokes and bench overlaid (they build against 0.9.0's header, without the shared
  meshes, the run counts and the run switch) wrote every smoke's images; this build compared them, decoded, pixel by
  pixel: the flags smoke's 25 images (640,000 pixels), the prewarm smoke's 16 window frames and 16 target renders
  (1,843,200), the world detail smoke's 56 (1,245,184), `btbh_gpu_scene_target_smoke`'s 9 (90,368),
  `btbh_gpu_font_smoke`'s 25 (6,549,120), the SDF terrain smoke's saved frame (25,600) and the runs smoke's gallery of
  5 (184,320; each of its 20 renders compared): **153 images, 10,577,792 pixels, 0 differ**
  - with nothing set (runs on, as every game gets them);
  - with `BTBH_SMOKE_SHARED_MESHES=1` (every smoke's custom meshes in the shared buffers);
  - with `BTBH_GPU_SCENE_RUNS=0`.
- Every smoke passed its own checks in each run. `check.ps1`: 25/25, 71 shaders, the headers on their own. Swift: the
  build and the full suite, 301 tests, 2 skipped, 0 failures; three of its source-text checks follow the moved uniform
  code (same facts, new indentation).

**Measured** (Windows, RTX 3070 Ti, Vulkan, no debug device; the frame bench's CPU mode, `tools/frame_bench_cpu.py`)
- **The frame.** Like v2's map edge: 943 draws of the bench's 32 kit pieces (one material, transient instances, the
  table with world detail on) and a ground, then the ring: 257 batches, a ground and 128 building pieces, each a wall
  (the table with world detail on) and its glass (no table), each one a distinct custom mesh, in 8 chunks with a
  persistent instance buffer each, no sun shadow. Every frame of a configuration alternates with the ring and without
  it; a pair's difference over 257 is the marginal cost a ring batch. Ten configurations as hidden windows in one
  process, in shuffled turns of 8 frames, 12 rounds; six pairs of processes, 0.9.0's candidate's core (with this
  bench overlaid) and this branch's alternating, each pair starting with the other build. A frame over 1.5 times its
  configuration's 25th percentile drops its pair (another session ran one to five `v2-game` processes on the machine
  throughout); 1.25 and 2.5 give the same picture within 0.1–0.4 µs.
- **Geometry encode a ring batch** (`cpu_geometry_encode_milliseconds`), µs, median of the pairs [25th, 75th
  percentile]:

  | Ring batches | 0.9.0 | 0.10.0 |
  | --- | --- | --- |
  | World detail on, interleaved (a wall, its glass), distinct meshes: v2's "as it is" | 2.19 [1.94, 2.58] | 2.01 [1.68, 2.42] |
  | the same, one mesh for every ring batch: v2's "one shared mesh" | 1.49 [1.31, 1.77] | 1.25 [1.18, 1.57] |
  | the same, distinct meshes in the shared buffers | 2.18 [1.94, 2.51]¹ | 1.38 [1.18, 1.98] |
  | World detail on, sorted (a chunk's walls, then its glass), distinct meshes | 1.19 [1.01, 1.50] | 0.52 [0.44, 0.68] |
  | the same, in the shared buffers | 1.13 [0.99, 1.43]¹ | **0.14 [0.12, 0.20]** |
  | the same, runs off (`BTBH_GPU_SCENE_RUNS=0`) | 1.16 [0.96, 1.44]¹ | 0.45 [0.31, 0.58] |
  | World detail off, interleaved, distinct meshes: v2's "as it is" | 1.45 [1.22, 1.69] | 1.26 [1.06, 1.51] |
  | the same, one mesh for every ring batch | 0.81 [0.66, 0.98] | 0.58 [0.52, 0.68] |
  | the same, in the shared buffers | 1.46 [1.24, 1.73]¹ | 0.59 [0.53, 0.67] |
  | World detail off, sorted, in the shared buffers | 1.08 [0.96, 1.38]¹ | **0.11 [0.09, 0.15]** |

  ¹ 0.9.0 has no flag: these windows draw the distinct meshes with buffers of their own (and runs off means nothing),
  so they repeat the row above them and show the noise.
- **The frame without the ring** (944 batches of one material): 0.549 → 0.136 ms, **0.58 → 0.14 µs a batch**; with
  runs off 0.380 ms (0.40 µs). Every game's batches that run by material gain so, without changing anything.
- **What each part gives** (0.10.0, world detail on): runs 0.45 → 0.14 µs a ring batch (sorted, shared); the shared
  buffers 0.52 → 0.14 (sorted) and 2.01 → 1.38 (interleaved); the frame's uniforms once a pass and the vertex buffer
  bound when it changes about 0.18 µs a batch that doesn't run (the frame without the ring, runs off, against 0.9.0).
- **The interleave.** Sorting by vertex shader takes the shared ring from 1.38 to 0.14 µs a batch with world detail
  on (0.9.0: 2.19 to 1.19). Without world detail, where the wall and the glass alternate only their materials, 0.59
  against 0.11: the pipeline switch costs about 0.8 µs a batch on top of a material change here, more than v2's
  0.25–0.55 µs. The renderer can't take it away without changing what the GPU runs, so the host's order does
  (Decisions, "Implementation notes").
- **Against v2's figures.** 0.9.0 here costs 1.5–2 times v2's numbers (2.19 and 1.45 against 1.31 and 0.76 µs; one
  mesh 1.49 and 0.81 against 0.64 and 0.39): another frame, another load. The order agrees. Scaled to v2's machine,
  the sorted shared ring would cost about 0.1 µs a batch, some 0.02–0.03 ms for the 257 batches against the 0.5 ms v2
  measured; v2's own measurement will tell.
- **Prepare and shadow encode** a ring batch: 0.03 and 0.01 µs in both builds (the ring casts no sun shadow); the
  present call's wall clock follows the geometry encode (sorted, shared, world detail on: 1.17 → 0.20 µs).
- **Contended.** A first pair run beside four `v2-game` processes measured 1.5–3 times these, in the same order (for
  example 2.64 → 0.31 µs sorted and shared with world detail on).
- **The GPU's time** isn't measured: beside three to five `v2-game` processes the cost log's opaque pass ran at
  2.5–2.8 ms instead of about 1.1, and an in-process comparison of runs and shared kit pieces on and off (the bench's
  `table+noruns`, `table+arena`) kept 4 of 64 rounds, with intervals of ±0.3 ms. The GPU gets the same draws and the
  same bytes (the identity runs), so none is expected.

- **Not done:** Metal (`check.sh` on the Mac), the Vulkan validation layers (not installed; SDL's debug device ran the
  smokes), the Deck, v2's own frames at the map edge.

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

## Phase 2 refreshed for 0.16.1 (2026-11-28)

"Phase 2" above was written against 0.8.0. v2 asked for draw lists again after its perf round of 2026-11-27 at the
Deck preset. This section restates phase 2 for 0.16.1: the layouts, how a list meets what came after 0.8.0, what SDL
3.4.2 does with it, the refusals, the tests, a measured baseline and the cost. Where it and the 0.8.0 text differ, this
section holds. Nothing here is implemented.

**Sources.** master `960dc17c` (v0.16.1); line references are that commit's unless they say otherwise. Short names:
`scene.c` and `internal.h` for `src/gpu/btbh_gpu_scene.c` and `src/gpu/btbh_gpu_internal.h`, `btbh_gpu.h` for
`include/btbh_gpu/btbh_gpu.h`, `order_test.c` for `Tests/btbh_gpu_scene_order_test.c`. SDL is `external/SDL` at
`release-3.4.2` (`683181b47`): `vulkan.c`, `metal.m` and `d3d12.c` for the backends under `src/gpu/`, `SDL_gpu.h` and
`SDL_gpu.c`. v2's code is BTBH `a96c846af` (`main`), paths under `v2/crates/`.

### v2's case

From v2's perf round of 2026-11-27 (the Deck preset, 1280 x 800):

- **Kit chunks are 62–88 % of the main draws:** 850–970 of 1,125–1,245 in a stage-3 fight, 2,079 of 2,375 on the
  title, with 17–23 materials a frame (37–40 at 4K and on the title).
- **Props are 83–188 draws,** each also a sun-shadow draw: 75–85 % of the 100–230 shadow draws.
- **Both are custom meshes in the shared geometry buffers** (phase 1a; `Gpu::shared_meshes`,
  `v2-platform/src/gpu.rs:1348-1357`), over persistent instance buffers, one a chunk (`v2-render/src/props.rs:1-3`,
  `v2-render/src/kitruns.rs:1-17`). v2's custom-mesh batches carry `material_slot` -1
  (`v2-platform/src/gpu.rs:3090-3101`).
- **A chunk's draws are sorted into material runs over its instance buffer;** inside a run only the mesh changes. A run
  is what a list holds.
- **The ask:** an indirect draw a run, turning the 600–2,100 kit draws into about 50–100 calls. v2 put the gain at
  0.2–0.5 ms a frame (RFC 0015, design (d)).
- **Decision 4 stands:** a list per chunk, LOD and material. What changed is how often a list changes: where the stage's
  look drifts, about half the chunk-frames re-sort their runs (`kitruns.rs:11-13`). The renderer reads lists during the
  present only and keeps nothing of them, so a list rebuilt every frame costs it what a kept one does.

### What changed since the 0.8.0 text

| 0.8.0 | 0.16.1 | For lists |
| --- | --- | --- |
| The batch is 280 bytes, `world_detail_table` at 272 | 288 bytes: `height_lift` at 280 (RFC 0012, 0.11.0), `flags2` in byte 254 (0.12.0) (`order_test.c:24-38`) | `draw_list` lands at 288; the batch is 296 bytes |
| The run key compares 53 of 59 fields | 55 of 61, with `flags2` and `height_lift` (`scene.c:5660-5722`, `order_test.c:378-440`) | `draw_list` is a seventh field that keeps a run |
| Three sites draw geometry: the geometry passes, the shadow maps, the cascades' sources | Four: the particle transmittance map joined them (RFC 0013 phase 2, 0.14.0; `scene.c:8199-8302`) | Every site draws lists |
| One camera | Scene views (RFC 0014, 0.16.0): each view's visibility and shadow visibility per batch, its own opaque order and, where its matrices differ, its own sun and spot maps; each geometry pass holds the views in turn (`scene.c:12476-13178`) | One set of commands serves every view and pass |
| – | `BTBH_GPU_SCENE_RUNS=verify` (0.13.0) | A list is checked as one batch |
| – | The upload queue in the frame's transfer (0.11.0; `scene.c:11393-11414`) | The commands go before it |
| – | The lazy creation log (0.11.0), the prewarms for targets (0.9.0) and views (0.16.0), the cost log's pass frames and view groups | See "Reports, logs and prewarm" |
| – | The frame context (RFC 0015 phase 1a, 0.16.1) | Nothing; phase 1b would copy lists |

The 0.8.0 text's line references are 0.7.0's: the fast shadow path's instance stream (`btbh_gpu_scene.c:6015-6032`
there) is `scene.c:8574-8591` now. Its SDL references still hold.

### The API

```c
/* One entry of a draw list (RFC 0011 phase 2): a geometry and a range of the batch's instances, drawn with the
 * batch's material. 32 bytes on 64-bit. */
typedef struct BTBHGPUSceneDrawListEntry {
    BTBHGPUModel *model;              /* a model, drawn unanimated; or NULL */
    BTBHGPUCustomMesh *custom_mesh;   /* or a custom mesh made with BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS: one of the
                                       * two is set */
    int32_t material_slot;            /* a model's primitives, as BTBHGPUSceneGeometryBatch.material_slot (below 0:
                                       * all of them); ignored for a custom mesh */
    uint32_t first_instance;          /* in the batch's instance buffer, or the call's transient instances */
    uint32_t instance_count;          /* 0 draws nothing */
    uint32_t reserved;                /* 0 */
} BTBHGPUSceneDrawListEntry;

/* A batch's draw list (RFC 0011 phase 2). 16 bytes on 64-bit. */
typedef struct BTBHGPUSceneDrawList {
    const BTBHGPUSceneDrawListEntry *entries;
    uint32_t entry_count;             /* 1 or more */
    uint32_t flags;                   /* 0 */
} BTBHGPUSceneDrawList;

/* In BTBHGPUSceneGeometryBatch, appended after height_lift: */
    /* NULL: drawn as before. Otherwise the batch draws its list (RFC 0011 phase 2): each entry's geometry over its
     * range of the batch's instances, with the batch's material, in the entries' order, through indirect draws; in
     * every pass as the entries would draw as batches of their own with this material, one after another. kind is
     * then 0, model, custom_mesh, animation_library and animation_pose NULL, and first_instance, instance_count,
     * animation_index and animation_time 0; material_slot is ignored. The visibility bytes, the shadows' flags and the
     * opaque sort take the list as one batch. The host owns the list and its entries; the renderer reads them during
     * the present call only. 296 bytes on 64-bit with it. */
    const BTBHGPUSceneDrawList *draw_list;

/* Defined when BTBHGPUSceneGeometryBatch.draw_list and its structs exist. */
#define BTBH_GPU_SCENE_HAS_DRAW_LISTS 1
```

**Layouts on 64-bit** (phase 2's `order_test.c` asserts them):
- **The batch:** `draw_list` at 288, after `height_lift` at 280 (`order_test.c:37-38`); 296 bytes, no padding added.
  0.16.1's header compiled on its own gives 288 bytes, `height_lift` at 280, `flags2` at 254, `ssao_reserved` at 255.
- **`BTBHGPUSceneDrawListEntry`:** 32 bytes: `model` 0, `custom_mesh` 8, `material_slot` 16, `first_instance` 20,
  `instance_count` 24, `reserved` 28.
- **`BTBHGPUSceneDrawList`:** 16 bytes: `entries` 0, `entry_count` 8, `flags` 12.
- **With question 1's recommendation:** `BTBHGPUSceneReport.draw_call_count` at 556, in the report's tail padding (it
  stays 560 bytes: `upscale_mode` at 552, `upscale_reserved` 553-555), and `BTBHGPUSceneViewReport.draw_call_count`
  at 32 with a reserved word at 36 (32 to 40 bytes).

**The rules:**
- **A list batch's material** is every field but the seven: its instance buffer (or the call's transient instances),
  textures, colours, world detail and table, lift, `dug_by`, `flags` and `flags2`, stencil, blend, cull, surface
  variant, billboard. A list batch may be anything a static direct batch may be: opaque, transparent, refracting, a lit
  or soft particle, lifted, dug, a particle-shadow caster.
- **Each entry** draws a custom mesh, or the primitives of a model that its `material_slot` selects, over its
  instances. A model draws unanimated: entries carry no pose. Ranges may overlap (a kit piece's surfaces over the same
  instances). An entry of 0 instances draws nothing.
- **The order is the entries',** and a model's primitives in theirs: the renderer never reorders a list. In every pass a
  list draws byte for byte as its entries would as consecutive batches with its material, except under the opaque
  sort, which moves a list as one batch.
- **Several batches may point at one list,** each drawing it with its own material.
- **Lifetime:** the host owns lists and entries and may rebuild them every frame or rewrite them between presents; the
  renderer reads them during the present call only and keeps nothing of them.

### Encoding

1. **Validation,** in `btbh_gpu_scene_validate_batches` (`scene.c:5297-5456`) with the rest of the batch, before
   anything is made or drawn: the refusals below. The prewarms validate lists without their ranges, as they skip
   batches' (`check_instances` false, `scene.c:14330`).
2. **A list batch is one geometry slot.** Its kind is 0, not `BTBH_GPU_SCENE_GEOMETRY_MODEL`, so every loop that walks a
   batch's geometries (`kind == MODEL ? primitive_count : 1` with `btbh_gpu_scene_batch_uses_geometry`,
   `scene.c:5642-5651`) counts it once: the bones give it `UINT32_MAX` (`scene.c:6735-6800`), the pipelines' loop
   makes its material's static pipeline (`scene.c:10444-10500`), the views' shader check wants its static vertex shader
   (`scene.c:9468-9485`), and the cursors, `opaque_geometry_base` and the check that a pass walked every geometry
   (`scene.c:13166-13176`) hold as they are. Only the draw changes: the four draw sites call one helper that draws a
   direct geometry as today, or a list's groups.
3. **Commands.** In the prepare, before the transfer is sized, each list batch's entries become
   `SDL_GPUIndexedIndirectDrawCommand`s in a runtime array, one per entry with instances and per geometry:
   `{ index_count, instance_count, first_index, vertex_base, first_instance }` from the geometry (as
   `btbh_gpu_scene_geometry_draw` reads it, `scene.c:6286-6309`) and the entry. Then the batch's **groups**: each
   longest run of consecutive commands whose vertex and index buffers (arena blocks) are the same, split at 65,535
   commands. A group is a vertex buffer, an index buffer, a byte offset and a count; a batch keeps its first group and
   its group count. The models `BTBH_GPU_COST_SKIP_MODEL` names (a cost log measurement, `scene.c:811-822`) get
   instance count 0.
4. **Upload.** The commands follow the instances, bones and motions in the frame's transfer (each of those a multiple of
   16 bytes, so the commands start at a multiple of 4) and come before the upload queue, which then starts at the next
   multiple of 16 after them (`scene.c:11393-11414`). The frame's copy pass copies them into the runtime's indirect
   buffer, with cycling, as it copies the instances (`scene.c:11522-11564`).
   - The buffer has `SDL_GPU_BUFFERUSAGE_INDIRECT` alone and is sized as the instance buffer is
     (`btbh_gpu_scene_buffer_capacity`, `scene.c:5137-5145`: the next power of two from 256 bytes). It is made by the
     first frame with lists and made again only when a frame needs more. The title's 2,300 commands are 46 KB, in
     64 KiB.
   - Each runtime has its own: the window's, the one renders into targets use, each target view's
     (`internal.h:205-212`). It goes with its runtime.
   - A frame without lists writes nothing and makes nothing: its transfer is byte for byte 0.16.1's.
5. **Draw.** The list batch passes the pass's tests and binds and pushes its state as any batch, or continues a run and
   binds nothing. Then, per group: the vertex and index buffers where they changed (the passes' caches), and
   `SDL_DrawGPUIndexedPrimitivesIndirect(pass, indirect_buffer, offset, count)`. Under FSR 3 the motion slot is the
   batch's as before: transient instances' motions are read by the same instance id (`production_model.vert.hlsl:279`),
   so a command's first instance picks its motions too.

### Every pass

| Pass | Where | A list batch |
| --- | --- | --- |
| Opaque, transparent, refraction, each view in turn | `scene.c:12106-13196`; the draw at `:13086-13163` | The batch's tests as today (`scene_visibility`, the view's `visibility`, its phase: `:12633-12639`), its state once, its groups. Transparent and refracting lists keep their entries' order: both APIs draw the commands of a multi-draw in order, so the blending is the batches' |
| The sun's, the secondary and the spot map; a second view's own sun and spot maps | `scene.c:8304-8648`, called at `:11624-11778` | The casters' tests per batch as today (`:8403-8411`: transparency, the call's or the view's shadow visibility byte, `NO_SUN_SHADOW`; for the spot map `spot_shadow_visible` and the view's byte). The slow path binds the instance storage buffer, as now. The fast path (`:8421-8428`: unanimated, undug, a surface variant that keeps the vertex, no billboard) binds the instance stream at offset 0 instead of `first_instance` x 96 (`:8574-8583`), and each command's first instance picks (the direct draw passes 0, `:8590-8591`). Vulkan fetches instance-rate attributes at the instance index, which starts at the first instance: each vertex reads the bytes the direct draw reads, so the map is the same |
| The omni map's six faces | `scene.c:11683-11713` | The same commands, each face |
| The particle transmittance map (and a second view's own) | `scene.c:8199-8302`, `:11810-11837` | A `PARTICLE_SHADOW` list draws its groups after the batch's pushes and binds |
| The cascades' material sources | `scene.c:8759-9087` | As every batch there: drawn when it emits, whatever its visibility, unlifted. Its groups |

### Views, visibility and the opaque sort

- **The visibility bytes stay per batch.** `scene_visibility`, `directional_shadow_visibility` and each view's
  `visibility` and `shadow_visibility` let a list in or keep it out whole. A frame's commands are written once, and
  every view and pass that lets the batch in draws them: the renderer culls no entry, per view or otherwise. A host that
  culls finer submits other lists. v2 culls per batch, each view by its frustum and half-plane
  (`v2-platform/src/views.rs:1-7`); a list batch's box is its entries' boxes over their instances.
- **The views' refusal of a lifted caster** (a lifted batch that a view's `shadow_visibility` would cast) applies to a
  lifted list as to a lifted batch.
- **The opaque sort** moves a batch only with transient instances, depth `LESS` (or `LESS_EQUAL` when asked), the
  stencil off and no `KEEP_ORDER` (`scene.c:6344-6358`). A list batch that qualifies sorts by one key: the mean view
  depth over every entry's instances, each through its entry's geometry's bounds' centre (the direct batch's sum,
  `scene.c:6361-6413`, over the entries). An entry without valid bounds keeps the list in place. Each view sorts its own
  order (`scene.c:11270-11296`). v2's kit and props have persistent buffers and never move, with lists or without.

### Runs, the verify mode and a switch

- **`draw_list` keeps a run,** with `kind`, `model`, `custom_mesh`, `material_slot`, `first_instance` and
  `instance_count`: it changes what is drawn, not what is bound. A list batch then continues a run of direct batches of
  its material, and two lists of one material over one buffer bind once. `btbh_gpu_scene_batch_continues_run` compares
  the other 55 fields (`scene.c:5660-5722`) and doesn't change; `order_test.c` lists the field among those that keep a
  run, and its coverage check (`:457-470`) fails until it does.
- **`BTBH_GPU_SCENE_RUNS=0`:** a list binds its whole state once a pass, as any batch; its groups are still indirect
  draws.
- **`BTBH_GPU_SCENE_RUNS=verify`:** a list that continues a run is checked once a pass, as one batch; its commands bind
  nothing.
- **`BTBH_GPU_SCENE_DRAW_LISTS=0`** (new; read once a present; for measuring and debugging): every list's commands are
  drawn one by one with `SDL_DrawGPUIndexedPrimitives`, with their parameters and in their order, the list's state
  bound once; nothing is uploaded. The frame is byte for byte the same and the CPU pays a direct draw a command, so v2
  can compare lists and draws in its own frames without changing code.

### Reports, logs and prewarm

- **The draw counts of the report and the view reports:** question 1. As recommended there: `primitive_draw_count`
  counts a list's commands, each a mesh drawn, so a frame reads the same with and without lists;
  `prepared_geometry_count` counts the commands too; a new `draw_call_count` counts the direct draws and the indirect
  calls of the opaque, transparent and refraction passes.
- **`view_reports[].instance_count`** adds a list's entries' instances, once a batch in the view, as for a batch.
- **The cost log** (`BTBH_GPU_COST_LOG=1`; `scene.c:879-899`, `:1247-1314`): each command counts as a draw with its
  triangles, in its pass, in the static city's column when the batch's instances are persistent, and in its model's or
  mesh's line; the totals and the per-model lines are those of the entries drawn as batches. The frame line gains
  `main_calls=`, `shadow_calls=` and `rc_calls=` at its end: the SDL draw calls, direct and indirect, which equal the
  draws without lists.
- **The package telemetry** (`src/gpu/btbh_gpu_package_internal.h:10-36`) gains the indirect calls per kind of pass and
  the frame's command count.
- **The lazy creation log:** the indirect buffer's creation and growth are `BUFFER` entries with `prewarm_covers` 0, as
  the buffers that grow with a frame's instances (`btbh_gpu.h:3104-3107`); so is the transfer buffer's growth by the
  commands.
- **The prewarms** (`btbh_gpu_window_scene_prewarm`, `_render_to_target`, `_views`): a list batch makes its material's
  static pipelines, as a direct static batch does; its entries are validated, their ranges not; no command is written
  and no indirect buffer made.
- **RFC 0015 phase 1b** (the frame thread; not built) copies at the submit, by value, what batches point at
  (`docs/rfc/0015-frame-thread.md:129-132`). Lists and their entries join that copy, deduplicated by pointer: 32 bytes
  an entry, 74 KB for the title's 2,300. Or the submit writes the commands itself; that is phase 1b's choice.

### What SDL 3.4.2 does

| Backend | One `SDL_DrawGPUIndexedPrimitivesIndirect(pass, buffer, offset, count)` |
| --- | --- |
| Vulkan with `multiDrawIndirect` (desktop NVIDIA and AMD; RADV on the Deck) | One `vkCmdDrawIndexedIndirect` of `count` commands, stride 20 (`vulkan.c:5438-5445`). SDL enables the feature where the device has it (`vulkan.c:12293-12296`) |
| Vulkan without it | One `vkCmdDrawIndexedIndirect` a command (`vulkan.c:5446-5456`): the driver's work a draw stays; the renderer's and SDL's work a draw goes |
| Vulkan, both | Once a call, not a command: the descriptor sets' bind (`vulkan.c:5436`) and the indirect buffer's tracking (`:5458`), a scan from the end of the buffers the command buffer has used (`TRACK_RESOURCE`, `:2376-2391`). The copy pass tracks the indirect buffer first, so each call scans past every buffer tracked after it (the chunks' instance buffers, the arena blocks, the frame's buffers): tens to a few hundred comparisons a call. Not measured |
| Metal | No multi-draw: a loop of `drawIndexedPrimitives:...indirectBuffer:indirectBufferOffset:`, one a command (`metal.m:2930-2938`), after the resources are bound once (`:2928`). The renderer's and SDL's work a draw goes; the encoder's call a draw stays |
| D3D12 | `ExecuteIndirect` (`d3d12.c:5297-5318`). Not a target: the renderer's shader formats are SPIR-V and metallib (`btbh_gpu.h:25-29`) |

- **Layout.** Tightly packed `SDL_GPUIndexedIndirectDrawCommand`s (`SDL_gpu.h:1566-1573`): 20 bytes, the layout of
  Vulkan's `VkDrawIndexedIndirectCommand` and Metal's `MTLDrawIndexedPrimitivesIndirectArguments`.
- **Alignment.** Vulkan wants the offset a multiple of 4 and, for more than one command, the stride a multiple of 4 and
  at least 20; Metal wants the offset a multiple of 4. Groups start at multiples of 20 bytes from the buffer's start, so
  both hold, and the capacity covers each group's last command.
- **Count.** With `multiDrawIndirect`, Vulkan guarantees `maxDrawIndirectCount` of at least 65,535. SDL doesn't report
  the limit, so the renderer splits a group there.
- **First instance.** SDL asks for `drawIndirectFirstInstance` by default (`vulkan.c:12472`) and passes over a device
  without it unless the host opts out (`vulkan.c:12036`, `SDL_gpu.h:2272-2276`). The renderer sets no feature property
  (`src/gpu/btbh_gpu_device.c:30-70`), so every Vulkan device it runs on honours a command's first instance. Metal's
  indirect arguments carry `baseInstance`.
- **Instance index.** The scene's vertex shaders read `instances[SV_InstanceID]` (`production_model.vert.hlsl:138-140`
  and the others). That includes the first instance on Vulkan (SPIR-V's `InstanceIndex`) and on Metal
  (`[[instance_id]]`), as the direct draws already rely on: they pass `first_instance` to every draw. SDL warns that
  this isn't portable (`SDL_gpu.h:1553-1560`); it fails on D3D12, which the renderer doesn't use.
- **The fast shadow path's stream on Metal.** Whether Metal's per-instance vertex fetch starts at `baseInstance` in an
  indirect draw is what phase 1 left unverified ("Implementation notes (phase 1)"). The Mac session's check decides; if
  it doesn't, list batches take the slow shadow path on Metal (storage-buffer instances, which direct draws use with a
  base instance today).
- **Synchronisation.** The buffer's only usage is `INDIRECT`, so SDL's barrier after the copy pass makes it ready for
  the draw-indirect stage (`vulkan.c:2561-2563`, `:2590-2592`, `:2752-2753`).

### Refusals

`BTBH_GPU_INVALID_ARGUMENT`, the window left as it was, the message naming the batch and the entry ("batch 12, draw list
entry 3: ..."), as `btbh_gpu_scene_refuse_batch` names a batch's field (`scene.c:5463`). In every call that takes
batches: the presents, `present_views`, the renders into targets, the prewarms (without the ranges).
- **The batch:** `draw_list` set with `kind` not 0, `model` or `custom_mesh` set, `first_instance` or `instance_count`
  not 0, `animation_library` or `animation_pose` set, or `animation_index` or `animation_time` not 0.
- **The list:** `entries` NULL, `entry_count` 0, `flags` not 0.
- **An entry:**
  - both or neither of `model` and `custom_mesh`;
  - a geometry of another window;
  - a custom mesh made without `BTBH_GPU_CUSTOM_MESH_SHARED_BUFFERS` (it would gain nothing: its own buffers are a
    group of one);
  - a geometry without indices: a custom mesh made without them, or a model primitive without them (phase 2 draws
    indexed geometry only);
  - a model's `material_slot` at or past its material count;
  - a range past the instances: the instance buffer's uploaded prefix, as for batches (`scene.c:5436-5451`), or the
    call's transient instance count;
  - `reserved` not 0.
- **Not refused:** an entry of 0 instances (nothing drawn, no command); a model entry whose slot selects no primitive
  (nothing drawn); overlapping ranges; one list in several batches; any material a static direct batch may have.

### Measured: what a draw in a run costs now

The frame bench's CPU mode (`BTBH_FRAME_BENCH_CPU=1`) on 0.16.0's core: the reference build of the 0.16.1 release
(`%LOCALAPPDATA%\btbh-wt-0161-ref0160`, built from `btbh-renderer-v0.16.0`). 0.16.1 changed the window queries, the
reflections and the SSAO denoise, not the draw path. Windows, Vulkan, RTX 3070 Ti, under the perf lock, 2026-11-28. Two
processes of three hidden windows, 60 rounds of 8 frames, the first two frames of each turn dropped (360 frames a window
a process); the ring row is a third process of 16 rounds through `tools/frame_bench_cpu.py`. Medians, with the 25th and
75th percentiles; one range per process.

| Window | Geometry encode | Shadow encode | Prepare |
| --- | --- | --- | --- |
| `table+arena`: 2,001 draws of one material (the bench's 32 kit pieces in the shared buffers, transient instances, a world-detail table); each draw after the first continues a run | 0.10 and 0.15 ms [0.09–0.34]: 0.05–0.07 µs a draw at the median, 0.17 at the 75th percentile | 0.13 and 0.17 ms [0.13–0.27]: 0.07–0.09 µs a draw | 0.057 ms: 0.03 µs a batch |
| The same, runs off | 0.62 and 0.74 ms [0.54–1.08]: 0.31–0.37 µs a draw | 0.12 and 0.16 ms | 0.056 ms |
| `ring+table+sorted+arena`: v2's ring of 0.10.0's bench, 257 batches in 8 chunks with persistent buffers, sorted, shared | 0.13 µs a ring batch [0.11, 0.15] | 0.00 (the ring casts no shadow) | 0.03 µs a ring batch |

- **The geometry encode is bimodal** from frame to frame (about 0.09 or 0.33 ms with runs), hence the wide quartiles.
- **What it means.** Inside a run a draw costs about 0.05–0.17 µs of encode on this PC (SDL's draw and the driver's
  record included), and a caster in the sun's map 0.07–0.09 µs. That is what lists take away per entry and pass.
- **Not in these numbers:** the submission. v2's decision 1 found 0.25–0.35 ms of 257 batches' cost in the submit on
  0.8.0, before the shared buffers; nobody has measured it since.

### Cost

**Per frame** (this PC; estimates until the bench below measures them):

| Item | Estimate |
| --- | --- |
| An entry, once a frame (its checks, 20 bytes written and uploaded) | 0.01–0.02 µs |
| An entry, in a pass that draws its list | Nothing more on the CPU |
| A group, in a pass that draws its list | One indirect call: about 0.2–0.4 µs (the vertex and index binds where they change, SDL's descriptor check, the call, the tracking scan) |
| The list batch's state | A batch's: it starts a run or continues one |
| GPU | 20 bytes read a command; the same vertices and pixels. Expected unchanged; measured by the bench |
| Memory | 20 bytes a command, per runtime, in a buffer of the next power of two, times the copies SDL cycles: 64 KiB a copy at the title's 2,300 commands |

**v2's frames** (this PC; the Deck two to three times each):
- **Stage-3 fight:** 900 kit entries in 50–100 lists. The main pass's 0.05–0.15 ms of continuing draws becomes about
  0.01–0.02 ms of entries and 0.01–0.04 ms of calls: about 0.03–0.1 ms less.
- **Title:** 2,079 entries in about 100–150 lists: 0.1–0.35 ms becomes 0.05–0.1 ms: about 0.05–0.25 ms less.
- **Props in the sun's map:** 83–188 casters, about 0.01 ms.
- **Prepare:** 0.03 µs for each batch a list replaces: 0.02–0.06 ms.
- **Two views** draw each list twice: the geometry passes' gain doubles.
- **The submission,** if its share a draw is still there, on top. v2's 0.2–0.5 ms looks reachable on the Deck only with
  it; the bench measures the present call and the submit apart to tell.

**The work:**

| Part | Lines (estimate) | Days |
| --- | --- | --- |
| Header: the structs, the field, the macro, the report fields, their documentation | 120 | 0.25 |
| `scene.c`: validation and the refusals | 150 | 0.5 |
| `scene.c`: commands, groups, the indirect buffer, the upload | 200 | 0.5 |
| `scene.c`: the draw helper in the four sites, the fast path's stream, the sort's depth, the counts, the cost log, the telemetry, `BTBH_GPU_SCENE_DRAW_LISTS=0` | 300 | 0.75–1 |
| `order_test.c` and the public header test | 40 | 0.1 |
| The new smoke (below) | 1,000 | 1–1.5 |
| The smokes' fold mode (below) | 150 | 0.25–0.5 |
| The frame bench's list modes | 350 | 0.5 |
| Identity runs (five modes, two builds), `check.ps1`, the Swift suite, the bench's runs | – | 0.75–1 |
| CHANGELOG, implementation notes | 150 | 0.25 |
| **Total** | **about 2,500** | **about 5–6** |

No shader changes; no Swift change (the Swift layer zero-fills its batches,
`Sources/BTBHRendererGPU/BTBHRendererGPU.swift:2793`). Version 0.17.0: new structs, and the batch grows from 288 to 296
bytes, so every host that takes 0.17.0 regenerates its bindings or rebuilds (v2's `..Default::default()` leaves
`draw_list` NULL; CHIMERA regenerates; Tyrian3D is pinned).

### Test plan

1. **Identity with lists off, byte for byte.** This branch against 0.16.1's build (`960dc17c`): every smoke's images in
   the four modes (plain, `BTBH_SMOKE_SHARED_MESHES=1`, `BTBH_GPU_SCENE_RUNS=0`, runs verified), each also through
   `present_views` with one view (`BTBH_SMOKE_PRESENT_VIEWS=1`): 421 images a mode at 0.16.1, 0 px different, 0
   run-verify mismatches. No smoke sets a list, so this checks that `draw_list` NULL changes nothing.
2. **Lists against batches in every smoke: a fifth mode.** `BTBH_SMOKE_DRAW_LISTS=1`, with the shared meshes, makes the
   smokes' present helper (`Tests/btbh_gpu_smoke_present.h`) fold each longest run of consecutive batches that the run
   key joins (the fields as `order_test.c` lists them) into one list batch: unskinned, indexed, models or shared
   meshes, under the opaque sort only batches it can't move. The images against 0.16.1's: 0 px different; again with
   runs verified (0 mismatches) and with `BTBH_GPU_SCENE_DRAW_LISTS=0`.
3. **`btbh_gpu_scene_draw_lists_smoke`** (new, the 47th test). One scene drawn as batches, as lists and as lists with
   `BTBH_GPU_SCENE_DRAW_LISTS=0`, compared byte for byte:
   - kit chunks with persistent buffers, three materials each, 6–12 entries a list; a model entry of two primitives with
     material slots; overlapping ranges; an entry of 0 instances; transient-instance lists; a list across two arena
     blocks and a group split at its count (a test hook in `internal.h` shrinks the blocks and the split, as the stage
     bench reads internals);
   - through the opaque pass in submitted order and front to back (a movable transient list, against the same list
     expanded), transparent and refracting lists, lit particles; the sun's soft shadow (the fast path, and a billboard
     list on the slow one), the secondary and spot maps, the omni faces, the particle transmittance map, the cascades'
     sources; a lifted list and a list with a world-detail table;
   - two clipped views, each with its own visibility, its own shadow visibility and its own sun map; a render into a
     target and a target view;
   - runs verified, 0 mismatches; the telemetry's calls and commands; the reports' counts and the cost log line's calls;
   - every refusal, each followed by a present that draws as before;
   - a prewarm with lists, after which the lazy creation log names nothing but the indirect buffer.
4. **Native.** `order_test.c`: `offsetof(draw_list)` 288, the batch 296 bytes, the entry's and the list's offsets and
   sizes; `draw_list` among the fields that keep a run.
5. **Bench.** The frame bench gains `+lists` on its ring (each chunk's walls one list, its glass another), comparable
   with phase 1's numbers, and a `kit` mode at v2's Deck-like counts: a stage-3 fight of 960 kit entries in 12 chunks
   of 5–7 materials (about 70 lists, 20 materials a frame) with 150 prop entries casting into the sun's map and 250
   other draws, and a title of 2,080 entries in about 150 lists over 38 materials. Each as batches with runs (today),
   as lists, and as lists expanded; through one view and two. CPU: the report's prepare, shadow and geometry encode,
   the present's wall clock and the canvas present's (the submit); GPU: the cost log's split frames (`main_ms`,
   `shadow_ms`). Interleaved hidden windows, shuffled turns, several processes, under the perf lock, as phase 1 ran.
6. **Elsewhere.** The Mac session's `check.sh` with the new smoke (Metal's loop, and the fast shadow path's stream);
   v2's frames at the Deck preset with lists, and with `BTBH_GPU_SCENE_DRAW_LISTS=0` for the comparison; the Deck
   itself through v2.

### Not in phase 2

- **Geometry without indices** (`SDL_DrawGPUPrimitivesIndirect`, 16-byte commands): when a host asks.
- **Regrouping a list by arena block,** as the 0.8.0 wording could be read: it would reorder the entries, which changes
  blending and depth ties. Groups are consecutive runs instead: a list whose meshes share their blocks is one call.
- **A shadow-only run key.** The shadow passes read few of a batch's fields (the cull mode, the slow path's surface
  uniforms, `dug_by`, the instance buffer) but compare all 55. A key of their own would let consecutive lists of
  different materials over one buffer (a chunk's props) bind once in the shadow maps, and their groups merge. After
  phase 2's numbers.
- **GPU culling and count buffers.** SDL 3.4 has no indirect draw with a count from a buffer; culling stays the host's.
- **Lists kept on the GPU** (an object uploaded once, like an instance buffer): v2's lists change often (`kitruns.rs`),
  and writing them costs microseconds a frame.

### Open questions for v2

1. **What `draw_calls` should count.** With lists, a list batch draws its entries in one indirect call or a few.
   `report.primitive_draw_count`, which v2 logs as `draw_calls` (`v2-platform/src/gpu.rs:1985`, `:1990`, `:2616`) and
   as the scene cost log's `draws` (`:2875`), could count the meshes drawn (each entry, as today) or the calls.
   **Recommended:** keep `primitive_draw_count` and `prepared_geometry_count` counting meshes drawn, so a frame reads
   the same with lists and without, and add `draw_call_count` (the direct draws and the indirect calls of the opaque,
   transparent and refraction passes) to `BTBHGPUSceneReport` (at byte 556, in its tail padding: it stays 560 bytes)
   and to `BTBHGPUSceneViewReport` (at 32, with a reserved word: 32 to 40 bytes, which the regenerated bindings take
   along with the batch's growth). v2 points `draw_calls` at the new field. The cost log keeps counting a draw a mesh
   and adds the calls.
2. **Shadows per entry.** A list casts as a whole: its batch's `NO_SUN_SHADOW` and `spot_shadow_visible`, its
   `directional_shadow_visibility` byte and each view's `shadow_visibility` byte. v2's casting comes from the material
   (`Material::casts_shadow`, `v2-platform/src/gpu.rs:3152`), so a (chunk, LOD, material) list casts or doesn't as
   one. Must some entries of one list stay out of the shadow maps (props past a shadow distance, a shadow LOD)?
   **Recommended:** no; such entries go in a second list with the same material and `NO_SUN_SHADOW`. If yes, the
   entry's `reserved` word takes a flag and each list writes a second command range for the shadow passes: 20 bytes
   more a casting entry, and about a day more with its tests.

### Decisions on the refresh (v2, 2026-11-28)

1. **`draw_call_count`: as recommended, with one change on v2's side.** `primitive_draw_count` and
   `prepared_geometry_count` keep counting meshes drawn. `draw_call_count` is added to `BTBHGPUSceneReport` at byte
   556 (it stays 560 bytes) and to `BTBHGPUSceneViewReport` at 32 with a reserved word (32 to 40 bytes). v2 keeps its
   `draw_calls` / `draws` logging meshes, so old logs and baselines still compare, and adds a separate `calls` field
   from `draw_call_count`. The cost log's `*_calls` fields are fine.
2. **Shadows per entry: no.** A list casts as a whole; entries that must stay out of the shadow maps go in a second
   list with the same material and `NO_SUN_SHADOW`. No per-entry flag.
3. **Order:** the submit cost is measured first (the frame bench's submit timing, a cheap step). If a per-draw submit
   cost is real, phase 2 follows half-resolution SSR; if not, it waits until after RFC 0016's depth prepass. Both come
   after HDR output (RFC 0017), the distortion pass (RFC 0018) and volumetrics phase 1 (RFC 0019).
