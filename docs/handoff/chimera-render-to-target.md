# Handoff to CHIMERA: live portraits under FSR 3 (v0.1.1)

From the renderer coordinator (Windows PC), 2026-11-09. The request and the design are in
[RFC 0006](../rfc/0006-scene-render-to-target.md). `gh` can't open an issue on this PC, so this file is the
handoff.

## What to pin

- **Tag `v0.1.1`**, a checkout at `../btbh-renderer-v0.1.1` (detached, next to `RTS/`).
- `RTS/renderer.pin`: change `../btbh-renderer-v0.1.0` to `../btbh-renderer-v0.1.1`.
- The prebuilt SDL3/Assimp stay as they are (`target/btbh-prebuilt`). The shader list and the shaders are
  unchanged since v0.1.0. Rebuild the shaders anyway, as the launchers do after a bump.
- **The look is v0.1.0's.** v0.1.1 is v0.1.0 plus C code only: no shader, uniform or default changed. Checked
  on this PC with CHIMERA `main` at `ee0487c2`, built against v0.1.0 and against the candidate. A sandbox
  Vesper was selected with its portrait, at 1600 × 900, and the PNGs are **byte-identical** both with
  `--graphics high` and with `high,upscaler=fsr3,render_scale=67`. v0.1.1 does not contain the v0.2.0 work
  (PCSS, HBAO, etc. are on `release/v0.2.0`).

## The API

```c
BTBHGPUResult btbh_gpu_window_scene_render_to_target(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,   /* btbh_gpu_window_render_target_create(window, side, side, LINEAR, ...) */
    /* then the arguments of btbh_gpu_window_scene_present_instances_with_visibility, in order:
       11 shader paths, shader format, render_width, render_height, view_projection, camera right/up/
       position, lighting, post, instances, transient count, logical count, batches, scene_visibility
       (NULL), directional_shadow_visibility (NULL), batch_count, clear r g b a, report */
);
```

- It renders the portrait at `render_width × render_height` and puts it into `target`, at the target's size.
  Render it at **2 × the slot** with the target **1 × the slot**: the renderer halves it exactly (bilinear),
  which replaces the overlay's own halving. Or make the target 2 × and keep halving in the overlay.
- **FSR 3 in `post` is fine.** It runs as FSR 1, and the `temporal_*` fields are ignored and not validated.
  The report says `upscale_mode == FSR1`.
- **The window's FSR 3 history is untouched.** No reset, no accumulation: the previous view-projection, bone
  palettes, retained shadow maps and the radiance cascades are left alone. The next world frame reprojects as
  if no portrait had been drawn. The renderer's GPU smoke checks this byte for byte.
- Call it **before** the frame's `scene_present_instances` (where `render_aside` is called now), or after the
  canvas present. While a scene waits for its canvas, it returns `INVALID_ARGUMENT` and the waiting frame
  survives.
- The camera is the portrait's own. `CameraFrame::placed` (the corner of a full frame) is not needed: the
  target is the whole picture.
- The first call makes the window's second scene runtime: about 20 ms CPU here, once (pipelines). With a
  shadow it also makes its own shadow maps (21 MB). Later calls cost about 2 ms CPU in the smoke (debug
  device, one cube). Warm it at load with one call if the first selection should not hitch.
- Feature test: `BTBH_GPU_SCENE_HAS_RENDER_TO_TARGET` (bindgen: `ffi::BTBH_GPU_SCENE_HAS_RENDER_TO_TARGET`),
  if `btbh-gpu-sys/build.rs` wants a cfg as it has for refraction.

A sketch for `chimera-platform/src/gpu.rs`, in the style of `render_aside`:

```rust
/// Renders a scene into `into` (a `render_target` of any size) apart from the frame: drawn at
/// `render` pixels and scaled to `into`'s size, never temporal; the frame's FSR 3 history is
/// left alone (no `temporal.idle()`), so call it before `present` every frame if you like.
pub fn render_to_target(&self, render: (u32, u32), camera: &CameraFrame, lighting: &Lighting, post: &Post,
                        instances: &[[f32; INSTANCE_FLOATS]], batches: &[Batch], clear: [u8; 4],
                        into: *mut Texture) -> Result<(), String> {
    if batches.is_empty() || instances.is_empty() || into.is_null() {
        return Err("nothing to render to a target".into());
    }
    let s = |i: usize| self.shaders[i].as_ptr();
    unsafe {
        let mut report = ffi::BTBHGPUSceneReport::default();
        let result = ffi::btbh_gpu_window_scene_render_to_target(
            self.window, into, s(0), s(1), s(2), s(3), s(4), s(5), s(6), s(7), s(8), s(9), s(10), self.format,
            render.0, render.1,
            camera.view_projection.as_ptr(), camera.right.as_ref().as_ptr(), camera.up.as_ref().as_ptr(),
            camera.position.as_ref().as_ptr(), lighting, post,
            instances.as_ptr().cast(), instances.len() as u32, instances.len() as u32,
            batches.as_ptr(), std::ptr::null(), std::ptr::null(), batches.len() as u32,
            clear[0], clear[1], clear[2], clear[3], &mut report,
        );
        if result != ffi::BTBHGPUResult_BTBH_GPU_OK {
            return Err(format!("scene to target: {}", message(&report.message)));
        }
    }
    Ok(())
}
```

Then, in `portrait.rs`, `Last::stale` no longer needs its temporal branch: the portrait can be live at 30 Hz
under FSR 3 too, and `Gpu::fresh_history` no longer matters for it. `render_target` can be slot-sized instead
of frame-sized (14.7 MB → about 0.3 MB at 259²).

## Also in v0.1.1 (optional)

- **Per-batch reactive mask:** set `batch.reactive_mode = BTBH_GPU_SCENE_REACTIVE_NONE` (bindgen:
  `ffi::BTBHGPUSceneReactiveMode_BTBH_GPU_SCENE_REACTIVE_NONE as u8`) on the ground detail layers
  (`scenery.rs`). They are then accumulated like the opaque ground, and `temporal_reactive_scale` can go back
  toward AMD's 0.9 for the real effects. Measure again (fsr3.md §3.2). The renderer's smoke covers only the
  validation, not the image.
- **Fallback notice:** `report.upscale_mode` after every present tells you what ran. `FSR1` while `post` asked
  for `FSR3` means FSR 3 is unavailable on this device (or failed once). `Gpu::temporal()` could read it, so the
  history logic and the Graphics tab know.

## Not in v0.1.1

- Motion vectors for meshes updated in place (cloth, decals) and the true rotation of transient instances both
  need a previous position or transform per vertex or instance, which is a vertex-layout and pipeline change.
  They are open questions in RFC 0006 and need their own RFC.
- Known: a `temporal_reactive_scale` of exactly 0 leaves the shading's ambient term in the reactive mask. It is
  unchanged in 0.1.x (fixing it changes FSR 3 images), and `REACTIVE_NONE` doesn't have it.

## What ran where (Windows PC, RTX 3070 Ti, Vulkan)

- `tools/check.ps1 -SkipGpuTests` on the candidate: build, 66 shaders (SPIR-V + MSL), the native tests, the
  headers as C11/C++17.
- GPU tests run separately: `btbh_gpu_device_smoke` and the new `btbh_gpu_scene_target_smoke`, which uses
  hidden windows and never takes focus.
- `btbh_gpu_window_smoke` was **not** run: it opens a visible window and toggles fullscreen, and the author
  may be playing. Its code is untouched, except a no-op release at window destroy.
- CHIMERA: `cargo build --release --locked -p chimera-game` against the candidate and against v0.1.0. Each
  used its own `CARGO_TARGET_DIR` under `%LOCALAPPDATA%` (`btbh-wt-rtt-cargo`, `btbh-wt-rtt-cargo-base`),
  `RTS/target` untouched. Bindgen sees the new function, enum and fields. The A/B screenshots are above.
- Swift layer: `swift build --target BTBHRendererGPU`.
- Mac: not run. The change is C only, and the smoke runs there with metallibs. FSR 3 falls back to FSR 1 on
  Metal in 0.1.x.
