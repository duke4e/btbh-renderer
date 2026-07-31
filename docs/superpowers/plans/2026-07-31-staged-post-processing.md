# Staged Post-Processing Implementation Plan

Status: rejected after benchmark

Date: 2026-07-31

1. Add a failing source-contract test for the staged shader and native pass.
2. Add the HDR LUT/CAS pre-post shader to the cross-platform shader build.
3. Add the pre-post pipeline and size-dependent HDR texture to the scene runtime.
4. Encode the pass only when LUT or CAS is enabled and feed its output to the
   simplified production post shader.
5. Release all new GPU resources through the existing scene teardown path.
6. Build shaders and the GPU-enabled release game.
7. Run focused and real-GPU tests, then gameplay-only telemetry and a fresh GPU
   capture.
8. Keep the change only if measured frame time is neutral or improved and visual
   captures show no unacceptable regression.

Result: the test-first implementation passed functional verification but
regressed warmed gameplay frame time to 13.8-16.8 ms, so the staged runtime and
shader changes were reverted.
