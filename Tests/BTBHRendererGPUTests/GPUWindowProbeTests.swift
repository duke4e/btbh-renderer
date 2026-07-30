import XCTest
import BTBHRenderer
import BTBHRendererGPUCore
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUWindowProbeTests: XCTestCase {
    func testProductionGPUColorInputsUseSRGBToLinearConversion() {
        XCTAssertEqual(
            GPUColorSpace.linearComponent(0.5),
            0.214_041_14,
            accuracy: 0.000_001
        )
        XCTAssertEqual(GPUColorSpace.linearComponent(Float(0)), 0)
        XCTAssertEqual(GPUColorSpace.linearComponent(Float(1)), 1)

        let color = GPUColorSpace.linearized(
            .init(r: 128, g: 64, b: 32, a: 127)
        )
        XCTAssertEqual(color.r, 55)
        XCTAssertEqual(color.g, 13)
        XCTAssertEqual(color.b, 4)
        XCTAssertEqual(color.a, 127)
    }

    func testLegacyEnvironmentAmbientPreservesAuthoredComponents() {
        XCTAssertEqual(
            GPUColorSpace.legacyAmbientComponent(205),
            Float(205) / 255,
            accuracy: 0.000_001
        )
        XCTAssertNotEqual(
            GPUColorSpace.legacyAmbientComponent(205),
            GPUColorSpace.linearComponent(UInt8(205))
        )
    }

    func testProductionTextureColorSpaceIsFormatDriven() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let modelImporter = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_model.c"
            ),
            encoding: .utf8
        )
        let textureLoader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_texture.c"
            ),
            encoding: .utf8
        )

        XCTAssertFalse(shader.contains("srgb_to_linear"))
        XCTAssertTrue(modelImporter.contains(
            "SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB"
        ))
        XCTAssertTrue(modelImporter.contains(
            "SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM"
        ))
        XCTAssertTrue(textureLoader.contains(
            "btbh_gpu_window_texture_load_format("
        ))
        XCTAssertTrue(textureLoader.contains(
            "SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,"
        ))
    }

    func testImportedNormalTexturesUseLinearUNORM() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let modelImporter = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_model.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(modelImporter.contains(
            "&source->normal_texture,\n" +
                "                    model_path,\n" +
                "                    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,"
        ))
    }

    func testImportedEmissionTexturesUseSRGB() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let modelImporter = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_model.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(modelImporter.contains(
            "&source->emissive_texture,\n" +
                "                model_path,\n" +
                "                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,"
        ))
    }

    func testProductionPBRUsesLegacyBRDFLookupTexture() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let scene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(shader.contains(
            "Texture2D<float2> brdf_lut_texture"
        ))
        XCTAssertTrue(shader.contains(
            "brdf_lut_texture.Sample("
        ))
        XCTAssertTrue(scene.contains(
            "SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT"
        ))
        XCTAssertTrue(scene.contains(
            "samplers[4].texture = runtime->brdf_lut_texture;"
        ))
    }

    func testImportedModelTexturesUseLegacyTrilinearMipChain() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let modelImporter = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_model.c"
            ),
            encoding: .utf8
        )
        let textureLoader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_texture.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(modelImporter.contains(
            "btbh_gpu_window_texture_create_rgba8_mipmapped("
        ))
        XCTAssertGreaterThanOrEqual(
            modelImporter.components(
                separatedBy: "BTBH_GPU_TEXTURE_FILTER_TRILINEAR"
            ).count - 1,
            3
        )
        XCTAssertTrue(textureLoader.contains(
            "btbh_gpu_window_texture_create_rgba8_mipmapped("
        ))
        XCTAssertTrue(textureLoader.contains(
            "SDL_GenerateMipmapsForGPUTexture(command, resource->texture);"
        ))
    }

    func testTextureLoadDefaultSamplingMatchesLegacyPointFilter() {
        XCTAssertEqual(
            GPUTextureSamplingProjection.filter(
                .init(path: "implicit-filter.png")
            ).rawValue,
            TextureFilter.nearest.rawValue
        )
        XCTAssertEqual(
            GPUTextureSamplingProjection.filter(
                .init(path: "explicit-bilinear.png", filter: .bilinear)
            ).rawValue,
            TextureFilter.bilinear.rawValue
        )
        XCTAssertEqual(
            GPUTextureSamplingProjection.filter(
                .init(path: "explicit-trilinear.png", filter: .trilinear)
            ).rawValue,
            TextureFilter.trilinear.rawValue
        )
    }

    func testCustomMeshTextureCoordinatesPreserveLegacyAuthoredValues() {
        let projected = GPUCustomMeshProjection.textureCoordinate(
            .init(x: 0.25, y: 0.75)
        )

        XCTAssertEqual(projected.x, 0.25, accuracy: 0.000_001)
        XCTAssertEqual(projected.y, 0.75, accuracy: 0.000_001)
    }

    func testCustomMeshTriangleWindingProjectsLegacyOrderToGPUFrontFace() {
        XCTAssertEqual(
            GPUCustomMeshProjection.triangleIndices([
                0, 1, 2,
                2, 1, 3,
            ]),
            [
                0, 2, 1,
                2, 3, 1,
            ]
        )
    }

    func testScenePostProjectsRequestedAntiAliasingQuality() {
        XCTAssertEqual(GPUAntiAliasingProjection.native(.off), 0)
        XCTAssertEqual(GPUAntiAliasingProjection.native(.smaaMedium), 1)
        XCTAssertEqual(GPUAntiAliasingProjection.native(.smaaHigh), 2)
        XCTAssertEqual(GPUAntiAliasingProjection.native(.fxaa), 3)
    }

    func testProductionFXAAUsesOneDedicatedFullscreenPass() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeSource = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_fxaa.frag.hlsl"
            ),
            encoding: .utf8
        )
        let cmake = try String(
            contentsOf: packageRoot.appendingPathComponent("CMakeLists.txt"),
            encoding: .utf8
        )

        XCTAssertTrue(nativeSource.contains(
            "#define BTBH_GPU_SCENE_ANTI_ALIASING_FXAA 3u"
        ))
        XCTAssertTrue(nativeSource.contains(
            "post->anti_aliasing == BTBH_GPU_SCENE_ANTI_ALIASING_FXAA"
        ))
        XCTAssertTrue(nativeSource.contains(
            "runtime->fxaa_pipeline"
        ))
        XCTAssertTrue(shader.contains(
            "float3 fxaa(float2 uv)"
        ))
        XCTAssertTrue(shader.contains(
            "return float4(fxaa(input.texture_coordinate), 1.0f);"
        ))
        XCTAssertTrue(cmake.contains(
            "\"production_fxaa.frag|fragment\""
        ))
    }

    func testProductionFontAtlasUsesLegacyRaylibRasterizationContract() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_font.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(source.contains("BTBH_GPU_FONT_GLYPH_PADDING 4u"))
        XCTAssertTrue(source.contains("stbtt_GetCodepointBitmap("))
        XCTAssertTrue(source.contains(
            "font->ascent = (float)((int)((float)ascent * metric_scale));"
        ))
        XCTAssertTrue(source.contains(
            "BTBH_GPU_TEXTURE_FILTER_NEAREST,\n" +
            "        BTBH_GPU_TEXTURE_WRAP_CLAMP"
        ))
    }

    func testProductionPostDoesNotMultiplyComposedSceneByDepthOnlyAO() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_post.frag.hlsl"
            ),
            encoding: .utf8
        )

        XCTAssertFalse(shader.contains(
            "* ambient_occlusion(uv)"
        ))
        XCTAssertTrue(shader.contains(
            "const float3 post_color = tilt_shift(uv);"
        ))
        XCTAssertTrue(shader.contains(
            "const float3 center = cas_sample(uv);"
        ))
        XCTAssertTrue(shader.contains(
            "const float3 e = grade_lut(scene_sample(uv));"
        ))
    }

    func testProductionSSAOResolvesAmbientBeforePostProcessing() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let modelShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let postShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_post.frag.hlsl"
            ),
            encoding: .utf8
        )
        let resolveShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_ssao_resolve.frag.hlsl"
            ),
            encoding: .utf8
        )
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(modelShader.contains(
            "float4 ambient : SV_Target1;"
        ))
        XCTAssertTrue(modelShader.contains(
            "float4 normal : SV_Target2;"
        ))
        XCTAssertTrue(modelShader.contains(
            "float4 ambient_specular : SV_Target3;"
        ))
        XCTAssertTrue(modelShader.contains(
            "ambient_diffuse_lighting"
        ))
        XCTAssertTrue(modelShader.contains(
            "ambient_specular_lighting"
        ))
        XCTAssertFalse(postShader.contains(
            "Texture2D<float4> ambient_texture"
        ))
        XCTAssertFalse(postShader.contains(
            "Texture2D<float4> normal_texture"
        ))
        XCTAssertTrue(resolveShader.contains(
            "ambient_diffuse * ambient_occlusion"
        ))
        XCTAssertTrue(resolveShader.contains(
            "specular_occlusion("
        ))
        XCTAssertTrue(resolveShader.contains(
            "ambient_specular * specular_ratio"
        ))
        XCTAssertTrue(nativeScene.contains(
            "SDL_GPUColorTargetDescription color_targets[4];"
        ))
        XCTAssertTrue(nativeScene.contains(
            "SDL_GPUTextureSamplerBinding resolve_samplers[6];"
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->ambient_specular_texture"
        ))
        XCTAssertTrue(nativeScene.contains(
            "color_targets[3].blend_state.color_write_mask =\n" +
                "        transparent ? 0u : 0x0fu;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "SDL_GPUTextureSamplerBinding post_samplers[2];"
        ))
        XCTAssertTrue(nativeScene.contains(
            "depth_target.store_op = SDL_GPU_STOREOP_STORE;"
        ))
    }

    func testProductionSSAOUsesLegacyWorldSpaceObscurance() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let resolveShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_ssao_resolve.frag.hlsl"
            ),
            encoding: .utf8
        )
        let denoiseShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_ssao_denoise.frag.hlsl"
            ),
            encoding: .utf8
        )
        let ssaoShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_ssao.frag.hlsl"
            ),
            encoding: .utf8
        )
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(ssaoShader.contains(
            "float4x4 inverse_view_projection;"
        ))
        XCTAssertTrue(ssaoShader.contains(
            "const float world_radius = 1.0f;"
        ))
        XCTAssertTrue(ssaoShader.contains(
            "const float world_bias = 0.03f;"
        ))
        XCTAssertTrue(ssaoShader.contains(
            "const float legacy_intensity = 0.4f;"
        ))
        XCTAssertTrue(ssaoShader.contains(
            "const float radius_falloff = max(radius_squared - vv, 0.0f);"
        ))
        XCTAssertTrue(ssaoShader.contains(
            "max((vn - world_bias) / (0.02f + vv), 0.0f)"
        ))
        XCTAssertFalse(ssaoShader.contains(
            "smoothstep(0.0002f, 0.0045f, depth_delta)"
        ))
        XCTAssertTrue(denoiseShader.contains(
            "const float blur_radius = 4.0f;"
        ))
        XCTAssertTrue(denoiseShader.contains(
            "plane_distance * plane_distance * 100.0f"
        ))
        XCTAssertTrue(denoiseShader.contains(
            "normal_difference * normal_difference * 20.0f"
        ))
        XCTAssertTrue(resolveShader.contains(
            "const float depth_edge_tolerance = 0.05f;"
        ))
        XCTAssertTrue(resolveShader.contains(
            "float depth_aware_upsample("
        ))
        XCTAssertTrue(nativeScene.contains(
            "btbh_gpu_scene_invert_matrix4("
        ))
        XCTAssertTrue(nativeScene.contains(
            "const float projection_scale_y = sqrtf("
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->ssao_texture"
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->ssao_denoised_texture"
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->ssao_denoise_pipeline"
        ))
        XCTAssertTrue(nativeScene.contains(
            "\"production_ssao_denoise.frag.%s\""
        ))
        XCTAssertTrue(nativeScene.contains(
            "(width + 1u) / 2u"
        ))
        XCTAssertTrue(nativeScene.contains(
            "SDL_GPUTextureSamplerBinding resolve_samplers[6];"
        ))
    }

    func testProductionShadowMapsUseValidatedResolutionBudget() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )

        XCTAssertEqual(GPUSceneDirectionalShadowProbe.mapSize, 2_048)
        XCTAssertTrue(nativeScene.contains(
            "#define BTBH_GPU_DIRECTIONAL_SHADOW_MAP_SIZE 2048u"
        ))
        XCTAssertTrue(nativeScene.contains(
            "#define BTBH_GPU_SPOT_SHADOW_MAP_SIZE 2048u"
        ))
        XCTAssertTrue(nativeScene.contains(
            "BTBH_GPU_SPOT_SHADOW_MAP_SIZE"
        ))
        XCTAssertGreaterThanOrEqual(
            shader.components(separatedBy: "2048.0f").count - 1,
            3
        )
    }

    func testProductionShadowPipelinesRespectMaterialCullMode() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(nativeScene.contains(
            "shadow_pipelines[BTBH_GPU_SCENE_CULL_COUNT]"
        ))
        XCTAssertTrue(nativeScene.contains(
            "animated_shadow_pipelines[BTBH_GPU_SCENE_CULL_COUNT]"
        ))
        XCTAssertTrue(nativeScene.contains(
            "btbh_gpu_scene_cull_mode((BTBHGPUSceneCullMode)cull_index)"
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->shadow_pipelines[batch->cull_mode]"
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->animated_shadow_pipelines[batch->cull_mode]"
        ))
    }

    func testProductionAmbientMatchesLegacyFinalAlbedoApplication() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(shader.contains(
            "irradiance * ambient_diffuse"
        ))
        XCTAssertTrue(shader.contains(
            "radiance * single_scattering"
        ))
    }

    func testProductionNormalMappingMatchesLegacyDoubleSidedBackfaces() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let fragmentShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let staticVertexShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.vert.hlsl"
            ),
            encoding: .utf8
        )
        let animatedVertexShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_animated_model.vert.hlsl"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(fragmentShader.contains(
            "bool is_front_face : SV_IsFrontFace;"
        ))
        XCTAssertTrue(fragmentShader.contains(
            "nointerpolation float animated_model : TEXCOORD8;"
        ))
        XCTAssertTrue(fragmentShader.contains(
            "input.animated_model > 0.5f"
        ))
        XCTAssertTrue(fragmentShader.contains(
            "? input.is_front_face"
        ))
        XCTAssertTrue(fragmentShader.contains(
            ": !input.is_front_face"
        ))
        XCTAssertTrue(fragmentShader.contains(
            "normal = -normal;"
        ))
        XCTAssertTrue(staticVertexShader.contains(
            "output.animated_model = 0.0f;"
        ))
        XCTAssertTrue(animatedVertexShader.contains(
            "output.animated_model = 1.0f;"
        ))
    }

    func testProductionNormalMappingPreservesLegacyInterpolatedTBN() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let fragmentShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let staticVertexShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.vert.hlsl"
            ),
            encoding: .utf8
        )
        let animatedVertexShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_animated_model.vert.hlsl"
            ),
            encoding: .utf8
        )

        for shader in [staticVertexShader, animatedVertexShader] {
            XCTAssertTrue(shader.contains(
                "float3 world_bitangent : TEXCOORD6;"
            ))
            XCTAssertTrue(shader.contains(
                "transform_normal(instance_linear, "
            ))
            XCTAssertTrue(shader.contains(
                "cross(world_normal, world_tangent) * tangent_handedness"
            ))
            XCTAssertFalse(shader.contains("orthogonal_tangent"))
        }
        XCTAssertTrue(animatedVertexShader.contains(
            "transform_normal(skin_linear, input.tangent.xyz)"
        ))
        XCTAssertTrue(fragmentShader.contains(
            "float3 world_bitangent : TEXCOORD6;"
        ))
        XCTAssertTrue(fragmentShader.contains(
            "input.world_tangent * mapped_normal.x +"
        ))
        XCTAssertTrue(fragmentShader.contains(
            "input.world_bitangent * mapped_normal.y +"
        ))
        XCTAssertTrue(fragmentShader.contains(
            "mapped_normal.z = sqrt("
        ))
        XCTAssertTrue(fragmentShader.contains(
            "1.0f - saturate(dot(mapped_normal.xy, mapped_normal.xy))"
        ))
        XCTAssertFalse(fragmentShader.contains("orthogonal_tangent"))
    }

    func testProductionNormalTransformsRemainStableAtSmallModelScales() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shaderPaths = [
            "shaders/gpu_spike/production_animated_model.vert.hlsl",
        ]

        for shaderPath in shaderPaths {
            let shader = try String(
                contentsOf: packageRoot.appendingPathComponent(shaderPath),
                encoding: .utf8
            )
            XCTAssertTrue(shader.contains(
                "const float matrix_scale = max("
            ))
            XCTAssertTrue(shader.contains(
                "const float3 scaled_row0 = row0 / matrix_scale;"
            ))
            XCTAssertTrue(shader.contains(
                "dot(cross(scaled_row1, scaled_row2), normal)"
            ))
        }
    }

    func testProductionDirectionalSpecularMatchesLegacyLightDefault() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(shader.contains(
            "legacy_default_light_specular = 0.5f"
        ))
        XCTAssertTrue(shader.contains(
            "geometry * legacy_default_light_specular"
        ))
    }

    func testProductionWaterMatchesLegacyDeferredNormalDecode() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu_spike/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )

        let waterStart = try XCTUnwrap(
            shader.range(of: "else if (surface_variant == 2u)")
        )
        let waterEnd = try XCTUnwrap(
            shader.range(
                of: "else if (surface_variant == 3u",
                range: waterStart.upperBound..<shader.endIndex
            )
        )
        let waterSource = shader[waterStart.lowerBound..<waterEnd.lowerBound]
        XCTAssertTrue(waterSource.contains("normal_map = normalize("))
        XCTAssertTrue(waterSource.contains(") * 2.0f - 1.0f;"))
    }

    func testProductionSurfaceVertexAndShadowPassesShareDeformationContract()
        throws
    {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shaderDirectory = packageRoot.appendingPathComponent(
            "shaders/gpu_spike"
        )
        let vertexShaders = [
            "production_model.vert.hlsl",
            "production_animated_model.vert.hlsl",
            "production_shadow.vert.hlsl",
            "production_animated_shadow.vert.hlsl",
        ]

        for name in vertexShaders {
            let shader = try String(
                contentsOf: shaderDirectory.appendingPathComponent(name),
                encoding: .utf8
            )
            XCTAssertTrue(
                shader.contains("#include \"production_surface.hlsli\""),
                "\(name) must use the shared production surface contract"
            )
            XCTAssertTrue(
                shader.contains("btbh_apply_surface_vertex("),
                "\(name) must apply production surface deformation"
            )
        }

        let shadowFragment = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_shadow.frag.hlsl"
            ),
            encoding: .utf8
        )
        XCTAssertTrue(shadowFragment.contains(
            "btbh_surface_shadow_visible"
        ))
        XCTAssertTrue(shadowFragment.contains("discard;"))
    }

    func testProductionStaticShadowFastPathIsRestrictedToPlainMeshes()
        throws
    {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )
        let shaderDirectory = packageRoot.appendingPathComponent(
            "shaders/gpu_spike"
        )
        let vertexShader = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_shadow_fast.vert.hlsl"
            ),
            encoding: .utf8
        )
        let fragmentShader = try String(
            contentsOf: shaderDirectory.appendingPathComponent(
                "production_shadow_fast.frag.hlsl"
            ),
            encoding: .utf8
        )
        let normalizedNativeScene = nativeScene
            .split(whereSeparator: \.isWhitespace)
            .joined(separator: " ")

        XCTAssertTrue(normalizedNativeScene.contains(
            "batch->surface_variant == BTBH_GPU_SCENE_SURFACE_NONE"
        ))
        XCTAssertTrue(normalizedNativeScene.contains(
            "batch->billboard_mode == BTBH_GPU_SCENE_BILLBOARD_DISABLED"
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->fast_shadow_pipelines["
        ))
        XCTAssertTrue(vertexShader.contains(
            "output.position = mul(light_view_projection, world_position);"
        ))
        XCTAssertFalse(vertexShader.contains("btbh_apply_surface_vertex"))
        XCTAssertFalse(fragmentShader.contains("discard"))
    }

    func testTransparentSceneBatchesUseLegacyMaterialStateOrderAfterOpaqueBatches() {
        func material(
            descriptor: MaterialDescriptor
        ) -> GPUSceneMaterialProbe {
            var result = GPUSceneMaterialProbe(
                albedoTexture: nil,
                albedoColor: .white,
                normalTexture: nil,
                ormTexture: nil,
                normalScale: 1,
                occlusion: 1,
                roughness: 1,
                metalness: 0,
                emissionTexture: nil,
                emissionColor: .white,
                emissionEnergy: 0,
                alphaCutoff: 0.01,
                blendMode: .mix,
                billboardMode: .disabled,
                depthCompare: descriptor.depthCompare,
                stencilCompare: .always,
                stencilReference: 0,
                stencilMask: 0xFF,
                stencilFail: .keep,
                stencilDepthFail: .keep,
                stencilPass: .replace,
                cullMode: descriptor.cullMode,
                transparency: descriptor.transparency,
                unlit: descriptor.unlit,
                surface: .none
            )
            result.legacySortKey = .init(descriptor: descriptor)
            return result
        }
        var opaqueDescriptor = MaterialDescriptor.default
        opaqueDescriptor.albedoTexture = .init(rawValue: 1)
        var textureOneProjectile = opaqueDescriptor
        textureOneProjectile.transparency = .alpha
        textureOneProjectile.unlit = true
        var textureOneShadow = textureOneProjectile
        textureOneShadow.depthCompare = .always
        textureOneShadow.cullMode = .none
        var textureTwoProjectile = textureOneProjectile
        textureTwoProjectile.albedoTexture = .init(rawValue: 2)

        XCTAssertEqual(
            GPULegacyMaterialSortKey(descriptor: textureOneProjectile)
                .depthHash,
            0x8c3bced8
        )
        XCTAssertEqual(
            GPULegacyMaterialSortKey(descriptor: textureOneShadow).depthHash,
            0x5126607a
        )

        let pointer = OpaquePointer(bitPattern: 1)!
        let opaque = GPUSceneGeometryProbeBatch(
            geometry: .customMesh(pointer),
            animation: nil,
            material: material(descriptor: opaqueDescriptor),
            materialSlot: nil,
            firstInstance: 0,
            instanceCount: 1
        )
        let textureTwo = GPUSceneGeometryProbeBatch(
            geometry: .customMesh(pointer),
            animation: nil,
            material: material(descriptor: textureTwoProjectile),
            materialSlot: nil,
            legacyInstanced: true,
            firstInstance: 1,
            instanceCount: 1
        )
        let textureOneSprite = GPUSceneGeometryProbeBatch(
            geometry: .customMesh(pointer),
            animation: nil,
            material: material(descriptor: textureOneProjectile),
            materialSlot: nil,
            legacyInstanced: true,
            firstInstance: 2,
            instanceCount: 1
        )
        let textureOneGroundShadow = GPUSceneGeometryProbeBatch(
            geometry: .customMesh(pointer),
            animation: nil,
            material: material(descriptor: textureOneShadow),
            materialSlot: nil,
            legacyInstanced: true,
            firstInstance: 3,
            instanceCount: 1
        )
        let importedTransparent = GPUSceneGeometryProbeBatch(
            geometry: .model(pointer),
            animation: nil,
            material: material(descriptor: textureOneProjectile),
            materialSlot: 0,
            legacyInstanced: false,
            firstInstance: 4,
            instanceCount: 1
        )
        let ordered = GPUSceneBatchOrdering.opaqueThenLegacyMaterialOrder(
            [
                importedTransparent,
                textureTwo,
                textureOneSprite,
                opaque,
                textureOneGroundShadow,
            ]
        )

        XCTAssertEqual(ordered[0].firstInstance, 0)
        XCTAssertEqual(ordered[1].firstInstance, 3)
        XCTAssertEqual(ordered[2].firstInstance, 2)
        XCTAssertEqual(ordered[3].firstInstance, 1)
        XCTAssertEqual(ordered[4].firstInstance, 4)
    }

    func testAdjacentPersistentInstanceRangesConsolidateWithoutCrossingGaps()
    {
        let geometry = OpaquePointer(bitPattern: 1)!
        let otherGeometry = OpaquePointer(bitPattern: 2)!
        let instanceBuffer = OpaquePointer(bitPattern: 3)!
        func batch(
            geometry pointer: OpaquePointer = geometry,
            firstInstance: Int,
            instanceCount: Int
        ) -> GPUSceneGeometryProbeBatch {
            GPUSceneGeometryProbeBatch(
                geometry: .model(pointer),
                animation: nil,
                material: nil,
                materialSlot: 0,
                legacyInstanced: true,
                instanceBuffer: instanceBuffer,
                firstInstance: firstInstance,
                instanceCount: instanceCount
            )
        }

        let consolidated =
            GPUSceneBatchConsolidation.adjacentPersistentRanges([
                batch(firstInstance: 0, instanceCount: 4),
                batch(firstInstance: 4, instanceCount: 2),
                batch(firstInstance: 8, instanceCount: 1),
                batch(
                    geometry: otherGeometry,
                    firstInstance: 9,
                    instanceCount: 1
                ),
            ])

        XCTAssertEqual(consolidated.count, 3)
        XCTAssertEqual(consolidated[0].firstInstance, 0)
        XCTAssertEqual(consolidated[0].instanceCount, 6)
        XCTAssertEqual(consolidated[1].firstInstance, 8)
        XCTAssertEqual(consolidated[1].instanceCount, 1)
        XCTAssertEqual(consolidated[2].firstInstance, 9)
        XCTAssertEqual(consolidated[2].instanceCount, 1)
    }

    func testSpotShadowCullingUsesOnlyAuthoredClusterBounds() {
        let geometry = OpaquePointer(bitPattern: 1)!
        func batch(
            firstInstance: Int,
            shadowBounds: RenderBounds3?
        ) -> GPUSceneGeometryProbeBatch {
            GPUSceneGeometryProbeBatch(
                geometry: .model(geometry),
                animation: nil,
                material: nil,
                materialSlot: 0,
                firstInstance: firstInstance,
                instanceCount: 1,
                shadowBounds: shadowBounds
            )
        }
        let inside = batch(
            firstInstance: 0,
            shadowBounds: .init(
                minimum: .init(x: -0.5, y: -0.5, z: 0.1),
                maximum: .init(x: 0.5, y: 0.5, z: 0.9)
            )
        )
        let outside = batch(
            firstInstance: 1,
            shadowBounds: .init(
                minimum: .init(x: 2, y: -0.5, z: 0.1),
                maximum: .init(x: 3, y: 0.5, z: 0.9)
            )
        )
        let unbounded = batch(firstInstance: 2, shadowBounds: nil)
        let shadow = GPUSceneDirectionalShadowProbe(
            enabled: true,
            range: 10,
            softness: 0,
            viewProjection: [
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1,
            ]
        )

        let culled = GPUSceneBatchShadowCulling
            .applyingSpotFrustum(
                to: [inside, outside, unbounded],
                shadow: shadow
            )

        XCTAssertTrue(culled[0].spotShadowVisible)
        XCTAssertFalse(culled[1].spotShadowVisible)
        XCTAssertTrue(culled[2].spotShadowVisible)
    }

    func testPersistentRangesWithDifferentSpotVisibilityDoNotMerge() {
        let geometry = OpaquePointer(bitPattern: 1)!
        let instanceBuffer = OpaquePointer(bitPattern: 2)!
        func batch(
            firstInstance: Int,
            visible: Bool
        ) -> GPUSceneGeometryProbeBatch {
            GPUSceneGeometryProbeBatch(
                geometry: .model(geometry),
                animation: nil,
                material: nil,
                materialSlot: 0,
                legacyInstanced: true,
                instanceBuffer: instanceBuffer,
                firstInstance: firstInstance,
                instanceCount: 1,
                spotShadowVisible: visible
            )
        }

        let consolidated =
            GPUSceneBatchConsolidation.adjacentPersistentRanges([
                batch(firstInstance: 0, visible: true),
                batch(firstInstance: 1, visible: false),
            ])

        XCTAssertEqual(consolidated.count, 2)
    }

    func testProductionSpotShadowSkipsCulledClusterBatches() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(nativeScene.contains(
            "shadow_index == BTBH_GPU_SCENE_SHADOW_SPOT"
        ))
        XCTAssertTrue(nativeScene.contains(
            "batch->spot_shadow_visible == 0u"
        ))
    }

    func testProductionSurfaceShadersClassifyKnownVariantsAndParameters()
        throws
    {
        let glitter = try GPUSceneSurfaceProjection.classify(
            fragmentPath: "C:/game/Shaders/glitter_static.glsl",
            parameters: [
                "u_color": .vec3(.init(x: 1, y: 0.5, z: 0.25))
            ]
        )
        XCTAssertEqual(glitter.variant, .glitterStatic)
        XCTAssertEqual(glitter.parameters0.x, 1)
        XCTAssertEqual(glitter.parameters0.y, 0.5)
        XCTAssertEqual(glitter.parameters0.z, 0.25)

        let shatter = try GPUSceneSurfaceProjection.classify(
            fragmentPath: "C:/game/Shaders/shatter.glsl",
            parameters: [
                "u_shatterDuration": .float(2.5),
                "u_cellSize": .float(0.4),
                "u_gravity": .float(8),
                "u_spreadSpeed": .float(1.5),
                "u_spinSpeed": .float(3),
            ]
        )
        XCTAssertEqual(shatter.variant, .shatter)
        XCTAssertEqual(shatter.parameters0.x, 2.5)
        XCTAssertEqual(shatter.parameters0.y, 0.4)
        XCTAssertEqual(shatter.parameters0.z, 8)
        XCTAssertEqual(shatter.parameters0.w, 1.5)
        XCTAssertEqual(shatter.parameters1.x, 3)

        XCTAssertEqual(
            try GPUSceneSurfaceProjection.classify(
                fragmentPath: "water_surface.glsl",
                parameters: [:]
            ).variant,
            .water
        )
        XCTAssertEqual(
            try GPUSceneSurfaceProjection.classify(
                fragmentPath: "dissolve.glsl",
                parameters: [:]
            ).variant,
            .dissolve
        )
        XCTAssertThrowsError(
            try GPUSceneSurfaceProjection.classify(
                fragmentPath: "unknown.glsl",
                parameters: [:]
            )
        )
    }

    func testDirectionalLightingProjectionKeepsShadowContractWithSortedSlot()
        throws
    {
        #if os(Windows)
        let platform = GPUPlatform(request: .vulkan, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH directional shadow projection",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let resources = GPUResourceBackend(platform: platform)
        defer {
            resources.shutdown()
            platform.closeWindow()
        }
        let primary = try resources.createLight(.init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .init(x: -1, y: -1, z: -1),
            color: .white,
            energy: 2,
            range: 16,
            castsShadow: true,
            shadowSoftness: 2
        ))
        _ = try resources.createLight(.init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .down,
            color: .white,
            energy: 0.5,
            range: 50,
            castsShadow: false,
            shadowSoftness: 7
        ))
        let camera = RenderCamera(
            position: .init(x: 0, y: 4, z: -8),
            target: .zero,
            up: .up,
            verticalFOVDegrees: 60,
            projection: .perspective
        )

        var projection = try resources.sceneLighting(
            environment: .default,
            camera: camera,
            aspect: 320 / 180
        )

        XCTAssertTrue(projection.directionalShadow.enabled)
        XCTAssertEqual(projection.directionalShadow.range, 16)
        XCTAssertEqual(projection.directionalShadow.softness, 2)
        XCTAssertEqual(
            projection.directionalShadow.viewProjection.count,
            16
        )
        XCTAssertTrue(
            projection.directionalShadow.viewProjection.allSatisfy(
                \.isFinite
            )
        )
        XCTAssertFalse(projection.secondaryDirectionalShadow.enabled)
        XCTAssertEqual(projection.secondaryDirectionalShadow.range, 50)
        XCTAssertEqual(projection.secondaryDirectionalShadow.softness, 7)

        try resources.updateLight(primary, descriptor: .init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .zero,
            color: .white,
            energy: 2,
            range: 16,
            castsShadow: true,
            shadowSoftness: 2
        ))
        projection = try resources.sceneLighting(
            environment: .default,
            camera: camera,
            aspect: 320 / 180
        )
        XCTAssertFalse(projection.directionalShadow.enabled)
        XCTAssertEqual(
            projection.directionalShadow.viewProjection,
            GPUSceneDirectionalShadowProbe.disabled.viewProjection
        )
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }

    func testSpotLightingProjectionPreservesLegacyDefaultsAndShadowMatrix()
        throws
    {
        #if os(Windows)
        let platform = GPUPlatform(request: .vulkan, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH spot shadow projection",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let resources = GPUResourceBackend(platform: platform)
        defer {
            resources.shutdown()
            platform.closeWindow()
        }
        _ = try resources.createLight(.init(
            kind: .spot,
            active: true,
            position: .init(x: 2, y: 5, z: -3),
            direction: .init(x: 0, y: -1, z: 0.25),
            color: .init(r: 128, g: 192, b: 255, a: 255),
            energy: 3,
            range: 50,
            castsShadow: true,
            shadowSoftness: 4
        ))
        let projection = try resources.sceneLighting(
            environment: .default,
            camera: .init(
                position: .init(x: 0, y: 4, z: -8),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            aspect: 320 / 180
        )

        XCTAssertEqual(projection.spotPosition, .init(x: 2, y: 5, z: -3))
        XCTAssertEqual(
            projection.spotDirection,
            RenderVec3(x: 0, y: -1, z: 0.25).normalized()
        )
        XCTAssertEqual(
            projection.spotColor,
            .init(r: 128, g: 192, b: 255, a: 255)
        )
        XCTAssertEqual(projection.spotEnergy, 3)
        XCTAssertEqual(projection.spotRange, 50)
        XCTAssertEqual(projection.spotFalloff, 1)
        XCTAssertEqual(
            projection.spotInnerCutoff,
            cos(22.5 * .pi / 180),
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            projection.spotOuterCutoff,
            cos(45 * .pi / 180),
            accuracy: 0.000_001
        )
        XCTAssertTrue(projection.spotShadow.enabled)
        XCTAssertEqual(projection.spotShadow.range, 50)
        XCTAssertEqual(projection.spotShadow.softness, 4)
        XCTAssertEqual(projection.spotShadow.viewProjection.count, 16)
        XCTAssertTrue(
            projection.spotShadow.viewProjection.allSatisfy(\.isFinite)
        )
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }

    func testDescriptorAndModelPatchPreservePBRMaterialChannels() throws {
        #if os(Windows)
        let platform = GPUPlatform(request: .vulkan, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH descriptor PBR material channels",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let resources = GPUResourceBackend(platform: platform)
        defer {
            resources.shutdown()
            platform.closeWindow()
        }
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let texture = try resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent(
                    "examples/resources/images/placeholder.png"
                )
                .path
        ))
        let texturePointer = try resources.canvasTexture(texture).pointer
        var descriptor = MaterialDescriptor.default
        descriptor.normalTexture = texture
        descriptor.normalScale = 0.62
        descriptor.ormTexture = texture
        descriptor.occlusion = 0.83
        descriptor.roughness = 0.27
        descriptor.metalness = 0.91
        descriptor.alphaCutoff = 0.37
        descriptor.stencilCompare = .notEqual
        descriptor.stencilReference = 7
        descriptor.stencilMask = 0x3F
        descriptor.stencilFail = .incrementClamp
        descriptor.stencilDepthFail = .decrementClamp
        descriptor.stencilPass = .zero
        descriptor.cullMode = .front
        let standaloneHandle = try resources.createMaterial(descriptor)
        let standalone = try resources.sceneMaterial(standaloneHandle)

        XCTAssertEqual(standalone.normalTexture, texturePointer)
        XCTAssertEqual(standalone.ormTexture, texturePointer)
        XCTAssertEqual(standalone.normalScale, 0.62, accuracy: 0.0001)
        XCTAssertEqual(standalone.occlusion, 0.83, accuracy: 0.0001)
        XCTAssertEqual(standalone.roughness, 0.27, accuracy: 0.0001)
        XCTAssertEqual(standalone.metalness, 0.91, accuracy: 0.0001)
        XCTAssertEqual(standalone.alphaCutoff, 0.37, accuracy: 0.0001)
        XCTAssertEqual(standalone.stencilCompare, .notEqual)
        XCTAssertEqual(standalone.stencilReference, 7)
        XCTAssertEqual(standalone.stencilMask, 0x3F)
        XCTAssertEqual(standalone.stencilFail, .incrementClamp)
        XCTAssertEqual(standalone.stencilDepthFail, .decrementClamp)
        XCTAssertEqual(standalone.stencilPass, .zero)
        XCTAssertEqual(standalone.cullMode, .front)

        let model = try resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/material-factor-triangle.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let patchHandle = try resources.createMaterialOverride(
            from: model,
            materialIndex: 1,
            patch: .init(
                normalTexture: texture,
                ormTexture: texture,
                stencilReference: 9,
                cullMode: CullMode.none
            )
        )
        let patchedEntries = try resources.sceneModelMaterials(
            model: model,
            overrides: [.init(slot: 1, material: patchHandle)]
        )
        XCTAssertEqual(resources.sceneModelMaterialResolutionCount, 1)
        _ = try resources.sceneModelMaterials(
            model: model,
            overrides: [.init(slot: 1, material: patchHandle)]
        )
        XCTAssertEqual(
            resources.sceneModelMaterialResolutionCount,
            1,
            "Identical model material projections should reuse cached results"
        )
        let patched = try XCTUnwrap(patchedEntries.first {
            $0.materialSlot == 1
        }?.material)

        XCTAssertEqual(patched.normalTexture, texturePointer)
        XCTAssertEqual(patched.ormTexture, texturePointer)
        XCTAssertEqual(patched.normalScale, 0.45, accuracy: 0.0001)
        XCTAssertEqual(patched.occlusion, 0.66, accuracy: 0.0001)
        XCTAssertEqual(patched.roughness, 0.31, accuracy: 0.0001)
        XCTAssertEqual(patched.metalness, 0.72, accuracy: 0.0001)
        XCTAssertEqual(patched.stencilReference, 9)
        XCTAssertEqual(patched.cullMode, .none)

        resources.destroyMaterial(patchHandle)
        XCTAssertThrowsError(try resources.sceneModelMaterials(
            model: model,
            overrides: [.init(slot: 1, material: patchHandle)]
        ))
        resources.destroyModel(model)
        resources.destroyMaterial(standaloneHandle)
        resources.destroyTexture(texture)
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }

    func testGLTFImportPreservesPBRMaterialChannels() throws {
        #if os(Windows)
        let probe = try GPUWindowProbe(
            request: .vulkan,
            title: "BTBH imported PBR material channels",
            width: 320,
            height: 180,
            hidden: true,
            debugMode: true
        )
        defer { probe.shutdown() }
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try probe.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/material-factor-triangle.gltf"
                )
                .path
        )
        defer { GPUWindowProbe.destroyModel(model.pointer) }

        let material = try XCTUnwrap(model.materials.indices.contains(1)
            ? model.materials[1]
            : nil)
        XCTAssertNotNil(material.normalTexture)
        XCTAssertNotNil(material.ormTexture)
        XCTAssertEqual(material.normalScale, 0.45, accuracy: 0.0001)
        XCTAssertEqual(material.occlusion, 0.66, accuracy: 0.0001)
        XCTAssertEqual(material.roughness, 0.31, accuracy: 0.0001)
        XCTAssertEqual(material.metalness, 0.72, accuracy: 0.0001)
        XCTAssertEqual(material.alphaCutoff, 0.42, accuracy: 0.0001)
        XCTAssertEqual(material.cullMode, .none)
        XCTAssertEqual(material.transparency, .disabled)

        let transparentMaterial = try XCTUnwrap(
            model.materials.indices.contains(2)
                ? model.materials[2]
                : nil
        )
        XCTAssertEqual(transparentMaterial.cullMode, .back)
        XCTAssertEqual(transparentMaterial.transparency, .alpha)

        let implicitModel = try probe.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/instanced-transformed-default-material.gltf"
                )
                .path
        )
        defer { GPUWindowProbe.destroyModel(implicitModel.pointer) }
        XCTAssertEqual(
            implicitModel.materials[0].metalness,
            0,
            accuracy: 0.0001
        )
        XCTAssertEqual(
            implicitModel.materials[1].roughness,
            1,
            accuracy: 0.0001
        )
        XCTAssertEqual(
            implicitModel.materials[1].metalness,
            1,
            accuracy: 0.0001
        )
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }

    func testAssimpOBJImportPreservesLegacyMaterialFactors() throws {
        #if os(Windows)
        let probe = try GPUWindowProbe(
            request: .vulkan,
            title: "BTBH imported OBJ material factors",
            width: 320,
            height: 180,
            hidden: true,
            debugMode: true
        )
        defer { probe.shutdown() }
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try probe.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/vertical-uv-flip.obj"
                )
                .path
        )
        defer { GPUWindowProbe.destroyModel(model.pointer) }

        let material = try XCTUnwrap(model.materials.first)
        XCTAssertEqual(material.albedoColor.r, 63)
        XCTAssertEqual(material.albedoColor.g, 127)
        XCTAssertEqual(material.albedoColor.b, 191)
        XCTAssertEqual(material.albedoColor.a, 204)
        XCTAssertEqual(material.roughness, 0.4, accuracy: 0.0001)
        XCTAssertEqual(material.metalness, 0, accuracy: 0.0001)
        XCTAssertEqual(material.emissionColor.r, 25)
        XCTAssertEqual(material.emissionColor.g, 51)
        XCTAssertEqual(material.emissionColor.b, 76)
        XCTAssertEqual(material.emissionEnergy, 1, accuracy: 0.0001)
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }

    func testAssimpOBJImportExtractsConcaveFloorFootprint() throws {
        #if os(Windows)
        let probe = try GPUWindowProbe(
            request: .vulkan,
            title: "BTBH imported model footprint",
            width: 320,
            height: 180,
            hidden: true,
            debugMode: true
        )
        defer { probe.shutdown() }
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try probe.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/concave-floor-footprint.obj"
                )
                .path
        )
        defer { GPUWindowProbe.destroyModel(model.pointer) }

        XCTAssertEqual(model.metadata.footprintKind, .raster)
        XCTAssertGreaterThanOrEqual(model.metadata.baseFootprint.count, 6)
        let first = try XCTUnwrap(model.metadata.baseFootprint.first)
        XCTAssertEqual(first.x, 0, accuracy: 0.0001)
        XCTAssertEqual(first.y, 0, accuracy: 0.0001)
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }

    func testFrameTelemetryFinishesPendingFencesAfterMeasuredPath() throws {
        #if os(Windows)
        let probe = try GPUWindowProbe(
            request: .vulkan,
            title: "BTBH non-blocking frame telemetry",
            width: 320,
            height: 180,
            hidden: false,
            debugMode: true
        )
        defer { probe.shutdown() }
        probe.setFrameTelemetryEnabled(true)

        for frame in 0..<3 {
            try probe.presentClear(
                red: UInt8(20 + frame),
                green: 40,
                blue: 60,
                alpha: 255
            )
        }

        var samples: [GPUFrameTelemetrySample] = []
        probe.finishFrameTelemetry(into: &samples)

        XCTAssertEqual(samples.map(\.frameSerial), [1, 2, 3])
        XCTAssertTrue(samples.allSatisfy {
            $0.cpuSubmitMilliseconds >= 0 &&
            $0.fenceCompletionMilliseconds >=
                $0.cpuSubmitMilliseconds
        })
        XCTAssertTrue(probe.pollFrameTelemetry().isEmpty)
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }

    func testCustomMeshCABIRejectsMismatchedLayoutAndIndices() throws {
        #if os(Windows)
        let probe = try GPUWindowProbe(
            request: .vulkan,
            title: "BTBH custom mesh ABI validation",
            width: 320,
            height: 180,
            hidden: true,
            debugMode: true
        )
        defer { probe.shutdown() }
        let packedVertices = [Float](repeating: 0, count: 24 * 3)
        var validIndices: [UInt32] = [0, 1, 2]
        XCTAssertThrowsError(try packedVertices.withUnsafeBytes {
            vertexBytes in
            try validIndices.withUnsafeBufferPointer { indexBuffer in
                try probe.createCustomMesh(
                    vertices: vertexBytes,
                    vertexCount: 4,
                    indices: indexBuffer
                )
            }
        })
        validIndices[2] = 3
        XCTAssertThrowsError(try packedVertices.withUnsafeBytes {
            vertexBytes in
            try validIndices.withUnsafeBufferPointer { indexBuffer in
                try probe.createCustomMesh(
                    vertices: vertexBytes,
                    vertexCount: 3,
                    indices: indexBuffer
                )
            }
        })
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }

    func testSceneDrawsFromPersistentInstanceBufferWithoutTransientUpload() throws {
        #if os(Windows)
        let probe = try GPUWindowProbe(
            request: .vulkan,
            title: "BTBH persistent scene instance buffer",
            width: 320,
            height: 180,
            hidden: false,
            debugMode: true
        )
        defer { probe.shutdown() }
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try probe.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/instanced-transformed-default-material.gltf"
                )
                .path
        )
        defer { GPUWindowProbe.destroyModel(model.pointer) }
        let buffer = try probe.createInstanceBuffer(
            capacity: 2,
            stride: 96
        )
        defer { GPUWindowProbe.destroyInstanceBuffer(buffer.pointer) }
        let records: [Float] = [
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            -0.5, 0, 0, 1,
            1, 0, 0, 1,
            11, 12, 13, 14,
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0.5, 0, 0, 1,
            0, 1, 0, 1,
            21, 22, 23, 24,
        ]
        let shaderDirectory = packageRoot
            .appendingPathComponent(
                "out/build/x64-gpu-debug/generated/btbh_gpu/shaders"
            )
        var testLighting = GPUSceneLightingProbe(
            ambientColor: .white,
            ambientEnergy: 1,
            directionalDirection: .down,
            directionalColor: .white,
            directionalEnergy: 0,
            secondaryDirectionalDirection: .down,
            secondaryDirectionalColor: .white,
            secondaryDirectionalEnergy: 0
        )
        let present = { (cameraPosition: RenderVec3) in
            try probe.presentModelInstances(
                vertexShaderPath: shaderDirectory
                    .appendingPathComponent("production_model.vert.spv")
                    .path,
                animatedVertexShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_animated_model.vert.spv"
                    )
                    .path,
                shadowVertexShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_shadow.vert.spv"
                    )
                    .path,
                animatedShadowVertexShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_animated_shadow.vert.spv"
                    )
                    .path,
                shadowFragmentShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_shadow.frag.spv"
                    )
                    .path,
                fragmentShaderPath: shaderDirectory
                    .appendingPathComponent("production_model.frag.spv")
                    .path,
                unshadowedFragmentShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_model_unshadowed.frag.spv"
                    )
                    .path,
                postVertexShaderPath: shaderDirectory
                    .appendingPathComponent("production_post.vert.spv")
                    .path,
                postFragmentShaderPath: shaderDirectory
                    .appendingPathComponent("production_post.frag.spv")
                    .path,
                shaderFormat: BTBH_GPU_SHADER_FORMAT_SPIRV,
                viewProjection: [
                    1, 0, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 1, 0,
                    0, 0, 0, 1,
                ],
                cameraRight: .right,
                cameraUp: .up,
                cameraPosition: cameraPosition,
                lighting: testLighting,
                post: .default,
                instances: [],
                logicalInstanceCount: 2,
                batches: [
                    .init(
                        geometry: .model(model.pointer),
                        animation: nil,
                        material: model.materials.first,
                        materialSlot: nil,
                        instanceBuffer: buffer.pointer,
                        firstInstance: 0,
                        instanceCount: 2
                    ),
                ],
                clearColor: .black
            )
        }
        records[24..<48].withUnsafeBytes { bytes in
            var uploadReport = BTBHGPUInstanceBufferReport()
            XCTAssertEqual(
                btbh_gpu_instance_buffer_upload(
                    buffer.pointer,
                    96,
                    bytes.baseAddress,
                    UInt32(bytes.count),
                    &uploadReport
                ),
                BTBH_GPU_OK
            )
        }
        let validCameraPosition = RenderVec3(x: 0, y: 0, z: -3)
        XCTAssertThrowsError(try present(validCameraPosition))
        try records[0..<24].withUnsafeBytes {
            try probe.uploadInstanceBuffer(buffer.pointer, bytes: $0)
        }
        XCTAssertThrowsError(try present(validCameraPosition))
        try records.withUnsafeBytes {
            try probe.uploadInstanceBuffer(buffer.pointer, bytes: $0)
        }
        let report = try present(validCameraPosition)

        XCTAssertEqual(report.instanceCount, 2)
        XCTAssertEqual(report.transientInstanceCount, 0)
        XCTAssertEqual(report.persistentBatchCount, 1)
        XCTAssertFalse(report.shadowResourcesReady)
        XCTAssertGreaterThan(report.cpuEncodeMilliseconds, 0)
        XCTAssertGreaterThan(report.cpuPrepareMilliseconds, 0)
        XCTAssertGreaterThanOrEqual(report.cpuUploadMilliseconds, 0)
        XCTAssertEqual(report.cpuShadowEncodeMilliseconds, 0)
        XCTAssertGreaterThanOrEqual(
            report.cpuSwapchainAcquireMilliseconds,
            0
        )
        XCTAssertGreaterThanOrEqual(
            report.cpuTargetEnsureMilliseconds,
            0
        )
        XCTAssertGreaterThan(report.cpuGeometryEncodeMilliseconds, 0)
        XCTAssertGreaterThanOrEqual(report.cpuPostEncodeMilliseconds, 0)
        XCTAssertThrowsError(try present(.init(
            x: .nan,
            y: 0,
            z: -3
        )))
        var invalidCoefficients = Array(
            repeating: RenderVec3.zero,
            count: 9
        )
        invalidCoefficients[0].x = .nan
        testLighting = .init(
            ambientColor: .white,
            ambientEnergy: 1,
            environment: .init(
                irradianceSH: invalidCoefficients,
                sky: .init(color: .white, resolution: 32)
            ),
            directionalDirection: .down,
            directionalColor: .white,
            directionalEnergy: 0,
            secondaryDirectionalDirection: .down,
            secondaryDirectionalColor: .white,
            secondaryDirectionalEnergy: 0
        )
        XCTAssertThrowsError(try present(validCameraPosition))
        try probe.presentCanvas(
            vertexShaderPath: shaderDirectory
                .appendingPathComponent("production_canvas.vert.spv")
                .path,
            fragmentShaderPath: shaderDirectory
                .appendingPathComponent("production_canvas.frag.spv")
                .path,
            texturedFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_canvas_textured.frag.spv"
                )
                .path,
            shaderFormat: BTBH_GPU_SHADER_FORMAT_SPIRV,
            vertices: [],
            batches: [],
            passes: [
                .init(
                    firstBatch: 0,
                    batchCount: 0,
                    target: nil,
                    clearColor: nil,
                    screenClearToken: nil
                ),
            ]
        )
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }

    func testWindowsProbePresentsResizesAndRestoresARealVulkanWindow() throws {
        #if os(Windows)
        let probe = try GPUWindowProbe(
            request: .vulkan,
            title: "BTBH production GPU package probe",
            width: 640,
            height: 360,
            hidden: false,
            debugMode: true
        )
        XCTAssertEqual(probe.deviceDiagnostics.actualDriver, "vulkan")
        XCTAssertTrue(probe.windowDiagnostics.claimed)
        XCTAssertEqual(probe.windowDiagnostics.logicalWidth, 640)
        XCTAssertEqual(probe.windowDiagnostics.logicalHeight, 360)

        try probe.presentClear(red: 18, green: 52, blue: 86, alpha: 255)
        try probe.resize(width: 800, height: 450)
        XCTAssertEqual(probe.windowDiagnostics.logicalWidth, 800)
        XCTAssertEqual(probe.windowDiagnostics.logicalHeight, 450)

        try probe.minimize()
        XCTAssertTrue(probe.windowDiagnostics.minimized)
        try probe.restore()
        XCTAssertFalse(probe.windowDiagnostics.minimized)
        XCTAssertTrue(probe.windowDiagnostics.claimed)
        try probe.presentClear(red: 86, green: 52, blue: 18, alpha: 255)

        probe.shutdown()
        probe.shutdown()
        XCTAssertTrue(probe.isShutdown)
        #else
        throw XCTSkip("The Windows Vulkan window probe runs only on Windows")
        #endif
    }

    func testMaterialSlotBatchesPrepareOnlyDrawnModelPrimitives() throws {
        #if os(Windows)
        let probe = try GPUWindowProbe(
            request: .vulkan,
            title: "BTBH material-slot preparation test",
            width: 320,
            height: 180,
            hidden: false,
            debugMode: true
        )
        defer { probe.shutdown() }
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try probe.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/instanced-transformed-default-material.gltf"
                )
                .path
        )
        defer { GPUWindowProbe.destroyModel(model.pointer) }
        let shaderDirectory = packageRoot
            .appendingPathComponent(
                "out/build/x64-gpu-debug/generated/btbh_gpu/shaders"
            )
        let instances: [Float] = [
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
            1, 1, 1, 1,
            0, 0, 0, 0,
        ]
        let batches = [0, 1].map { slot in
            GPUSceneGeometryProbeBatch(
                geometry: .model(model.pointer),
                animation: nil,
                material: model.materials[slot],
                materialSlot: slot,
                firstInstance: 0,
                instanceCount: 1
            )
        }
        let report = try probe.presentModelInstances(
            vertexShaderPath: shaderDirectory
                .appendingPathComponent("production_model.vert.spv")
                .path,
            animatedVertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_animated_model.vert.spv"
                )
                .path,
            shadowVertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_shadow.vert.spv"
                )
                .path,
            animatedShadowVertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_animated_shadow.vert.spv"
                )
                .path,
            shadowFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_shadow.frag.spv"
                )
                .path,
            fragmentShaderPath: shaderDirectory
                .appendingPathComponent("production_model.frag.spv")
                .path,
            unshadowedFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_model_unshadowed.frag.spv"
                )
                .path,
            postVertexShaderPath: shaderDirectory
                .appendingPathComponent("production_post.vert.spv")
                .path,
            postFragmentShaderPath: shaderDirectory
                .appendingPathComponent("production_post.frag.spv")
                .path,
            shaderFormat: BTBH_GPU_SHADER_FORMAT_SPIRV,
            viewProjection: [
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1,
            ],
            cameraRight: .right,
            cameraUp: .up,
            cameraPosition: .init(x: 0, y: 0, z: -3),
            lighting: .init(
                ambientColor: .white,
                ambientEnergy: 1,
                directionalDirection: .down,
                directionalColor: .white,
                directionalEnergy: 0,
                secondaryDirectionalDirection: .down,
                secondaryDirectionalColor: .white,
                secondaryDirectionalEnergy: 0,
                directionalShadow: .init(
                    enabled: true,
                    range: 10,
                    softness: 1,
                    viewProjection: [
                        1, 0, 0, 0,
                        0, 1, 0, 0,
                        0, 0, 1, 0,
                        0, 0, 0, 1,
                    ]
                )
            ),
            post: .default,
            instances: instances,
            batches: batches,
            clearColor: .black
        )
        XCTAssertEqual(report.primitiveDrawCount, 4)
        XCTAssertEqual(report.preparedGeometryCount, 4)
        XCTAssertTrue(report.shadowResourcesReady)
        XCTAssertGreaterThan(report.cpuEncodeMilliseconds, 0)
        XCTAssertGreaterThan(report.cpuShadowEncodeMilliseconds, 0)
        try probe.presentCanvas(
            vertexShaderPath: shaderDirectory
                .appendingPathComponent("production_canvas.vert.spv")
                .path,
            fragmentShaderPath: shaderDirectory
                .appendingPathComponent("production_canvas.frag.spv")
                .path,
            texturedFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_canvas_textured.frag.spv"
                )
                .path,
            shaderFormat: BTBH_GPU_SHADER_FORMAT_SPIRV,
            vertices: [],
            batches: [],
            passes: [
                .init(
                    firstBatch: 0,
                    batchCount: 0,
                    target: nil,
                    clearColor: nil,
                    screenClearToken: nil
                ),
            ]
        )
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }
}
