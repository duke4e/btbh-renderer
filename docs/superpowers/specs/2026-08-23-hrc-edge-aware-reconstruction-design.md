# HRC Edge-Aware Reconstruction Design

Status: implemented and verified on Windows

Date: 2026-08-23

## Problem

The HRC resolve currently writes the selected tier's internal resolution and the
compose pass samples that texture with ordinary linear filtering. Low therefore
stretches a quarter-width/quarter-height lighting solution across the render
target. High avoids that screen-resolution loss, but still exposes the discrete
512-square transport field because the only spatial filtering is the small
visibility-aware cross used by temporal quadrant reconstruction.

## Decision

Add a dedicated full-resolution HRC reconstruction compute pass between material
resolve and compose. It follows joint bilateral upsampling: low-resolution HDR
lighting is reconstructed using high-resolution depth and normal guides, with
the resolved RC confidence/visibility channel as an additional rejection term.
The same pass runs at High as a narrow edge-aware smoothing pass, so both tiers
share one deterministic reconstruction contract.

The filter gathers a bounded 3x3 low-resolution neighborhood. Each contribution
is weighted by:

- distance from the continuous source coordinate;
- normal agreement with the full-resolution destination surface;
- destination-plane distance reconstructed from depth and inverse view-
  projection;
- agreement with the center RC confidence/visibility value.

Background pixels remain zero in the reconstruction target and compose retains
the original scene background. If no neighbor survives, the bilinear source
sample is used as a finite fallback. No tone mapping or clamping is introduced;
the texture remains linear `RGBA16F` HDR.

This is based on the reconstruction principle from Kopf et al., *Joint
Bilateral Upsampling* (2007), with the geometry/normal edge stops used by modern
spatiotemporal lighting filters such as Schied et al., *SVGF* (2017). It is not a
generic blur and does not replace temporal HRC reconstruction.

## Resources and telemetry

- Add one render-resolution `RGBA16F` HRC reconstruction texture.
- Add one compute pipeline with three sampled textures and one storage output.
- Compose samples the reconstructed texture for HRC only; Classic is unchanged.
- Expose CPU encode time and dispatch count for the reconstruction pass.
- Include the shader in reflection tests and Windows/Linux packaging lists.

## Acceptance

- Low no longer uses ordinary linear upscale as its final lighting
  reconstruction.
- Light does not visibly cross depth/normal silhouettes in deterministic Low and
  High captures.
- Visibility discontinuities remain sharper than smooth lighting gradients.
- Debug final-indirect view shows the reconstructed output.
- Release shader reflection, focused tests, production build, live Vulkan
  telemetry, and deterministic High/Low captures pass.
- The added pass has a measured, recorded Windows cost; no screenshot run is
  used for performance evidence.

## Verification result

- Thirteen focused renderer tests pass, including the shader-source contract and
  zero-valued public telemetry contract. Native SPIR-V reflection confirms three
  samplers and one read/write storage texture for the reconstruction shader.
- The production Release game builds and packages the new shader. Live Vulkan
  telemetry reports HRC active with no fallback and exactly one reconstruction
  dispatch per successful frame; CPU dispatch encoding is approximately
  0.002 ms in the measured steady state.
- Repeated 300-frame no-readback High runs measure 3.893/3.929 ms average,
  3.8/3.9 ms median, and 4.4/4.3 ms p95. The matched Low run measures 3.825 ms
  average, 3.7 ms median, and 4.4 ms p95. The earlier pre-reconstruction High
  reference was 4.270 ms average and 4.6 ms p95, so the pass does not create a
  measured frame-time regression on this Windows configuration.
- Deterministic 1920x1080 High and Low captures are visually close; Low no
  longer exposes its quarter-linear-resolution texel grid at geometry edges.
  The occupancy confidence view separates buildings from ground while depth and
  normal stops preserve silhouettes without a broad cross-edge halo.
