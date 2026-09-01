# HRC Production Lighting Lab Design

Status: automatically approved for implementation

Date: 2026-08-24

Parent roadmap:
`docs/superpowers/plans/2026-08-23-rc-roadmap-1-7-windows.md`

## Goal

Turn the existing F9 Radiance Cascades debug menu into a production lighting
authoring surface. Artists and developers must be able to isolate the authored
light hierarchy, tune it live while simulation is paused, and compare repeatable
normal and stress scenes without changing source constants or restarting the
game.

## Lighting controls

The menu has separate `Lighting` and `System` pages so the controls remain
usable at 640x480 and do not turn into one dense wall of sliders.

The Lighting page provides:

- production presets: `Readable`, `Balanced`, and `Neon`;
- an `All`, `Hero`, `World/Env`, `Pickup`, and `Transient` class selector;
- energy, source-size, and saturation controls for the selected class;
- the existing global indirect-intensity control;
- a class-solo toggle and a frame-scoped emitter-freeze toggle;
- live raw/output emitter counts.

`World/Env` deliberately maps to the existing `.world` class. No fifth public
renderer class is introduced merely for the tool.

The System page retains tier, backend, Auto frame budget, and all RC debug
views. Reset restores the launch configuration plus neutral class tuning.

## Runtime behavior

Tuning is applied in the game integration layer before constructing the
renderer descriptor. Every emitter keeps its stable identity, position, shape,
direction, and class. Energy scales both indirect and RC-native direct energy;
source size scales radius and vertical/source extent; saturation preserves alpha
and luminance. Invalid or non-finite input is rejected and every slider has a
documented finite range.

The player cone is a renderer-managed source derived from the gameplay spot
`LightDescriptor`, rather than an entry in the dedicated gameplay emitter
array. The game integration therefore applies the same Hero tuning and solo
policy to that descriptor before renderer submission and includes it in the
displayed source counts. This keeps the class controls authoritative across the
complete light hierarchy without adding a renderer ABI or duplicating the cone
as a second RC emitter.

Default-neutral state forwards the original emitter array without rebuilding
it. Active tuning reuses a retained scratch array. Soloing filters by semantic
class rather than array order. Freeze captures the authoritative input array on
the next descriptor consumption and continues to use that immutable snapshot
until released. Solo changes invalidate temporal history once so removed light
does not linger in the comparison.

## Production presets

- `Readable` lowers broad world/pickup density and color saturation while
  retaining a slightly stronger hero hierarchy.
- `Balanced` is the current authored baseline: neutral class scales and 0.90
  indirect intensity.
- `Neon` adds controlled hero/transient presence and modest saturation without
  returning to the 1.50 global brightness wash.

Presets affect only the live lab state. Shipping defaults and RC-Off behavior
remain unchanged.

## Calibration and acceptance

- Focused tests cover bounds, presets, per-class transforms, direct-energy
  scaling, saturation/luminance behavior, solo, freeze, reset, page layout, and
  edge-triggered history invalidation.
- The existing deterministic `visual` and `radiance` profile scenarios are the
  normal and stress calibration authorities. A small interactive Windows
  launcher selects either scene or a normal playable run and always starts HRC
  High with the Balanced baseline.
- Release Vulkan startup, a deterministic composed capture for each production
  preset, and a no-readback profile verify integration and performance.
- The default tool state must produce byte-for-byte equivalent emitter values.
  RC Off remains allocation/dispatch free.

## Explicit boundary

PBR Neutral currently ignores `tonemapWhite`, and the post chain has no real
multi-pass bloom implementation. This change therefore does not label a white
point or global intensity multiplier as exposure/bloom. Proper scene exposure
and thresholded multi-resolution bloom require a separate renderer post-chain
design and GPU performance gate.
