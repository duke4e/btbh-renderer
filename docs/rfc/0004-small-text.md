# RFC 0004: Readable small text

- **Status:** Draft (for later; no implementation scheduled)
- **Requested by:** CHIMERA (its HUD's small text is unreadable)
- **Affects:** CHIMERA (adopts it). Be The Bullet Hell and Tyrian3D: nothing, opt-in. The linear atlas default
  that `v0.1.0` takes from `tyrian3d` (`c505715`) is discussed below
- **Issue:** to be opened

## Need

Small UI text (roughly 9-16 px) looks bad: uneven stems, letters that blur or break up, uneven spacing. The
requirement is better rendering at **no per-frame cost**.

## Today

- `btbh_gpu_window_font_load(window, path, base_size, glyphs, count, report)` rasterises each glyph once with
  `stbtt_GetCodepointBitmap` at `base_size` px into an atlas: no oversampling, no hinting, no kerning.
- Advances are rounded to whole pixels (`(int)(advance * scale)`), so spacing drifts at small sizes.
- The atlas is filtered with nearest on GitHub `master`, linear since `c505715` (in `v0.1.0`).
- The host lays out glyph quads from `BTBHGPUFontGlyph` and draws them with the canvas. The canvas goes to the
  swapchain at the window's size after the scene, so it is already at native resolution after any upscale
  (FSR 1/3, Lanczos).
- The swapchain is SDR UNORM, so coverage is blended in gamma space.
- CHIMERA bakes 16, 22, 30, 42 and 60 px (`RTS/crates/chimera-game/src/sandbox.rs`, `ui_font`). Its HUD
  probably draws at other sizes too, scaling a baked size: that blurs (linear) or breaks up (nearest).

## Options for the rasteriser

| | Small text quality | Cost | Dependency | Notes |
| --- | --- | --- | --- | --- |
| **A. stb_truetype `PackFontRanges` with oversampling and prefilter** | good; no hinting | bake only | none (stb already vendored) | 2-3x horizontal oversampling gives 1/2-1/3 px placement; the prefilter keeps it sharp under bilinear |
| B. FreeType with light hinting / the autohinter | best at 9-12 px (stems on the pixel grid) | bake only | FreeType (+ HarfBuzz later for shaping) | a second font stack on three platforms; LCD subpixel AA is no use on a game swapchain |
| C. MSDF (multi-channel signed distance fields) | poor at small sizes (rounded corners, no hinting) | small shader cost | msdfgen offline or at bake | excellent for large or freely scaled text (titles, world-space labels) |

**Proposal: A now; C later and only for large or scaled text; B only if A at exact sizes still falls short at
9-12 px.**

## Proposal

The old `btbh_gpu_window_font_load` stays as it is. A new loader, used only by a game that asks for it:

```c
typedef struct BTBHGPUFontDesc {
    float pixel_size;              /* the exact size it will be drawn at, in output pixels (size x UI scale) */
    uint8_t oversample_x;          /* 0 or 1: none; 2-3 recommended for UI text */
    uint8_t oversample_y;          /* 0 or 1: none (vertical oversampling rarely helps) */
    uint8_t snap_advances;         /* 1: advances to whole pixels (crisp, uneven); 0: fractional */
    uint8_t reserved;
    /* Coverage contrast: 0 means 1 (as rasterised). Values above 1 thicken light-on-dark small text, which
     * gamma-space blending otherwise thins out. */
    float coverage_gamma;
} BTBHGPUFontDesc;

BTBHGPUFont *btbh_gpu_window_font_load_ex(
    BTBHGPUWindow *window,
    const char *path,
    const BTBHGPUFontDesc *desc,
    const uint32_t *glyphs,
    uint32_t glyph_count,
    BTBHGPUFontReport *report
);

/* Vertical metrics in pixels at the font's size, and kerning between two codepoints (0 if none). */
typedef struct BTBHGPUFontMetrics { float ascent; float descent; float line_gap; float pixel_size; } BTBHGPUFontMetrics;
BTBHGPUResult btbh_gpu_font_metrics(const BTBHGPUFont *font, BTBHGPUFontMetrics *metrics);
float btbh_gpu_font_kerning(const BTBHGPUFont *font, uint32_t left, uint32_t right);
```

- **Baking:** `stbtt_PackBegin`, `stbtt_PackSetOversampling(oversample_x, oversample_y)`, `PackFontRanges`, so
  `BTBHGPUFontGlyph` gets fractional offsets and advances and UVs that undo the oversampling. Atlases from
  `_ex` are always sampled linearly (the prefilter assumes it). `coverage_gamma` is applied to the coverage once,
  while baking, so the canvas shader does not change.
- **Exact sizes:** the game bakes one font per size it draws at (size x UI scale, rounded to whole pixels for
  sizes below about 20 px) and re-bakes when the UI scale or window size changes the pixel size. Text is never
  scaled at draw time; that is what makes small text sharp, more than any filter.
- **Pixel snapping:** documented layout rule (and a small helper if the Swift and Rust layers want it): put the
  baseline and each run's start on whole output pixels; with `oversample_x` = n, glyph x positions may snap to
  1/n px, otherwise to whole pixels; y always to whole pixels.
- **Resolution:** the canvas already draws at native resolution after FSR / Lanczos. Rule for hosts: draw text
  with the canvas, never into the scene's render target.
- **Cost:** baking happens once per font and size at load or UI-scale change (a few ms per atlas). Per frame
  nothing changes: the same quads, one texture, the same canvas shader. Larger atlases (2-3x wider with
  oversampling) are the only cost.

## The linear default from `tyrian3d` (`c505715`)

`v0.1.0` takes Tyrian3D's change that makes every font atlas linear. That changes the existing loader's look
for every game (Be The Bullet Hell included) and, by `COORDINATION.md`, a changed default is a breaking change.
Two ways forward, to decide when this RFC is accepted:

1. Keep it linear (Tyrian3D draws text at many sizes, where linear looks better than nearest), noted as
   breaking in the CHANGELOG, and check Be The Bullet Hell's text before `v0.1.0` is tagged; or
2. Give the old loader nearest back in `v0.2.0` and let Tyrian3D move to `_ex` (or a filter field) for linear.

Recommendation: 1 if Be The Bullet Hell's text still looks right on linear (it draws at its baked sizes, where
the two filters differ least), otherwise 2.

## Later: MSDF for large or scaled text

A separate opt-in atlas kind (`BTBH_GPU_FONT_KIND_MSDF`) and a canvas fragment variant
(`production_canvas_msdf.frag`) for titles, damage numbers and world-space labels that scale or animate. Not
for small text. Its own RFC when a game needs it.

## What CHIMERA changes

- Request the sizes it actually draws, times the UI scale, and draw each string at the size it was baked for;
  re-bake on UI-scale or window-size changes. No scaling of baked text in the HUD.
- Use `btbh_gpu_window_font_load_ex` with `oversample_x = 3`, `oversample_y = 1`, fractional advances and
  `coverage_gamma` around 1.2-1.4 for light-on-dark HUD text below 14 px (tune by eye).
- Snap the baseline and run starts to whole pixels; apply `btbh_gpu_font_kerning`.
- Pick a face made for small sizes for anything below about 14 px (a UI sans with a large x-height and open
  apertures, such as Inter or Noto Sans), and keep display faces for 22 px and up. Below about 10 px, a
  pixel font drawn at integer scale with nearest filtering reads better than any outline font.

## Compatibility

- **Versioning:** minor. New functions and a struct; the existing loader is unchanged.
- **Be The Bullet Hell / Tyrian3D:** nothing to change unless they adopt `_ex`. The linear default question above
  is the only cross-game decision.

## Platforms and testing

Native test: `_ex` validation and that oversampled glyph metrics match the plain loader within a pixel; a
readback of a text line at 10, 12 and 16 px (Windows and macOS) compared with a reference PNG; CHIMERA's HUD
before and after.

## Open questions

- Is A at exact sizes enough at 9-11 px, or does CHIMERA need hinting (B)?
- Shaping (ligatures, non-Latin scripts) is out of scope; it would bring HarfBuzz and its own RFC.
