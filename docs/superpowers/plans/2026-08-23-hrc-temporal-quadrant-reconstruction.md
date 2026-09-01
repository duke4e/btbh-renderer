# HRC Temporal Quadrant Reconstruction Plan

Status: complete

Date: 2026-08-23

Design:
`docs/superpowers/specs/2026-08-23-hrc-temporal-quadrant-reconstruction-design.md`

1. [x] Add native temporal state, public pass telemetry, source-contract tests, and
   deterministic phase/origin helpers.
2. [x] Add the HRC history-reprojection compute shader, reflection/build wiring, and
   opacity/disocclusion rejection coverage.
3. [x] Make Finish support a full refresh or selected layers and preserve the
   reprojected work atlas during partial writes.
4. [x] Integrate cold-start/invalidation and successful-frame state commits into the
   existing HRC main-command sequence, then mirror Vendor byte-for-byte.
5. [x] Build Release and compare static, moving-origin, and dynamic-light telemetry
   and captures against the non-temporal baseline.

Implementation begins automatically under the approved roadmap.
