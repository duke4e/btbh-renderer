# RFC 0004: Readable small text

- **Status:** Implemented in v0.4.0 (option A). CHIMERA tested it on its HUD and took it (2026-11-12). Findings
  below, under [Prototype](#prototype-2026-11-11). Three opt-in follow-ups it asked for are in the next release:
  [After v0.4.0](#after-v040)
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
- Advances are rounded down to whole pixels (`(int)(advance * scale)`), so spacing drifts at small sizes. The
  prototype measured it: a 76-character HUD line set in Titillium Web is 11-18% narrower than the font's own
  advances at 16-10 px, and words run together.
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
    uint8_t flags;
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
typedef struct BTBHGPUFontMetrics {
    float ascent; float descent; float line_gap; float pixel_size;
    float x_height; float cap_height;
} BTBHGPUFontMetrics;
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

## Prototype (2026-11-11)

Built on branch `claude/rfc0004-small-text-037444` as sketched above, with nothing changed in
`btbh_gpu_window_font_load` (the diff to `btbh_gpu_font.c` only adds) and no shader changed.

**What it does**

- `BTBHGPUFontDesc` as in the sketch. Validation: `pixel_size` 1 to 1024 (fractional allowed),
  `oversample_x` and `oversample_y` 0 to 8 (stb_truetype's limit), `snap_advances` 0 or 1, `flags` 0,
  `coverage_gamma` 0 or 0.25 to 4. A NULL or invalid descriptor, an unclaimed window, no glyphs or a codepoint
  above `INT_MAX` return NULL with `BTBH_GPU_INVALID_ARGUMENT` and the reason in the report. A zeroed
  descriptor is invalid, because there is no size to default to.
- Baking: `stbtt_PackBegin` (2 texels of padding), `stbtt_PackSetOversampling`, `stbtt_PackFontRanges` with
  its prefilter, on stb_truetype's built-in row packer (no `stb_rect_pack.h`). The atlas starts at the smallest
  power of two the glyph boxes need and doubles until they fit, up to 8192. Only codepoints the font has are
  packed. The others get an empty glyph with no advance, as with the plain loader.
- Each glyph's quad is stb's packed quad plus one texel of margin all round, so bilinear filtering at a
  fractional position isn't cut off at the quad's edge. The UVs cover the same texels, so a quad drawn at its
  `width` x `height` takes `oversample_x` texels per pixel across.
- A glyph placed on the 1/`oversample_x` grid samples texel centres exactly: the box prefilter is then the
  pixel's own coverage, and bilinear adds no blur. Off the grid it adds at most 1/`oversample_x` px of blur.
- `coverage_gamma`: coverage' = coverage^(1/gamma), through a 256-entry table over the atlas after the
  prefilter. 0 and 255 stay as they are.
- `btbh_gpu_font_kerning`: stb_truetype's `GetGlyphKernAdvance` (GPOS pair adjustment, else the kern table),
  computed once per font between every two baked glyphs. Only nonzero pairs are kept, sorted, and a lookup
  is a binary search. With `snap_advances` the pairs are whole pixels too. A font from the plain loader has
  none.
- `btbh_gpu_font_metrics`: unrounded ascent, descent and line gap for `_ex`. For a plain font it returns what
  its report said (ascent rounded down, line gap 2). x-height and cap height (added after CHIMERA's test,
  before v0.4.0, so the struct didn't have to grow after a release) are the tops of the 'x' and 'H' outlines,
  unrounded for both loaders.

**What the native test found** (`tests/btbh_gpu_font_smoke.c`, Windows/Vulkan, RTX 3070 Ti; CHIMERA's face
Titillium Web SemiBold and its 117-glyph set):

- Metrics against the plain loader at 10, 12, 16 and 24 px with `oversample_x` 1-3:
  - advances within 0.997 px (the plain loader rounds down);
  - glyph centres within 1/3 px across and identical down;
  - ascent within a pixel, descent identical.
- The UVs undo the oversampling exactly. Advances and kerning are whole numbers with `snap_advances`.
- A line of HUD text (76 characters), light on dark, read back at 10 / 12 / 16 px:

  | | plain loader | CHIMERA's layout today | `_ex`, `oversample_x` 3, kerning |
  | --- | --- | --- | --- |
  | line width (px) | 168 / 210 / 294 | 202.5 / 243 / 324 | 206.3 / 247.5 / 330.1 |
  | furthest a glyph sits from its pen (px) | 0 (but the pen drifts) | 0.50 / 0.40 / 0.47 | 0.16 / 0.17 / 0.16 |
  | ink, pixels of full coverage | 296 / 430 / 773 | 302 / 435 / 775 | 302 / 435 / 773 |

  CHIMERA's layout scales advances from its 60 px face and snaps each glyph to the nearest pixel. That gets
  the width right, but places glyphs up to half a pixel off. That is the jitter CHIMERA measured, and in the
  readback it shows as uneven gaps ("Plan t", "1 00" at 12 px).
  `_ex` puts every glyph within 1/6 px of its pen and keeps the ink. `coverage_gamma` 1.3 adds 16-19% ink
  (359 / 502 / 867), which is the thicker light-on-dark text it is meant to give.
- Kerning in Titillium Web at 16 px, in px: AV −0.29, To −0.83, Ty −0.61, Yo −0.74, LT −0.88, P. −0.74.
- Bake cost: CHIMERA's 117 glyphs at 12 px take about 2 ms (the plain loader about 1 ms), with an atlas of
  128 x 128 to 256 x 128 at 10-16 px. 560 codepoints (Latin to Latin Extended-B) take about 7 ms (plain:
  2 ms). Kerning grows with the square of the glyph count, so a CJK-sized set would need another way (on-demand
  lookups). Per frame nothing changes: the same quads, one texture each, the same canvas shader.
- The PNGs (`font-smoke/` in the build directory, or `BTBH_SMOKE_DUMP`): `text-<size>px-{plain, snapped, ex,
  ex-gamma}.png`, and `text-all.png` / `text-all-x4.png` with every row.

**By eye** (the 4x PNGs): the plain rows run words together. CHIMERA's layout has visibly uneven gaps. `_ex` is
evenly spaced and as sharp as the plain loader at 12 and 16 px. At 10 px the stems are as soft as before,
because there is still no hinting, and `coverage_gamma` 1.3 makes them read heavier. Whether that is enough at
9-11 px (the open question) is for CHIMERA to judge in its HUD.

**Not done:** macOS/Metal, a Swift wrapper (Be The Bullet Hell doesn't need one now), and CHIMERA's in-game
before and after. The plain loader also fails to pack very small glyph sets (three glyphs at 12 px); the
prototype leaves it as it is, because that loader must stay byte for byte the same.

## After v0.4.0

CHIMERA asked for three things after testing the v0.4.0 loader. All are opt-in (a zeroed `flags` bakes the
v0.4.0 atlas, and every existing font smoke PNG and scene-target smoke dump is byte for byte the same), and they
go in the release after v0.4.0, not in v0.4.0:

- **`btbh_gpu_font_kerning_pairs(font, pairs, capacity)`**: every nonzero pair, sorted by (left, right), with
  the advances `btbh_gpu_font_kerning` returns; `NULL, 0` gives the count. It replaces the 117² FFI calls per
  face above with one call. Titillium Web at 16 px has 1754 pairs among CHIMERA's 117 glyphs.
- **`BTBH_GPU_FONT_FLAG_R8_ATLAS`** (`BTBHGPUFontDesc.flags`, which was `reserved`; `reserved` stays as an
  alias in a nameless union, so the struct's size and layout don't change): an R8 coverage atlas, a quarter of
  the RGBA8 one (10 px: 128 x 128, 64 KiB to 16 KiB; 12 and 16 px: 256 x 128, 128 KiB to 32 KiB). SDL3 GPU has no
  texture swizzle, so the canvas draws a one-channel texture with a new fragment shader,
  `production_canvas_coverage.frag` (white x coverage x the vertex colour), picked per batch from the texture's
  format and loaded beside `production_canvas_textured.frag` the first time it is needed. The existing pipelines
  don't change. On the RTX 3070 Ti the R8 rows read back identical to the RGBA8 rows, pixel for pixel.
- **`BTBH_GPU_FONT_FLAG_SNAP_X_HEIGHT`**: the glyphs are rasterised with a vertical scale of
  scale x round(x) / x (x the x-height in pixels), through stb_truetype's gather, pack and render steps with
  their own vertical scale, because `PackFontRanges` takes one scale. Advances, kerning and the horizontal
  scale don't change. `btbh_gpu_font_metrics` gives the whole-pixel x-height and the ascent, descent, line gap
  and cap height at the scale used. In Titillium Web the x-height goes 3.29 → 3 px at 10 px (vertical x0.91),
  3.95 → 4 at 12 (x1.01) and 5.26 → 5 at 16 (x0.95). The most ink in the top row of the x-height, on a
  whole-pixel baseline, without → with the flag:

  | | 10 px | 12 px | 16 px |
  | --- | --- | --- | --- |
  | 'z' (a bar on top) | 0.29 → 0.72 | 0.92 → 0.97 | 0.26 → 1.00 |
  | 'u' (stems) | 0.20 → 0.71 | 0.80 → 0.85 | 0.26 → 1.00 |
  | 'x' | 0.22 → 0.58 | 0.76 → 0.80 | 0.26 → 0.93 |

  Nothing lands in the row above for flat tops. At 10 px the bar of 'z' and the stems of 'u' are thinner than a
  pixel, so their top row is as dark as they get anywhere (0.72 is the bar's own thickness), not 1.0. What it
  doesn't fix: only the x-height's top edge is put on the grid. A horizontal stroke that isn't a whole number of
  pixels thick, or that sits anywhere else (the bar of 'e', the cross of 't', the underside of the bar of 'z'),
  still straddles a pixel edge and leaves a grey row; round tops ('o', 'n', 'e') overshoot into the row above by
  a few percent; capitals and ascenders aren't snapped; and rounding changes the x-height by up to half a pixel,
  so 10 px text gets about 9% shorter. The PNGs: `text-<size>px-ex-snap.png` (compare `text-<size>px-ex.png`)
  and `probe-<size>px-ex-vs-snap.png` in `font-smoke/`.

## What CHIMERA changes

- Request the sizes it actually draws, times the UI scale, and draw each string at the size it was baked for;
  re-bake on UI-scale or window-size changes. No scaling of baked text in the HUD.
- Use `btbh_gpu_window_font_load_ex` with `oversample_x = 3`, `oversample_y = 1`, fractional advances and
  `coverage_gamma` around 1.2-1.4 for light-on-dark HUD text below 14 px (tune by eye).
- Snap the baseline and run starts to whole pixels; apply `btbh_gpu_font_kerning`.
- Pick a face made for small sizes for anything below about 14 px (a UI sans with a large x-height and open
  apertures, such as Inter or Noto Sans), and keep display faces for 22 px and up. Below about 10 px, a
  pixel font drawn at integer scale with nearest filtering reads better than any outline font.

Concretely, in `crates/chimera-platform/src/gpu.rs`, behind `BTBH_GPU_HAS_FONT_LOAD_EX` (as the V-Sync row
is behind `BTBH_GPU_WINDOW_HAS_PRESENT_MODE`):

- `bake_sizes`: bake each face with `btbh_gpu_window_font_load_ex`, `pixel_size` = the size,
  `oversample_x` 3, `oversample_y` 1, `snap_advances` 0, and `coverage_gamma` 1.3 below 14 px (0 above).
  Take the ascent from `btbh_gpu_font_metrics`, now unrounded. At bake, fill a per-face table of the nonzero
  `btbh_gpu_font_kerning` pairs between its glyphs (117² FFI calls, once), so layout doesn't cross the FFI per
  pair.
- `Font::advance`: use the exact face's own advance, which is now fractional and right. Keep scaling the
  biggest face's advance only for a size that has no face yet.
- `lay_out`: keep the pen fractional and add the face's kerning before each glyph. Place each glyph at
  `(pen * 3.0).round() / 3.0 + g.offset_x` instead of `pen.round() + g.offset_x`. Keep the run start and the
  baseline on whole pixels, as now. `width` must add the same kerning, so measured text matches drawn text.
- The fake glyphs in its layout test (around line 2105) get fractional advances to match.

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
