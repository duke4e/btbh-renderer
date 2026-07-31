# Screen-shader kind cache design

Status: approved

Outcome: retained. W286 reduced adapter prepare from approximately `0.598 ms`
to `0.080 ms` and screen-post resolution from `0.517 ms` to approximately
`0.0026 ms`. Repeated W287/W288 gameplay captures produced an
`18.203/23.743 ms` median/p95 mean, `4.89%/3.29%` faster than beam-only
W277/W278, with unchanged entity, shutdown, resolution, and visual gates.

## Evidence

W285 measures `scenePost` at `0.517 ms` of the `0.522 ms` combined
lighting/post phase. The current implementation creates a Foundation `URL`,
removes its extension, extracts its last component, and lowercases it for
every screen shader on every frame.

## Design

Classify each immutable fragment path once when its screen-shader resource is
created. Store an internal kind (`lut`, `cas`, `tiltShift`, or unsupported
name) beside the parameters. `scenePost` switches on the cached kind.

Unsupported shaders continue to be accepted at resource creation and fail
during scene submission with the same name, preserving existing error timing.
Parameter updates do not invalidate the kind because the fragment path is
immutable.

## Validation

1. Path classification tests cover case folding, Windows paths, and unknown
   shaders.
2. Existing GPU resource/platform tests pass.
3. Warmed gameplay A/B must materially reduce `post_resolve_ms`; otherwise
   revert the cache.
