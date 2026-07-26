# Phase 3–4 Windows game integration design

Status: automatski odobren za izvedbu

Date: 2026-07-26

## Goal

Move the real Be The Bullet Hell executable from the legacy
raylib/r3d/OpenGL implementation to the SDL_GPU renderer on Windows/Vulkan,
while keeping an explicit legacy A/B path until the Windows and macOS gates
both pass.

This milestone is not complete when only a spike, test window, or isolated
scene renders. The same release `Main.exe` must launch the game, Combat Lab,
particle editor, city-block editor and unified-city mode through the GPU
backend, with real assets, UI, input, resize, readback and clean shutdown.

## Backend selection and failure policy

`BTBH_RENDER_BACKEND` is the only runtime selector:

- `legacy` installs the existing `LegacyRenderServices` bundle;
- `gpu` installs the production `BTBHRendererGPU` bundle;
- an absent value remains `legacy` until all Phase 3–5 cross-platform gates
  pass;
- any other value fails startup with a clear diagnostic.

On Windows, `gpu` requests exactly Vulkan. On macOS, it requests exactly
Metal. A failed GPU request never falls back to D3D12, OpenGL, or the legacy
bundle.

The executable and capture reports always print the selected project backend
and actual SDL_GPU driver, preventing a legacy run from being presented as a
Vulkan result.

## Package and module boundary

The renderer repository gains two opt-in SwiftPM products:

1. `BTBHRendererGPUCore`, a C/C++ target containing the opaque production GPU,
   platform, asset, frame and readback ABI. It compiles the shared SDL_GPU
   device/window/resource/frame implementation already exercised by Phase 2.
2. `BTBHRendererGPU`, a Swift target depending on `BTBHRenderer` and
   `BTBHRendererGPUCore`. It implements `RenderBackendBundle`,
   `GamePlatform`, `Canvas2DBackend`, `RenderResourceBackend` and
   `SceneBackend`.

The base `BTBHRenderer` product and its tests do not acquire an SDL dependency.
The game opts into `BTBHRendererGPU` explicitly. SDL, Vulkan, Metal,
command-buffer and native-window types remain absent from the public project
seam.

The production path does not call the one-shot Phase 2 spike runtime. Phase 2
shaders, reflection, capability checks and ownership primitives are reused,
but the game backend owns a persistent device, window, frame slots, resource
registry and pipelines.

## Runtime ownership and frame order

One GPU bundle owns, in shutdown order:

1. SDL platform/window and input state;
2. persistent GPU renderer/device and swapchain;
3. resource stores and deferred-release queue;
4. scene and Canvas2D frame builders.

A frame executes:

1. poll events and converge resize/high-DPI/minimize state;
2. begin a reusable frame slot and drain uploads;
3. collect immutable 3D and Canvas2D commands;
4. execute shadows, G-buffer, lighting, transparent/post and UI passes;
5. submit/present once and attach the frame fence;
6. collect resources whose retire serial completed.

No production frame recreates the device or all resources. No operation may
use a swapchain texture from a different command buffer, and no acquired
swapchain command buffer may be cancelled.

## Feature migration order

The main game is the first end-to-end slice:

1. window/input/time/clipboard and explicit Vulkan initialization;
2. texture, mesh, material, model and render-target lifecycle;
3. opaque mesh/model draw, persistent instancing and frustum culling;
4. lights, shadows, sky/IBL, animation player/tree and transparent surfaces;
5. post-chain features used by the game;
6. Canvas2D primitives, textured quads, font atlas/text and scissor;
7. level-up/world-frame capture and presented-frame readback.

After the main game passes, the same backend is completed for Combat Lab,
particle editor, city-block editor and unified-city. An unsupported operation
is a failing parity test, not a silent no-op.

Runtime Assimp remains available behind the GPU asset layer for current
development/editor model formats. Image and font decoding use renderer-owned
decoder/rasterizer modules. These CPU dependencies do not authorize calling
the legacy OpenGL renderer.

## Build and executable contract

`tools/build-gpu-game.ps1` performs the ordered Windows build:

1. build offline GPU shaders and reflection;
2. build the production GPU core;
3. build the Swift package in release mode;
4. stage `Main.exe`, required runtime libraries and shader/assets into a
   deterministic output directory;
5. run a backend-identity smoke that requires `project_backend=gpu` and
   `driver=vulkan`.

The staged executable is the artifact used for captures and FPS comparisons.
A successful `swift build -c release` of a legacy executable alone does not
satisfy this contract.

## Verification and acceptance

### Automated

- Renderer Swift tests and C/C++ tests pass.
- Game full Swift tests pass in addition to GPU-specific selection, lifecycle,
  resource and command-routing tests.
- Public headers and Swift interfaces contain no SDL/Vulkan/Metal types.
- The release GPU `Main.exe` starts with exact Vulkan, exercises at least one
  real game frame and exits cleanly under automation.
- A routing audit proves the GPU run did not instantiate or call
  `LegacyRenderServices`.

### Five-mode Windows gate

For game, Combat Lab, particle editor, city-block editor and unified city:

- run two independent warmed 1920x1080 capture series;
- retain exact canonical entity signatures and cue inputs;
- record median, nearest-rank p95 and worst presented-frame time plus CPU
  submit/fence-completion diagnostics;
- compare to the retained Windows Run 3/4 legacy authority at comparable load;
- diagnose any median or p95 regression above 5%;
- compare platform-local output against the approved legacy golden at SSIM
  0.98 and inspect required cue regions;
- verify resize/high-DPI, asset lifetime and clean shutdown.

Goldens and thresholds are not changed to make the GPU backend pass.
Historical Windows-to-Apple text and antialiasing differences remain a
separate documented cross-platform diagnostic.

## Rollback and completion boundary

Until the later cross-platform removal gate, rollback is selecting
`BTBH_RENDER_BACKEND=legacy` or reverting the GPU integration commits.
Legacy remains buildable but does not share GPU resources or frame state with
the production backend.

Windows Phase 3–4 is complete only after the real Vulkan `Main.exe` and all
five modes pass. Final backend migration remains incomplete until the same
bundle passes native Metal on macOS and the later plan makes GPU the default
and removes legacy OpenGL dependencies.
