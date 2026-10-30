# RFC 0003: Light regions with their own key light and LUT

- **Status:** Draft
- **Requested by:** CHIMERA (`RTS/docs/v2/izgled-mapa.md` §4 and §9.5, branch `v2-izgled`)
- **Affects:** CHIMERA (adopts it). Be The Bullet Hell and Tyrian3D: nothing, opt-in
- **Issue:** to be opened

## Need

CHIMERA's maps have zones with their own mood (warm south-west, cold basin, misty Drain, violet Citadel, soft
12 m edges between them). The renderer has one sun and one LUT per scene, so today the regions are baked into
the ground's vertex colour only: units, props and walls inside a zone do not take its light, and the grade is the
same everywhere.

## Proposal

Appended to `BTBHGPUSceneLighting` and `BTBHGPUScenePost` (zeroed: no regions, today's look):

```c
#define BTBH_GPU_SCENE_MAX_LIGHT_REGIONS 4u

typedef struct BTBHGPUSceneLightRegion {
    /* World XZ rectangle (min x, min z, max x, max z) and the soft edge width outside it. */
    float bounds[4];
    float edge;
    /* Multipliers on the primary directional light (rgb, energy) and on the ambient/sky light (rgb) inside
     * the region; 0 in a channel means 1 (unchanged), so a zeroed region only changes the grade. */
    float key_tint[4];
    float ambient_tint[3];
} BTBHGPUSceneLightRegion;

/* BTBHGPUSceneLighting, appended: */
    uint32_t light_region_count;                     /* 0..BTBH_GPU_SCENE_MAX_LIGHT_REGIONS */
    BTBHGPUSceneLightRegion light_regions[BTBH_GPU_SCENE_MAX_LIGHT_REGIONS];

/* BTBHGPUScenePost, appended: a LUT per region (NULL: the scene's lut_texture there). Outside every
 * region the scene LUT applies. Needs lut_enabled. */
    BTBHGPUTexture *light_region_luts[BTBH_GPU_SCENE_MAX_LIGHT_REGIONS];
```

Weights: each region's weight at a world position is `1 - smoothstep(0, edge, distance outside bounds)`;
overlapping regions are normalised; the remainder is the scene's own light and LUT.

## Implementation

- **Scene shader:** the region weights at the fragment's world position scale the primary directional light's
  colour and energy and the ambient/sky terms. 4 regions x 3 float4 in the scene uniforms; a loop of at most four
  rectangles per pixel, skipped when the count is 0.
- **Grading:** `production_lut.frag.hlsl` gains the depth texture and the inverse view-projection to rebuild the
  world XZ, samples up to five LUTs (scene + four) weighted the same way. Five LUT bindings instead of one;
  only bound when `light_region_count > 0`.
- **Radiance cascades:** the sun term in the cascades stays global in this RFC (regions change only direct light
  and grade). Noted as an open question.

## Compatibility

- **Versioning:** minor (appended fields, zero = off, a new optional binding in the LUT pass).
- **Be The Bullet Hell / Tyrian3D:** nothing to change; zeroed structs have no regions. The LUT pass binds the
  extra textures only with regions, so their passes are unchanged.
- **CHIMERA:** move the regions from `map.json` into `light_regions` and drop the baked tint from the ground's
  vertex colour (or keep a weaker one).

## Platforms and testing

Validation test (count, finite bounds, edge >= 0); readback test with two regions and two LUTs; CHIMERA's zone
borders before/after on Windows; Metal via CI build plus a hand check on the Mac when a game there adopts it.

## Alternatives

- Several directional lights: the shadow cost scales with them; regions only tint the one sun.
- Region grading in the game (render to texture, grade in the host): loses the HDR chain.

## Open questions

- Should regions also tint the radiance cascades' sky and sun?
- Circles as well as rectangles? (CHIMERA's regions are soft rectangles today.)
