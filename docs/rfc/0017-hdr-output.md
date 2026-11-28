# RFC 0017: HDR output: an HDR swapchain where the display has one, highlights above the SDR look, the UI at paper white

- **Status:** Draft, 2026-11-28. Waiting for v2's answers to the open questions at the end
- **Requested by:** Be The Bullet Hell v2. Its coordinator asked after the owner's seventeenth play-test (2026-11-28),
  and the owner asked for it directly. He plays on a 4K TV and on the Steam Machine, where the game is a Windows Vulkan
  app under Proton in gamescope
- **Affects:**
  - Be The Bullet Hell v2 adopts it (opt-in). It adds the Display option, a calibration screen and the defaults itself.
  - Be The Bullet Hell (v1, Swift), CHIMERA and Tyrian3D change nothing. A window stays SDR until its host asks for HDR,
    and SDR frames stay byte for byte 0.16.1's.
  - The release adds new shader files only. The SDR shaders' SPIR-V and MSL must stay byte-identical (checked in phase
    1).
- **Issue:** none; v2 answers through its session
- **Depends on:** nothing.
  - The base is RFC 0015 phase 1a's frame context (0.16.1): the presents already read the swapchain's format once and
    pass it in.
  - RFC 0015 phase 1b (the frame thread) can come before or after this. Its present step then runs this RFC's encode
    pass instead of a blit.
- **Sources:**
  - Every line reference is master `c20c4d0b` (v0.16.1).
  - SDL is `external/SDL` at `release-3.4.2` (`683181b4`).
  - Short names below: `window.c`, `canvas.c`, `scene.c`, `texture.c`, `internal.h` for `src/gpu/btbh_gpu_*`;
    `vulkan.c` for `external/SDL/src/gpu/vulkan/SDL_gpu_vulkan.c`; `metal.m` for
    `external/SDL/src/gpu/metal/SDL_gpu_metal.m`; `video.c` for `external/SDL/src/video/SDL_video.c`; `SDL_gpu.h` under
    `external/SDL/include/SDL3/`; shaders under `shaders/gpu/`.
  - The Proton, gamescope and Wine facts are researched from their sources and docs, not run (see "Proton, gamescope
    and the Steam Machine"). The URLs are in "Sources" at the end.
  - This PC's display was probed with a small SDL 3.4.2 program (see "What this PC reports").

## Need

The owner plays on a 4K HDR TV, from the Windows PC and from the Steam Machine. v2's ask:

- **An HDR swapchain where the display supports it,** through SDL 3.4's swapchain compositions (HDR10 ST2084 and/or
  extended-linear scRGB), with a clean fallback to SDR.
- **The scene tone mapped for HDR, with the SDR look as the reference.** HDR extends the highlights (beams, fire,
  emissive glass, sky) and leaves the mid-tones where they are. The UI is composited at a paper-white level.
- **From the API:**
  - whether HDR is available on this window and display;
  - turning it on and off at runtime, without a restart where possible;
  - peak brightness and paper white, in nits, as settings;
  - the display's reported headroom or peak, where SDL gives one.
- **Proton/gamescope:** what that path needs (environment variables, colour-space extensions, gamescope's conditions),
  stated exactly enough that v2's deploy can set it. There is also a test procedure for the Machine, which neither
  session can run from here.

## What the renderer does today

### The swapchain

- **The claim asks SDL for SDR and nothing else.**
  - It checks SDR, sets SDR with the present mode, and sets the frames in flight (`window.c:661-679`).
  - It reads the format SDL picked (`window.c:690-692`): B8G8R8A8_UNORM, or R8G8B8A8_UNORM where the surface lacks it
    (`vulkan.c:226-238`). It maps that through `btbh_gpu_texture_format` (`window.c:321-338`).
  - It stores `composition = BTBH_GPU_SWAPCHAIN_SDR` (`window.c:712`).
- **`btbh_gpu_window_set_present_mode` sets SDR again** with the new mode, and falls back to SDR with the old mode
  (`window.c:882-899`). Its header comment says "The swapchain keeps its SDR composition" (`btbh_gpu.h:1962`).
- **The public enums are ready for more:**
  - `BTBHGPUSwapchainComposition` has only `INVALID` and `SDR` (`btbh_gpu.h:128-131`). `BTBHGPUWindowReport.composition`
    reports it (`btbh_gpu.h:170`).
  - `BTBHGPUTextureFormat` already has `RGB10A2_UNORM` (5) and `RGBA16_FLOAT` (6) (`btbh_gpu.h:117-118`), the two HDR
    swapchain formats.

### The frame context (RFC 0015 phase 1a)

`BTBHGPUFrameContext` (`internal.h:144-156`) carries `swapchain_format`, filled by
`btbh_gpu_window_frame_context_fill` from `SDL_GetGPUSwapchainTextureFormat` (`canvas.c:927-958`). That one format
is used for four different things:

1. **The window scene's last post targets.** `final_color_texture` and `post_color_texture` (`scene.c:4994-5001`,
   `:5019-5027`), `upscaled_color_texture` and `sharpened_color_texture` (`scene.c:4875-4942`), and the motion-error
   view's copy (`scene.c:13862-13870`).
2. **The pipelines that write those targets:** the output pass (`btbh_gpu_scene_ensure_post_pipeline`,
   `scene.c:2005`, its format at `:2057`), tilt-shift (`:2250-2350`), FXAA (`:3364`), the SMAA blend (`:3218`), and EASU,
   RCAS and Lanczos (`:2454`). `btbh_gpu_scene_prepare_frame` passes the format to each (`scene.c:10368`, `:10380`,
   `:10616`, `:10638`, `:10647`, `:10677`, `:10700`).
3. **The canvas pipelines,** created once with that format (`canvas.c:420`, `:1449-1480`). The canvas also refuses a
   target pass whose target has another format (`canvas.c:501`).
4. **Render targets** (`btbh_gpu_window_render_target_create`, `texture.c:311-325`), **captures**
   (`scene.c:15722-15723`) and **copies** (`scene.c:15867-15870`). These ask SDL themselves.

The `ensure_*` functions reuse a target when its size matches and never look at its format (`scene.c:4885-4890`,
`:4955-4967`). A format change would leave stale targets.

### The post chain, and which passes assume 0-1

In the present's order (`scene.c:13374-14005`):

| Pass | Input | Writes | Range |
| --- | --- | --- | --- |
| Bloom (`:13383-13470`) | the scene, HDR linear | RGBA16F levels | HDR. Its threshold is divided by exposure (`:13391-13392`) |
| HDR composite: fog, bloom, vignette (`:13471-13552`, `production_hdr_composite.frag.hlsl`) | HDR linear | `post_pong`, RGBA16F | HDR, alpha 1 |
| LUT and grading (`:13553-13573`, `production_lut.frag.hlsl:43-85`) | HDR linear | `post_ping`, RGBA16F | **0-1 out.** PBR Neutral and ACES are tone mapped and graded here (`btbh_prepare_lut_input`, `production_tonemap.hlsli:100-119`). Reinhard is not. The input is clamped by `saturate(pow(x, 1/2.2))` (`production_lut.frag.hlsl:56`), so Reinhard with the LUT clips the scene at 1 |
| CAS (`:13574-13596`, `production_cas.frag.hlsl:39-74`) | the LUT's output, or HDR without the LUT | the other RGBA16F target | Either. `saturate()` stops HDR from sharpening (`:66-69`) |
| Tilt-shift + output (`:13597-13620`) or output (`:13622-13646`); `production_post.frag.hlsl`, `production_tilt_shift.frag.hlsl:196-209` | the post stage's input | `post_color` (with AA or the mirror) or `final_color`, **in the swapchain format** | **The tone map:** `btbh_tone_map_for_output` (`production_tonemap.hlsli:121-142`) is the curve, then contrast and saturation with a final `saturate` (`:58-72`). With the LUT on it only `saturate`s. Then `linear_to_srgb` (`production_post.frag.hlsl:36-42`, a pure 1/2.4 power with 1.055/-0.055 and no linear toe) and a ±0.5/255 dither (`:55-59`). **0-1, sRGB-encoded, 8-bit** |
| FXAA (`:13647-13671`) | `post_color` | `final_color` | Luma is the green channel (`production_fxaa.frag.hlsl:20-28`). Thresholds are relative; no clamp |
| SMAA edges, weights, blend (`:13672-13745`) | `post_color` | `final_color` | Absolute luma thresholds, tuned for 0-1 sRGB |
| Mirror (`:13746-13771`) | `final_color` | `post_color` | A same-format blit |
| FSR 3 (`:13772-13856`) | `final_color`: **tonemapped and sRGB-encoded** | its RGBA16F output | Built with `FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 0` (`fsr3/btbh_fsr3_options.hlsli:24`). The HDR option only adds a reversible tone map around its blends (`ffx_fsr3upscaler_accumulate.h:29-41`, `ffx_fsr3upscaler_upsample.h:390-431`); the LDR build has no clamp |
| RCAS after FSR 3 or EASU (`:13820-13846`, `:13947-13968`) | 0-1 | `sharpened_color`, swapchain format | **Assumes a peak of 1:** `(1 - max(...))` (`production_upscale_fsr_rcas.frag.hlsl:45`) and `saturate(color)` (`:50`) |
| EASU, Lanczos (`:13902-14001`) | 0-1 | `upscaled_color`, swapchain format | Deringing clamps to the neighbours' min and max (`production_upscale_fsr_easu.frag.hlsl:142`); no 0-1 clamp |

So:
- **The tone map, grading, the LUT and sRGB encoding all happen in the output pass,** or in the LUT pass when the LUT
  is on. Everything after it (FXAA, SMAA, FSR 1, FSR 3, RCAS, Lanczos, the mirror, the canvas) runs on 8-bit
  sRGB-encoded values in 0-1.
- **FSR 3 runs after the tone map** here, on the same sRGB-encoded values SMAA sees (most engines feed it linear HDR).

### The canvas, the present and the read-back

`btbh_gpu_window_canvas_present` (`canvas.c:1344-1386`) runs in three steps:

1. **Acquire** (`canvas.c:1522-1591`): the command buffer, the swapchain image, and the target the screen passes
   draw into (`btbh_gpu_window_present_target`, `canvas.c:962-1013`). That target is the swapchain image, or
   `window->presented_frame` (same format) when a read-back is requested.
2. **Draw** (`canvas.c:1593-1850`):
   - The pending scene's composite is blitted into the target with a linear filter (`btbh_gpu_window_blit_pending_scene`,
     `scene.c:15802-15838`).
   - Then the passes are drawn. Each blend mode (`canvas.c:116-152`: alpha, additive, multiplied by the destination,
     premultiplied) does its maths on sRGB-encoded 8-bit values, clamped by the UNORM target.
3. **Submit** (`canvas.c:1856-1897`): a read-back frame is blitted to the image (`canvas.c:1015-1043`), then the frame is
   submitted.

The other ways out of a frame:
- **`btbh_gpu_window_readback_presented_frame`** (`canvas.c:1110-1266`) downloads 4 bytes a pixel and swizzles BGRA
  (`canvas.c:1233-1257`). It assumes an 8-bit format.
- **`capture_pending_scene_texture`** (`scene.c:15668-15799`) makes a texture in the swapchain's format and copies the
  composite into it.
- **`copy_pending_scene_texture`** (`scene.c:15843-15955`) refuses a destination not in the swapchain's format.
- **`render_to_target`** draws through the aside runtime (or a target view's) into a render target: the same chain,
  ending in the target's format.
- **`present_views`** (RFC 0014, 0.16.0) composes the views before post, so the post chain runs once.

## What SDL 3.4.2 gives

### The swapchain

| Composition | Vulkan format and colour space (`vulkan.c:226-263`) | Metal (`metal.m:368-373`, `:4596-4602`) | Meaning (`SDL_gpu.h:1340-1371`) |
| --- | --- | --- | --- |
| SDR | B8G8R8A8_UNORM (else R8G8B8A8_UNORM), SRGB_NONLINEAR | BGRA8, sRGB | sRGB-encoded |
| SDR_LINEAR | B8G8R8A8_SRGB (else R8G8B8A8_SRGB) | BGRA8_SRGB, sRGB | Linear in the shader |
| HDR_EXTENDED_LINEAR | R16G16B16A16_SFLOAT, EXTENDED_SRGB_LINEAR_EXT, no fallback | RGBA16F, `kCGColorSpaceExtendedLinearSRGB`, EDR | Linear BT.709, values above 1 allowed |
| HDR10_ST2084 | A2B10G10R10_UNORM_PACK32 (SDL's `R10G10B10A2_UNORM`), HDR10_ST2084_EXT, no fallback | RGB10A2, `kCGColorSpaceITUR_2100_PQ` (macOS 11+) | PQ, BT.2020 |

- **`SDL_WindowSupportsGPUSwapchainComposition`:**
  - Vulkan asks the surface for its format and colour-space pairs each call (`vulkan.c:9667-9712`). SDL turns on
    `VK_EXT_swapchain_colorspace` at the instance when it exists (`vulkan.c:11143-11147`, `:11817-11820`).
  - **Metal says yes to every composition on macOS 11+, whatever the display** (`metal.m:3631-3647`).
- **`SDL_SetGPUSwapchainParameters`:**
  - Vulkan re-checks, then re-creates the swapchain at once: `VULKAN_Wait` is a device wait, then the destroy and create
    (`vulkan.c:10208-10242`, `:9898-9924`).
  - Metal waits, then sets the layer's pixel format, colour space and `wantsExtendedDynamicRangeContent`
    (`metal.m:3988-4035`).
  - So a switch costs a GPU wait, and it must not happen while a frame holds a swapchain image.
- **Re-creations SDL does by itself** (resize, out-of-date, suboptimal: `vulkan.c:10005-10023`, `:10712-10730`) keep the
  composition. If the surface no longer offers it, the create fails ("Device does not support requested swapchain
  composition", `vulkan.c:4555-4566`) and so does every acquire after it.
- **HDR metadata:**
  - **SDL 3.4.2 never calls `vkSetHdrMetadataEXT` and never turns on `VK_EXT_hdr_metadata`.** Nothing in
    `external/SDL/src/gpu` mentions either.
  - `SDL_GPUVulkanOptions` could add the extension, but the swapchain handle is not exposed. The renderer cannot call
    it without changing SDL.
  - Metal sets no `CAEDRMetadata`.

### The window's HDR state

- **The properties** `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`, `SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT` (1.0 is 80 nits)
  and `SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT` are set by `SDL_SetWindowHDRProperties` (`video.c:1177-1196`). Enabled means
  headroom > 1.
- **The event** `SDL_EVENT_WINDOW_HDR_STATE_CHANGED` is sent when a display's values change and the driver doesn't send
  its own (`video.c:1198-1215`).
- **On creation** the window takes its display's values, with no event (`video.c:2539-2542`).

| Platform | Where the values come from | When they update |
| --- | --- | --- |
| Windows | DXGI: the output's `ColorSpace == G2084_NONE_P2020`; headroom = `MaxLuminance / 80 / SDR white`; SDR white from DisplayConfig's `SDRWhiteLevel / 1000` (`SDL_windowsmodes.c:512-552`) | Only on `WM_DISPLAYCHANGE` (`SDL_windowsevents.c:2217-2221` -> `SDL_windowsmodes.c:628-633`). Toggling Windows HDR changes the mode, so it should send one; the SDR-brightness slider does not |
| macOS | SDR white 1.0; headroom = `maximumExtendedDynamicRangeColorComponentValue`, else the potential one (`SDL_cocoamodes.m:309-323`) | On `NSApplicationDidChangeScreenParametersNotification` (`SDL_cocoaevents.m:163-164`, `:280-284`) |
| Wayland | The colour-management protocol: headroom = max / reference luminance (`SDL_waylandcolor.c:168-175`). gamescope's own `frog_color_managed_surface`: PQ against 203 nits, scRGB against 80 (`SDL_waylandwindow.c:1675-1706`) | The compositor's events |
| X11 | Nothing | - |
| Windows app under Proton/gamescope | Windows' path above, but DXGI is DXVK's: HDR is reported only with `DXVK_HDR=1`, and Wine has no SDR white level (see "Proton, gamescope and the Steam Machine") | As Windows |

**Two gaps in 3.4.2.** The first is SDL issue #16068 ("window HDR properties don't update when the SDR white level
changes or the window moves to another display"), fixed for the 3.6.0 milestone, after 3.4.2.
- **Moving the window to another display updates nothing.** `SDL_EVENT_WINDOW_DISPLAY_CHANGED` only records the
  display id (`SDL_windowevents.c:192-197`, `video.c:4239-4271`).
- **`SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN` is set when a display is added and never again** (`video.c:891-895`;
  `SDL_SetDisplayHDRProperties` doesn't touch it).

### What this PC reports

A small probe (about 190 lines) linked against 3.4.2 created a window, claimed it, and set each supported composition.
It also asked Windows directly (DXGI 1.6 and DisplayConfig; read-only, no settings touched).

```
SDL 3.4.2, video driver windows
display 1 'LG ULTRAGEAR+': 2560x1440 @ 60.00 Hz, XRGB8888, HDR_enabled=0
DXGI adapter 'NVIDIA GeForce RTX 3070 Ti' output '\\.\DISPLAY1': ColorSpace=0 (SDR G22 P709) BitsPerColor=8 MaxLum=0.0
DisplayConfig target 'LG ULTRAGEAR+': advancedColorSupported=0 advancedColorEnabled=0 bitsPerColorChannel=8
window: HDR_enabled=0 SDR_white_level=1.000 HDR_headroom=1.000 (created, claimed, shown: the same)
Vulkan:     SDR yes (B8G8R8A8_UNORM)  SDR_LINEAR yes (B8G8R8A8_UNORM_SRGB)  HDR_EXTENDED_LINEAR no  HDR10_ST2084 no
Direct3D12: SDR yes  SDR_LINEAR yes  HDR_EXTENDED_LINEAR no  HDR10_ST2084 yes (R10G10B10A2_UNORM)
```

- **Windows does not consider this monitor path HDR-capable** (`advancedColorSupported=0`, no luminance data). HDR
  cannot be verified on a real display on this PC.
- **NVIDIA's Vulkan surface offers no HDR colour space while the display is SDR.** D3D12 offers HDR10 anyway. So on
  Vulkan a supported HDR composition is a meaningful signal; on other backends it is not.
- The renderer is tested here offscreen, against a CPU reference of every curve (see "Platforms and testing").

## Proton, gamescope and the Steam Machine (researched, not verified)

Read from the sources of gamescope (`7ed5e25`), Valve's Wine fork (Proton 9-11), DXVK and SDL on 2026-11-28, plus
maintainer comments. Nobody has run this path for this renderer. **[D]** marks what is documented in source or by a
maintainer, **[I]** what is inferred.

| Component | What it needs or does |
| --- | --- |
| **gamescope's WSI layer** (`VkLayer_FROG_gamescope_wsi`) | **[D]** Turned on by `ENABLE_GAMESCOPE_WSI=1`, which gamescope sets for everything it launches. `DISABLE_GAMESCOPE_WSI=1` turns it off. It adds the HDR pairs (A2B10G10R10 or A2R10G10B10 with HDR10_ST2084, and R16G16B16A16_SFLOAT with EXTENDED_SRGB_LINEAR) to an X11 surface only when **all** of these hold **at surface creation**: the surface is X11 (XWayland); gamescope's HDR output is on; and the window can be scanned out directly, matching its top-level window to within 1-2 px with nothing covering it (layer `:619-695`, `:1137-1200`, `:1436`). It never adds them to a native Wayland surface. With `PROTON_LOG=1` it logs `hdr formats exposed to client: true/false` (`:1505-1512`) |
| **gamescope's output** | **[D]** HDR when `--hdr-enabled` or the root property `GAMESCOPE_DISPLAY_HDR_ENABLED` is set, and the display supports it (the kernel's HDR properties and an EDID with BT.2020 RGB and PQ). **[I]** SteamOS's "Enable HDR" toggle sets that property. **[D]** It takes scRGB from a client: BT.709 to BT.2020 by a matrix, 1.0 = 80 nits, through an FP16 intermediate. **[D]** An SDR app on an HDR output shows at `GAMESCOPE_SDR_ON_HDR_CONTENT_BRIGHTNESS`, default 203 nits in the code (the `--help` text still says 400); **[I]** that is SteamOS's SDR brightness slider |
| **HDR metadata through gamescope** | **[D]** An app's `vkSetHdrMetadataEXT` is dropped when MaxCLL or MaxFALL is 0. On an HDR10 TV it replaces what gamescope sends, so the TV tone maps to the app's values. On the Deck OLED, Steam's own values are used instead |
| **winevulkan** | **[D]** Exposes `VK_EXT_swapchain_colorspace` (since Wine 4.5) and `VK_EXT_hdr_metadata` (since 8.1). It passes the host surface's format list and the metadata call through unchanged. So, under gamescope, the HDR pairs exist only because the layer adds them to Wine's X11 surface |
| **DXGI (DXVK's `dxgi.dll`, Proton's default)** | **[D]** `IDXGIOutput6::GetDesc1` reports `G2084_NONE_P2020` **only with `DXVK_HDR=1`** ("We have no way of checking the actual Windows colorspace"). `MaxLuminance` comes from the EDID, which gamescope patches with the panel's values. Proton's Wine reads `DXVK_HDR` for DisplayConfig's advanced-colour answer too. **[D]** Wine implements no SDR white level. **[I]** Game Mode appears to set `DXVK_HDR=1` when HDR is on (a launcher that overrode it broke HDR; players turn HDR off with `DXVK_HDR=0`). Valve's `proton` script doesn't set it in 9, 10, 11 or Experimental |
| **SDL inside Wine** | **[D, from the above]** Without `DXVK_HDR=1`, SDL reports HDR off even when the surface offers HDR10. With it, SDR white is 1.0 (80 nits) and headroom is `MaxLuminance / 80`. The 80 nits is not what the player sees: gamescope shows SDR at 203 |
| **Proton variables** | **[D]** `PROTON_ENABLE_HDR` and `PROTON_ENABLE_WAYLAND` don't exist in Valve's Proton 9, 10, 11 or Experimental. In GE-Proton, `PROTON_ENABLE_HDR` is a legacy alias for `DXVK_HDR=1`, and `PROTON_ENABLE_WAYLAND` picks Wine's Wayland driver, which gets no HDR inside gamescope. `ENABLE_HDR_WSI` belongs to a KDE desktop layer and must not be used with gamescope |
| **Steam Machine** | **[I, press]** DisplayPort 1.4 and HDMI 2.0 with HDR. Valve said HDMI 2.1 is still in progress in software; 4K120 HDR over HDMI uses chroma subsampling. **[D]** gamescope has no Steam Machine display profile. **[I]** So the TV's EDID drives it: PQ output, the SDR brightness slider applies, and the game's metadata reaches the TV |
| **Valve's developer guidance** | None found on Steamworks about HDR. The Deck and Steam hardware recommendation pages don't mention it |

**What this means for the renderer:**

1. **Under Wine, trust the swapchain, not SDL's flag.** gamescope offers HDR pairs only when its output is HDR, so a
   supported HDR composition is the truth there. SDL's `HDR_ENABLED` depends on `DXVK_HDR`. The renderer detects Wine
   through `ntdll.dll`'s `wine_get_version` export, and its availability rule changes accordingly (Proposal).
2. **Paper white defaults to 203 nits.** That matches gamescope's SDR content, BT.2408's reference white and the
   Wayland frog path. SDL's 80 nits under Wine is not real.
3. **Peak is the reported one (MaxLuminance, through `DXVK_HDR`) where known, else the setting.** TV EDIDs are often
   wrong, which is what v2's calibration screen is for.
4. **The surface must be made while the game is fullscreen with HDR already on.** The layer fixes the format list at
   surface creation, and SDL makes the surface at the claim (`vulkan.c:9787-9800`). A game that claims its window
   windowed, or a player who turns HDR on in SteamOS mid-game, keeps SDR until the surface is made again (a phase 2
   option, decided on the Machine test).
5. **No metadata from SDL 3.4.2,** so the TV gets gamescope's own values.

**What v2's deploy must set for players: nothing.**
- Valve Proton 8.0-5 or newer (9, 10, 11 or Experimental), its default X11 driver and DXVK's DXGI.
- Not `PROTON_ENABLE_WAYLAND=1`, `PROTON_USE_WINED3D=1`, `DISABLE_GAMESCOPE_WSI=1` or `ENABLE_HDR_WSI=1`.
- Not `DXVK_HDR=1` in launch options either: forced on, DXGI would keep saying HDR after the player turns it off.
- The player needs Game Mode, HDR on in SteamOS's display settings, an HDR TV, and the game fullscreen.

**For the test only:** `PROTON_LOG=1`, the renderer's log switch, and one A/B run with `DXVK_HDR=1` (see "The Steam
Machine test").

## Designs considered

### Where the HDR picture is made

| | How | SDR identity | Quality | Captures | Work |
| --- | --- | --- | --- | --- | --- |
| **(a) SDR image plus a gain map** | The chain stays 8-bit SDR. The output pass also writes a per-pixel highlight gain (R16F, at render size); the canvas also writes its coverage into it; the encode multiplies | By construction | **Aliased, shimmering highlight edges:** the gain isn't anti-aliased, isn't accumulated by FSR 3, is jittered and bilinearly upscaled. Beams are exactly where it shows | Free (the SDR image exists) | Multiple-render-target variants of the output, tilt-shift and every canvas pipeline |
| **(b) A float tail in extended sRGB (recommended)** | In HDR the output pass writes the SDR-encoded value times the highlight gain, so values can pass 1. The tail (FXAA, SMAA, FSR, RCAS, Lanczos, the mirror) and the canvas run on RGBA16F; an encode pass writes PQ or scRGB | SDR mode untouched. In HDR every value at or below the knee equals SDR's within the 8-bit rounding | True HDR through anti-aliasing, upscaling and temporal accumulation | A frame-format capture is exact; an SDR target gets a fold | HDR variants of 5 shaders, one encode and one fold shader, format-keyed targets and pipelines |
| **(c) A folded tail** | As (b), but the output pass folds values above 0.8 into (0.8, 1) so every tail pass works in its 0-1 domain. Unfolded before the canvas | As (b) | As (b); the 0.8-1 band behaves a little differently from SDR in the tail | Every consumer of the composite needs an unfold | (b) plus an unfold at the scene blit, captures and read-back; no RCAS variant; safe for FSR 3's LDR build |
| **(d) Inverse tone map of the SDR frame** | Expand the finished 8-bit frame | Yes | Banding (8-bit highlights), no real highlight detail, UI can't be told from scene | Free | Least |

**Recommendation: (b).**
- It is the only design that gives anti-aliased, temporally stable highlights, which is the point of the request.
- SDR mode doesn't run any of it.
- (c) is the fallback if FSR 3's LDR build ghosts on values above 1 (phase 2). It would apply only while FSR 3 runs,
  inside the output pass and the RCAS variant.

### The HDR tone map: a highlight gain on the SDR result

| | Mid-tones | Grading and LUT | Highlights |
| --- | --- | --- | --- |
| **Gain on the SDR result (recommended)** | Exactly SDR's (the SDR value times 1) | Unchanged: they run on the SDR curve's output, which never passes 1 | Brighter, with SDR's hue and saturation (SDR's desaturation at the shoulder stays) |
| **Each curve generalised to a peak above 1** | PBR Neutral's family is exact below its shoulder; ACES and Reinhard have none | `saturate` and contrast's log-space pivot push past the peak; a LUT authored over 0-1 can't take values above 1 | More saturated |
| **A separate HDR curve (ACES ODT, GT and so on)** | Shift | Re-author | Most "HDR" |

**Recommendation: the gain** (maths in "Proposal").
- It is the only option that keeps the SDR look as the reference by construction.
- It is one code path for all three tone mappers, with or without the LUT.
- With peak = paper white it is exactly SDR, which also makes it testable on an SDR PC.
- More colour in highlights is question 4.

### HDR10 or scRGB, per platform

| Platform | HDR10 ST2084 | Extended linear (scRGB / EDR) | Pick |
| --- | --- | --- | --- |
| Windows, Vulkan | Native for TVs. 32 bpp like SDR. The renderer converts to BT.2020 and PQ | 64 bpp: twice the swapchain bandwidth. DWM composes in it natively; 1.0 = 80 nits | **HDR10**, else extended linear |
| Steam Machine and Deck (gamescope) | The format gamescope outputs | Accepted, then converted by gamescope through FP16: one more full-screen pass on the APU | **HDR10**, else extended linear |
| macOS, Metal (EDR) | `kCGColorSpaceITUR_2100_PQ`; macOS maps it relative to its own reference | The native EDR path: 1.0 = the system's SDR white, values up to the headroom | **Extended linear**, else HDR10 |
| Linux native, Wayland | Where the compositor's colour management offers it | Same | HDR10, else extended linear. Not a target; researched only |

**Recommendation: the renderer picks** in that order; v2 can name one (`settings.composition`) for tests. On Apple, nits
are ratios: paper white is the system's SDR white and the peak is at most the headroom.

## Proposal

```c
/* HDR output (RFC 0017, asked for by Be The Bullet Hell v2). A window is SDR until the host turns HDR on
 * (btbh_gpu_window_set_hdr); its SDR frames are as before, byte for byte. In HDR:
 * - the scene is tone mapped and graded exactly as in SDR, then highlights above the tone map's knee are lifted toward
 *   the peak, keeping their SDR hue;
 * - the SDR range of the picture and the canvas (the UI) show at paper white;
 * - the swapchain is HDR10 (BT.2020, PQ) or extended linear (scRGB; EDR on Apple). */

/* BTBHGPUSwapchainComposition, appended. */
    BTBH_GPU_SWAPCHAIN_HDR10_ST2084 = 2,          /* RGB10A2_UNORM: BT.2020 primaries, PQ, absolute nits */
    BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR = 3    /* RGBA16_FLOAT: BT.709 primaries, linear; 1.0 is 80 nits (Windows,
                                                     gamescope) or the system's SDR white (Apple's EDR) */

typedef enum BTBHGPUHDRMode {
    BTBH_GPU_HDR_MODE_OFF = 0,    /* SDR, as before (the zero value) */
    BTBH_GPU_HDR_MODE_AUTO = 1,   /* HDR while BTBHGPUWindowHDRState.available is 1, SDR otherwise, following the OS and
                                     the display (the switch happens at the next frame's start) */
    BTBH_GPU_HDR_MODE_FORCE = 2   /* HDR whenever the swapchain offers an HDR composition, whatever the OS reports (for
                                     tests) */
} BTBHGPUHDRMode;

/* Why a mode that asks for HDR is not in HDR now. */
typedef enum BTBHGPUHDRFallback {
    BTBH_GPU_HDR_FALLBACK_NONE = 0,           /* HDR is active, or the mode is OFF */
    BTBH_GPU_HDR_FALLBACK_UNSUPPORTED = 1,    /* the swapchain offers no HDR composition (or not the one named) */
    BTBH_GPU_HDR_FALLBACK_OS_SDR = 2,         /* AUTO: offered, but the OS reports the display SDR */
    BTBH_GPU_HDR_FALLBACK_SWITCH_FAILED = 3,  /* SDL refused or failed the switch; the window went back to SDR */
    BTBH_GPU_HDR_FALLBACK_LOST = 4,           /* the swapchain lost its HDR composition (the display or the OS changed) */
    BTBH_GPU_HDR_FALLBACK_PENDING = 5         /* AUTO: waiting for the next frame's start to switch */
} BTBHGPUHDRFallback;

/* 32 bytes on 64-bit: mode 0, composition 1, flags 2, paper_white_nits 4, peak_nits 8, reserved 12. */
typedef struct BTBHGPUWindowHDRSettings {
    uint8_t mode;               /* BTBHGPUHDRMode */
    uint8_t composition;        /* 0: the renderer's pick (HDR10 then extended linear; extended linear first on Apple);
                                   else BTBH_GPU_SWAPCHAIN_HDR10_ST2084 or BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR */
    uint16_t flags;             /* 0 */
    float paper_white_nits;     /* the UI's and the SDR picture's white. 0: 203. Clamped to 80..1000 */
    float peak_nits;            /* the brightest highlight. 0: the display's reported peak when it is above paper white,
                                   else 1000. Clamped to paper_white_nits..10000 */
    uint32_t reserved[5];       /* 0 */
} BTBHGPUWindowHDRSettings;

#define BTBH_GPU_HDR_DEFAULT_PAPER_WHITE_NITS 203.0f
#define BTBH_GPU_HDR_DEFAULT_PEAK_NITS 1000.0f

/* What the window can do and is doing. 224 bytes on 64-bit: serial 0, composition 4, swapchain_format 8,
 * frame_format 12, target_format 16, mode 20, active 21, available 22, os_hdr_enabled 23, supported 24, under_wine 28,
 * relative_luminance 29, metadata_set 30, fallback 31, sdr_white_level 32, hdr_headroom 36, display_sdr_white_nits 40,
 * display_peak_nits 44, paper_white_nits 48, peak_nits 52, highlight_ratio 56, switch_count 60,
 * last_switch_milliseconds 64, display_id 72, reserved 76, message 96. */
typedef struct BTBHGPUWindowHDRState {
    uint32_t serial;                           /* from 1; changes whenever anything below changes */
    BTBHGPUSwapchainComposition composition;   /* in use: SDR, or one of the two above */
    BTBHGPUTextureFormat swapchain_format;     /* the swapchain image's (RGB10A2_UNORM, RGBA16_FLOAT in HDR) */
    BTBHGPUTextureFormat frame_format;         /* what the window's scene ends in, the canvas's screen passes draw in and
                                                  captures are: the swapchain's in SDR, RGBA16_FLOAT in HDR */
    BTBHGPUTextureFormat target_format;        /* render targets' (btbh_gpu_window_render_target_create) and what
                                                  render_to_target writes: the SDR swapchain's, in SDR and HDR alike */
    uint8_t mode;                              /* the settings' BTBHGPUHDRMode */
    uint8_t active;                            /* 1: the swapchain is HDR */
    uint8_t available;                         /* 1: HDR can be shown here (the rule below) */
    uint8_t os_hdr_enabled;                    /* SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN */
    uint8_t supported[4];                      /* SDL_WindowSupportsGPUSwapchainComposition for SDL's SDR, SDR_LINEAR,
                                                  HDR_EXTENDED_LINEAR, HDR10_ST2084, in that order */
    uint8_t under_wine;                        /* 1: running under Wine/Proton */
    uint8_t relative_luminance;                /* 1 on Apple: paper white is the system's SDR white, nits are ratios */
    uint8_t metadata_set;                      /* 1: HDR metadata reached the swapchain. 0 with SDL 3.4.2, which has no
                                                  call for it */
    uint8_t fallback;                          /* BTBHGPUHDRFallback */
    float sdr_white_level;                     /* SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT (1.0 = 80 nits) */
    float hdr_headroom;                        /* SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT */
    float display_sdr_white_nits;              /* sdr_white_level * 80; 0 on Apple */
    float display_peak_nits;                   /* hdr_headroom * display_sdr_white_nits; 0 when the headroom is 1
                                                  (SDR, or not reported: Wine without DXVK_HDR) */
    float paper_white_nits;                    /* in use (the settings', resolved and clamped) */
    float peak_nits;                           /* in use */
    float highlight_ratio;                     /* the tone map's peak over paper white (P below); 1 in SDR */
    uint32_t switch_count;                     /* composition switches since the claim */
    double last_switch_milliseconds;           /* the last switch's CPU time (SDL waits for the GPU in it) */
    uint32_t display_id;                       /* SDL_GetDisplayForWindow */
    uint32_t reserved[5];
    char message[128];                         /* the last refusal or fallback, in words; empty otherwise */
} BTBHGPUWindowHDRState;

/* Fills `state` (SDL is asked again: the properties and the compositions). On the thread that made the window, at any
 * time, between frames or not; it never touches the GPU device's state. BTBH_GPU_INVALID_ARGUMENT for NULL or a window
 * not claimed. */
BTBHGPUResult btbh_gpu_window_hdr_state(BTBHGPUWindow *window, BTBHGPUWindowHDRState *state);

/* Takes HDR settings, and fills `state` with the result.
 * - A mode or composition change switches the swapchain now. That waits for the GPU once, and makes the HDR pipelines,
 *   the frame texture and the encode pass so the next frame makes nothing (see "Toggling").
 * - A change of nits only takes effect from the next frame, with no wait: a calibration screen can move the sliders
 *   every frame.
 * - Returns BTBH_GPU_OK when the settings are taken, even if HDR could not become active: then state->active is 0 and
 *   state->fallback and state->message say why, and the window is SDR and works.
 * - Refusals, BTBH_GPU_INVALID_ARGUMENT, with nothing taken and `state` filled with the window's current state where
 *   it can be: NULL window, settings or state; a window not claimed; a mode above 2; a composition not 0, 2 or 3;
 *   non-zero flags or reserved words; nits that aren't finite or are below 0; a mode or composition change while a
 *   frame is pending (a scene presented, its canvas not yet), with message "A frame is pending: switch HDR between
 *   frames". */
BTBHGPUResult btbh_gpu_window_set_hdr(
    BTBHGPUWindow *window,
    const BTBHGPUWindowHDRSettings *settings,
    BTBHGPUWindowHDRState *state
);

/* Writes the window's HDR line (the format under "The log line") into `buffer`, NUL-terminated and truncated to
 * capacity - 1. Returns the characters written. For the host's own log; BTBH_GPU_HDR_LOG=1 prints the same line at
 * the claim and at every change. */
#define BTBH_GPU_HDR_DESCRIBE_CAPACITY 512u
uint32_t btbh_gpu_window_hdr_describe(BTBHGPUWindow *window, char *buffer, uint32_t capacity);

/* btbh_gpu_window_render_target_create with flags. BTBH_GPU_RENDER_TARGET_FRAME_FORMAT makes it in the window's frame
 * format at the call (RGBA16_FLOAT in HDR), for a scene copy that keeps its highlights (a pause backdrop). 0 is
 * btbh_gpu_window_render_target_create. Other bits are refused (NULL, report->result BTBH_GPU_INVALID_ARGUMENT). */
enum { BTBH_GPU_RENDER_TARGET_FRAME_FORMAT = 1u << 0 };
BTBHGPUTexture *btbh_gpu_window_render_target_create_ex(
    BTBHGPUWindow *window,
    uint32_t width,
    uint32_t height,
    BTBHGPUTextureFilter filter,
    uint32_t flags,
    BTBHGPUTextureReport *report
);

/* The presented frame as RGBA16F halves, 8 bytes a pixel (request it as for the RGBA8 read-back). In HDR it is the
 * frame before the encode: extended sRGB-encoded, 1.0 = paper white, highlights above 1 (see "The encoding"). In SDR it
 * is the 8-bit frame widened. */
BTBHGPUResult btbh_gpu_window_readback_presented_frame_rgba16f(
    BTBHGPUWindow *window,
    uint16_t *rgba16f,
    uint32_t byte_capacity,
    BTBHGPUCanvasReport *report
);

/* BTBHGPUPlatformSnapshot: reserved[5] becomes hdr_changed and reserved[4] (offset 47; the size stays 56). */
    uint8_t hdr_changed;      /* 1: since the last poll SDL reported an HDR change, the window moved to another display,
                                 or the renderer switched or fell back; read btbh_gpu_window_hdr_state */

/* Defined when the declarations above exist. */
#define BTBH_GPU_HAS_HDR_OUTPUT 1
```

**Zero values.**
- A zeroed `BTBHGPUWindowHDRSettings` is mode OFF: SDR.
- Every existing struct keeps its size and meaning. `hdr_changed` takes a reserved byte, which hosts read as 0 and
  ignore.
- `BTBHGPUWindowReport.composition` and `.swapchain_format` report HDR values only after the host turned HDR on.
- `btbh_gpu_window_set_present_mode` keeps the window's composition; its comment changes to say so.

### The three formats

The frame context (`internal.h:144-156`) gains `frame_format`, `target_format` and the HDR parameters (composition,
paper white, P, the encode's scale). The window remembers its SDR format at the claim, which always starts in SDR.

| | SDR | HDR |
| --- | --- | --- |
| `swapchain_format` | B8G8R8A8_UNORM (or R8G8B8A8_UNORM) | RGB10A2_UNORM or RGBA16_FLOAT |
| `frame_format`: the window scene's last post targets and pipelines, the canvas's screen passes, captures | = swapchain | RGBA16_FLOAT |
| `target_format`: render targets, `render_to_target` (aside and target-view runtimes), the canvas's target passes into them | = swapchain | the SDR format remembered at the claim |

- **In SDR the three are equal,** so every existing check and pipeline sees the value it sees today.
- **The canvas keeps one pipeline set per colour-target format** (at most two: the SDR format and RGBA16F). A target
  pass into an RGBA16F render target (one made with `BTBH_GPU_RENDER_TARGET_FRAME_FORMAT`) is allowed in both modes.
  The check at `canvas.c:501` becomes "the target format or RGBA16F".
- **The scene's tail targets are kept per format.** The `ensure_*` functions compare format as well as size and make
  them again on a switch. **The tail pipelines are kept per format class** (SDR, HDR), so switching back and forth makes
  nothing new after the first time.
- **`render_to_target` stays SDR.** It writes `target_format` with the SDR shaders, as today. A frame-format RGBA16F
  target is refused there in phase 1 ("render a scene into an SDR render target"); an HDR scene into a target is
  phase 3, if a game asks.

### The HDR tone map

In HDR the post chain runs exactly as in SDR up to the tone map. Bloom, the HDR composite, the LUT, the grading and
CAS keep their passes and uniforms. Then:

1. **The SDR result:** `v_sdr`, the value the SDR output pass writes before its dither: `linear_to_srgb` of the tone
   map, grading and LUT, in 0-1.
2. **The scene's heat:** `m = exposure * max(x.r, x.g, x.b)`, with `x` the post-composite scene value (bloom, fog and
   vignette included) before the tone map. With the LUT on, the LUT pass is the last to see `x`, so its HDR variant
   computes the gain and writes it into alpha. The CAS and tilt-shift variants carry alpha through (tilt-shift blurs it
   with the colour). Without the LUT, the output pass computes `m` from its own input.
3. **The gain** (`btbh_hdr_gain`, identical in `production_hdr.hlsli` and the CPU reference `src/gpu/btbh_gpu_hdr.h`):

   ```
   P = peak_nits / paper_white_nits           (>= 1; on Apple min(that, headroom))
   t = max(m - m_k, 0) / tau
   g = 1 + (P - 1) * t^2 / (1 + t^2)
   ```

   - `g` is exactly 1 at and below the knee `m_k`, and C1 there (`dg/dm = 0`).
   - It rises monotonically and tends to `P`, where the SDR value has reached 1, so the brightest highlight shows at
     the peak.
   - **At P = 1 it is 1 everywhere: HDR then shows the SDR picture exactly.**
4. **The knee** is where the bare SDR curve (on grey, exposure 1, no grading) reaches `k`. The CPU finds `m_k` by
   bisection when the tone mapper or white point changes, and `tau = 4 * m_k`.

   | Tone mapper | k | m_k | tau |
   | --- | --- | --- | --- |
   | PBR Neutral | 0.76, its own shoulder start (`production_tonemap.hlsli:18`) | 0.80 | 3.2 |
   | ACES (Narkowicz) | 0.6 | 0.476 | 1.90 |
   | Reinhard, white point W | 0.6 | from W (1.254 at W = 4) | 4 m_k |

   `BTBH_GPU_HDR_KNEE=<k>` and `BTBH_GPU_HDR_SHOULDER=<tau / m_k>` override both for the play-test's tuning (question 5).
5. **The output:** `v_hdr = v_sdr * g^(1/2.2)`. That is the SDR-encoded value with the gain applied in display light
   (gamma 2.2, the next section). No 8-bit dither in HDR; the encode dithers at its own depth.

**What it does, in numbers** (ACES, exposure 1, paper white 203, peak 1000, so P = 4.93):

| Scene `m` | SDR display | HDR display | |
| --- | --- | --- | --- |
| 0.18 (mid grey) | 55 nits at paper white | 55 nits | the same: below the knee |
| 0.476 (knee) | 123 | 123 | the same |
| 1.0 | 164 | 210 | gain 1.28 |
| 4.0 | 198 | 800 | gain 4.04 |
| 16 | 203 (clipped white) | 988 | gain 4.87, near the peak |

**Hue.** `g` is one number per pixel, so a highlight keeps SDR's hue and saturation: SDR's shoulder desaturation stays
and a white-hot core stays white. That is the brief ("extend highlights, don't shift mid-tones"). More saturated
highlights would need the curve per channel in HDR; that is question 4.

### The encoding, paper white and the UI

- **The frame (RGBA16F) holds extended sRGB-encoded values:** the SDR encoding for 0-1, and above 1 for highlights.
  - The tail runs on it as in SDR, with the same shaders. FXAA's thresholds are relative, and EASU and Lanczos clamp to
    the neighbours. SMAA's thresholds are absolute: at or below 1 it sees SDR's values, and inside highlights it only
    finds more edges. The mirror is a blit.
  - RCAS gets an HDR variant: peak `P^(1/2.2)` instead of 1, and a clamp to `[0, peak]` instead of `saturate`.
  - FSR 3 keeps its LDR build. Values go up to `P^(1/2.2)` (2.07 at P = 4.93, 2.85 at P = 10). Phase 2 checks the
    highlights for ghosting, and falls back to (c)'s fold while FSR 3 runs if they do.
- **The scene's composite is blitted into the frame,** as into the swapchain today, but the frame is RGBA16F.
- **The canvas draws its screen passes over it with the SDR blend states,** so UI over mid-tones is SDR's to the
  rounding.
  - UI over a highlight blends with the highlight: a 60 % dark panel over a beam lets 40 % of the beam through.
  - **The one difference:** an additive batch whose sum passes 1 is no longer clipped by an 8-bit target, so it shows
    above paper white (question 12).
- **The encode pass** (`production_hdr_encode.frag`, a full-screen pass from the frame into the swapchain image,
  replacing the read-back blit of `canvas.c:1015-1043`):

  ```
  L = max(v, 0)^2.2                         relative light, 1.0 = paper white; the decode a typical SDR display applies
  L *= min(1, P / max(L.r, L.g, L.b))       hue-preserving clamp at the peak (only overshoot from sharpening reaches it)
  HDR10:          N = L * paper_white_nits;  E = PQ(M709to2020 * N / 10000) + dither(1/1023)
                  M709to2020 = [0.6274 0.3293 0.0433; 0.0691 0.9195 0.0114; 0.0164 0.0880 0.8956]
                  PQ: m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875
  Extended linear, Windows and gamescope:   E = L * paper_white_nits / 80
  Extended linear, Apple (EDR):             E = L                    (1.0 = the system's SDR white; P <= headroom)
  ```

  The dither is the output pass's own interleaved gradient noise, scaled to one 10-bit step.

- **Paper white:** the UI, and every scene pixel at or below the knee, show at `L * paper_white_nits`. That is the SDR
  picture as an SDR TV would show it at that brightness.
- **Decoding with gamma 2.2** rather than the inverse of `linear_to_srgb` matches what most TVs and monitors do with
  sRGB-encoded SDR. The piecewise sRGB curve would lift near-black (question 6).

### When HDR is available

`available` is 1 when the window's swapchain offers an HDR composition (the one named, if `settings.composition` names
one) and:

| Platform | and |
| --- | --- |
| Windows, Vulkan | SDL's `os_hdr_enabled` |
| Under Wine/Proton (`under_wine`) | nothing more. gamescope offers HDR pairs only when its output is HDR, and SDL's flag depends on `DXVK_HDR` |
| Apple | `hdr_headroom > 1` (Metal says every composition is supported) |
| Wayland, X11 | SDL's `os_hdr_enabled` |

- **AUTO** follows `available`.
- **FORCE** needs only an offered composition. On this PC's Vulkan, FORCE finds none: SDL's list matches the display.

### Toggling at runtime

`btbh_gpu_window_set_hdr` with a new mode or composition, between frames:

1. Re-queries the compositions and the properties.
2. Calls `SDL_SetGPUSwapchainParameters` with the composition and the current present mode. SDL waits for the GPU and
   re-creates the swapchain.
3. Makes, if missing:
   - the frame texture (RGBA16F at the drawable size; freed again on the switch back to SDR);
   - the encode pipeline for the swapchain's format;
   - the canvas's screen pipelines in the frame format;
   - the window scene runtime's tail pipelines in the new class, if a scene has presented. The window keeps the last
     post and canvas fragment-shader paths it was given; the HDR shaders are loaded as their siblings, like every post
     shader today (`scene.c:9540-9588`).
4. Warms SDL's blit pipeline for the frame format with a 1 x 1 blit. SDL makes blit pipelines per destination format
   on first use.
5. Logs the line, bumps `serial`, and sets `hdr_changed` for the next poll.

- **Targets that depend on the render size** (the tail at the new format) are made by the next present, or by
  `btbh_gpu_window_scene_prewarm`. That prewarm builds the window's current frame format and should be called after
  enabling.
- The lazy creation log names anything still made in a frame, under its existing scopes. New kinds: `hdr_encode`,
  `post_hdr`, `canvas_hdr`.
- The CPU time is in `last_switch_milliseconds`; expect a GPU wait plus a few pipelines the first time.
- **No restart is needed on Windows or Apple.** Under gamescope, a mode switch works only if the surface already had
  HDR pairs; see "Display and OS changes".
- **With RFC 0015's frame thread** this is a waiting call, like the present mode.

### Display and OS changes

- **`btbh_gpu_window_poll_events`** watches `SDL_EVENT_WINDOW_HDR_STATE_CHANGED` and `SDL_EVENT_WINDOW_DISPLAY_CHANGED`
  for its window. It marks the HDR state dirty and sets `hdr_changed`. It touches no GPU state, as RFC 0015 requires of
  it.
- **At the next frame's start** (the first present call with no frame pending), a dirty window re-queries.
  - In AUTO, a change of `available` switches the composition as `set_hdr` does. That frame waits for the GPU once.
  - The fallback reads `PENDING` until then.
- **The swapchain losing its HDR composition:**
  - SDL fails every acquire once its own re-creation finds the composition gone (`vulkan.c:4555-4566`, `:10005-10023`).
    On a failed acquire in HDR, the canvas present re-checks the composition.
  - If it is gone, the window falls back to SDR at once (fallback `LOST`, the line logged), drops that one frame and
    reports `BTBH_GPU_WINDOW_FAILURE` as today. The next frame is SDR.
  - In AUTO it returns to HDR when `available` does.
- **SDL 3.4.2's stale properties after a display move** (#16068):
  - On `DISPLAY_CHANGED` the renderer re-checks the compositions, so `available` follows the new display where the
    surface's list does. That is the case on NVIDIA's Vulkan per this PC's probe.
  - `os_hdr_enabled`, `sdr_white_level` and `hdr_headroom` may still be the old display's until SDL 3.6. The header says
    so.
- **gamescope, mid-game:** turning SteamOS's HDR on while the game runs adds no HDR pairs to the existing surface.
  - Phase 2 can add a settings flag that re-makes the surface (`SDL_ReleaseWindowFromGPUDevice` and
    `SDL_ClaimWindowForGPUDevice`; the renderer's GPU objects belong to the device and survive) when AUTO finds no HDR
    composition. Only if the Machine test shows it's needed (question 11).
  - Turning HDR off mid-game makes gamescope convert or fail the presents. The Machine test checks which; the LOST path
    covers a failed acquire.

### HDR metadata

- **SDL 3.4.2 can't send it** (see above), so `metadata_set` is 0 and the log line says why.
- Windows, gamescope and the TV then use their defaults. Under gamescope, the TV gets gamescope's own values.
- **When SDL has a call for it** (phase 3), the renderer sends:
  - the mastering display: BT.2020 primaries, D65, `max_luminance = peak_nits`, `min_luminance = 0.0001`;
  - MaxCLL = `peak_nits`;
  - MaxFALL = `min(peak_nits, 2 * paper_white_nits)`;
  - again whenever the nits change.
  - The values are non-zero, which gamescope requires.
- **Not by patching SDL.** RFC 0015 rejected a fork of SDL that every game builds; the same applies here.

### Captures, copies and the read-back

| Call | SDR | HDR |
| --- | --- | --- |
| `capture_pending_scene_texture` | as today | a texture in the frame format (RGBA16F), an exact copy: highlights kept |
| `copy_pending_scene_texture` into a frame-format target | as today | exact copy |
| ... into an SDR-format target (every target made before this RFC) | as today | a fold pass (`production_hdr_fold.frag`). In display light `L = v^2.2` with max channel `M`: kept at or below k = 0.6, above it scaled to `F(M) = k + (1 - k)(1 - e^-((M - k)/(1 - k)))`, hue kept, then encoded back. Highlights land under paper white, close to the SDR picture |
| ... an SDR frame into an RGBA16F target | - | a blit (exact in the encoding) |
| `readback_presented_frame` (RGBA8) | as today, byte for byte | the frame clamped to 0-1 and rounded: UI and mid-tones as SDR, highlights clipped at paper white |
| `readback_presented_frame_rgba16f` (new) | the 8-bit frame widened | the frame as encoded |
| `present_clear` | as today | the colour encoded on the CPU (decode 2.2, paper white, PQ or scRGB) and cleared into the swapchain directly |

**For v2's pause backdrop:** a target made with `BTBH_GPU_RENDER_TARGET_FRAME_FORMAT` at the pause (as now, once per
pause), then `copy_pending_scene_texture`. The frozen frame then looks like the live one, highlights included. An old
SDR-format target still works, with highlights folded (question 7).

### Scene views, the cost log, the frame thread

- **`present_views`:** the views compose before post, so the HDR variants run once over the composed frame, as the SDR
  passes do. The per-view composites (`scene.c:13513-13537`) write RGBA16F already.
- **The cost log:** one internal step, `BTBH_GPU_COST_STEP_HDR_ENCODE` (20; `BTBH_GPU_COST_STEP_COUNT` becomes 21), in
  the UI group (`internal.h:633`), so the encode shows apart from the scene's blit and the UI. The post group keeps its
  steps; their HDR variants report under the same names.
- **RFC 0015 phase 1b:**
  - The frame texture becomes "always on" there. In HDR it is this RFC's frame, and the window thread's present step
    runs the encode instead of the blit.
  - The frame context carries the HDR parameters, so the frame thread reads no window state.
  - `set_hdr` waits like `set_present_mode`. `hdr_state` and `hdr_describe` never wait.

### The log line

With `BTBH_GPU_HDR_LOG=1` the renderer prints one line through `SDL_Log` at the claim and at every change: a switch,
a set, an OS or display event, a fallback. `btbh_gpu_window_hdr_describe` returns the same text for the host's own log.
One line, `key=value`, fields in this order:

```
BTBH_GPU_HDR event=claim serial=1 gpu=vulkan wine=0 display=1 'LG ULTRAGEAR+' os_hdr=0 sdr_white=1.000 (80 nits)
  headroom=1.000 peak_reported=0 supported=SDR,SDR_LINEAR available=0 mode=off composition=SDR
  swapchain=BGRA8_UNORM colorspace=SRGB_NONLINEAR frame=BGRA8_UNORM target=BGRA8_UNORM paper=203 peak=1000 P=1.00
  metadata=0 (SDL 3.4.2 has no call) fallback=none switch_ms=0.0
```

(Wrapped here; one line in the log. These are this PC's values.)

| Field | Meaning |
| --- | --- |
| `event` | `claim`, `set`, `switch` (AUTO), `os` (SDL's HDR event), `display` (the window moved), `lost` |
| `gpu` | `SDL_GetGPUDeviceDriver` |
| `wine` | `under_wine` |
| `display` | id and `SDL_GetDisplayName` |
| `supported` | the compositions SDL reports for this window now |
| `available`, `mode` | the state's `available` and the settings' mode (`off`, `auto`, `force`) |
| `colorspace` | derived from the composition by SDL's own tables (`vulkan.c:258-263`, `metal.m:4596-4602`): `SRGB_NONLINEAR`, `HDR10_ST2084`, `EXTENDED_SRGB_LINEAR` (Vulkan); `SRGB`, `ITUR_2100_PQ`, `EXTENDED_LINEAR_SRGB` (Metal) |
| `peak_reported` | `display_peak_nits` |
| `paper`, `peak`, `P` | in use |
| `metadata` | `metadata_set`, with the reason when 0 |
| `fallback` | the BTBHGPUHDRFallback name, and `reason="..."` (the state's message) when not `none` |

### Debug switches

Read once, at the claim. Like `BTBH_GPU_COST_LOG`, not API.

| Switch | Effect |
| --- | --- |
| `BTBH_GPU_HDR_LOG=1` | The log line above |
| `BTBH_GPU_HDR=off\|auto\|force\|hdr10\|scrgb` | Overrides the host's mode at the claim and on every `set_hdr` (`hdr10` and `scrgb` force that composition). For tests and the Machine, before v2's option exists |
| `BTBH_GPU_HDR_NITS=<paper>,<peak>` | Overrides the nits |
| `BTBH_GPU_HDR_KNEE`, `BTBH_GPU_HDR_SHOULDER` | The tone map's `k` and `tau / m_k` |
| `BTBH_GPU_HDR_SIMULATE=hdr10\|scrgb` | Runs the whole HDR path with the swapchain left SDR. The encode writes an offscreen "virtual swapchain" of that format, which tests read back; the screen shows the frame clamped to SDR. How HDR is tested on an SDR PC |

## Minimal example

```c
BTBHGPUWindowHDRState hdr;
btbh_gpu_window_hdr_state(window, &hdr);
/* The Display option: offer HDR when hdr.available (or, greyed, say why: hdr.fallback). */

BTBHGPUWindowHDRSettings settings;
SDL_zero(settings);
settings.mode = BTBH_GPU_HDR_MODE_AUTO;
settings.paper_white_nits = saved.paper_white;   /* 0: 203 */
settings.peak_nits = saved.peak;                 /* 0: the display's, else 1000 */
if (btbh_gpu_window_set_hdr(window, &settings, &hdr) == BTBH_GPU_OK && !hdr.active) {
    /* SDR: hdr.message says why */
}
btbh_gpu_window_scene_prewarm(window, /* as at load */ ...);   /* the HDR tail at the render size */

while (running) {
    btbh_gpu_window_poll_events(window, &snapshot);
    if (snapshot.hdr_changed) {
        btbh_gpu_window_hdr_state(window, &hdr);     /* update the option; AUTO switches by itself */
        char line[BTBH_GPU_HDR_DESCRIBE_CAPACITY];
        btbh_gpu_window_hdr_describe(window, line, sizeof(line));
        log_line(line);
    }
    /* the calibration screen: settings.paper_white_nits / peak_nits changed -> btbh_gpu_window_set_hdr, no wait */
    /* ... scene present, canvas present, as today ... */
}
```

## Implementation

### Phase 1: HDR output (everything testable on this PC)

- **`window.c`:**
  - the state and settings, the availability rule and Wine detection;
  - the switch, with `set_present_mode` passing the current composition;
  - the events, AUTO at the frame's start, the LOST path, the log line and `describe`, the debug switches.
- **The frame context's three formats and HDR parameters:**
  - `canvas.c:927-958`;
  - every `frame_context->swapchain_format` use in `scene.c` and `canvas.c` reclassified as frame or target format;
  - the `ensure_*` functions checking format.
- **`canvas.c`:**
  - in HDR, the present target is always the frame texture;
  - the submit runs the encode instead of the read-back blit;
  - pipelines per target format;
  - the read-backs (RGBA8 clamp, new RGBA16F);
  - `present_clear`'s CPU encode.
- **`scene.c`:**
  - the HDR tail-pipeline class (output, tilt-shift, LUT, CAS, RCAS variants; FXAA, SMAA blend, EASU and Lanczos at
    RGBA16F);
  - the HDR uniforms: a second fragment uniform block at slot 1, 16 bytes (`P`, `m_k`, `tau`, `P^(1/2.2)`), so
    `PostUniforms` is unchanged;
  - captures and copies by format, and the fold.
- **`texture.c`:** `render_target_create_ex`.
- **Shaders, new:**
  - `production_hdr.hlsli`;
  - `production_post_hdr.frag`, `production_tilt_shift_hdr.frag`, `production_lut_hdr.frag`, `production_cas_hdr.frag`
    and `production_upscale_fsr_rcas_hdr.frag`. Each is a two-line wrapper (`#define BTBH_HDR_OUTPUT 1` and
    `#include` of the SDR source), and the SDR sources gain `#if BTBH_HDR_OUTPUT` blocks only;
  - `production_hdr_encode.frag` and `production_hdr_fold.frag`.
  - Seven built files more, in SPIR-V, MSL and metallib. **The SDR files' compiled bytes must not change:** the check
    hashes them against 0.16.1's build.
- **`src/gpu/btbh_gpu_hdr.h`:** the CPU reference (gain, knee search, 2.2 and sRGB encodings, PQ and its inverse, the
  matrix, the fold, the clear's encode), used by the renderer and the tests.
- **Tests:** see "Platforms and testing".
- **Estimate:** 7-10 days.

### Phase 2: on the Machine's results

- Tuning the knee, shoulder and decode exponent from the owner's look.
- FSR 3's highlights: the fold while FSR 3 runs, or an `HDR_COLOR_INPUT 1` build, only if they ghost.
- The surface re-make flag, if the Machine test shows the game stuck in SDR.
- Cost at 4K on this PC (simulated) and on the Machine (v2's cost log).
- Prewarm coverage from the lazy log.
- **Estimate:** 2-4 days.

### Phase 3: when a reason comes

- HDR metadata once SDL has a call.
- SDL 3.6's property fix (#16068), with the SDL bump done as a release of its own.
- An HDR scene into a frame-format target, if a game asks.
- RFC 0015 phase 1b's present step running the encode, whenever 1b lands.
- **Estimate:** 1-2 days each.

### Cost at 4K

Estimates. Traffic is counted at about 430 GB/s effective on the RTX 3070 Ti (608 GB/s peak); all of it is measured
in phase 2.

| Item | Extra traffic | Estimate |
| --- | --- | --- |
| Encode: read the frame (66 MB), write PQ (33 MB) or scRGB (66 MB) | 100-130 MB | 0.23-0.31 ms. The ALU (three powers and PQ per pixel) is small on this GPU |
| The scene's blit into an RGBA16F frame instead of the 8-bit swapchain | +66 MB | 0.15 ms |
| The tail at RGBA16F, native 4K with SMAA (output write, edge read, blend read and write) | about +130 MB | 0.3 ms |
| The tail at RGBA16F, FSR 3 from 1080p (tail at 1080p, RCAS writing 4K) | about +65 MB | 0.15 ms |
| The canvas blending on 64 bpp | depends on UI area | small |
| **Total** | | **+0.5 ms (FSR 3) to +0.7 ms (native, SMAA): 4-6 % of v2's 10-13 ms 4K High frame** |
| Memory | the frame (66 MB) and the tail targets doubled | +130-200 MB in HDR, freed in SDR |
| The Steam Machine | | Not known here: expect roughly the PC's milliseconds times the bandwidth ratio. Measure with the cost log |

## Compatibility

- **Versioning: minor (0.17.0).**
  - New functions, structs, enums and macros, two values appended to `BTBHGPUSwapchainComposition`, and a reserved
    byte given a meaning.
  - No existing struct changes size; no default changes; SDR frames are byte for byte 0.16.1's.
  - New shader files, rebuilt with the C core as every release.
- **Be The Bullet Hell v2 (Rust, bindgen):**
  - Regenerate the bindings. If `BTBHGPUSwapchainComposition` is a Rust enum (`rustified_enum`), v2 must take the two
    new values before turning HDR on: an unknown discriminant in a Rust enum is undefined behaviour.
  - It adds the Display option (AUTO / off), the calibration screen (`set_hdr` per slider move), the defaults, the
    backdrop's frame-format target, and its log of `hdr_describe` on `hdr_changed`.
- **Be The Bullet Hell (v1, Swift), CHIMERA (Rust), Tyrian3D (C):** nothing to change. They never turn HDR on, so they
  never see the new enum values. CHIMERA's bindings take them harmlessly when regenerated.

## Platforms and testing

### Native (every machine)

- **`tests/btbh_gpu_scene_order_test.c`:** the two new structs' sizes and offsets above; `BTBHGPUPlatformSnapshot` still
  56 bytes with `hdr_changed` at 47.
- **The public header test:** the header on its own as C11 and C++17.
- **`tests/btbh_gpu_hdr_test.c` (new, CPU only), against `btbh_gpu_hdr.h`:**
  - the gain: 1 at and below `m_k`; derivative 0 at `m_k`; monotonic; reaches `P` within 1 % by `m = m_k + 10 tau`;
    exactly 1 for P = 1;
  - the knee search against the table above;
  - PQ round trip within 1e-5;
  - PQ of 203 nits = 0.5807 and of 1000 nits = 0.7519, within 1e-4;
  - the matrix keeps white;
  - the fold: identity at or below 0.6, continuous, below 1;
  - the clear's encode.

### GPU smoke, Windows/Vulkan (this PC, no HDR display)

- **SDR identity, the release gate:** 421 images a mode, four modes (plain, shared meshes, no runs, runs verified),
  against 0.16.1. 0 px different, 0 run-verify mismatches. Also through `present_views` with one view, and with the cost
  log's pass frames. And the hash check of the SDR shaders' compiled files.
- **`btbh_gpu_hdr_smoke` (new), with `BTBH_GPU_HDR_SIMULATE`:**
  - **The SDR picture through the HDR path:** at P = 1, every identity scene's RGBA16F frame, clamped and rounded to 8
    bits, equals its SDR frame within ±1 code (SDR dithers, HDR doesn't).
  - **The mid-tones at P = 4.93:** pixels whose `m` is at or below `m_k` equal SDR within ±1 code; the others are at
    least as bright. That needs a debug output of `m`, available in simulate mode.
  - **The encode:** the virtual swapchain (RGB10A2 and RGBA16F) read back against the CPU reference applied to the
    RGBA16F read-back. Within 1 code in 10 bits with the dither off (the switch turns it off), 1e-3 relative for scRGB.
    Over the identity scenes plus a ramp, primaries and white at 0.5, 1, 2 and `P^(1/2.2)`.
  - **Toggling:** SDR, HDR, SDR ... ten times between frames. No error, no lazy creation after the first round
    following a prewarm, the SDR frames byte for byte the plain run's, `switch_count` 10.
  - **Pending:** `set_hdr` with a frame pending is refused and changes nothing.
  - **Captures and copies:** frame-format exact; an SDR target's fold equals the CPU fold within 1 code; the RGBA8
    read-back equals the clamp.
  - **Views:** the views smoke in simulate mode passes, both views lifted the same.
  - **RCAS:** the HDR variant at P = 1 equals the SDR RCAS within 1 code.
- **A real HDR display:** not on this PC. Windows reports no HDR capability for its monitor path.

### macOS/Metal

Compiles with the release. The Mac session can run `btbh_gpu_hdr_smoke` (simulate) with `check.sh`. A real EDR check
needs an EDR-capable display there: optional, no game on Metal asks.

### The Steam Machine test

v2 deploys with its `tools/deploy_machine.py` (the devkit protocol, `gamescopectl` screenshots, environment variables
per deploy); the owner launches. Each step is one launch. Everything comes back as files, and screenshots:
- **v2's own log,** with its `hdr_describe` lines. This is the sure copy.
- **Proton's log:** `PROTON_LOG=1` writes `steam-<appid>.log` in the home directory, with the gamescope layer's lines.
  It should also hold the renderer's `SDL_Log` lines, which go to stderr.

Researched, not verified: step 1 confirms the basics before the rest is worth running.

**Before the first step (owner):** Game Mode; Settings, Display: HDR on; the TV's HDR/game mode on. Note the SteamOS
version, the game's Proton version (Properties, Compatibility), the TV model, and the connection (HDMI or DisplayPort;
4K60 is enough).

| Step | v2 deploys | The owner does, and looks for on the TV | Send back |
| --- | --- | --- | --- |
| **1. The facts** | The build. `BTBH_GPU_HDR_LOG=1`, `PROTON_LOG=1`. v2's HDR off | Launch to the title screen, play 30 s, quit. The picture is the usual SDR one | The `BTBH_GPU_HDR event=claim` line (`supported` should list `HDR10_ST2084` and `HDR_EXTENDED_LINEAR`; `wine=1`; `os_hdr` 0 or 1). Proton log lines with `gamescope` and `hdr formats exposed to client`. **If `supported` lacks HDR, stop: check fullscreen and SteamOS's HDR, and send the log** |
| **2. Does Game Mode set `DXVK_HDR`?** | As 1, plus `DXVK_HDR=1` | As 1 | The claim line: compare `os_hdr`, `headroom`, `peak_reported` with step 1. If step 1 already had `os_hdr=1`, Game Mode sets it |
| **3. HDR on** | As 1 (no `DXVK_HDR`), `BTBH_GPU_HDR=auto` (or v2's option on) | Launch. The TV's HDR badge or info shows HDR10. Title screen: the UI as bright as in SDR, not dim, not glaring. A night street: lamps, neon and windows brighter, the dark parts unchanged. Daylight: sky bright, no banding. Fire, explosions, beams: bright cores. **Report:** washed-out grey (transfer or metadata), oversaturated colours (primaries), UI too dim or bright (paper white), black screen, flicker | The `set`/`switch` lines (`composition=HDR10_ST2084 swapchain=RGB10A2_UNORM colorspace=HDR10_ST2084 metadata=0`). A `gamescopectl` screenshot of each scene (send whatever file it writes). Optional: phone photos, HDR on and off, same scene and TV settings (for reference only) |
| **4. Paper white and peak** | As 3, `BTBH_GPU_HDR_NITS=203,1000`, then `=150,1000`, `=250,1000`, `=203,600`, `=203,1500` (or v2's calibration screen) | The same paused scene with each: the UI's white against the SteamOS overlay's white (open the Steam menu over it); where the brightest highlights stop getting brighter | Which paper white matches the overlay; the peak past which nothing changes. The lines (`paper`, `peak`, `P`) |
| **5. Toggle in game** | As 3, with v2's Display option wired to `set_hdr` | Switch HDR off and on 3 times. A short black frame is fine; no crash, no stuck dark or washed-out picture; the TV badge follows | The `set` lines with `switch_ms`. Anything odd, with the time |
| **6. SteamOS toggle mid-game** | As 3 | In game, Steam button, Settings, Display: HDR off; back to the game; then HDR on again; back | Every line (`os`, `lost`, `switch`, `fallback`). Whether the game is SDR, HDR or broken after each, and whether HDR came back without a restart (if not, question 11's flag is needed) |
| **7. Pause and screenshots** | As 3 | Pause in a bright scene: the frozen backdrop looks like the live frame (no dimming of the highlights). Take a v2 screenshot | The screenshot file; whether the backdrop dims |
| **8. SDR fallback** | As 3 | Settings, Display: HDR off **before** launching. The game starts SDR, the option says unavailable, no error | The claim line (`available=0`, `fallback=unsupported`) |
| **9. Cost** | As 3, `BTBH_GPU_COST_LOG=1`, v2's 4K preset; then the same with `BTBH_GPU_HDR=off` | Stand still in the same busy scene 30 s each | Both cost logs: the post and UI groups, the `hdr_encode` step, the frame time |
| **10. (Deck OLED, optional)** | As 3 on the Deck | As 3, on the panel | As 3 |

## Risks

| Risk | Mitigation |
| --- | --- |
| The SDR path changes by accident: the most-used path of every game | SDR runs none of the new code; the formats in SDR are the values of today; the identity gate (421 x 4, 0 px) and the SDR shaders' byte hashes |
| FSR 3's LDR build ghosts or flickers on highlights above 1 | Phase 2's check; then (c)'s fold while FSR 3 runs, or the HDR-input build |
| The gamescope path fails differently from the research (formats not offered, HDR lost on a toggle) | Step 1 before anything else; the LOST fallback keeps the game running in SDR; the surface re-make flag in phase 2 |
| SDL 3.4.2's stale properties after a display move | `available` follows the surface's list; the header says the nits may be the old display's; SDL 3.6 fixes it |
| No metadata: some TVs tone map as if mastered at 4000 nits, dimming highlights | The peak setting and the calibration screen; metadata once SDL can send it |
| Under Wine, SDR white reads 80 nits | The default is 203 regardless; v2 shows the OS value only as a hint |
| Additive UI above paper white | Question 12; a UI ceiling (a coverage mask in the encode) only if needed |
| A hitch on a toggle (a GPU wait, the first HDR pipelines) | `set_hdr` makes them; prewarm after it; measured in step 5 |
| 4K cost on the Machine | Step 9; HDR off is always available |
| Gamma 2.2 vs sRGB disagreement with the TV's SDR mode | Question 6; one constant |

## Alternatives

| Alternative | Why not |
| --- | --- |
| (a) SDR plus a gain map | Aliased and jittered highlight edges, exactly on beams |
| (c) A folded tail everywhere | Unfolds at every consumer of the composite; kept as FSR 3's fallback |
| (d) Inverse tone map of the SDR frame | Banding, no highlight detail, can't separate UI |
| Per-channel HDR curves | Shift mid-tones and grading; a LUT over 0-1 can't hold HDR. Question 4 for later |
| v2 picks the composition | v2 doesn't know the platform rules (Metal says yes to everything; gamescope converts scRGB); the renderer picks, v2 can override |
| Trusting SDL's HDR flag everywhere | Under Proton it depends on `DXVK_HDR`, which the player may not have |
| Trusting the swapchain's list everywhere | D3D12 offers HDR10 on this PC's SDR display (probe); Windows-native Vulkan drivers other than NVIDIA's are untested |
| Patching SDL for metadata and the display-move fix | A fork every game builds (RFC 0015's reason); 3.6 brings the fix, metadata when SDL adds it |
| An 8-bit UI layer composited in the encode | The canvas's multiplied mode reads the destination and its additive mode's alpha would hide the scene: not the SDR picture |

## Open questions for v2

Each has the renderer's recommendation; v2 answers per number.

1. **Who picks HDR10 or scRGB?** Recommended: the renderer (HDR10 first on Windows and the Machine; extended linear on
   Apple), with `settings.composition` 0. v2 keeps a hidden debug override at most.
2. **What does v2's "HDR: On" mean?** Recommended: `BTBH_GPU_HDR_MODE_AUTO`. HDR whenever the display can, SDR
   otherwise, following OS and display changes by itself. FORCE only for tests.
3. **Defaults.** Recommended:
   - paper white 203 nits everywhere (on Windows show the OS's SDR white, `display_sdr_white_nits`, as a hint only);
   - peak = `display_peak_nits` when it is above paper white, else 1000;
   - the calibration screen adjusts both.
4. **The highlights' colour.** Phase 1 lifts brightness and keeps SDR's hue and its shoulder desaturation; white-hot
   cores stay white. Recommended: start there and judge on the Machine; more saturated highlights (per-channel curves)
   only if the owner asks after seeing it.
5. **The knee and shoulder.** Recommended: constants (k = 0.6, or 0.76 for PBR Neutral; `tau = 4 m_k`), tuned once
   from the owner's look with `BTBH_GPU_HDR_KNEE` / `_SHOULDER`, not settings for players. Does v2 want them in its
   options instead?
6. **The SDR decode:** gamma 2.2 (what the owner's TV most likely does with SDR) or the piecewise sRGB curve (lifts
   near-black). Recommended: 2.2.
7. **The pause backdrop.** Recommended: make it with `render_target_create_ex(..., BTBH_GPU_RENDER_TARGET_FRAME_FORMAT)`
   at the pause, so the paused frame keeps its highlights. The old SDR target still works, with highlights folded under
   paper white.
8. **Screenshots in HDR.** The RGBA8 read-back clips at paper white; the RGBA16F read-back has the HDR frame.
   Recommended: keep PNGs from the RGBA8 read-back for now; an HDR screenshot format later if wanted.
9. **The toggle's hitch.** A GPU wait and, the first time, a few pipelines. Recommended: acceptable in the Display
   option; call `btbh_gpu_window_scene_prewarm` after enabling.
10. **Proton without `DXVK_HDR`.** Under Wine the renderer trusts the swapchain's colour spaces, so v2's deploy sets
    nothing for players. Recommended: yes. The Machine test's step 2 only finds out whether Game Mode sets
    `DXVK_HDR` (it would add the reported peak).
11. **SteamOS's HDR toggled while the game runs.** gamescope fixes the colour spaces when the surface is made.
    Recommended: if step 6 shows the game stuck in SDR after turning HDR on, the renderer adds a flag that re-makes the
    surface. Until then v2 says "restart to apply" in that case.
12. **Additive HUD elements.** They can pass paper white in HDR (no 8-bit clip). Recommended: look at v2's HUD on the
    Machine; add a UI ceiling only if its glows look too bright.
13. **The order with RFC 0015 phase 1b.** Recommended: HDR phase 1 first. 1b still waits for the Machine's CPU
    measurement, and its present step then runs the encode.
14. **The Machine run.** Recommended: one session with the owner for steps 1-9 at 4K60 over HDMI, on a build from
    phase 1's candidate. v2 sends the files back on the issue, and phase 2 starts from them.

## Sources

- SDL 3.4.2: `external/SDL` (`release-3.4.2`), files and lines as cited. SDL issue #16068:
  https://github.com/libsdl-org/SDL/issues/16068
- gamescope (`7ed5e25`):
  - WSI layer: https://github.com/ValveSoftware/gamescope/blob/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/layer/VkLayer_FROG_gamescope_wsi.cpp
    and its manifest https://github.com/ValveSoftware/gamescope/blob/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/layer/VkLayer_FROG_gamescope_wsi.json.in
  - Compositor: https://github.com/ValveSoftware/gamescope/blob/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/src/steamcompmgr.cpp
  - DRM backend: https://github.com/ValveSoftware/gamescope/blob/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/src/Backends/DRMBackend.cpp
  - Wayland backend: https://github.com/ValveSoftware/gamescope/blob/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/src/Backends/WaylandBackend.cpp
  - wlserver: https://github.com/ValveSoftware/gamescope/blob/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/src/wlserver.cpp
  - rendervulkan: https://github.com/ValveSoftware/gamescope/blob/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/src/rendervulkan.hpp
  - colorimetry: https://github.com/ValveSoftware/gamescope/blob/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/src/shaders/colorimetry.h
  - EDID: https://github.com/ValveSoftware/gamescope/blob/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/src/edid.cpp
  - Display profiles: https://github.com/ValveSoftware/gamescope/tree/7ed5e25f565f49d8be35fbf3cf1747020ec17e97/scripts/00-gamescope/displays
  - Maintainer on XWayland HDR: https://github.com/ValveSoftware/gamescope/issues/1225
- Proton:
  - Launcher script: https://github.com/ValveSoftware/Proton/blob/proton_11.0/proton
  - Changelog: https://github.com/ValveSoftware/Proton/wiki/Changelog
  - Valve's Wine: https://github.com/ValveSoftware/wine/blob/proton_10.0/dlls/win32u/sysparams.c,
    https://github.com/ValveSoftware/wine/blob/proton_11.0/dlls/winex11.drv/display.c,
    https://github.com/ValveSoftware/wine/blob/proton_11.0/dlls/winex11.drv/xrandr.c
  - GE-Proton README: https://github.com/GloriousEggroll/proton-ge-custom/blob/master/README.md
- Wine:
  - `VK_EXT_swapchain_colorspace`: https://gitlab.winehq.org/wine/wine/-/commit/bea86f01805b253567a2ee408cd03d6e445ba65d
  - `VK_EXT_hdr_metadata`: https://gitlab.winehq.org/wine/wine/-/commit/90416a48af01c07b19b620474c4a65888415fc9e
  - win32u's Vulkan: https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/win32u/vulkan.c
- DXVK:
  - `DXVK_HDR`: https://github.com/doitsujin/dxvk/blob/master/src/dxgi/dxgi_options.cpp
  - `GetDesc1`: https://github.com/doitsujin/dxvk/blob/master/src/dxgi/dxgi_output.cpp
  - EDID: https://github.com/doitsujin/dxvk/blob/master/src/wsi/wsi_edid.cpp
- The KDE HDR layer (not for gamescope): https://github.com/Zamundaaa/VK_hdr_layer
- Community reports on `DXVK_HDR` in Game Mode: https://github.com/sonic2kk/steamtinkerlaunch/pull/983,
  https://github.com/ValveSoftware/SteamOS/issues/1227
- Steamworks:
  - Steam Deck recommendations: https://partner.steamgames.com/doc/steamdeck/recommendations
  - Steam hardware recommendations: https://partner.steamgames.com/doc/steamhardware/recommendations
- Steam Machine press:
  - https://www.tweaktown.com/news/108862/steam-machine-can-output-4k-at-120hz-but-with-significant-sacrifice/index.html
  - https://heise.de/-11107440
  - https://www.pcgameshardware.de/Steam-Machine-Hardware-256919/News/SteamOS-39-HDMI-FRL-4K144-VRR-Problem-1553675/
- Vulkan: `vkSetHdrMetadataEXT`: https://docs.vulkan.org/refpages/latest/refpages/source/vkSetHdrMetadataEXT.html
