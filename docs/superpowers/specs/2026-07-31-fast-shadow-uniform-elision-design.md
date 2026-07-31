# Fast Shadow Uniform Elision Design

Status: implemented and verified

Date: 2026-07-31

## Goal

Remove redundant CPU-side uniform allocation and command recording from the
static fast-shadow path while preserving exact shadow output.

## Design

The fast static shadow vertex shader consumes only the 64-byte light view-
projection matrix, which is constant for one shadow pass. The native renderer
currently constructs and pushes the full 144-byte regular shadow block for
every fast draw.

Each shadow render pass tracks whether vertex uniform slot zero has been
initialized:

- the first fast draw pushes only the 16-float light view-projection matrix;
- later fast draws reuse the current binding;
- every regular, animated, billboard, dissolve, or shatter draw constructs
  and pushes the existing full per-draw uniform structure;
- a full push also initializes the slot for later fast draws because its first
  64 bytes are the same light view-projection matrix;
- the initialized flag resets for every shadow pass.

No shader interface, pipeline selection, storage binding, instance range,
draw order, or shadow-update cadence changes.

## Verification

- A source contract proves the one-per-pass fast push and unchanged full push
  for non-fast draws.
- Shader reflection continues to report one 64-byte vertex uniform block and
  no storage buffer for the fast vertex shader.
- Existing directional/spot shadow Vulkan pixel tests remain green.
- A gameplay capture preserves visual parity and measures shadow encode time.
- The change is retained only when warmed total-frame median and p95 improve
  or remain neutral; no golden or threshold changes are allowed.

## Result

The fast shadow path now pushes its 64-byte light matrix once per pass and
uses an instance-rate vertex stream with a dedicated transient vertex mirror.
Its shader paths are mandatory; the ABI-incompatible regular-shader fallback
was removed. Reflection reports zero storage buffers, one uniform buffer, and
locations 0–4. Directional shadow pixel tests and both full renderer suites
pass, and the final 10-pair Vulkan median/p95 beats legacy by
`28.37%/27.10%` without a golden or threshold change.
