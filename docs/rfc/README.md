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

CHIMERA's map-look note (`RTS/docs/v2/izgled-mapa.md` §9, branch `v2-izgled`) made six proposals. Three became
the RFCs above. The other three need none:

- **Up to 32 shadowless point lights** (§9.2): already in v0.1.0, twice: `lamp_*` (street lamps, up to
  `BTBH_GPU_SCENE_MAX_LAMPS` = 40, through the full BRDF on lit surfaces) and `point_light_count` /
  `point_lights` (up to 32, diffuse on lit surfaces, and they brighten unlit ones). CHIMERA should use
  `lamp_*`. Folding the two into one is a candidate for the next breaking release.
- **Frustum and shadow-cascade culling in the renderer** (§9.6): noted; CHIMERA culls on the CPU today. Worth an
  RFC once a second game asks for it.
