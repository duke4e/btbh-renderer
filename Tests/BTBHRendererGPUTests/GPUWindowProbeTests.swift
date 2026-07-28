import XCTest
import BTBHRenderer
import BTBHRendererGPUCore
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUWindowProbeTests: XCTestCase {
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
        let standaloneHandle = try resources.createMaterial(descriptor)
        let standalone = try resources.sceneMaterial(standaloneHandle)

        XCTAssertEqual(standalone.normalTexture, texturePointer)
        XCTAssertEqual(standalone.ormTexture, texturePointer)
        XCTAssertEqual(standalone.normalScale, 0.62, accuracy: 0.0001)
        XCTAssertEqual(standalone.occlusion, 0.83, accuracy: 0.0001)
        XCTAssertEqual(standalone.roughness, 0.27, accuracy: 0.0001)
        XCTAssertEqual(standalone.metalness, 0.91, accuracy: 0.0001)

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
                ormTexture: texture
            )
        )
        let patchedEntries = try resources.sceneModelMaterials(
            model: model,
            overrides: [.init(slot: 1, material: patchHandle)]
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

        resources.destroyMaterial(patchHandle)
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
