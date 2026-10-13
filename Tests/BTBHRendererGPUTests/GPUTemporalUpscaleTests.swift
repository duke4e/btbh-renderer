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
}
