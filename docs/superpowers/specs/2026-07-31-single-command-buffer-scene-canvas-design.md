# Single Command Buffer Scene/Canvas Design

Status: implemented and verified

Date: 2026-07-31

## Goal

Reduce Vulkan gameplay frame-time overhead and variance without changing
rendered output by replacing the normal scene-plus-Canvas two-submit sequence
with one command buffer and one final submit.

The current native scene path records all offscreen 3D work and immediately
submits it. Canvas then acquires a second command buffer, waits for a swapchain
texture, composites the scene, draws 2D content, and submits again. The scene
submit itself is only a small part of CPU time; this design is an isolated
scheduling experiment after the static-shadow GPU bottleneck was removed.
Combining commands can also delay current-frame GPU work until after the
swapchain wait, so it must be rejected if repeated total-frame measurements
regress despite reducing API calls.

## Design

The scene presenter records exactly the same upload, shadow, geometry, and
post-processing commands, but hands the successful command buffer to the
window instead of submitting it:

- `pending_command` owns the recorded command buffer;
- `pending_scene_ready` marks the final offscreen scene texture available for
  composition;
- Canvas performs the existing late swapchain acquisition on that command
  buffer, composites the scene, records any 2D work and requested presented-
  frame copy, then performs the only submit.

Canvas-only frames continue to acquire their own command buffer. A Canvas
submission must never overwrite an existing pending scene command. The
zero-Canvas-pass path uses `presentClear`; when a scene is pending, it must
late-acquire the swapchain, composite the scene, and submit the combined frame
instead of submitting the offscreen scene without presentation.

`captureWorldFrameTexture` is an explicit rare-path exception. Before running
the existing standalone capture copy, it submits a still-pending scene command
without a telemetry fence and retains `pending_scene_ready`. This preserves the
public capture texture's existing lifetime and failure semantics. Canvas then
acquires a fresh command buffer and composites the already submitted scene.
Normal gameplay, which does not call capture between scene and Canvas, keeps
the one-submit fast path.

Every failure after a scene command is pending must route through
`btbh_gpu_window_finish_pending_frame(window, false)`. Before swapchain
acquisition this cancels the command; after a swapchain texture has been
acquired the existing helper submits it as required by the presentation
lifetime rules. Window destruction and detection of an unfinalized previous
scene keep the same cleanup behavior.

Frame telemetry attaches its fence to the final combined submit, so fence
completion now describes the complete 3D-plus-2D frame. CPU phase attribution
will shift: scene submission no longer includes an SDL submit, while Canvas
presentation contains the single final submit and any swapchain wait.

## Verification

- A source contract test proves that normal scene presentation stores rather
  than submits its command buffer, and that Canvas reuses it conditionally.
- Source contracts cover the `presentClear` pending-scene composition path and
  the explicit standalone-capture fallback.
- Native GPU tests cover a normal scene/Canvas frame, a zero-Canvas-pass frame,
  presented-frame readback, and `captureWorldFrameTexture` followed by sampling
  in Canvas.
- Full renderer and game tests, release build, API audit, and diff checks pass.
- Repeated warmed gameplay captures retain the canonical entity signature,
  clean shutdown, 1920x1080 output, and visual parity. Goldens and thresholds
  stay unchanged.
- The optimization is retained only if repeated warmed Vulkan median and p95
  improve or remain neutral; final completion still requires both to beat the
  tuned legacy OpenGL baseline.

## Result

Normal scene/Canvas frames retain one pending command buffer and perform one
final submit; zero-Canvas and explicit capture paths preserve their documented
lifecycle. Native/readback tests and full suites pass. The retained path is in
the final release where Vulkan measures `4.043/4.371 ms` against legacy
`5.644/5.996 ms`. Strict cross-backend visual SSIM remains a separate open
gate; no golden or threshold changed.
