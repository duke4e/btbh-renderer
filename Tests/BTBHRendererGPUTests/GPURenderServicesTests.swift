import BTBHRenderer
import Foundation
import XCTest
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPURenderServicesTests: XCTestCase {
    func testSceneRenderResolutionMatchesLegacyRenderScaleContract() throws {
        XCTAssertEqual(
            try GPUSceneRenderResolution.scaled(
                drawableSize: .init(width: 2_560, height: 1_440),
                renderScale: 0.5
            ),
            .init(width: 1_280, height: 720)
        )
        XCTAssertEqual(
            try GPUSceneRenderResolution.scaled(
                drawableSize: .init(width: 101, height: 51),
                renderScale: 0.5
            ),
            .init(width: 50, height: 25)
        )
        XCTAssertEqual(
            try GPUSceneRenderResolution.scaled(
                drawableSize: .init(width: 2_560, height: 1_440),
                renderScale: 0.01
            ),
            .init(width: 256, height: 144)
        )
        XCTAssertEqual(
            try GPUSceneRenderResolution.scaled(
                drawableSize: .init(width: 2_560, height: 1_440),
                renderScale: 3
            ),
            .init(width: 5_120, height: 2_880)
        )
    }

    func testSceneRenderResolutionRejectsInvalidInput() {
        let drawableSize = PixelSize2D(width: 2_560, height: 1_440)
        for renderScale in [
            Float.zero,
            -1,
            .nan,
            .infinity,
        ] {
            XCTAssertThrowsError(
                try GPUSceneRenderResolution.scaled(
                    drawableSize: drawableSize,
                    renderScale: renderScale
                )
            )
        }
        for invalidSize in [
            PixelSize2D(width: 0, height: 720),
            PixelSize2D(width: 1_280, height: 0),
            PixelSize2D(width: Int(Int32.max) + 1, height: 720),
        ] {
            XCTAssertThrowsError(
                try GPUSceneRenderResolution.scaled(
                    drawableSize: invalidSize,
                    renderScale: 1
                )
            )
        }
    }

    func testProceduralEnvironmentProjectionIsFiniteAndPreservesConstantSky() {
        let descriptor = ProceduralSkyDescriptor(
            color: .init(r: 64, g: 128, b: 192, a: 255),
            resolution: 32
        )

        let projection =
            GPUProceduralEnvironmentProjection.project(descriptor)
        let repeatedProjection =
            GPUProceduralEnvironmentProjection.project(descriptor)

        XCTAssertEqual(projection.irradianceSH.count, 9)
        XCTAssertEqual(
            projection.irradianceSH,
            repeatedProjection.irradianceSH
        )
        XCTAssertTrue(projection.irradianceSH.allSatisfy {
            $0.x.isFinite && $0.y.isFinite && $0.z.isFinite
        })
        for direction in [
            RenderVec3.up,
            RenderVec3.down,
            RenderVec3.forward,
            RenderVec3.right,
        ] {
            let irradiance = projection.irradiance(at: direction)
            XCTAssertEqual(
                irradiance.x,
                GPUColorSpace.linearComponent(descriptor.color.r),
                accuracy: 0.015
            )
            XCTAssertEqual(
                irradiance.y,
                GPUColorSpace.linearComponent(descriptor.color.g),
                accuracy: 0.015
            )
            XCTAssertEqual(
                irradiance.z,
                GPUColorSpace.linearComponent(descriptor.color.b),
                accuracy: 0.015
            )
        }
    }

    func testProceduralEnvironmentProjectionRetainsDirectionalColor() {
        let descriptor = ProceduralSkyDescriptor(
            resolution: 32,
            groundBottomColor: .init(r: 0, g: 16, b: 255, a: 255),
            groundEnergy: 1,
            groundHorizonColor: .black,
            groundHorizonCurve: 0.2,
            skyEnergy: 1,
            skyHorizonColor: .black,
            skyHorizonCurve: 0.2,
            skyTopColor: .init(r: 255, g: 16, b: 0, a: 255),
            sunColor: .white,
            sunCurve: 0.15,
            sunDirection: .down,
            sunEnergy: 0,
            sunSizeRadians: 0
        )

        let projection =
            GPUProceduralEnvironmentProjection.project(descriptor)
        let above = projection.irradiance(at: .up)
        let below = projection.irradiance(at: .down)

        XCTAssertGreaterThan(above.x, above.z)
        XCTAssertGreaterThan(below.z, below.x)
        XCTAssertNotEqual(above, below)
    }

    func testProceduralEnvironmentProjectionUsesLegacyNegatedSunDirection() {
        let descriptor = ProceduralSkyDescriptor(
            resolution: 64,
            groundBottomColor: .black,
            groundEnergy: 0,
            groundHorizonColor: .black,
            groundHorizonCurve: 0.2,
            skyEnergy: 0,
            skyHorizonColor: .black,
            skyHorizonCurve: 0.2,
            skyTopColor: .black,
            sunColor: .init(r: 255, g: 32, b: 16, a: 255),
            sunCurve: 0.15,
            sunDirection: .down,
            sunEnergy: 4,
            sunSizeRadians: 0.05
        )

        let projection =
            GPUProceduralEnvironmentProjection.project(descriptor)
        let authoredDirection =
            projection.irradiance(at: .down)
        let legacyNegatedDirection =
            projection.irradiance(at: .up)

        XCTAssertGreaterThan(
            legacyNegatedDirection.x,
            authoredDirection.x * 2
        )
    }

    func testSemanticInstancePackingNormalizesColorAndPreservesCustomValues() {
        var packed: [Float] = []
        GPUInstanceRecordPacking.append(
            .init(
                transform: .identity,
                color: .init(r: 0, g: 127, b: 255, a: 64),
                custom: .init(x: 11, y: 12, z: 13, w: 14)
            ),
            to: &packed
        )

        XCTAssertEqual(packed.count, 24)
        XCTAssertEqual(Array(packed[0..<16]), [
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
        ])
        XCTAssertEqual(packed[16], 0)
        XCTAssertEqual(
            packed[17],
            Float(127) / 255,
            accuracy: 0.000_001
        )
        XCTAssertEqual(packed[18], 1)
        XCTAssertEqual(
            packed[19],
            Float(64) / 255,
            accuracy: 0.000_001
        )
        XCTAssertEqual(Array(packed[20..<24]), [11, 12, 13, 14])
    }

    func testSemanticInstanceRecordMatchesNativeTwentyFourFloatLayout() {
        let record = GPUInstanceRecordPacking.record(
            matrix: .identity,
            color: .init(r: 0, g: 127, b: 255, a: 64),
            custom: .init(x: 11, y: 12, z: 13, w: 14)
        )

        XCTAssertEqual(
            MemoryLayout<GPUInstanceRecord>.stride,
            GPUInstanceRecordPacking.byteStride
        )
        XCTAssertEqual(record.matrix0, SIMD4(1, 0, 0, 0))
        XCTAssertEqual(record.matrix3, SIMD4(0, 0, 0, 1))
        XCTAssertEqual(record.color.y, Float(127) / 255, accuracy: 0.000_001)
        XCTAssertEqual(record.custom, SIMD4(11, 12, 13, 14))
    }

    func testTransformStreamsPackDirectlyIntoNativeInstanceRecords() {
        var records: [GPUInstanceRecord] = []
        let positions = [
            RenderVec3(x: 1, y: 2, z: 3),
            RenderVec3(x: 4, y: 5, z: 6),
        ]
        let rotations = [RenderQuaternion.identity, .identity]
        let scales = [
            RenderVec3(x: 1, y: 1, z: 1),
            RenderVec3(x: 2, y: 3, z: 4),
        ]
        let colors = [
            RenderColor.white,
            RenderColor(r: 0, g: 127, b: 255, a: 64),
        ]
        let customs = [
            RenderVec4(x: 0, y: 0, z: 0, w: 0),
            RenderVec4(x: 11, y: 12, z: 13, w: 14),
        ]
        XCTAssertTrue(GPUInstanceRecordPacking.appendTransformStreams(
            positions: positions,
            rotations: rotations,
            scales: scales,
            colors: colors,
            customs: customs,
            sourceStart: 0,
            count: 2,
            to: &records
        ))

        XCTAssertEqual(records.count, 2)
        XCTAssertEqual(records[0].matrix3, SIMD4(1, 2, 3, 1))
        XCTAssertEqual(records[1].matrix0, SIMD4(2, 0, 0, 0))
        XCTAssertEqual(records[1].matrix1, SIMD4(0, 3, 0, 0))
        XCTAssertEqual(records[1].matrix2, SIMD4(0, 0, 4, 0))
        XCTAssertEqual(records[1].color.y, Float(127) / 255, accuracy: 0.000_001)
        XCTAssertEqual(records[1].custom, SIMD4(11, 12, 13, 14))
    }

    func testSceneSubmissionScratchRetainsWarmCapacityAcrossFrames() {
        let scratch = GPUSceneSubmissionScratch()
        scratch.packedInstances.reserveCapacity(4)
        scratch.batches.reserveCapacity(8)
        scratch.packedInstances.append(
            GPUInstanceRecordPacking.record(
                matrix: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )
        )
        scratch.batches.append(.init(
            geometry: .customMesh(OpaquePointer(bitPattern: 1)!),
            animation: nil,
            material: nil,
            materialSlot: nil,
            firstInstance: 0,
            instanceCount: 1
        ))

        let instanceCapacity = scratch.packedInstances.capacity
        let batchCapacity = scratch.batches.capacity
        var packedInstances = scratch.takePackedInstances()
        var batches = scratch.takeBatches()
        XCTAssertTrue(scratch.packedInstances.isEmpty)
        XCTAssertTrue(scratch.batches.isEmpty)
        scratch.reclaim(
            packedInstances: &packedInstances,
            batches: &batches
        )
        scratch.reset()

        XCTAssertTrue(scratch.packedInstances.isEmpty)
        XCTAssertTrue(scratch.batches.isEmpty)
        XCTAssertGreaterThanOrEqual(
            scratch.packedInstances.capacity,
            instanceCapacity
        )
        XCTAssertGreaterThanOrEqual(scratch.batches.capacity, batchCapacity)

        scratch.release()

        XCTAssertEqual(scratch.packedInstances.capacity, 0)
        XCTAssertEqual(scratch.batches.capacity, 0)
    }

    func testBundleInstallsPresentsClearAndShutsDownExactlyOnce() throws {
        RenderServices.uninstall()
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)

        let access = RenderServices.access
        try access.platform.openWindow(.init(
            title: "BTBH GPU bundle test",
            width: 640,
            height: 360,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        access.platform.pollEvents()
        _ = access.platform.beginFrame()
        access.canvas.beginFrame()
        access.canvas.submit(.clear(.init(
            r: 12,
            g: 24,
            b: 48,
            a: 255
        )))
        access.canvas.endFrame()
        XCTAssertFalse(access.platform.shouldClose)

        RenderServices.uninstall()
        RenderServices.uninstall()
        XCTAssertTrue(bundle.isShutdown)
    }

    func testTextureLoadCreatesARealGPUResourceAndDestroyKeepsWindowAlive() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU texture resource test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let handle = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path,
            generateMipmaps: true,
            filter: .trilinear,
            wrap: .repeatMode
        ))
        XCTAssertTrue(handle.isValid)
        RenderServices.resources.destroyTexture(handle)
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testTextureHandleFinishesSafelyAfterOwnerWindowClosesAndReopens() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        let descriptor = WindowDescriptor(
            title: "BTBH GPU texture owner test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        )
        try RenderServices.platform.openWindow(descriptor)
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let handle = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path
        ))

        RenderServices.platform.closeWindow()
        try RenderServices.platform.openWindow(descriptor)
        RenderServices.resources.destroyTexture(handle)
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.canvas.endFrame()

        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testTextureWrapperFinishesSafelyAfterOwnerWindowCloses() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU closed texture owner test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let handle = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path
        ))

        RenderServices.platform.closeWindow()
        RenderServices.resources.destroyTexture(handle)
        RenderServices.uninstall()

        XCTAssertTrue(bundle.isShutdown)
    }

    func testUnsupportedResourceCallFailsClosedWithExactOperation() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU unsupported resource test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let retainedTexture = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path
        ))
        XCTAssertTrue(retainedTexture.isValid)

        XCTAssertThrowsError(try RenderServices.resources.createTexture(
            .init(
                size: .init(width: 1, height: 1),
                mipCount: 1,
                format: .rgba8,
                usage: .sampled,
                filter: .bilinear,
                wrap: .clamp
            ),
            bytes: nil
        )) { error in
            XCTAssertEqual(
                error as? GPUBackendUnsupportedError,
                .operation("resources.createTexture(_:bytes:)")
            )
        }
        XCTAssertTrue(RenderServices.platform.shouldClose)
        RenderServices.uninstall()
        XCTAssertTrue(bundle.isShutdown)
    }

    func testScreenShaderDescriptorStagesTypedParametersAndTextureDependency() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU staged screen shader test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let texture = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path
        ))
        let shader = try RenderServices.resources.createScreenShader(.init(
            fragmentPath: packageRoot
                .appendingPathComponent("shaders/post/output.frag")
                .path,
            parameters: [
                "u_texture": .texture(texture),
                "sharpness": .float(0.5)
            ]
        ))
        XCTAssertTrue(shader.isValid)
        RenderServices.resources.destroyTexture(texture)
        try RenderServices.resources.updateScreenShader(
            shader,
            parameter: "sharpness",
            value: .float(0.8)
        )

        RenderServices.resources.destroyScreenShader(shader)
        XCTAssertThrowsError(
            try RenderServices.resources.createScreenShader(.init(
                fragmentPath: packageRoot
                    .appendingPathComponent("shaders/post/output.frag")
                    .path,
                parameters: ["u_texture": .texture(texture)]
            ))
        ) { error in
            XCTAssertEqual(
                error as? GPUResourceStagingError,
                .invalidTexture(texture.rawValue)
            )
        }
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testSurfaceShaderStagesPathsParametersAndDependencyLifetimes() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU staged surface shader test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let vertexPath = packageRoot
            .appendingPathComponent("shaders/scene/scene.vert")
            .path
        let fragmentPath = packageRoot
            .appendingPathComponent("shaders/scene/forward.frag")
            .path
        let texturePath = packageRoot
            .appendingPathComponent("external/SDL/test/icon.png")
            .path

        XCTAssertThrowsError(
            try RenderServices.resources.createSurfaceShader(.init(
                vertexPath: nil,
                fragmentPath: packageRoot
                    .appendingPathComponent("missing-surface.frag")
                    .path,
                parameters: [:]
            ))
        )
        XCTAssertThrowsError(
            try RenderServices.resources.createSurfaceShader(.init(
                vertexPath: packageRoot
                    .appendingPathComponent("missing-surface.vert")
                    .path,
                fragmentPath: fragmentPath,
                parameters: [:]
            ))
        )

        let texture = try RenderServices.resources.loadTexture(.init(
            path: texturePath
        ))
        let shader = try RenderServices.resources.createSurfaceShader(.init(
            vertexPath: vertexPath,
            fragmentPath: fragmentPath,
            parameters: [
                "u_texture": .texture(texture),
                "roughness": .float(0.5),
            ]
        ))
        XCTAssertTrue(shader.isValid)

        var descriptor = MaterialDescriptor.default
        descriptor.surfaceShader = shader
        let material = try RenderServices.resources.createMaterial(descriptor)
        RenderServices.resources.destroyTexture(texture)
        RenderServices.resources.destroySurfaceShader(shader)
        XCTAssertThrowsError(
            try RenderServices.resources.updateSurfaceShader(
                shader,
                parameter: "roughness",
                value: .float(0.8)
            )
        )
        XCTAssertThrowsError(
            try RenderServices.resources.createMaterial(descriptor)
        )

        try RenderServices.resources.updateMaterial(
            material,
            descriptor: .default
        )
        let replacementTexture = try RenderServices.resources.loadTexture(
            .init(path: texturePath)
        )
        XCTAssertNotEqual(replacementTexture.rawValue, texture.rawValue)
        let replacementShader = try RenderServices.resources
            .createSurfaceShader(.init(
                vertexPath: nil,
                fragmentPath: fragmentPath,
                parameters: ["u_texture": .texture(replacementTexture)]
            ))
        XCTAssertNotEqual(replacementShader.rawValue, shader.rawValue)

        RenderServices.resources.destroyTexture(replacementTexture)
        try RenderServices.resources.updateSurfaceShader(
            replacementShader,
            parameter: "u_texture",
            value: .float(1)
        )
        let finalTexture = try RenderServices.resources.loadTexture(
            .init(path: texturePath)
        )
        XCTAssertNotEqual(finalTexture.rawValue, replacementTexture.rawValue)
        RenderServices.resources.destroySurfaceShader(replacementShader)
        XCTAssertThrowsError(
            try RenderServices.resources.updateSurfaceShader(
                replacementShader,
                parameter: "u_texture",
                value: .float(0)
            )
        )
        RenderServices.resources.destroyMaterial(material)
        RenderServices.resources.destroyTexture(finalTexture)
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testInstanceBuffersAllocateUploadValidateAndReleaseGPUStorage() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU instance buffer test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))

        let semantic = try RenderServices.resources.createInstanceBuffer(.init(
            capacity: 128,
            stride: 0,
            includesColor: true,
            customFloatCount: 4
        ))
        XCTAssertTrue(semantic.isValid)

        let raw = try RenderServices.resources.createInstanceBuffer(.init(
            capacity: 4,
            stride: 32,
            includesColor: false,
            customFloatCount: 0
        ))
        var records = [UInt8](repeating: 0x5A, count: 64)
        try records.withUnsafeBytes {
            try RenderServices.resources.uploadInstances(
                raw,
                instances: $0
            )
        }
        records = [UInt8](repeating: 0, count: 33)
        XCTAssertThrowsError(try records.withUnsafeBytes {
            try RenderServices.resources.uploadInstances(
                raw,
                instances: $0
            )
        })
        records = [UInt8](repeating: 0, count: 160)
        XCTAssertThrowsError(try records.withUnsafeBytes {
            try RenderServices.resources.uploadInstances(
                raw,
                instances: $0
            )
        })
        XCTAssertThrowsError(
            try RenderServices.resources.createInstanceBuffer(.init(
                capacity: 0,
                stride: 32,
                includesColor: false,
                customFloatCount: 0
            ))
        )

        RenderServices.resources.destroyInstanceBuffer(raw)
        XCTAssertThrowsError(try records.withUnsafeBytes {
            try RenderServices.resources.uploadInstances(
                raw,
                instances: $0
            )
        })
        RenderServices.resources.destroyInstanceBuffer(semantic)
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testInstanceBufferFinishesSafelyAfterOwnerWindowCloses() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        let window = WindowDescriptor(
            title: "BTBH GPU instance buffer owner test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        )
        try RenderServices.platform.openWindow(window)
        let buffer = try RenderServices.resources.createInstanceBuffer(.init(
            capacity: 8,
            stride: 32,
            includesColor: false,
            customFloatCount: 0
        ))

        RenderServices.platform.closeWindow()
        try RenderServices.platform.openWindow(window)
        RenderServices.resources.destroyInstanceBuffer(buffer)
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.canvas.endFrame()

        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testModelInstancesUsePersistentGPUBufferInsteadOfTransientPacking() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH persistent model instance integration",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/sparse-attributes-and-indices.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let buffer = try RenderServices.resources.createInstanceBuffer(.init(
            capacity: 2,
            stride: 0,
            includesColor: true,
            customFloatCount: 4
        ))
        let instances = [
            RenderInstance(
                transform: .identity,
                color: .init(r: 255, g: 0, b: 0, a: 255),
                custom: .init(x: 1, y: 2, z: 3, w: 4)
            ),
            RenderInstance(
                transform: .identity,
                color: .init(r: 0, g: 255, b: 0, a: 255),
                custom: .init(x: 5, y: 6, z: 7, w: 8)
            ),
        ]
        let frame = FrameDescriptor(
            camera: .init(
                position: .init(x: 0.5, y: 0.5, z: -3),
                target: .init(x: 0.5, y: 0.5, z: 0),
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default,
            renderScale: 0.5
        )
        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: instances,
            overrides: [],
            persistentBuffer: buffer,
            persistentRevision: 1,
            sourceStart: 0,
            count: 2
        )))
        try RenderServices.scene.endFrame()

        let scene = try XCTUnwrap(bundle.scene as? GPUSceneBackend)
        let resources = try XCTUnwrap(
            bundle.resources as? GPUResourceBackend
        )
        let report = try XCTUnwrap(scene.lastPresentationReport)
        XCTAssertEqual(report.drawableSize, .init(width: 320, height: 180))
        XCTAssertEqual(report.renderSize, .init(width: 160, height: 90))
        XCTAssertEqual(report.instanceCount, 2)
        XCTAssertEqual(report.transientInstanceCount, 0)
        XCTAssertGreaterThan(report.persistentBatchCount, 0)
        XCTAssertEqual(resources.semanticInstanceUploadCount, 1)

        RenderServices.canvas.endFrame()

        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: instances,
            overrides: [],
            persistentBuffer: buffer,
            persistentRevision: 1,
            sourceStart: 0,
            count: 2
        )))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [instances[0]],
            overrides: [],
            persistentBuffer: nil,
            sourceStart: 0,
            count: 1
        )))
        try RenderServices.scene.endFrame()

        let mixedReport = try XCTUnwrap(scene.lastPresentationReport)
        XCTAssertEqual(mixedReport.instanceCount, 3)
        XCTAssertEqual(mixedReport.transientInstanceCount, 1)
        XCTAssertGreaterThan(mixedReport.persistentBatchCount, 0)
        XCTAssertEqual(resources.semanticInstanceUploadCount, 1)

        RenderServices.canvas.endFrame()

        var updatedInstances = instances
        updatedInstances[0] = .init(
            transform: .identity,
            color: .init(r: 255, g: 0, b: 0, a: 255),
            custom: .init(x: 9, y: 10, z: 11, w: 12)
        )
        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: updatedInstances,
            overrides: [],
            persistentBuffer: buffer,
            persistentRevision: 2,
            sourceStart: 0,
            count: 2
        )))
        try RenderServices.scene.endFrame()

        let updatedReport = try XCTUnwrap(scene.lastPresentationReport)
        XCTAssertEqual(updatedReport.instanceCount, 2)
        XCTAssertEqual(updatedReport.transientInstanceCount, 0)
        XCTAssertGreaterThan(updatedReport.persistentBatchCount, 0)
        XCTAssertEqual(resources.semanticInstanceUploadCount, 2)

        RenderServices.canvas.endFrame()

        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: updatedInstances,
            overrides: [],
            persistentBuffer: buffer,
            persistentRevision: 2,
            sourceStart: 0,
            count: 2
        )))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: instances,
            overrides: [],
            persistentBuffer: buffer,
            persistentRevision: 1,
            sourceStart: 0,
            count: 2
        )))
        XCTAssertThrowsError(try RenderServices.scene.endFrame())
        XCTAssertEqual(resources.semanticInstanceUploadCount, 2)

        RenderServices.canvas.endFrame()
        RenderServices.resources.destroyInstanceBuffer(buffer)
        RenderServices.resources.destroyModel(model)
    }

    func testPersistentModelRangeListQueuesOnceAndCullsRangesInGPUBackend()
        throws
    {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH persistent model range-list integration",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/sparse-attributes-and-indices.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let buffer = try RenderServices.resources.createInstanceBuffer(.init(
            capacity: 2,
            stride: 0,
            includesColor: true,
            customFloatCount: 4
        ))
        let instances = (0..<2).map { index in
            RenderInstance(
                transform: .identity,
                color: .white,
                custom: .init(
                    x: Float(index),
                    y: 0,
                    z: 0,
                    w: 0
                )
            )
        }
        let frame = FrameDescriptor(
            camera: .init(
                position: .init(x: 0.5, y: 0.5, z: -3),
                target: .init(x: 0.5, y: 0.5, z: 0),
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        )

        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submitPersistentModelRanges(.init(
            model: model,
            instances: instances,
            overrides: [],
            persistentBuffer: buffer,
            persistentRevision: 1,
            ranges: [
                .init(
                    sourceStart: 0,
                    count: 1,
                    bounds: .init(
                        minimum: .init(x: 0, y: 0, z: 0),
                        maximum: .init(x: 1, y: 1, z: 1)
                    )
                ),
                .init(
                    sourceStart: 1,
                    count: 1,
                    bounds: .init(
                        minimum: .init(x: 100, y: 0, z: 0),
                        maximum: .init(x: 101, y: 1, z: 1)
                    )
                ),
            ]
        ))

        let scene = try XCTUnwrap(bundle.scene as? GPUSceneBackend)
        XCTAssertEqual(scene.pendingModelDrawCountForTesting, 1)
        try RenderServices.scene.endFrame()

        let resources = try XCTUnwrap(
            bundle.resources as? GPUResourceBackend
        )
        let report = try XCTUnwrap(scene.lastPresentationReport)
        XCTAssertEqual(report.instanceCount, 1)
        XCTAssertEqual(report.transientInstanceCount, 0)
        XCTAssertGreaterThan(report.persistentBatchCount, 0)
        XCTAssertEqual(resources.semanticInstanceUploadCount, 1)
        RenderServices.canvas.endFrame()

        RenderServices.resources.destroyInstanceBuffer(buffer)
        RenderServices.resources.destroyModel(model)
    }

    func testSkyAmbientAndLightResourcesValidateDependenciesAndStaleHandles() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU environment resource test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))

        XCTAssertThrowsError(
            try RenderServices.resources.createProceduralSky(.init(
                color: .black,
                resolution: 0
            ))
        )
        let sky = try RenderServices.resources.createProceduralSky(.init(
            color: .black,
            resolution: 32
        ))
        let ambient = try RenderServices.resources.createAmbientMap(from: sky)
        RenderServices.resources.destroyCubemap(sky)
        XCTAssertThrowsError(
            try RenderServices.resources.createAmbientMap(from: sky)
        )
        RenderServices.resources.destroyAmbientMap(ambient)
        XCTAssertThrowsError(
            try RenderServices.resources.createAmbientMap(from: sky)
        )
        let replacementSky = try RenderServices.resources
            .createProceduralSky(.init(color: .white, resolution: 32))
        XCTAssertNotEqual(replacementSky.rawValue, sky.rawValue)

        let lightDescriptor = LightDescriptor(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .down,
            color: .white,
            energy: 1,
            range: 100,
            castsShadow: true,
            shadowSoftness: 0.25
        )
        let light = try RenderServices.resources.createLight(lightDescriptor)
        try RenderServices.resources.updateLight(
            light,
            descriptor: lightDescriptor
        )
        RenderServices.resources.destroyLight(light)
        XCTAssertThrowsError(
            try RenderServices.resources.updateLight(
                light,
                descriptor: lightDescriptor
            )
        )
        let invalidLight = LightDescriptor(
            kind: .spot,
            active: true,
            position: .zero,
            direction: .forward,
            color: .white,
            energy: .infinity,
            range: 100,
            castsShadow: false,
            shadowSoftness: 0
        )
        XCTAssertThrowsError(
            try RenderServices.resources.createLight(invalidLight)
        )
        let invalidShadowPeriod = LightDescriptor(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .down,
            color: .white,
            energy: 1,
            range: 100,
            castsShadow: true,
            shadowSoftness: 0.25,
            shadowUpdatePeriodFrames: 0
        )
        XCTAssertThrowsError(
            try RenderServices.resources.createLight(invalidShadowPeriod)
        )

        RenderServices.resources.destroyCubemap(replacementSky)
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testCustomMeshUploadsUpdatesValidatesAndReleasesGPUStorage() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU custom mesh test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let descriptor = MeshDescriptor(
            topology: .triangles,
            storage: .streamed,
            vertexCount: 3,
            indexCount: 3,
            bounds: .init(
                minimum: .init(x: -1, y: -1, z: 0),
                maximum: .init(x: 1, y: 1, z: 0)
            )
        )
        var vertices = [
            RenderVertex(
                position: .init(x: -1, y: -1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 1, y: -1, z: 0),
                textureCoordinate: .init(x: 1, y: 0),
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 0, y: 1, z: 0),
                textureCoordinate: .init(x: 0.5, y: 1),
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
        ]
        var indices: [UInt32] = [0, 1, 2]
        let mesh = try vertices.withUnsafeBytes { vertexBytes in
            try indices.withUnsafeBytes { indexBytes in
                try RenderServices.resources.createMesh(
                    descriptor,
                    vertices: vertexBytes,
                    indices: indexBytes
                )
            }
        }
        vertices[0].position.x = -0.5
        try vertices.withUnsafeBytes { vertexBytes in
            try indices.withUnsafeBytes { indexBytes in
                try RenderServices.resources.updateMesh(
                    mesh,
                    vertices: vertexBytes,
                    indices: indexBytes
                )
            }
        }
        vertices.append(RenderVertex(
            position: .init(x: 1, y: 1, z: 0),
            textureCoordinate: .one,
            normal: .backward,
            tangent: .init(x: 1, y: 0, z: 0, w: 1),
            color: .white
        ))
        indices = [0, 1, 2, 2, 1, 3]
        try vertices.withUnsafeBytes { vertexBytes in
            try indices.withUnsafeBytes { indexBytes in
                try RenderServices.resources.updateMesh(
                    mesh,
                    vertices: vertexBytes,
                    indices: indexBytes
                )
            }
        }
        indices[5] = 4
        XCTAssertThrowsError(try vertices.withUnsafeBytes { vertexBytes in
            try indices.withUnsafeBytes { indexBytes in
                try RenderServices.resources.updateMesh(
                    mesh,
                    vertices: vertexBytes,
                    indices: indexBytes
                )
            }
        })
        RenderServices.resources.destroyMesh(mesh)
        XCTAssertThrowsError(try vertices.withUnsafeBytes { vertexBytes in
            try RenderServices.resources.updateMesh(
                mesh,
                vertices: vertexBytes,
                indices: nil
            )
        })
        XCTAssertFalse(RenderServices.platform.shouldClose)
        indices[5] = 3
        let resizedDescriptor = MeshDescriptor(
            topology: .triangles,
            storage: .streamed,
            vertexCount: 4,
            indexCount: 6,
            bounds: descriptor.bounds
        )
        let detachedMesh = try vertices.withUnsafeBytes { vertexBytes in
            try indices.withUnsafeBytes { indexBytes in
                try RenderServices.resources.createMesh(
                    resizedDescriptor,
                    vertices: vertexBytes,
                    indices: indexBytes
                )
            }
        }
        RenderServices.platform.closeWindow()
        RenderServices.resources.destroyMesh(detachedMesh)
        XCTAssertTrue(RenderServices.platform.shouldClose)
    }

    func testSceneFrustumQueryUsesCurrentCameraWithoutFailClosing() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 1920, height: 1080)
        )
        XCTAssertFalse(RenderServices.scene.intersectsFrustum(.init(
            minimum: .init(x: 1, y: -1, z: 9),
            maximum: .init(x: -1, y: 1, z: 11)
        )))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .zero,
                target: .forward,
                up: .up,
                verticalFOVDegrees: 90,
                projection: .perspective
            ),
            environment: .default
        ))

        XCTAssertTrue(RenderServices.scene.intersectsFrustum(.init(
            minimum: .init(x: -1, y: -1, z: 9),
            maximum: .init(x: 1, y: 1, z: 11)
        )))
        XCTAssertFalse(RenderServices.scene.intersectsFrustum(.init(
            minimum: .init(x: 99, y: -1, z: 9),
            maximum: .init(x: 101, y: 1, z: 11)
        )))
        XCTAssertFalse(RenderServices.scene.intersectsFrustum(.init(
            minimum: .init(x: -1, y: -1, z: -11),
            maximum: .init(x: 1, y: 1, z: -9)
        )))
        XCTAssertTrue(RenderServices.scene.intersectsFrustum(.init(
            minimum: .init(x: -1, y: -1, z: 9_999),
            maximum: .init(x: 1, y: 1, z: 10_001)
        )))
        try RenderServices.scene.endFrame()
    }

    func testSceneFrustumStatePrecomputesPerspectiveCameraGeometry() {
        let state = GPUSceneFrustumCullingState(
            frame: .init(
                camera: .init(
                    position: .zero,
                    target: .forward,
                    up: .up,
                    verticalFOVDegrees: 90,
                    projection: .perspective
                ),
                environment: .default
            ),
            drawableSize: .init(width: 1920, height: 1080)
        )

        XCTAssertNotNil(state)
        XCTAssertTrue(state!.intersects(.init(
            minimum: .init(x: -1, y: -1, z: 9),
            maximum: .init(x: 1, y: 1, z: 11)
        )))
        XCTAssertFalse(state!.intersects(.init(
            minimum: .init(x: 99, y: -1, z: 9),
            maximum: .init(x: 101, y: 1, z: 11)
        )))
    }

    func testSceneProjectAndUnprojectAreDrawableScaleIndependent() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 800, height: 400)
        )
        let perspective = RenderCamera(
            position: .zero,
            target: .forward,
            up: .up,
            verticalFOVDegrees: 90,
            projection: .perspective
        )
        let center = RenderServices.scene.projectNormalized(
            .init(x: 0, y: 0, z: 10),
            camera: perspective
        )
        XCTAssertEqual(center.x, 0.5, accuracy: 0.001)
        XCTAssertEqual(center.y, 0.5, accuracy: 0.001)
        let centerRay = RenderServices.scene.unprojectRay(
            center,
            camera: perspective
        )
        XCTAssertEqual(centerRay.origin, .zero)
        XCTAssertEqual(centerRay.direction.x, 0, accuracy: 0.001)
        XCTAssertEqual(centerRay.direction.y, 0, accuracy: 0.001)
        XCTAssertEqual(centerRay.direction.z, 1, accuracy: 0.001)

        try RenderServices.scene.resize(
            drawableSize: .init(width: 1600, height: 800)
        )
        XCTAssertEqual(
            RenderServices.scene.projectNormalized(
                .init(x: 0, y: 0, z: 10),
                camera: perspective
            ),
            center
        )
        XCTAssertEqual(
            RenderServices.scene.unprojectRay(center, camera: perspective),
            centerRay
        )

        let orthographic = RenderCamera(
            position: .zero,
            target: .forward,
            up: .up,
            verticalFOVDegrees: 20,
            projection: .orthographic
        )
        let edgeRay = RenderServices.scene.unprojectRay(
            .init(x: 1, y: 0.5),
            camera: orthographic
        )
        XCTAssertEqual(edgeRay.origin.x, -20, accuracy: 0.001)
        XCTAssertEqual(edgeRay.origin.y, 0, accuracy: 0.001)
        XCTAssertEqual(edgeRay.origin.z, 0.01, accuracy: 0.001)
        XCTAssertEqual(edgeRay.direction, .forward)

        let inset = NormalizedViewport(
            x: 0.25,
            y: 0.25,
            width: 0.5,
            height: 0.5
        )
        XCTAssertEqual(
            RenderServices.scene.projectNormalized(
                .init(x: 0, y: 0, z: 10),
                camera: perspective,
                viewport: inset
            ),
            center
        )
        XCTAssertEqual(
            RenderServices.scene.unprojectRay(
                center,
                camera: perspective,
                viewport: inset
            ),
            centerRay
        )

        let invalid = NormalizedViewport(
            x: 0.75,
            y: 0,
            width: 0.5,
            height: 1
        )
        XCTAssertEqual(
            RenderServices.scene.projectNormalized(
                .init(x: 0, y: 0, z: 10),
                camera: perspective,
                viewport: invalid
            ),
            .zero
        )
        XCTAssertEqual(
            RenderServices.scene.unprojectRay(
                center,
                camera: perspective,
                viewport: invalid
            ),
            .init(origin: .zero, direction: .zero)
        )
    }

    func testScenePresentsDebugPrimitivesThroughFXAAWithoutFailClosing() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU debug primitive test",
            width: 800,
            height: 400,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 800, height: 400)
        )
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .zero,
                target: .forward,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default,
            antiAliasing: .fxaa
        ))
        RenderServices.scene.submit(.debug(.grid(slices: 10, spacing: 1)))
        RenderServices.scene.submit(.debug(.wireCube(
            center: .zero,
            size: .init(x: 2, y: 2, z: 2),
            color: .white
        )))
        RenderServices.scene.submit(.debug(.sphere(
            center: .init(x: 0, y: 1, z: 3),
            radius: 1,
            color: .white
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.endFrame()
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testGLBModelLoadUploadsGPUResourcesAndReportsMetadata() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU model upload test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let modelPath = ProcessInfo.processInfo.environment[
            "BTBH_GPU_MODEL_TEST_PATH"
        ] ?? packageRoot
            .appendingPathComponent(
                "external/raylib/examples/models/resources/models/gltf/raylib_logo_3d.glb"
            )
            .path
        let model = try RenderServices.resources.loadModel(
            path: modelPath,
            flags: [.meshData, .highQuality]
        )
        let metadata = try RenderServices.resources.modelMetadata(model)

        XCTAssertTrue(model.isValid)
        XCTAssertGreaterThan(metadata.meshCount, 0)
        XCTAssertGreaterThan(metadata.materialCount, 0)
        XCTAssertEqual(metadata.meshMaterialSlots.count, metadata.meshCount)
        XCTAssertLessThan(metadata.bounds.minimum.x, metadata.bounds.maximum.x)
        XCTAssertLessThan(metadata.bounds.minimum.y, metadata.bounds.maximum.y)
        XCTAssertLessThan(metadata.bounds.minimum.z, metadata.bounds.maximum.z)

        RenderServices.resources.destroyModel(model)
        XCTAssertFalse(RenderServices.platform.shouldClose)
        let detachedModel = try RenderServices.resources.loadModel(
            path: modelPath,
            flags: [.meshData]
        )
        RenderServices.platform.closeWindow()
        RenderServices.resources.destroyModel(detachedModel)
        XCTAssertTrue(RenderServices.platform.shouldClose)
    }

    func testOBJModelLoadUploadsAssimpMeshAndReportsMetadata() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU Assimp OBJ upload test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let modelPath = ProcessInfo.processInfo.environment[
            "BTBH_GPU_OBJ_TEST_PATH"
        ] ?? packageRoot.appendingPathComponent(
                "external/raylib/examples/models/resources/models/obj/cube.obj"
            ).path
        let model = try RenderServices.resources.loadModel(
            path: modelPath,
            flags: [.meshData, .highQuality]
        )
        let metadata = try RenderServices.resources.modelMetadata(model)

        XCTAssertTrue(model.isValid)
        XCTAssertGreaterThan(metadata.meshCount, 0)
        XCTAssertGreaterThan(metadata.materialCount, 0)
        XCTAssertEqual(metadata.meshMaterialSlots.count, metadata.meshCount)
        XCTAssertTrue(metadata.meshMaterialSlots.allSatisfy {
            $0 >= 0 && $0 < metadata.materialCount
        })
        XCTAssertLessThan(metadata.bounds.minimum.x, metadata.bounds.maximum.x)
        XCTAssertLessThan(metadata.bounds.minimum.y, metadata.bounds.maximum.y)
        XCTAssertLessThan(metadata.bounds.minimum.z, metadata.bounds.maximum.z)

        RenderServices.resources.destroyModel(model)
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testGLBAnimationLibraryLoadsRealClipData() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let path = ProcessInfo.processInfo.environment[
            "BTBH_GPU_MODEL_TEST_PATH"
        ] ?? packageRoot
            .appendingPathComponent(
                "external/raylib/examples/models/resources/models/gltf/robot.glb"
            )
            .path
        let probe = try GPUAnimationLibraryProbe.load(path: path)
        defer { GPUAnimationLibraryProbe.destroy(probe.pointer) }
        XCTAssertGreaterThan(probe.animationCount, 0)
        XCTAssertEqual(probe.durations.count, probe.animationCount)
        XCTAssertTrue(probe.durations.allSatisfy {
            $0.isFinite && $0 >= 0
        })
        XCTAssertTrue(probe.durations.contains(where: { $0 > 0 }))
        if ProcessInfo.processInfo.environment[
            "BTBH_GPU_MODEL_TEST_PATH"
        ] != nil {
            XCTAssertGreaterThan(probe.animationCount, 24)
        }
        let library = try RenderServices.resources.loadAnimationLibrary(
            path: path
        )
        XCTAssertTrue(library.isValid)
        RenderServices.resources.destroyAnimationLibrary(library)
    }

    func testFontLoadRasterizesARealTTFAtlasAndReleasesIt() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU font atlas test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let font = try RenderServices.resources.loadFont(.init(
            path: packageRoot.appendingPathComponent(
                "external/raylib/examples/text/resources/anonymous_pro_bold.ttf"
            ).path,
            baseSize: 32,
            glyphs: Array(32...126).map(UInt32.init)
        ))
        XCTAssertTrue(font.isValid)
        RenderServices.resources.destroyFont(font)
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testCanvasMeasuresTextFromRasterizedGlyphMetrics() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU text measurement test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let font = try RenderServices.resources.loadFont(.init(
            path: packageRoot.appendingPathComponent(
                "external/raylib/examples/text/resources/anonymous_pro_bold.ttf"
            ).path,
            baseSize: 32,
            glyphs: Array(32...126).map(UInt32.init)
        ))

        let normal = RenderServices.canvas.measureText(
            "HUD",
            font: font,
            size: 32,
            spacing: 2
        )
        let doubled = RenderServices.canvas.measureText(
            "HUD",
            font: font,
            size: 64,
            spacing: 4
        )

        XCTAssertGreaterThan(normal.x, 0)
        XCTAssertEqual(normal.y, 32, accuracy: 0.01)
        XCTAssertEqual(doubled.x, normal.x * 2, accuracy: 0.01)
        XCTAssertEqual(doubled.y, normal.y * 2, accuracy: 0.01)
    }

    func testLanczosUpscalePresentsDrawableSizedScene() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        let drawableSize = PixelSize2D(width: 320, height: 180)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU Lanczos upscale integration",
            width: drawableSize.width,
            height: drawableSize.height,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(drawableSize: drawableSize)

        let environment = EnvironmentDescriptor(
            backgroundColor: .init(r: 255, g: 0, b: 0, a: 255),
            backgroundEnergy: 1,
            ambientColor: .black,
            ambientEnergy: 0,
            ambientMap: .invalid,
            skyCubemap: .invalid,
            ssaoEnabled: false,
            tonemap: .reinhard,
            tonemapWhite: 1
        )
        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0, y: 0, z: -3),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: environment,
            renderScale: 0.5,
            antiAliasing: .off,
            upscaleMode: .lanczos
        ))
        RenderServices.scene.submit(.debug(.sphere(
            center: .zero,
            radius: 0.25,
            color: .white
        )))
        try RenderServices.scene.endFrame()

        let scene = try XCTUnwrap(bundle.scene as? GPUSceneBackend)
        let report = try XCTUnwrap(scene.lastPresentationReport)
        XCTAssertEqual(report.drawableSize, drawableSize)
        XCTAssertEqual(report.renderSize, .init(width: 160, height: 90))

        RenderServices.canvas.endFrame()
        let readback = try RenderServices.canvas.readbackPresentedFrame()
        XCTAssertEqual(readback.size, drawableSize)
        let center = ((drawableSize.height / 2) * drawableSize.width
            + drawableSize.width / 2) * 4
        XCTAssertGreaterThan(readback.rgba8[center], 150)
        XCTAssertLessThan(readback.rgba8[center + 1], 10)
        XCTAssertLessThan(readback.rgba8[center + 2], 10)
        XCTAssertEqual(readback.rgba8[center + 3], 255)
    }

    func testCanvasFillRectPresentsWithoutFailClosing() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU canvas fill test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))

        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.init(
            r: 255,
            g: 0,
            b: 0,
            a: 255
        )))
        RenderServices.canvas.endFrame()
        XCTAssertThrowsError(
            try RenderServices.canvas.readbackPresentedFrame()
        )

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.canvas.submit(.fillRect(.init(
            rect: .init(x: 20, y: 15, width: 80, height: 50),
            color: .white
        )))
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        XCTAssertEqual(readback.size, .init(width: 320, height: 180))
        XCTAssertEqual(readback.rgba8.count, 320 * 180 * 4)
        let backgroundOffset = 0
        XCTAssertEqual(
            Array(readback.rgba8[backgroundOffset..<(backgroundOffset + 4)]),
            [0, 0, 0, 255]
        )
        let fillOffset = (20 * 320 + 30) * 4
        XCTAssertEqual(
            Array(readback.rgba8[fillOffset..<(fillOffset + 4)]),
            [255, 255, 255, 255]
        )
        let pngURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "btbh-gpu-\(UUID().uuidString).png"
            )
        defer { try? FileManager.default.removeItem(at: pngURL) }
        try RenderServices.canvas.writePresentedFramePNG(to: pngURL.path)
        let png = try Data(contentsOf: pngURL)
        XCTAssertEqual(
            Array(png.prefix(8)),
            [137, 80, 78, 71, 13, 10, 26, 10]
        )
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testCanvasRoundedRectClosesTopFanSegment() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU rounded rect test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.canvas.submit(.roundedRect(.init(
            rect: .init(x: 20, y: 15, width: 280, height: 150),
            roundness: 0.2,
            segments: 8,
            color: .white
        )))
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let topCenter = (30 * readback.size.width + 160) * 4
        XCTAssertEqual(
            Array(readback.rgba8[topCenter..<(topCenter + 4)]),
            [255, 255, 255, 255]
        )
    }

    func testCanvasPresentsTextureAndFontAtlasBatches() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU textured canvas test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let texture = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path
        ))
        let font = try RenderServices.resources.loadFont(.init(
            path: packageRoot.appendingPathComponent(
                "external/raylib/examples/text/resources/anonymous_pro_bold.ttf"
            ).path,
            baseSize: 32,
            glyphs: Array(32...126).map(UInt32.init)
        ))

        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.canvas.submit(.pushScissor(.init(
            x: 0,
            y: 0,
            width: 320,
            height: 180
        )))
        RenderServices.canvas.submit(.texturedQuad(.init(
            texture: texture,
            source: .init(x: 0, y: 0, width: 32, height: 32),
            destination: .init(x: 30, y: 30, width: 64, height: 64),
            origin: .zero,
            rotationDegrees: 15,
            tint: .white
        )))
        RenderServices.canvas.submit(.text(.init(
            text: "Vulkan HUD",
            font: font,
            position: .init(x: 160, y: 100),
            size: 32,
            spacing: 1,
            color: .white,
            anchor: .top
        )))
        RenderServices.canvas.submit(.popScissor)
        RenderServices.canvas.endFrame()

        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testCanvasRendersOffscreenTargetBackToPresentedFrame() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU render target test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let target = try RenderServices.resources.createRenderTarget(.init(
            size: .init(width: 64, height: 64),
            colorFormat: .rgba8,
            hasDepth: false,
            filter: .nearest
        ))
        let texture = try RenderServices.resources.colorTexture(of: target)
        defer { RenderServices.resources.destroyRenderTarget(target) }

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.pushRenderTarget(target))
        RenderServices.canvas.submit(.clear(.init(
            r: 255,
            g: 0,
            b: 0,
            a: 255
        )))
        RenderServices.canvas.submit(.fillRect(.init(
            rect: .init(x: 16, y: 16, width: 32, height: 32),
            color: .white
        )))
        RenderServices.canvas.submit(.popRenderTarget)
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.canvas.submit(.texturedQuad(.init(
            texture: texture,
            source: .init(x: 0, y: 0, width: 64, height: 64),
            destination: .init(x: 20, y: 20, width: 64, height: 64),
            origin: .zero,
            rotationDegrees: 0,
            tint: .white
        )))
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let redOffset = (25 * 320 + 25) * 4
        XCTAssertEqual(
            Array(readback.rgba8[redOffset..<(redOffset + 4)]),
            [255, 0, 0, 255]
        )
        let whiteOffset = (50 * 320 + 50) * 4
        XCTAssertEqual(
            Array(readback.rgba8[whiteOffset..<(whiteOffset + 4)]),
            [255, 255, 255, 255]
        )
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testCanvasRenderTargetSamplingMatchesLegacyVerticalOrigin() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU render target origin test",
            width: 128,
            height: 128,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let target = try RenderServices.resources.createRenderTarget(.init(
            size: .init(width: 64, height: 64),
            colorFormat: .rgba8,
            hasDepth: false,
            filter: .nearest
        ))
        let texture = try RenderServices.resources.colorTexture(of: target)
        defer { RenderServices.resources.destroyRenderTarget(target) }

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.pushRenderTarget(target))
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.canvas.submit(.fillRect(.init(
            rect: .init(x: 0, y: 0, width: 64, height: 32),
            color: .init(r: 255, g: 0, b: 0, a: 255)
        )))
        RenderServices.canvas.submit(.fillRect(.init(
            rect: .init(x: 0, y: 32, width: 64, height: 32),
            color: .init(r: 0, g: 0, b: 255, a: 255)
        )))
        RenderServices.canvas.submit(.popRenderTarget)
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.canvas.submit(.texturedQuad(.init(
            texture: texture,
            source: .init(x: 0, y: 64, width: 64, height: -64),
            destination: .init(x: 20, y: 20, width: 64, height: 64),
            origin: .zero,
            rotationDegrees: 0,
            tint: .white
        )))
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let topOffset = (25 * 128 + 25) * 4
        XCTAssertEqual(
            Array(readback.rgba8[topOffset..<(topOffset + 4)]),
            [255, 0, 0, 255]
        )
        let bottomOffset = (75 * 128 + 25) * 4
        XCTAssertEqual(
            Array(readback.rgba8[bottomOffset..<(bottomOffset + 4)]),
            [0, 0, 255, 255]
        )
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testAnimationTreeTracksValidatedClipAndSwitchState() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU animation tree test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let path = packageRoot.appendingPathComponent(
            "external/raylib/examples/models/resources/models/gltf/robot.glb"
        ).path
        let model = try RenderServices.resources.loadModel(
            path: path,
            flags: [.meshData]
        )
        let library = try RenderServices.resources.loadAnimationLibrary(
            path: path
        )
        XCTAssertThrowsError(
            try RenderServices.resources.createAnimationTree(.init(
                model: model,
                library: library,
                clips: [
                    .init(
                        name: "invalid",
                        animationIndex: .max,
                        loop: true,
                        speed: 1
                    )
                ],
                switches: [],
                rootNode: "invalid",
                rootBone: nil
            ))
        )
        let probe = try GPUAnimationLibraryProbe.load(path: path)
        defer { GPUAnimationLibraryProbe.destroy(probe.pointer) }
        let clipDuration = probe.durations[0]
        let tree = try RenderServices.resources.createAnimationTree(.init(
            model: model,
            library: library,
            clips: [
                .init(
                    name: "idle",
                    animationIndex: 0,
                    loop: true,
                    speed: 2
                ),
                .init(
                    name: "once",
                    animationIndex: 0,
                    loop: false,
                    speed: 1
                )
            ],
            switches: [
                .init(
                    name: "root",
                    inputs: ["idle", "once"],
                    activeInput: 0,
                    crossfadeSeconds: 0.2,
                    synchronized: false
                )
            ],
            rootNode: "root",
            rootBone: nil
        ))
        let loopDelta: Float = 1.0 / 60.0
        let loopStart = clipDuration - loopDelta / 2
        var legacyWrappedTime = loopStart + loopDelta
        legacyWrappedTime -= clipDuration
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.idle.speed",
            value: 1
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.idle.time",
            value: loopStart
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: loopDelta
        )
        XCTAssertEqual(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.idle.time"
            ).bitPattern,
            legacyWrappedTime.bitPattern
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.idle.time",
            value: 0
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.once.time",
            value: 0
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.idle.speed",
            value: 2
        )
        XCTAssertEqual(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.idle.speed"
            ),
            2
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: 0.1
        )
        let advanced = try RenderServices.resources.animationTreeParameter(
            tree,
            name: "clip.idle.time"
        )
        XCTAssertGreaterThan(advanced, 0)
        let backend = try XCTUnwrap(
            bundle.resources as? GPUResourceBackend
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "switch.root.activeInput",
            value: 1
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: 0
        )
        var transitionPose = try backend.animationPose(
            model: model,
            player: nil,
            tree: tree
        )
        XCTAssertEqual(
            transitionPose.samples.map(\.animationIndex),
            [0]
        )
        XCTAssertEqual(transitionPose.samples.map(\.weight), [1])
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: 0.1
        )
        transitionPose = try backend.animationPose(
            model: model,
            player: nil,
            tree: tree
        )
        XCTAssertEqual(
            transitionPose.samples.map(\.animationIndex),
            [0, 0]
        )
        XCTAssertEqual(
            transitionPose.samples.map(\.weight),
            [0.5, 0.5]
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: 0.1
        )
        transitionPose = try backend.animationPose(
            model: model,
            player: nil,
            tree: tree
        )
        XCTAssertEqual(
            transitionPose.samples.map(\.animationIndex),
            [0]
        )
        XCTAssertEqual(transitionPose.samples.map(\.weight), [1])
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "switch.root.activeInput",
            value: 0
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: 0.2
        )
        XCTAssertThrowsError(
            try RenderServices.resources.updateAnimationTree(
                tree,
                deltaSeconds: -.infinity
            )
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.idle.time",
            value: 10_000
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.once.time",
            value: 10_000
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: 0
        )
        XCTAssertLessThan(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.idle.time"
            ),
            clipDuration
        )
        XCTAssertEqual(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.once.time"
            ),
            clipDuration,
            accuracy: 0.0001
        )
        XCTAssertEqual(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.once.play"
            ),
            0
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.once.play",
            value: 1
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "switch.root.activeInput",
            value: 1
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: 0
        )
        XCTAssertEqual(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.once.time"
            ),
            0
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "switch.root.activeInput",
            value: 0
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: 0
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.idle.speed",
            value: .greatestFiniteMagnitude
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: .greatestFiniteMagnitude
        )
        XCTAssertTrue(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.idle.time"
            ).isFinite
        )
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.idle.speed",
            value: 2
        )
        let pausedTime = try RenderServices.resources
            .animationTreeParameter(tree, name: "clip.idle.time")
        try RenderServices.resources.setAnimationTreeParameter(
            tree,
            name: "clip.idle.play",
            value: 0
        )
        try RenderServices.resources.updateAnimationTree(
            tree,
            deltaSeconds: 0.1
        )
        XCTAssertEqual(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.idle.time"
            ),
            pausedTime,
            accuracy: 0.0001
        )
        XCTAssertThrowsError(
            try RenderServices.resources.setAnimationTreeParameter(
                tree,
                name: "switch.root.activeInput",
                value: 2
            )
        )
        XCTAssertThrowsError(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.unknown.time"
            )
        )

        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0, y: 0, z: -4),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.animatedModelTransformStreams(.init(
            model: model,
            player: nil,
            tree: tree,
            positions: [.zero],
            rotations: [.identity],
            scales: [.one],
            count: 1
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()
        XCTAssertFalse(RenderServices.platform.shouldClose)

        RenderServices.resources.destroyModel(model)
        RenderServices.resources.destroyAnimationLibrary(library)
        XCTAssertEqual(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "switch.root.activeInput"
            ),
            0
        )
        RenderServices.resources.destroyAnimationTree(tree)
        XCTAssertThrowsError(
            try RenderServices.resources.modelMetadata(model)
        )
        XCTAssertThrowsError(
            try RenderServices.resources.animationTreeParameter(
                tree,
                name: "clip.idle.time"
            )
        )
    }

    func testModelMaterialOverrideStagesPatchAndPinsModelLifetime() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU material override test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let path = packageRoot.appendingPathComponent(
            "external/raylib/examples/models/resources/models/gltf/robot.glb"
        ).path
        let model = try RenderServices.resources.loadModel(
            path: path,
            flags: [.meshData]
        )
        let materialCount = try RenderServices.resources
            .modelMetadata(model).materialCount
        XCTAssertGreaterThan(materialCount, 0)
        XCTAssertThrowsError(
            try RenderServices.resources.createMaterialOverride(
                from: model,
                materialIndex: materialCount,
                patch: .identity
            )
        )
        XCTAssertThrowsError(
            try RenderServices.resources.createMaterialOverride(
                from: model,
                materialIndex: 0,
                patch: .init(albedoTexture: .invalid)
            )
        )
        let material = try RenderServices.resources.createMaterialOverride(
            from: model,
            materialIndex: 0,
            patch: .init(stencilReference: 1)
        )
        XCTAssertTrue(material.isValid)
        RenderServices.resources.destroyModel(model)
        XCTAssertThrowsError(
            try RenderServices.resources.modelMetadata(model)
        )
        RenderServices.resources.destroyMaterial(material)

        let replacementModel = try RenderServices.resources.loadModel(
            path: path,
            flags: [.meshData]
        )
        XCTAssertNotEqual(replacementModel.rawValue, model.rawValue)
        RenderServices.resources.destroyModel(replacementModel)

        let standalone = try RenderServices.resources.createMaterial(.default)
        XCTAssertTrue(standalone.isValid)
        try RenderServices.resources.updateMaterial(
            standalone,
            descriptor: .default
        )
        RenderServices.resources.destroyMaterial(standalone)

        let texturePath = packageRoot
            .appendingPathComponent("external/SDL/test/icon.png")
            .path
        let texture = try RenderServices.resources.loadTexture(.init(
            path: texturePath
        ))
        var texturedDescriptor = MaterialDescriptor.default
        texturedDescriptor.albedoTexture = texture
        let textured = try RenderServices.resources.createMaterial(
            texturedDescriptor
        )
        RenderServices.resources.destroyTexture(texture)
        XCTAssertThrowsError(
            try RenderServices.resources.createScreenShader(.init(
                fragmentPath: packageRoot
                    .appendingPathComponent("shaders/post/output.frag")
                    .path,
                parameters: ["u_texture": .texture(texture)]
            ))
        )
        try RenderServices.resources.updateMaterial(
            textured,
            descriptor: .default
        )
        let replacementTexture = try RenderServices.resources.loadTexture(
            .init(path: texturePath)
        )
        XCTAssertNotEqual(replacementTexture.rawValue, texture.rawValue)
        RenderServices.resources.destroyMaterial(textured)
        RenderServices.resources.destroyTexture(replacementTexture)
    }

    func testGLTFModelLoadHandlesNodesSparseDataMaterialsAndUnicodePath() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU glTF correctness test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let fixtures = packageRoot.appendingPathComponent("Tests/Fixtures")
        let instanced = try RenderServices.resources.loadModel(
            path: fixtures
                .appendingPathComponent(
                    "instanced-transformed-default-material.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let instancedMetadata = try RenderServices.resources.modelMetadata(
            instanced
        )
        XCTAssertEqual(instancedMetadata.meshCount, 4)
        XCTAssertEqual(instancedMetadata.materialCount, 2)
        XCTAssertEqual(instancedMetadata.meshMaterialSlots, [0, 1, 0, 1])
        XCTAssertEqual(instancedMetadata.bounds.minimum.x, 0, accuracy: 0.001)
        XCTAssertEqual(instancedMetadata.bounds.maximum.x, 11, accuracy: 0.001)
        XCTAssertEqual(instancedMetadata.bounds.maximum.y, 1, accuracy: 0.001)
        XCTAssertEqual(instancedMetadata.bounds.maximum.z, 1, accuracy: 0.001)

        let sparse = try RenderServices.resources.loadModel(
            path: fixtures
                .appendingPathComponent(
                    "sparse-attributes-and-indices.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let sparseMetadata = try RenderServices.resources.modelMetadata(sparse)
        XCTAssertEqual(sparseMetadata.meshCount, 1)
        XCTAssertEqual(sparseMetadata.materialCount, 1)
        XCTAssertEqual(sparseMetadata.meshMaterialSlots, [0])
        XCTAssertEqual(sparseMetadata.bounds.minimum.x, 0, accuracy: 0.001)
        XCTAssertEqual(sparseMetadata.bounds.maximum.x, 1, accuracy: 0.001)
        XCTAssertEqual(sparseMetadata.bounds.maximum.y, 1, accuracy: 0.001)
        XCTAssertEqual(sparseMetadata.bounds.maximum.z, 1, accuracy: 0.001)

        let unicodeDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("btbh-模型-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: unicodeDirectory) }
        try FileManager.default.createDirectory(
            at: unicodeDirectory,
            withIntermediateDirectories: true
        )
        let unicodeFixture = unicodeDirectory
            .appendingPathComponent("场景-model.gltf")
        let embeddedURI =
            "data:application/octet-stream;base64," +
            "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAIA/AAABAAIA"
        let unicodeBufferName = "几何.bin"
        let sourceJSON = try String(
            contentsOf: fixtures.appendingPathComponent(
                "instanced-transformed-default-material.gltf"
            ),
            encoding: .utf8
        )
        try sourceJSON
            .replacingOccurrences(
                of: embeddedURI,
                with: unicodeBufferName
            )
            .write(
                to: unicodeFixture,
                atomically: true,
                encoding: .utf8
            )
        let externalBytes = try XCTUnwrap(
            Data(
                base64Encoded:
                    "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAIA/AAABAAIA"
            )
        )
        try externalBytes.write(
            to: unicodeDirectory.appendingPathComponent(unicodeBufferName)
        )
        let unicodeModel = try RenderServices.resources.loadModel(
            path: unicodeFixture.path,
            flags: [.meshData]
        )
        let unicodeMetadata = try RenderServices.resources.modelMetadata(
            unicodeModel
        )
        XCTAssertEqual(unicodeMetadata.meshCount, 4)
        XCTAssertEqual(unicodeMetadata.bounds.maximum.x, 11, accuracy: 0.001)

        RenderServices.resources.destroyModel(unicodeModel)
        RenderServices.resources.destroyModel(sparse)
        RenderServices.resources.destroyModel(instanced)
        XCTAssertFalse(RenderServices.platform.shouldClose)
    }

    func testCorruptEmbeddedModelTextureFailsClosed() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU corrupt embedded texture test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        XCTAssertThrowsError(
            try RenderServices.resources.loadModel(
                path: packageRoot
                    .appendingPathComponent(
                        "Tests/Fixtures/corrupt-embedded-texture-triangle.gltf"
                    )
                    .path,
                flags: [.meshData]
            )
        )
    }

    func testExternalModelTextureLoadsRelativeToGLTF() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU external texture test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/external-texture-triangle.gltf"
                )
                .path,
            flags: [.meshData]
        )
        RenderServices.resources.destroyModel(model)
    }

    func testModelInstancesPresentsLoadedGeometryWithoutFailClosing() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU model instances scene test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/sparse-attributes-and-indices.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let frame = FrameDescriptor(
            camera: .init(
                position: .init(x: 0.5, y: 0.5, z: -3),
                target: .init(x: 0.5, y: 0.5, z: 0),
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        )
        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [.init(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )],
            overrides: [],
            persistentBuffer: nil
        )))

        try RenderServices.scene.endFrame()
        let capturedWorld = try RenderServices.scene
            .captureWorldFrameTexture()
        RenderServices.canvas.submit(.texturedQuad(.init(
            texture: capturedWorld,
            source: .init(x: 0, y: 0, width: 320, height: 180),
            destination: .init(x: 8, y: 8, width: 80, height: 45),
            origin: .zero,
            rotationDegrees: 0,
            tint: .white
        )))
        RenderServices.canvas.endFrame()
        RenderServices.resources.destroyTexture(capturedWorld)

        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.modelTransformStreams(.init(
            model: model,
            positions: [.init(x: 0, y: 0, z: 0)],
            rotations: [.identity],
            scales: [.one],
            colors: [.white],
            sourceStart: 0,
            count: 1
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let vertices = [
            RenderVertex(
                position: .init(x: -0.5, y: -0.5, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 0.5, y: -0.5, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 0, y: 0.5, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
        ]
        let mesh = try vertices.withUnsafeBytes {
            try RenderServices.resources.createMesh(
                .init(
                    topology: .triangles,
                    storage: .immutable,
                    vertexCount: vertices.count,
                    indexCount: 0,
                    bounds: .init(
                        minimum: .init(x: -0.5, y: -0.5, z: 0),
                        maximum: .init(x: 0.5, y: 0.5, z: 0)
                    )
                ),
                vertices: $0,
                indices: nil
            )
        }
        let material = try RenderServices.resources.createMaterial(.default)
        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.mesh(.init(
            mesh: mesh,
            material: material,
            transform: .identity
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.meshInstances(.init(
            mesh: mesh,
            material: material,
            instances: [.init(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )],
            persistentBuffer: nil
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.meshTransformStreams(.init(
            mesh: mesh,
            material: material,
            positions: [.zero],
            rotations: [.identity],
            scales: [.one],
            colors: [.white],
            count: 1
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        XCTAssertFalse(RenderServices.platform.shouldClose)
        RenderServices.resources.destroyMaterial(material)
        RenderServices.resources.destroyMesh(mesh)
        RenderServices.resources.destroyModel(model)
    }

    func testModelInstanceStandaloneMaterialOverrideColorsItsSlot() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU model material override render test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/sparse-attributes-and-indices.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let metadata = try RenderServices.resources.modelMetadata(model)
        var descriptor = MaterialDescriptor.default
        descriptor.albedoColor = .init(r: 255, g: 0, b: 0, a: 255)
        descriptor.unlit = true
        let material = try RenderServices.resources.createMaterial(descriptor)
        let overrides = (0..<metadata.materialCount).map { slot in
            MaterialOverride(
                slot: slot,
                material: material
            )
        }
        defer {
            RenderServices.resources.destroyMaterial(material)
            RenderServices.resources.destroyModel(model)
        }

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0.5, y: 0.5, z: -3),
                target: .init(x: 0.5, y: 0.5, z: 0),
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [.init(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )],
            overrides: overrides,
            persistentBuffer: nil
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let center = ((readback.size.height / 2) * readback.size.width
            + readback.size.width / 2) * 4
        XCTAssertGreaterThan(readback.rgba8[center], 200)
        XCTAssertLessThan(readback.rgba8[center + 1], 80)
        XCTAssertLessThan(readback.rgba8[center + 2], 80)

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0.5, y: 0.5, z: -3),
                target: .init(x: 0.5, y: 0.5, z: 0),
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [.init(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )],
            overrides: overrides,
            persistentBuffer: nil
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let sceneOnlyReadback = try RenderServices.canvas
            .readbackPresentedFrame()
        XCTAssertGreaterThan(sceneOnlyReadback.rgba8[center], 200)
        XCTAssertLessThan(sceneOnlyReadback.rgba8[center + 1], 80)
        XCTAssertLessThan(sceneOnlyReadback.rgba8[center + 2], 80)
    }

    func testImportedModelFlipsVerticalTextureCoordinateLikeLegacyImporter() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU model UV orientation test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/vertical-uv-flip.obj"
                )
                .path,
            flags: [.meshData, .highQuality]
        )
        let texture = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path,
            filter: .nearest
        ))
        let metadata = try RenderServices.resources.modelMetadata(model)
        var descriptor = MaterialDescriptor.default
        descriptor.albedoTexture = texture
        descriptor.unlit = true
        descriptor.cullMode = .none
        let material = try RenderServices.resources.createMaterial(descriptor)
        let overrides = (0..<metadata.materialCount).map {
            MaterialOverride(slot: $0, material: material)
        }
        defer {
            RenderServices.resources.destroyMaterial(material)
            RenderServices.resources.destroyTexture(texture)
            RenderServices.resources.destroyModel(model)
        }

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.init(
            r: 255,
            g: 0,
            b: 255,
            a: 255
        )))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0, y: 0, z: -3),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [.init(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )],
            overrides: overrides,
            persistentBuffer: nil
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let center = ((readback.size.height / 2) * readback.size.width
            + readback.size.width / 2) * 4
        XCTAssertLessThan(readback.rgba8[center], 30)
        XCTAssertLessThan(readback.rgba8[center + 1], 30)
        XCTAssertLessThan(readback.rgba8[center + 2], 30)
    }

    func testImportedOBJKeepsUpwardFaceFrontFacingForDirectionalLight() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU imported OBJ winding test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/upward-lit-slab.obj"
                )
                .path,
            flags: [.meshData, .highQuality]
        )
        let metadata = try RenderServices.resources.modelMetadata(model)
        let material = try RenderServices.resources.createMaterial(.default)
        let overrides = (0..<metadata.materialCount).map {
            MaterialOverride(slot: $0, material: material)
        }
        let directional = try RenderServices.resources.createLight(.init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .down,
            color: .white,
            energy: 3,
            range: 0,
            castsShadow: false,
            shadowSoftness: 0
        ))
        defer {
            RenderServices.resources.destroyLight(directional)
            RenderServices.resources.destroyMaterial(material)
            RenderServices.resources.destroyModel(model)
        }

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0, y: 3, z: -3),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .init(
                backgroundColor: .black,
                backgroundEnergy: 1,
                ambientColor: .black,
                ambientEnergy: 0,
                ambientMap: .invalid,
                skyCubemap: .invalid,
                ssaoEnabled: false,
                tonemap: .reinhard,
                tonemapWhite: 1
            )
        ))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [.init(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )],
            overrides: overrides,
            persistentBuffer: nil
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let center = ((readback.size.height / 2) * readback.size.width
            + readback.size.width / 2) * 4
        XCTAssertGreaterThan(readback.rgba8[center], 180)
        XCTAssertGreaterThan(readback.rgba8[center + 1], 180)
        XCTAssertGreaterThan(readback.rgba8[center + 2], 180)
    }

    func testTransparentMeshDoesNotWriteDepthBeforeOpaqueGeometry() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU transparent depth-write test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let vertices = [
            RenderVertex(
                position: .init(x: -1, y: -1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 1, y: -1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 0, y: 1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
        ]
        let mesh = try vertices.withUnsafeBytes {
            try RenderServices.resources.createMesh(
                .init(
                    topology: .triangles,
                    storage: .immutable,
                    vertexCount: vertices.count,
                    indexCount: 0,
                    bounds: .init(
                        minimum: .init(x: -1, y: -1, z: 0),
                        maximum: .init(x: 1, y: 1, z: 0)
                    )
                ),
                vertices: $0,
                indices: nil
            )
        }
        var frontDescriptor = MaterialDescriptor.default
        frontDescriptor.albedoColor = .init(
            r: 255,
            g: 0,
            b: 0,
            a: 128
        )
        frontDescriptor.transparency = .alpha
        frontDescriptor.cullMode = .none
        frontDescriptor.unlit = true
        let front = try RenderServices.resources.createMaterial(
            frontDescriptor
        )
        var backDescriptor = MaterialDescriptor.default
        backDescriptor.albedoColor = .init(
            r: 0,
            g: 255,
            b: 0,
            a: 255
        )
        backDescriptor.cullMode = .none
        backDescriptor.unlit = true
        let back = try RenderServices.resources.createMaterial(
            backDescriptor
        )
        defer {
            RenderServices.resources.destroyMaterial(back)
            RenderServices.resources.destroyMaterial(front)
            RenderServices.resources.destroyMesh(mesh)
        }

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0, y: 0, z: -3),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.mesh(.init(
            mesh: mesh,
            material: front,
            transform: .init(
                matrix: .translation(.init(x: 0, y: 0, z: -0.4))
            )
        )))
        RenderServices.scene.submit(.mesh(.init(
            mesh: mesh,
            material: back,
            transform: .init(
                matrix: .translation(.init(x: 0, y: 0, z: 0.4))
            )
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let center = ((readback.size.height / 2) * readback.size.width
            + readback.size.width / 2) * 4
        XCTAssertGreaterThan(readback.rgba8[center], 80)
        XCTAssertGreaterThan(readback.rgba8[center + 1], 80)
        XCTAssertLessThan(readback.rgba8[center + 2], 20)
    }

    func testModelMaterialDepthCompareGreaterDoesNotCoverEqualDepth() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU model depth compare test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/sparse-attributes-and-indices.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let metadata = try RenderServices.resources.modelMetadata(model)
        var baseDescriptor = MaterialDescriptor.default
        baseDescriptor.albedoColor = .init(r: 255, g: 0, b: 0, a: 255)
        baseDescriptor.cullMode = .none
        baseDescriptor.unlit = true
        let baseMaterial = try RenderServices.resources.createMaterial(
            baseDescriptor
        )
        var overlayDescriptor = MaterialDescriptor.default
        overlayDescriptor.albedoColor = .init(r: 0, g: 255, b: 0, a: 255)
        overlayDescriptor.depthCompare = .greater
        overlayDescriptor.cullMode = .none
        overlayDescriptor.unlit = true
        let overlayMaterial = try RenderServices.resources.createMaterial(
            overlayDescriptor
        )
        let baseOverrides = (0..<metadata.materialCount).map {
            MaterialOverride(slot: $0, material: baseMaterial)
        }
        let overlayOverrides = (0..<metadata.materialCount).map {
            MaterialOverride(slot: $0, material: overlayMaterial)
        }
        defer {
            RenderServices.resources.destroyMaterial(overlayMaterial)
            RenderServices.resources.destroyMaterial(baseMaterial)
            RenderServices.resources.destroyModel(model)
        }

        let instance = RenderInstance(
            transform: .identity,
            color: .white,
            custom: .init(x: 0, y: 0, z: 0, w: 0)
        )
        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0.5, y: 0.5, z: -3),
                target: .init(x: 0.5, y: 0.5, z: 0),
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [instance],
            overrides: baseOverrides,
            persistentBuffer: nil
        )))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [instance],
            overrides: overlayOverrides,
            persistentBuffer: nil
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let center = ((readback.size.height / 2) * readback.size.width
            + readback.size.width / 2) * 4
        XCTAssertGreaterThan(readback.rgba8[center], 200)
        XCTAssertLessThan(readback.rgba8[center + 1], 80)
        XCTAssertLessThan(readback.rgba8[center + 2], 80)
    }

    func testLitMeshUsesFrameAmbientAndDirectionalLight() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU scene lighting render test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let vertices = [
            RenderVertex(
                position: .init(x: -1, y: -1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 1, y: -1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 0, y: 1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
        ]
        let mesh = try vertices.withUnsafeBytes {
            try RenderServices.resources.createMesh(
                .init(
                    topology: .triangles,
                    storage: .immutable,
                    vertexCount: vertices.count,
                    indexCount: 0,
                    bounds: .init(
                        minimum: .init(x: -1, y: -1, z: 0),
                        maximum: .init(x: 1, y: 1, z: 0)
                    )
                ),
                vertices: $0,
                indices: nil
            )
        }
        let zeroNormalVertices = vertices.map {
            RenderVertex(
                position: $0.position,
                textureCoordinate: $0.textureCoordinate,
                normal: .zero,
                tangent: $0.tangent,
                color: $0.color
            )
        }
        let zeroNormalMesh = try zeroNormalVertices.withUnsafeBytes {
            try RenderServices.resources.createMesh(
                .init(
                    topology: .triangles,
                    storage: .immutable,
                    vertexCount: zeroNormalVertices.count,
                    indexCount: 0,
                    bounds: .init(
                        minimum: .init(x: -1, y: -1, z: 0),
                        maximum: .init(x: 1, y: 1, z: 0)
                    )
                ),
                vertices: $0,
                indices: nil
            )
        }
        let material = try RenderServices.resources.createMaterial(.default)
        let directional = try RenderServices.resources.createLight(.init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .forward,
            color: .init(r: 0, g: 255, b: 0, a: 255),
            energy: 2,
            range: 100,
            castsShadow: false,
            shadowSoftness: 0
        ))
        let fill = try RenderServices.resources.createLight(.init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .forward,
            color: .init(r: 0, g: 0, b: 255, a: 255),
            energy: 0.25,
            range: 100,
            castsShadow: false,
            shadowSoftness: 0
        ))
        let sky = try RenderServices.resources.createProceduralSky(.init(
            color: .init(r: 40, g: 80, b: 120, a: 255),
            resolution: 32
        ))
        defer {
            RenderServices.resources.destroyCubemap(sky)
            RenderServices.resources.destroyLight(fill)
            RenderServices.resources.destroyLight(directional)
            RenderServices.resources.destroyMaterial(material)
            RenderServices.resources.destroyMesh(zeroNormalMesh)
            RenderServices.resources.destroyMesh(mesh)
        }

        let ambientRed = RenderColor(r: 255, g: 0, b: 0, a: 255)
        let frame = FrameDescriptor(
            camera: .init(
                position: .init(x: 0, y: 0, z: -3),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .init(
                backgroundColor: .white,
                backgroundEnergy: 0.5,
                ambientColor: ambientRed,
                ambientEnergy: 1,
                ambientMap: .invalid,
                skyCubemap: sky,
                ssaoEnabled: false,
                tonemap: .reinhard,
                tonemapWhite: 1
            )
        )
        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.mesh(.init(
            mesh: mesh,
            material: material,
            transform: .identity
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        XCTAssertEqual(readback.rgba8[0], 29, accuracy: 2)
        XCTAssertEqual(readback.rgba8[1], 56, accuracy: 2)
        XCTAssertEqual(readback.rgba8[2], 87, accuracy: 2)
        let center = ((readback.size.height / 2) * readback.size.width
            + readback.size.width / 2) * 4
        XCTAssertEqual(readback.rgba8[center], 254, accuracy: 2)
        XCTAssertEqual(readback.rgba8[center + 1], 210, accuracy: 8)
        XCTAssertEqual(readback.rgba8[center + 2], 80, accuracy: 5)

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(frame)
        RenderServices.scene.submit(.mesh(.init(
            mesh: zeroNormalMesh,
            material: material,
            transform: .identity
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let zeroNormalReadback =
            try RenderServices.canvas.readbackPresentedFrame()
        XCTAssertGreaterThan(zeroNormalReadback.rgba8[center], 80)
        XCTAssertLessThanOrEqual(zeroNormalReadback.rgba8[center], 255)
        XCTAssertLessThan(zeroNormalReadback.rgba8[center + 1], 30)
        XCTAssertLessThan(zeroNormalReadback.rgba8[center + 2], 30)
    }

    func testDirectionalShadowDarkensReceiverBehindOpaqueCaster() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU directional shadow test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        func quad(
            minimumX: Float,
            maximumX: Float,
            minimumY: Float,
            maximumY: Float,
            z: Float
        ) -> [RenderVertex] {
            let positions = [
                RenderVec3(x: minimumX, y: minimumY, z: z),
                RenderVec3(x: maximumX, y: minimumY, z: z),
                RenderVec3(x: maximumX, y: maximumY, z: z),
                RenderVec3(x: minimumX, y: minimumY, z: z),
                RenderVec3(x: maximumX, y: maximumY, z: z),
                RenderVec3(x: minimumX, y: maximumY, z: z),
            ]
            return positions.map {
                RenderVertex(
                    position: $0,
                    textureCoordinate: .zero,
                    normal: .backward,
                    tangent: .init(x: 1, y: 0, z: 0, w: 1),
                    color: .white
                )
            }
        }

        let receiverVertices = quad(
            minimumX: -2,
            maximumX: 2,
            minimumY: -1.2,
            maximumY: 1.2,
            z: 0
        )
        let casterVertices = quad(
            minimumX: -0.95,
            maximumX: -0.25,
            minimumY: -0.35,
            maximumY: 0.35,
            z: -0.6
        )
        let receiver = try receiverVertices.withUnsafeBytes {
            try RenderServices.resources.createMesh(
                .init(
                    topology: .triangles,
                    storage: .immutable,
                    vertexCount: receiverVertices.count,
                    indexCount: 0,
                    bounds: .init(
                        minimum: .init(x: -2, y: -1.2, z: 0),
                        maximum: .init(x: 2, y: 1.2, z: 0)
                    )
                ),
                vertices: $0,
                indices: nil
            )
        }
        let caster = try casterVertices.withUnsafeBytes {
            try RenderServices.resources.createMesh(
                .init(
                    topology: .triangles,
                    storage: .immutable,
                    vertexCount: casterVertices.count,
                    indexCount: 0,
                    bounds: .init(
                        minimum: .init(x: -0.95, y: -0.35, z: -0.6),
                        maximum: .init(x: -0.25, y: 0.35, z: -0.6)
                    )
                ),
                vertices: $0,
                indices: nil
            )
        }
        let material = try RenderServices.resources.createMaterial(.default)
        let unshadowedDescriptor = LightDescriptor(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .init(x: 1, y: 0, z: 1),
            color: .white,
            energy: 3,
            range: 10,
            castsShadow: false,
            shadowSoftness: 1
        )
        let light = try RenderServices.resources.createLight(
            unshadowedDescriptor
        )
        defer {
            RenderServices.resources.destroyLight(light)
            RenderServices.resources.destroyMaterial(material)
            RenderServices.resources.destroyMesh(caster)
            RenderServices.resources.destroyMesh(receiver)
        }
        let frame = FrameDescriptor(
            camera: .init(
                position: .init(x: 0, y: 0, z: -5),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 50,
                projection: .perspective
            ),
            environment: .init(
                backgroundColor: .black,
                backgroundEnergy: 0,
                ambientColor: .black,
                ambientEnergy: 0,
                ambientMap: .invalid,
                skyCubemap: .invalid,
                ssaoEnabled: false,
                tonemap: .reinhard,
                tonemapWhite: 1
            )
        )

        func renderFrame() throws {
            RenderServices.canvas.requestPresentedFrameReadback()
            RenderServices.canvas.beginFrame()
            RenderServices.canvas.submit(.clear(.black))
            RenderServices.scene.beginFrame(frame)
            RenderServices.scene.submit(.mesh(.init(
                mesh: receiver,
                material: material,
                transform: .identity
            )))
            RenderServices.scene.submit(.mesh(.init(
                mesh: caster,
                material: material,
                transform: .identity
            )))
            try RenderServices.scene.endFrame()
            RenderServices.canvas.endFrame()
        }

        try renderFrame()
        let unshadowed =
            try RenderServices.canvas.readbackPresentedFrame()
        let shadowedDescriptor = LightDescriptor(
            kind: unshadowedDescriptor.kind,
            active: unshadowedDescriptor.active,
            position: unshadowedDescriptor.position,
            direction: unshadowedDescriptor.direction,
            color: unshadowedDescriptor.color,
            energy: unshadowedDescriptor.energy,
            range: unshadowedDescriptor.range,
            castsShadow: true,
            shadowSoftness: unshadowedDescriptor.shadowSoftness
        )
        try RenderServices.resources.updateLight(
            light,
            descriptor: shadowedDescriptor
        )
        try renderFrame()
        let shadowed =
            try RenderServices.canvas.readbackPresentedFrame()

        XCTAssertEqual(shadowed.size, unshadowed.size)
        var materiallyDarkerPixels = 0
        var litPixels = 0
        for offset in stride(
            from: 0,
            to: shadowed.rgba8.count,
            by: 4
        ) {
            let unshadowedBrightness =
                Int(unshadowed.rgba8[offset]) +
                Int(unshadowed.rgba8[offset + 1]) +
                Int(unshadowed.rgba8[offset + 2])
            let shadowedBrightness =
                Int(shadowed.rgba8[offset]) +
                Int(shadowed.rgba8[offset + 1]) +
                Int(shadowed.rgba8[offset + 2])
            if unshadowedBrightness - shadowedBrightness >= 24 {
                materiallyDarkerPixels += 1
            }
            if shadowedBrightness >= 96 {
                litPixels += 1
            }
        }
        XCTAssertGreaterThan(materiallyDarkerPixels, 100)
        XCTAssertGreaterThan(litPixels, 1_000)
    }

    func testDirectPBRUsesNormalRoughnessAndMetalnessMaps() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU direct PBR material test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let temporaryDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("btbh-direct-pbr-\(UUID().uuidString)")
        try FileManager.default.createDirectory(
            at: temporaryDirectory,
            withIntermediateDirectories: true
        )
        defer {
            try? FileManager.default.removeItem(at: temporaryDirectory)
        }
        func writePPM(_ name: String, rgb: [UInt8]) throws -> String {
            let url = temporaryDirectory.appendingPathComponent(name)
            var bytes = Data("P6\n1 1\n255\n".utf8)
            bytes.append(contentsOf: rgb)
            try bytes.write(to: url)
            return url.path
        }
        let neutralNormal = try RenderServices.resources.loadTexture(.init(
            path: writePPM("normal-neutral.ppm", rgb: [128, 128, 255]),
            filter: .nearest
        ))
        let tiltedNormal = try RenderServices.resources.loadTexture(.init(
            path: writePPM("normal-tilted.ppm", rgb: [255, 128, 128]),
            filter: .nearest
        ))
        let roughDielectric = try RenderServices.resources.loadTexture(.init(
            path: writePPM("orm-rough.ppm", rgb: [255, 255, 0]),
            filter: .nearest
        ))
        let smoothMetal = try RenderServices.resources.loadTexture(.init(
            path: writePPM("orm-metal.ppm", rgb: [255, 32, 255]),
            filter: .nearest
        ))
        defer {
            RenderServices.resources.destroyTexture(smoothMetal)
            RenderServices.resources.destroyTexture(roughDielectric)
            RenderServices.resources.destroyTexture(tiltedNormal)
            RenderServices.resources.destroyTexture(neutralNormal)
        }

        let vertices = [
            RenderVertex(
                position: .init(x: -1, y: -1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 1, y: -1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 0, y: 1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
        ]
        let mesh = try vertices.withUnsafeBytes {
            try RenderServices.resources.createMesh(
                .init(
                    topology: .triangles,
                    storage: .immutable,
                    vertexCount: vertices.count,
                    indexCount: 0,
                    bounds: .init(
                        minimum: .init(x: -1, y: -1, z: 0),
                        maximum: .init(x: 1, y: 1, z: 0)
                    )
                ),
                vertices: $0,
                indices: nil
            )
        }
        let slantedNormal =
            RenderVec3(x: 1, y: 0, z: -1).normalized()
        let slantedVertices = vertices.map {
            RenderVertex(
                position: $0.position,
                textureCoordinate: $0.textureCoordinate,
                normal: slantedNormal,
                tangent: $0.tangent,
                color: $0.color
            )
        }
        let slantedMesh = try slantedVertices.withUnsafeBytes {
            try RenderServices.resources.createMesh(
                .init(
                    topology: .triangles,
                    storage: .immutable,
                    vertexCount: slantedVertices.count,
                    indexCount: 0,
                    bounds: .init(
                        minimum: .init(x: -1, y: -1, z: 0),
                        maximum: .init(x: 1, y: 1, z: 0)
                    )
                ),
                vertices: $0,
                indices: nil
            )
        }
        defer {
            RenderServices.resources.destroyMesh(slantedMesh)
            RenderServices.resources.destroyMesh(mesh)
        }

        func makeMaterial(
            normal: TextureHandle,
            orm: TextureHandle
        ) throws -> MaterialHandle {
            var descriptor = MaterialDescriptor.default
            descriptor.normalTexture = normal
            descriptor.ormTexture = orm
            descriptor.normalScale = 1
            descriptor.occlusion = 1
            descriptor.roughness = 1
            descriptor.metalness = 1
            return try RenderServices.resources.createMaterial(descriptor)
        }
        let neutralRough = try makeMaterial(
            normal: neutralNormal,
            orm: roughDielectric
        )
        let tiltedRough = try makeMaterial(
            normal: tiltedNormal,
            orm: roughDielectric
        )
        let neutralMetal = try makeMaterial(
            normal: neutralNormal,
            orm: smoothMetal
        )
        defer {
            RenderServices.resources.destroyMaterial(neutralMetal)
            RenderServices.resources.destroyMaterial(tiltedRough)
            RenderServices.resources.destroyMaterial(neutralRough)
        }

        let directional = try RenderServices.resources.createLight(.init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: .forward,
            color: .white,
            energy: 0.5,
            range: 100,
            castsShadow: false,
            shadowSoftness: 0
        ))
        defer { RenderServices.resources.destroyLight(directional) }
        let frame = FrameDescriptor(
            camera: .init(
                position: .init(x: 0, y: 0, z: -3),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .init(
                backgroundColor: .black,
                backgroundEnergy: 0,
                ambientColor: .white,
                ambientEnergy: 0.05,
                ambientMap: .invalid,
                skyCubemap: .invalid,
                ssaoEnabled: false,
                tonemap: .reinhard,
                tonemapWhite: 1
            )
        )
        func centerRGB(
            _ material: MaterialHandle,
            mesh renderMesh: MeshHandle,
            transform: RenderTransform
        ) throws -> [UInt8] {
            RenderServices.canvas.requestPresentedFrameReadback()
            RenderServices.canvas.beginFrame()
            RenderServices.canvas.submit(.clear(.black))
            RenderServices.scene.beginFrame(frame)
            RenderServices.scene.submit(.mesh(.init(
                mesh: renderMesh,
                material: material,
                transform: transform
            )))
            try RenderServices.scene.endFrame()
            RenderServices.canvas.endFrame()
            let readback =
                try RenderServices.canvas.readbackPresentedFrame()
            let center = (
                (readback.size.height / 2) * readback.size.width +
                readback.size.width / 2
            ) * 4
            return Array(readback.rgba8[center..<(center + 3)])
        }
        let neutralRoughRGB = try centerRGB(
            neutralRough,
            mesh: mesh,
            transform: .identity
        )
        let tiltedRoughRGB = try centerRGB(
            tiltedRough,
            mesh: mesh,
            transform: .identity
        )
        let neutralMetalRGB = try centerRGB(
            neutralMetal,
            mesh: mesh,
            transform: .identity
        )
        let slantedIdentityRGB = try centerRGB(
            neutralRough,
            mesh: slantedMesh,
            transform: .identity
        )
        let slantedScaledRGB = try centerRGB(
            neutralRough,
            mesh: slantedMesh,
            transform: .init(
                matrix: .scale(.init(x: 3, y: 1, z: 1))
            )
        )
        let neutralRoughLuma = neutralRoughRGB.reduce(0) { $0 + Int($1) }
        let tiltedRoughLuma = tiltedRoughRGB.reduce(0) { $0 + Int($1) }
        let neutralMetalLuma = neutralMetalRGB.reduce(0) { $0 + Int($1) }
        let slantedIdentityLuma =
            slantedIdentityRGB.reduce(0) { $0 + Int($1) }
        let slantedScaledLuma =
            slantedScaledRGB.reduce(0) { $0 + Int($1) }

        XCTAssertGreaterThan(
            neutralRoughLuma,
            tiltedRoughLuma + 60,
            "normal map must rotate the direct-light response"
        )
        XCTAssertNotEqual(
            neutralMetalLuma,
            neutralRoughLuma,
            "roughness/metalness map must change the specular response"
        )
        XCTAssertGreaterThan(
            slantedScaledLuma,
            slantedIdentityLuma + 30,
            "non-uniform scale must use inverse-transpose normals"
        )
    }

    func testProceduralIBLUsesMaterialAOAndMetallicEnvironmentSpecular() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU procedural IBL material test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let temporaryDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("btbh-procedural-ibl-\(UUID().uuidString)")
        try FileManager.default.createDirectory(
            at: temporaryDirectory,
            withIntermediateDirectories: true
        )
        defer {
            try? FileManager.default.removeItem(at: temporaryDirectory)
        }
        func writePPM(_ name: String, rgb: [UInt8]) throws -> String {
            let url = temporaryDirectory.appendingPathComponent(name)
            var bytes = Data("P6\n1 1\n255\n".utf8)
            bytes.append(contentsOf: rgb)
            try bytes.write(to: url)
            return url.path
        }
        let openORM = try RenderServices.resources.loadTexture(.init(
            path: writePPM("orm-open.ppm", rgb: [255, 220, 0]),
            filter: .nearest
        ))
        let occludedORM = try RenderServices.resources.loadTexture(.init(
            path: writePPM("orm-occluded.ppm", rgb: [24, 220, 0]),
            filter: .nearest
        ))
        let metalORM = try RenderServices.resources.loadTexture(.init(
            path: writePPM("orm-metal.ppm", rgb: [255, 48, 255]),
            filter: .nearest
        ))
        defer {
            RenderServices.resources.destroyTexture(metalORM)
            RenderServices.resources.destroyTexture(occludedORM)
            RenderServices.resources.destroyTexture(openORM)
        }

        let vertices = [
            RenderVertex(
                position: .init(x: -1, y: -1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 1, y: -1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 0, y: 1, z: 0),
                textureCoordinate: .zero,
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
        ]
        let mesh = try vertices.withUnsafeBytes {
            try RenderServices.resources.createMesh(
                .init(
                    topology: .triangles,
                    storage: .immutable,
                    vertexCount: vertices.count,
                    indexCount: 0,
                    bounds: .init(
                        minimum: .init(x: -1, y: -1, z: 0),
                        maximum: .init(x: 1, y: 1, z: 0)
                    )
                ),
                vertices: $0,
                indices: nil
            )
        }
        defer { RenderServices.resources.destroyMesh(mesh) }

        func makeMaterial(_ orm: TextureHandle) throws -> MaterialHandle {
            var descriptor = MaterialDescriptor.default
            descriptor.ormTexture = orm
            descriptor.occlusion = 1
            descriptor.roughness = 1
            descriptor.metalness = 1
            return try RenderServices.resources.createMaterial(descriptor)
        }
        let openMaterial = try makeMaterial(openORM)
        let occludedMaterial = try makeMaterial(occludedORM)
        let metalMaterial = try makeMaterial(metalORM)
        defer {
            RenderServices.resources.destroyMaterial(metalMaterial)
            RenderServices.resources.destroyMaterial(occludedMaterial)
            RenderServices.resources.destroyMaterial(openMaterial)
        }

        let sky = try RenderServices.resources.createProceduralSky(.init(
            resolution: 32,
            groundBottomColor: .init(r: 16, g: 32, b: 255, a: 255),
            groundEnergy: 1,
            groundHorizonColor: .init(r: 16, g: 32, b: 255, a: 255),
            groundHorizonCurve: 0.2,
            skyEnergy: 1,
            skyHorizonColor: .init(r: 255, g: 48, b: 16, a: 255),
            skyHorizonCurve: 0.2,
            skyTopColor: .init(r: 255, g: 48, b: 16, a: 255),
            sunColor: .white,
            sunCurve: 0.15,
            sunDirection: .down,
            sunEnergy: 0,
            sunSizeRadians: 0
        ))
        let ambient = try RenderServices.resources.createAmbientMap(from: sky)
        defer {
            RenderServices.resources.destroyAmbientMap(ambient)
            RenderServices.resources.destroyCubemap(sky)
        }
        func frame(
            ambientMap: AmbientMapHandle,
            cameraY: Float
        ) -> FrameDescriptor {
            .init(
                camera: .init(
                    position: .init(x: 0, y: cameraY, z: -3),
                    target: .zero,
                    up: .up,
                    verticalFOVDegrees: 60,
                    projection: .perspective
                ),
                environment: .init(
                    backgroundColor: .black,
                    backgroundEnergy: 0,
                    ambientColor: ambientMap.isValid ? .white : .black,
                    ambientEnergy: 1,
                    ambientMap: ambientMap,
                    skyCubemap: .invalid,
                    ssaoEnabled: false,
                    tonemap: .reinhard,
                    tonemapWhite: 1
                )
            )
        }
        func centerRGB(
            _ material: MaterialHandle,
            ambientMap: AmbientMapHandle,
            cameraY: Float = 0
        ) throws -> [UInt8] {
            RenderServices.canvas.requestPresentedFrameReadback()
            RenderServices.canvas.beginFrame()
            RenderServices.canvas.submit(.clear(.black))
            RenderServices.scene.beginFrame(frame(
                ambientMap: ambientMap,
                cameraY: cameraY
            ))
            RenderServices.scene.submit(.mesh(.init(
                mesh: mesh,
                material: material,
                transform: .identity
            )))
            try RenderServices.scene.endFrame()
            RenderServices.canvas.endFrame()
            let readback =
                try RenderServices.canvas.readbackPresentedFrame()
            let center = (
                (readback.size.height / 2) * readback.size.width +
                readback.size.width / 2
            ) * 4
            return Array(readback.rgba8[center..<(center + 3)])
        }

        let openRGB = try centerRGB(openMaterial, ambientMap: ambient)
        let occludedRGB =
            try centerRGB(occludedMaterial, ambientMap: ambient)
        let skyReflectionRGB = try centerRGB(
            metalMaterial,
            ambientMap: ambient,
            cameraY: -1.5
        )
        let groundReflectionRGB = try centerRGB(
            metalMaterial,
            ambientMap: ambient,
            cameraY: 1.5
        )
        let metalFallbackRGB =
            try centerRGB(
                metalMaterial,
                ambientMap: .invalid,
                cameraY: -1.5
            )
        let openLuma = openRGB.reduce(0) { $0 + Int($1) }
        let occludedLuma = occludedRGB.reduce(0) { $0 + Int($1) }
        let skyReflectionLuma =
            skyReflectionRGB.reduce(0) { $0 + Int($1) }
        let metalFallbackLuma =
            metalFallbackRGB.reduce(0) { $0 + Int($1) }

        XCTAssertGreaterThan(
            openLuma,
            occludedLuma + 100,
            "ORM.r must attenuate indirect diffuse lighting"
        )
        XCTAssertGreaterThan(
            skyReflectionLuma,
            metalFallbackLuma + 50,
            "metal must receive environment specular from the ambient map"
        )
        XCTAssertGreaterThan(
            skyReflectionRGB[0],
            skyReflectionRGB[2] + 20,
            "upward reflection must retain the red sky color"
        )
        XCTAssertGreaterThan(
            groundReflectionRGB[2],
            groundReflectionRGB[0] + 20,
            "view-dependent downward reflection must retain the blue ground"
        )
    }

    func testSceneMeshSamplesItsMaterialAlbedoTexture() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU textured scene mesh test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let texture = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path,
            filter: .nearest
        ))
        let vertices = [
            RenderVertex(
                position: .init(x: -1, y: -1, z: 0),
                textureCoordinate: .init(x: 0, y: 1),
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 1, y: -1, z: 0),
                textureCoordinate: .init(x: 1, y: 1),
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: 1, y: 1, z: 0),
                textureCoordinate: .init(x: 1, y: 0),
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
            RenderVertex(
                position: .init(x: -1, y: 1, z: 0),
                textureCoordinate: .init(x: 0, y: 0),
                normal: .backward,
                tangent: .init(x: 1, y: 0, z: 0, w: 1),
                color: .white
            ),
        ]
        let indices: [UInt32] = [0, 1, 2, 0, 2, 3]
        let mesh = try vertices.withUnsafeBytes { vertexBytes in
            try indices.withUnsafeBytes { indexBytes in
                try RenderServices.resources.createMesh(
                    .init(
                        topology: .triangles,
                        storage: .immutable,
                        vertexCount: vertices.count,
                        indexCount: indices.count,
                        bounds: .init(
                            minimum: .init(x: -1, y: -1, z: 0),
                            maximum: .init(x: 1, y: 1, z: 0)
                        )
                    ),
                    vertices: vertexBytes,
                    indices: indexBytes
                )
            }
        }
        var materialDescriptor = MaterialDescriptor.default
        materialDescriptor.albedoTexture = texture
        materialDescriptor.unlit = true
        materialDescriptor.cullMode = .none
        let material = try RenderServices.resources.createMaterial(
            materialDescriptor
        )
        defer {
            RenderServices.resources.destroyMaterial(material)
            RenderServices.resources.destroyMesh(mesh)
            RenderServices.resources.destroyTexture(texture)
        }

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0, y: 0, z: -3),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.mesh(.init(
            mesh: mesh,
            material: material,
            transform: .identity
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let center = ((readback.size.height / 2) * readback.size.width
            + readback.size.width / 2) * 4
        XCTAssertGreaterThan(readback.rgba8[center], 200)
        XCTAssertGreaterThan(readback.rgba8[center + 1], 200)
        XCTAssertLessThan(readback.rgba8[center + 2], 100)
    }

    func testSceneMaterialEmissionContributesToPresentedColor() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU material emission test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/sparse-attributes-and-indices.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let metadata = try RenderServices.resources.modelMetadata(model)
        var descriptor = MaterialDescriptor.default
        descriptor.albedoColor = .black
        descriptor.emissionColor = .init(r: 255, g: 32, b: 16, a: 255)
        descriptor.emissionEnergy = 1
        let material = try RenderServices.resources.createMaterial(descriptor)
        let overrides = (0..<metadata.materialCount).map {
            MaterialOverride(slot: $0, material: material)
        }
        defer {
            RenderServices.resources.destroyMaterial(material)
            RenderServices.resources.destroyModel(model)
        }

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0.5, y: 0.5, z: -3),
                target: .init(x: 0.5, y: 0.5, z: 0),
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [.init(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )],
            overrides: overrides,
            persistentBuffer: nil
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let center = ((readback.size.height / 2) * readback.size.width
            + readback.size.width / 2) * 4
        XCTAssertGreaterThan(readback.rgba8[center], 220)
        XCTAssertGreaterThan(readback.rgba8[center + 1], 15)
        XCTAssertLessThan(readback.rgba8[center + 1], 80)
        XCTAssertLessThan(readback.rgba8[center + 2], 50)

        descriptor.unlit = true
        try RenderServices.resources.updateMaterial(
            material,
            descriptor: descriptor
        )
        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0.5, y: 0.5, z: -3),
                target: .init(x: 0.5, y: 0.5, z: 0),
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [.init(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )],
            overrides: overrides,
            persistentBuffer: nil
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let unlitReadback = try RenderServices.canvas
            .readbackPresentedFrame()
        XCTAssertLessThan(unlitReadback.rgba8[center], 10)
        XCTAssertLessThan(unlitReadback.rgba8[center + 1], 10)
        XCTAssertLessThan(unlitReadback.rgba8[center + 2], 10)
    }

    func testModelMaterialPatchPreservesImportedBaseColor() throws {
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        try RenderServices.install(GPURenderServices.make(
            request: .platformDefault,
            debugMode: true
        ))
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU imported material patch test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(
            drawableSize: .init(width: 320, height: 180)
        )

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot
                .appendingPathComponent(
                    "Tests/Fixtures/material-factor-triangle.gltf"
                )
                .path,
            flags: [.meshData]
        )
        let material = try RenderServices.resources.createMaterialOverride(
            from: model,
            materialIndex: 1,
            patch: .init(stencilReference: 1)
        )
        defer {
            RenderServices.resources.destroyMaterial(material)
            RenderServices.resources.destroyModel(model)
        }

        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0.5, y: 0.5, z: -3),
                target: .init(x: 0.5, y: 0.5, z: 0),
                up: .up,
                verticalFOVDegrees: 60,
                projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.modelInstances(.init(
            model: model,
            instances: [.init(
                transform: .identity,
                color: .white,
                custom: .init(x: 0, y: 0, z: 0, w: 0)
            )],
            overrides: [.init(slot: 1, material: material)],
            persistentBuffer: nil
        )))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()

        let readback = try RenderServices.canvas.readbackPresentedFrame()
        let center = ((readback.size.height / 2) * readback.size.width
            + readback.size.width / 2) * 4
        XCTAssertLessThan(readback.rgba8[center], 50)
        XCTAssertGreaterThan(readback.rgba8[center + 1], 80)
        XCTAssertLessThan(readback.rgba8[center + 2], 60)
    }
}
