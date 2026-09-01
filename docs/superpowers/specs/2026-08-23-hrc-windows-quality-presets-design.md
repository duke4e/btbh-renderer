# HRC Windows Quality Presets Design

Status: implemented and verified on Windows; native Mac acceptance deferred

Date: 2026-08-23

Parent design:
`docs/superpowers/specs/2026-08-23-rc-roadmap-1-7-windows-design.md`

## Goal

Give Low, High, and Auto distinct, production-usable HRC behavior on Windows,
make HRC the default backend whenever RC is explicitly enabled, and close the
roadmap with repeatable performance and visual evidence. Native Metal execution
remains deferred.

## Locked presets

- **Low:** one-times holographic linear grid, quarter-resolution indirect
  resolve followed by the shared edge-aware reconstruction. It retains the
  complete logical light set, visibility field, temporal history, HDR material
  response, and content language; it reduces transport working set rather than
  deleting lights.
- **High:** two-times holographic linear grid and full-resolution indirect
  resolve followed by the same reconstruction. This is the Windows reference
  quality tier.
- **Auto:** starts at Low, evaluates a 16-sample p95 completion window, records
  only an adjacent pending tier, and applies it only at the existing explicit
  transition boundary. The current hysteresis thresholds remain locked: High
  to Low above 105% of budget, Low to Off above 120%, Low to High below 82%,
  and Off to Low below 70%.

The default game remains RC Off. If a launch explicitly requests Low, High, or
Auto without naming a backend, the game selects HRC. Classic RC remains an
explicit diagnostic selection and the capability fallback.

## Runtime and launcher contract

- HRC scale is selected from the active effective tier and is covered by a
  native contract test.
- A tier/backend generation change continues to invalidate temporal history and
  rebuild incompatible resources once.
- `run-radiance-cascades.cmd` names the HRC backend and uses the one-frame queue
  for consistent Windows quality/performance comparisons.
- The F9 menu labels HRC as the production path and still permits explicit
  Classic comparison.

## Acceptance gates

- Native profile/layout and Auto hysteresis tests pass, including proof that
  Low has a smaller HRC working set than High.
- Game configuration/menu tests prove Off remains the default, explicit RC
  defaults to HRC, Classic remains opt-in, and Auto transition permission stays
  boundary-gated.
- Two warmed 240-frame no-readback runs per Low and High at 1280x720 record
  median, p95, and worst frame time. Both must remain below the 16.67 ms total
  frame target on the Windows reference machine; Low must demonstrate a real
  working-set/performance distinction.
- Deterministic 1920x1080 composed and final-indirect captures for Low and High
  preserve silhouettes, material separation, source colors, occlusion, and UI.
  High should resolve finer gradients; Low may be softer but may not exhibit
  unbounded block artifacts or light leakage.
- Live Vulkan telemetry must report HRC active, no fallback, expected internal
  dimensions, valid temporal history, and phased fresh quadrants.

## Deferred acceptance

Metal shader translation remains build-covered, but native Mac startup,
performance, and screenshots are a separately recorded follow-up and do not
block this Windows closeout.
