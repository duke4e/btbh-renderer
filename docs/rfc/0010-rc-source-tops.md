# RFC 0010: Radiance-cascade sources with a top

- **Status:** Accepted on 2026-11-23 for 0.10.0 (the owner left the decision to the coordinator). The wall falloff
  v2 prefers (high floors take less light, answer 5) isn't blocking and is deferred to phase 2. See "Decisions".
  Phase 1 is released in v0.10.0: see "Implementation notes"
- **Requested by:** Be The Bullet Hell v2 (its session, 2026-11-22, after the owner's sixth play-test)
- **Affects:** Be The Bullet Hell v2 (adopts it). Be The Bullet Hell (v1, Swift) and Tyrian3D use the radiance
  cascades but don't set it, so nothing changes for them. CHIMERA doesn't use the cascades. All of them rebuild the
  radiance-cascade shaders with the C core
- **Issue:** none. v2 answered all six questions through its session on 2026-11-23 (see "Decisions")
- **Depends on:** nothing. It is written against 0.8.0's layout (`feature/0.8.0-detail-table`, unreleased), but it
  touches no struct 0.8.0 changes: the emitter and the cascades' configuration are as in 0.7.0. It lands in 0.10.0,
  on 0.9.0 and RFC 0011 phase 1, neither of which changes those two structs (Decisions, 6)
- **Related, no RFC (in 0.9.0):** a per-batch opt-out from the radiance-cascade sources
  (`BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE`), and HBAO mode 1 under additive cascades. Both are small additive changes v2
  asked for in the same play-test

## Need

v2's request, verbatim:

> "A source is a column with no height, so a street glow lights the roofs around it as strongly as the street. A
> height per emitter (sources stop rays only below their top) would keep street glows in the street canyons."

How v2 uses the cascades (v2 paths below are under `Be-The-Bullet-Hell/v2/crates/`, at BTBH `dec1a3b7`):

- **Additive, for glows.** `additive` is always 1 (`v2-platform/src/radiance.rs:14-16`). By day the glow barely
  shows; at dusk and at night it carries (`v2-game/src/glow.rs:11-14`).
- **The street glows are material sources.** "The city's glows (props' lamp heads, the kit's glows, power lines)
  light the field through their own emission" (`v2-game/src/glow.rs:83`). Those are lit batches with an emission,
  which the renderer always draws into the field (`v2-render/src/radiance.rs:26-27`). v2's explicit lamp emitters
  are off.
- **Unlit draws are receivers.** v2 sets `rc_receiver` on every unlit draw that isn't flagged as a source
  (`v2-platform/src/gpu.rs:1832-1837`), so the night's pools and ground marks don't glow into the field.
- **The gameplay glows are emitters.** Shots at 0.55 units plus their arc, blasts, gems, fires at a roof's top plus
  0.4 (`v2-render/src/radiance.rs`). Emitter `y` is "kept for the record" there: the renderer ignores it.
- **The field.** Low: 128² texels of one cell. High: 256² texels of half a cell, traced at twice that
  (`v2-render/src/radiance.rs:85-86`). One world unit is one cell; a storey is 0.35 units, a lamp head 0.6 up.
- **The occluders.** v2 writes each texel's signed distance to the nearest column and that column's top
  (`CityLayer::top_at`). Columns under 0.1 units are left out.

A lamp head at 0.6 next to a two-storey house (roof at 0.7) lights the house's roof as much as the pavement under
it. In the play-test the roofs round every lit street glowed.

## What the code does today

### The field is two textures

| Texture | Format | Written by | Holds |
| --- | --- | --- | --- |
| Source field | RGBA16F, the field's size | the emitter pass (blend ADD, `src/gpu/btbh_gpu_radiance_cascades.c:904-905`), then the material-source pass (blend MAX, `:998-999`) | rgb the light times its coverage, a the coverage. No height |
| Occlusion | RG32F, the host's samples (`:1231`) | uploaded from `occlusion_cascades[0].samples` | x the signed distance to the nearest occluder (negative inside), y its top |

- **Emitters** are drawn from above as a disk, a square or a segment (`shaders/gpu/production_rc_source.vert.hlsl`).
  `position_y` isn't read there; only the direct-light pass reads it, as the light's height
  (`production_rc_direct_light.vert.hlsl:82`). `height` is a segment's width there (`production_rc_source.vert.hlsl:51-57`),
  which is how v2 sets it (`v2-platform/src/radiance.rs:168`), and the direct light's range
  (`production_rc_direct_light.vert.hlsl:44`). The Swift layer documents it as a "vertical source extent"
  (`Sources/BTBHRenderer/RenderResources.swift:987-989`); no shader uses it that way.
- **Material sources** are every batch that is unlit and no receiver, or lit with an emission
  (`btbh_gpu_scene_encode_rc_material_sources`, `src/gpu/btbh_gpu_scene.c:6190`, test at `:6254-6260`). The
  vertex shader projects them straight down (`production_model.vert.hlsl` with `BTBH_RC_SOURCE_PROJECTION`); the
  fragment shader writes their light (`production_rc_material_source.frag.hlsl:47-64`). The fragment knows its
  world height, but nothing keeps it.

### How a ray meets them

The trace (`production_hrc_trace.comp.hlsl`) and the finish pass (`production_hrc_finish.comp.hlsl`) march each ray
piece over both textures:

- **The piece's start height** is the top of the column it starts in, or 0 outside a column (`trace:159`,
  `finish:162-166`).
- **Occluders:** the ray rises 0.65 units per unit it travels from that height (`trace:193`). A texel within 0.55
  texels of a column whose top stands more than a quarter texel above the ray stops it (`trace:196`).
- **Sources:** every sample with coverage is composited at any height (`trace:201-216`,
  `production_hrc_source_transport.hlsli`). That is the "column with no height".

### Why the field can't follow one ray's height

The holographic cascades share ray pieces between receivers. That is what makes them cheap, and it decides what a
height test can do.

- **Pieces.** Levels 0–2 are traced; higher levels are merged from level-2 pieces
  (`BTBH_GPU_HRC_TRACED_LEVEL_COUNT`, `src/gpu/btbh_gpu_holographic_radiance_cascades.h:9`). A level-0 piece is two
  display cells long: one texel (half a cell) on v2's High, two texels (two cells) on Low.
- **A ray is a chain of pieces.** From the receiver, pieces start at 0, 1, 3, 7, 15 … level-0 lengths
  (`production_hrc_merge_down.comp.hlsl`). The finish pass traces the receiver's own first piece.
- **Each piece starts from its own height.** A piece that starts over the street starts at 0, whatever receiver its
  result later reaches. Today's occluder test has the same limit: a roof's ray that has left the roof is blocked by
  the next house as if it ran at street level.

So a height test on sources works for the pieces that start on a roof. For a roof receiver and a street source:

| Where the source lies | The roof receiver is shielded when |
| --- | --- |
| Within 7 level-0 lengths of the receiver (High 3.5 cells, Low 14) | it is farther from the roof's edge than the source is: the piece that reaches the source starts about half way |
| Farther | the source lies less than about 4 lengths (High 2 cells, Low 8) past the roof's edge: the piece that reaches it starts at most 4 lengths before it |

A lamp half a cell out from a facade then lights only the outer half cell of the roof. A glow in the middle of a
two-cell street lights about the outer cell. The rest of the roof stays dark, and the field's light falls off with
distance anyway, so the edge band is the dimmer part of what roofs show today. Making every receiver's height count
means tracing the field once per height band (Alternatives).

### Why sources can't use the occluders' rising ray

With the occluders' rule (the ray rises 0.65 per unit from the piece's start), a lamp head at 0.6 would be passed
over by every street piece that reaches it from more than about 0.9 units away. The street would lose its own glow
beyond a unit of the lamp. Sources therefore compare their top with the piece's start height, flat: a piece that
starts higher than a source's top passes over it.

## Proposal

One switch on the cascades' configuration, one float on the emitter, a second texture in the source passes, and a
test in the trace.

1. **`source_tops`** in `BTBHGPUSceneRadianceCascades`, in the byte that was `reserved[0]`. 0: as before. 1: every
   source has a top.
2. **A material source's top** is the highest world height of its glowing fragments in each texel. No API: the
   renderer already draws the fragment and knows its height.
3. **An emitter's top** is `position_y + top_above`. `top_above` is a float in the four bytes that were the
   emitter's `reserved`.
4. **The test:** a piece that starts higher than a source's top (plus a twentieth of a texel) passes over it. It
   takes none of its light and isn't dimmed by it.
5. **Walls:** with `source_tops` on, a steep face looks the field up half a texel out from its wall (below).

### Where this differs from the request

- **"A height per emitter" covers material sources too.** v2's street glows are lit batches with an emission, not
  emitters. Their tops come from their geometry, so a lamp head's top is its head's height with no field to set.
- **The top is compared with the piece's start, not with a rising ray** (section "Why sources can't use the
  occluders' rising ray").
- **Roof edges.** The field shields a roof from the street sources whose ray pieces start on the roof. A band along
  each roof's edge about as wide as the source's distance from that edge still takes the light ("Why the field
  can't follow one ray's height"). Open question 3.
- **`height` keeps its meaning.** It is a segment's width and the direct light's range in the shaders, and a
  segment's width in v2. The top gets its own field. The Swift comment is corrected to say what the shaders do.

### API

```c
typedef struct BTBHGPUSceneRadianceEmitter {
    /* ... every field before light_class as in 0.7.0 ... */
    BTBHGPUSceneRadianceEmitterShape shape;
    BTBHGPUSceneRadianceEmitterClass light_class;
    /* With BTBHGPUSceneRadianceCascades.source_tops 1: the source's top stands this far above position_y (world
     * units; finite and at least 0; 0 puts it at position_y). A ray piece that starts higher than the top passes
     * over the source. Ignored, and not checked, with source_tops 0. (RFC 0010, since 0.9.0, in the four bytes
     * that were reserved.) */
    float top_above;
} BTBHGPUSceneRadianceEmitter;

typedef struct BTBHGPUSceneRadianceCascades {
    /* ... as in 0.7.0 up to additive ... */
    uint8_t additive;
    /* 0: as before: every source lights every ray that crosses it, at any height. 1: sources have tops (RFC
     * 0010, since 0.9.0, in the byte that was reserved[0]). A material source's top is the highest world height
     * of its glowing fragments in each field texel; an emitter's is position_y + top_above. A ray piece passes
     * over a source whose top is lower than the height it starts from: the top of the occluder it starts in, or
     * 0 outside one. Steep faces then look the field up half a texel out from their wall. Other values fail the
     * present with BTBH_GPU_INVALID_ARGUMENT. */
    uint8_t source_tops;
    const BTBHGPUSceneRadianceEmitter *emitters;
    BTBHGPUSceneRadianceCascadesOcclusionCascade
        occlusion_cascades[BTBH_GPU_RC_MAX_OCCLUSION_CASCADES];
} BTBHGPUSceneRadianceCascades;

/* Defined when BTBHGPUSceneRadianceCascades.source_tops and BTBHGPUSceneRadianceEmitter.top_above exist (since
   0.9.0). */
#define BTBH_GPU_SCENE_HAS_RC_SOURCE_TOPS 1
```

Layouts on 64-bit; neither struct changes size.

| Struct | Field | Offset | Size | Was |
| --- | --- | --- | --- | --- |
| `BTBHGPUSceneRadianceEmitter` (80 bytes) | `top_above` | 76 | 4 | `uint32_t reserved` |
| `BTBHGPUSceneRadianceCascades` (168 bytes) | `source_tops` | 35 | 1 | `reserved[0]` |

**Zero values.** `source_tops` 0 draws as before, byte for byte, whatever `top_above` holds. With `source_tops` 1,
`top_above` 0 puts an emitter's top at its `position_y`. Material sources need nothing.

**Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, as the other configuration checks in
`btbh_gpu_radiance_cascades_config_valid`):
- `source_tops` above 1;
- with `source_tops` 1, an emitter's `top_above` that isn't finite or is below 0.

`top_above` isn't checked with `source_tops` 0, so a host that left garbage in `reserved` still draws.

### The tops texture

- **Format and size.** R16F at the source field's size: 32 KiB on v2's Low, 128 KiB on High. Cleared to −65504 (the
  lowest half float) by the emitter pass, which already clears the source field (`btbh_gpu_radiance_cascades.c:2599`).
- **Blend MAX in both source passes.** It is a second colour target beside the source field. Several sources in one
  texel keep the highest top, so a mixed texel errs toward today's look.
- **Emitters** write `position_y + top_above` over their footprint (`production_rc_source.frag` gains
  `SV_Target1`). A spot also writes it over its angular footprint's radius (at least 1.5 texels, as
  `production_rc_directional_source.vert` draws it), so its directional light has a top where it lands.
- **Material sources** write their fragment's world height (`production_rc_material_source.frag` gains
  `SV_Target1`, `input.world_position.y`). A discarded fragment (no glow, alpha cut, dug) writes nothing.
- **Precision.** A half float steps by 1/64 unit or less up to 32 units and by one unit at 2048. v2's tops lie under
  10 units.

### The trace

In `production_hrc_trace.comp.hlsl` and `production_hrc_finish.comp.hlsl`, inside the loop:

```hlsl
const float4 source = source_field_texture.SampleLevel(source_field_sampler, uv, 0.0f);
const float4 directed_source = directional_source(uv, angle);
bool passes_over = false;
if (source_tops != 0u && max(source.a, directed_source.a) > 0.001f)
{
    // The source sample is bilinear, so the top is the highest of the four texels it read.
    const float4 tops = source_top_texture.GatherRed(source_top_sampler, uv);
    const float top = max(max(tops.x, tops.y), max(tops.z, tops.w));
    passes_over = receiver_height > top + origin_cell.z * 0.05f;
}
if (!passes_over && hrc_source_composite(source, directed_source, background_luminance, radiance, transmittance))
{
    break;
}
```

- **One gather, only where a source is.** Most samples have no coverage, so most steps pay one compare.
- **The margin is a twentieth of a texel** (0.025 units on v2's High, 0.05 on Low). It keeps a glow at a roof's own
  height lit from that roof despite the half float's rounding. A two-storey roof (0.7) then passes over a lamp head
  (0.6); a one-storey roof (0.35) still takes its light, as it should.
- **Occluders are unchanged.** The rising ray, the 0.55-texel reach and the quarter-texel margin stay as they are.
- **Bindings.** Each shader takes one more sampled texture (t3 in the trace, t4 in the finish pass) and a uniform
  word, `source_tops`, in a padding slot of its block (`HRCTraceUniforms.padding`, and a new word in
  `HRCFinishUniforms`). With `source_tops` 0 the tops texture is still made, cleared and bound, and never read.

### Wall receivers

The resolve reads the field at a receiver's xz (`production_hrc_resolve.comp.hlsl:285`, `:344`). A facade's pixels
lie on its column's edge, so the bilinear lookup mixes the street texel with the column's. The column's texel starts
its pieces at the roof, so with tops it no longer takes the street's glow. Without a change, every facade at street
level would lose up to half of it.

- With `source_tops` on, a receiver whose normal is steeper than 45° (|n.y| < 0.7) looks the field up half a texel
  along its normal's horizontal part. That is the street side of its wall.
- Walls then take what they take today: the street texel's light, up the whole facade. Roofs and level ground are
  read where they are.
- With `source_tops` 0 the lookup doesn't move.

## Minimal example

```c
/* v2: the city's street glows (lamp heads, the kit's emissive batches) get tops from their geometry. */
post.radiance_cascades.source_tops = 1;

/* A shot's glow at its flight height; it lights roofs lower than that, and the street. */
BTBHGPUSceneRadianceEmitter shot = {0};
/* ... position, radius, colour, energy, cutoffs as today ... */
shot.position_y = 0.55f + arc;
shot.top_above = 0.0f;

/* A fire on a roof: its flames reach 0.6 above the roof's top. */
fire.position_y = roof_top;
fire.top_above = 0.6f;
```

In v2's Rust: `Emitter::raw` names every field of the C struct (`v2-platform/src/radiance.rs:183-215`), so
`reserved: 0` becomes `top_above: self.top_above` after the bindings are regenerated, and `Radiance::apply` sets
`rc.source_tops = 1`.

## Implementation

| File | Change |
| --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | The two fields, their comments, the macro. Comments on `height` (a segment's width, the direct light's range) and on `position_y` (the direct light's height, and the base of the top) |
| `src/gpu/btbh_gpu_radiance_cascades.c` | The checks; the R16F tops texture (create, release, clear); a second colour target with MAX blend in the emitter and material-source pipelines; the emitters' tops buffer; the tops texture and the switch in the trace and finish dispatches; the switch in the resolve's uniforms; the name check |
| `src/gpu/btbh_gpu_scene.c` | The material-source pass binds the second target (`btbh_gpu_scene_encode_rc_material_sources`) |
| `shaders/gpu/production_rc_source.vert/.frag` | The tops buffer (a second storage buffer); the emitter's top out to `SV_Target1`; a spot's footprint at least 1.5 texels in that target |
| `shaders/gpu/production_rc_material_source.frag` | `SV_Target1 = world_position.y` |
| `shaders/gpu/production_hrc_trace.comp`, `production_hrc_finish.comp` | The tops texture, the switch, the test above |
| `shaders/gpu/production_hrc_resolve.comp` | The wall lookup, behind the switch |
| `Sources/` (Swift) | Nothing but the corrected comment on `height`. A `RadianceEmitter.topAbove` and a switch can follow when v1 asks |

- **The emitters' tops travel in a buffer of their own.** `BTBHGPUSceneRadianceEmitterGPU` is four `float4`s with
  no free lane (`src/gpu/btbh_gpu_radiance_cascades.c:544-549`), and three shaders declare it
  (`production_rc_source.vert`, `production_rc_directional_source.vert`, `production_rc_direct_light.vert`). A fifth
  `float4` would change its stride under all three. Instead the tops (one float per emitter, in the record's order)
  go in a second structured buffer, uploaded in the same copy, and only `production_rc_source.vert` binds it. The
  record and the other two shaders stay as they are.
- **No new shader file.** Every changed file is already in `CMakeLists.txt`'s list. CHIMERA's and Tyrian3D's
  lists (read from it) and v2's are unchanged.
- **Old shaders.** 0.8.0's RC shaders under the new core draw as before: the extra target, texture and storage
  buffer go unread, and the record is unchanged. They ignore the switch. So a present with `source_tops` 1 checks
  that the trace's SPIR-V names `source_top_texture`, as 0.5.0 checks the scene shader for `dig_mask_texture`
  (`src/gpu/btbh_gpu_scene.c:5692`), and fails with the shader's path otherwise. Metal libraries aren't checked, as
  before.
- **History.** Turning the switch on or off changes the field's light. The temporal history follows over a few
  frames; a host that wants a clean cut sets `invalidate` for that frame.

### Cost

Estimates, to measure with the frame bench (`btbh_gpu_frame_bench`, RFC 0009 phase 2) with the cascades on.

| Item | Estimate |
| --- | --- |
| Tops texture | 32 KiB (Low), 128 KiB (High) |
| Source passes | One more R16F target written per source fragment: a few thousand texels a frame |
| Trace and finish, on | One `GatherRed` per step that crosses a source; one compare per step otherwise. The steps already take three to four samples |
| Trace and finish, off | One compare on a uniform per step. Whether the longer path costs registers is measured, as 0.6.0 measured the world detail's |
| Resolve | A few ALU instructions per receiver with the switch on |
| CPU | One more colour target per source pass, one more sampler per trace and finish dispatch, four bytes per emitter uploaded |

On the bench GPU the cascades' trace and finish are a fraction of a millisecond at High. A gather on the few steps
that meet a source should stay within the run-to-run noise. If the off path shows a cost, the fallback is a define
and two more compute pipelines from the same files.

## Implementation notes (phase 1, 2026-11-23)

Branch `feature/0.10.0-rc-tops`, on `feature/0.10.0-cheaper-draws` (0.9.0 and RFC 0011 phase 1). The API is as drafted;
what differs, and why, is under "Deviations".

### What landed

| Where | What |
| --- | --- |
| `include/btbh_gpu/btbh_gpu.h` | `source_tops` (offset 35, was `reserved[0]`), `top_above` (76, was `reserved`), `BTBH_GPU_SCENE_HAS_RC_SOURCE_TOPS`; the comments on `position_y` and `height` say what the shaders do with them |
| `src/gpu/btbh_gpu_radiance_cascades.c` | The checks in `btbh_gpu_radiance_cascades_config_valid`. The R16F tops texture (the source field's size, made, cleared, written and bound whatever the switch says). A second colour target with MAX in the emitter and material-source pipelines. The emitters' tops in a structured buffer of their own, one float per record (0 with the switch off), from the same transfer buffer in the same copy. The tops texture and the switch in the trace (`padding` became `source_tops`) and the finish (16 bytes appended: 176 to 192); the switch in the resolve (`padding1[0]`) and in the emitter pass's uniforms (a spot's footprint) |
| `src/gpu/btbh_gpu_scene.c` | The material-source pass binds both targets; `btbh_gpu_scene_check_rc_shader` in the present and the prewarm |
| `production_rc_source.vert/.frag` | The tops buffer (t1); the top out to `SV_Target1`; with tops, a spot's quad as the directional pass draws it (at least 1.5 texels) and its top over that disk |
| `production_rc_material_source.frag` | `SV_Target1 = world_position.y` |
| `production_hrc_trace.comp`, `production_hrc_finish.comp` | The tops texture (t3, t4), the test as drafted, and the frame (below) |
| `production_hrc_resolve.comp` | The lookup with tops: the frame, and the wall's half texel |
| `Sources/` | The corrected comment on `RadianceEmitter.height` |

C copies of the shaders' math for the native test (`btbh_gpu_radiance_cascades.h`): `_emitter_top`,
`_piece_passes_over`, `_half_round` (the R16F texture's rounding) and `_tops_lookup_offset`.

### Deviations

1. **Bad values turn the cascades off for the present; they don't fail it.** The RFC said "fail the present with
   `BTBH_GPU_INVALID_ARGUMENT`, as the other configuration checks". The struct's other checks never did that: a
   configuration `btbh_gpu_radiance_cascades_config_valid` refuses leaves the cascades off for that present, with
   `BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION` in the report's status and a warning logged once, and the frame
   draws without them. The new checks join them and behave the same. The stale-shader check does fail the present
   (and the prewarm) with `BTBH_GPU_INVALID_ARGUMENT` and the trace shader's path, as drafted and as the dig mask's
   does.
2. **With tops, the cascades read the field where its texels lie** (not in the RFC). The trace and finish sample the
   field at `(d / linear_scale + 0.5) / size` for display position `d`, half a texel toward +x and +z of where its
   texels lie (the finish's own occluder test samples without the half). The resolve then reads the probe a texel
   toward +x and +z of the pixel. In a smooth field nobody saw it. With tops the field is sharp at every wall, and the
   two directions along each axis came out a texel apart. Measured with the smoke's street, the lamp half a cell out
   from the two-storey facade, before the change:

   | The two-storey house | West of the street | North | East | South |
   | --- | --- | --- | --- | --- |
   | Low: the roof's interior keeps | 0.1 % | 0.2 % | 35 % | 35 % |
   | High: the roof's interior keeps | 0.0 % | 0.0 % | 50 % | 51 % |
   | Low: the street keeps | 100 % | 100 % | 80 % | 79 % |

   So with the switch on the trace and finish drop the half (`frame` 0) and the resolve reads half a texel back. The
   four turns of the street then measure alike to 0.1 % (below). With the switch off the frame is as before, byte for
   byte. Turning tops on therefore moves the field's light half a texel toward -x and -z, everywhere (the header
   says so). Making it every game's default would change every game's look, so it waits for a release that says so.
   The direct-light texture (`direct_energy`, not additive; v2 doesn't use it) is read as before.
3. **The wall lookup is as drafted**, half a texel along the level normal, from the corrected frame. It keeps every
   facade's light at street level as with nothing shielded (100.0 %, the four ways, both tiers).
4. **The occluder field.** v2's field is 0 inside a column and the distance to the nearest inside texel outside, so a
   probe on a wall reads a positive distance and starts at street level. A field that is exactly 0 on its walls (an
   exact signed distance) leaves those probes to a 0.001 nudge in the trace's positions, which differs by direction:
   with one, the first smoke runs lost from a fifth to all of the street's light beside the walls, depending on the
   side. The smoke uses v2's convention; other hosts should too.
5. **The shield is narrower than the RFC estimated** (its table assumed a piece that reaches a source starts about
   half way to it). The smoke's street, the lamp's share of light kept with tops against the same scene with nothing
   shielded (the lamp's hood, a dimmer glow at 2 units over it, gives its texels a top of 2):

   | Lamp (its texels' distance from the roof's edge) | High: band where it keeps half | High: interior (2.5–4 units in) | Low: band | Low: interior |
   | --- | --- | --- | --- | --- |
   | Half a cell (High 0.25 and 0.75; Low 0.5) | 1.5 units | 16 % | 0.4 units | 0 % |
   | A cell (High 0.75 and 1.25; Low 0.5 and 1.5) | the whole roof | 78 % | 0.5 units | 75 % |
   | A cell and a half (High 1.25 and 1.75) | the whole roof | 97 % | – | – |

   - The one-storey roof and the street keep all of it (100.0 %, 99.4–99.5 % on Low's street); so do the facades.
   - Emitters half a cell out: a shot, a rectangle and a spot leave 15.7–15.8 % in the interior, a segment 13.3 %. A
     fire at a one-storey roof's height leaves the same with `top_above` 0 and lights the roof as a column with 0.6.
   - So tops keep a street glow off the roofs it stands next to, and only those: a glow on the far side of a two-cell
     street (a cell and a half from the facade) lights the roof nearly as without tops on High. On Low the field's
     longer pieces shield a lamp half a cell out completely, and one a cell out hardly more than High. The two-band
     field (phase 2) is what would shield whole roofs; v2 decided against its cost (Decisions, 3).
6. A spot's top is drawn over its directional footprint only with tops on, so its quad in the source field is as
   before with the switch off.

### Old shaders, refusals

- 0.9.0's (c5390f0b's) cascade shaders under this core draw the smoke's gallery byte for byte as this release's with
  the switch off; with it on, the present and the prewarm fail naming
  `.../production_hrc_trace.comp.spv`.
- `source_tops` 2 and 255, and with 1 a `top_above` of NaN, ±infinity, −0.01 or −1e−30: the cascades off for that
  present (fallback 7), and the next valid present runs them. −0 and 3.4e38 are accepted. With 0 the same values draw
  as with 0.

### Tests and evidence

- `check.ps1`: 26/26 (with the new smoke), 71 shaders, the headers on their own. The Swift build and the full suite:
  301 tests, 2 skipped, 0 failures.
- `btbh_gpu_radiance_cascades_test`: the offsets (35, 76; 168 and 80 bytes), the refusals, and the C copies with v2's
  heights (half floats of 0.6, 0.735 and 0.385; a two-storey roof passes over a lamp head and a shot on both tiers, a
  one-storey roof and the street don't; a roof's own glow isn't passed over thanks to the margin; a fire with
  `top_above` 0 and 0.6; the lookup's offsets).
- `btbh_gpu_rc_tops_smoke` (new; under 15 s): the numbers above, as bounds; the gallery off; the refusals; the
  prewarm. `BTBH_RC_TOPS_MEASURE=1` prints a failed bound and goes on, and marks the points it measured.
- `btbh_gpu_phase2_shader_test` and one Swift source-text check follow the new bindings and outputs.
- **Identity with the switch off.** A build of c5390f0b with the new smoke overlaid wrote every smoke's images
  (flags 25, prewarm 32, world detail 56, scene target 9, font 25, SDF terrain 1, runs 5, the new smoke's gallery 8
  with garbage in `top_above`); this build's: **161 images, 11,102,080 pixels, 0 differ**, with nothing set, with
  `BTBH_SMOKE_SHARED_MESHES=1` and with `BTBH_GPU_SCENE_RUNS=0`.

### Cost

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

## Compatibility

- **Versioning:** minor (0.9.0). A byte and a float taken from reserved fields, as 0.5.0 took `ssao_reserved[0]` and
  0.3.0 the window descriptor's `reserved`. Neither struct changes size. The RC shaders change and are rebuilt with
  the core; 0.8.0's draw as before under the new core with the switch off.
- **Be The Bullet Hell v2 (Rust, bindgen):** `Radiance::apply` starts from `::default()`, so `source_tops` is 0.
  `Emitter::raw` names `reserved: 0` and stops compiling until it names `top_above` (CHANGELOG, "Upgrading", as
  for 0.5.0's `ssao_reserved`). To adopt: `source_tops = 1`, and each emitter's `top_above`.
- **Be The Bullet Hell (v1, Swift):** nothing changes. `BTBHRendererGPU.swift` fills a zeroed native emitter
  (`nativeEmitter.height = emitter.height`, …, `Sources/BTBHRendererGPU/BTBHRendererGPU.swift:2560`) and never sets
  the new byte.
- **Tyrian3D (C):** nothing changes. It zeroes its structs and is pinned at v0.2.1.
- **CHIMERA:** doesn't use the cascades.
- **0.9.0's per-batch opt-out** is independent: a batch that opts out writes neither light nor top.

## Platforms and testing

- **Native** (`btbh_gpu_radiance_cascades_test`): `offsetof(top_above) == 76`, `sizeof(BTBHGPUSceneRadianceEmitter)
  == 80`, `offsetof(source_tops) == 35`, `sizeof(BTBHGPUSceneRadianceCascades) == 168`; the refusals; `top_above`
  unchecked with the switch off.
- **GPU smoke** (Windows/Vulkan, a small city in the RC gallery of `btbh_gpu_world_detail_smoke` or its own):
  - **Off, byte for byte:** the RC scenes as with 0.8.0's core and shaders, with `top_above` set to garbage.
  - **Street and roof:** a lamp-head source (emissive, 0.6 up) between a two-storey and a one-storey box. With tops,
    the two-storey roof's interior takes none of its light and the one-storey roof takes it; the pavement as without
    tops.
  - **Edge band:** the shielded roof's pixels read back against the table in "Why the field can't follow one ray's
    height", at Low and High.
  - **Walls:** a facade's light at street level with tops within a few percent of without.
  - **Emitters:** a disk at 0.55 with `top_above` 0 against a roof at 0.7 (shielded) and at 0.35 (lit); a fire with
    `top_above` 0.6 on a roof lights the next roof of the same height.
  - **Old shaders:** 0.8.0's RC shaders under the new core draw byte for byte as before with the switch off, and a
    present with it on fails with the trace shader's path.
- **Bench:** the frame bench with the cascades at High, off against on, and the off path against 0.8.0's shaders.
- **Platforms:** Windows/Vulkan runs the smokes. The Mac session runs `check.sh` (metallib: the gather and a second
  target go through shadercross as other shaders' do). Linux and the Deck through v2.
- **Games:** v2's night captures (stages 1 and 3) with and without tops, and its readability measure.

## Alternatives

| Alternative | Why not (now) |
| --- | --- |
| **Rising-ray test for sources** (the occluders' rule) | A lamp head 0.6 up would light the street only within about 0.9 units of its foot |
| **Two height bands** (the field traced twice: from the ground, and from a band height with every piece starting at least that high; the resolve picks by the receiver's height) | Shields whole roofs, edges included. But the trace, both merges and the finish run twice and their atlases double: about twice the cascades' GPU time and memory. Phase 2, if v2's captures show the edge band |
| **A fade at the receiver** (the resolve dims a receiver standing above the highest source top near it) | A tall source farther off (a roof fire) would be dimmed with the street's glows, and "near" has no right radius |
| **Reuse `height` as the top** | It is a segment's width in the shader and in v2 (`Emitter::segment`). Changing it would change every segment emitter |
| **A top per batch** (a field in `BTBHGPUSceneGeometryBatch`) | The fragment already knows its height. A batch field would grow the batch (280 bytes on 0.8.0) for less |
| **The top in the source field's alpha** | Alpha is the coverage the composite reads, and the two passes blend it with ADD and MAX |
| **Only the per-batch opt-out (0.9.0)** | It takes a glow out of the field everywhere. v2 wants its street glows to light the street, not to vanish |
| **The direct-light field** (`direct_energy`) | It assumes receivers at street level (`production_rc_direct_light.frag`) and isn't used in additive mode |

## Open questions for v2

1. **Which glows.** From your code, the street glows are lit batches with an emission (lamp heads, the kit's glows,
   power lines), and unlit draws are receivers. Is that the set you mean? Will the 0.9.0 opt-out take any of them out
   of the field entirely?
2. **The flat test.** A piece passes over a source when it starts higher than the source's top plus a twentieth of
   a texel. So a two-storey roof (0.7) passes over a lamp head (0.6), and a one-storey roof (0.35) takes its light.
   Is that the line you want?
3. **The edge band.** A band along each roof's edge, about as wide as the glow's distance from that edge, keeps the
   light (half a cell for a lamp by a facade). Is that acceptable, or do you need the two-band field at about twice
   the cascades' cost?
4. **Gameplay emitters.** With tops on, a shot at 0.55 lights roofs lower than that and not higher ones; a fire's
   `top_above` decides how far round the roofs it reaches. Should your gameplay glows have tops, or stay columns
   (`top_above` large)?
5. **Walls.** With tops on, steep faces read the field half a texel out from their wall, so a facade keeps the
   street's glow up its whole height, as today. Is that right, or should high floors take less?
6. **Release.** 0.9.0, with the opt-out and HBAO mode 1, in one rebuild of the RC shaders?

All six are answered: see "Decisions".

## Decisions (v2, 2026-11-23)

v2 answered all six questions on 2026-11-23, through its lighting agent, measured at 4K at night with gameplay off. The
owner left the decision to the coordinator, who accepted phase 1 for 0.10.0. The wall falloff v2 prefers (answer 5)
isn't blocking and is deferred to phase 2.

1. **Which glows: the set the RFC read.**
   - The material sources are:
     - the props' lamp heads (0.1 amber times the window glow);
     - the kit's glow meshes and window panes;
     - the power lines (0.35 by day, 1.2 at night);
     - the resource-coloured buildings, roof gear and car lights;
     - enemy bodies and the kaiju.
   - No unlit draw is a source.
   - Share of the field at night:

     | Source | Share |
     | --- | --- |
     | Lamp heads | 57% |
     | Roof gear | 9% |
     | Lit windows | 6% |
     | Resource buildings | 4% |
     | Other kit glows and power lines | 11% |
     | Cars | under 1% |

     At dusk the kit's glows and the power lines are about 70%. Enemy bodies are 3% of a fight frame.
   - With 0.9.0's `BTBH_GPU_SCENE_BATCH_NO_RC_SOURCE` v2 takes out the lit panes, the props' lamp heads, the power
     lines, the traffic, ambulance and boat lights, enemy bodies, boss parts, the kaiju's plates and rim, and the
     cocoon. The lamps come back as explicit emitters. It keeps the kit's glow meshes, the resource buildings' surfaces
     and the roof gear.
   - With tops, the lamp heads could stay material sources: a lamp head's top is its head's height, with nothing to
     set.
   - **What it means:** tops serve both kinds of source. The kit's glow meshes, the resource buildings and the roof
     gear take their tops from their geometry. Lamps drawn as emitters take `position_y + top_above`.
2. **The flat test is the right line.**
   - A storey is 0.35 and a roof's top is floors × 0.35 + 0.035: 0.385 for one storey, 0.735 for two. A lamp head is
     at 0.6.
   - With the margin of a twentieth of a texel (0.025 on High, 0.05 on Low), a two-storey roof (0.735) passes over a
     lamp head and a one-storey roof (0.385) takes its light.
   - One-storey mass cells by stage: 21%, 13%, 10%, 18% and 10% on stages 1–5. Those roofs stay lit, as they should.
   - The tests use these heights.
3. **The edge band is acceptable.**
   - It reads as a street light's rim on the roof.
   - Glow already costs about +0.46 ms on the Deck and about +1.7 ms at 4K, so the two-band field, at about twice the
     cascades' cost, isn't worth it. Phase 2's two bands aren't planned.
4. **Gameplay emitters get tops.**

   | Emitter | `top_above` |
   | --- | --- |
   | Shots, beams, arcs, drones, XP | 0: their top is their own height |
   | Blasts | about their radius (0.6–3 cells) |
   | Fires (`position_y` at the roof's top) | 0.6 |
   | Roof gear | 0.3 above its own height |

   None stays a column. A host that wants one sets a large `top_above`.
5. **Walls: high floors should take less light. Deferred to phase 2.**
   - v2 prefers that a facade take a street glow in full up to the source's top + 0.7 (two storeys) and none of it
     above the top + 2.1. Its towers are 5–12 storeys. Not blocking.
   - The resolve knows a wall pixel's height, but not which sources lit the texel it reads: the field holds light, not
     the heights the light came from. A falloff from each source's top needs that height in the field. That means one
     of two things:
     - a light-weighted top carried beside the light through the trace, both merges and the finish, which is one more
       atlas at each;
     - phase 2's two bands, whose upper band is what high floors would read: its pieces start at the band's height and
       pass over the street's glows.
   - A fade from the highest top near the wall would be cheap. But it is the receiver-side fade under "Alternatives":
     a roof fire's light on a tower would fade as a lamp's does, and "near" has no right radius. A fade between two
     heights the host sets would be cheaper still, but it isn't relative to the source's top.
   - So phase 1 keeps the walls as drafted. With tops on, a steep face reads the field half a texel out from its wall
     and keeps the street's glow up its whole height, as today.
6. **Release: one rebuild of the cascades' shaders is fine.**
   - 0.9.0 was cut as a candidate before the answers came, so tops ship in 0.10.0 beside RFC 0011's phase 1.
   - Every "since 0.9.0" in "Proposal" reads "since 0.10.0". The layouts are as the table there gives them: neither
     0.9.0 nor RFC 0011 changes the emitter or the cascades' configuration.

**Also from v2, outside this RFC:**
- v2 now asks for contact AO explicitly (`ssao_contact`, mode 3), so it doesn't need 0.9.0's `KEEP_HBAO`. That flag
  stays for others.
- v2 will turn on `SSR_SURFACE_REFLECTIVITY` once it pins 0.9.0, and retune its reflection strengths.
- With the cascades on at night, the plaza trees near lamps get darker (−5 L at the darkest 1%). Noted for a separate
  look.

Nothing is open for phase 1.

## Phases

1. **Phase 1, source tops (0.10.0; Decisions, 6).** The switch, `top_above`, the tops texture, the test in the trace
   and finish passes, the wall lookup, the checks, the tests and the bench.
2. **Phase 2, only if asked: two height bands, and the wall falloff.** A second set of the cascades' atlases traced
   with every piece starting at least a band height up, and the resolve choosing by the receiver's height. It would
   shield roof edges too, at about twice the cascades' cost, and its upper band is what a facade's high floors
   would read (v2's wall falloff, Decisions, 5). v2 found the edge band acceptable (Decisions, 3) and the falloff
   not blocking, so it isn't planned.
