# Lazy Directional-Shadow Resources Design

Status: approved automatically on 2026-07-28.

## Goal

Remove the measured GPU runtime/resource-residency regression from scenes
that never enable directional shadows, without changing pixels, public
renderer semantics, shader contracts, or the active-shadow path.

A scene runtime whose two directional shadow slots are disabled must create
only the four-sampler unshadowed static/animated model pipelines and common
scene resources. It must not load shadow shaders or create:

- six-sampler shadow-capable model pipeline families;
- static or animated depth-only shadow pipelines;
- either 2048x2048 D32 shadow map;
- the directional-shadow sampler.

## Lifecycle

The first scene submission with either directional shadow slot enabled
creates the complete shadow-only resource set before command-buffer
acquisition. Later shadowed or unshadowed submissions reuse that set until
`btbh_gpu_window_release_scene`.

Resources deliberately remain resident after a shadow has first been enabled.
Releasing and recreating them on a per-frame light toggle would trade steady
state memory for visible frame hitches and driver allocation churn. A window
that never enables shadows receives the intended zero-shadow-resource path.

Shadow initialization remains fail-closed. Any shader, pipeline, texture, or
sampler failure releases the complete scene runtime through the existing
cleanup path, reports the exact device failure, and permits a clean later
retry. Partial resources may never become observable to rendering.

## Structure

Split native scene setup into:

1. base runtime setup, which creates only unshadowed model pipelines and
   common fallback resources;
2. `ensure` logic for all shadow-only pipelines, maps, and sampler, invoked
   only when the validated lighting payload has an enabled shadow slot.

The scene submission already receives all shader paths on every call, so the
lazy initializer requires no new caller ownership or cached path storage.
A package-internal native query exposes whether the complete shadow resource
set is resident without changing the public C scene-report layout. This is
diagnostic test surface only; the public C ABI and backend-neutral Swift
renderer API remain unchanged.

## Acceptance

- A real Windows Vulkan no-shadow presentation reports no resident
  shadow-only resources.
- The first real shadowed presentation reports the complete resource set and
  the existing receiver/caster regression still proves visible shadowing.
- Subsequent submissions reuse the resource set and normal shutdown releases
  it once.
- Native `/W4 /WX`, CTest, renderer Swift tests, game tests, release build,
  render API audit, and diff checks remain green.
- A warmed five-mode Windows capture diagnoses whether no-shadow editor
  median/p95 values return to the preceding same-machine control range.
- Goldens, visual thresholds, entity authority, and the documented
  Windows-to-Apple SSIM issue remain unchanged.
