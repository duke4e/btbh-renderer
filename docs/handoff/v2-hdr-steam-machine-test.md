# HDR on the Steam Machine: the test v2 deploys and the owner runs (RFC 0017)

For Be The Bullet Hell v2's session and the owner. Renderer 0.17.0's HDR output (RFC 0017, phase 1) has been checked on
the Windows PC only offscreen: the PC's monitor connection has no HDR. The Proton/gamescope path is researched, not
run (RFC 0017, "Proton, gamescope and the Steam Machine"). This run is the first time the HDR path meets a real HDR
display through Proton and gamescope. Its results decide phase 2: tuning, the FSR 3 check, the surface re-make flag.

v2 deploys each step with its `tools/deploy_machine.py` (the devkit protocol, `gamescopectl` screenshots, environment
variables per deploy). The owner launches the game and looks at the TV. Each step is one launch. Everything comes back
through v2 as files.

## What to send back, every step

- **v2's own log.** v2 writes the renderer's HDR line into it: `btbh_gpu_window_hdr_describe` at start-up and whenever
  a poll's snapshot has `hdr_changed`. This is the copy that counts.
- **Proton's log.** `PROTON_LOG=1` writes `steam-<appid>.log` in the home directory. It holds the gamescope layer's
  line `hdr formats exposed to client: true/false`, and should also hold the renderer's own lines (`BTBH_GPU_HDR ...`,
  printed with `BTBH_GPU_HDR_LOG=1` through SDL_Log to stderr).
- **Screenshots** with `gamescopectl screenshot`: send whatever file it writes (an HDR output may give an AVIF).
- **The owner's notes:** what the TV showed, in a sentence or two per scene.

## The renderer's line

One line per event; the fields are always in this order:

```
BTBH_GPU_HDR event=claim serial=1 gpu=vulkan wine=1 simulate=off display=1 '<TV name>' os_hdr=0 sdr_white=1.000
(80 nits) headroom=1.000 peak_reported=0 supported=SDR,SDR_LINEAR,HDR_EXTENDED_LINEAR,HDR10_ST2084 available=1
mode=off composition=SDR swapchain=BGRA8_UNORM colorspace=SRGB_NONLINEAR frame=BGRA8_UNORM target=BGRA8_UNORM paper=203
peak=1000 P=1.00 metadata=0 (SDL 3.4.2 has no call) fallback=none switch_ms=0.0
```

(Wrapped here; one line in the log. The values are an example.)

| Field | What to look at |
| --- | --- |
| `event` | `claim` (window start), `set` (the game changed the setting), `switch` (Auto followed the OS or display), `os` / `display` (SDL reported a change), `recheck` / `state` (what SDL reports changed with no event), `lost` (the swapchain lost HDR, or 3 presents in a row found no image; the game went to SDR), `encode` (the HDR encode can't be made; SDR). `lost` and `encode` print even without `BTBH_GPU_HDR_LOG=1` |
| `wine` | 1 on the Machine |
| `os_hdr`, `sdr_white`, `headroom`, `peak_reported` | what SDL reports. Under Proton these depend on `DXVK_HDR` (step 2) |
| `supported` | the compositions the swapchain offers. On the Machine with HDR on and the game fullscreen it should list `HDR10_ST2084` and `HDR_EXTENDED_LINEAR` |
| `available` | 1 when HDR can be shown |
| `mode`, `composition`, `swapchain`, `colorspace` | what the game asked for and what is in use. HDR on: `HDR10_ST2084`, `RGB10A2_UNORM`, `HDR10_ST2084` |
| `paper`, `peak`, `P` | the nits in use and their ratio |
| `metadata` | 0: SDL 3.4.2 can't send HDR metadata; the TV gets gamescope's own values |
| `fallback`, `reason` | why HDR is off when it was asked for |

## Before the first step (owner)

1. Game Mode (not Desktop Mode).
2. Steam button, Settings, Display: **HDR on**. The TV in its HDR/game mode.
3. Note: the SteamOS version (Settings, System), the game's Proton version (the game's Properties, Compatibility),
   the TV's make and model, and the connection (HDMI or DisplayPort). 4K at 60 Hz is enough.

## The steps

Each step: what v2 deploys, what the owner does and looks for, what comes back. "As 3" means step 3's build and
variables.

### 1. The facts

- **Deploy:** the candidate build; `BTBH_GPU_HDR_LOG=1`, `PROTON_LOG=1`; v2's HDR setting off.
- **Owner:** launch to the title screen, play 30 seconds, quit. The picture is the usual SDR one.
- **Send back:** the `BTBH_GPU_HDR event=claim` line, and the Proton log's lines with `gamescope` or
  `hdr formats exposed`.
- **Expected:** `wine=1`; `supported` lists `HDR10_ST2084` and `HDR_EXTENDED_LINEAR`; `os_hdr` 0 or 1.
- **If `supported` lacks the HDR compositions, stop here** and send the logs. Check that the game runs fullscreen and
  that SteamOS's HDR is on; the gamescope layer only offers HDR to a fullscreen window when HDR was on at start-up.

### 2. Does Game Mode set `DXVK_HDR`?

- **Deploy:** as 1, plus `DXVK_HDR=1`.
- **Owner:** as 1.
- **Send back:** the claim line.
- **Compare with step 1:** `os_hdr`, `headroom`, `peak_reported`. If step 1 already had `os_hdr=1`, Game Mode sets
  `DXVK_HDR` by itself. Players need nothing either way: under Wine the renderer trusts the swapchain's list.

### 3. HDR on

- **Deploy:** as 1 (no `DXVK_HDR`), plus `BTBH_GPU_HDR=auto` (or v2's Display option set to On).
- **Owner:** launch. Look for:
  - the TV's HDR badge or its info screen saying HDR10;
  - the title screen: the UI as bright as in SDR, neither dim nor glaring;
  - a night street: lamps, neon and lit windows brighter than in SDR, the dark parts the same;
  - daylight: the sky bright without banding;
  - fire, explosions, beams: bright cores.
- **Report anything like:** a washed-out grey picture (wrong transfer or metadata), oversaturated colours (primaries),
  UI too dim or too bright (paper white), a black screen, flicker.
- **Send back:** the `set` or `claim` line with `composition=HDR10_ST2084 swapchain=RGB10A2_UNORM
  colorspace=HDR10_ST2084 metadata=0`; a `gamescopectl` screenshot of each of those scenes. Optional: phone photos of
  the same scene with HDR on and off at the same TV settings (for reference only).

### 4. Paper white and peak

- **Deploy:** as 3, five launches with `BTBH_GPU_HDR_NITS=203,1000`, `=150,1000`, `=250,1000`, `=203,600` and
  `=203,1500` (or v2's calibration screen, if it is in the build).
- **Owner:** the same paused scene each time.
  - Paper white: open the Steam menu over the game and compare the game's UI white with the overlay's white.
  - Peak: where the brightest highlights stop getting brighter.
- **Send back:** which paper white matches the overlay; the peak past which nothing changes; the lines (`paper`,
  `peak`, `P`).

### 5. The game's own toggle

- **Deploy:** as 3, with v2's Display option wired to `btbh_gpu_window_set_hdr`.
- **Owner:** switch HDR off and on three times in the options. A short black frame is fine. Look for: no crash, no
  picture stuck dark or washed out, the TV's badge following.
- **Send back:** the `set` lines (`switch_ms` is the switch's time); anything odd, with the time.

### 6. SteamOS's toggle while the game runs

- **Deploy:** as 3.
- **Owner:** in game, Steam button, Settings, Display: HDR off; back to the game. Then HDR on again; back to the game.
- **Send back:** every line (`os`, `lost`, `switch`, `fallback`). After each change: is the game SDR, HDR or broken? Did
  HDR come back without restarting the game? If it did not, the renderer adds the surface re-make flag (RFC 0017,
  question 11); until then v2's text says "restart to apply".

### 7. Pause and screenshots

- **Deploy:** as 3.
- **Owner:** pause in a bright scene. The frozen backdrop should look like the live frame: no dimming of the
  highlights. Take a v2 screenshot.
- **Send back:** the screenshot file; whether the backdrop dimmed.

### 8. SDR fallback

- **Deploy:** as 3.
- **Owner:** Settings, Display: HDR **off before** launching the game. The game starts in SDR, the option says
  unavailable, no error.
- **Send back:** the claim line (`available=0`, `fallback=unsupported`). Then turn SteamOS's HDR back on.

### 9. Cost

- **Deploy:** as 3, plus `BTBH_GPU_COST_LOG=1` and v2's 4K preset; then the same with `BTBH_GPU_HDR=off`.
- **Owner:** stand still in the same busy scene for 30 seconds each time.
- **Send back:** both cost logs: the frame's `gpu_ms`, the `post` and `ui` groups (the HDR encode is in `ui`), and the
  frame time.

### 10. The Deck OLED (optional)

- **Deploy:** as 3, on the Deck.
- **Owner:** as 3, on the Deck's own screen.
- **Send back:** as 3.

## The renderer's switches used here

| Switch | Effect |
| --- | --- |
| `BTBH_GPU_HDR_LOG=1` | The `BTBH_GPU_HDR` line at the window's start and at every change |
| `BTBH_GPU_HDR=off\|auto\|force\|hdr10\|scrgb` | Overrides the game's HDR setting (`hdr10` and `scrgb` force that composition) |
| `BTBH_GPU_HDR_NITS=<paper>,<peak>` | Overrides the nits |
| `BTBH_GPU_HDR_KNEE=<k>`, `BTBH_GPU_HDR_SHOULDER=<scale>` | The highlight curve's knee (0.6, or 0.76 for PBR Neutral) and shoulder (4 x the knee), for tuning after the owner's look |
| `BTBH_GPU_COST_LOG=1` | The GPU cost log |

Players need none of them, and none of Proton's: no `DXVK_HDR`, no `PROTON_ENABLE_WAYLAND`, no `PROTON_USE_WINED3D`, no
`DISABLE_GAMESCOPE_WSI`, no `ENABLE_HDR_WSI`.
