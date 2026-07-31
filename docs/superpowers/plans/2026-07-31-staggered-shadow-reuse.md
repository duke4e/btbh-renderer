# Staggered Shadow Reuse Implementation Plan

Status: completed

Date: 2026-07-31

1. [x] Add native runtime state for retained shadow matrices and activation.
2. [x] Prove period-two scheduling behind a temporary opt-in experiment.
3. [x] Stagger active shadow updates and use retained matrices for shading.
4. [x] Run shadow-active GPU tests at periods one and two.
5. [x] Build and measure seeded uncapped gameplay.
6. [x] Replace the experiment with the typed per-light
   `shadowUpdatePeriodFrames` API after it beat same-source OpenGL cleanly.
7. [x] Preserve default period-one behavior and validate the 1...60 range.
