# HRC Material Response Design

Status: implemented and verified

Date: 2026-08-23

Parent design:
`docs/superpowers/specs/2026-08-23-rc-roadmap-1-7-windows-design.md`

## Goal

Make the final HRC result a material-aware HDR lighting result rather than a
tinted brightness overlay. Preserve the existing RC-native direct, bounced,
environment, material-AO, contact-AO, and emission separation while closing the
unlit-material leak found by the audit.

## G-buffer contract

- albedo-material RGB/A stores linear albedo and metalness;
- normal RGB/A stores encoded normal and material occlusion;
- specular-material RGB/A stores dielectric/metal F0 and roughness;
- emission RGB stores authored emission, or albedo plus authored emission for
  an unlit material;
- emission A stores an unlit mask (`1` unlit, `0` lit).

The emission render target is already HDR RGBA and cleared to zero, so this
adds no texture, pass, bandwidth class, or public ABI.

## Resolve contract

Lit surfaces retain the existing split:

- source emission is added once and is not multiplied by AO or the indirect
  intensity slider;
- RC-native direct diffuse is material colored and not controlled by the
  indirect slider;
- transported radiance is evaluated with albedo, normal, roughness,
  metalness/F0, Cook-Torrance BRDF, material AO, and indirect intensity;
- broad environment diffuse/specular uses material response and material AO,
  but remains independent from the local-bounce intensity slider;
- the existing post contact-AO mode preserves emission and attenuates only the
  occludable lighting remainder.

Unlit surfaces bypass direct, transported, and environment response. Final
indirect returns only emission RGB. HRC returns full confidence for that exact
surface value so reconstruction does not mix nearby lighting into it; direct-
light debug returns black for unlit surfaces.

Both classic RC and HRC implement the same unlit contract to prevent backend
behavior drift.

## Verification

- Focused shader-contract tests cover BRDF inputs, AO placement, intensity
  placement, emission separation, unlit mask encoding, and contact-AO emission
  preservation in both resolve shaders.
- A Vulkan GPU pixel test exercises HRC final-indirect material response and
  proves unlit output is invariant under strong external RC lighting.
- Production shader reflection, Release build, deterministic final-indirect
  capture, and no-readback profile close the item.
