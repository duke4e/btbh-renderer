# Phase 3–4 Windows game integration implementation plan

Status: automatski odobren za izvedbu

Design:
`docs/superpowers/specs/2026-07-26-phase-3-4-windows-game-integration-design.md`

## Goal and acceptance

Produce a real release `Main.exe` that runs every game/tool mode through the
production SDL_GPU backend on exact Windows/Vulkan, then compare its visual
and performance results with the retained legacy authority. The isolated
Phase 2 spike remains a regression test but is not acceptance evidence for
this plan.

## Task 1 — Add opt-in production GPU package targets

Renderer files:

- `Package.swift`
- `Sources/BTBHRendererGPUCore/**`
- `Sources/BTBHRendererGPU/**`
- `Tests/BTBHRendererGPUTests/**`

Work:

- Add separate GPU core and Swift adapter products so the base renderer stays
  SDL-free.
- Compile the shared device, window, resource and frame primitives into the
  GPU core.
- Define an opaque production runtime ABI with structured backend identity,
  capability, window state and shutdown reports.
- Add exact Vulkan/Metal request mapping and reject silent fallback.

Verification:

- Base `swift test` remains green without building the GPU product.
- GPU package tests create/shut down a real Vulkan device and window on
  Windows.
- Header/API scans reject backend type leakage.

## Task 2 — Integrate explicit backend selection into the game

Game files:

- `Package.swift`
- `Sources/Main/main.swift`
- `Sources/Main/Sources/Renderer/RenderBootstrap.swift`
- `Sources/Main/Sources/Renderer/GPU/**`
- `Tests/MainTests/RenderBackendSelectionTests.swift`

Work:

- Parse `BTBH_RENDER_BACKEND=legacy|gpu`.
- Install one coherent legacy or GPU bundle; unknown values fail.
- Emit project backend and actual driver in startup/capture diagnostics.
- Keep legacy as the temporary default and prove GPU runs never instantiate
  the legacy bundle.

Verification:

- Selection, unknown-value, no-fallback and exactly-once shutdown tests pass.
- A GPU bootstrap smoke opens, presents, resizes and closes a Vulkan window.

## Task 3 — Port main-game resource and scene features

Renderer/GPU areas:

- persistent texture/mesh/material/model stores;
- Assimp-backed model import and metadata;
- instance buffers and partial uploads;
- lights, shadow atlas, G-buffer, lighting, sky/IBL and transparent passes;
- animation library/player/tree and bone storage uploads;
- used surface and post-chain variants;
- frame readback and world-frame texture capture.

Work:

- Implement every resource and `SceneDraw` operation exercised by the main
  game.
- Use generation-checked handles and fence-keyed deferred destruction.
- Preserve deterministic scene/camera/environment semantics and project math.
- Reject unsupported operations during parity development.

Verification:

- Focused lifecycle, stale-handle, animation, instance, resize and readback
  tests pass.
- Main game runs a canonical GPU capture without legacy renderer calls.

## Task 4 — Port Canvas2D and platform behavior

Work:

- Implement SDL event/input/time/cursor/clipboard/file-dialog behavior.
- Batch all used Canvas2D primitives, blend modes, scissor and render targets.
- Add texture quads, font atlas/rasterization, deterministic measurement and
  text rendering.
- Composite UI after 3D/post and support presented-frame readback/PNG.

Verification:

- Existing platform/canvas routing contracts pass against the GPU bundle.
- Game UI, cards, HUD, text and level-up snapshots pass cue comparisons.

## Task 5 — Close all four tool modes

Work:

- Run Combat Lab, particle editor, city-block editor and unified city through
  the same GPU bundle.
- Implement any remaining command/resource variants found by failing mode
  audits; do not add tool-specific legacy fallback.
- Verify resize/high-DPI, file dialogs, asset churn and shutdown.

Verification:

- All five launch modes complete deterministic automated captures with exact
  Vulkan identity and zero unsupported-operation diagnostics.

## Task 6 — Build, benchmark and compare the real executable

Files:

- `tools/build-gpu-game.ps1`
- `tools/capture-gpu-game.ps1`
- `docs/rendering/phase-3-4-windows-vulkan-report.md`

Work:

- Build and stage a release GPU `Main.exe`.
- Run two warmed 1920x1080 series per mode with retained seeds/entities.
- Record image hashes, cue data, median/p95/worst presented-frame time, CPU
  submit and fence-completion values.
- Compare against Windows Run 3/4 and approved platform-local goldens.
- Diagnose any median/p95 regression above 5% without lowering thresholds.

Verification:

- Real `Main.exe` selects GPU/Vulkan, not legacy/OpenGL.
- All ten captures, performance reports, visual comparisons and shutdown
  reports pass.

## Task 7 — Full closeout

Work:

- Run renderer and game full test suites, release builds, API audits and
  `git diff --check`.
- Request one independent final review and fix important findings.
- Commit/push renderer first, then update the game submodule gitlink and
  commit/push the game branch.

Remaining after this plan:

- repeat the production bundle gate on native macOS/Metal;
- only after cross-platform parity, make GPU the default and later remove the
  legacy OpenGL implementation.

## Risks

- Feature surface is large: close coherent slices in dependency order and
  keep unsupported operations loud.
- SwiftPM/CMake drift: one checked-in build script owns shader/core/game
  ordering and the staged executable manifest.
- Accidental legacy evidence: backend identity and a legacy-construction audit
  are mandatory in every GPU capture.
- Temporary dual backends can diverge: no new game feature is accepted without
  routing through the project seam and both selectable implementations until
  removal.
