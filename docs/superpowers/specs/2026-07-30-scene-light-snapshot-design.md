# Scene light snapshot design

Status: approved

Outcome: rejected after W285. `sceneLighting` measured only `0.0038 ms`
median; the single-sort change was neutral and was reverted.

## Evidence

W284 attributes `0.521 ms` of the `0.595 ms` Swift adapter prepare median to
lighting, clear-color, and post resolution. `sceneLighting` currently sorts
the same light dictionary twice per frame before selecting directional and
spot lights.

## Design

Create one handle-ordered light value snapshot per scene-lighting resolution.
Use it for both directional filtering and spot selection. Handle ordering and
all primary/secondary/spot selection rules remain unchanged.

Add opt-in timestamps around the individual lighting, clear-color, and post
calls so the candidate can prove whether this is the dominant part of the
combined phase.

## Validation

Existing directional/secondary/spot selection and shadow projection tests must
pass. Retain the single-sort change only if warmed gameplay telemetry improves
the lighting phase and repeated frame measurements do not regress.
