import XCTest
import BTBHRendererGPUCore
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUWindowProbeTests: XCTestCase {
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
            fragmentShaderPath: shaderDirectory
                .appendingPathComponent("production_model.frag.spv")
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
            lighting: .init(
                ambientColor: .white,
                ambientEnergy: 1,
                directionalDirection: .down,
                directionalColor: .white,
                directionalEnergy: 0,
                secondaryDirectionalDirection: .down,
                secondaryDirectionalColor: .white,
                secondaryDirectionalEnergy: 0
            ),
            instances: instances,
            batches: batches,
            clearColor: .black
        )
        XCTAssertEqual(report.primitiveDrawCount, 4)
        XCTAssertEqual(report.preparedGeometryCount, 4)
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
