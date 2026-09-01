# RC Occlusion Upload Batching Design

Status: implemented and verified on Windows

Date: 2026-08-23

## Evidence

The static HRC profile proved stable emitter uploads, but the occlusion-revision
path still creates a transfer buffer, acquires a command buffer, submits it, and
releases staging memory synchronously from RC preparation. The game changes the
near-field revision whenever the camera crosses a snapped 16-cell origin or
destructible geometry changes, so the static profile did not cover this path.

## Design

- Keep one capacity-managed transfer buffer per occlusion cascade.
- Preparation maps/cycles staging and records pending dimensions and revision;
  it performs no GPU submission.
- The existing pre-source copy pass uploads all pending occlusion fields and the
  emitter buffer in the main scene command buffer.
- Commit a revision only after its copy has been encoded. A failed/cancelled
  frame therefore retries instead of silently treating stale GPU data as current.
- Release staging with the RC runtime and preserve cached uploads while revision
  and dimensions are unchanged.

## Acceptance

- No `SDL_AcquireGPUCommandBuffer` or `SDL_SubmitGPUCommandBuffer` remains in
  occlusion preparation.
- Multiple pending cascades and the emitter buffer share one copy pass.
- Unchanged revisions produce no occlusion upload.
- Focused tests, Release build, an origin/revision stress profile, and a
  deterministic HRC capture pass.

## Verification result

- Twelve focused renderer tests passed.
- The production Release game build completed and ran through Vulkan.
- A 300-frame profile forced a revision upload every 30 frames. RC resource
  preparation measured 0.002 ms median, 0.003 ms p95, and 0.0288 ms maximum,
  with zero samples above 0.1 ms. The same path previously reached 13.327 ms.
- The HRC dispatch signature remained `3/6/8/1/1/1/1`; the stress run measured
  4.2 ms median, 5.1 ms p95, and 5.752 ms worst frame.
- A no-stress 1920x1080 deterministic capture completed with the expected 101
  projectile entity signature and no visible upload-path regression.
