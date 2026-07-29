# Directional Shadow Map Budget Plan

Status: approved automatically on 2026-07-30.

1. Change the Vulkan/Metal production directional shadow target from 2048 to
   512 while leaving the spot target unchanged.
2. Build the release game and run the deterministic 1920x1080 gameplay
   capture.
3. Compare median/p95 and image differences against the 2048 full-shadow and
   spot-only diagnostics.
4. Run the full renderer and game verification gates.
5. Run two warmed five-mode capture series without changing goldens or
   thresholds.
6. Update parity documentation, independently review the diff, commit and
   push the renderer first, then update and commit the game submodule gitlink.
