# Native render-pass pipeline-bind cache design

Status: approved

Outcome: retained. W291/W292 skipped approximately 734 shadow and 425
geometry pipeline binds per final frame while issuing only 12 and 19-21 real
binds. Shadow encode fell from `0.465 ms` in W288 to `0.277-0.280 ms`;
geometry encode fell from `0.423 ms` to `0.213-0.215 ms`; aggregate native
encode fell from `1.352 ms` to `0.960-0.964 ms`. The repeated frame mean was
`18.088/23.285 ms` median/p95, `0.63%/1.93%` faster than W287/W288, with
unchanged entity, shutdown, resolution, and visual gates.

## Evidence

W287/W288 leave approximately `0.47 ms` in native shadow encoding and
`0.42 ms` in native geometry encoding for roughly 400 submitted batches.
Both native loops currently call `SDL_BindGPUGraphicsPipeline` for every
drawn geometry, including consecutive draws that select the same immutable
pipeline.

SDL's render-pass contract requires a pipeline before drawing and defines
vertex/index bindings for subsequent draw calls. State therefore persists
within one render pass; rebinding the exact same pipeline pointer without an
intervening different bind is redundant.

## Design

Track the last graphics-pipeline pointer independently inside each shadow and
geometry render pass. Call `SDL_BindGPUGraphicsPipeline` only when the selected
pointer changes. Reset the cache at every `SDL_BeginGPURenderPass`.

Do not cache uniforms, textures, samplers, storage buffers, vertex buffers,
index buffers, or stencil state in this candidate. Those remain unchanged so
the experiment isolates one state transition.

Expose actual and skipped pipeline-bind counts through opt-in scene telemetry.
The pointer comparison implements the cache in both paths; counter bookkeeping
remains disabled when frame telemetry is off.

## Validation

1. A production Vulkan presentation test with repeated compatible draws must
   report at least one skipped bind while preserving the draw count.
2. Existing material, shadow, and real-window tests remain green.
3. Gameplay-only capture must show non-zero skipped binds and reduced
   shadow/geometry encode or scene-submit time.
4. Retain only if repeated median/p95 improves without changing entity,
   shutdown, resolution, or visual gates.
