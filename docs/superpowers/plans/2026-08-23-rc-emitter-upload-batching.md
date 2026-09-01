# RC Emitter Upload Batching Plan

Status: complete

Date: 2026-08-23

1. Add persistent emitter upload-buffer state and focused source-contract
   regression coverage.
2. Move emitter copy encoding from preparation into the existing scene command
   buffer and preserve zero-emitter behavior.
3. Mirror the renderer into the game Vendor, build Release, and run the
   no-readback HRC profile plus deterministic visual capture.
4. Compare CPU preparation outliers, frame metrics, and exact HRC dispatch
   counts against the profiling baseline.

Invariant: no extra fence, wait, submission, visual change, or emitter limit.
