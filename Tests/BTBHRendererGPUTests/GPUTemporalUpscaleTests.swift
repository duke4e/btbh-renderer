import BTBHRendererGPUCore
import XCTest
@testable import BTBHRenderer
@testable import BTBHRendererGPU

/// FSR 3.1 temporal upscaling (`UpscaleMode.fsr3`): AMD's upscaler passes driven as the renderer's own
/// SDL GPU compute passes, fed by a jittered projection, motion vectors and a reactive mask.
final class GPUTemporalUpscaleTests: XCTestCase {
    private func read(_ path: String) throws -> String {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        return try String(contentsOf: packageRoot.appendingPathComponent(path), encoding: .utf8)
            .replacingOccurrences(of: "\r\n", with: "\n")
    }

    private static let passes = [
        "prepare_inputs", "luma_pyramid", "shading_change_pyramid", "shading_change", "prepare_reactivity",
        "luma_instability", "accumulate", "debug_view",
    ]

    func testFSR3ProjectsToItsNativeModeAndCarriesTheTemporalInputs() throws {
        XCTAssertEqual(GPUUpscaleModeProjection.native(.fsr3), 3)
        XCTAssertEqual(BTBH_GPU_SCENE_UPSCALE_FSR3.rawValue, 3)
        XCTAssertNotNil(MemoryLayout<BTBHGPUScenePost>.offset(of: \.temporal_instance_motions))
        XCTAssertNotNil(MemoryLayout<BTBHGPUScenePost>.offset(of: \.temporal_reset))
        // Appended after the existing fields, so their offsets hold.
        XCTAssertGreaterThan(
            try XCTUnwrap(MemoryLayout<BTBHGPUScenePost>.offset(of: \.temporal_instance_motions)),
            try XCTUnwrap(MemoryLayout<BTBHGPUScenePost>.offset(of: \.fog_edge_enabled))
        )
        let frame = FrameDescriptor(
            camera: .init(position: .zero, target: .init(x: 0, y: 0, z: 1), up: .init(x: 0, y: 1, z: 0),
                verticalFOVDegrees: 45, projection: .perspective),
            environment: .default,
            upscaleMode: .fsr3
        )
        XCTAssertFalse(frame.resetsTemporalHistory)
        XCTAssertEqual(frame.frameSeconds, 1.0 / 60.0)
    }

    func testEveryPassIsBuiltStagedAndBoundAsItsShaderDeclares() throws {
        let cmake = try read("CMakeLists.txt")
        let fsr3 = try read("src/gpu/btbh_gpu_fsr3.c")
        for pass in Self.passes + ["clear", "motion_view"] {
            XCTAssertTrue(cmake.contains("\"production_fsr3_\(pass).comp|compute\""), pass)
        }
        for pass in Self.passes {
            let shader = try read("shaders/gpu/production_fsr3_\(pass).comp.hlsl")
            let samplers = shader.components(separatedBy: "#define FSR3UPSCALER_BIND_SRV_").count - 1
            let storage = shader.components(separatedBy: "#define FSR3UPSCALER_BIND_UAV_").count - 1
            let uniforms = shader.components(separatedBy: "#define FSR3UPSCALER_BIND_CB_").count - 1
            XCTAssertTrue(
                fsr3.contains("{\"production_fsr3_\(pass).comp\", \(samplers)u, \(storage)u, \(uniforms)u,"),
                "\(pass): \(samplers) textures, \(storage) read-write textures, \(uniforms) uniform buffers"
            )
            XCTAssertTrue(shader.contains("#include \"fsr3/btbh_fsr3_options.hlsli\""), pass)
        }
    }

    func testTheVendoredShadersRecordTheirSourceAndLicence() throws {
        let readme = try read("shaders/gpu/fsr3/README.md")
        let licence = try read("shaders/gpu/fsr3/LICENSE.md")
        let callbacks = try read("shaders/gpu/fsr3/btbh_fsr3_callbacks_hlsl.h")
        let options = try read("shaders/gpu/fsr3/btbh_fsr3_options.hlsli")
        XCTAssertTrue(readme.contains("v2.0.0"))
        XCTAssertTrue(readme.contains("3.1.5"))
        XCTAssertTrue(licence.contains("Permission is hereby granted, free of charge"))
        // SDL GPU's compute register spaces, and no shared samplers left.
        XCTAssertTrue(callbacks.contains("register(BTBH_FSR3_T(index), space0)"))
        XCTAssertTrue(callbacks.contains("register(BTBH_FSR3_U(index), space1)"))
        XCTAssertTrue(callbacks.contains("register(BTBH_FSR3_B(index), space2)"))
        XCTAssertFalse(callbacks.contains("s_LinearClamp"))
        XCTAssertTrue(options.contains("#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 0"))
        XCTAssertTrue(options.contains("#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0"))
        XCTAssertTrue(options.contains("#define FFX_SPD_NO_WAVE_OPERATIONS 1"))
    }

    func testTheSceneJittersWritesMotionAndReplacesAntiAliasing() throws {
        let scene = try read("src/gpu/btbh_gpu_scene.c")
        let vertex = try read("shaders/gpu/production_model.vert.hlsl")
        let fragment = try read("shaders/gpu/production_model.frag.hlsl")
        let temporal = try read("shaders/gpu/production_temporal.hlsli")
        // AMD's jitter convention: render pixels, y down, clip space (2 x / w, -2 y / h).
        XCTAssertTrue(scene.contains("temporal_controls[0] = 2.0f * jitter_x / (float)render_width;"))
        XCTAssertTrue(scene.contains("temporal_controls[1] = -2.0f * jitter_y / (float)render_height;"))
        XCTAssertTrue(scene.contains("const uint8_t anti_aliasing = temporal_active ? 0u : post->anti_aliasing;"))
        XCTAssertTrue(scene.contains("color_targets[5].format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;"))
        XCTAssertTrue(scene.contains("color_targets[1].blend_state.color_blend_op = SDL_GPU_BLENDOP_MAX;"))
        XCTAssertTrue(scene.contains("? (transparent ? 2u : 6u)"))
        // FSR 3 runs on the finished frame, RCAS sharpens it, Mirror Curse mirrors it last.
        let encode = try XCTUnwrap(scene.range(of: "btbh_gpu_fsr3_encode(command, &runtime->fsr3, &fsr3_inputs)"))
        let rcas = try XCTUnwrap(scene.range(of: "\"BTBH FSR3 RCAS\""))
        XCTAssertLessThan(encode.lowerBound, rcas.lowerBound)
        // Motion vectors: previous minus current UV, from the unjittered clip positions.
        XCTAssertTrue(temporal.contains("return (previous - current) * float2(0.5f, -0.5f);"))
        XCTAssertTrue(temporal.contains("jittered_clip.xy - temporal_controls.xy * jittered_clip.w"))
        XCTAssertTrue(vertex.contains("StructuredBuffer<float4> instance_motion : register(t1, space0);"))
        XCTAssertTrue(fragment.contains("float4 velocity : SV_Target5;"))
        XCTAssertTrue(fragment.contains("btbh_temporal_reactive(output.color, procedural_controls.z)"))
    }

    func testTransformStreamsCarryTheirMotion() {
        let model = ModelHandle(rawValue: 1)
        let draw = ModelTransformStreamsDraw(
            model: model,
            positions: [.zero, .one],
            rotations: [.identity, .identity],
            scales: [.one, .one],
            count: 2,
            motions: [.init(x: 1, y: 0, z: 0), .zero]
        )
        XCTAssertEqual(draw.motions?.first, RenderVec3(x: 1, y: 0, z: 0))
        XCTAssertNil(ModelTransformStreamsDraw(
            model: model, positions: [.zero], rotations: [.identity], scales: [.one], count: 1
        ).motions)
    }

    func testFSR3PicksTextureDetailForTheOutputSize() throws {
        let scene = try read("src/gpu/btbh_gpu_scene.c")
        let fragment = try read("shaders/gpu/production_model.frag.hlsl")
        // AMD's guide, log2(render width / display width) - 1, and only while FSR 3 runs: FSR 1 and
        // native frames keep a bias of 0.
        XCTAssertTrue(scene.contains(": log2f((float)render_width / (float)temporal_window_width) - 1.0f;"))
        XCTAssertTrue(scene.contains("float temporal_lod_bias = 0.0f;\n    if (temporal_active) {"))
        XCTAssertTrue(scene.contains("fragment_uniforms.procedural_controls[3] =\n                temporal_lod_bias;"))
        XCTAssertTrue(scene.contains("SDL_getenv(\"BTBH_FSR3_MIP_BIAS\")"))
        // Every material texture samples with it, and the surface detail's footprint follows it.
        for texture in ["albedo", "emission", "normal", "orm"] {
            XCTAssertTrue(fragment.contains(
                "\(texture)_texture.SampleBias(\n        \(texture)_sampler,\n        input.texture_coordinate,\n" +
                    "        lod_bias\n    )"
            ), texture)
            XCTAssertFalse(fragment.contains("\(texture)_texture.Sample("), texture)
        }
        XCTAssertTrue(fragment.contains("const float lod_bias = procedural_controls.w;"))
        XCTAssertTrue(fragment.contains("* exp2(procedural_controls.w);"))
    }

    @preconcurrency @MainActor
    func testVertexAnimatedSurfacesCarryTheirMotion() throws {
        let draw = ModelTransformStreamsDraw(
            model: ModelHandle(rawValue: 1),
            positions: [.zero],
            rotations: [.identity],
            scales: [.one],
            customs: [.init(x: 0.2, y: 0, z: 0.5, w: 0)],
            count: 1,
            surfaceMotions: [0.25]
        )
        XCTAssertEqual(draw.surfaceMotions, [0.25])
        XCTAssertNil(draw.motions)
        let animated = AnimatedModelTransformStreamsDraw(
            model: ModelHandle(rawValue: 1), player: nil, tree: AnimationTreeHandle(rawValue: 1),
            positions: [.zero], rotations: [.identity], scales: [.one], count: 1, surfaceMotions: [0.5]
        )
        XCTAssertEqual(animated.surfaceMotions, [0.5])
        // The motion record: translation in xyz, the surface's time step in w.
        XCTAssertEqual(
            GPUSceneBackend.packedMotion(.init(x: 1, y: 2, z: 3), surfaceSeconds: 0.25),
            SIMD4<Float>(1, 2, 3, 0.25)
        )
        XCTAssertEqual(GPUSceneBackend.packedMotion(.zero, surfaceSeconds: .nan), SIMD4<Float>(0, 0, 0, 0))
        // The vertex shaders run the dissolve (time in custom x) or shatter (custom y) once more at the
        // previous frame's time for the previous position.
        let surface = try read("shaders/gpu/production_surface.hlsli")
        XCTAssertTrue(surface.contains("if (surface_variant == 3u && instance_custom.x > 0.0f)"))
        XCTAssertTrue(surface.contains("else if (surface_variant == 4u && instance_custom.y > 0.0f)"))
        XCTAssertTrue(surface.contains(
            "float4 btbh_surface_previous_custom(float4 instance_custom, uint surface_variant, float seconds)"
        ))
        XCTAssertTrue(surface.contains("if (surface_variant == 3u)\n    {\n        previous.x -= seconds;"))
        XCTAssertTrue(surface.contains("else if (surface_variant == 4u)\n    {\n        previous.y -= seconds;"))
        for shader in ["production_model.vert.hlsl", "production_animated_model.vert.hlsl"] {
            let vertex = try read("shaders/gpu/\(shader)")
            XCTAssertTrue(
                vertex.contains("btbh_surface_previous_custom(instance.custom, surface_variant, motion.w)"),
                shader
            )
        }
        XCTAssertTrue(try read("shaders/gpu/production_model.vert.hlsl").contains(
            "previous_world_position -= motion.xyz;"
        ))
    }

    func testSkinnedLimbsMoveWithThePreviousFramesBonePalette() throws {
        let scene = try read("src/gpu/btbh_gpu_scene.c")
        let animated = try read("shaders/gpu/production_animated_model.vert.hlsl")
        let temporal = try read("shaders/gpu/production_temporal.hlsli")
        // The C vertex uniforms and the shaders' cbuffer end alike.
        XCTAssertTrue(scene.contains(
            "    uint32_t previous_bone_offset;\n    uint32_t temporal_reserved[3];\n} BTBHGPUSceneUniforms;"
        ))
        XCTAssertTrue(temporal.contains(
            "    float4 temporal_controls; \\\n    uint previous_bone_offset; \\\n    uint3 temporal_reserved;"
        ))
        // The previous palettes sit in the bone buffer after this frame's, one per pose and skin, and
        // only a temporal frame that continues the previous one reads them.
        XCTAssertTrue(scene.contains("temporal_active ? runtime : NULL"))
        XCTAssertTrue(scene.contains(
            "if (!temporal_active ||\n        post->temporal_reset != 0u ||\n        !runtime->temporal_previous_valid) {\n" +
                "        runtime->temporal_palette_count = 0u;"
        ))
        XCTAssertTrue(scene.contains("bone_offsets[prepared_geometry_count + geometry_cursor];"))
        XCTAssertTrue(scene.contains("uniforms.previous_bone_offset =\n                animated ? previous_bone_offset : 0u;"))
        XCTAssertTrue(scene.contains("entry->pose != sample->pose ||"))
        // The vertex skinned with the previous pose, moved back by the instance's translation.
        XCTAssertTrue(animated.contains("bones[previous_bone_offset + joint_indices.x] * input.weights.x +"))
        XCTAssertTrue(animated.contains("mul(previous_skin_matrix, float4(previous_local_position, 1.0f))"))
        XCTAssertTrue(animated.contains(").xyz - translation;"))
    }
}
