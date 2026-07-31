# Split Scene / Swapchain Submission Implementation Plan

Design authority:
`docs/superpowers/specs/2026-07-31-split-scene-swapchain-submission-design.md`

1. Add a failing GPU source-contract test requiring scene submission to avoid
   swapchain acquisition and Canvas to own late acquisition/composition.
2. Add explicit submitted-scene state and final-color storage to the native
   GPU window/scene runtime.
3. Allocate the final-color target at drawable size and swapchain format.
4. Replace scene swapchain acquisition with a drawable pixel-size query.
5. Route all final post/AA output to final color and submit the scene command
   immediately.
6. Make Canvas always acquire its own command buffer/swapchain image, blit a
   pending scene before screen UI, and finalize the scene-ready lifecycle.
7. Run focused renderer tests, full renderer tests with GPU tests enabled, full
   game tests, release build, API audit, and `git diff --check`.
8. Run a warmed gameplay-only uncapped Vulkan measurement using the unchanged
   seed/frame/profile and compare phase plus native medians with the saved
   OpenGL baseline.
9. Preserve the change only if it is a measured win, then profile and implement
   the next largest Vulkan bottleneck until the overall goal is met.
