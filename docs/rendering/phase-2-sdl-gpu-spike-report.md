# Phase 2 SDL_GPU spike report

Status: **Windows vertical-foundation checkpoint; Phase 2 is not closed**

Date: 2026-07-26

This checkpoint proves the pinned toolchain, explicit Windows/Vulkan device
contract, window/swapchain lifecycle, offline shader artefacts and a minimal
offscreen graphics+compute+readback path. It does not yet claim the complete
Phase 2 scene or Windows/macOS parity gate.

## Source and host

- Renderer baseline before this checkpoint:
  `6c9cc49432e66fcc02f7fcb00dc9f66f5d590d15`
- SDL: `683181b47cfabd293e3ea409f838915b8297a4fd`
  (`release-3.4.2`)
- SDL_shadercross:
  `e55cf5e31ced6f3d1be5cc6d0c50e99384f9f4ba`
- OS: Microsoft Windows 10 Pro 10.0.19045
- CPU: AMD Ryzen 9 9950X 16-Core Processor
- Selected GPU: NVIDIA GeForce RTX 3070 Ti
- NVIDIA Windows driver: `32.0.16.1074`
- Secondary adapter present: AMD Radeon Graphics, driver `32.0.12033.1030`

## Proven contract

- Requested and actual SDL_GPU driver: exactly `vulkan`.
- Hardware-accelerated Vulkan is required at device creation.
- Required feature mask: `0x00000000000001ff`.
- Supported feature mask: `0x00000000000001ff`.
- Missing feature mask: `0x0000000000000000`.
- Public `btbh_gpu` header exposes no SDL, Vulkan or Metal API types.
- Hidden high-DPI window creation, device claim, SDR swapchain selection,
  resize and clean shutdown pass.
- Offline HLSL produces SPIR-V, MSL and JSON reflection. The fragment
  reflection reports three outputs and the compute reflection reports one
  read-write storage texture with `8x8x1` threads.
- A Vulkan offscreen pass writes:
  - G0 `RGBA8_UNORM`;
  - G1 `RGBA8_UNORM`;
  - G2 `RGBA16_FLOAT`;
  - sampleable `D32_FLOAT` depth.
- A compute pass writes deterministic `RGBA16_FLOAT` data and the same command
  buffer reads back both compute output and G0.

The original minimal scene remains as a diagnostic-triangle regression test.
The expanded scene checkpoint below now covers the planned pass families, but
the final Windows performance/resize gate and native macOS gate remain open.

## Resource and frame lifecycle checkpoint

The source after the vertical-foundation commit now also contains the
backend-neutral ownership seam required by the larger scene:

- opaque, fixed-capacity resource registry;
- generation-checked 64-bit handles;
- exact resource-kind validation;
- atomic rejection when the registry or deferred queue is full;
- retirement keyed by completed submission serial;
- stable deferred collection and exactly-once release callbacks;
- fixed frame slots that cannot be reused before their retire serial;
- generation-checked frame tokens;
- per-slot upload/readback arenas with no warm-frame heap allocation;
- power-of-two allocation alignment and overflow-safe row-pitch alignment.

Focused tests cover stale handles, double retirement, type mismatch, deferred
release, resize-style texture recreation, full queues, stale frame tokens,
arena exhaustion, 100 warm frame cycles and row-pitch overflow. Both public
GPU headers are separately scanned for SDL, Vulkan and Metal API leakage.

The expanded scene adds concrete pipeline, transfer and readback helpers. The
existing minimal spike remains unchanged as the smaller regression path.

## Expanded deterministic scene checkpoint

The expanded Vulkan runtime executes this fixed 11-pass sequence:

1. instance, bone and interval-buffer upload;
2. directional shadow depth;
3. spot shadow depth;
4. opaque G-buffer draw;
5. 14-instance G-buffer draw;
6. skinned G-buffer draw with four storage-buffer bone matrices, including a
   deterministic non-identity 22.5-degree animation sample;
7. deterministic compute-pattern write;
8. interval-buffer compute trace;
9. deferred-lighting fullscreen draw sampling all three G-buffer targets and
   the compute-pattern texture plus directional and spot depth maps;
10. alpha-blended forward draw;
11. final-color and interval-texture readback.

The canonical backend-neutral scene contract fixes seed `0x42544248`, camera,
16 instances, four bones, animation time `1250 ms`, entity counts and pass
order. The same contract now generates the exact instance, bone and interval
payload uploaded by the runtime. Its FNV-1a signature covers both the pass
manifest and canonical fixture bytes:

`7cc30645be466e5f`

Windows/Vulkan 1920x1080 capture A and B were byte-identical:

- SHA-256:
  `9CD14A90A5A562937C4F4F7E23A2A7311151A66BC9F8672A503572B5E0FCC73E`;
- final-image FNV-1a checksum: `58b10bf80d3441b1`;
- center final pixel: `[47, 117, 164, 255]`;
- directional-only shadow pixel: `[58, 96, 131, 255]`;
- skinned-region final pixel: `[130, 67, 87, 255]`;
- center interval cue: `[0.125000, 0.125000, 0.000000, 1.000000]`;
- driver: exactly `vulkan`;
- executed pass count: `11`.

All 24 new Phase 2 SPIR-V, MSL and JSON reflection artefacts were rebuilt and
remained byte-identical by SHA-256.

The expanded 1920x1080 result was manually inspected. It shows the expected
4x4 colored geometry fixture, deterministic lighting gradient, visibly
rotated animated-bone instances and transparent overlay without corruption.
The 320x180 runtime regression also checks center RGB/alpha, a dedicated
directional-only shadow cue, an animated-bone region cue, interval RGB/alpha
and Windows final-image checksum `d2ee844acc01f04c`. Metal checks the same
tolerant semantic cues, including separate shadow-map contribution and the
skinned region, without falsely requiring byte-identical antialiased edges.

Both shadow maps now execute a real depth pipeline and contribute independently
to the lighting shader through six reflected sampler bindings. Shadow depth
uses a dedicated nearest sampler so D32 does not rely on optional Vulkan linear
filter support. The spike uses deterministic screen-aligned depth lookup to
expose cross-backend binding and depth-sampling failures; production
light-space projection, comparison sampling, bias and filtering remain later
renderer work.

This is not yet the final Windows gate. Warm in-process frame metrics,
resize/minimize coverage and external GPU-capture validation are still
pending.

## Determinism evidence

All nine offline artefacts were compiled a second time and matched
byte-for-byte:

| Artefact | SHA-256 |
| --- | --- |
| `vertical_spike.vert.spv` | `DC0EF122949FEA3D69B9D14337B3EE220759EA5A59EEFC8DFF5958AB3D3C670A` |
| `vertical_spike.vert.msl` | `E7CF636738E9CDCA1952ADD303D834D6E23B90C3C229DCFAEC7CE00B6FBD0E09` |
| `vertical_spike.vert.json` | `D6C0E09D4FC9F6F24CAC8DB3680E56679049B759CD9BDD5E0811D98163BDF4BF` |
| `vertical_spike.frag.spv` | `DF7EC1A49D3E3B6BC253F8CA0BC1B208D7D074F62C56704E160D40AC285079EC` |
| `vertical_spike.frag.msl` | `677B2717E84A7F13EA4C48FEC9BEB5AFC7EDD3566AC98A4D145C70B796BCEA04` |
| `vertical_spike.frag.json` | `3CF0BE392951F67A087A90D183E594FF321C718E8981CCE449A898EDCDAD07DC` |
| `vertical_spike.comp.spv` | `C28378C4C87B019A551B032961F4EA938B1FD0EE966FA6C9BDBDFF6033002F9C` |
| `vertical_spike.comp.msl` | `F60F15852947887C67CFBE5D408600307874222A1736455A1310CF16850498A9` |
| `vertical_spike.comp.json` | `FF1BA3DB9DDA2B69DDC06E1003C6010134B0F6DDB3351F7BC6E5B96B3CE1AECD` |

After one warm-up, two independent 1920x1080 captures matched exactly:

- Capture A:
  `4918B7B89FFD10ACE4B4AFDDED8063C4EC26CEA8BC3B1097DD54AC60DDAF028F`
- Capture B:
  `4918B7B89FFD10ACE4B4AFDDED8063C4EC26CEA8BC3B1097DD54AC60DDAF028F`
- Center G0 pixel: `[129, 93, 91, 255]`
- Center compute pixel:
  `[0.507812, 0.507812, 0.250000, 1.000000]`

These captures prove deterministic output for the minimal checkpoint only.
They are not replacements for the final Phase 2 scene captures or the
Phase 0-1 goldens.

## Verification

- `swift test`: 65 XCTest tests and 1 Windows Swift Testing smoke passed.
- `cmake --build out/build/x64-gpu-debug --parallel 8`: passed.
- `ctest --test-dir out/build/x64-gpu-debug --output-on-failure`: 12/12
  passed.
- `cmake --preset x64-release` followed by the `r3d` target build: passed.
- `git diff --check`: passed before report creation and must be repeated at
  commit time.

The vendored DXC configure emits an untriggered Windows path-length warning
for `dxrfallbackcompiler`; the actually required `shadercross`, DXC and
vertical-spike targets build successfully in the current repository path.
The missing `METAL_IRCONVERTER` configure warning does not affect the
HLSL-to-SPIR-V/MSL path and is not evidence for the still-unrun macOS gate.

## Remaining gates

1. route concrete scene ownership through the generation/deferred-release
   seam;
2. structured per-frame metrics and warmed median/p95/worst results;
3. final Windows resize/minimize/restore, capture and diagnostics series;
4. native macOS/Metal metallib build, captures and cross-backend SSIM/manual
   comparison.
