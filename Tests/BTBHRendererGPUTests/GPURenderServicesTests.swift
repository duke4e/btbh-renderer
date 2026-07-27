import BTBHRenderer
import Foundation
import XCTest
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPURenderServicesTests: XCTestCase {
    func testBundleInstallsPresentsClearAndShutsDownExactlyOnce() throws {
        #if os(Windows)
        RenderServices.uninstall()
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testTextureLoadCreatesARealGPUResourceAndDestroyKeepsWindowAlive() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testTextureHandleFinishesSafelyAfterOwnerWindowClosesAndReopens() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testTextureWrapperFinishesSafelyAfterOwnerWindowCloses() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testUnsupportedResourceCallFailsClosedWithExactOperation() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testScreenShaderDescriptorStagesTypedParametersAndTextureDependency() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testSurfaceShaderStagesPathsParametersAndDependencyLifetimes() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testInstanceBuffersAllocateUploadValidateAndReleaseGPUStorage() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testInstanceBufferFinishesSafelyAfterOwnerWindowCloses() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testSkyAmbientAndLightResourcesValidateDependenciesAndStaleHandles() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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

        RenderServices.resources.destroyCubemap(replacementSky)
        XCTAssertFalse(RenderServices.platform.shouldClose)
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testCustomMeshUploadsUpdatesValidatesAndReleasesGPUStorage() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
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

    func testSceneProjectAndUnprojectUseDrawableCameraGeometry() throws {
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
        let center = RenderServices.scene.project(
            .init(x: 0, y: 0, z: 10),
            camera: perspective
        )
        XCTAssertEqual(center.x, 400, accuracy: 0.001)
        XCTAssertEqual(center.y, 200, accuracy: 0.001)
        let centerRay = RenderServices.scene.unprojectRay(
            center,
            camera: perspective
        )
        XCTAssertEqual(centerRay.origin, .zero)
        XCTAssertEqual(centerRay.direction.x, 0, accuracy: 0.001)
        XCTAssertEqual(centerRay.direction.y, 0, accuracy: 0.001)
        XCTAssertEqual(centerRay.direction.z, 1, accuracy: 0.001)

        let orthographic = RenderCamera(
            position: .zero,
            target: .forward,
            up: .up,
            verticalFOVDegrees: 20,
            projection: .orthographic
        )
        let edgeRay = RenderServices.scene.unprojectRay(
            .init(x: 800, y: 200),
            camera: orthographic
        )
        XCTAssertEqual(edgeRay.origin.x, -20, accuracy: 0.001)
        XCTAssertEqual(edgeRay.origin.y, 0, accuracy: 0.001)
        XCTAssertEqual(edgeRay.origin.z, 0.01, accuracy: 0.001)
        XCTAssertEqual(edgeRay.direction, .forward)
        try RenderServices.scene.resize(
            drawableSize: .init(width: 0, height: 0)
        )
        XCTAssertEqual(
            RenderServices.scene.unprojectRay(
                .init(x: .infinity, y: 0),
                camera: perspective
            ),
            .init(origin: .zero, direction: .zero)
        )
    }

    func testScenePresentsDebugPrimitivesWithoutFailClosing() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
            environment: .default
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
        #else
        throw XCTSkip("The Windows Vulkan debug primitive test runs only on Windows")
        #endif
    }

    func testGLBModelLoadUploadsGPUResourcesAndReportsMetadata() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testOBJModelLoadUploadsAssimpMeshAndReportsMetadata() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testGLBAnimationLibraryLoadsRealClipData() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testFontLoadRasterizesARealTTFAtlasAndReleasesIt() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan font test runs only on Windows")
        #endif
    }

    func testCanvasMeasuresTextFromRasterizedGlyphMetrics() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan font test runs only on Windows")
        #endif
    }

    func testCanvasFillRectPresentsWithoutFailClosing() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan canvas test runs only on Windows")
        #endif
    }

    func testCanvasPresentsTextureAndFontAtlasBatches() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan canvas test runs only on Windows")
        #endif
    }

    func testCanvasRendersOffscreenTargetBackToPresentedFrame() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan render-target test runs only on Windows")
        #endif
    }

    func testAnimationTreeTracksValidatedClipAndSwitchState() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testModelMaterialOverrideStagesPatchAndPinsModelLifetime() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testGLTFModelLoadHandlesNodesSparseDataMaterialsAndUnicodePath() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testModelInstancesPresentsLoadedGeometryWithoutFailClosing() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
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
        #else
        throw XCTSkip("The Windows Vulkan scene test runs only on Windows")
        #endif
    }
}
