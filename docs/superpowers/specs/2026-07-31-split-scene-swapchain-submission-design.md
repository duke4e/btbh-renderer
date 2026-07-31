# Split Scene / Swapchain Submission Design

Status: automatically approved on 2026-07-31 after the user authorized
renderer and game API changes in pursuit of Vulkan performance.

## Problem

The Windows start-gameplay A/B establishes an uncapped Vulkan frame median of
11.720 ms versus 4.190 ms for legacy OpenGL. The dominant Vulkan phase is
`scene.endFrame()`, which expands to roughly 8 ms under queue pressure.

A focused uncapped diagnostic run proves the synchronization point:

- swapchain acquire median: 6.114 ms;
- complete native scene encode median: 7.725 ms;
- allowed frames in flight: 3;
- present mode: immediate.

The native scene path calls
`SDL_WaitAndAcquireGPUSwapchainTexture` before geometry and post encoding.
Until that call returns, none of the current frame's scene work can be
submitted to the GPU. The final post, FXAA, or SMAA pass then writes directly
to the acquired swapchain image and leaves the combined command buffer for
Canvas2D to extend and submit.

This preserves the sequencing shape inherited from r3d, but serializes useful
offscreen GPU work behind swapchain availability.

## Decision

Split one logical game frame into two ordered GPU submissions:

1. `scene.endFrame()` queries the current drawable pixel size without
   acquiring a swapchain image, records the complete 3D scene into offscreen
   textures, writes its final color into a swapchain-format offscreen texture,
   and immediately submits that command buffer.
2. `canvas.endFrame()` acquires a fresh command buffer and waits for a
   swapchain image only after all game UI commands have been authored.
3. Canvas blits the pending scene final-color texture into the acquired
   swapchain image, then records screen UI passes with load semantics and
   submits the presentation command buffer.

SDL guarantees that every command in an earlier submission begins before any
command in a later submission. Because scene and canvas are submitted on the
same device queue in strict order, the canvas blit observes the completed scene
write. The next frame's scene submission follows the previous frame's canvas
submission, so the single reusable final-color texture is not overwritten
before the previous frame samples it.

## Native frame state

`BTBHGPUWindow` will distinguish:

- a pending presentation command and acquired swapchain image;
- a submitted offscreen scene waiting to be consumed by Canvas2D.

`pending_command != NULL` continues to mean an unsubmitted command buffer.
A new boolean scene-ready state records the scene/canvas lifecycle. Scene
submission fails closed if the previous scene was never consumed by Canvas2D.
Canvas clears scene-ready state on successful presentation and on a terminal
presentation failure so the next logical frame can recover.

Scene transient upload resources are released immediately after scene command
submission. SDL resource release remains deferred internally until queued GPU
usage completes.

## Render targets

The scene runtime gains a final-color texture created with:

- the current swapchain texture format;
- color-target and sampler usage;
- the current drawable width and height.

Production post always writes offscreen:

- no AA: production post -> final color;
- FXAA: production post -> post color -> FXAA -> final color;
- SMAA: production post -> post color -> edge/weight passes -> blend -> final
  color.

Canvas uses `SDL_BlitGPUTexture` to copy/scale final color into the current
swapchain extent before UI. Exact-size windows use a one-to-one blit. A resize
between scene and canvas remains safe because the destination extent comes
from swapchain acquisition and the source extent comes from scene submission.

## Compatibility and scope

The public Swift `scene.beginFrame/endFrame` and `canvas.beginFrame/endFrame`
sequence remains valid for this first optimization. The native dependency on
r3d-style direct scene presentation is removed. This is not a promise to
preserve the public split API: if the measured result still leaves a material
gap, the game and renderer may adopt one explicit logical-frame submission API
in the next iteration.

No golden, visual threshold, post-processing option, or gameplay workload is
changed. Testing remains gameplay-only until the performance goal is met.

## Failure behavior

- Drawable-size query failure cancels the unsubmitted scene command.
- Scene submission failure does not publish scene-ready state.
- Swapchain acquisition failure after a submitted scene consumes/discards the
  pending scene state and reports a window failure.
- Canvas-only frames still acquire, clear/draw, and present normally.
- Normal frame readback continues to capture the fully composited swapchain
  image. Explicit world-frame capture uses a separately submitted,
  queue-ordered copy from the submitted offscreen final-color texture and does
  not require a pending swapchain command.
- Shutdown releases offscreen resources safely even if queued work exists.

## Acceptance

The change is accepted only if:

1. renderer tests, GPU tests, game tests, and the Windows release build pass;
2. a real gameplay Vulkan frame presents the same scene plus UI and shuts down
   cleanly;
3. entity signature, output size, and approved visual cues remain unchanged;
4. uncapped gameplay A/B shows a lower Vulkan active/frame median without
   lowering goldens or thresholds;
5. the source contract proves scene submission no longer acquires or references
   a swapchain texture;
6. the source contract proves Canvas acquires the swapchain and composites the
   pending offscreen scene before screen UI;
7. subsequent profiling targets the next largest measured gap until Vulkan
   exceeds legacy OpenGL on the controlled start-gameplay workload.

The world-frame capture regression exposed by the split was fixed by moving
capture ownership to the scene runtime. The regression test now proves model
geometry can be captured after offscreen scene submission and before Canvas
presentation.
