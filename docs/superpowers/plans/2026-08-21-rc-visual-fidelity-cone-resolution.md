# RC Visual Fidelity Implementation Plan

Status: In progress

- [x] Capture the current `finalIndirect`, cascade 0, confidence, legacy, and final composite evidence from the same deterministic game frame.
- [ ] Replace the rejected emissive cone footprint with a compact spot emitter plus a 32-bin directional source atlas sampled by trace and merge.
- [x] Feed authored sky irradiance SH into RC resolve through cascade-derived open-sky visibility.
- [x] Upgrade only the high tier to one-cell base probes and a full-resolution resolve; keep low/auto fallback behavior intact.
- [x] Extend C and Swift shader-contract tests for the new cone and tier invariants, then synchronize the game vendor copy.
- [ ] Add and test a quiet deterministic capture scenario so visual A/B evidence is not hidden by the Growth 5 stress loadout.
- [ ] Rebuild shaders/renderer/game, run unit and live-GPU verification, recapture the same A/B/debug views, and reject remaining circular, desaturated, or grid-like output.
