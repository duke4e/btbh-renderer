# RFC 0015: A frame thread: the renderer encodes a frame while the host builds the next

- **Status:** Accepted on 2026-11-27, design (b): v2 answered all 12 questions through its session (see "Decisions").
  Phase 1a after 0.16.0; phase 1b after v2's CPU-capped measurement of the Deck preset (and, if the owner agrees, one on
  the Steam Machine)
- **Requested by:** Be The Bullet Hell v2 (its perf round of 2026-11-27, `v2/docs/perf-2026-11-27.md`: "Where the CPU
  goes", ranked item 5 and "Renderer items" item 6)
- **Affects:** Be The Bullet Hell v2 (adopts it, opt-in). Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D: nothing
  to change, and nothing changes for them unless they call `btbh_gpu_window_frame_thread_start`. Phase 1a refactors how
  the scene present asks for the window's size and how the canvas reaches the swapchain; their images must stay byte for
  byte
- **Issue:** none. v2 answered through its session on 2026-11-27
- **Depends on:** nothing for phases 1 and 2. Phase 3 (views in a frame) comes after RFC 0014 phase 1
- **Sources:** master `1dfe3c99` (v0.15.0); every line reference is that commit's. SDL is `external/SDL` at
  `release-3.4.2`. v2's code is its perf worktree (`btbh-wt/p21-perfscout`, `557be6d5`), paths under `v2/crates/`.
  v2's per-frame numbers are its hitch logs (`btbh-wt/p21-perfscout-out/runs/*-deck-r1.csv`, medians over each scene's
  window) and its perf doc. Short names below: `scene.c`, `canvas.c`, `window.c`, `custom_mesh.c`,
  `instance_buffer.c`, `texture.c`, `model.c`, `device.c` for `src/gpu/btbh_gpu_*.c`; `SDL_gpu.h`, `SDL_events.h`,
  `SDL_video.h` under `external/SDL/include/SDL3/`; `vulkan.c` for `external/SDL/src/gpu/vulkan/SDL_gpu_vulkan.c`

## Need

At v2's Deck preset (1280 x 800, uncapped) this PC (Ryzen 9 9950X, RTX 3070 Ti) is the CPU's. One thread runs at
97-98 % of a core. The frame is serial: v2's sim, its scene build, the HUD, the renderer's scene present, the canvas
present. The two halves are about equal:

| Scene (Deck preset) | Frame | v2's part (sim, scene build, HUD, the rest) | Scene present (prepare, upload, shadow, geometry, post) | Canvas present, submit, swapchain | GPU frame (cost log) |
| --- | --- | --- | --- | --- | --- |
| Stage-3 fight | 2.41 | 1.13 | 1.03 | 0.25 | 1.33 |
| Late run | 2.39 | 1.12 | 1.02 | 0.25 | 1.26 |
| Boss | 2.24 | 1.01 | 1.02 | 0.22 | 1.36 |
| Tower Offense | 2.31 | 1.17 | 0.92 | 0.23 | 1.34 |
| One big level (USA) | 3.39 | 1.67 | 1.41 | 0.28 | 1.46 |
| Co-op split | 3.79 | 1.27 | 1.08 | 1.44 (the second view's call, 1.2, and swapchain waits) | 1.42, first view only |

Milliseconds. The middle columns are v2's `total_ms - present`, `r_prepare + r_upload + r_shadow + r_geometry +
r_post`, and the rest of `present`. In the stage-3 fight v2's run log has 1,113 transient instances and 1,088 draws
(`s3fight-deck-r1.out`). The sampling profile adds: the driver 19-26 % of the main thread, SDL's draw path 9-11 %,
bone palettes 5-11 % (being fixed apart from this RFC).

v2 asks: "if the renderer's encode and submit could run on a thread of their own while v2 builds the next frame, the
Deck preset's CPU frame would drop by about 40 %". It wants to know what it hands over per frame (instances, draws, the
canvas) and what it must not touch until the encode is done.

What overlap can buy, plainly:
- **The frame becomes the longer half:** about 1.2-1.3 ms of CPU in the fights (-45 %), 1.7-1.9 in the USA level.
- **On this PC the GPU then limits:** its 1.26-1.46 ms Deck frame. Expect about 2.4 -> 1.35-1.45 ms (-40 %), which is
  v2's estimate. CPU work beyond that buys nothing here at the Deck preset.
- **At 4K High it buys nothing:** the GPU is 12.9-15.7 ms of the frame.
- **Under V-Sync at 60 Hz it buys nothing either:** a 2.4 ms frame already fits 16.7 ms. A pipelined frame then adds up
  to one frame of latency (question 3).
- **The Steam Machine:** v2 expects it CPU-bound "the same way". Not measured. It pays off only where the Machine is
  CPU-bound at the frame rate it targets (question 2).

## What the present reads from the host, and until when

### The scene present's pointer arguments

`btbh_gpu_window_scene_present_instances_internal` (`scene.c:9362-12558`) runs in this order: refusals
(`:9418-9493`), frame setup (`:9525`), jitter (`:9600-9690`), bones and dig bindings (`:9718-9730`), shader checks
(`:9741-9790`), the opaque sort (`:9813`), pipelines (`:9825`), the upload (`:9958-9998`), its command buffer
(`:10000`), the copy pass (`:10016-10058`), shadows (`:10088-10245`), targets (`:10261`), geometry
(`:10377-11730`), the radiance cascades (`:11733-11900`), post (`:11911-12502`), bookkeeping (`:12513-12532`), the
report (`:12534-12556`).

| Argument | Read at | Last read | Copied by the present today? |
| --- | --- | --- | --- |
| `instances` (96 bytes each) | the opaque sort `:9813-9822`; into the transfer buffer `:9975` | `:9975` | Yes, `:9975` |
| `post->temporal_instance_motions` | `:9984-9990` | the upload | Yes |
| `batches` (288 bytes each) | refusals `:9473`, world details `:9489`, setup `:9541`, bones `:9719`, dig `:9730`, checks `:9743-9773`, sort `:9814`, pipelines `:9833`, shadows `:10140-10230`, geometry `:11162`, cascades' sources `:11799`, report `:12543` | `:12543` | No |
| What a batch points at: `world_detail`, `world_detail_table` with its `entries`, `height_lift` | `:9489` (`btbh_gpu_scene_validate_world_details`, `:5201`), geometry | geometry | No |
| `scene_visibility` | setup `:9542`, sort `:9815`, geometry `:11194-11195` | `:11195` | No |
| `directional_shadow_visibility` | `:9489`, shadows `:10141`, `:10168`, `:10231` | `:10231` | No |
| `lighting` (3,340 bytes) | setup, frame blocks, shadows, cascades `:11840` | `:11840` | No |
| `post` (432 bytes) | setup to bookkeeping | `:12524` | No |
| `post->radiance_cascades.emitters` | the cascades' encode (`btbh_gpu_radiance_cascades.c:107`, `:144`, `:231`) | cascades | No |
| `post->radiance_cascades.occlusion_cascades[].samples` | only when `revision` changed (`btbh_gpu_radiance_cascades.c:1345`) | its upload | Yes, `btbh_gpu_radiance_cascades.c:1255` |
| Camera vectors and matrix | jitter, frame blocks, billboards, cascades `:11797`, bookkeeping `:12519` | `:12519` | The matrix is kept as the next frame's previous (`:12516-12520`) |
| The eleven shader paths | setup, checks, pipelines, targets, post | post | No |

So the instances, motions and occlusion samples are done with at the upload, a third of the way in (prepare is 0.33 of
the stage-3 fight's 1.03 ms). Everything else is read until the present returns. At the end the present leaves its
command buffer on the window (`:12528-12529`) for the canvas to submit.

### Objects the host owns and changes

| Object | Read by the present | The host changes it with | v2 per frame |
| --- | --- | --- | --- |
| Animation poses | bones, `:9718` -> `scene.c:6418` (joint counts `:6469`, samples) | `btbh_gpu_animation_pose_set_samples` (`model.c:3888`), which frees and replaces the pose's arrays (`model.c:3941-3943`) | Yes: while building batches (`v2-platform/src/gpu.rs:1797`) |
| Custom meshes | geometry: buffers, counts, arena base | `_update` (its own command buffer, or recorded into a frame waiting for its canvas, `custom_mesh.c:1495`); `_update_ex` QUEUED (copied into the window's queue at the call; counts and bounds change at once); `_update_range` (`custom_mesh.c:1245`); destroy | Queued, on damage (`gpu.rs:1392`) |
| Persistent instance buffers | geometry: the SDL buffer; refusals read `uploaded_prefix_byte_count` (`scene.c:5152-5160`) | `_upload` (cycles, own command buffer); NO_CYCLE (into the waiting frame, `instance_buffer.c:268`); QUEUED (into the queue; the prefix grows at once); destroy | Queued, on damage (`gpu.rs:1430`) |
| The window's upload queue | written into the scene's transfer `scene.c:9992`, recorded `:10055`, committed `:12532` | every QUEUED call (`custom_mesh.c:809`) | Yes |
| Textures: render targets, heightfields (with their CPU heights), atlases, the LUT | sampled in every pass; heightfields' heights by the sort | `btbh_gpu_texture_update_rgba8` (own command buffer; cycles on a whole overwrite, `texture.c:597`, `:617`); renders into a target; destroy | The minimap every 0.5 s (`v2-game/src/minimap.rs:62`) |
| Models, animation libraries | geometry, bones | load, destroy | Stage changes |
| SDF terrains (`dug_by`) | dig bindings, `scene.c:7010` | `btbh_gpu_sdf_terrain_update`, uploads on the calling thread | v1 only |
| Scene runtimes (the window's, the target views') | the present | prewarms, shadow periods and size, `release_target_view` | Shadow periods when the split opens |

### The canvas

`btbh_gpu_window_canvas_present` (`canvas.c:1304-1769`):
- **Vertices:** copied into the canvas's transfer buffer at `canvas.c:1517`.
- **Batches and passes:** read in the pass loop (`canvas.c:1546-1668`): each batch's texture and scissor, each pass's
  target.
- **Textures:** font atlases and render targets (v2's v1 cards, the split's target), sampled.
- **The window:** it acquires the swapchain (`canvas.c:1405-1413`), asks `SDL_GetWindowSize` (`:1474`), blits the
  scene (`:1439-1446`), and submits the frame (`:1755` -> `btbh_gpu_window_finish_pending_frame`, `:616`).

### Input and the window

- **`btbh_gpu_window_poll_events`** (`window.c:1137-1231`): `SDL_PollEvent` (`:1157`), Alt+Enter's fullscreen toggle
  (`:1209` -> `:488-497`, with `SDL_SetWindowFullscreen` and `SDL_SyncWindow`), the gamepads (`:1228`). No GPU state.
- **`btbh_gpu_window_begin_frame`** (`:1313-1346`): the sizes through `SDL_GetWindowSize` and
  `SDL_GetWindowSizeInPixels` (`:283-319`).
- **The present asks SDL for the window's size itself:** `scene.c:8879-8883` (FSR 3's output size), `:9204-9209`
  (the drawable size), `canvas.c:1474`. And the swapchain's format, eight times in `scene.c` (for example `:1797`,
  `:4714`) and at `canvas.c:932`.
- **v2 drives the window directly** through `btbh_gpu_window_sdl_window`: windowed, borderless or exclusive fullscreen,
  then `SDL_SyncWindow` (`v2-platform/src/gpu.rs:1086-1106`).
- **The present reads no input.** But the window's events change SDL's swapchain state: SDL's Vulkan backend watches
  resize events on the thread that pumps them and flags the swapchain (`vulkan.c:9653-9661`); the next acquire
  re-creates it (`vulkan.c:10007-10022`). Neither side takes a lock.

### What to copy, what to borrow

v2's stage-3 fight: 1,113 instances (107 KB), about 1,100 batches (317 KB), lighting (3.3 KB), post (0.4 KB), two
visibility arrays (2 KB), the HUD's vertices (32 bytes each; how many isn't logged).

Decision:
- **Copy at the call everything the frame points at.** About 0.5-1 MB: 0.03-0.06 ms on this PC (an estimate, memcpy at
  10-20 GB/s). The instances and the canvas's vertices are copied today already (`scene.c:9975`, `canvas.c:1517`).
- **Copy by value what batches point at,** deduplicated by pointer: world details (64 bytes), tables with up to 8
  entries (36 bytes each), lifts (40 bytes).
- **Snapshot the poses** a frame's batches use: each pose's samples, a few clips. `set_samples` may then run at once.
- **Copy the radiance cascades' occlusion samples only when their revision changed,** as the present does.
- **Borrow GPU objects by handle** (textures, meshes, instance buffers, models, libraries, terrains). The host can't
  change them behind a frame: every call that could waits for it (Proposal).
- **No borrowing of host memory until a fence in phases 1-2.** It would save the 0.03-0.06 ms, at the price of double
  buffers in v2 and silent corruption on a slip. Phase 3, if the copy shows in a profile.

## SDL 3.4.2's GPU API and threads

### What the documentation says

| Call | Rule | Where |
| --- | --- | --- |
| Command buffers | "you should only access a command buffer on the thread you acquired it from" | `SDL_gpu.h:538-539` |
| `SDL_AcquireGPUCommandBuffer` | "may only be used on the thread it was acquired on ... should be submitted on the thread it was acquired on"; several at once on one thread are fine | `SDL_gpu.h:3142-3146` |
| `SDL_SubmitGPUCommandBuffer`, `..AndAcquireFence`, `SDL_CancelGPUCommandBuffer` | "must be called from the thread the command buffer was acquired on" | `:4341`, `:4367`, `:4395` |
| Several command buffers | "configured across multiple threads in parallel, as long as they are submitted in the correct order"; they run in submission order | `:53-55`, `:4343-4344` |
| `SDL_ClaimWindowForGPUDevice` | only from the thread that created the window | `:4097`, `:4110` |
| `SDL_AcquireGPUSwapchainTexture`, `SDL_WaitAndAcquireGPUSwapchainTexture`, `SDL_WaitForGPUSwapchain` | "MUST NOT call this function from any thread other than the one that created the window"; the texture "should only be referenced by the command buffer used to acquire it" | `:4231`, `:4245`, `:4274`, `:4303`, `:4320`; `:4216`, `:4293` |
| `SDL_PollEvent`, `SDL_PumpEvents` | the main thread (the one that initialized video) | `SDL_events.h:1093`, `:1265-1266`, `:1296` |
| `SDL_GetWindowSize`, `SDL_GetWindowSizeInPixels`, `SDL_SetWindowFullscreen`, `SDL_SyncWindow` | the main thread | `SDL_video.h:1865`, `:2006`, `:2351`, `:2379` |
| `SDL_GetGPUSwapchainTextureFormat` | no rule stated | `SDL_gpu.h:4207` |

So:
- **A command buffer is recorded and submitted on the thread that acquired it.** Recording on one thread and submitting
  on another is outside the documented rules. The backend shows why: its command pools are per thread
  (`vulkan.c:9400-9460`, keyed by `SDL_GetCurrentThreadID` at `:9495`), and Vulkan wants a pool's buffers used by one
  thread at a time. A strict hand-off would probably work on Vulkan; the renderer shouldn't rely on it.
- **The swapchain, the window and the events stay on the thread that made the window.** The present is the submit of
  the command buffer that acquired the swapchain, so it stays there too.

### What the Vulkan backend locks

The locks are at `vulkan.c:1196-1206`, made at `:12698-12708`.

| Lock | Guards | Taken at |
| --- | --- | --- |
| `acquireCommandBufferLock` | the per-thread pools' free lists, descriptor-set caches | `:9497`, `:10472` |
| `acquireUniformBufferLock` | the uniform buffer pool | `:7629`, `:10414` |
| `submitLock` | `vkQueueSubmit`, `vkQueuePresentKHR`, the submitted list, clean-ups | `:10630`, `:10530`, `:10560` |
| `allocatorLock` | device memory | `:1394-1967`; defragmentation `:10821` |
| `disposeLock` | releases and pending destroys | `:6914-7101`, `:10317` |
| `renderPassFetchLock`, `framebufferFetchLock`, the layout locks | their caches | `:7165`, `:2969`, `:7263` |
| `windowLock` | the claimed-window list | `:9807`, `:9882` |
| Reference counts | atomic, per command buffer's tracking | `TRACK_RESOURCE`, `:2376-2391` |

### What it doesn't lock

- **Cycling.** A map, upload or pass with cycle re-points a resource's container to a free buffer or texture
  (`vulkan.c:5840-5880`, `:5883-5925`, decided at `:5930-5950`, `:5954-5975`). No lock: a resource must not be cycled
  on one thread while another binds it.
- **Defragmentation inside a presenting submit.** A submit whose command buffer acquired a swapchain cleans up
  (`:10624-10626`) and defragments one memory allocation (`:10646-10656`). It copies each resource to new memory and
  re-points its container (`:10885-10894` buffers, `:10970-10976` textures) under `allocatorLock` alone. A command
  buffer recording on another thread reads containers without that lock: a write it records into the old buffer after
  the copy is lost. It happens after a new allocation finds fragmented ones (`:2062-2067`), so after releases: after a
  stage's load.
- **The window's swapchain state,** written by the resize watch (`:9653-9661`, on the pumping thread) and read by the
  acquire (`:10007-10022`).
- **Clean-ups in a fence wait** (`:10530-10547`) and a device wait (`:10560-10580`) are locked and safe beside a
  recording thread.

The rule this RFC keeps: **one thread uses SDL's GPU device at a time, and the thread that made the window does every
swapchain, window and event call.** The one overlap allowed: the window thread's input and window calls (no GPU device
state) beside the frame thread's recording.

## Designs

### The five

| | Saves of the stage-3 fight's 2.41 ms (estimate) | Latency | API | Risk for v1, CHIMERA, Tyrian3D | SDL's rules |
| --- | --- | --- | --- | --- | --- |
| **(a)** A render thread owns the whole present and the canvas | CPU about 1.3 ms; frame 1.35-1.45 (this PC's GPU) | CPU-bound: about none. Capped or GPU-bound: up to one frame | New frame API, copies, waits | None unless opted in; the refactor of phase 1a | Broken: the swapchain on another thread |
| **(b)** Encode on a frame thread; the window thread keeps the swapchain and present | CPU about 1.3 ms; frame 1.35-1.45; one more blit on the GPU | As (a). A finished frame also waits for the host's next submit (`frame_present` cures it) | As (a), plus `frame_present` | As (a) | Kept |
| **(c)** Passes encoded in parallel into several command buffers | 0.05-0.15 ms (2-6 %) | None | None (inside) | The same refactor, in every game's frame | Kept if every recording ends before the presenting submit |
| **(d)** No thread: a cheaper encode | 0.25-0.6 ms (10-25 %); bones 0.12-0.27 more (in progress) | None | RFC 0011 phase 2's draw lists | Low | Kept |
| **(e)** Not asked: v2 threads its own frame | As (a) | As (a) | None | None | Kept |

### (a) A render thread owns the whole present

- **Where things run.** The window thread: events, sim, scene build, HUD, a frame submit that copies. The render
  thread: target renders, the scene present, the canvas with its swapchain acquire, the presenting submit.
- **The halves.** The window's 1.13 + 0.05 (copy) = about 1.2 ms; the render thread's 1.03 + 0.25 = about 1.3 ms. The
  frame is about 1.3 ms of CPU; this PC's GPU (1.33 ms) caps it at 1.35-1.45.
- **What breaks.** `SDL_WaitAndAcquireGPUSwapchainTexture` off the window's thread (`SDL_gpu.h:4303`). The present's
  size queries (`scene.c:8879`, `:9205`, `canvas.c:1474`) can be passed in; the acquire can't. Its unlocked read of the
  resize flags (`vulkan.c:10007`) races the event pump (`:9653`). On Windows v2 offers exclusive fullscreen and calls
  `SDL_SyncWindow` (`gpu.rs:1097-1102`; `window.c:495`). If the driver presents through DXGI (NVIDIA's control panel
  can layer Vulkan's present on DXGI), a present on one thread while the window thread waits for it can deadlock; that
  is DXGI's documented multithreading rule. Untested here. On Linux (the Machine) and macOS: unknown.

### (b) Encode on a frame thread; the window thread presents

- **The frame thread** renders the frame's targets, the scene and the canvas into a frame texture, then submits. The
  frame texture already exists: with a readback requested, the canvas draws into `window->presented_frame` instead of
  the swapchain (`canvas.c:914-968`) and blits it to the swapchain at the end (`canvas.c:970-998`). The frame thread
  makes that the only path.
- **The window thread,** inside the host's next frame submit (or `frame_present`): acquires a command buffer and the
  swapchain image, blits the frame texture into it (`btbh_gpu_window_record_presented_frame`'s blit), submits. That
  submit presents.
- **Strict turns.** The presenting submit runs while the frame thread is idle, because of the defragmentation above.
  `btbh_gpu_window_frame_submit(N + 1)` does: check and copy frame N + 1 (the frame thread may still encode N); wait for
  N; present N; hand N + 1 over; return. One frame texture is then enough: the GPU runs N's draws, N's present, N + 1's
  draws, in that order.
- **The halves.** The window's 1.13 + 0.05 (copy) + 0.05-0.15 (the present step: acquire, blit, submit; an estimate
  from the canvas's 0.18-0.2 ms, `canvas.c`'s part of v2's profile) = about 1.3 ms. The frame thread's 1.03 + about 0.15
  (the canvas's encode and its submit), idle for the present step: about 1.25 ms. The frame is about 1.3 ms of CPU;
  this PC's GPU caps it at 1.35-1.45 (USA: about 1.85-1.95; the split: about 2.4-2.5, the frame thread's half, until
  RFC 0014's views).
- **Latency.** Frame N is presented in submit N + 1. CPU-bound, the window's half of N + 1 is about the frame thread's
  encode of N, so the input-to-present time stays about today's 2.4 ms (plus up to 0.2). With a frame limiter, or a long
  job on the window thread, the finished frame waits for the next submit: the host calls
  `btbh_gpu_window_frame_present(window, 1)` before sleeping or before the job (and any call that waits presents too).
  Under V-Sync: up to one frame more, as any pipelined renderer.
- **The GPU.** One more blit at the drawable size: about 0.02 ms at 1280 x 800, 0.1-0.15 ms at 4K (estimates: 8 MB and
  66 MB moved). Leave the thread off at 4K, where it buys nothing anyway.

### (c) Passes encoded in parallel

- SDL allows a command buffer per thread, submitted in order (`SDL_gpu.h:53-55`). It has no secondary command buffers,
  so a render pass stays in one command buffer. The main pass (geometry, 0.49 ms of the 1.03) can't be split without
  ending and restarting it: six render-size targets stored and loaded again.
- What can overlap: the sun's shadow encode (0.09 ms) and post (0.09 ms) with geometry. Prepare (0.33 ms: bones, sort,
  pipelines) stays in front of all of them, and v2's 1.13 ms half is untouched.
- Each extra command buffer costs a submit and a fence.
- The presenting submit must wait for every recording to end (defragmentation).
- Net: 0.05-0.15 ms of 2.41. Not worth the risk to every game's frame.

### (d) No thread: a cheaper encode

- **Bones** (ranked item 6): 0.12-0.27 ms, being fixed separately. It counts for every design.
- **RFC 0011 phase 2, draw lists:** v2's estimate is -0.2 to -0.5 ms (ranked item 7). SDL issues one
  `vkCmdDrawIndexedIndirect` for many commands where the device has `multiDrawIndirect` (RFC 0011, `vulkan.c:12293-12295`).
- **Prepare kept from frame to frame:** prepare without the bones is about 0.1-0.2 ms (refusals, sort, pipeline
  lookups per batch). Keeping what didn't change: -0.05-0.1 ms. An estimate.
- **Recorded commands reused across frames:** SDL has no bundles or secondary command buffers. Not available.
- **Total:** about 2.41 -> 1.8-2.15 ms without the bones. It can't reach 1.4 alone. With a thread it shortens the frame
  thread's half; on this PC the GPU then sets the frame.

### (e) v2 threads its own frame (not asked; for comparison)

- v2 runs its sim and scene build for frame N + 1 on its own worker while its main thread presents N with today's calls.
  The renderer stays on one thread, and SDL's rules hold by themselves.
- v2's work: its scene build calls the renderer today. Queued uploads (`gpu.rs:1392`, `:1430`), mesh creation in kit
  rebuilds and the minimap's texture (`minimap.rs:62`) would be recorded and replayed on the main thread. The pose
  samples are set inside `scene_batches` (`gpu.rs:1797`), already on the present's side. Its `Gpu` uses `RefCell`
  (not `Sync`), so Rust makes the split explicit.
- The same gain as (a), no renderer change, available on 0.15.0 today. It costs v2 a restructured frame loop.

### Recommendation

**(b).**
1. The same gain as (a): 2.4 -> 1.35-1.45 ms at the Deck preset on this PC, where the GPU then limits.
2. Within SDL's documented rules: the window thread keeps the events, the window and the swapchain. The frame thread
   records and submits its own command buffers. One thread uses the GPU device at a time.
3. Safe by construction for the host. Everything it hands over is copied at the call (0.03-0.06 ms). Every call that
   could change what a frame in flight reads waits for that frame. A slip costs a wait, never a wrong picture.
4. Opt-in per window. The other games run the same code, unthreaded, byte for byte.

(d) goes on beside it: the bones now, RFC 0011 phase 2 when v2 wants it. (e) is the choice if v2 would rather own the
thread (question 1). Not recommended: (a) for SDL's rules, (c) for its small gain.

## Proposal

```c
/* The frame thread (RFC 0015, asked for by Be The Bullet Hell v2): the window's scene and canvas are encoded on a thread
 * of the renderer's while the host builds its next frame; the host's thread (the one that made the window) keeps the
 * events, the window and the swapchain, and presents. Opt-in per window, one per device. A window that never starts it
 * works as before. */

/* The paths a frame thread uses, in this order: btbh_gpu_window_scene_present_instances' eleven (vertex_shader_path to
 * post_fragment_shader_path), then btbh_gpu_window_canvas_present's three. */
#define BTBH_GPU_FRAME_SHADER_PATH_COUNT 14u
/* The most renders into targets one frame carries. */
#define BTBH_GPU_FRAME_MAX_TARGETS 8u
/* How many frames' results a window keeps for btbh_gpu_window_frame_result. */
#define BTBH_GPU_FRAME_RESULT_CAPACITY 4u

/* 136 bytes on 64-bit: shader_paths 0, shader_format 112, flags 116, reserved 120. */
typedef struct BTBHGPUFrameThreadDescriptor {
    const char *shader_paths[BTBH_GPU_FRAME_SHADER_PATH_COUNT];   /* copied at the start */
    BTBHGPUShaderFormat shader_format;
    uint32_t flags;          /* 0 */
    uint32_t reserved[4];    /* 0 */
} BTBHGPUFrameThreadDescriptor;

struct BTBHGPUSceneView;     /* RFC 0014; incomplete until its phase 1 ships */

/* A scene in a frame: btbh_gpu_window_scene_present_instances_with_visibility's arguments. Everything it points at is
 * copied by btbh_gpu_window_frame_submit. 184 bytes on 64-bit: render_width 0, render_height 4, view_projection 8,
 * camera_right 72, camera_up 84, camera_position 96, clear_color 108, lighting 112, post 120, instances 128,
 * transient_instance_count 136, logical_instance_count 140, batches 144, scene_visibility 152,
 * directional_shadow_visibility 160, batch_count 168, view_count 172, views 176. */
typedef struct BTBHGPUFrameScene {
    uint32_t render_width;
    uint32_t render_height;
    float view_projection[16];
    float camera_right[3];
    float camera_up[3];
    float camera_position[3];
    uint8_t clear_color[4];                           /* red, green, blue, alpha */
    const BTBHGPUSceneLighting *lighting;
    const BTBHGPUScenePost *post;                     /* with its emitters, motions, occlusion samples */
    const float *instances;
    uint32_t transient_instance_count;
    uint32_t logical_instance_count;
    const BTBHGPUSceneGeometryBatch *batches;         /* with what they point at by value, and their poses' samples */
    const uint8_t *scene_visibility;                  /* NULL: every batch */
    const uint8_t *directional_shadow_visibility;     /* NULL: every batch */
    uint32_t batch_count;
    uint32_t view_count;                              /* phase 3 (RFC 0014); 0: the camera above */
    const struct BTBHGPUSceneView *views;             /* phase 3; NULL */
} BTBHGPUFrameScene;

/* A render into a target before the frame's scene: btbh_gpu_window_scene_render_to_target(_view). 224 bytes on 64-bit:
 * target 0, view 8, scene 40. */
typedef struct BTBHGPUFrameTarget {
    BTBHGPUTexture *target;                           /* a render target of this window */
    BTBHGPUSceneTargetView view;                      /* all zero: btbh_gpu_window_scene_render_to_target */
    BTBHGPUFrameScene scene;                          /* views must be NULL */
} BTBHGPUFrameTarget;

/* The frame's canvas: btbh_gpu_window_canvas_present's arguments, copied. 40 bytes on 64-bit. */
typedef struct BTBHGPUFrameCanvas {
    const BTBHGPUCanvasVertex *vertices;
    const BTBHGPUCanvasBatch *batches;
    const BTBHGPUCanvasPass *passes;
    uint32_t vertex_count;
    uint32_t batch_count;
    uint32_t pass_count;                              /* 1 or more */
    uint32_t reserved;                                /* 0 */
} BTBHGPUFrameCanvas;

enum {
    /* Keeps this frame's picture for btbh_gpu_window_readback_presented_frame (what
     * btbh_gpu_window_request_presented_frame_readback does for a canvas present). */
    BTBH_GPU_FRAME_READBACK = 1u << 0
};

/* One frame: its renders into targets, its scene, its canvas. 72 bytes on 64-bit: targets 0, scene 8, scene_copy 16,
 * canvas 24, target_count 64, flags 68. */
typedef struct BTBHGPUFrame {
    const BTBHGPUFrameTarget *targets;                /* drawn first, in order; the canvas may sample them */
    const BTBHGPUFrameScene *scene;                   /* NULL: none (the canvas alone, as a canvas present alone) */
    BTBHGPUTexture *scene_copy;                       /* NULL, or a render target of the drawable size: the finished
                                                         scene is copied into it before the canvas draws
                                                         (btbh_gpu_window_copy_pending_scene_texture's effect) */
    BTBHGPUFrameCanvas canvas;
    uint32_t target_count;                            /* 0 to BTBH_GPU_FRAME_MAX_TARGETS */
    uint32_t flags;                                   /* BTBH_GPU_FRAME_* bits; 0 */
} BTBHGPUFrame;

typedef struct BTBHGPUFrameSubmitReport {
    BTBHGPUResult result;               /* this call's own */
    uint32_t presented;                 /* 1: this call presented a frame (the one before) */
    uint64_t serial;                    /* this frame's, from 1; 0 when refused */
    uint64_t completed_serial;          /* the newest frame the thread has finished; 0: none */
    uint64_t failed_serial;             /* the newest frame that failed since the last report; 0: none */
    BTBHGPUResult failed_result;
    uint32_t reserved;
    double cpu_copy_milliseconds;       /* checking and copying this frame */
    double cpu_wait_milliseconds;       /* waiting for the thread to finish the frame before */
    double cpu_present_milliseconds;    /* presenting it: command buffer, swapchain image, blit, submit */
    char message[256];                  /* this call's refusal; else the failed frame's message; else empty */
} BTBHGPUFrameSubmitReport;

typedef struct BTBHGPUFrameResult {
    uint64_t serial;
    BTBHGPUResult result;               /* the first failure among its targets, scene, canvas and present */
    uint8_t encoded;                    /* 1: the thread has finished it */
    uint8_t presented;                  /* 1: its picture went to the swapchain (0: not yet, dropped, failed, or no
                                           swapchain image, as for a minimized window) */
    uint8_t reserved[2];
    uint32_t target_count;
    double cpu_encode_milliseconds;     /* the thread's time on it */
    BTBHGPUSceneReport scene;           /* as the present's report; zero without a scene */
    BTBHGPUSceneReport targets[BTBH_GPU_FRAME_MAX_TARGETS];
    BTBHGPUCanvasReport canvas;
} BTBHGPUFrameResult;

/* Starts (or wakes) the window's frame thread. Call it on the thread that made the window, between frames. Refused
 * with BTBH_GPU_INVALID_ARGUMENT: a NULL path, a non-zero flag or reserved word, a window not claimed, another window
 * of the device running one. The paths are copied. */
BTBHGPUResult btbh_gpu_window_frame_thread_start(
    BTBHGPUWindow *window,
    const BTBHGPUFrameThreadDescriptor *descriptor
);
/* Waits for the frame in flight, presents it, and parks the thread: the window then works as before. The report holds
 * that frame's outcome. The thread itself lives until the window is destroyed (SDL keeps a command pool for every thread
 * that ever acquired a command buffer until the device is destroyed: vulkan.c:9400-9460, freed at :4896). */
BTBHGPUResult btbh_gpu_window_frame_thread_stop(
    BTBHGPUWindow *window,
    BTBHGPUFrameSubmitReport *report
);
uint8_t btbh_gpu_window_frame_thread_running(const BTBHGPUWindow *window);

/* Hands a frame to the thread: checks it as the presents check their arguments and copies it; waits for the frame
 * before it to be encoded, presents that one; hands this one over and returns. The host may rewrite everything `frame`
 * points at once it returns. A refused frame (BTBH_GPU_INVALID_ARGUMENT, report->message) is not queued, and the frame
 * before is still presented. What fails on the thread comes back a frame late: in btbh_gpu_window_frame_result and in
 * the next report's failed_serial. */
BTBHGPUResult btbh_gpu_window_frame_submit(
    BTBHGPUWindow *window,
    const BTBHGPUFrame *frame,
    BTBHGPUFrameSubmitReport *report
);
/* Presents the finished frame now. wait 0: only if the thread has finished it (report->presented says); 1: waits for
 * it. For before a frame limiter's sleep, or a long job on this thread. */
BTBHGPUResult btbh_gpu_window_frame_present(
    BTBHGPUWindow *window,
    uint8_t wait,
    BTBHGPUFrameSubmitReport *report
);
/* The outcome of frame `serial` (0: the newest submitted), if it is among the last BTBH_GPU_FRAME_RESULT_CAPACITY.
 * wait 1 waits for the thread to finish it, and presents it. An unknown serial returns BTBH_GPU_INVALID_ARGUMENT. */
BTBHGPUResult btbh_gpu_window_frame_result(
    BTBHGPUWindow *window,
    uint64_t serial,
    uint8_t wait,
    BTBHGPUFrameResult *result
);

/* BTBHGPULazyCreationCall, appended: the window thread's present of a frame (its swapchain image and submit). */
    BTBH_GPU_LAZY_CREATION_CALL_FRAME_PRESENT = 4

/* Defined when the frame thread exists. */
#define BTBH_GPU_HAS_FRAME_THREAD 1
```

**Zero values.** No existing struct changes. A zeroed `BTBHGPUFrame` with a canvas pass is a canvas-only frame; a zeroed
`BTBHGPUFrameTarget.view` is `render_to_target`'s; zeroed `views` is one camera.

### Ownership: what v2 hands over, and what it must not touch

**Copied at `btbh_gpu_window_frame_submit`; free to rewrite when it returns:**
- the targets and their scenes, the scene, the canvas structs;
- lighting; post with its emitters, its temporal instance motions, and the occlusion samples whose revision changed;
- the instances, batches and both visibility arrays;
- what batches point at by value: world details, world-detail tables with their entries, height lifts;
- each pose's samples (so `btbh_gpu_animation_pose_set_samples` may run at once);
- the canvas's vertices, batches and passes.

**Referenced, not copied:** models, custom meshes, instance buffers, textures (render targets, heightfields, font
atlases, world-detail textures, the LUT), animation libraries, SDF terrains. The host must not destroy or change them
while a frame that uses them is in flight. Phase 1 makes that impossible rather than a rule to remember: **every call
that could, waits.**

**Calls that never wait (phase 1):**
- input and the window: `poll_events`, `begin_frame`, every `gamepad_*`, `key_*`, `mouse_*`, `set_mouse_position`,
  `set_cursor_visible`, `set_clipboard_text`, `show_file_dialog`, `btbh_gpu_window_sdl_window` (and the host's own SDL
  window calls), `btbh_gpu_monotonic_elapsed_seconds`;
- CPU-only queries: `btbh_gpu_font_glyph`, `_metrics`, `_kerning`, `_kerning_pairs`, `btbh_gpu_model_material_slot`,
  `_material_info`, `_mesh_info`, `btbh_gpu_animation_library_duration`;
- poses: `btbh_gpu_animation_pose_create`, `_set_samples`, `_destroy` (frames hold copies);
- logs: `btbh_gpu_window_lazy_creation_report` (locked already, `device.c:391-398`, `:514-523`) and
  `btbh_gpu_window_poll_frame_telemetry` (locked from phase 1b);
- the frame calls.

**Every other call waits:** for the frame the thread encodes; then it presents that frame (on this thread); then it runs
as without a thread. That covers creation, loading and destroying; every update (plain, NO_CYCLE and, in phase 1,
QUEUED); texture updates; renders into targets and prewarms outside a frame; the shadow periods and size; the present
mode, resize, minimize and restore; `present_clear`; readbacks; `set_frame_telemetry_enabled`; the SDF terrain calls;
the old presents (`scene_present_instances(_with_visibility)` and `canvas_present`, which then work unthreaded). All
calls belong on the thread that made the window, as today.

In short, for v2: after a submit returns, v2 may rewrite every buffer it passed. It may not change a GPU object a frame
in flight uses, and it can't: the call waits first. Phase 2 lets the queued uploads through without that wait.

Two more refusals: `btbh_gpu_device_set_upload_staging_budget` while a window of the device runs a frame thread (v2
sets it at start), and a second frame thread on one device. Destroy windows before their device, as today.

### Errors a frame late

1. **At the submit:** what the presents refuse from their arguments (`btbh_gpu_scene_validate_batches`,
   `scene.c:5018`; `_validate_world_details`, `:5201`; `btbh_gpu_scene_lighting_valid`, `:6693`; `_post_valid`,
   `:6867`; the canvas's `:450`, `:480`), plus the frame's own (counts, NULLs, `scene_copy`'s size, `views` before
   phase 3). Refused at once; nothing queued.
2. **On the thread:** shader files, pipelines, targets, SDL's failures, a pose the bones can't use. In
   `btbh_gpu_window_frame_result(serial)` and the next submit's `failed_serial`, `failed_result` and `message`. A host
   that only submits still sees every failure, one frame late.
3. **At the present (window thread):** a swapchain failure, today's `BTBH_GPU_WINDOW_FAILURE` (`canvas.c:1414-1427`).
   The frame's result says `presented` 0, and the next report carries it.
4. **A failed frame is dropped,** as a failed present is today. Its queued uploads still land
   (`btbh_gpu_upload_queue_finish_pending`, `canvas.c:628`), and the scene's temporal history restarts
   (`btbh_gpu_window_scene_submission_finished`, `canvas.c:716-719`). The next frame goes on.

The reports v2 reads right after the present today (`gpu.rs:2216-2247`: `last_costs`, `rc_status`) come a frame late,
by serial.

### Renders into targets, and views

- **In the frame:** `BTBHGPUFrame.targets`, run first on the thread through today's `render_to_target` internals
  (`scene.c:13510`), each submitted on its own as now (`scene.c:13710`). The frame's canvas may sample them. v2's split
  second view (`v2-game/src/split_view.rs:190`) goes here until RFC 0014.
- **Outside a frame:** the call waits, then runs on the host's thread, as today: its submission lands after the frame
  before and before the next. v2's v1 card previews can stay there (they are rare).
- **Views (RFC 0014):** `BTBHGPUFrameScene.views` and `view_count` in phase 3, once 0014's `present_views` exists. Then
  v2's split needs no target.
- **The target views' runtimes** are used by the thread during a frame and by the host only after a wait: never both.

### The lazy creation log, the cost log, frame telemetry

- **Lazy creation log.** The thread opens the scopes the presents open (scene, target, canvas). The log records one
  thread at a time already (`device.c:429-430`); with strict turns, the window thread's present step records too, under
  the new call kind 4 (its swapchain image and submit, today's slow calls `BTBH_GPU_LAZY_CALL_SWAPCHAIN_ACQUIRE` and
  `_FRAME_SUBMIT`). `frame_index` stays the window's submitted frames. The header's "Only the thread that presents is
  recorded" (`btbh_gpu.h:2909`) becomes "the frame thread, and the window thread's present step".
- **Cost log.** Runs on the thread as now. It waits for the GPU at each frame's start (`scene.c:1046`) and at each
  group (`scene.c:1063-1068`), so its frames never overlap: its GPU groups stay right, its CPU times mean little. The
  present's blit is in no group (about 0.02 ms at 1280 x 800). Measure CPU with the thread on, GPU with the cost log.
- **Frame telemetry.** Its fences ride the thread's submit (`canvas.c:664-700`). `poll_frame_telemetry` takes a lock.

### Shutdown

- **`frame_thread_stop`:** waits for the frame in flight, presents it, parks the thread. Results stay readable until the
  next start.
- **`frame_thread_start` again** wakes the same thread: no new SDL command pool (they are per thread and live until the
  device is destroyed, `vulkan.c:9400-9460`, `:4896`). So: start once per window, stop when the option changes, not per
  frame.
- **`btbh_gpu_window_destroy`** stops and ends the thread first.

## Minimal example

```c
BTBHGPUFrameThreadDescriptor ft;
SDL_zero(ft);
for (uint32_t i = 0u; i < BTBH_GPU_FRAME_SHADER_PATH_COUNT; ++i) {
    ft.shader_paths[i] = shader_paths[i];        /* the scene's eleven, then the canvas's three */
}
ft.shader_format = BTBH_GPU_SHADER_FORMAT_SPIRV;
if (btbh_gpu_window_frame_thread_start(window, &ft) != BTBH_GPU_OK) { /* run unthreaded */ }

while (running) {
    btbh_gpu_window_poll_events(window, &events);          /* never waits */
    btbh_gpu_window_begin_frame(window, &window_report, &snapshot);
    step_sim();
    build_scene(&scene);                                   /* the game's own buffers, reused every frame */
    build_hud(&frame_canvas);

    BTBHGPUFrame frame;
    SDL_zero(frame);
    frame.scene = &scene;
    frame.canvas = frame_canvas;
    BTBHGPUFrameSubmitReport submitted;
    if (btbh_gpu_window_frame_submit(window, &frame, &submitted) != BTBH_GPU_OK) {
        /* this frame was refused: submitted.message */
    } else if (submitted.failed_serial != 0u) {
        /* an earlier frame failed on the thread: submitted.message */
    }
    /* scene's and frame_canvas's buffers may be rewritten from here */
    if (frame_limiter_on) {
        btbh_gpu_window_frame_present(window, 1u, &submitted);   /* don't leave the frame waiting through the sleep */
        sleep_until_next_frame();
    }
}
btbh_gpu_window_frame_thread_stop(window, &submitted);
```

In v2, `Gpu::present` (`v2-platform/src/gpu.rs:1677`) becomes one submit: `present_scene` and `present_canvas` fill a
`BTBHGPUFrame` instead of calling the presents.

## Implementation

### Phase 1a: the present without window queries (no API; a patch)

- A frame context passed into the scene and canvas presents: the drawable and logical sizes and the swapchain's format.
  Unthreaded it is filled by the same SDL calls as now (`scene.c:8879`, `:9205`, the eight format queries;
  `canvas.c:932`, `:1474`).
- `btbh_gpu_window_canvas_present_internal` (`canvas.c:1304-1769`, 470 lines) split in two: draw (the scene's blit,
  the upload, the passes) into a target; present (command buffer, swapchain image, blit, submit). Unthreaded it calls
  both, acquiring the swapchain first as now, so frames stay the same.
- **Measure:** every smoke byte for byte against 0.15.0; the frame bench within noise.
- **Estimate:** 2-3 days. **Risk:** a regression in the most used path of every game; the identity runs guard it.

### Phase 1b: the frame thread

- `src/gpu/btbh_gpu_frame_thread.c` (new): the thread (`SDL_CreateThread`), two copy arenas (the copy of N + 1 overlaps
  the encode of N), the checks and copies, pose snapshots, the result ring, the wait (a mutex, two condition
  variables), the present step, the frame texture (`window->presented_frame`, always on). The frame pool in
  `src/gpu/btbh_gpu_frame.c` (`:100`, `:196`: arenas per slot with serials, used only by its test) may hold the copies.
- The header's declarations; the lazy log's call kind; a lock in `poll_frame_telemetry`.
- A one-line wait guard at the top of every waiting entry point (about 90). With no thread it is one pointer load.
- Debug switches: `BTBH_GPU_FRAME_THREAD=serial` runs each frame on the thread but waits for it inside the submit (A/B
  images, bisecting); `BTBH_GPU_FRAME_THREAD_LOG_WAITS=1` logs every call that waited, with its name and time, so v2
  finds its per-frame waits.
- **Measure:** v2's Deck bench (stage-3 fight, late run, boss, Tower Offense, USA, split, title) with the thread on and
  off, hitch logs and the sampler. Expected 2.4 -> 1.35-1.45 ms where this PC's GPU allows. The frame bench with a
  synthetic host load (a 1 ms spin): the period should be the longer half plus about 0.1 ms.
- **Estimate:** 5-8 days; v2's adoption 1-2 days.

### Phase 2: uploads without waits

- QUEUED mesh updates (whole and range) and QUEUED instance uploads stop waiting. A mesh or instance buffer keeps two
  views: the host's (counts, bounds, uploaded prefix, for its reports and the submit's checks) and the frames' (what an
  encode draws). The thread applies the queue at a frame's start, growing buffers there. The window's queue is swapped
  into the frame at the submit (one pointer swap).
- A QUEUED texture update (`btbh_gpu_texture_update_rgba8_ex`) for v2's minimap.
- `set_shadow_update_periods` taken at the next frame.
- **Measure:** frames with kit damage (v2's `uploads` column above 0) keep the overlap; `LOG_WAITS` lists no per-frame
  call.
- **Estimate:** 3-5 days.

### Phase 3: views in a frame; borrowed frames

- `BTBHGPUFrameScene.views` once RFC 0014 phase 1 ships. Measure: the split at the Deck preset (today 3.79 ms).
- A borrow flag (no copy until the frame's result), only if the copy shows up in v2's profile.
- **Estimate:** 1-3 days after RFC 0014 phase 1.

### Beside it: (d)

The bones fix (in progress), RFC 0011 phase 2 when v2 asks, prepare kept between frames. Each shortens the frame
thread's half; none depends on this RFC.

### Cost

| Item | Estimate |
| --- | --- |
| The window thread, per frame | the copy 0.03-0.06 ms; the present step 0.05-0.15 ms |
| The frame thread, per frame | today's scene and canvas encode; the copies into transfer buffers again (the instances and vertices: 0.01-0.03 ms) |
| GPU | one blit at the drawable size: 0.02 ms at 1280 x 800, 0.1-0.15 ms at 4K |
| Memory | the frame texture (4 MB at 1280 x 800, 33 MB at 4K), two arenas (1-3 MB at v2's sizes) |
| Threads | one more busy thread beside v2's main thread and the driver's (about 10 % here) |

### Risks

- **A missed wait guard is a race.** Mitigation: a test that calls every public function while a frame is in flight,
  with the Vulkan validation layer's thread-safety checks on; `serial` mode for A/B.
- **SDL state this RFC didn't find.** The same validation runs; SDL used by one thread at a time by design.
- **The finished frame waiting for the next submit** (limiters, long jobs). Documented; `frame_present`; every wait
  presents.
- **Latency under V-Sync** (up to one frame). v2's choice (question 3).
- **Errors and reports a frame late** are harder to debug. `serial` mode gives them back at the call.
- **The present refactor (phase 1a)** touches every game's frame. The identity runs, v1's Swift suite, CHIMERA's
  0-pixel check.

## Compatibility

- **Versioning:** minor. New functions, structs, macros and an appended enum value (`BTBH_GPU_LAZY_CREATION_CALL_FRAME_PRESENT`
  4). No existing struct changes; no default changes. Phase 1a is a patch that keeps every image.
- **Be The Bullet Hell v2 (Rust, bindgen):** regenerated bindings. `Gpu::present` builds a `BTBHGPUFrame`; `hitch.rs`
  reads the renderer's parts from the frame results by serial; the frozen backdrop uses `scene_copy`; screenshots use
  `BTBH_GPU_FRAME_READBACK`; a Graphics row turns the thread on.
- **Be The Bullet Hell (v1, Swift), CHIMERA (Rust), Tyrian3D (C):** nothing to change. They compile the new file with
  `src/gpu/*.c` as every release; no new shader.

## Platforms and testing

- **Native:** the structs' sizes and offsets above in `tests/btbh_gpu_scene_order_test.c`; the header on its own as C11
  and C++17 (`btbh_gpu_public_header_test.c`), with the incomplete `struct BTBHGPUSceneView`.
- **GPU smoke, Windows/Vulkan** (new `btbh_gpu_frame_thread_smoke`):
  - **Identity:** the flags, prewarm, world-detail, target, target-view, runs and lit-particle smokes through
    `frame_submit` against the presents, byte for byte (read back with `BTBH_GPU_FRAME_READBACK`).
  - **Ownership:** the host fills every buffer it passed with garbage right after the submit; the image must not change.
  - **Waits:** a mesh update, a texture update and a destroy after a submit wait and land in the next frame.
  - **Errors:** a refused argument at the submit; a missing shader path in the next report's `failed_serial`.
  - **Life:** stop and start; destroy with a frame in flight; a minimized window (frames encoded, nothing presented, no
    error); a resize between a submit and its present (the blit scales once).
  - With the Vulkan validation layer, thread-safety checks on.
- **macOS/Metal:** compiles with the release; not run. No game on Metal opts in.
- **Linux, the Steam Deck and the Machine:** through v2 when it runs there. (b) keeps every window and swapchain call on
  the window's thread, so X11 and Wayland see what they see today.
- **Games:** v2's Deck bench with the thread on and off; CHIMERA 0 pixels different; v1 its Swift suite.

## Alternatives

| Alternative | Why not |
| --- | --- |
| **(a) The render thread presents too** | Breaks SDL's rule for the swapchain (`SDL_gpu.h:4303`); races the resize watch (`vulkan.c:9653` against `:10007`); a deadlock risk with exclusive fullscreen and `SDL_SyncWindow` on Windows, untested. Same gain as (b) |
| **(c) Passes on several threads** | 0.05-0.15 ms; SDL can't split a render pass; prepare and v2's half stay serial |
| **(e) v2 owns the thread** | Viable and needs nothing from the renderer; costs v2 a restructured frame loop. Question 1 |
| **Borrowing host memory until a fence** | Saves 0.03-0.06 ms; costs v2 double buffers and makes a slip a wrong picture. Phase 3 if it shows |
| **Two hand-off slots** | More latency and memory; no throughput, since the halves are equal |
| **Recording on the frame thread, submitting on the window thread** | Outside SDL's rules (`SDL_gpu.h:3142-3144`); per-thread command pools (`vulkan.c:9400-9460`) |
| **Making the window on the render thread** | Window messages go to the thread that made the window, and `SDL_PollEvent` is the main thread's (`SDL_events.h:1296`) |
| **Patching SDL's defragmentation** | Would let the present overlap the encode; a fork of SDL that every game builds or links. Strict turns cost about 0.1 ms of the frame thread instead |

## Decisions

Be The Bullet Hell v2 answered all 12 questions through its session on 2026-11-27:

1. **Design (b): the renderer owns the thread.** v2's frame code stays single-threaded.
2. **Where it must pay off: the Steam Machine.** v2 first measures the Deck preset on its Windows PC with the CPU capped
   (affinity and a clock limit), uncapped and at 60 Hz. A run on the Machine needs its owner; v2 asks him before phase
   1b. Phase 1b starts on those numbers.
3. **"Auto" with one change:** on when the last 120 frames were CPU-bound **and missed their target frame time**,
   whatever set the target (V-Sync, the limiter or none). On the Machine's 60 Hz TV with V-Sync, a CPU frame over
   16.7 ms still misses 60, which is where the thread should help. Off at 4K, where the GPU limits. With a limiter v2
   calls `frame_present(window, 1)` before sleeping.
4. **Copy at the call:** yes.
5. **At most one frame ahead:** yes.
6. **Reports a frame late, matched by serial** (`btbh_gpu_window_frame_result`): yes; v2 adapts hitch.rs, `rc_status`
   and `last_costs`.
7. **Calls that wait in phase 1:** accepted until phase 2. v2 sends the list from `BTBH_GPU_FRAME_THREAD_LOG_WAITS=1`.
8. **The split's second view in the frame's targets** until RFC 0014's views: yes.
9. **Long jobs:** `frame_present(window, 1)` before a stage's build. v2 is also moving the stage start's scene build
   (127-205 ms) to a worker and removing the HUD's 23 ms frames.
10. **The frozen backdrop and screenshots:** to `scene_copy` and `BTBH_GPU_FRAME_READBACK`, the backdrop's target made
    once at a pause.
11. **Measuring:** CPU with hitch.rs and the sampler with the thread on; GPU with the cost log.
12. **Starting and stopping:** once per window; stop only when the option changes.
