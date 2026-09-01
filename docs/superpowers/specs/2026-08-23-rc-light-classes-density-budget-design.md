# RC Light Classes and Density Budget Design

Status: implemented and verified

Date: 2026-08-23

Parent design:
`docs/superpowers/specs/2026-08-23-rc-roadmap-1-7-windows-design.md`

## Goal

Keep every logical RC light while preventing authored or transient source piles
from turning into white, order-dependent energy spikes. The renderer must apply
one policy to engine-owned and gameplay-owned sources after they are merged.

## Light classes

- `hero`: the active player/local light selected by the renderer.
- `world`: persistent authored world sources such as shrines and city lights.
- `pickup`: XP, loot, and other collectible presentation sources.
- `transient`: short-lived impacts, attacks, and hazards.

`RadianceEmitter` exposes the class as `lightClass`; the source-facing default is
`world`. The class is also carried through the native ABI and GPU source record
so later debug views and transport policies do not need another ABI change.

## Production policy

Budgeting runs once, after engine and gameplay emitter arrays are merged. It
preserves count, stable IDs, positions, shapes, and order. Only `energy` and
`directEnergy` may be scaled, by the same factor.

The policy uses 1.5-world-unit cells and a 3x3 neighborhood. Each class has its
own per-emitter and neighborhood energy ceiling:

| Class | Emitter ceiling | Neighborhood ceiling |
| --- | ---: | ---: |
| hero | 30 | 45 |
| world | 30 | 90 |
| pickup | 30 | 30 |
| transient | 18 | 30 |

Class neighborhoods are isolated: an XP pile must not dim the hero cone or an
authored shrine. The existing XP-only shaping remains an authoring rule that
keeps dense blue crystal piles near their current nine-point target; the shared
renderer policy is the final cross-source safety budget.

Accumulation is deterministic. Entries are sorted by class, cell, stable ID,
position bits, and original index before floating-point totals are formed. The
result therefore does not depend on dictionary or caller ordering.

## Validation

- Core tests prove per-emitter caps, dense-neighborhood caps, class isolation,
  stable identity/order preservation, matching direct-energy scaling, and
  permutation independence.
- Gameplay tests prove every existing source is assigned to the intended class.
- Native tests prove the new class is validated and reaches the unused fourth
  component of the 64-byte GPU source record without changing its size.
- Release build and focused Windows Vulkan launch remain the final gate.

## Verification result

- Seven focused budget tests pass; 10,000 co-located logical sources are kept
  and budgeted in 0.043 seconds in the unoptimized Debug test build.
- Fourteen RC contract tests, twelve gameplay-light tests, and the native C RC
  validation test pass.
- The production Release build and shader reflection complete successfully.
- Windows bootstrap selects hardware Vulkan. A 180-frame HRC High radiance
  showcase reports 59.593 FPS under vsync, no RC fallback, and one HRC
  reconstruction dispatch per frame.
