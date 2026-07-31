# Shadow performance isolation plan

1. [x] Verify the active game, nested renderer, and standalone renderer remain
   clean on `codex/renderer-phase-0-1`, and verify historical external
   submodule pins match the current checked-out dependencies.
2. [x] Materialize exact `c31c593e` game and `7553484` renderer archives in an
   ignored diagnostic directory without switching branches or using a
   worktree.
3. [x] Build the historical native GPU runtime, production shaders, and release
   game executable; prove hardware Vulkan bootstrap.
4. [x] Capture the five historical modes and one fresh current-source
   five-mode control with identical seed, 1920x1080 output, and unchanged
   comparison authority.
5. [x] Validate entity signatures, lifecycle markers, visual cues, CPU-submit,
   fence-completion, and presented-frame distributions.
6. [x] Add package-internal scene and canvas CPU timing probes, reject the
   disproved shadow-uniform-tail candidate, and attribute the shadowed-mode
   CPU delta to the two new directional shadow passes.
7. [ ] Run proportionate verification, independent review for the internal
   telemetry change, update the Windows parity report, and preserve renderer-
   before-game commit ordering.
