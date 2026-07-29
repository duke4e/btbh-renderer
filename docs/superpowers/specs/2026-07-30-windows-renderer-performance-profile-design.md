# Windows Renderer Performance Profile Design

**Status:** Approved

## Goal

Explain the current Windows Game and Combat frame-time gap between the retained
legacy raylib/r3d/OpenGL backend and the production SDL_GPU/Vulkan backend, then
fix only high-impact problems supported by direct measurements.

The investigation must attribute the expensive parts of a warmed frame before
changing rendering behavior. Switching graphics APIs, reducing visual quality,
changing approved goldens, or weakening parity thresholds is outside this
profile.

## Fixed comparison contract

Every retained comparison uses:

- game commit `861eacb9d82148e46e35f1d31ffc7a26644cba4f`;
- renderer commit `f7431699797361b349e3849039b6c50e0f6ab6c1` as the
  pre-profile source authority;
- branch `codex/renderer-phase-0-1`;
- seed `424242`;
- 1920x1080 one-to-one output;
- the deterministic Game and Combat baseline workloads;
- two warmed 120-sample runs per retained configuration;
- identical entity signatures and clean process shutdown.

The existing final Vulkan source uses FXAA, disables SSAO, and uses a 512x512
directional shadow map. That is the shipping-profile starting point.

## Measurement layers

### Application telemetry

Existing baseline logs remain authoritative for:

- presented-frame median, p95, and worst time;
- CPU scene encoding;
- CPU canvas/present encoding;
- CPU command submission;
- batch, instance, primitive, and upload statistics already reported by the
  renderer.

Observed fence-completion latency is only a coarse queue-completion signal. It
is affected by polling cadence and must not be reported as native GPU duration.

### GPU frame capture

RenderDoc captures one warmed deterministic frame from each backend on the same
machine. The Vulkan command stream receives stable debug markers for:

1. transient instance and bone upload;
2. primary directional shadow;
3. secondary directional shadow;
4. spot shadow;
5. scene geometry, with opaque and transparent draw boundaries;
6. SSAO, denoise, and resolve when enabled;
7. production post;
8. FXAA or the individual SMAA stages;
9. canvas upload;
10. each canvas target pass;
11. final submission/presentation boundary.

The legacy capture is grouped by the closest equivalent OpenGL framebuffer,
shadow, scene, post, canvas, and swap/present regions available in RenderDoc.
Legacy and Vulkan do not need identical internal pass structure; the comparison
uses semantically equivalent work and explicitly records structural
differences.

### Controlled isolation

Only after the full captures identify a plausible dominant cost may a
diagnostic run disable or simplify one category. Each experiment changes one
factor, retains the deterministic workload, and records the predicted and
observed frame-time delta.

Previously measured directional-shadow and AA/SSAO experiments remain useful
context, but fresh final-source evidence is required for any new conclusion.

## Ranking and repair policy

The report ranks candidates by:

1. absolute Vulkan GPU or CPU time;
2. Vulkan-minus-legacy delta;
3. fraction of the total frame regression explained;
4. confidence that the difference is unnecessary work rather than an intended
   rendering feature.

A repair is eligible when it is an obvious correctness or efficiency problem,
such as redundant upload, repeated state/pipeline setup, avoidable attachment
traffic, excessive draw expansion, preventable synchronization, or work that
the shipping profile does not consume.

Do not stack speculative optimizations. After each repair, rerun the focused
measurement. Retain a change only when the causal measurement improves and
visual/output invariants remain intact.

## Acceptance criteria

- Fresh legacy and Vulkan Game/Combat baselines use identical deterministic
  entities, 1920x1080 output, and clean shutdown.
- At least one warmed RenderDoc frame is captured for each backend.
- Vulkan pass markers make every production pass and canvas phase identifiable.
- The comparison table covers CPU frame/encode/submit, GPU pass structure,
  draw/primitive counts, uploads, post work, canvas, and presentation.
- The dominant remaining bottleneck is identified with a falsifiable isolation
  result, or the report explicitly records why the available evidence cannot
  distinguish the remaining candidates.
- Any retained repair has a focused regression test where practical, fresh
  build/test evidence, and repeated performance evidence.
- Goldens and thresholds remain unchanged.
- The historical Windows-to-Apple text and antialiasing-edge SSIM issue is not
  claimed fixed by this Windows-only work.
