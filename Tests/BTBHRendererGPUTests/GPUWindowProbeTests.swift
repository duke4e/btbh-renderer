import XCTest
import BTBHRenderer
import BTBHRendererGPUCore
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUWindowProbeTests: XCTestCase {
    private var platformShaderExtension: String {
        switch GPUBackendRequest.platformDefault {
        case .vulkan: "spv"
        case .metal: "metallib"
        }
    }

    private var platformShaderFormat: BTBHGPUShaderFormat {
        switch GPUBackendRequest.platformDefault {
        case .vulkan: BTBH_GPU_SHADER_FORMAT_SPIRV
        case .metal: BTBH_GPU_SHADER_FORMAT_METALLIB
        }
    }

    private var platformShaderFallbackDirectory: String {
        switch GPUBackendRequest.platformDefault {
        case .vulkan:
            "out/build/x64-gpu-debug/generated/btbh_gpu/shaders"
        case .metal:
            "out/build/macos-gpu-debug/generated/btbh_gpu/shaders"
        }
    }

    func testRadianceCascadesPipelineCacheSeparatesAttachmentLayouts()
        throws
    {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")

        XCTAssertTrue(source.contains(
            "#define BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT 8u"
        ))
        XCTAssertTrue(source.contains(
            "(temporal ? 4u : 0u) +\n" +
                "        (radiance_cascades_active ? 2u : 0u) +\n" +
                "        (auxiliary_geometry_outputs ? 1u : 0u)"
        ))
    }

    func testNativeCanvasProjectsScreenCommandsInLogicalWindowSpace()
        throws
    {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_canvas.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")

        XCTAssertTrue(source.contains("SDL_GetWindowSize("))
        XCTAssertTrue(source.contains(
            "is_screen ? logical_width : target_width"
        ))
        XCTAssertTrue(source.contains(
            "btbh_gpu_canvas_scale_screen_scissor("
        ))
    }

    func testCanvasBatchCoalescingRequiresExactAdjacentPassState() {
        let texture = OpaquePointer(bitPattern: 0x100)!
        let otherTexture = OpaquePointer(bitPattern: 0x200)!
        let scissor = RenderRect(x: 1, y: 2, width: 30, height: 40)
        let first = GPUCanvasBatchProbe(
            firstVertex: 0,
            vertexCount: 6,
            blendMode: .alpha,
            scissor: scissor,
            texture: texture
        )
        let adjacent = GPUCanvasBatchProbe(
            firstVertex: 6,
            vertexCount: 9,
            blendMode: .alpha,
            scissor: scissor,
            texture: texture
        )

        let merged = first.merged(
            with: adjacent,
            withinSamePass: true
        )
        XCTAssertEqual(merged?.firstVertex, 0)
        XCTAssertEqual(merged?.vertexCount, 15)
        XCTAssertNil(first.merged(
            with: adjacent,
            withinSamePass: false
        ))
        XCTAssertNil(first.merged(
            with: .init(
                firstVertex: 7,
                vertexCount: 9,
                blendMode: .alpha,
                scissor: scissor,
                texture: texture
            ),
            withinSamePass: true
        ))
        XCTAssertNil(first.merged(
            with: .init(
                firstVertex: 6,
                vertexCount: 9,
                blendMode: .additive,
                scissor: scissor,
                texture: texture
            ),
            withinSamePass: true
        ))
        XCTAssertNil(first.merged(
            with: .init(
                firstVertex: 6,
                vertexCount: 9,
                blendMode: .alpha,
                scissor: nil,
                texture: texture
            ),
            withinSamePass: true
        ))
        XCTAssertNil(first.merged(
            with: .init(
                firstVertex: 6,
                vertexCount: 9,
                blendMode: .alpha,
                scissor: scissor,
                texture: otherTexture
            ),
            withinSamePass: true
        ))
    }

    func testNativeSceneAndCanvasShareSingleNormalSubmitLifecycle()
        throws
    {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let sceneSource = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let canvasSource = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_canvas.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let windowSource = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_window.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let sceneAdapterSource = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "Sources/BTBHRendererGPU/BTBHRendererGPU.swift"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let sceneSubmission = try XCTUnwrap(
            sceneSource.range(
                of: "btbh_gpu_window_scene_present_instances_internal("
            ).flatMap { start in
                sceneSource.range(
                    of: "\nBTBHGPUTexture *btbh_gpu_window_capture_pending_scene_texture(",
                    range: start.lowerBound..<sceneSource.endIndex
                ).map {
                    String(sceneSource[start.lowerBound..<$0.lowerBound])
                }
            }
        )
        let sceneCapture = try XCTUnwrap(
            sceneSource.range(
                of: "btbh_gpu_window_capture_pending_scene_texture("
            ).flatMap { start in
                sceneSource.range(
                    of: "\nbool btbh_gpu_window_blit_pending_scene(",
                    range: start.lowerBound..<sceneSource.endIndex
                ).map {
                    String(sceneSource[start.lowerBound..<$0.lowerBound])
                }
            }
        )
        let publicSceneSubmission = try XCTUnwrap(
            sceneSource.range(
                of: "\nBTBHGPUResult btbh_gpu_window_scene_present_instances("
            ).flatMap { start in
                sceneSource.range(
                    of: "\nBTBHGPUTexture *btbh_gpu_window_capture_pending_scene_texture(",
                    range: start.lowerBound..<sceneSource.endIndex
                ).map {
                    String(sceneSource[start.lowerBound..<$0.lowerBound])
                }
            }
        )
        let canvasPresentation = try XCTUnwrap(
            canvasSource.range(
                of: "btbh_gpu_window_canvas_present("
            ).flatMap { start in
                canvasSource.range(
                    of: "\nvoid btbh_gpu_window_release_canvas(",
                    range: start.lowerBound..<canvasSource.endIndex
                ).map {
                    String(canvasSource[start.lowerBound..<$0.lowerBound])
                }
            }
        )
        let clearPresentation = try XCTUnwrap(
            windowSource.range(
                of: "btbh_gpu_window_present_clear("
            ).flatMap { start in
                windowSource.range(
                    of: "\nstatic void btbh_gpu_platform_snapshot(",
                    range: start.lowerBound..<windowSource.endIndex
                ).map {
                    String(windowSource[start.lowerBound..<$0.lowerBound])
                }
            }
        )

        XCTAssertFalse(
            sceneSubmission.contains(
                "SDL_WaitAndAcquireGPUSwapchainTexture"
            ),
            "Scene submission must not wait for presentation availability"
        )
        XCTAssertTrue(
            sceneSubmission.contains("pending_scene_ready = true"),
            "A recorded offscreen scene must be handed to Canvas2D"
        )
        XCTAssertTrue(
            sceneSubmission.contains("window->pending_command = command"),
            "Scene submission must retain its command for Canvas2D"
        )
        XCTAssertFalse(
            sceneSubmission.contains("SDL_SubmitGPUCommandBuffer(command)"),
            "Normal scene submission must not submit before Canvas2D"
        )
        XCTAssertTrue(
            canvasPresentation.contains(
                "SDL_WaitAndAcquireGPUSwapchainTexture"
            ),
            "Canvas owns late swapchain acquisition"
        )
        XCTAssertTrue(
            canvasPresentation.contains(
                "if (window->pending_command == NULL)"
            ),
            "Canvas must reuse a pending scene command when available"
        )
        XCTAssertTrue(
            canvasPresentation.contains(
                "btbh_gpu_window_blit_pending_scene"
            ),
            "Canvas must composite the pending offscreen scene"
        )
        XCTAssertTrue(
            clearPresentation.contains(
                "btbh_gpu_window_blit_pending_scene"
            ),
            "A scene with no 2D batches must still reach the swapchain"
        )
        XCTAssertTrue(
            sceneCapture.contains(
                "SDL_SubmitGPUCommandBuffer(scene_command)"
            ),
            "Explicit capture must flush the pending scene rare path"
        )
        XCTAssertTrue(
            sceneCapture.contains("window->pending_command = NULL"),
            "Capture must hand Canvas a clean command-buffer slot"
        )
        let normalizedPublicSceneSubmission = publicSceneSubmission
            .split(whereSeparator: \.isWhitespace)
            .joined(separator: " ")
        XCTAssertTrue(
            normalizedPublicSceneSubmission.contains(
                "batches, NULL, NULL, batch_count"
            ),
            "The stable public C entry point must preserve legacy always-visible scene and directional-shadow behavior"
        )
        XCTAssertTrue(
            sceneSubmission.contains("const uint8_t *scene_visibility") &&
                sceneSubmission.contains(
                    "const uint8_t *directional_shadow_visibility"
                ),
            "New visibility controls must remain package-internal sidecars"
        )
        XCTAssertTrue(
            sceneAdapterSource.contains(
                "btbh_gpu_window_scene_present_instances_with_visibility("
            ),
            "The Swift adapter must use the package-internal visibility entry point"
        )
        let earlyCanvasSetup = try XCTUnwrap(
            canvasPresentation.range(of: "SDL_GPUDevice *device")
        ).lowerBound
        let earlyCanvasFailures = String(
            canvasPresentation[..<earlyCanvasSetup]
        )
        XCTAssertGreaterThanOrEqual(
            earlyCanvasFailures.components(
                separatedBy: "btbh_gpu_window_finish_pending_frame"
            ).count - 1,
            2,
            "Invalid input and runtime creation failures must both abandon a pending scene command"
        )
        XCTAssertTrue(
            canvasSource.contains(
                "btbh_gpu_window_scene_submission_finished(\n" +
                "        window,\n" +
                "        must_submit && result\n" +
                "    )"
            ),
            "Final command cancellation or submission failure must invalidate speculative shadow-cache state"
        )
        XCTAssertTrue(
            sceneSubmission.contains(
                "btbh_gpu_scene_cancel_submission(window, command)"
            ),
            "Scene encode failures must invalidate speculative shadow-cache state when cancelling"
        )
        XCTAssertFalse(
            sceneSubmission.contains(
                "(void)SDL_CancelGPUCommandBuffer(command)"
            ),
            "Scene submission must not bypass shadow-cache invalidation when cancelling"
        )
        XCTAssertTrue(
            sceneCapture.contains(
                "btbh_gpu_window_scene_submission_finished(\n" +
                "                window,\n" +
                "                false\n" +
                "            )"
            ),
            "A failed explicit scene submit must invalidate speculative shadow-cache state"
        )
    }

    func testSceneAdapterLightingTelemetryBracketsLightingResolution()
        throws
    {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "Sources/BTBHRendererGPU/GPUSceneBackend.swift"
            ),
            encoding: .utf8
        )
        let endFrame = try XCTUnwrap(
            source.range(of: "private func endFrameImpl() throws {")
                .flatMap { start in
                    source.range(
                        of: "\n    private func appendModelBatches(",
                        range: start.lowerBound..<source.endIndex
                    ).map {
                        String(source[start.lowerBound..<$0.lowerBound])
                    }
                }
        )
        let started = try XCTUnwrap(
            endFrame.range(of: "let lightingResolveStarted")
        )
        let resolution = try XCTUnwrap(
            endFrame.range(of: "let lighting = try resources.sceneLighting(")
        )
        let finished = try XCTUnwrap(
            endFrame.range(of: "let lightingResolveFinished")
        )

        XCTAssertLessThan(started.lowerBound, resolution.lowerBound)
        XCTAssertLessThan(resolution.lowerBound, finished.lowerBound)
    }

    func testNativeCanvasCachesPipelineAndScissorWithinEachPass()
        throws
    {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_canvas.c"
            ),
            encoding: .utf8
        )
        let presentation = try XCTUnwrap(
            source.range(of: "btbh_gpu_window_canvas_present(")
                .flatMap { start in
                    source.range(
                        of: "\nvoid btbh_gpu_window_release_canvas(",
                        range: start.lowerBound..<source.endIndex
                    ).map {
                        String(source[start.lowerBound..<$0.lowerBound])
                    }
                }
        )

        XCTAssertTrue(presentation.contains(
            "SDL_GPUGraphicsPipeline *bound_pipeline = NULL"
        ))
        XCTAssertTrue(presentation.contains(
            "if (pipeline != bound_pipeline)"
        ))
        XCTAssertTrue(presentation.contains(
            "bool has_bound_scissor = false"
        ))
        XCTAssertTrue(presentation.contains(
            "if (!has_bound_scissor ||"
        ))
        XCTAssertTrue(presentation.contains(
            "SDL_BindGPUFragmentSamplers("
        ))
    }

    func testProductionScreenShaderKindIsDerivedOnceFromFragmentPath() {
        XCTAssertEqual(
            GPUProductionScreenShaderKind(
                fragmentPath: #"C:\shaders\LUT.frag"#
            ),
            .lut
        )
        XCTAssertEqual(
            GPUProductionScreenShaderKind(
                fragmentPath: "/tmp/shaders/cas.frag"
            ),
            .cas
        )
        XCTAssertEqual(
            GPUProductionScreenShaderKind(
                fragmentPath: "/tmp/shaders/TiltShift.frag"
            ),
            .tiltShift
        )
        XCTAssertEqual(
            GPUProductionScreenShaderKind(
                fragmentPath: "/tmp/shaders/custom-pass.frag"
            ),
            .unsupported("custom-pass")
        )
    }

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
                "shaders/gpu/production_model.frag.hlsl"
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
        ).replacingOccurrences(of: "\r\n", with: "\n")

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
        ).replacingOccurrences(of: "\r\n", with: "\n")

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
                "shaders/gpu/production_model.frag.hlsl"
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

    func testProductionFXAAMatchesLegacyMediumQualityPreset() throws {
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
                "shaders/gpu/production_fxaa.frag.hlsl"
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
            "anti_aliasing == BTBH_GPU_SCENE_ANTI_ALIASING_FXAA"
        ))
        XCTAssertTrue(nativeSource.contains(
            "runtime->fxaa_pipeline"
        ))
        XCTAssertTrue(shader.contains(
            "float4 fxaa_quality_15(float2 uv)"
        ))
        XCTAssertTrue(shader.contains(
            "const float luma_center = center.g;"
        ))
        XCTAssertTrue(shader.contains(
            "max(1.0f / 16.0f, range_maximum * (1.0f / 8.0f))"
        ))
        XCTAssertTrue(shader.contains(
            "negative_position -= edge_offset * 1.5f;"
        ))
        XCTAssertTrue(shader.contains(
            "negative_position -= edge_offset * 4.0f;"
        ))
        XCTAssertTrue(shader.contains(
            "negative_position -= edge_offset * 12.0f;"
        ))
        XCTAssertTrue(shader.contains(
            "const float subpixel_h = subpixel_g * 0.75f;"
        ))
        XCTAssertTrue(shader.contains(
            "return fxaa_quality_15(input.texture_coordinate);"
        ))
        XCTAssertFalse(shader.contains("dot(color"))
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
        ).replacingOccurrences(of: "\r\n", with: "\n")

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

    func testProductionPostRunsLegacyHDRStagesBeforeOutput() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let lutShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_lut.frag.hlsl"
            ),
            encoding: .utf8
        )
        let casShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_cas.frag.hlsl"
            ),
            encoding: .utf8
        )
        let tiltShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_tilt_shift.frag.hlsl"
            ),
            encoding: .utf8
        )
        let outputShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_post.frag.hlsl"
            ),
            encoding: .utf8
        )
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(lutShader.contains("return grade_lut(scene_sample(uv));"))
        XCTAssertTrue(casShader.contains("const float3 e = scene_sample(uv);"))
        XCTAssertFalse(casShader.contains("grade_lut("))
        XCTAssertTrue(tiltShader.contains(
            "const float3 center = scene_sample(uv);"
        ))
        XCTAssertFalse(tiltShader.contains("cas_sample("))
        XCTAssertTrue(outputShader.contains(
            "scene_texture.SampleLevel("
        ))
        XCTAssertTrue(outputShader.contains(
            "1.0f / inverse_resolution.y - input.position.y"
        ))
        XCTAssertFalse(outputShader.contains("tilt_shift("))
        XCTAssertFalse(outputShader.contains("grade_lut("))
        XCTAssertTrue(nativeScene.contains("post_ping_texture"))
        XCTAssertTrue(nativeScene.contains("post_pong_texture"))
        XCTAssertTrue(nativeScene.contains("BTBH Post LUT"))
        XCTAssertTrue(nativeScene.contains("BTBH Post CAS"))
        XCTAssertTrue(nativeScene.contains("BTBH Post Tilt Shift"))
        XCTAssertTrue(nativeScene.contains(
            "SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT"
        ))
    }

    func testProductionTiltShiftFusesOutputAtLegacyHalfPrecisionBoundary() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_tilt_shift.frag.hlsl"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(shader.contains("float3 quantize_half3(float3 color)"))
        XCTAssertTrue(shader.contains(
            "f16tof32(f32tof16(color.x))"
        ))
        XCTAssertTrue(shader.contains(
            "output_color(\n" +
            "            quantize_half3(post_tilt_shift(input.texture_coordinate))"
        ))
        XCTAssertTrue(nativeScene.contains(
            "\"BTBH Post Tilt Shift + Output\""
        ))
        XCTAssertTrue(nativeScene.contains(
            "if (fullscreen_ok && !production_output_rendered)"
        ))
    }

    func testProductionTiltShiftProjectsLegacyBottomOriginCoordinatesToGPUTextureSpace() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_tilt_shift.frag.hlsl"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(shader.contains(
            "float2 legacy_screen_coordinate(float2 gpu_texture_coordinate)"
        ))
        XCTAssertTrue(shader.contains(
            "return float2(gpu_texture_coordinate.x, 1.0f - gpu_texture_coordinate.y);"
        ))
        XCTAssertTrue(shader.contains(
            "float2 gpu_texture_offset(float2 legacy_screen_offset)"
        ))
        XCTAssertTrue(shader.contains(
            "return float2(legacy_screen_offset.x, -legacy_screen_offset.y);"
        ))
        XCTAssertTrue(shader.contains(
            "const float2 legacy_uv = legacy_screen_coordinate(uv);"
        ))
        XCTAssertTrue(shader.contains(
            "hash12(legacy_uv / inverse_resolution)"
        ))
        XCTAssertTrue(shader.contains(
            "uv + gpu_texture_offset(rotated) * inverse_resolution * radius"
        ))
    }

    func testProductionSSAOResolvesAmbientBeforePostProcessing() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let modelShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let postShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_post.frag.hlsl"
            ),
            encoding: .utf8
        )
        let resolveShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_ssao_resolve.frag.hlsl"
            ),
            encoding: .utf8
        )
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")

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
            "float4 emission : SV_Target4;"
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
        XCTAssertTrue(resolveShader.contains(
            "Texture2D<float4> emission_texture"
        ))
        XCTAssertTrue(resolveShader.contains(
            "if (ssao_enabled == 2u)"
        ))
        XCTAssertTrue(resolveShader.contains(
            "scene - emission"
        ))
        XCTAssertTrue(resolveShader.contains(
            "const float contact_visibility = lerp("
        ))
        XCTAssertTrue(resolveShader.contains(
            "rc_contact_ao_strength"
        ))
        // The contact AO is the ambient light's shadow: the scene shader writes its light's directness to the
        // emission target's alpha (below the HRC resolve's unlit test), and direct light keeps a small share of
        // the AO only.
        XCTAssertTrue(resolveShader.contains(
            "rc_contact_ao_strength * lerp(1.0f, rc_contact_ao_direct_share, directness)"
        ))
        XCTAssertTrue(resolveShader.contains("static const float rc_directness_scale = 0.45f;"))
        XCTAssertTrue(modelShader.contains("static const float btbh_rc_directness_scale = 0.45f;"))
        XCTAssertTrue(modelShader.contains(
            "unlit ? 1.0f : saturate(directness) * btbh_rc_directness_scale"
        ))
        XCTAssertTrue(nativeScene.contains(
            "SDL_GPUColorTargetDescription color_targets[6];"
        ))
        XCTAssertTrue(nativeScene.contains(
            "SDL_GPUTextureSamplerBinding resolve_samplers[7];"
        ))
        XCTAssertTrue(nativeScene.contains(
            "resolve_samplers[6].texture = runtime->emission_texture;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "radiance_cascades_active\n" +
                "            ? BTBH_GPU_SCENE_SSAO_RC_CONTACT_MODE"
        ))
        XCTAssertTrue(nativeScene.contains(
            "const bool ssao_resolve_active ="
        ))
        XCTAssertTrue(nativeScene.contains(
            "!radiance_cascades_debug_output;"
        ))
        // Resolved at the start of the transparent phase, on the opaque scene only.
        XCTAssertTrue(nativeScene.contains(
            "if (transparent_phase && ssao_resolve_active) {"
        ))
        XCTAssertFalse(nativeScene.contains(
            "post->ssao_enabled != 0u && !radiance_cascades_active"
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->ambient_specular_texture"
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->emission_texture"
        ))
        XCTAssertTrue(nativeScene.contains(
            "color_targets[3].blend_state.color_write_mask =\n" +
                "        transparent ? 0u : 0x0fu;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "post_input = runtime->ssao_resolved_texture;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "SDL_GPUTextureSamplerBinding lut_samplers[2];"
        ))
        XCTAssertTrue(nativeScene.contains(
            "depth_target.store_op = SDL_GPU_STOREOP_STORE;"
        ))
    }

    func testProductionGeometrySkipsUnusedSSAOTargets() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")

        XCTAssertTrue(nativeScene.contains(
            "#define BTBH_GPU_SCENE_COLOR_TARGET_VARIANT_COUNT 8u"
        ))
        XCTAssertTrue(nativeScene.contains(
            "info.target_info.num_color_targets = temporal\n" +
                "        ? (transparent ? 2u : 6u)\n" +
                "        : (auxiliary_geometry_outputs ? 5u : 1u);"
        ))
        XCTAssertTrue(nativeScene.contains(
            "const uint32_t color_target_count = temporal_active\n" +
                "        ? 6u\n" +
                "        : (auxiliary_geometry_outputs ? 5u : 1u);"
        ))
        XCTAssertTrue(nativeScene.contains(
            "                ? (temporal_active ? 2u : 1u)\n" +
                "                : color_target_count,"
        ))
    }

    func testProductionTargetsUseInternalRenderResolutionAndCompositeToDrawable() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")

        XCTAssertTrue(nativeScene.contains(
            "btbh_gpu_scene_ensure_depth(\n" +
                "            window,\n" +
                "            render_width,\n" +
                "            render_height,"
        ))
        XCTAssertTrue(nativeScene.contains(
            "post_uniforms.inverse_resolution[0] =\n" +
                "        1.0f / (float)render_width;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "fxaa_uniforms.metrics[2] = (float)render_width;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "report->drawable_width = drawable_width;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "blit.source.w = window->scene_runtime->composite_width;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "blit.destination.w = destination_width;"
        ))
    }

    func testProductionLanczosUpscaleUsesDedicatedDrawableSizedPass() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_upscale_lanczos.frag.hlsl"
            ),
            encoding: .utf8
        )
        let cmake = try String(
            contentsOf: packageRoot.appendingPathComponent("CMakeLists.txt"),
            encoding: .utf8
        )

        XCTAssertTrue(cmake.contains(
            "\"production_upscale_lanczos.frag|fragment\""
        ))
        XCTAssertTrue(shader.contains("float4 lanczos(float4 value)"))
        XCTAssertEqual(
            shader.components(separatedBy: ".SampleLevel(").count - 1,
            12
        )
        XCTAssertTrue(nativeScene.contains(
            "upscale_mode == BTBH_GPU_SCENE_UPSCALE_LANCZOS"
        ))
        XCTAssertTrue(nativeScene.contains(
            "\"BTBH Lanczos Upscale\""
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->composite_color_texture =\n" +
                "            runtime->upscaled_color_texture;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "blit.source.texture =\n" +
                "        window->scene_runtime->composite_color_texture;"
        ))
    }

    func testProductionFSR1UpscaleRunsEASUThenRCASAtTheDrawableSize() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        func read(_ path: String) throws -> String {
            try String(contentsOf: packageRoot.appendingPathComponent(path), encoding: .utf8)
                .replacingOccurrences(of: "\r\n", with: "\n")
        }
        let nativeScene = try read("src/gpu/btbh_gpu_scene.c")
        let easu = try read("shaders/gpu/production_upscale_fsr_easu.frag.hlsl")
        let rcas = try read("shaders/gpu/production_upscale_fsr_rcas.frag.hlsl")
        let cmake = try read("CMakeLists.txt")

        XCTAssertTrue(cmake.contains("\"production_upscale_fsr_easu.frag|fragment\""))
        XCTAssertTrue(cmake.contains("\"production_upscale_fsr_rcas.frag|fragment\""))
        // EASU reads the twelve texels around the output pixel; RCAS the cross of five.
        XCTAssertEqual(easu.components(separatedBy: "= load_source(").count - 1, 12)
        XCTAssertEqual(rcas.components(separatedBy: "= load_source(").count - 1, 5)
        XCTAssertTrue(nativeScene.contains("upscale_mode == BTBH_GPU_SCENE_UPSCALE_FSR1"))
        XCTAssertTrue(nativeScene.contains("\"BTBH FSR1 EASU\""))
        XCTAssertTrue(nativeScene.contains("\"BTBH FSR1 RCAS\""))
        XCTAssertTrue(nativeScene.contains(
            "runtime->composite_color_texture =\n" +
                "                runtime->sharpened_color_texture;"
        ))
        XCTAssertEqual(GPUUpscaleModeProjection.native(.fsr1), 2)
    }

    func testProductionMirrorFlipsTheFinishedImageBeforeUpscaleAndComposite() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let header = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "include/btbh_gpu/btbh_gpu.h"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")

        XCTAssertTrue(header.contains(
            "    uint8_t upscale_mode;\n" +
                "    /* 1 flips the final scene image left-right before it is composited. */\n" +
                "    uint8_t mirror_horizontal;\n"
        ))
        XCTAssertTrue(nativeScene.contains("post->mirror_horizontal <= 1u &&"))
        XCTAssertTrue(nativeScene.contains(
            "anti_aliasing != 0u ||\n" +
                "                post->mirror_horizontal != 0u,"
        ))
        XCTAssertTrue(nativeScene.contains(
            "mirror_blit.flip_mode = SDL_FLIP_HORIZONTAL;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "mirror_blit.filter = SDL_GPU_FILTER_NEAREST;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "upscale_sampler.texture = runtime->composite_color_texture;"
        ))
        let blend = try XCTUnwrap(nativeScene.range(of: "\"BTBH SMAA Blend\""))
        let mirror = try XCTUnwrap(nativeScene.range(of: "\"BTBH Scene Mirror\""))
        let upscale = try XCTUnwrap(nativeScene.range(of: "\"BTBH Lanczos Upscale\""))
        XCTAssertLessThan(blend.lowerBound, mirror.lowerBound, "the mirror follows anti-aliasing")
        XCTAssertLessThan(mirror.lowerBound, upscale.lowerBound, "the upscale reads the mirrored image")
        // FSR 3 mirrors after it upscales instead (its history and motion vectors stay unmirrored).
        XCTAssertEqual(nativeScene.components(separatedBy: "flip_mode").count - 1, 2)
    }

    func testScenePostCarriesTheMirrorFlagOnlyWhenRequested() throws {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH scene post mirror projection",
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
        XCTAssertFalse(GPUScenePostProbe.default.mirrorHorizontally)
        let plain = try resources.scenePost(
            environment: .default,
            chain: .empty,
            antiAliasing: .fxaa,
            upscaleMode: .linear
        )
        XCTAssertFalse(plain.mirrorHorizontally)
        let mirrored = try resources.scenePost(
            environment: .default,
            chain: .init(
                screenShaders: [],
                casSharpness: 0,
                tiltShiftEnabled: false,
                mirrorHorizontally: true
            ),
            antiAliasing: .fxaa,
            upscaleMode: .linear
        )
        XCTAssertTrue(mirrored.mirrorHorizontally)
        XCTAssertTrue(
            mirrored.withRadianceCascades(.off).mirrorHorizontally,
            "rebuilding the probe keeps the flag"
        )
        XCTAssertEqual(
            MemoryLayout<BTBHGPUScenePost>.offset(of: \.mirror_horizontal),
            MemoryLayout<BTBHGPUScenePost>.offset(of: \.upscale_mode).map { $0 + 1 }
        )
        XCTAssertEqual(MemoryLayout<BTBHGPUScenePost>.offset(of: \.radiance_cascades), 72)
    }

    func testScenePostCarriesFogBloomAndVignetteOnlyWhenRequested() throws {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH scene post HDR composite projection",
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
        XCTAssertEqual(GPUScenePostProbe.default.fog, .disabled)
        XCTAssertEqual(GPUScenePostProbe.default.bloom, .disabled)
        XCTAssertEqual(GPUScenePostProbe.default.vignette, .disabled)
        let plain = try resources.scenePost(
            environment: .default,
            chain: .empty,
            antiAliasing: .fxaa,
            upscaleMode: .linear
        )
        XCTAssertFalse(plain.fog.isEnabled)
        XCTAssertFalse(plain.bloom.isEnabled)
        XCTAssertFalse(plain.vignette.isEnabled)

        let fog = FogDescriptor(color: .white, density: 0.05, startDistance: 18, maximumOpacity: 1)
        let bloom = BloomDescriptor(intensity: 0.6, threshold: 1.4, softKnee: 0.5, scatter: 0.65)
        let vignette = VignetteDescriptor(intensity: 0.25, smoothness: 0.6)
        let composed = try resources.scenePost(
            environment: .init(
                backgroundColor: .black, backgroundEnergy: 1, ambientColor: .white, ambientEnergy: 1,
                ambientMap: .invalid, skyCubemap: .invalid, ssaoEnabled: true, tonemap: .pbrNeutral,
                tonemapWhite: 1, fog: fog
            ),
            chain: .init(screenShaders: [], casSharpness: 0, tiltShiftEnabled: false, bloom: bloom, vignette: vignette),
            antiAliasing: .fxaa,
            upscaleMode: .linear
        )
        XCTAssertEqual(composed.fog, fog)
        XCTAssertEqual(composed.bloom, bloom)
        XCTAssertEqual(composed.vignette, vignette)
        let rebuilt = composed.withRadianceCascades(.off)
        XCTAssertEqual(rebuilt.fog, fog, "rebuilding the probe keeps the HDR composite")
        XCTAssertEqual(rebuilt.bloom, bloom)
        XCTAssertEqual(rebuilt.vignette, vignette)

        XCTAssertTrue(GPUWindowProbe.hdrCompositeIsValid(fog: fog, bloom: bloom, vignette: vignette))
        XCTAssertTrue(GPUWindowProbe.hdrCompositeIsValid(fog: .disabled, bloom: .disabled, vignette: .disabled))
        XCTAssertFalse(GPUWindowProbe.hdrCompositeIsValid(
            fog: .init(color: .white, density: 0.05, startDistance: 0, maximumOpacity: 1.5),
            bloom: bloom, vignette: vignette
        ))
        XCTAssertFalse(GPUWindowProbe.hdrCompositeIsValid(
            fog: fog, bloom: .init(intensity: 0.6, threshold: 1, softKnee: 2, scatter: 0.5), vignette: vignette
        ))
        XCTAssertFalse(GPUWindowProbe.hdrCompositeIsValid(
            fog: fog, bloom: bloom, vignette: .init(intensity: 0.3, smoothness: 0)
        ))
        let edge = FogEdgeDescriptor(minimumX: -10, minimumZ: -20, maximumX: 30, maximumZ: 40, fadeDistance: 6)
        let edgedFog = FogDescriptor(color: .white, density: 0.05, startDistance: 18, maximumOpacity: 1, edge: edge)
        XCTAssertTrue(GPUWindowProbe.hdrCompositeIsValid(fog: edgedFog, bloom: bloom, vignette: vignette))
        XCTAssertFalse(GPUWindowProbe.hdrCompositeIsValid(
            fog: .init(color: .white, density: 0.05, startDistance: 18, maximumOpacity: 1,
                       edge: .init(minimumX: 30, minimumZ: -20, maximumX: -10, maximumZ: 40, fadeDistance: 6)),
            bloom: bloom, vignette: vignette
        ), "an inverted edge rectangle is rejected")
        XCTAssertFalse(GPUWindowProbe.hdrCompositeIsValid(
            fog: .init(color: .white, density: 0.05, startDistance: 18, maximumOpacity: 1,
                       edge: .init(minimumX: -10, minimumZ: -20, maximumX: 30, maximumZ: 40, fadeDistance: 0)),
            bloom: bloom, vignette: vignette
        ), "the edge needs a fade band")
        XCTAssertFalse(GPUWindowProbe.hdrCompositeIsValid(
            fog: .init(color: .white, density: 0.05, startDistance: 18, maximumOpacity: 1,
                       edge: .init(minimumX: -10, minimumZ: -20, maximumX: 30, maximumZ: 40, fadeDistance: 6,
                                   clearDistance: -1)),
            bloom: bloom, vignette: vignette
        ), "the clear depth cannot be negative")
        let edged = try resources.scenePost(
            environment: .init(
                backgroundColor: .black, backgroundEnergy: 1, ambientColor: .white, ambientEnergy: 1,
                ambientMap: .invalid, skyCubemap: .invalid, ssaoEnabled: true, tonemap: .pbrNeutral,
                tonemapWhite: 1, fog: edgedFog
            ),
            chain: .empty,
            antiAliasing: .fxaa,
            upscaleMode: .linear
        )
        XCTAssertEqual(edged.fog.edge, edge)

        // The new fields follow the radiance block, so every earlier offset is unchanged.
        let radianceOffset = try XCTUnwrap(MemoryLayout<BTBHGPUScenePost>.offset(of: \.radiance_cascades))
        let fogOffset = try XCTUnwrap(MemoryLayout<BTBHGPUScenePost>.offset(of: \.fog_color))
        XCTAssertGreaterThan(fogOffset, radianceOffset)
        XCTAssertEqual(
            MemoryLayout<BTBHGPUScenePost>.offset(of: \.vignette_enabled),
            MemoryLayout<BTBHGPUScenePost>.offset(of: \.bloom_enabled).map { $0 + 1 }
        )
        XCTAssertEqual(
            MemoryLayout<BTBHGPUScenePost>.offset(of: \.fog_edge_enabled),
            MemoryLayout<BTBHGPUScenePost>.offset(of: \.vignette_enabled).map { $0 + 1 }
        )
    }

    func testHDRCompositeRunsBeforeGradingFromItsOwnShaders() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent("src/gpu/btbh_gpu_scene.c"),
            encoding: .utf8
        )
        let cmake = try String(contentsOf: packageRoot.appendingPathComponent("CMakeLists.txt"), encoding: .utf8)
        let composite = try String(
            contentsOf: packageRoot.appendingPathComponent("shaders/gpu/production_hdr_composite.frag.hlsl"),
            encoding: .utf8
        )
        let cas = try String(
            contentsOf: packageRoot.appendingPathComponent("shaders/gpu/production_cas.frag.hlsl"),
            encoding: .utf8
        )
        for name in ["production_bloom_downsample.frag", "production_bloom_upsample.frag", "production_hdr_composite.frag"] {
            XCTAssertTrue(cmake.contains("\"\(name)|fragment\""), name)
            XCTAssertTrue(nativeScene.contains("\"\(name)\""), name)
        }
        let compositeRange = try XCTUnwrap(nativeScene.range(of: "\"BTBH HDR Composite\""))
        let lutRange = try XCTUnwrap(nativeScene.range(of: "\"BTBH Post LUT\""))
        XCTAssertLessThan(compositeRange.lowerBound, lutRange.lowerBound, "the composite runs on HDR, before grading")
        XCTAssertTrue(nativeScene.contains("runtime->post_pong_texture,\n                &composite_uniforms") ||
            nativeScene.contains("runtime->post_pong_texture,\r\n                &composite_uniforms"),
            "the LUT stage writes post_ping, so the composite must write post_pong")
        XCTAssertTrue(composite.contains("depth >= 0.99999f"), "the composite leaves the cleared background alone")
        XCTAssertTrue(
            nativeScene.contains("if (hdr_composite_active && post->fog_enabled != 0u) {"),
            "the scene pass clears the background to the fog colour, so effects over it stay visible"
        )
        XCTAssertTrue(composite.contains("float4 fog_edge_bounds;"), "the edge rectangle follows the vignette")
        XCTAssertTrue(composite.contains("(distance - fog_edge.z)"), "the play area keeps a crisp edge")
        // AMD CAS sharpens with a negative neighbour lobe and stays finite for HDR input.
        XCTAssertTrue(cas.contains("const float weight = -amplitude * amount;"))
        XCTAssertTrue(cas.contains("sqrt(saturate("))
    }

    func testProductionSSAOUsesLegacyWorldSpaceObscurance() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let resolveShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_ssao_resolve.frag.hlsl"
            ),
            encoding: .utf8
        )
        let denoiseShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_ssao_denoise.frag.hlsl"
            ),
            encoding: .utf8
        )
        let ssaoShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_ssao.frag.hlsl"
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
            "SDL_GPUTextureSamplerBinding resolve_samplers[7];"
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
                "shaders/gpu/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )

        // The sun (primary directional) covers the whole view, so it gets the large map; the
        // secondary directional keeps the small one.
        XCTAssertEqual(GPUSceneDirectionalShadowProbe.primaryMapSize, 2048)
        XCTAssertEqual(GPUSceneDirectionalShadowProbe.mapSize, 512)
        XCTAssertEqual(GPUSceneOmniShadowProbe.mapSize, 512)
        XCTAssertTrue(nativeScene.contains(
            "#define BTBH_GPU_PRIMARY_DIRECTIONAL_SHADOW_MAP_SIZE 2048u"
        ))
        XCTAssertTrue(nativeScene.contains(
            "#define BTBH_GPU_DIRECTIONAL_SHADOW_MAP_SIZE 512u"
        ))
        XCTAssertTrue(nativeScene.contains(
            "#define BTBH_GPU_SPOT_SHADOW_MAP_SIZE 1024u"
        ))
        XCTAssertTrue(nativeScene.contains(
            "#define BTBH_GPU_OMNI_SHADOW_MAP_SIZE 512u"
        ))
        XCTAssertTrue(nativeScene.contains(
            "shadow_info.type = SDL_GPU_TEXTURETYPE_CUBE;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "shadow_info.layer_count_or_depth = " +
                "BTBH_GPU_OMNI_SHADOW_FACE_COUNT;"
        ))
        XCTAssertTrue(nativeScene.contains(
            "BTBH_GPU_SPOT_SHADOW_MAP_SIZE"
        ))
        XCTAssertEqual(
            shader.components(separatedBy: "512.0f").count - 1,
            2
        )
        XCTAssertEqual(
            shader.components(separatedBy: "2048.0f").count - 1,
            1
        )
        XCTAssertEqual(
            shader.components(separatedBy: "1024.0f").count - 1,
            1
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
        let compactScene = nativeScene.filter { !$0.isWhitespace }

        XCTAssertTrue(nativeScene.contains(
            "shadow_pipelines[BTBH_GPU_SCENE_CULL_COUNT]"
        ))
        XCTAssertTrue(nativeScene.contains(
            "animated_shadow_pipelines[BTBH_GPU_SCENE_CULL_COUNT]"
        ))
        XCTAssertTrue(nativeScene.contains(
            "btbh_gpu_scene_cull_mode((BTBHGPUSceneCullMode)cull_index)"
        ))
        XCTAssertTrue(compactScene.contains(
            "runtime->shadow_pipelines[batch->cull_mode]"
        ))
        XCTAssertTrue(compactScene.contains(
            "runtime->fast_shadow_pipelines[batch->cull_mode]"
        ))
        XCTAssertTrue(compactScene.contains(
            "runtime->non_discard_shadow_pipelines[batch->cull_mode]"
        ))
        XCTAssertTrue(compactScene.contains(
            "runtime->animated_shadow_pipelines[batch->cull_mode]"
        ))
        XCTAssertTrue(compactScene.contains(
            "runtime->animated_non_discard_shadow_pipelines[batch->cull_mode]"
        ))
    }

    func testProductionAmbientMatchesLegacyFinalAlbedoApplication() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_model.frag.hlsl"
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
                "shaders/gpu/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let staticVertexShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_model.vert.hlsl"
            ),
            encoding: .utf8
        )
        let animatedVertexShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_animated_model.vert.hlsl"
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

    func testProductionNeutralNormalFallbackMatchesLegacyBytes() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_scene.c"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(source.contains(
            "const uint8_t normal_pixel[4] = {127u, 127u, 255u, 0u};"
        ))
    }

    func testAnimatedModelMatchesLegacyHalfPackedTextureCoordinates() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_animated_model.vert.hlsl"
            ),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")

        XCTAssertTrue(shader.contains(
            "f16tof32(f32tof16(input.texture_coordinate.x))"
        ))
        XCTAssertTrue(shader.contains(
            "1.0f - f16tof32(\n" +
                "            f32tof16(1.0f - input.texture_coordinate.y)"
        ))
    }

    func testProductionNormalMappingPreservesLegacyInterpolatedTBN() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let fragmentShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_model.frag.hlsl"
            ),
            encoding: .utf8
        )
        let staticVertexShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_model.vert.hlsl"
            ),
            encoding: .utf8
        )
        let animatedVertexShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_animated_model.vert.hlsl"
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
            "shaders/gpu/production_animated_model.vert.hlsl",
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
                "shaders/gpu/production_model.frag.hlsl"
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

    func testProductionWaterWavesAreWorldSpaceWithOneNormalDecode() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_model.frag.hlsl"
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
        XCTAssertTrue(waterSource.contains("input.world_position.xz"))
        XCTAssertTrue(waterSource.contains("albedo.rgb *= lerp("))
        XCTAssertTrue(waterSource.contains("normal_map = normalize("))
        // A second decode laid the wave normal in the tangent plane: the sun lit water as a wall.
        XCTAssertFalse(waterSource.contains(") * 2.0f - 1.0f;"))

        // No vertex bob: in model space it split the seams between water tiles.
        let surface = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_surface.hlsli"
            ),
            encoding: .utf8
        )
        XCTAssertFalse(surface.contains("surface_variant == 2u"))
    }

    func testProductionSurfaceVertexAndShadowPassesShareDeformationContract()
        throws
    {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shaderDirectory = packageRoot.appendingPathComponent(
            "shaders/gpu"
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

    func testProductionShadowFastPathsPreserveDeformationAndDiscardVariants()
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
            "shaders/gpu"
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
        let shadowFunction = try XCTUnwrap(
            nativeScene.range(
                of: "static bool btbh_gpu_scene_render_directional_shadow("
            ).flatMap { start in
                nativeScene.range(
                    of: "\nstatic bool btbh_gpu_scene_render_fullscreen(",
                    range: start.lowerBound..<nativeScene.endIndex
                ).map {
                    String(nativeScene[start.lowerBound..<$0.lowerBound])
                }
            }
        )
        let normalizedShadowFunction = shadowFunction
            .split(whereSeparator: \.isWhitespace)
            .joined(separator: " ")
        let fastUniformBlock = try XCTUnwrap(
            vertexShader.range(of: "cbuffer ShadowUniforms")
                .flatMap { start in
                    vertexShader.range(
                        of: "};",
                        range: start.lowerBound..<vertexShader.endIndex
                    ).map {
                        String(vertexShader[start.lowerBound..<$0.upperBound])
                    }
                }
        )
        let normalizedFastUniformBlock = fastUniformBlock
            .split(whereSeparator: \.isWhitespace)
            .joined(separator: " ")

        XCTAssertTrue(normalizedNativeScene.contains(
            "surface_variant == BTBH_GPU_SCENE_SURFACE_GLITTER_STATIC"
        ))
        XCTAssertTrue(normalizedNativeScene.contains(
            "surface_variant == BTBH_GPU_SCENE_SURFACE_GLITTER"
        ))
        XCTAssertTrue(normalizedNativeScene.contains(
            "batch->billboard_mode == BTBH_GPU_SCENE_BILLBOARD_DISABLED"
        ))
        XCTAssertTrue(nativeScene.contains(
            "SDL_GPUBuffer *instance_vertex_buffer;"
        ))
        XCTAssertTrue(normalizedNativeScene.contains(
            "buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX; " +
                "runtime->instance_vertex_buffer = " +
                "SDL_CreateGPUBuffer(device, &buffer_info);"
        ))
        XCTAssertTrue(normalizedShadowFunction.contains(
            "batch->instance_buffer != NULL ? " +
                "batch->instance_buffer->vertex_buffer : " +
                "transient_instance_vertex_buffer"
        ))
        XCTAssertTrue(normalizedShadowFunction.contains(
            "batch->first_instance * BTBH_GPU_SCENE_INSTANCE_STRIDE"
        ))
        XCTAssertTrue(nativeScene.contains(
            "runtime->fast_shadow_pipelines["
        ))
        XCTAssertTrue(nativeScene.contains(
            "non_discard_shadow_pipelines["
        ))
        XCTAssertTrue(nativeScene.contains(
            "animated_non_discard_shadow_pipelines["
        ))
        XCTAssertTrue(normalizedNativeScene.contains(
            "surface_variant == BTBH_GPU_SCENE_SURFACE_DISSOLVE"
        ))
        XCTAssertTrue(normalizedNativeScene.contains(
            "surface_variant == BTBH_GPU_SCENE_SURFACE_SHATTER"
        ))
        XCTAssertTrue(vertexShader.contains(
            "output.position = mul(light_view_projection, world_position);"
        ))
        XCTAssertFalse(vertexShader.contains("btbh_apply_surface_vertex"))
        XCTAssertFalse(fragmentShader.contains("discard"))
        XCTAssertTrue(shadowFunction.contains(
            "bool shadow_uniform_bound = false"
        ))
        XCTAssertTrue(normalizedShadowFunction.contains(
            "if (uses_fast_shadow) { if (!shadow_uniform_bound)"
        ))
        XCTAssertTrue(normalizedShadowFunction.contains(
            "light_view_projection, sizeof(float) * 16u"
        ))
        XCTAssertTrue(normalizedShadowFunction.contains(
            "} else { BTBHGPUShadowUniforms uniforms;"
        ))
        XCTAssertEqual(
            shadowFunction.components(
                separatedBy: "SDL_PushGPUVertexUniformData("
            ).count - 1,
            2
        )
        XCTAssertEqual(
            shadowFunction.components(
                separatedBy: "bool shadow_uniform_bound = false"
            ).count - 1,
            1
        )
        XCTAssertEqual(
            shadowFunction.components(
                separatedBy: "shadow_uniform_bound = true"
            ).count - 1,
            2
        )
        XCTAssertEqual(
            normalizedFastUniformBlock,
            "cbuffer ShadowUniforms : register(b0, space1) { " +
                "float4x4 light_view_projection; };"
        )
    }

    func testFastShadowShaderPathsAreRequiredAndNeverAliasRegularShaders()
        throws
    {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let probeSource = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "Sources/BTBHRendererGPU/BTBHRendererGPU.swift"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(probeSource.contains(
            "fastShadowVertexShaderPath: String,"
        ))
        XCTAssertTrue(probeSource.contains(
            "fastShadowFragmentShaderPath: String,"
        ))
        XCTAssertFalse(probeSource.contains(
            "fastShadowVertexShaderPath ?? shadowVertexShaderPath"
        ))
        XCTAssertFalse(probeSource.contains(
            "fastShadowFragmentShaderPath ?? shadowFragmentShaderPath"
        ))
    }

    func testTransparentSceneBatchesUseLegacyMaterialStateOrderAfterOpaqueBatches() {
        func material(
            descriptor: MaterialDescriptor
        ) -> GPUSceneMaterialProbe {
            var result = GPUSceneMaterialProbe(
                albedoTexture: nil,
                albedoColor: .white,
                uvScale: descriptor.uvScale,
                uvOffset: descriptor.uvOffset,
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
                blendMode: descriptor.blend,
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
        var additiveProjectile = opaqueDescriptor
        additiveProjectile.blend = .additive

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
        let additiveWithTransparencyDisabled = GPUSceneGeometryProbeBatch(
            geometry: .customMesh(pointer),
            animation: nil,
            material: material(descriptor: additiveProjectile),
            materialSlot: nil,
            legacyInstanced: false,
            firstInstance: 5,
            instanceCount: 1
        )
        let ordered = GPUSceneBatchOrdering.opaqueThenLegacyMaterialOrder(
            [
                additiveWithTransparencyDisabled,
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
        XCTAssertEqual(ordered[4].firstInstance, 5)
        XCTAssertEqual(ordered[5].firstInstance, 4)
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

    func testAdjacentPersistentModelDrawsCoalesceBeforeBatchResolution() {
        let instances = (0..<8).map { _ in
            RenderInstance(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )
        }
        let buffer = InstanceBufferHandle(rawValue: 7)
        let left = ModelInstancesDraw(
            model: ModelHandle(rawValue: 3),
            instances: instances,
            overrides: [],
            persistentBuffer: buffer,
            persistentRevision: 11,
            sourceStart: 1,
            count: 3
        )
        let right = ModelInstancesDraw(
            model: ModelHandle(rawValue: 3),
            instances: instances,
            overrides: [],
            persistentBuffer: buffer,
            persistentRevision: 11,
            sourceStart: 4,
            count: 2
        )

        let merged = GPUModelInstanceDrawCoalescing.merge(left, right)

        XCTAssertEqual(merged?.sourceStart, 1)
        XCTAssertEqual(merged?.count, 5)
        XCTAssertEqual(merged?.persistentBuffer, buffer)
        XCTAssertEqual(merged?.persistentRevision, 11)
    }

    func testPersistentModelDrawsDoNotCoalesceAcrossRevisionOrRangeGap() {
        let instances = (0..<8).map { _ in
            RenderInstance(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )
        }
        let left = ModelInstancesDraw(
            model: ModelHandle(rawValue: 3),
            instances: instances,
            overrides: [],
            persistentBuffer: InstanceBufferHandle(rawValue: 7),
            persistentRevision: 11,
            sourceStart: 0,
            count: 2
        )
        let revisionChanged = ModelInstancesDraw(
            model: left.model,
            instances: instances,
            overrides: [],
            persistentBuffer: left.persistentBuffer,
            persistentRevision: 12,
            sourceStart: 2,
            count: 2
        )
        let rangeGap = ModelInstancesDraw(
            model: left.model,
            instances: instances,
            overrides: [],
            persistentBuffer: left.persistentBuffer,
            persistentRevision: left.persistentRevision,
            sourceStart: 3,
            count: 2
        )

        XCTAssertNil(
            GPUModelInstanceDrawCoalescing.merge(left, revisionChanged)
        )
        XCTAssertNil(GPUModelInstanceDrawCoalescing.merge(left, rangeGap))
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
            updatePeriodFrames: 2,
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

    /// A low sun throws building shadows several times their height. A range the camera does not
    /// see stays in the sun's shadow pass while the ground its shadow covers is in view, so those
    /// shadows no longer pop in and out at the screen edges, and nothing farther joins the pass.
    func testSunShadowFootprintSweepsTheTopDownToTheBase() throws {
        let tower = RenderBounds3(
            minimum: .init(x: 0, y: 0, z: 0),
            maximum: .init(x: 1, y: 2, z: 1)
        )
        // Overhead sun: the shadow stays under the box.
        XCTAssertEqual(
            GPUSceneBatchShadowCulling.sunShadowFootprint(of: tower, lightDirection: .init(x: 0, y: -1, z: 0)),
            tower
        )
        // 45 degrees toward +x and -z: the shadow reaches one height out along the light.
        let slanted = try XCTUnwrap(GPUSceneBatchShadowCulling.sunShadowFootprint(
            of: tower, lightDirection: .init(x: 1, y: -1.4142135, z: -1)
        ))
        XCTAssertEqual(slanted.minimum.x, 0, accuracy: 1e-5)
        XCTAssertEqual(slanted.maximum.x, 1 + 2 / 1.4142135, accuracy: 1e-4)
        XCTAssertEqual(slanted.minimum.z, -2 / 1.4142135, accuracy: 1e-4)
        XCTAssertEqual(slanted.maximum.z, 1, accuracy: 1e-5)
        XCTAssertEqual(slanted.minimum.y, 0)
        XCTAssertEqual(slanted.maximum.y, 2)
        // A level light, or invalid bounds, casts nothing into view from off screen.
        XCTAssertNil(GPUSceneBatchShadowCulling.sunShadowFootprint(of: tower, lightDirection: .init(x: 1, y: 0, z: 0)))
        XCTAssertNil(GPUSceneBatchShadowCulling.sunShadowFootprint(
            of: .init(minimum: .init(x: 1, y: 0, z: 0), maximum: .init(x: 0, y: 1, z: 1)),
            lightDirection: .init(x: 0, y: -1, z: 0)
        ))
    }

    func testShadowOnlyRangesStayOutOfTheScenePassAndDoNotMergeWithVisibleOnes() {
        let geometry = OpaquePointer(bitPattern: 1)!
        let instanceBuffer = OpaquePointer(bitPattern: 2)!
        func batch(firstInstance: Int, sceneVisible: Bool) -> GPUSceneGeometryProbeBatch {
            GPUSceneGeometryProbeBatch(
                geometry: .model(geometry),
                animation: nil,
                material: nil,
                materialSlot: 0,
                legacyInstanced: true,
                instanceBuffer: instanceBuffer,
                firstInstance: firstInstance,
                instanceCount: 1,
                sceneVisible: sceneVisible
            )
        }
        let shadowOnly = batch(firstInstance: 1, sceneVisible: false)

        XCTAssertTrue(GPUSceneBatchShadowCulling.castsDirectionalShadow(shadowOnly))
        XCTAssertEqual(
            GPUSceneBatchConsolidation.adjacentPersistentRanges([
                batch(firstInstance: 0, sceneVisible: true),
                shadowOnly,
            ]).count,
            2
        )
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

    func testStaticFastShadowUsesInstanceRateVertexInput() throws {
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
        let instanceBuffer = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "src/gpu/btbh_gpu_instance_buffer.c"
            ),
            encoding: .utf8
        )
        let fastShader = try String(
            contentsOf: packageRoot.appendingPathComponent(
                "shaders/gpu/production_shadow_fast.vert.hlsl"
            ),
            encoding: .utf8
        )

        XCTAssertTrue(nativeScene.contains(
            "SDL_GPU_VERTEXINPUTRATE_INSTANCE"
        ))
        XCTAssertTrue(nativeScene.contains(
            "BTBH_GPU_SCENE_INSTANCE_STRIDE"
        ))
        XCTAssertTrue(instanceBuffer.contains(
            "SDL_GPU_BUFFERUSAGE_VERTEX"
        ))
        XCTAssertFalse(fastShader.contains("StructuredBuffer"))
        XCTAssertTrue(fastShader.contains("model_column_3"))
    }

    /// Soft particles fade against the opaque scene's distance, written between the opaque and
    /// transparent passes into the normal map's slot of soft batches only.
    func testSoftParticlesFadeAgainstTheSceneDistancePass() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        func source(_ path: String) throws -> String {
            try String(contentsOf: packageRoot.appendingPathComponent(path), encoding: .utf8)
        }
        let model = try source("shaders/gpu/production_model.frag.hlsl")
        XCTAssertTrue(model.contains("surface_variant == 6u"))
        XCTAssertTrue(model.contains("normal_texture.Load(int3(input.pixel_position.xy, 0)).r"))
        let distance = try source("shaders/gpu/production_scene_distance.frag.hlsl")
        XCTAssertTrue(distance.contains("length(world.xyz / world.w - camera_position.xyz)"))
        XCTAssertTrue(try source("CMakeLists.txt").contains("\"production_scene_distance.frag|fragment\""))
        let scene = try source("src/gpu/btbh_gpu_scene.c")
        XCTAssertTrue(scene.contains("\"BTBH Scene Distance\""))
        XCTAssertTrue(scene.contains("samplers[2].texture = runtime->scene_distance_texture;"))
    }

    /// Ambient occlusion resolves on the opaque scene before the transparent pass draws over it,
    /// so an effect over a dark corner keeps its light (the resolve used to darken everything).
    func testAmbientOcclusionResolvesBeforeTheTransparentPass() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let scene = try String(
            contentsOf: packageRoot.appendingPathComponent("src/gpu/btbh_gpu_scene.c"),
            encoding: .utf8
        )
        let resolve = try XCTUnwrap(scene.range(of: "\"BTBH SSAO Resolve\""))
        let transparent = try XCTUnwrap(scene.range(of: "SDL_GPUColorTargetInfo transparent_targets[2];"))
        XCTAssertLessThan(resolve.lowerBound, transparent.lowerBound)
        XCTAssertTrue(scene.contains("if (transparent_phase && ssao_resolve_active) {"))
        XCTAssertEqual(scene.components(separatedBy: "post_input = runtime->ssao_resolved_texture;").count - 1, 1)
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

        let rim = try GPUSceneSurfaceProjection.classify(
            fragmentPath: "C:/game/Shaders/rim_light.glsl",
            parameters: [
                "u_color": .vec3(.init(x: 0.2, y: 0.9, z: 0.7)),
                "u_strength": .float(1.5),
                "u_glow": .float(0.2),
            ]
        )
        XCTAssertEqual(rim.variant, .rim)
        XCTAssertEqual(rim.parameters0.y, 0.9)
        XCTAssertEqual(rim.parameters0.w, 1.5)
        XCTAssertEqual(rim.parameters1.x, 0.2)

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
        let soft = try GPUSceneSurfaceProjection.classify(
            fragmentPath: "C:/game/Shaders/soft_particle.glsl",
            parameters: ["u_fadeDistance": .float(0.8)]
        )
        XCTAssertEqual(soft.variant, .softParticle)
        XCTAssertEqual(soft.parameters0.x, 0.8)
        XCTAssertEqual(
            GPUSceneSurfaceVariant.softParticle.rawValue,
            UInt32(BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE.rawValue)
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
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
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
            shadowSoftness: 2,
            shadowUpdatePeriodFrames: 2
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
            projection.directionalShadow.updatePeriodFrames,
            2
        )
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
    }

    func testDirectionalShadowFocusCoversASphereAroundTheCameraTarget()
        throws
    {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH directional shadow focus",
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
        // Range 0 leaves the frustum fit without a shadow; the focus spends the map on a sphere
        // of radius 6 around the camera target, moved 2 along the camera's flattened forward (+Z).
        let sun = try resources.createLight(.init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .down,
            color: .white,
            energy: 2,
            range: 0,
            castsShadow: true,
            shadowSoftness: 1.5,
            shadowFocus: .init(radius: 6, forwardShift: 2)
        ))
        let camera = RenderCamera(
            position: .init(x: 10, y: 8, z: -3),
            target: .init(x: 10, y: 0, z: 5),
            up: .up,
            verticalFOVDegrees: 45,
            projection: .perspective
        )
        let shadow = try resources.sceneLighting(
            environment: .default,
            camera: camera,
            aspect: 16 / 9
        ).directionalShadow
        XCTAssertTrue(shadow.enabled)
        XCTAssertEqual(shadow.softness, 1.5)

        func project(_ point: RenderVec3) -> RenderVec3 {
            let m = shadow.viewProjection
            return RenderVec3(
                x: m[0] * point.x + m[4] * point.y + m[8] * point.z + m[12],
                y: m[1] * point.x + m[5] * point.y + m[9] * point.z + m[13],
                z: m[2] * point.x + m[6] * point.y + m[10] * point.z + m[14]
            )
        }
        let center = RenderVec3(x: 10, y: 0, z: 7)
        // The centre snaps to whole texels of the 2048 map, two NDC units wide.
        let snap = 2 / GPUSceneDirectionalShadowProbe.primaryMapSize * 1.01
        let projectedCenter = project(center)
        XCTAssertLessThanOrEqual(abs(projectedCenter.x), snap)
        XCTAssertLessThanOrEqual(abs(projectedCenter.y), snap)
        let inside: [RenderVec3] = [
            .init(x: 5.9, y: 0, z: 0), .init(x: -5.9, y: 0, z: 0),
            .init(x: 0, y: 0, z: 5.9), .init(x: 0, y: 0, z: -5.9),
            .init(x: 0, y: 5.9, z: 0), .init(x: 0, y: -5.9, z: 0),
        ]
        for offset in inside {
            let point = project(center + offset)
            XCTAssertTrue(
                (-1...1).contains(point.x) && (-1...1).contains(point.y) &&
                    (0...1).contains(point.z),
                "\(offset) projects to \(point)"
            )
        }
        let outside: [RenderVec3] = [.init(x: 7.2, y: 0, z: 0), .init(x: 0, y: 0, z: -7.2)]
        for offset in outside {
            let point = project(center + offset)
            XCTAssertGreaterThan(max(abs(point.x), abs(point.y)), 1, "\(offset) projects to \(point)")
        }

        // A focus without a radius casts nothing rather than falling back to the frustum.
        try resources.updateLight(sun, descriptor: .init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .down,
            color: .white,
            energy: 2,
            range: 30,
            castsShadow: true,
            shadowSoftness: 1.5,
            shadowFocus: .init(radius: 0)
        ))
        XCTAssertFalse(try resources.sceneLighting(
            environment: .default,
            camera: camera,
            aspect: 16 / 9
        ).directionalShadow.enabled)
    }

    /// A spot's occluder shadow (the share of the sun, fill and sky its near occluder takes) reaches the
    /// scene lighting only while the spot casts its shadow, and out-of-range shares are refused.
    func testSpotOccluderShadowReachesTheSceneLightingWithItsShadow() throws {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH spot occluder shadow",
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
        func lamp(castsShadow: Bool, strength: Float, reach: Float) -> LightDescriptor {
            LightDescriptor(kind: .spot, active: true, position: .init(x: 0, y: 2, z: 0),
                direction: .init(x: 0, y: -1, z: 1), color: .white, energy: 1, range: 50,
                castsShadow: castsShadow, shadowSoftness: 4, occluderShadowStrength: strength,
                occluderShadowReach: reach)
        }
        let camera = RenderCamera(position: .init(x: 0, y: 6, z: -8), target: .zero, up: .up,
                                      verticalFOVDegrees: 60, projection: .perspective)
        let handle = try resources.createLight(lamp(castsShadow: true, strength: 0.55, reach: 2.8))
        var lighting = try resources.sceneLighting(environment: .default, camera: camera, aspect: 320 / 180)
        XCTAssertEqual(lighting.spotOccluderShadowStrength, 0.55)
        XCTAssertEqual(lighting.spotOccluderShadowReach, 2.8)

        try resources.updateLight(handle, descriptor: lamp(castsShadow: false, strength: 0.55, reach: 2.8))
        lighting = try resources.sceneLighting(environment: .default, camera: camera, aspect: 320 / 180)
        XCTAssertEqual(lighting.spotOccluderShadowStrength, 0, "no shadow, no occluder")
        XCTAssertEqual(lighting.spotOccluderShadowReach, 0)

        XCTAssertThrowsError(try resources.updateLight(handle, descriptor: lamp(castsShadow: true, strength: 1.5, reach: 2.8)))
        XCTAssertThrowsError(try resources.updateLight(handle, descriptor: lamp(castsShadow: true, strength: 0.5, reach: -1)))
    }

    func testSpotLightingProjectionPreservesLegacyDefaultsAndShadowMatrix()
        throws
    {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
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
            shadowSoftness: 4,
            shadowUpdatePeriodFrames: 3,
            indirectEnergy: 2
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
        XCTAssertEqual(projection.spotShadow.updatePeriodFrames, 3)
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
        XCTAssertFalse(projection.heroOmniShadow.enabled)
        XCTAssertEqual(projection.spotOccluderShadowStrength, 0, "the classic spot shadow by default")
        XCTAssertEqual(projection.spotOccluderShadowReach, 0)
        let post = try resources.scenePost(
            environment: .init(
                backgroundColor: .black,
                backgroundEnergy: 0,
                ambientColor: .black,
                ambientEnergy: 0,
                ambientMap: .invalid,
                skyCubemap: .invalid,
                ssaoEnabled: false,
                tonemap: .reinhard,
                tonemapWhite: 1,
                radianceCascades: .init(
                    tier: .high,
                    backend: .holographic
                )
            ),
            chain: .empty,
            antiAliasing: .off,
            upscaleMode: .linear
        )
        let indirectSource = try XCTUnwrap(
            post.radianceCascades.emitters.first
        )
        XCTAssertEqual(post.radianceCascades.backend, .holographic)
        XCTAssertEqual(indirectSource.direction, projection.spotDirection)
        XCTAssertEqual(indirectSource.radius, 0.5, accuracy: 0.000_001)
        XCTAssertEqual(
            indirectSource.height,
            projection.spotRange * 0.65,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            indirectSource.innerCutoff,
            GPUSceneLightingProbe.radianceSpotInnerCutoff,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            indirectSource.outerCutoff,
            GPUSceneLightingProbe.radianceSpotOuterCutoff,
            accuracy: 0.000_001
        )
        XCTAssertEqual(post.heroOmniLightSourceIndex, -1)
    }

    func testOmniLightingProjectsSixFaceHeroShadowForDirectAndIndirect()
        throws
    {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH hero omni shadow projection",
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
            kind: .omni,
            active: true,
            position: .init(x: 2, y: 3, z: -4),
            direction: .down,
            color: .init(r: 255, g: 160, b: 96, a: 255),
            energy: 4,
            range: 24,
            castsShadow: true,
            shadowSoftness: 3,
            shadowUpdatePeriodFrames: 2,
            indirectEnergy: 6
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

        XCTAssertEqual(projection.spotPosition, .init(x: 2, y: 3, z: -4))
        XCTAssertEqual(projection.spotInnerCutoff, -1)
        XCTAssertEqual(projection.spotOuterCutoff, -2)
        XCTAssertFalse(projection.spotShadow.enabled)
        XCTAssertTrue(projection.heroOmniShadow.enabled)
        XCTAssertEqual(projection.heroOmniShadow.range, 24)
        XCTAssertEqual(projection.heroOmniShadow.softness, 3)
        XCTAssertEqual(projection.heroOmniShadow.updatePeriodFrames, 2)
        XCTAssertEqual(
            projection.heroOmniShadow.viewProjections.count,
            GPUSceneOmniShadowProbe.faceCount * 16
        )
        XCTAssertTrue(
            projection.heroOmniShadow.viewProjections.allSatisfy(\.isFinite)
        )
        XCTAssertNotEqual(
            Array(projection.heroOmniShadow.viewProjections[0..<16]),
            Array(projection.heroOmniShadow.viewProjections[16..<32])
        )
        XCTAssertEqual(GPUSceneOmniShadowProbe.mapSize, 512)
        XCTAssertEqual(GPUSceneOmniShadowProbe.nearPlane, 0.05)

        let post = try resources.scenePost(
            environment: .init(
                backgroundColor: .black,
                backgroundEnergy: 0,
                ambientColor: .black,
                ambientEnergy: 0,
                ambientMap: .invalid,
                skyCubemap: .invalid,
                ssaoEnabled: false,
                tonemap: .reinhard,
                tonemapWhite: 1,
                radianceCascades: .init(tier: .high)
            ),
            chain: .empty,
            antiAliasing: .off,
            upscaleMode: .linear
        )
        XCTAssertEqual(post.heroOmniLightSourceIndex, 0)
        let source = try XCTUnwrap(post.radianceCascades.emitters.first)
        XCTAssertEqual(source.position, projection.spotPosition)
        XCTAssertEqual(source.energy, 6)
        XCTAssertEqual(source.innerCutoff, -1)
        XCTAssertEqual(source.outerCutoff, -2)
    }

    func testMaterialsThatDoNotCastShadowsStayOutOfShadowPasses() throws {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH material shadow casting",
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
        var descriptor = MaterialDescriptor.default
        descriptor.castsShadow = false
        let quiet = try resources.sceneMaterial(resources.createMaterial(descriptor))
        let ordinary = try resources.sceneMaterial(resources.createMaterial(.default))
        XCTAssertFalse(quiet.castsShadow)
        XCTAssertTrue(ordinary.castsShadow)

        let model = try resources.loadModel(
            path: packageRoot.appendingPathComponent("Tests/Fixtures/material-factor-triangle.gltf").path,
            flags: [.meshData]
        )
        let quietPatch = try resources.createMaterialOverride(
            from: model, materialIndex: 1, patch: .init(castsShadow: false)
        )
        let keptPatch = try resources.createMaterialOverride(
            from: model, materialIndex: 1, patch: .init(stencilReference: 3)
        )
        let quietEntry = try XCTUnwrap(resources.sceneModelMaterials(
            model: model, overrides: [.init(slot: 1, material: quietPatch)]
        ).first { $0.materialSlot == 1 }?.material)
        let keptEntry = try XCTUnwrap(resources.sceneModelMaterials(
            model: model, overrides: [.init(slot: 1, material: keptPatch)]
        ).first { $0.materialSlot == 1 }?.material)
        XCTAssertFalse(quietEntry.castsShadow)
        XCTAssertTrue(keptEntry.castsShadow, "an unset patch keeps the imported shadow casting")

        func batch(_ material: GPUSceneMaterialProbe?) -> GPUSceneGeometryProbeBatch {
            GPUSceneGeometryProbeBatch(
                geometry: .model(OpaquePointer(bitPattern: 1)!),
                animation: nil,
                material: material,
                materialSlot: 0,
                legacyInstanced: true,
                instanceBuffer: nil,
                firstInstance: 0,
                instanceCount: 1
            )
        }
        XCTAssertFalse(GPUSceneBatchShadowCulling.castsDirectionalShadow(batch(quiet)))
        XCTAssertFalse(GPUSceneBatchShadowCulling.castsSpotShadow(batch(quiet)))
        XCTAssertTrue(GPUSceneBatchShadowCulling.castsDirectionalShadow(batch(ordinary)))
        XCTAssertTrue(GPUSceneBatchShadowCulling.castsSpotShadow(batch(ordinary)))
        XCTAssertTrue(GPUSceneBatchShadowCulling.castsDirectionalShadow(batch(nil)))
    }

    func testSurfaceDetailReachesTheSceneMaterialAndShader() throws {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH surface detail",
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
        let grass = SurfaceDetail(pattern: .grass, albedoVariation: 0.2, bumpHeight: 0.004, frequency: 9)
        var descriptor = MaterialDescriptor.default
        descriptor.detail = grass
        XCTAssertEqual(try resources.sceneMaterial(resources.createMaterial(descriptor)).detail, grass)
        XCTAssertEqual(try resources.sceneMaterial(resources.createMaterial(.default)).detail, .none)

        let model = try resources.loadModel(
            path: packageRoot.appendingPathComponent("Tests/Fixtures/material-factor-triangle.gltf").path,
            flags: [.meshData]
        )
        let detailedPatch = try resources.createMaterialOverride(
            from: model, materialIndex: 1, patch: .init(detail: grass)
        )
        let keptPatch = try resources.createMaterialOverride(
            from: model, materialIndex: 1, patch: .init(stencilReference: 3)
        )
        let detailed = try XCTUnwrap(resources.sceneModelMaterials(
            model: model, overrides: [.init(slot: 1, material: detailedPatch)]
        ).first { $0.materialSlot == 1 }?.material)
        let kept = try XCTUnwrap(resources.sceneModelMaterials(
            model: model, overrides: [.init(slot: 1, material: keptPatch)]
        ).first { $0.materialSlot == 1 }?.material)
        XCTAssertEqual(detailed.detail, grass)
        XCTAssertEqual(kept.detail, .none, "an unset patch keeps the imported material's detail")

        let native = GPUSceneSurfaceDetailParameters.native(grass)
        XCTAssertEqual(native.0, 0.2)
        XCTAssertEqual(native.1, 0.004)
        XCTAssertEqual(native.2, 9)
        XCTAssertEqual(native.3, 2)
        let off = GPUSceneSurfaceDetailParameters.native(
            SurfaceDetail(pattern: .facade, albedoVariation: 0, bumpHeight: 0, frequency: 9)
        )
        XCTAssertEqual(off.0, 0)
        XCTAssertEqual(off.1, 0)
        XCTAssertEqual(off.2, 1)
        XCTAssertEqual(off.3, 0)
        // The native batch check bounds the pattern: glass (5) is the last one it accepts.
        let paving = GPUSceneSurfaceDetailParameters.native(
            SurfaceDetail(pattern: .paving, albedoVariation: 0.1, bumpHeight: 0.003, frequency: 10)
        )
        XCTAssertEqual(paving.3, 4)
        // Glass takes no bump: its second value is the share of panes lit at night, clamped to 0...1, and it
        // changes the reflection even without variation or lit panes.
        let glass = GPUSceneSurfaceDetailParameters.native(
            SurfaceDetail(pattern: .glass, albedoVariation: 0.1, bumpHeight: 0.5, frequency: 1.6, litShare: 0.3)
        )
        XCTAssertEqual(glass.0, 0.1)
        XCTAssertEqual(glass.1, 0.3)
        XCTAssertEqual(glass.2, 1.6)
        XCTAssertEqual(glass.3, 5)
        XCTAssertEqual(GPUSceneSurfaceDetailParameters.native(
            SurfaceDetail(pattern: .glass, albedoVariation: 0, bumpHeight: 0, frequency: 2, litShare: 4)
        ).1, 1)
        XCTAssertTrue(SurfaceDetail(pattern: .glass, albedoVariation: 0, bumpHeight: 0, frequency: 2).isEnabled)
        XCTAssertFalse(SurfaceDetail(pattern: .glass, albedoVariation: 0, bumpHeight: 0, frequency: 2,
                                     litShare: .nan).isEnabled)
        let nativeScene = try String(
            contentsOf: packageRoot.appendingPathComponent("src/gpu/btbh_gpu_scene.c"),
            encoding: .utf8
        )
        XCTAssertTrue(nativeScene.contains("batch->detail_parameters[3] <= 5.0f"))

        // The fragment uniform block carries the detail right after the procedural sky controls, the surface
        // wetness right after the detail and the street lamps after the wetness, in the shader and in the native
        // struct alike (both stop before the shadow block when shadows are off).
        let shader = try String(
            contentsOf: packageRoot.appendingPathComponent("shaders/gpu/production_model.frag.hlsl"),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        let scene = try String(
            contentsOf: packageRoot.appendingPathComponent("src/gpu/btbh_gpu_scene.c"),
            encoding: .utf8
        ).replacingOccurrences(of: "\r\n", with: "\n")
        XCTAssertTrue(shader.contains("float4 procedural_controls;\n"))
        XCTAssertTrue(shader.contains("    float4 detail_parameters;\n"))
        XCTAssertTrue(shader.contains("    float4 surface_wetness;\n"))
        XCTAssertTrue(shader.contains("    float4 lamp_color_energy[BTBH_MAX_LAMPS];\n#if BTBH_DIRECTIONAL_SHADOWS"))
        XCTAssertTrue(shader.contains("#define BTBH_MAX_LAMPS 32"))
        XCTAssertLessThan(try XCTUnwrap(shader.range(of: "float4 detail_parameters;")).lowerBound,
                          try XCTUnwrap(shader.range(of: "float4 surface_wetness;")).lowerBound)
        XCTAssertTrue(scene.contains(
            "    float procedural_controls[4];\n    float detail_parameters[4];\n"
        ))
        XCTAssertTrue(scene.contains(
            "    float surface_wetness[4];\n"
        ))
        XCTAssertTrue(scene.contains(
            "    float lamp_color_energy[BTBH_GPU_SCENE_MAX_LAMPS][4];\n    float directional_shadow_view_projection[16];"
        ))
        XCTAssertEqual(Int(BTBH_GPU_SCENE_MAX_LAMPS), LampLightDescriptor.maximumCount)
        // Street lamps light every lit pixel through the sun's BRDF, after the spot.
        XCTAssertTrue(shader.contains("const float3 lamp_lighting = evaluate_lamps("))
        // With the cascades on, the lamps add to their light (the emission target) instead of losing to it.
        XCTAssertTrue(shader.contains("emission + lamp_lighting,"))
        XCTAssertTrue(scene.contains("fragment_uniforms.lamp_controls[0] = (float)lamp_count;"))
        XCTAssertTrue(shader.contains(
            "const float3 detail_gradient = apply_surface_detail(input, albedo_sample, emission);"
        ))
        // The wetness is the environment's, clamped to 0...1, and only the ground and paving details take it.
        XCTAssertTrue(shader.contains("const float roughness = apply_surface_wetness("))
        XCTAssertTrue(shader.contains("(pattern != 1u && pattern != 3u && pattern != 4u)"))
        XCTAssertTrue(shader.contains("1.0f + 2.5f * saturate(surface_wetness.x)"))
        XCTAssertTrue(scene.contains("fragment_uniforms.surface_wetness[0] ="))
        // The lit windows' energy rides the wetness's second component; glass panes add their light to the
        // emission and reflect like coated glass.
        XCTAssertTrue(scene.contains("fragment_uniforms.surface_wetness[1] ="))
        XCTAssertTrue(shader.contains("emission += btbh_window_light(cell) * inside;"))
        XCTAssertTrue(shader.contains("? btbh_glass_reflectance : 0.0f"))
        func environment(wetness: Float) -> EnvironmentDescriptor {
            EnvironmentDescriptor(backgroundColor: .black, backgroundEnergy: 0, ambientColor: .white, ambientEnergy: 0,
                                  ambientMap: .invalid, skyCubemap: .invalid, ssaoEnabled: false, tonemap: .reinhard,
                                  tonemapWhite: 1, surfaceWetness: wetness)
        }
        XCTAssertEqual(environment(wetness: 0.4).surfaceWetness, 0.4)
        XCTAssertEqual(environment(wetness: 3).surfaceWetness, 1)
        XCTAssertEqual(environment(wetness: -1).surfaceWetness, 0)
        XCTAssertEqual(environment(wetness: .nan).surfaceWetness, 0)
        XCTAssertEqual(EnvironmentDescriptor.default.surfaceWetness, 0)
        // Lamps: the first `maximumCount`, reach and energy never negative.
        let lamp = LampLightDescriptor(position: .zero, reach: -1, color: .white, energy: .nan)
        XCTAssertEqual(lamp.reach, 0)
        XCTAssertEqual(lamp.energy, 0)
        let many = Array(repeating: LampLightDescriptor(position: .zero, reach: 2, color: .white, energy: 1), count: 40)
        let lit = EnvironmentDescriptor(backgroundColor: .black, backgroundEnergy: 0, ambientColor: .white,
                                        ambientEnergy: 0, ambientMap: .invalid, skyCubemap: .invalid, ssaoEnabled: false,
                                        tonemap: .reinhard, tonemapWhite: 1, lamps: many, lampCoreRadius: -2)
        XCTAssertEqual(lit.lamps.count, LampLightDescriptor.maximumCount)
        XCTAssertEqual(lit.lampCoreRadius, 0)
        XCTAssertTrue(EnvironmentDescriptor.default.lamps.isEmpty)
        // The window glow is never negative.
        func glowing(_ glow: Float) -> EnvironmentDescriptor {
            EnvironmentDescriptor(backgroundColor: .black, backgroundEnergy: 0, ambientColor: .white, ambientEnergy: 0,
                                  ambientMap: .invalid, skyCubemap: .invalid, ssaoEnabled: false, tonemap: .reinhard,
                                  tonemapWhite: 1, windowGlow: glow)
        }
        XCTAssertEqual(glowing(0.8).windowGlow, 0.8)
        XCTAssertEqual(glowing(-1).windowGlow, 0)
        XCTAssertEqual(glowing(.nan).windowGlow, 0)
        XCTAssertEqual(EnvironmentDescriptor.default.windowGlow, 0)
    }

    func testModelPatchCanFadeAnImportedMaterial() throws {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH model patch transparency",
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
        let model = try resources.loadModel(
            path: packageRoot.appendingPathComponent("Tests/Fixtures/material-factor-triangle.gltf").path,
            flags: [.meshData]
        )
        let fadedPatch = try resources.createMaterialOverride(
            from: model, materialIndex: 1, patch: .init(castsShadow: false, transparency: .alpha)
        )
        let keptPatch = try resources.createMaterialOverride(
            from: model, materialIndex: 1, patch: .init(stencilReference: 3)
        )
        let imported = try XCTUnwrap(resources.sceneModelMaterials(model: model, overrides: [])
            .first { $0.materialSlot == 1 }?.material)
        let faded = try XCTUnwrap(resources.sceneModelMaterials(
            model: model, overrides: [.init(slot: 1, material: fadedPatch)]
        ).first { $0.materialSlot == 1 }?.material)
        let kept = try XCTUnwrap(resources.sceneModelMaterials(
            model: model, overrides: [.init(slot: 1, material: keptPatch)]
        ).first { $0.materialSlot == 1 }?.material)
        XCTAssertEqual(imported.transparency, .disabled)
        XCTAssertEqual(faded.transparency, .alpha)
        XCTAssertFalse(faded.castsShadow)
        XCTAssertEqual(kept.transparency, imported.transparency, "an unset patch keeps the imported transparency")
    }

    func testDescriptorAndModelPatchPreservePBRMaterialChannels() throws {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
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
                    "Tests/Fixtures/placeholder.png"
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
        descriptor.uvScale = .init(x: 0.25, y: 0.5)
        descriptor.uvOffset = .init(x: 0.375, y: 0.125)
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
        XCTAssertEqual(standalone.uvScale, descriptor.uvScale)
        XCTAssertEqual(standalone.uvOffset, descriptor.uvOffset)
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
        var unrelatedDescriptor = descriptor
        unrelatedDescriptor.roughness = 0.12
        try resources.updateMaterial(
            standaloneHandle,
            descriptor: unrelatedDescriptor
        )
        _ = try resources.sceneModelMaterials(
            model: model,
            overrides: [.init(slot: 1, material: patchHandle)]
        )
        XCTAssertEqual(
            resources.sceneModelMaterialResolutionCount,
            1,
            "Updating an unrelated material must preserve cached projections"
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
    }

    func testGLTFImportPreservesPBRMaterialChannels() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
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
    }

    func testGLTFImportCanIgnoreMaterialTexturesWithoutLosingFactors() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
            title: "BTBH texture-free imported material",
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
                .path,
            flags: [.ignoreMaterialTextures]
        )
        defer { GPUWindowProbe.destroyModel(model.pointer) }

        let material = try XCTUnwrap(model.materials.indices.contains(1)
            ? model.materials[1]
            : nil)
        XCTAssertNil(material.albedoTexture)
        XCTAssertNil(material.normalTexture)
        XCTAssertNil(material.ormTexture)
        XCTAssertNil(material.emissionTexture)
        XCTAssertEqual(material.normalScale, 0.45, accuracy: 0.0001)
        XCTAssertEqual(material.occlusion, 0.66, accuracy: 0.0001)
        XCTAssertEqual(material.roughness, 0.31, accuracy: 0.0001)
        XCTAssertEqual(material.metalness, 0.72, accuracy: 0.0001)
        XCTAssertEqual(material.alphaCutoff, 0.42, accuracy: 0.0001)
    }

    func testAssimpOBJImportPreservesLegacyMaterialFactors() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
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
    }

    func testAssimpOBJImportExtractsConcaveFloorFootprint() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
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
    }

    func testFrameTelemetryFinishesPendingFencesAfterMeasuredPath() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
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
    }

    func testCustomMeshCABIRejectsMismatchedLayoutAndIndices() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
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
    }

    func testSceneDrawsFromPersistentInstanceBufferWithoutTransientUpload() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
            title: "BTBH persistent scene instance buffer",
            width: 320,
            height: 180,
            hidden: false,
            debugMode: true
        )
        defer { probe.shutdown() }
        probe.setFrameTelemetryEnabled(true)
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
        let shaderDirectory =
            ProcessInfo.processInfo.environment[
                "BTBH_GPU_TEST_SHADER_DIR"
            ].map { URL(fileURLWithPath: $0) } ??
            packageRoot.appendingPathComponent(
                platformShaderFallbackDirectory
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
        let shaderExtension = platformShaderExtension
        let shaderFormat = platformShaderFormat
        let present = { (cameraPosition: RenderVec3) in
            try probe.presentModelInstances(
                vertexShaderPath: shaderDirectory
                    .appendingPathComponent("production_model.vert.\(shaderExtension)")
                    .path,
                animatedVertexShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_animated_model.vert.\(shaderExtension)"
                    )
                    .path,
                fastShadowVertexShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_shadow_fast.vert.\(shaderExtension)"
                    )
                    .path,
                fastShadowFragmentShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_shadow_fast.frag.\(shaderExtension)"
                    )
                    .path,
                shadowVertexShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_shadow.vert.\(shaderExtension)"
                    )
                    .path,
                animatedShadowVertexShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_animated_shadow.vert.\(shaderExtension)"
                    )
                    .path,
                shadowFragmentShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_shadow.frag.\(shaderExtension)"
                    )
                    .path,
                fragmentShaderPath: shaderDirectory
                    .appendingPathComponent("production_model.frag.\(shaderExtension)")
                    .path,
                unshadowedFragmentShaderPath: shaderDirectory
                    .appendingPathComponent(
                        "production_model_unshadowed.frag.\(shaderExtension)"
                    )
                    .path,
                postVertexShaderPath: shaderDirectory
                    .appendingPathComponent("production_post.vert.\(shaderExtension)")
                    .path,
                postFragmentShaderPath: shaderDirectory
                    .appendingPathComponent("production_post.frag.\(shaderExtension)")
                    .path,
                shaderFormat: shaderFormat,
                renderSize: .init(width: 320, height: 180),
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
        XCTAssertGreaterThanOrEqual(
            report.cpuPrepareSetupMilliseconds,
            0
        )
        XCTAssertGreaterThanOrEqual(
            report.cpuPrepareBonesMilliseconds,
            0
        )
        XCTAssertGreaterThanOrEqual(
            report.cpuPrepareBoneCountMilliseconds,
            0
        )
        XCTAssertGreaterThanOrEqual(
            report.cpuPrepareBoneAllocateMilliseconds,
            0
        )
        XCTAssertGreaterThanOrEqual(
            report.cpuPrepareBoneSampleMilliseconds,
            0
        )
        XCTAssertLessThanOrEqual(
            report.cpuPrepareBoneCountMilliseconds +
                report.cpuPrepareBoneAllocateMilliseconds +
                report.cpuPrepareBoneSampleMilliseconds,
            report.cpuPrepareBonesMilliseconds + 0.05
        )
        XCTAssertGreaterThan(report.preparedGeometryCount, 0)
        XCTAssertGreaterThanOrEqual(report.animatedGeometryCount, 0)
        XCTAssertGreaterThanOrEqual(report.animationSampleCount, 0)
        XCTAssertLessThanOrEqual(
            report.animationSampleCount,
            report.animatedGeometryCount
        )
        XCTAssertGreaterThanOrEqual(report.boneMatrixCount, 0)
        XCTAssertGreaterThanOrEqual(
            report.cpuPreparePipelinesMilliseconds,
            0
        )
        XCTAssertGreaterThanOrEqual(
            report.cpuPrepareBuffersMilliseconds,
            0
        )
        XCTAssertGreaterThan(
            report.cpuPrepareSetupMilliseconds +
                report.cpuPrepareBonesMilliseconds +
                report.cpuPreparePipelinesMilliseconds +
                report.cpuPrepareBuffersMilliseconds,
            0
        )
        XCTAssertLessThanOrEqual(
            report.cpuPrepareSetupMilliseconds +
                report.cpuPrepareBonesMilliseconds +
                report.cpuPreparePipelinesMilliseconds +
                report.cpuPrepareBuffersMilliseconds,
            report.cpuPrepareMilliseconds + 0.05
        )
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
                .appendingPathComponent("production_canvas.vert.\(platformShaderExtension)")
                .path,
            fragmentShaderPath: shaderDirectory
                .appendingPathComponent("production_canvas.frag.\(platformShaderExtension)")
                .path,
            texturedFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_canvas_textured.frag.\(platformShaderExtension)"
                )
                .path,
            shaderFormat: platformShaderFormat,
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
    }

    func testPlatformProbePresentsResizesAndRestoresARealGPUWindow() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
            title: "BTBH production GPU package probe",
            width: 640,
            height: 360,
            hidden: false,
            debugMode: true
        )
        XCTAssertEqual(
            probe.deviceDiagnostics.actualDriver,
            GPUBackendRequest.platformDefault.driverName
        )
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
    }

    func testMaterialSlotBatchesPrepareOnlyDrawnModelPrimitives() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
            title: "BTBH material-slot preparation test",
            width: 320,
            height: 180,
            hidden: false,
            debugMode: true
        )
        defer { probe.shutdown() }
        probe.setFrameTelemetryEnabled(true)
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
        let shaderDirectory =
            ProcessInfo.processInfo.environment[
                "BTBH_GPU_TEST_SHADER_DIR"
            ].map { URL(fileURLWithPath: $0) } ??
            packageRoot.appendingPathComponent(
                platformShaderFallbackDirectory
            )
        let instances = [
            GPUInstanceRecordPacking.record(
                matrix: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            ),
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
                .appendingPathComponent("production_model.vert.\(platformShaderExtension)")
                .path,
            animatedVertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_animated_model.vert.\(platformShaderExtension)"
                )
                .path,
            fastShadowVertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_shadow_fast.vert.\(platformShaderExtension)"
                )
                .path,
            fastShadowFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_shadow_fast.frag.\(platformShaderExtension)"
                )
                .path,
            shadowVertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_shadow.vert.\(platformShaderExtension)"
                )
                .path,
            animatedShadowVertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_animated_shadow.vert.\(platformShaderExtension)"
                )
                .path,
            shadowFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_shadow.frag.\(platformShaderExtension)"
                )
                .path,
            fragmentShaderPath: shaderDirectory
                .appendingPathComponent("production_model.frag.\(platformShaderExtension)")
                .path,
            unshadowedFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_model_unshadowed.frag.\(platformShaderExtension)"
                )
                .path,
            postVertexShaderPath: shaderDirectory
                .appendingPathComponent("production_post.vert.\(platformShaderExtension)")
                .path,
            postFragmentShaderPath: shaderDirectory
                .appendingPathComponent("production_post.frag.\(platformShaderExtension)")
                .path,
            shaderFormat: platformShaderFormat,
            renderSize: .init(width: 320, height: 180),
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
                    updatePeriodFrames: 2,
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
        XCTAssertGreaterThan(report.shadowPipelineBindSkipCount, 0)
        XCTAssertGreaterThan(report.geometryPipelineBindSkipCount, 0)
        try probe.presentCanvas(
            vertexShaderPath: shaderDirectory
                .appendingPathComponent("production_canvas.vert.\(platformShaderExtension)")
                .path,
            fragmentShaderPath: shaderDirectory
                .appendingPathComponent("production_canvas.frag.\(platformShaderExtension)")
                .path,
            texturedFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_canvas_textured.frag.\(platformShaderExtension)"
                )
                .path,
            shaderFormat: platformShaderFormat,
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
    }

    func testRadianceCascadesOffLowResizeAndMissingShaderFallback() throws {
        let probe = try GPUWindowProbe(
            request: .platformDefault,
            title: "BTBH Radiance Cascades runtime",
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
        let shaderDirectory =
            ProcessInfo.processInfo.environment[
                "BTBH_GPU_TEST_SHADER_DIR"
            ].map { URL(fileURLWithPath: $0) } ??
            packageRoot.appendingPathComponent(
                platformShaderFallbackDirectory
            )
        let model = try probe.loadModel(
            path: packageRoot.appendingPathComponent(
                "Tests/Fixtures/instanced-transformed-default-material.gltf"
            ).path
        )
        defer { GPUWindowProbe.destroyModel(model.pointer) }
        let instances = [GPUInstanceRecordPacking.record(
            matrix: .identity,
            color: .white,
            custom: .init(x: 0, y: 0, z: 0, w: 0)
        )]
        let batches = [GPUSceneGeometryProbeBatch(
            geometry: .model(model.pointer),
            animation: nil,
            material: model.materials.first,
            materialSlot: nil,
            firstInstance: 0,
            instanceCount: 1
        )]
        let present = {
            (directory: URL, size: PixelSize2D,
             radianceCascades: RadianceCascadesDescriptor) in
            try probe.presentModelInstances(
                vertexShaderPath: directory.appendingPathComponent(
                    "production_model.vert.\(self.platformShaderExtension)"
                ).path,
                animatedVertexShaderPath: directory.appendingPathComponent(
                    "production_animated_model.vert.\(self.platformShaderExtension)"
                ).path,
                fastShadowVertexShaderPath: directory.appendingPathComponent(
                    "production_shadow_fast.vert.\(self.platformShaderExtension)"
                ).path,
                fastShadowFragmentShaderPath: directory.appendingPathComponent(
                    "production_shadow_fast.frag.\(self.platformShaderExtension)"
                ).path,
                shadowVertexShaderPath: directory.appendingPathComponent(
                    "production_shadow.vert.\(self.platformShaderExtension)"
                ).path,
                animatedShadowVertexShaderPath: directory.appendingPathComponent(
                    "production_animated_shadow.vert.\(self.platformShaderExtension)"
                ).path,
                shadowFragmentShaderPath: directory.appendingPathComponent(
                    "production_shadow.frag.\(self.platformShaderExtension)"
                ).path,
                fragmentShaderPath: directory.appendingPathComponent(
                    "production_model.frag.\(self.platformShaderExtension)"
                ).path,
                unshadowedFragmentShaderPath: directory.appendingPathComponent(
                    "production_model_unshadowed.frag.\(self.platformShaderExtension)"
                ).path,
                postVertexShaderPath: directory.appendingPathComponent(
                    "production_post.vert.\(self.platformShaderExtension)"
                ).path,
                postFragmentShaderPath: directory.appendingPathComponent(
                    "production_post.frag.\(self.platformShaderExtension)"
                ).path,
                shaderFormat: self.platformShaderFormat,
                renderSize: size,
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
                    ambientEnergy: 0.25,
                    directionalDirection: .down,
                    directionalColor: .white,
                    directionalEnergy: 1,
                    secondaryDirectionalDirection: .down,
                    secondaryDirectionalColor: .white,
                    secondaryDirectionalEnergy: 0
                ),
                post: GPUScenePostProbe.default.withRadianceCascades(
                    radianceCascades
                ),
                instances: instances,
                batches: batches,
                clearColor: .black
            )
        }
        let finishFrame = {
            try probe.presentCanvas(
                vertexShaderPath: shaderDirectory.appendingPathComponent(
                    "production_canvas.vert.\(self.platformShaderExtension)"
                ).path,
                fragmentShaderPath: shaderDirectory.appendingPathComponent(
                    "production_canvas.frag.\(self.platformShaderExtension)"
                ).path,
                texturedFragmentShaderPath: shaderDirectory.appendingPathComponent(
                    "production_canvas_textured.frag.\(self.platformShaderExtension)"
                ).path,
                shaderFormat: self.platformShaderFormat,
                vertices: [],
                batches: [],
                passes: [.init(
                    firstBatch: 0,
                    batchCount: 0,
                    target: nil,
                    clearColor: nil,
                    screenClearToken: nil
                )]
            )
        }

        let offReport = try present(
            shaderDirectory,
            .init(width: 320, height: 180),
            .off
        )
        XCTAssertEqual(offReport.radianceCascadesStatus.activeTier, .off)
        XCTAssertEqual(
            offReport.radianceCascadesStatus.fallbackReason,
            .disabled
        )
        XCTAssertEqual(
            offReport.radianceCascadesStatus.internalSize,
            .init(width: 0, height: 0)
        )
        XCTAssertEqual(
            offReport.radianceCascadesStatus.cpuEncodeMilliseconds,
            0
        )
        try finishFrame()

        let temporaryDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
        try FileManager.default.copyItem(
            at: shaderDirectory,
            to: temporaryDirectory
        )
        defer { try? FileManager.default.removeItem(at: temporaryDirectory) }
        try FileManager.default.removeItem(
            at: temporaryDirectory.appendingPathComponent(
                "production_hrc_merge_up.comp.\(platformShaderExtension)"
            )
        )
        let fallbackReport = try present(
            temporaryDirectory,
            .init(width: 256, height: 144),
            .init(
                tier: .low,
                debugView: .fallbackReason,
                invalidate: true
            )
        )
        XCTAssertEqual(fallbackReport.radianceCascadesStatus.activeTier, .off)
        XCTAssertEqual(
            fallbackReport.radianceCascadesStatus.fallbackReason,
            .shaderUnavailable
        )
        XCTAssertGreaterThan(
            fallbackReport.radianceCascadesStatus.cpuEncodeMilliseconds,
            0
        )
        try finishFrame()

        let lowReport = try present(
            shaderDirectory,
            .init(width: 320, height: 180),
            .init(tier: .low, invalidate: true)
        )
        XCTAssertEqual(lowReport.radianceCascadesStatus.activeTier, .low)
        XCTAssertEqual(lowReport.radianceCascadesStatus.fallbackReason, .none)
        XCTAssertEqual(
            lowReport.radianceCascadesStatus.internalSize,
            .init(width: 80, height: 48)
        )
        XCTAssertGreaterThan(
            lowReport.radianceCascadesStatus.cpuEncodeMilliseconds,
            0
        )
        try finishFrame()

        let hrcField = RadianceCascadesOcclusionCascade(
            originWorldXZ: .init(x: -128, y: -128),
            cellSize: 1,
            width: 256,
            height: 256,
            revision: 1,
            samples: Array(repeating: [1_000 as Float, 0], count: 256 * 256)
                .flatMap { $0 }
        )
        let holographicReport = try present(
            shaderDirectory,
            .init(width: 320, height: 180),
            .init(
                tier: .low,
                backend: .holographic,
                occlusionCascades: [hrcField],
                invalidate: true
            )
        )
        XCTAssertEqual(
            holographicReport.radianceCascadesStatus.requestedBackend,
            .holographic
        )
        XCTAssertEqual(
            holographicReport.radianceCascadesStatus.activeBackend,
            .holographic
        )
        XCTAssertEqual(
            holographicReport.radianceCascadesStatus.fallbackReason,
            .none
        )
        XCTAssertGreaterThan(
            holographicReport.radianceCascadesStatus.cpuEncodeMilliseconds,
            0
        )
        let hrcPasses =
            holographicReport.radianceCascadesStatus.passTelemetry
        XCTAssertGreaterThanOrEqual(hrcPasses.cpuPrepareMilliseconds, 0)
        XCTAssertGreaterThanOrEqual(
            hrcPasses.cpuPipelinePrepareMilliseconds,
            0
        )
        XCTAssertGreaterThanOrEqual(
            hrcPasses.cpuResourcePrepareMilliseconds,
            0
        )
        XCTAssertGreaterThan(hrcPasses.traceDispatchCount, 0)
        XCTAssertGreaterThan(hrcPasses.mergeUpDispatchCount, 0)
        XCTAssertGreaterThan(hrcPasses.mergeDownDispatchCount, 0)
        XCTAssertEqual(hrcPasses.finishDispatchCount, 1)
        XCTAssertEqual(hrcPasses.reprojectDispatchCount, 1)
        XCTAssertEqual(hrcPasses.blurDispatchCount, 0)
        XCTAssertEqual(hrcPasses.resolveDispatchCount, 1)
        XCTAssertEqual(hrcPasses.reconstructDispatchCount, 1)
        XCTAssertEqual(hrcPasses.composeDrawCount, 1)
        try finishFrame()

        let resizedReport = try present(
            shaderDirectory,
            .init(width: 256, height: 144),
            .init(tier: .low)
        )
        XCTAssertEqual(
            resizedReport.radianceCascadesStatus.internalSize,
            .init(width: 64, height: 40)
        )
        XCTAssertGreaterThan(
            resizedReport.radianceCascadesStatus.generation,
            lowReport.radianceCascadesStatus.generation
        )
        try finishFrame()

        let invalidConfigurationReport = try present(
            shaderDirectory,
            .init(width: 256, height: 144),
            .init(tier: .low, indirectIntensity: 2.1)
        )
        XCTAssertEqual(
            invalidConfigurationReport.radianceCascadesStatus.activeTier,
            .off
        )
        XCTAssertEqual(
            invalidConfigurationReport.radianceCascadesStatus.fallbackReason,
            .invalidConfiguration
        )
        try finishFrame()

        let recoveredConfigurationReport = try present(
            shaderDirectory,
            .init(width: 256, height: 144),
            .init(tier: .low)
        )
        XCTAssertEqual(
            recoveredConfigurationReport.radianceCascadesStatus.activeTier,
            .low
        )
        XCTAssertEqual(
            recoveredConfigurationReport.radianceCascadesStatus.fallbackReason,
            .none
        )
        XCTAssertGreaterThan(
            recoveredConfigurationReport.radianceCascadesStatus.generation,
            invalidConfigurationReport.radianceCascadesStatus.generation
        )
        try finishFrame()

        let validEmitter = RadianceEmitter(
            position: .zero,
            color: .white,
            energy: 1,
            radius: 1
        )
        let manyEmitters = Array(repeating: validEmitter, count: 10_000)
        let manyEmitterReport = try present(
            shaderDirectory,
            .init(width: 256, height: 144),
            .init(tier: .low, emitters: manyEmitters)
        )
        XCTAssertEqual(
            manyEmitterReport.radianceCascadesStatus.activeTier,
            .low
        )
        XCTAssertEqual(
            manyEmitterReport.radianceCascadesStatus.fallbackReason,
            .none
        )
        try finishFrame()

        var automaticReport = resizedReport
        for _ in 0..<24 {
            automaticReport = try present(
                shaderDirectory,
                .init(width: 256, height: 144),
                .init(
                    tier: .auto,
                    autoFrameBudgetMilliseconds: 1_000
                )
            )
            try finishFrame()
        }
        XCTAssertEqual(automaticReport.radianceCascadesStatus.activeTier, .low)
        XCTAssertEqual(automaticReport.radianceCascadesStatus.pendingTier, .high)
        XCTAssertGreaterThanOrEqual(
            automaticReport.radianceCascadesStatus.sampleCount,
            16
        )
        let transitionedReport = try present(
            shaderDirectory,
            .init(width: 256, height: 144),
            .init(
                tier: .auto,
                autoFrameBudgetMilliseconds: 1_000,
                allowAutoTransition: true
            )
        )
        XCTAssertEqual(
            transitionedReport.radianceCascadesStatus.activeTier,
            .high
        )
        XCTAssertEqual(
            transitionedReport.radianceCascadesStatus.internalSize,
            .init(width: 256, height: 144)
        )
        try finishFrame()
    }
}
