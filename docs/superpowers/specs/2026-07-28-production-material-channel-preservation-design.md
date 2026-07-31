# Production material-channel preservation design

Status: automatski odobren za izvedbu

Date: 2026-07-28

Parent design:
`docs/superpowers/specs/2026-07-26-phase-3-4-windows-game-integration-design.md`

## Goal

Preserve the material inputs required by the later PBR pass across the
production GPU model-import and scene-submission path:

- normal texture and normal scale;
- combined occlusion/roughness/metalness texture;
- occlusion, roughness and metalness scalar factors.

This slice fixes representation and lifetime loss only. The current
simplified forward shader may continue to ignore these inputs until the
separate PBR/IBL pass is implemented. Completion must not be reported as
PBR visual parity.

## Canonical channel convention

The production seam already names the packed texture `ormTexture`; its
channels are:

- red: occlusion;
- green: roughness;
- blue: metalness.

glTF's metallic-roughness texture supplies green roughness and blue
metalness and is retained as the production ORM texture. The imported
occlusion strength is preserved independently. If a separate occlusion
texture is present, it may share the metallic-roughness image; combining
distinct source images is deferred to the material-preparation/PBR slice.

Synthetic slot-zero and standalone descriptor defaults match
`MaterialDescriptor.default`:

- normal scale `1`;
- occlusion `1`;
- roughness `1`;
- metalness `0`.

Actual glTF materials retain glTF defaults, including metalness `1` when
the optional `pbrMetallicRoughness` object is absent.

## Native import and ownership

`BTBHGPUModelMaterialInfo` owns optional normal and ORM `BTBHGPUTexture`
handles alongside its existing albedo and emission handles. glTF import
copies the metallic and roughness factors and normal scale, loads the
normal texture and metallic-roughness texture through the same
external/embedded image path already used by albedo and emission, and
destroys every owned texture exactly once with the model.

All error paths use the existing model destruction path, so a partial
material import cannot leak a successfully loaded earlier texture.

The public C ABI and `BTBHGPUSceneGeometryBatch` carry the same texture
pointers and scalar factors through scene preparation. Native validation
rejects foreign or dead texture handles even though the simplified
forward shader does not bind the new samplers yet.

## Swift production representation

`GPUSceneMaterialProbe` carries the six new values. Values originate from:

1. imported `BTBHGPUModelMaterialInfo`;
2. standalone `MaterialDescriptor`;
3. a model material override, which inherits imported factors and replaces
   `normalTexture` and/or `ormTexture` only when the patch supplies them.

Existing albedo/emission, blend, billboard, depth, transparency and unlit
behavior remains unchanged.

## Verification

- A deterministic glTF fixture contains non-default metallic/roughness
  factors plus normal and metallic-roughness textures.
- A real Windows/Vulkan GPU test proves the imported Swift material retains
  both texture handles and exact scalar values.
- Focused tests prove standalone descriptors and model patches propagate
  the same values into scene material probes.
- Native C tests, full GPU Swift tests, base Swift tests and `git diff
  --check` remain green.
- No golden, SSIM threshold or performance threshold changes are allowed.
