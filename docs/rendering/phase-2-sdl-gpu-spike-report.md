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

The minimal scene is intentionally only a diagnostic triangle. Directional
and spot shadows, instancing, skinning, deferred lighting, transparent draw
and interval-trace compute remain required before the final Phase 2 gate.

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

Concrete pipeline and readback wrappers will be introduced with the expanded
scene; the existing minimal spike still owns its concrete SDL objects
directly.

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
- `ctest --test-dir out/build/x64-gpu-debug --output-on-failure`: 9/9
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

1. concrete pipeline/readback wrappers using the new ownership seam;
2. full deterministic scene and semantic signature;
3. shadows, instancing, skinning, transparent and deferred-lighting passes;
4. interval-trace compute and compute-to-graphics sampling;
5. structured per-frame metrics and warmed median/p95/worst results;
6. final Windows resize/minimize/restore, capture and diagnostics series;
7. native macOS/Metal metallib build, captures and cross-backend SSIM/manual
   comparison.
