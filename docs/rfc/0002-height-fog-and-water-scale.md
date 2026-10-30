# RFC 0002: Height fog and water at RTS scale

- **Status:** Draft
- **Requested by:** CHIMERA (`RTS/docs/v2/izgled-mapa.md` §9.3 and §9.4, branch `v2-izgled`)
- **Affects:** CHIMERA (adopts both). Be The Bullet Hell and Tyrian3D: nothing, both are opt-in
- **Issue:** to be opened

Two small, independent, additive changes in one RFC because both are S-sized and touch the same games the same
way. Either can be accepted alone.

## A. Height fog

### Need

Fog today depends only on the distance from the camera (`fog_*`) and the world's edge (`fog_edge_*`). CHIMERA
wants low ground and chasms to read deeper and morning mist over a canal: fog that thickens below a height.
Today it fakes the chasm with three transparent fog sheets.

### Proposal

Appended to `BTBHGPUScenePost` (zeroed: off):

```c
    /* Height fog, in the HDR composite with the distance fog: below fog_height_base (world y) the fog
     * thickens toward fog_height_color by 1 - exp(-density * integral of exp(-(y - base) * falloff)) along
     * the view ray, capped at fog_height_maximum_opacity. The cleared background is not fogged. */
    float fog_height_color[3];
    float fog_height_base;
    float fog_height_falloff;        /* > 0 when enabled: 1/units over which it thins out above base */
    float fog_height_density;
    float fog_height_maximum_opacity;
    uint8_t fog_height_enabled;
    uint8_t fog_height_reserved[3];
```

### Implementation

`production_hdr_composite.frag.hlsl` already rebuilds the world position from depth for the edge fog; the height
fog adds the analytic integral of an exponential density along the camera-to-pixel segment (one `exp` pair, no
marching) and composites after the distance fog. One float4 x 2 in the composite uniforms. The composite runs
only when one of fog, bloom, vignette or height fog is on, as now.

## B. Water at RTS scale

### Need

`BTBH_GPU_SCENE_SURFACE_WATER` is tuned for Be The Bullet Hell's close camera: 7.5 waves per render unit, foam on
the highest crests broken into fine streaks. From CHIMERA's camera 32-50 m away the foam turns into uniform
speckles and the waves alias.

### Proposal

`WATER` reads `surface_parameters0/1` only when `surface_parameters1.w` is 1 (they are unused by `WATER` today, so a
zeroed batch and BTBH's current batches keep the built-in look exactly):

```c
/* BTBH_GPU_SCENE_SURFACE_WATER with surface_parameters1.w = 1:
 *   surface_parameters0.x  waves per render unit (built-in: 7.5)
 *   surface_parameters0.y  foam strength, 0 none ... 1 built-in ... more
 *   surface_parameters0.z  foam coverage: the crest height foam starts at, 0..1 (built-in: 0.72)
 *   surface_parameters0.w  ripple strength (normal tilt), 0..1 (built-in: 1)
 *   surface_parameters1.rgb  linear tint multiplied into the water's colour in the troughs (depth colour);
 *                            0,0,0 means no tint */
```

With the flag set the foam and ripples also fade with the pixel's footprint (`fwidth` of the wave position, as
`PIXEL_WATER` already does), so distant water turns smooth instead of speckled.

### Implementation

A branch in the `WATER` case of `production_model.frag.hlsl`; no uniform layout change (the parameters already
exist per batch).

## Compatibility

- **Versioning:** minor. A: fields appended to `BTBHGPUScenePost`, off when zero. B: new meaning for parameters
  `WATER` ignored, gated by `surface_parameters1.w = 1`.
- **Be The Bullet Hell:** nothing to change. Its Swift layer writes `surface_parameters0/1` for every batch:
  before acceptance, confirm it leaves them zero (or at least `surface_parameters1.w` != 1) for water. Its
  reference images must not change.
- **Tyrian3D:** nothing to change (it uses `GLINTS`/`PIXEL_WATER`, not `WATER`, and no height fog).
- **CHIMERA:** set the new fields; drop its three chasm fog sheets if the height fog replaces them.

## Platforms and testing

Native contract test for the new post fields' validation (finite values, falloff > 0 when enabled, opacity 0..1);
a readback test of the composite with height fog on a two-height scene; CHIMERA's chasm and canal before/after on
Windows; BTBH's water reference unchanged on Windows and macOS.

## Alternatives

- Volumetric fog (froxels): far heavier; the analytic height fog covers the request.
- A separate `RTS_WATER` surface variant: duplicates the water shader; the gated parameters keep one.

## Open questions

- Should the height fog also tint the sky near the horizon below `fog_height_base` (it does not in this draft)?
