import Foundation
import XCTest
@testable import BTBHRenderer

final class RadianceCascadesTests: XCTestCase {
    func testEmitterUploadUsesPersistentStagingAndMainSceneCommand() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_radiance_cascades.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let runtime = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_radiance_cascades_runtime.h"
            ),
            encoding: .utf8
        )
        let prepareStart = try XCTUnwrap(source.range(
            of: "static bool btbh_gpu_radiance_cascades_ensure_emitter_buffer("
        ))
        let prepareEnd = try XCTUnwrap(source.range(
            of: "\nvoid btbh_gpu_radiance_cascades_runtime_initialize(",
            range: prepareStart.upperBound..<source.endIndex
        ))
        let prepare = source[prepareStart.lowerBound..<prepareEnd.lowerBound]

        XCTAssertTrue(runtime.contains(
            "SDL_GPUTransferBuffer *emitter_transfer_buffer;"
        ))
        XCTAssertTrue(prepare.contains(
            "runtime->emitter_transfer_buffer"
        ))
        XCTAssertTrue(prepare.contains(
            "SDL_MapGPUTransferBuffer("
        ))
        XCTAssertFalse(prepare.contains("SDL_AcquireGPUCommandBuffer"))
        XCTAssertFalse(prepare.contains("SDL_SubmitGPUCommandBuffer"))
        XCTAssertTrue(source.contains(
            "SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);"
        ))
        XCTAssertTrue(source.contains(
            "destination_region.size = runtime->emitter_upload_size;"
        ))
    }

    func testOcclusionUploadUsesPersistentStagingAndMainSceneCommand() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_radiance_cascades.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let runtime = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_radiance_cascades_runtime.h"
            ),
            encoding: .utf8
        )
        let prepareStart = try XCTUnwrap(source.range(
            of: "static bool btbh_gpu_radiance_cascades_prepare_occlusion_upload("
        ))
        let prepareEnd = try XCTUnwrap(source.range(
            of: "\nstatic void btbh_gpu_radiance_cascades_release_occlusion_textures(",
            range: prepareStart.upperBound..<source.endIndex
        ))
        let prepare = source[prepareStart.lowerBound..<prepareEnd.lowerBound]

        XCTAssertTrue(runtime.contains(
            "SDL_GPUTransferBuffer *occlusion_transfer_buffers["
        ))
        XCTAssertTrue(prepare.contains(
            "runtime->occlusion_transfer_buffers[index]"
        ))
        XCTAssertTrue(prepare.contains("SDL_MapGPUTransferBuffer("))
        XCTAssertFalse(prepare.contains("SDL_AcquireGPUCommandBuffer"))
        XCTAssertFalse(prepare.contains("SDL_SubmitGPUCommandBuffer"))
        XCTAssertTrue(source.contains(
            "runtime->occlusion_upload_pending_mask != 0u"
        ))
        XCTAssertTrue(source.contains("SDL_UploadToGPUTexture("))
        XCTAssertTrue(source.contains(
            "runtime->occlusion_revisions[index] =\n" +
                "                    runtime->occlusion_pending_revisions[index];"
        ))
    }

    func testPassTelemetryZeroIsAnIdleValue() {
        let telemetry = RadianceCascadesPassTelemetry.zero

        XCTAssertEqual(telemetry.cpuPrepareMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuPipelinePrepareMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuResourcePrepareMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuTraceEncodeMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuMergeUpEncodeMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuMergeDownEncodeMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuReprojectEncodeMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuFinishEncodeMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuBlurEncodeMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuResolveEncodeMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuReconstructEncodeMilliseconds, 0)
        XCTAssertEqual(telemetry.cpuComposeEncodeMilliseconds, 0)
        XCTAssertEqual(telemetry.traceDispatchCount, 0)
        XCTAssertEqual(telemetry.mergeUpDispatchCount, 0)
        XCTAssertEqual(telemetry.mergeDownDispatchCount, 0)
        XCTAssertEqual(telemetry.reprojectDispatchCount, 0)
        XCTAssertEqual(telemetry.finishDispatchCount, 0)
        XCTAssertEqual(telemetry.blurDispatchCount, 0)
        XCTAssertEqual(telemetry.resolveDispatchCount, 0)
        XCTAssertEqual(telemetry.reconstructDispatchCount, 0)
        XCTAssertEqual(telemetry.composeDrawCount, 0)
        XCTAssertFalse(telemetry.temporalHistoryValid)
        XCTAssertEqual(telemetry.temporalPhase, 0)
        XCTAssertEqual(telemetry.freshQuadrantCount, 0)
    }

    func testFallbackDebugBindsCompleteComposeSamplerSet() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_radiance_cascades.c"
            ),
            encoding: .utf8
        )
        let functionStart = try XCTUnwrap(source.range(
            of: "bool btbh_gpu_radiance_cascades_runtime_encode_fallback_debug("
        ))
        let functionEnd = try XCTUnwrap(source.range(
            of: "\nbool btbh_gpu_radiance_cascades_runtime_encode(",
            range: functionStart.upperBound..<source.endIndex
        ))
        let function = source[
            functionStart.lowerBound..<functionEnd.lowerBound
        ]

        XCTAssertTrue(function.contains(
            "SDL_GPUTextureSamplerBinding samplers[8];"
        ))
        XCTAssertTrue(function.contains(
            "SDL_BindGPUFragmentSamplers(pass, 0u, samplers, 8u);"
        ))
    }

    func testHolographicTemporalHistoryIsBoundedAndRejectedByOpacity() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_radiance_cascades.c"
            ),
            encoding: .utf8
        )
        let runtime = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_radiance_cascades_runtime.h"
            ),
            encoding: .utf8
        )
        let reproject = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_hrc_reproject.comp.hlsl"
            ),
            encoding: .utf8
        )
        let finish = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_hrc_finish.comp.hlsl"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(runtime.contains("SDL_GPUTexture *hrc_quadrant_textures[3];"))
        XCTAssertTrue(runtime.contains("bool hrc_history_valid;"))
        XCTAssertTrue(source.contains("runtime->hrc_history_generation != runtime->state.generation"))
        XCTAssertTrue(source.contains("temporal_phase + pair_index * 2u"))
        XCTAssertTrue(source.contains("pair_index == 0u,"))
        XCTAssertTrue(source.contains("history_read_index == 1u ? 2u : 1u"))
        XCTAssertTrue(source.contains("BTBH HRC Temporal Reproject and Reconstruct"))
        XCTAssertTrue(source.contains("runtime->hrc_temporal_phase = temporal_history_valid"))
        XCTAssertTrue(reproject.contains("history_offset_pixels"))
        XCTAssertTrue(reproject.contains(
            "abs(value.a - expected_opacity) > 0.25f"
        ))
        XCTAssertTrue(reproject.contains("(layer & 1u) == fresh_phase"))
        XCTAssertTrue(finish.contains("refresh_rotation == 0xffffffffu"))
    }

    func testDefaultEnvironmentKeepsRadianceCascadesOff() {
        XCTAssertEqual(
            EnvironmentDescriptor.default.radianceCascades,
            .off
        )
        XCTAssertEqual(
            EnvironmentDescriptor.default.radianceCascades.tier,
            .off
        )
        XCTAssertEqual(
            EnvironmentDescriptor.default.radianceCascades.indirectIntensity,
            0.90
        )
        XCTAssertEqual(
            EnvironmentDescriptor.default.radianceCascades.backend,
            .holographic
        )
    }

    func testDescriptorPreservesDedicatedRadianceEmitters() {
        let emitter = RadianceEmitter(
            stableID: 42,
            position: .init(x: 4, y: 3, z: -2),
            color: .init(r: 64, g: 128, b: 255, a: 255),
            energy: 6,
            radius: 1.5,
            height: 3
        )
        let descriptor = RadianceCascadesDescriptor(
            tier: .high,
            emitters: [emitter]
        )

        XCTAssertEqual(descriptor.emitters, [emitter])
        XCTAssertEqual(emitter.direction, .forward)
        XCTAssertEqual(emitter.innerCutoff, -1)
        XCTAssertEqual(emitter.outerCutoff, -2)
        XCTAssertEqual(emitter.lightClass, .world)
        XCTAssertTrue(RadianceCascadesDescriptor.off.emitters.isEmpty)
    }

    func testRendererBudgetsMergedEngineAndGameplayEmitters() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let backend = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "Sources/BTBHRendererGPU/GPUResourceBackend.swift"
            ),
            encoding: .utf8
        )
        let bridge = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "Sources/BTBHRendererGPU/BTBHRendererGPU.swift"
            ),
            encoding: .utf8
        )
        let native = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_radiance_cascades.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(backend.contains(
            "RadianceEmitterBudgetPolicy.production.applying("
        ))
        XCTAssertTrue(backend.contains(
            "to: indirectEmitters + radianceCascades.emitters"
        ))
        XCTAssertTrue(backend.contains(
            "lightClass: handle == localSlot?.key ? .hero : .world"
        ))
        XCTAssertTrue(bridge.contains("nativeEmitter.light_class ="))
        XCTAssertTrue(native.contains(
            "destination->outer_shape[3] = (float)source->light_class;"
        ))
    }

    func testDescriptorPreservesOcclusionFieldPayload() {
        let field = RadianceCascadesOcclusionCascade(
            originWorldXZ: .init(x: -8, y: -8),
            cellSize: 2,
            width: 2,
            height: 2,
            revision: 7,
            samples: [
                -1, 4, 1, 4,
                2, 4, 3, 4,
            ]
        )
        let descriptor = RadianceCascadesDescriptor(
            tier: .high,
            occlusionCascades: [field]
        )

        XCTAssertEqual(descriptor.occlusionCascades, [field])
        XCTAssertTrue(RadianceCascadesDescriptor.off.occlusionCascades.isEmpty)
    }

    func testDirectionalTraceSamplesWorldFieldsWithoutBoundedLightLoop() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let trace = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_hrc_trace.comp.hlsl"
            ),
            encoding: .utf8
        )
        let unlit = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_model_unlit.frag.hlsl"
            ),
            encoding: .utf8
        )
        let sourceVertex = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_rc_source.vert.hlsl"
            ),
            encoding: .utf8
        )
        let sourceFragment = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_rc_source.frag.hlsl"
            ),
            encoding: .utf8
        )
        let directionalSourceVertex = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_rc_directional_source.vert.hlsl"
            ),
            encoding: .utf8
        )
        let directionalSourceFragment = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_rc_directional_source.frag.hlsl"
            ),
            encoding: .utf8
        )
        let directLightVertex = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_rc_direct_light.vert.hlsl"
            ),
            encoding: .utf8
        )
        let directLightFragment = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "shaders/gpu/production_rc_direct_light.frag.hlsl"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(trace.contains("source_field_texture"))
        XCTAssertTrue(trace.contains("directional_source_texture"))
        XCTAssertTrue(trace.contains("directional_source_count = 64u"))
        XCTAssertTrue(trace.contains("occlusion_texture"))
        XCTAssertTrue(trace.contains("direction_index"))
        XCTAssertTrue(trace.contains("environment_radiance"))
        XCTAssertTrue(trace.contains("directional_boundary_radiance"))
        XCTAssertTrue(trace.contains("sky_top_energy"))
        XCTAssertTrue(trace.contains("sky_horizon_curve"))
        XCTAssertFalse(trace.contains("light_source_count"))
        XCTAssertFalse(trace.contains("BTBH_RC_MAX_LIGHT_SOURCES"))
        XCTAssertFalse(trace.contains("for (uint source_index"))
        XCTAssertFalse(trace.contains("emission_texture"))
        XCTAssertFalse(trace.contains("scene_texture"))
        XCTAssertFalse(trace.contains("ambient_texture"))
        XCTAssertFalse(trace.contains("direct_and_emissive"))
        XCTAssertTrue(unlit.contains("output.emission = float4(emission"))
        XCTAssertFalse(sourceVertex.contains("cone_end_half_width"))
        XCTAssertTrue(sourceFragment.contains("if (is_spot)"))
        XCTAssertTrue(sourceFragment.contains(
            "return float4(0.0f, 0.0f, 0.0f, 0.0f);"
        ))
        XCTAssertFalse(sourceFragment.contains("clip(-1.0f)"))
        XCTAssertTrue(directionalSourceVertex.contains("emitter_index"))
        XCTAssertTrue(directionalSourceVertex.contains("direction_index"))
        XCTAssertTrue(directionalSourceVertex.contains("inner_cutoff"))
        XCTAssertTrue(directionalSourceVertex.contains("outer_cutoff"))
        XCTAssertTrue(directionalSourceVertex.contains("-receiver_to_source"))
        XCTAssertTrue(directionalSourceVertex.contains("origin_cell.z * 1.5f"))
        XCTAssertTrue(directionalSourceFragment.contains("radial_coverage"))
        XCTAssertTrue(directionalSourceFragment.contains("angular_coverage"))
        XCTAssertTrue(directLightVertex.contains("radiance_range.w"))
        XCTAssertTrue(directLightVertex.contains("outer_shape.z"))
        XCTAssertTrue(directLightVertex.contains("valid_spot"))
        XCTAssertTrue(directLightFragment.contains("source_visibility"))
        XCTAssertTrue(directLightFragment.contains("occlusion_texture"))
        XCTAssertTrue(directLightFragment.contains("smoothstep"))
        XCTAssertTrue(directLightFragment.contains("near_field"))
        XCTAssertTrue(directLightFragment.contains("field.x <= cell_size"))
        XCTAssertTrue(directLightFragment.contains("subcell_visibility"))
        XCTAssertTrue(directLightFragment.contains(
            "attenuation * visibility"
        ))
        XCTAssertFalse(directLightFragment.contains(
            "clip(visibility - 0.5f)"
        ))
        XCTAssertFalse(directLightFragment.contains("light_source_count"))
    }

    func testDirectionalMergeAndResolvePreserveRadianceTransport() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shaderDirectory = repositoryRoot
            .appendingPathComponent("shaders/gpu", isDirectory: true)
        let compose = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_rc_compose.frag.hlsl"
            ),
            encoding: .utf8
        )
        let holographicResolve = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_hrc_resolve.comp.hlsl"
            ),
            encoding: .utf8
        )
        let holographicReconstruct = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_hrc_reconstruct.comp.hlsl"
            ),
            encoding: .utf8
        )
        let nativeRadiance = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_radiance_cascades.c"
            ),
            encoding: .utf8
        )
        let material = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_model_rc.frag.hlsl"
            ),
            encoding: .utf8
        )
        let unshadowedMaterial = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_model_rc_unshadowed.frag.hlsl"
            ),
            encoding: .utf8
        )
        let twoDimensionalShadowMaterial = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_model_rc_2d_shadows.frag.hlsl"
            ),
            encoding: .utf8
        )
        let sharedMaterial = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let nativeScene = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )
        let materialSource = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_rc_material_source.frag.hlsl"
            ),
            encoding: .utf8
        )
        let materialSourceVertex = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_rc_material_source.vert.hlsl"
            ),
            encoding: .utf8
        )

        for rawDebugReturn in [
            "return float4(display_hdr(cascade.rgb), 1.0f);",
            "return float4(distance.xxx, 1.0f);",
            "return float4(indirect.aaa, 1.0f);",
            "return float4(display_hdr(indirect.rgb), 1.0f);"
        ] {
            XCTAssertTrue(compose.contains(rawDebugReturn))
        }
        XCTAssertTrue(compose.contains("if (debug_view == 8u)"))
        XCTAssertTrue(compose.contains("if (debug_view == 9u)"))
        XCTAssertTrue(compose.contains("if (debug_view == 10u)"))
        XCTAssertTrue(compose.contains("directional_source_texture"))
        XCTAssertTrue(holographicResolve.contains("if (debug_view == 10u)"))
        XCTAssertFalse(compose.contains("indirect.rgb * indirect.a"))
        XCTAssertTrue(holographicResolve.contains("float4(emission, -1.0f)"))
        XCTAssertTrue(holographicReconstruct.contains(
            "if (center_value.a < -0.5f)"
        ))
        XCTAssertTrue(holographicReconstruct.contains(
            "if (source_value.a < -0.5f)"
        ))
        XCTAssertTrue(compose.contains("if (indirect.a < -0.5f)"))
        XCTAssertTrue(compose.contains(
            "return float4(scene_texture.Sample(scene_sampler, uv).rgb, 1.0f);"
        ))
        XCTAssertTrue(holographicResolve.contains("environment_controls.yzw"))
        XCTAssertEqual(
            nativeRadiance.components(
                separatedBy: "resolve_uniforms.environment_controls[1] ="
            ).count - 1,
            1
        )
        XCTAssertTrue(holographicResolve.contains("lerp(luminance.xxx, irradiance, 1.18f)"))
        XCTAssertTrue(holographicResolve.contains("evaluate_brdf"))
        XCTAssertTrue(holographicResolve.contains("evaluate_irradiance_sh"))
        XCTAssertTrue(holographicResolve.contains("sky_visibility"))
        XCTAssertTrue(holographicResolve.contains("environment_diffuse"))
        XCTAssertTrue(holographicResolve.contains("environment_specular"))
        XCTAssertTrue(holographicResolve.contains("direct_light_texture"))
        XCTAssertTrue(holographicResolve.contains("direct_peak * 0.25f"))
        XCTAssertTrue(holographicResolve.contains("direct_diffuse * occlusion"))
        XCTAssertFalse(holographicResolve.contains(
            "direct_diffuse * occlusion * indirect_strength"
        ))
        XCTAssertTrue(holographicResolve.contains(
            "reflected_radiance * occlusion * indirect_strength"
        ))
        XCTAssertFalse(holographicResolve.contains(
            "environment_specular) * indirect_strength"
        ))
        XCTAssertTrue(holographicResolve.contains("fresnel_schlick_roughness"))
        XCTAssertTrue(holographicResolve.contains(
            "reflected_radiance * occlusion"
        ))
        XCTAssertTrue(material.contains("BTBH_RC_MATERIAL_OUTPUT 1"))
        XCTAssertTrue(material.contains("BTBH_DIRECTIONAL_SHADOWS 1"))
        XCTAssertTrue(material.contains("BTBH_OMNI_SHADOWS 1"))
        XCTAssertTrue(unshadowedMaterial.contains("BTBH_DIRECTIONAL_SHADOWS 0"))
        XCTAssertTrue(unshadowedMaterial.contains("BTBH_OMNI_SHADOWS 0"))
        XCTAssertTrue(twoDimensionalShadowMaterial.contains(
            "BTBH_DIRECTIONAL_SHADOWS 1"
        ))
        XCTAssertTrue(twoDimensionalShadowMaterial.contains(
            "BTBH_OMNI_SHADOWS 0"
        ))
        XCTAssertTrue(sharedMaterial.contains("float4 lit_color"))
        XCTAssertTrue(sharedMaterial.contains("output.color = lit_color"))
        XCTAssertFalse(nativeScene.contains(
            "if (radiance_cascades_active) {\n" +
                "        shadow_enabled[0] = 0u;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "production_model_rc_unshadowed.frag.%s"
        ))
        XCTAssertTrue(nativeScene.contains(
            "production_model_rc_2d_shadows.frag.%s"
        ))
        XCTAssertTrue(materialSource.contains("emission_texture.Sample"))
        XCTAssertTrue(materialSource.contains("unlit_enabled"))
        XCTAssertFalse(materialSource.contains("depth_texture"))
        XCTAssertFalse(materialSource.contains("scene_texture"))
        XCTAssertTrue(materialSourceVertex.contains("BTBH_RC_SOURCE_PROJECTION 1"))
        XCTAssertTrue(compose.contains(
            "return float4(max(scene, indirect.rgb), 1.0f)"
        ))
        XCTAssertEqual(
            compose.components(separatedBy: "indirect_strength").count - 1,
            1
        )
        XCTAssertEqual(
            holographicResolve.components(separatedBy: "indirect_strength").count - 1,
            2
        )
    }

    func testHRCMaterialResponseKeepsBRDFAOAndEmissionSeparated() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shaderDirectory = repositoryRoot.appendingPathComponent(
            "shaders/gpu",
            isDirectory: true
        )
        let hrcResolve = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_hrc_resolve.comp.hlsl"
            ),
            encoding: .utf8
        )
        let material = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let contactAO = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_ssao_resolve.frag.hlsl"
            ),
            encoding: .utf8
        )
        let build = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "CMakeLists.txt"
            ),
            encoding: .utf8
        )

        let resolve = hrcResolve
        XCTAssertTrue(resolve.contains("evaluate_brdf("))
        XCTAssertTrue(resolve.contains("(1.0f - metalness) * albedo / pi"))
        XCTAssertTrue(resolve.contains("fresnel_schlick_roughness("))
        XCTAssertTrue(resolve.contains("reflected_radiance * occlusion * indirect_strength"))
        XCTAssertTrue(resolve.contains("environment_diffuse * occlusion"))
        XCTAssertTrue(resolve.contains("const float4 emission_material ="))
        XCTAssertTrue(resolve.contains("const bool unlit_material ="))
        XCTAssertTrue(resolve.contains("if (unlit_material)"))
        XCTAssertTrue(resolve.contains("float4(emission, 1.0f)"))
        XCTAssertFalse(resolve.contains("emission * occlusion"))
        XCTAssertFalse(resolve.contains("emission * indirect_strength"))
        XCTAssertTrue(material.contains(
            "unlit ? 1.0f : 0.0f"
        ))
        XCTAssertTrue(contactAO.contains(
            "preserved_emission + occludable_lighting * contact_visibility"
        ))
        XCTAssertTrue(contactAO.contains("scene - emission"))
        XCTAssertTrue(build.contains(
            "_shader_name STREQUAL \"production_model_rc.frag\""
        ))
    }

    func testRCComposesOpaqueLightingBeforeForwardTransparency() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let scene = try String(
            contentsOf: repositoryRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")

        XCTAssertTrue(scene.contains("BTBH Opaque Draws"))
        XCTAssertTrue(scene.contains("BTBH Forward Transparent Draws"))
        XCTAssertTrue(scene.contains(
            "transparent_target.texture = radiance_cascades_post_input"
        ))
        XCTAssertTrue(scene.contains(
            "transparent_depth_target.load_op = SDL_GPU_LOADOP_LOAD"
        ))
        XCTAssertTrue(scene.contains(
            "!transparent_phase && auxiliary_geometry_outputs"
        ))
        XCTAssertTrue(scene.contains(
            "if (!transparent_phase &&\n            (radiance_cascades_active"
        ))
        XCTAssertTrue(scene.contains(
            "radiance_cascades_post_input = composed_texture"
        ))
    }

    func testDescriptorPreservesAutoTransitionContract() {
        let descriptor = RadianceCascadesDescriptor(
            tier: .auto,
            backend: .holographic,
            debugView: .confidence,
            autoFrameBudgetMilliseconds: 12.5,
            indirectIntensity: 0.90,
            allowAutoTransition: true,
            invalidate: true
        )
        XCTAssertEqual(descriptor.tier, .auto)
        XCTAssertEqual(descriptor.backend, .holographic)
        XCTAssertEqual(descriptor.debugView, .confidence)
        XCTAssertEqual(descriptor.autoFrameBudgetMilliseconds, 12.5)
        XCTAssertEqual(descriptor.indirectIntensity, 0.90)
        XCTAssertTrue(descriptor.allowAutoTransition)
        XCTAssertTrue(descriptor.invalidate)
    }

    func testHolographicShadersKeepPaperPipelineStages() throws {
        let repositoryRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shaderDirectory = repositoryRoot
            .appendingPathComponent("shaders/gpu", isDirectory: true)
        func shader(_ name: String) throws -> String {
            try String(
                contentsOf: shaderDirectory.appendingPathComponent(name),
                encoding: .utf8
            )
        }

        let trace = try shader("production_hrc_trace.comp.hlsl")
        let mergeUp = try shader("production_hrc_merge_up.comp.hlsl")
        let mergeDown = try shader("production_hrc_merge_down.comp.hlsl")
        let reproject = try shader("production_hrc_reproject.comp.hlsl")
        let finish = try shader("production_hrc_finish.comp.hlsl")
        let blur = try shader("production_hrc_blur.comp.hlsl")
        let resolve = try shader("production_hrc_resolve.comp.hlsl")
        let reconstruct = try shader(
            "production_hrc_reconstruct.comp.hlsl"
        )

        XCTAssertTrue(trace.contains("direction_count + 1u"))
        XCTAssertTrue(trace.contains("trace_display_segment"))
        XCTAssertTrue(mergeUp.contains("over_fluence"))
        XCTAssertTrue(mergeUp.contains("previous_direction_count"))
        XCTAssertTrue(mergeDown.contains("angle_size"))
        XCTAssertTrue(mergeDown.contains("next_radiance_valid"))
        XCTAssertTrue(reproject.contains("history_offset_pixels"))
        XCTAssertTrue(reproject.contains("current_opacity"))
        XCTAssertTrue(finish.contains("mixed_parity"))
        XCTAssertTrue(finish.contains("pi * 0.25f"))
        XCTAssertTrue(blur.contains("abs(sample_value.a - center.a)"))
        XCTAssertTrue(resolve.contains("rotation < 4u"))
        XCTAssertTrue(resolve.contains("quadrant_radiance"))
        XCTAssertTrue(resolve.contains("visibility_sum += quadrant.a"))
        XCTAssertTrue(reconstruct.contains("depth_texture"))
        XCTAssertTrue(reconstruct.contains("normal_texture"))
        XCTAssertTrue(reconstruct.contains("center_confidence"))
        XCTAssertTrue(reconstruct.contains("plane_distance"))
        XCTAssertTrue(reconstruct.contains("normal_agreement"))
    }
}
