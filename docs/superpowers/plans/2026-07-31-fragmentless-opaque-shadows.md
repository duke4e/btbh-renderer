# Fragmentless Opaque Shadows Implementation Plan

Status: rejected after controlled Vulkan runtime test

Date: 2026-07-31

1. Add a failing source-contract test for fragmentless non-discard shadows.
2. Stop loading the empty fast shadow fragment shader.
3. Create static, fast-static, and animated non-discard pipelines with a null
   fragment shader while preserving discard pipelines.
4. Run focused and real-GPU tests.
5. Rebuild the GPU-enabled release game and measure seeded uncapped gameplay.
6. Keep the change only if the measured result is neutral or faster.

Result: the first gameplay launch raced a still-active detached Windows Swift
frontend and was invalid. In the controlled retest, the fragmentless candidate
hung the shadow-active Vulkan test for more than 90 seconds, while the restored
fragment-shader control passed in 0.381 seconds. The candidate was removed.
