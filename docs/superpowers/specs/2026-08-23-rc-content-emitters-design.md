# RC Content Emitters Design

Status: implemented and verified on Windows

Date: 2026-08-23

Parent design:
`docs/superpowers/specs/2026-08-23-rc-roadmap-1-7-windows-design.md`

## Goal

Make RC visibly participate in the authored city and combat language without
introducing a second gameplay lifecycle, a fixed logical-light cap, or legacy
lighting while RC is enabled.

## Ownership and lifecycle

- City window and lamp sources are derived once from authoritative
  `SceneObject` transforms when a map is applied. Destructible owners retain
  their UUID only for live/dead filtering; stable RC IDs are deterministic
  hashes of authored model/transform data so seeded captures remain stable.
- Every active non-area enemy projectile produces one transient source using
  its authoritative projectile ref, position, radius, damage, and presentation
  family. Area projectiles are represented by the hazard path, not duplicated.
- Environmental sources are assembled from the existing authoritative hazard,
  nuke, storm, tornado, sweeping-beam, missile, healing-zone, and relay arrays.
  Sources disappear in the same frame their owning runtime item disappears.
- The existing renderer-level hero/world/pickup/transient density policy remains
  the only generic energy limiter. Assembly never truncates by array order.

## Presentation

- Building windows use restrained warm/cool authored variants and low steady
  world energy. Explicit sidewalk lamps use a stronger warm source.
- Enemy projectiles reuse the same semantic palette family as their rendered
  projectile material.
- Hazards reuse their authored color where one exists, otherwise the exact
  palette color used by their render pass.
- Persistent city and field sources are `world`; moving attacks, projectiles,
  nuke waves, beams, and missiles are `transient`.
- All content energies are normalized to the established 30-point reference
  scale and remain below that per-source peak before the generic density pass.

## Geometry

- City sources consume render-world `SceneObject` transforms directly.
- Gameplay positions are converted once with `cityToWorld3D`.
- Physical source radii derive from visible projectile/hazard footprints and
  are bounded; illumination range remains a consequence of RC transport.
- Sweeping beams use a segment source aligned with their authoritative
  direction. Other content uses disk sources.

## Verification

- Focused tests cover deterministic city authoring, destruction filtering,
  enemy projectile identity/color/lifecycle, hazard class/color/shape mapping,
  and absence of a small logical-source cap.
- The deterministic radiance showcase includes city, projectile, and hazard
  sources and records class counts.
- Production Release build, live Vulkan telemetry, deterministic composed and
  final-indirect captures, and a no-readback profile close roadmap item 6.
