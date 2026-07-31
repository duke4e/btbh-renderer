# Scene shader-path cache design

Status: retained after gameplay A/B

## Evidence

`GPUPlatform.presentModelInstances` currently reads the process environment,
standardizes the executable URL, resolves the backend extension, constructs
eleven child URLs, and materializes eleven path strings on every scene frame.
The backend request, executable location, and optional shader-directory
override are immutable for the lifetime of a platform instance.

This work sits in the approximately `1.6 ms` Swift/platform portion of native
presentation that is not included in the measured C encode.

## Design

Resolve a `GPUSceneShaderPaths` value once in `GPUPlatform.init`. It stores all
eleven scene shader paths and the native shader format. Scene presentation
passes those immutable strings directly to `GPUWindowProbe`.

Preserve the current precedence exactly: a non-empty
`BTBH_GPU_SHADER_DIR` override wins; otherwise use `GPUShaders` next to the
standardized executable path. Vulkan uses SPIR-V and Metal uses metallib.

## Validation

1. Unit tests cover override and executable-relative path resolution.
2. Existing platform and real Vulkan presentation tests remain green.
3. Gameplay-only A/B must reduce platform/native presentation or total frame
   time without changing scene counts.
4. Retain only with canonical entities, clean shutdown, exact resolution, and
   unchanged visual gates.

## Outcome

Retained. Back-to-back candidate/control pairs W304/W305 and W306/W307
produced identical canonical gameplay entity signatures. The two-run means
showed:

- platform/native wrapper overhead: approximately `1.635 ms` to `0.066 ms`
  (`-95.95%`);
- full platform/native presentation: `-59.83%`;
- frame median: `-7.43%`;
- frame p95: `-6.58%`.

Native C encode was effectively unchanged apart from ambient run drift; the
gain comes from eliminating per-frame Foundation environment, URL, and path
materialization.
