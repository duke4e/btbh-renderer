# RFC 0006: A scene into a render target, apart from the window's temporal history

- **Status:** Implemented in v0.1.1
- **Requested by:** CHIMERA (2026-11-09: live 3D unit portraits under FSR 3)
- **Affects:** CHIMERA (adopts it). Be The Bullet Hell and Tyrian3D: nothing, opt-in
- **Issue:** none. `gh` is not logged in on the Windows PC, so this file is the request record and the
  handoff. Open an issue that links it once `gh` works there.

This is a small additive change: a new function, an appended enum, a new batch field and a new report field,
and no render pass, uniform or G-buffer change. By `COORDINATION.md` it needs no RFC. It is written as one
because no issue could be opened.

## Need

CHIMERA shows the selected unit as a live 3D portrait (StarCraft II's portrait window). It draws it as a
second scene each frame (at most 30 a second) before the frame's own scene, and copies it into a texture
(`btbh_gpu_window_render_target_create` + `btbh_gpu_window_copy_pending_scene_texture`).

CHIMERA also renders its world with FSR 3 (`BTBH_GPU_SCENE_UPSCALE_FSR3`), and in v0.1.0 the window has
only one temporal history (`BTBHGPUSceneRuntime`: the FSR 3 resources, the previous view-projection, the bone
palettes). Every scene sets `temporal_previous_valid = temporal_active` at its end. So:

- a portrait without FSR 3 makes the next world frame start its history again (about three frames that look
  as if there were no history: 26.9 thousandths of flicker instead of 1.8, measured by CHIMERA);
- a portrait with FSR 3 would accumulate into the world's history and overwrite its previous camera;
- there is no flag to skip the temporal state, and a second window doesn't help, because textures, models and
  the canvas belong to one window (`destination->owner != window`).

So under FSR 3 the portrait is a still picture today. It is redrawn only when another type is selected, and
each redraw costs the world about three frames of history (`RTS/docs/v2/napredak/fsr3.md` §4).

The portrait is also expensive: every render is a whole frame. It is drawn at the frame's size (2560 × 1440)
into a corner, then copied. The slot is 150–300 px. One render costs about 0.25 ms of GPU and needs a
frame-sized texture (14.7 MB). The portrait can't use a smaller size, because the scene's targets would be
reallocated twice a frame.

From the same note's open problems:

- The reactive mask takes every transparent batch's alpha. CHIMERA's ground detail layers are transparent, so
  the whole ground counts as an "effect", and it settles for a scale of 0.3 instead of 0.9.
- When FSR 3 falls back to FSR 1, the host isn't told.
- Meshes updated in place (cloth, decals) have no motion vectors.

## Proposal

```c
/* Defined when btbh_gpu_window_scene_render_to_target exists ... (since 0.1.1). */
#define BTBH_GPU_SCENE_HAS_RENDER_TO_TARGET 1

typedef enum BTBHGPUSceneReactiveMode {
    BTBH_GPU_SCENE_REACTIVE_SCALED = 0,   /* alpha x temporal_reactive_scale (as before) */
    BTBH_GPU_SCENE_REACTIVE_NONE = 1,     /* nothing: accumulated like the opaque scene */
    BTBH_GPU_SCENE_REACTIVE_COUNT = 2
} BTBHGPUSceneReactiveMode;

/* BTBHGPUSceneGeometryBatch, after rc_receiver (in what was padding: the struct's size is unchanged) */
    uint8_t reactive_mode;

/* BTBHGPUSceneReport, appended */
    uint8_t upscale_mode;          /* what ran: post->upscale_mode, or FSR 1 where FSR 3 fell back */
    uint8_t upscale_reserved[3];

BTBHGPUResult btbh_gpu_window_scene_render_to_target(
    BTBHGPUWindow *window,
    BTBHGPUTexture *target,          /* btbh_gpu_window_render_target_create, this window's, any size */
    /* ... then exactly the arguments of btbh_gpu_window_scene_present_instances_with_visibility:
       eleven shader paths, shader format, render width and height, camera, lighting, post, instances,
       batches, visibility, clear colour, report */
);
```

Zero values: a zeroed `reactive_mode` is the old behaviour. `upscale_mode` is an output field. Nothing changes
for a host that doesn't call the new function.

What `btbh_gpu_window_scene_render_to_target` does:

- **Its own runtime.** The window keeps a second `BTBHGPUSceneRuntime` for these scenes, made by the first
  call. The call swaps it in for its duration, so it has its own scene targets (at its own render size), shadow
  maps, pipelines and state. The window's runtime is not touched at all: not its FSR 3 history, previous
  view-projection, bone palettes, retained shadow maps or radiance-cascade history, and its targets keep the
  frame's size. Models, custom meshes, textures and instance buffers are the window's, shared.
- **Never temporal.** `BTBH_GPU_SCENE_UPSCALE_FSR3` runs as FSR 1, and `temporal_*` are ignored (not even
  validated). The report says FSR 1.
- **Output size** is the target's. The scene renders at `render_width × render_height`, and FSR 1 or Lanczos
  upscales it to the target's size when asked. Otherwise a bilinear blit scales it, and a render twice the
  target's size is halved exactly (CHIMERA's supersampling). Then a blit puts it into the target, and the call
  submits.
- **When.** No scene may be waiting for its canvas: call it before the frame's scene present, or after the
  canvas present. Otherwise it returns `BTBH_GPU_INVALID_ARGUMENT` and leaves the waiting frame as it is (the
  old present path would drop it). The canvas, or a later scene, can sample `target` in the same frame.
- **Refusals** (`BTBH_GPU_INVALID_ARGUMENT`, the window left as it was): a texture of another window, or one
  that is not a render target (made without `SDL_GPU_TEXTUREUSAGE_COLOR_TARGET`).
- Telemetry: the window's `scene_telemetry` is restored afterwards, so `btbh_gpu_window_scene_read_telemetry`
  still describes the frame's scene.

`reactive_mode = BTBH_GPU_SCENE_REACTIVE_NONE` is a per-draw uniform. Without a shader change, it is a reactive
scale too small to reach the R8 mask's first step (1e-6). The shader writes the mask only for a positive scale.

## Minimal example

```c
/* Once: a target the size of the portrait slot (pixels). */
BTBHGPUTexture *portrait = btbh_gpu_window_render_target_create(
    window, 259, 259, BTBH_GPU_TEXTURE_FILTER_LINEAR, &texture_report);

/* Each frame (or 30 a second), before the frame's own scene: */
BTBHGPUScenePost post = portrait_post;          /* FSR 3 here is fine: it runs as FSR 1 */
BTBHGPUSceneReport report;
if (btbh_gpu_window_scene_render_to_target(
        window, portrait, /* the eleven shader paths */ ..., shader_format,
        2 * 259, 2 * 259,                        /* supersampled, halved into the target */
        portrait_camera.view_projection, portrait_camera.right, portrait_camera.up,
        portrait_camera.position, &portrait_lighting, &post,
        instances, instance_count, instance_count, batches, NULL, NULL, batch_count,
        12, 14, 20, 255, &report) != BTBH_GPU_OK) {
    /* report.message; show the thumbnail */
}
/* ... the frame's scene with FSR 3 as usual; the canvas draws `portrait` into the slot. */
```

In Rust (bindgen), the call is `ffi::btbh_gpu_window_scene_render_to_target(window, into, s(0), ..., s(10),
format, w, h, ..., &post, ..., std::ptr::null(), std::ptr::null(), batches.len() as u32, r, g, b, a, &mut
report)`. The camera no longer needs `CameraFrame::placed`: the target is the picture.

## Implementation

- `src/gpu/btbh_gpu_internal.h`: `BTBHGPUWindow.aside_scene_runtime`; `BTBHGPUTexture.color_target` (set by
  `btbh_gpu_window_texture_create_empty` from its usage).
- `src/gpu/btbh_gpu_scene.c`:
  - the internal present takes an output size (0 = the window's drawable size, as before), and a non-zero one
    is never temporal;
  - `btbh_gpu_window_scene_render_to_target` swaps the runtimes, maps FSR 3 to FSR 1, presents, blits the
    composite into the target, submits, and swaps back (also on failure);
  - `btbh_gpu_window_release_aside_scene` runs at window destroy;
  - batch validation accepts `reactive_mode < BTBH_GPU_SCENE_REACTIVE_COUNT`;
  - `report->upscale_mode` is set from what ran.
- No shader, uniform or G-buffer change, and no new pass in the window's frame. The blit into the target is the
  only new GPU work.
- **Cost:** CHIMERA's portrait no longer renders a whole frame. Its passes run at the portrait's render size
  (e.g. 518² instead of 2560 × 1440), and it no longer needs a frame-sized copy texture. The aside runtime's
  memory is its targets at the portrait's size (a few MB at 518²) plus, with shadows, its own shadow maps (2048²
  and two 512² D32 directional maps and a 1024² spot map: 21 MB). Its first call creates its pipelines, a
  one-time hitch: a host can warm it up with one call at load.

## Compatibility

- Versioning: additive and opt-in, so v0.1.1, a patch under the pre-1.0 rule ("additive → minor or patch").
  It is cut from `master`, which is still v0.1.0 plus documentation. v0.2.0 (`release/v0.2.0`) must merge it
  before its tag.
- `BTBHGPUSceneReport` grew by 4 bytes at the end. Hosts compile the C core with the header (all three do), so
  that is safe. Mixing a header and a C core of different versions was never supported.
- `BTBHGPUSceneGeometryBatch.reactive_mode` sits in padding: the size is unchanged.
- Be The Bullet Hell (Swift, zero-initialised `BTBHGPUSceneGeometryBatch()` and `BTBHGPUSceneReport()`), CHIMERA
  (bindgen, `Default`), Tyrian3D (`= {0}`): nothing to change. The image is identical: no default path changed.
- **CHIMERA keeps v0.1.0's look:** v0.1.1 is v0.1.0 plus this change, with no shader and no default changed.
  The A/B check is under Platforms and testing.

## Platforms and testing

- **Windows / Vulkan:** `tools/check.ps1` with the new GPU smoke `btbh_gpu_scene_target_smoke`. It uses two
  hidden windows, a lit cube, FSR 3 at 50 %, and a directional shadow:
  - window A renders world frames 0–3; window B renders the same frames, with three portraits into a 64 × 64
    target between frames 1 and 2 (rendered at 128, 32 and 64);
  - frame 3 is **byte-identical** in both windows;
  - then B renders a scene aside the old way (a present with FSR 1 and a copy), and B's next frame **differs**
    from A's (its history restarted), so the check can tell;
  - it reads the portrait back (cube on the clear colour, at the target's size) and checks
    `report.upscale_mode` (FSR 3 in the world, FSR 1 in the portrait), the three refusals and the
    `reactive_mode` validation.
- **macOS / Metal:** not verified on the Mac. The change is C only, and the smoke runs there with metallibs
  (it skips with 77 when they are missing). FSR 3 still falls back to FSR 1 on Metal in 0.1.x, so the
  byte-identity check only shows the fallback there.
- **Linux:** not built for this release (no Linux machine in the release check). The code path is the same
  Vulkan C as on Windows.
- **CHIMERA:** compiled against the candidate (`BTBH_RENDERER_DIR` = the worktree, its own `CARGO_TARGET_DIR`).
  The results are in the tag message and the handoff below.

## Alternatives

- **A flag on `BTBHGPUScenePost`** (`temporal_isolated`) for the normal present path. That would solve the
  history, but not the cost: the scene would still be frame-sized and copied. The pending scene would also
  belong to one of two runtimes, which complicates the canvas and the copy functions. A render target solves
  both.
- **Save and restore the temporal state around a scene.** The FSR 3 resources (history textures) can't be
  restored cheaply, and the targets would still be reallocated at a different size.
- **Share pipelines between the two runtimes.** That avoids the one-time hitch. But several "ensure" functions
  create pipelines and other resources together (shadow maps, SMAA lookup textures), keyed off the pipeline
  pointer, so sharing needs a careful split. It can come later if the hitch matters.

## Open questions

- **Motion vectors for meshes updated in place** (cloth, decals): the vertex shader would need each vertex's
  previous position (a second vertex stream, or the previous buffer kept per custom mesh). That changes the
  vertex layout and the pipelines, so it needs its own RFC.
- **Rotation of transient instances under FSR 3** (CHIMERA §8, "Samo pomak"): a previous transform per
  instance instead of a translation. Same kind of change. Not requested here.
- A reactive scale of 0 leaves the shading's ambient term in the mask (see CHANGELOG 0.1.1, Notes). Fixing it
  changes FSR 3 images for hosts that use 0, so it waits for a minor release with a note.
- Smaller shadow maps for the aside runtime (the portrait's sun covers one unit): a size control would save
  most of the 21 MB.

## Handoff to CHIMERA

See [`docs/handoff/chimera-render-to-target.md`](../handoff/chimera-render-to-target.md).
