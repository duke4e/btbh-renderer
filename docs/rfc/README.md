# RFCs

A request for comments is a short design note for a renderer change that affects more than one game or
is hard to undo: a new render pass, a uniform or G-buffer layout change, a new subsystem (terrain), or
anything breaking. Small additive changes do not need one; an issue is enough (see
[`COORDINATION.md`](../../COORDINATION.md)).

## Process

1. Copy [`0000-template.md`](0000-template.md) to `NNNN-short-title.md` with the next free number.
   Any session may draft one; the game that needs the feature usually writes the first version (or
   points at its own design note) and the coordinator turns it into the RFC.
2. Open a GitHub issue that links it, titled `RFC NNNN: <title>`, and name every game it affects.
3. Each affected game's session comments on the issue (agree, or what it needs changed).
4. The coordinator sets the status to **Accepted** and merges the RFC; the implementation follows on a
   branch, opt-in, with a CHANGELOG entry and a release.
5. When it ships, the status becomes **Implemented in vX.Y.Z**. Rejected RFCs stay, marked **Rejected**,
   with the reason.

## Index

| RFC | Title | Status | Requested by |
| --- | --- | --- | --- |
| [0001](0001-layered-terrain.md) | Layered terrain in one pass (texture arrays, splat control, height blend) | Draft | CHIMERA |
| [0002](0002-height-fog-and-water-scale.md) | Height fog; water at RTS scale | Draft | CHIMERA |
| [0003](0003-light-regions.md) | Light regions with their own key light and LUT | Draft | CHIMERA |
| [0004](0004-small-text.md) | Readable small text (oversampled exact-size atlases, kerning, snapping) | Implemented in v0.4.0 (option A) | CHIMERA |
| [0005](0005-screen-space-refraction.md) | Screen-space refraction batches (`BTBH_GPU_SCENE_SURFACE_REFRACT`) | Implemented in v0.1.0 | CHIMERA |
| [0006](0006-scene-render-to-target.md) | A scene into a render target, apart from the window's temporal history; per-batch reactive mask | Implemented in v0.1.1 | CHIMERA |
| [0007](0007-destructible-terrain.md) | Destructible terrain from SDF edits (sparse bricks, meshed patches, dig mask) | Implemented in v0.5.0 (phases 1–3) | Be The Bullet Hell, CHIMERA |
| [0008](0008-world-projected-detail.md) | World-projected detail: a second texture set per batch from the world position; BC textures (phase 2) | Phase 1 implemented in v0.6.0; phase 2 later | Be The Bullet Hell v2 |
| [0009](0009-world-detail-table.md) | A world-detail table chosen per vertex (texture arrays); the facade's street darkening and wetness without its grain (pattern 7) | Implemented in v0.7.0 (phase 1) and v0.8.0 (phase 2) | Be The Bullet Hell v2 |
| [0010](0010-rc-source-tops.md) | Radiance-cascade sources with a top, so street glows stay out of the roofs (`source_tops`, `top_above`) | Phase 1 implemented in v0.10.0; the wall falloff (high floors take less light) deferred to phase 2 | Be The Bullet Hell v2 |
| [0011](0011-cheaper-draws.md) | Cheaper draws: custom meshes in the shared geometry buffers and same-material runs; draw lists through multi-draw indirect (phase 2) | Phase 1 implemented in v0.10.0; phase 2 waits for phase 1's numbers | Be The Bullet Hell v2 |
| [0012](0012-vertex-height-lift.md) | A vertex-stage height lift from a heightfield, per batch, in vertex shader variants of its own (`height_lift`); custom-mesh updates queued into the frame | Phase 1 implemented in v0.11.0; phase 2 only if asked | Be The Bullet Hell v2 |
| [0013](0013-lit-particles.md) | Lit particles: a see-through surface lit per pixel with a normal and thickness map, soft and near fades, a UV rect per instance (`LIT_PARTICLE`, `INSTANCE_UV_RECT`); a transmittance map in sun space (phase 2) | Phase 1 implemented in v0.11.0; phase 2 in v0.14.0 | Be The Bullet Hell v2 |
| [0014](0014-scene-views.md) | Scene views: two cameras in one present, each clipped to a half-plane of the frame (`btbh_gpu_window_scene_present_views`), shared shadows and post, one FSR 3 history | Accepted 2026-11-27; phase 1 next (0.15.0's target views are the interim) | Be The Bullet Hell v2 |
| [0015](0015-frame-thread.md) | A frame thread: the renderer records and submits a frame while the host builds the next (`btbh_gpu_window_frame_thread_start`, `frame_submit`, copies at the call) | Accepted 2026-11-27 (design b); phase 1a after 0.16.0 | Be The Bullet Hell v2 |
| [0017](0017-hdr-output.md) | HDR output: an HDR10 or scRGB swapchain where the display has one, highlights lifted above the SDR look by a gain past the tone map's knee, the UI at paper white (`btbh_gpu_window_set_hdr`); the Steam Machine's Proton/gamescope path researched | Accepted 2026-11-28 (v2's answers); phase 1 next | Be The Bullet Hell v2 |
| [0018](0018-distortion.md) | Screen-space distortion: rings, discs and strips placed in the world for one frame (`btbh_gpu_window_scene_set_distortion`), drawn into a half-size offset field that the HDR composite applies before grading, with FSR 3's reactive mask and the split line handled | Accepted 2026-11-28 (v2's answers); after HDR's phase 1 | Be The Bullet Hell v2 |
| [0019](0019-volumetrics.md) | Lit volumetrics after IO Interactive's Smolder: one froxel grid for fog, height fog, game-written fields (fog of war, gas) and analytic local volumes, lit by the scene's sun, shadow map, sky, lamps and point lights (`btbh_gpu_window_scene_set_volumes`); shared sun-space shadows, RC and the other lights (phase 2); per-pixel hero volumes (phase 3) | Accepted 2026-11-28 for phase 1 (v2; CHIMERA technically, its author's look and pin pending); after HDR and distortion | Be The Bullet Hell v2 and CHIMERA (the owner, 2026-11-28) |
| [0020](0020-cover-and-ice.md) | A cover by facing: snow, sand or mud on whatever faces up, with a noisy edge, from two window layers that each batch picks by `flags2` bits (`btbh_gpu_window_scene_set_cover`), procedural, no pipeline; still ice on the water (`BTBH_GPU_SCENE_SURFACE_ICE`); the base band, wind, mud's halo and per-entry choice in phase 2; items 3-5 assessed | Accepted 2026-11-28 (v2's answers); after distortion, before volumetrics phase 1 | Be The Bullet Hell v2 |

CHIMERA's map-look note (`RTS/docs/v2/izgled-mapa.md` §9, branch `v2-izgled`) made six proposals. Three became
the RFCs above. The other three need none:

- **Up to 32 shadowless point lights** (§9.2): already in v0.1.0, twice: `lamp_*` (street lamps, up to
  `BTBH_GPU_SCENE_MAX_LAMPS` = 40, through the full BRDF on lit surfaces) and `point_light_count` /
  `point_lights` (up to 32, diffuse on lit surfaces, and they brighten unlit ones). CHIMERA should use
  `lamp_*`. Folding the two into one is a candidate for the next breaking release.
- **Frustum and shadow-cascade culling in the renderer** (§9.6): noted; CHIMERA culls on the CPU today. Worth an
  RFC once a second game asks for it.
