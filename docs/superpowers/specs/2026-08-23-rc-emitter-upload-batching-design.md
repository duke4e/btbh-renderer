# RC Emitter Upload Batching Design

Status: implemented

Date: 2026-08-23

## Goal

Remove the periodic RC CPU preparation hitch and one avoidable GPU submission
per frame without changing emitter data, HRC output, or frame ordering.

## Evidence

- The post-instrumentation profile isolated a 13.33 ms outlier inside RC
  resource preparation while stable HRC command encoding remained 0.09 ms.
- `btbh_gpu_radiance_cascades_ensure_emitter_buffer` currently creates and
  releases an upload transfer buffer, acquires a second command buffer, records
  a copy pass, and submits it on every RC frame.
- The main scene command buffer is acquired later and already has a legal copy
  pass boundary immediately before emitter/source-field rendering.

## Design

1. Keep a capacity-managed emitter upload transfer buffer in the RC runtime.
2. During preparation, map it with cycling enabled, write the current emitter
   records, unmap it, and retain the byte count. Do not submit GPU work there.
3. At the start of emitter encoding, record the transfer-to-storage-buffer copy
   into the existing scene command buffer before opening source-field render
   passes. Keep destination cycling enabled.
4. Release the persistent transfer buffer with the rest of the RC runtime and
   preserve the zero-emitter path.

## Acceptance criteria

- No emitter-specific `SDL_AcquireGPUCommandBuffer` or
  `SDL_SubmitGPUCommandBuffer` remains in RC preparation.
- The production HRC frame still reports the same dispatch counts and emitter
  showcase signature.
- The production Release build and focused RC tests pass.
- A repeated 300-frame no-readback profile retains visual output and does not
  reproduce the measured 13 ms RC resource-preparation stall.

## Result

- RC preparation now writes into a capacity-managed persistent transfer buffer
  and records its upload in the existing scene command buffer.
- Two deterministic 300-frame HRC High profiles reproduced no preparation
  hitch. `rc_cpu_resource_prepare_ms` measured 0.0016/0.0022 ms median,
  0.0029/0.0108 ms maximum, and zero samples above 1 ms. The instrumented
  pre-change run measured 0.124 ms median, 13.3272 ms maximum, and two samples
  above 1 ms.
- Frame metrics remained stable at 4.200 ms median and 4.600 ms p95; worst
  frame improved from 16.488 ms to 4.820/4.822 ms.
- HRC dispatch counts remain 3 trace, 6 merge-up, 8 merge-down, and one each
  for finish, blur, resolve, and compose.
- The deterministic visual baseline finished successfully with exact matching
  entity and cue signatures. Manual comparison found no visual change outside
  the time-dependent FPS overlay.
