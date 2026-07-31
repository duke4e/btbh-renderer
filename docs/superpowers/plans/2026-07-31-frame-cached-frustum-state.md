# Frame-Cached Frustum State Implementation Plan

Status: completed

Date: 2026-07-31

1. [x] Add a failing focused test for a reusable frustum state.
2. [x] Precompute camera basis and projection terms once per frame.
3. [x] Route scene frustum queries through the cached state.
4. [x] Preserve fail-closed invalid bounds before and during a frame.
5. [x] Run focused GPU tests and the game packing tests.
6. [x] Build release and compare seeded uncapped chunk-160 gameplay.
7. [x] Keep the measured total-frame win.
