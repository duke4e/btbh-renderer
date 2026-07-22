import Foundation
import XCTest
import BTBHRenderer

@preconcurrency @MainActor
final class RenderServicesTests: XCTestCase {
    override func setUp() {
        super.setUp()
        MainActor.assumeIsolated {
            RenderServices.uninstall()
        }
    }

    override func tearDown() {
        MainActor.assumeIsolated {
            RenderServices.uninstall()
        }
        super.tearDown()
    }

    func testInstalledBundleReceivesOneOrderedFrame() throws {
        let recording = RecordingRenderBundle()
        try RenderServices.install(recording)

        try RenderServices.platform.openWindow(.init(title: "test", width: 1280, height: 720, mode: .windowed))
        RenderServices.platform.pollEvents()
        RenderServices.scene.beginFrame(.init(
            camera: RenderCamera(
                position: .init(x: 0, y: 10, z: 10), target: .zero, up: .up,
                verticalFOVDegrees: 45, projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.model(.init(model: ModelHandle(rawValue: 7), transform: .identity)))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.fillRect(.init(
            rect: .init(x: 0, y: 0, width: 10, height: 10),
            color: .white
        )))
        RenderServices.canvas.endFrame()

        XCTAssertEqual(recording.events, [
            .openWindow, .pollEvents, .sceneBegin, .model, .sceneEnd,
            .canvasBegin, .fillRect, .canvasEnd,
        ])
    }

    func testDoubleInstallThrowsAlreadyInstalled() throws {
        let recording = RecordingRenderBundle()
        try RenderServices.install(recording)

        XCTAssertThrowsError(try RenderServices.install(RecordingRenderBundle())) { error in
            XCTAssertEqual(error as? RenderServicesError, .alreadyInstalled)
        }
    }

    func testRequireInstalledThrowsAfterUninstall() throws {
        try RenderServices.install(RecordingRenderBundle())
        RenderServices.uninstall()

        XCTAssertThrowsError(try RenderServices.requireInstalled()) { error in
            XCTAssertEqual(error as? RenderServicesError, .notInstalled)
        }
    }

    func testAccessSnapshotsOneCoherentBundle() throws {
        let recording = RecordingRenderBundle()
        try RenderServices.install(recording)

        let access = RenderServices.access

        XCTAssertIdentical(access.platform as AnyObject, recording)
        XCTAssertIdentical(access.canvas as AnyObject, recording)
        XCTAssertIdentical(access.resources as AnyObject, recording)
        XCTAssertIdentical(access.scene as AnyObject, recording)
    }

    func testAccessShutsRendererExactlyOnceBeforeBundleUninstall() throws {
        let recording = RecordingRenderBundle()
        try RenderServices.install(recording)

        let firstAccess = RenderServices.access
        let secondAccess = RenderServices.access
        firstAccess.shutdownRenderer()
        secondAccess.shutdownRenderer()
        firstAccess.shutdownRenderer()
        RenderServices.uninstall()

        XCTAssertEqual(recording.events, [
            .rendererShutdown,
            .shutdown,
        ])
    }

    func testPublicAccessInitializerInjectsExactBackendsWithoutGlobalInstallation() throws {
        let recording = RecordingRenderBundle()
        let access = RenderServicesAccess(
            platform: recording,
            canvas: recording,
            resources: recording,
            scene: recording,
            shutdownRenderer: { recording.shutdownRenderer() }
        )

        XCTAssertIdentical(access.platform as AnyObject, recording)
        XCTAssertIdentical(access.canvas as AnyObject, recording)
        XCTAssertIdentical(access.resources as AnyObject, recording)
        XCTAssertIdentical(access.scene as AnyObject, recording)
        let copiedAccess = access
        access.shutdownRenderer()
        copiedAccess.shutdownRenderer()
        XCTAssertEqual(recording.events, [.rendererShutdown])
        XCTAssertThrowsError(try RenderServices.requireInstalled()) { error in
            XCTAssertEqual(error as? RenderServicesError, .notInstalled)
        }
    }

    func testUninstallShutsDownExactlyOnceAndDoubleUninstallDoesNothing() throws {
        let recording = RecordingRenderBundle()
        try RenderServices.install(recording)

        RenderServices.uninstall()
        RenderServices.uninstall()

        XCTAssertEqual(recording.events, [.shutdown])
        XCTAssertThrowsError(try RenderServices.requireInstalled()) { error in
            XCTAssertEqual(error as? RenderServicesError, .notInstalled)
        }
    }

    func testRecordingBundleThrowingWitnessesFailClosedWithExactFunctionNames() {
        let recording = RecordingRenderBundle()
        let emptyBytes = UnsafeRawBufferPointer(start: nil, count: 0)
        let textureDescriptor = TextureDescriptor(
            size: .init(width: 1, height: 1), mipCount: 1, format: .rgba8,
            usage: .sampled, filter: .nearest, wrap: .clamp
        )
        let fontDescriptor = FontDescriptor(path: "font", baseSize: 12, glyphs: [])
        let targetDescriptor = RenderTargetDescriptor(
            size: .init(width: 1, height: 1), colorFormat: .rgba8, hasDepth: false
        )
        let meshDescriptor = MeshDescriptor(
            topology: .triangles, storage: .immutable, vertexCount: 0, indexCount: 0,
            bounds: .init(minimum: .zero, maximum: .zero)
        )
        let materialDescriptor = makeMaterialDescriptor()
        let instanceDescriptor = InstanceBufferDescriptor(
            capacity: 1, stride: 1, includesColor: false, customFloatCount: 0
        )
        let animationPlayerDescriptor = AnimationPlayerDescriptor(
            model: .invalid, library: .invalid, initialAnimation: "idle"
        )
        let animationTreeDescriptor = AnimationTreeDescriptor(
            model: .invalid, library: .invalid, rootAnimation: "root"
        )
        let lightDescriptor = LightDescriptor(
            kind: .directional, active: false, position: .zero, direction: .zero,
            color: .clear, energy: 0, range: 0, castsShadow: false, shadowSoftness: 0
        )
        let skyDescriptor = ProceduralSkyDescriptor(color: .clear, resolution: 1)
        let surfaceDescriptor = SurfaceShaderDescriptor(
            vertexPath: nil, fragmentPath: "surface", parameters: [:]
        )
        let screenDescriptor = ScreenShaderDescriptor(fragmentPath: "screen", parameters: [:])

        assertUnexpectedCall("setClipboardText(_:)") { try recording.setClipboardText("text") }
        assertUnexpectedCall("readbackPresentedFrame()") { try recording.readbackPresentedFrame() }
        assertUnexpectedCall("writePresentedFramePNG(to:)") { try recording.writePresentedFramePNG(to: "frame.png") }
        assertUnexpectedCall("loadTexture(path:)") { try recording.loadTexture(path: "texture") }
        assertUnexpectedCall("createTexture(_:bytes:)") { try recording.createTexture(textureDescriptor, bytes: nil) }
        assertUnexpectedCall("updateTexture(_:bytes:)") { try recording.updateTexture(.invalid, bytes: emptyBytes) }
        assertUnexpectedCall("loadFont(_:)") { try recording.loadFont(fontDescriptor) }
        assertUnexpectedCall("createRenderTarget(_:)") { try recording.createRenderTarget(targetDescriptor) }
        assertUnexpectedCall("colorTexture(of:)") { try recording.colorTexture(of: .invalid) }
        assertUnexpectedCall("loadModel(path:flags:)") { try recording.loadModel(path: "model", flags: []) }
        assertUnexpectedCall("modelBounds(_:)") { try recording.modelBounds(.invalid) }
        assertUnexpectedCall("createMesh(_:vertices:indices:)") {
            try recording.createMesh(meshDescriptor, vertices: emptyBytes, indices: nil)
        }
        assertUnexpectedCall("updateMesh(_:vertices:indices:)") {
            try recording.updateMesh(.invalid, vertices: emptyBytes, indices: nil)
        }
        assertUnexpectedCall("createMaterial(_:)") { try recording.createMaterial(materialDescriptor) }
        assertUnexpectedCall("updateMaterial(_:descriptor:)") {
            try recording.updateMaterial(.invalid, descriptor: materialDescriptor)
        }
        assertUnexpectedCall("createInstanceBuffer(_:)") { try recording.createInstanceBuffer(instanceDescriptor) }
        assertUnexpectedCall("uploadInstances(_:instances:)") {
            try recording.uploadInstances(.invalid, instances: emptyBytes)
        }
        assertUnexpectedCall("loadAnimationLibrary(path:)") { try recording.loadAnimationLibrary(path: "animations") }
        assertUnexpectedCall("createAnimationPlayer(_:)") { try recording.createAnimationPlayer(animationPlayerDescriptor) }
        assertUnexpectedCall("updateAnimationPlayer(_:deltaSeconds:)") {
            try recording.updateAnimationPlayer(.invalid, deltaSeconds: 0)
        }
        assertUnexpectedCall("setAnimation(_:name:loop:time:)") {
            try recording.setAnimation(.invalid, name: "idle", loop: false, time: nil)
        }
        assertUnexpectedCall("createAnimationTree(_:)") { try recording.createAnimationTree(animationTreeDescriptor) }
        assertUnexpectedCall("updateAnimationTree(_:deltaSeconds:)") {
            try recording.updateAnimationTree(.invalid, deltaSeconds: 0)
        }
        assertUnexpectedCall("setAnimationTreeParameter(_:name:value:)") {
            try recording.setAnimationTreeParameter(.invalid, name: "speed", value: 0)
        }
        assertUnexpectedCall("createLight(_:)") { try recording.createLight(lightDescriptor) }
        assertUnexpectedCall("updateLight(_:descriptor:)") {
            try recording.updateLight(.invalid, descriptor: lightDescriptor)
        }
        assertUnexpectedCall("createProceduralSky(_:)") { try recording.createProceduralSky(skyDescriptor) }
        assertUnexpectedCall("createAmbientMap(from:)") { try recording.createAmbientMap(from: .invalid) }
        assertUnexpectedCall("createSurfaceShader(_:)") { try recording.createSurfaceShader(surfaceDescriptor) }
        assertUnexpectedCall("updateSurfaceShader(_:parameter:value:)") {
            try recording.updateSurfaceShader(.invalid, parameter: "value", value: .float(0))
        }
        assertUnexpectedCall("createScreenShader(_:)") { try recording.createScreenShader(screenDescriptor) }
        assertUnexpectedCall("updateScreenShader(_:parameter:value:)") {
            try recording.updateScreenShader(.invalid, parameter: "value", value: .float(0))
        }
        assertUnexpectedCall("initialize(drawableSize:)") {
            try recording.initialize(drawableSize: .init(width: 1, height: 1))
        }
        assertUnexpectedCall("resize(drawableSize:)") {
            try recording.resize(drawableSize: .init(width: 1, height: 1))
        }
        assertUnexpectedCall("readbackWorldFrame()") { try recording.readbackWorldFrame() }
        assertUnexpectedCall("captureWorldFrameTexture()") { try recording.captureWorldFrameTexture() }
    }

    func testRecordingBundleDestroyWitnessesAreEventFree() {
        let recording = RecordingRenderBundle()

        recording.destroyTexture(.invalid)
        recording.destroyFont(.invalid)
        recording.destroyRenderTarget(.invalid)
        recording.destroyModel(.invalid)
        recording.destroyMesh(.invalid)
        recording.destroyMaterial(.invalid)
        recording.destroyInstanceBuffer(.invalid)
        recording.destroyAnimationLibrary(.invalid)
        recording.destroyAnimationPlayer(.invalid)
        recording.destroyAnimationTree(.invalid)
        recording.destroyLight(.invalid)
        recording.destroyCubemap(.invalid)
        recording.destroyAmbientMap(.invalid)
        recording.destroySurfaceShader(.invalid)
        recording.destroyScreenShader(.invalid)

        XCTAssertEqual(recording.events, [])
    }

    func testRecordingBundleQueriesAndDialogAreDeterministic() {
        let recording = RecordingRenderBundle()
        let platform: any GamePlatform = recording
        let canvas: any Canvas2DBackend = recording
        let scene: any SceneBackend = recording
        let dialogSelection = LockedBox<Result<[URL], FileDialogError>?>(nil)

        let frame = platform.beginFrame()
        XCTAssertEqual(frame.deltaSeconds, 0)
        XCTAssertEqual(frame.elapsedSeconds, 0)
        XCTAssertEqual(frame.drawableSize, .init(width: 0, height: 0))
        XCTAssertEqual(frame.logicalSize, .init(width: 0, height: 0))
        XCTAssertFalse(platform.shouldClose)
        XCTAssertFalse(platform.isWindowFocused)
        XCTAssertEqual(platform.refreshRate, 0)
        XCTAssertEqual(platform.framesPerSecond, 0)
        XCTAssertEqual(platform.mousePosition, .zero)
        XCTAssertEqual(platform.mouseDelta, .zero)
        XCTAssertEqual(platform.mouseWheelDelta, 0)
        XCTAssertTrue(InputKey.allCases.allSatisfy { !platform.isKeyDown($0) && !platform.wasKeyPressed($0) })
        XCTAssertTrue([MouseButton.left, .right, .middle].allSatisfy {
            !platform.isMouseButtonDown($0)
                && !platform.wasMouseButtonPressed($0)
                && !platform.wasMouseButtonReleased($0)
        })
        XCTAssertFalse(platform.gamepad.isConnected)
        XCTAssertEqual(platform.gamepad.leftStick, .zero)
        XCTAssertEqual(platform.gamepad.rightStick, .zero)
        XCTAssertEqual(platform.gamepad.dpad, .zero)
        XCTAssertEqual(platform.gamepad.leftTrigger, 0)
        XCTAssertEqual(platform.gamepad.rightTrigger, 0)
        XCTAssertFalse(platform.gamepad.hasActiveInput)
        XCTAssertTrue([
            GamepadButton.south, .east, .west, .north, .leftShoulder, .rightShoulder,
            .leftStick, .rightStick, .start, .back, .guide, .dpadUp, .dpadDown, .dpadLeft, .dpadRight,
        ].allSatisfy { !platform.isGamepadButtonDown($0) && !platform.wasGamepadButtonPressed($0) })
        XCTAssertFalse(platform.wasGamepadTriggerPressed(left: true, threshold: 0.5))
        XCTAssertTrue([NavigationDirection.up, .down, .left, .right].allSatisfy {
            !platform.wasNavigationPressed($0)
        })
        platform.setCursorVisible(true)
        XCTAssertFalse(platform.isCursorVisible)
        platform.showOpenFileDialog(.init(title: "open", allowedExtensions: [])) {
            dialogSelection.set($0)
        }
        XCTAssertEqual(dialogSelection.value, .success([]))

        XCTAssertEqual(canvas.measureText("text", font: .invalid, size: 12, spacing: 1), .zero)
        let camera = RenderCamera(
            position: .zero, target: .up, up: .up,
            verticalFOVDegrees: 45, projection: .perspective
        )
        XCTAssertEqual(scene.project(.zero, camera: camera), .zero)
        XCTAssertEqual(scene.unprojectRay(.zero, camera: camera), .init(origin: .zero, direction: .zero))
        XCTAssertFalse(scene.intersectsFrustum(.init(minimum: .zero, maximum: .zero)))
        XCTAssertEqual(recording.events, [])
    }

    func testPlatformPublicInitializersAndFileDialogDefaultAreAvailable() {
        let size = PixelSize2D(width: 64, height: 32)
        let window = WindowDescriptor(title: "window", width: 800, height: 600, mode: .borderless)

        XCTAssertEqual(size.width, 64)
        XCTAssertEqual(size.height, 32)
        XCTAssertEqual(window.title, "window")
        XCTAssertEqual(window.width, 800)
        XCTAssertEqual(window.height, 600)
        XCTAssertEqual(window.mode, .borderless)

        let gamepad = GamepadSnapshot(
            isConnected: true, leftStick: .left, rightStick: .right, dpad: .up,
            leftTrigger: 0.25, rightTrigger: 0.75, hasActiveInput: true
        )
        XCTAssertTrue(gamepad.isConnected)
        XCTAssertEqual(gamepad.leftStick, .left)
        XCTAssertEqual(gamepad.rightStick, .right)
        XCTAssertEqual(gamepad.dpad, .up)
        XCTAssertEqual(gamepad.leftTrigger, 0.25)
        XCTAssertEqual(gamepad.rightTrigger, 0.75)
        XCTAssertTrue(gamepad.hasActiveInput)

        let platformFrame = PlatformFrame(
            deltaSeconds: 1 / 60, elapsedSeconds: 2, drawableSize: size, logicalSize: size
        )
        XCTAssertEqual(platformFrame.deltaSeconds, 1 / 60)
        XCTAssertEqual(platformFrame.elapsedSeconds, 2)
        XCTAssertEqual(platformFrame.drawableSize, size)
        XCTAssertEqual(platformFrame.logicalSize, size)

        let dialog = FileDialogRequest(title: "open", allowedExtensions: ["png"])
        XCTAssertEqual(dialog.title, "open")
        XCTAssertEqual(dialog.allowedExtensions, ["png"])
        XCTAssertFalse(dialog.allowsMultipleSelection)
    }

    func testCanvasCommandPublicInitializersAreAvailable() {
        let texture = TextureHandle(rawValue: 1)
        let font = FontHandle(rawValue: 2)
        let rect = RenderRect(x: 0, y: 1, width: 2, height: 3)
        let destination = RenderRect(x: 4, y: 5, width: 6, height: 7)
        let fill = FillRectCommand(rect: rect, color: .white)
        XCTAssertEqual(fill.rect, rect)
        XCTAssertEqual(fill.color, .white)

        let stroke = StrokeRectCommand(rect: rect, thickness: 2, color: .black)
        XCTAssertEqual(stroke.rect, rect)
        XCTAssertEqual(stroke.thickness, 2)
        XCTAssertEqual(stroke.color, .black)

        let rounded = RoundedRectCommand(rect: rect, roundness: 0.5, segments: 8, color: .white)
        XCTAssertEqual(rounded.rect, rect)
        XCTAssertEqual(rounded.roundness, 0.5)
        XCTAssertEqual(rounded.segments, 8)
        XCTAssertEqual(rounded.color, .white)

        let gradient = HorizontalGradientCommand(rect: rect, left: .black, right: .white)
        XCTAssertEqual(gradient.rect, rect)
        XCTAssertEqual(gradient.left, .black)
        XCTAssertEqual(gradient.right, .white)

        let line = Line2DCommand(start: .zero, end: .one, thickness: 3, color: .white)
        XCTAssertEqual(line.start, .zero)
        XCTAssertEqual(line.end, .one)
        XCTAssertEqual(line.thickness, 3)
        XCTAssertEqual(line.color, .white)

        let triangle = Triangle2DCommand(a: .zero, b: .one, c: .up, color: .white, outline: true)
        XCTAssertEqual(triangle.a, .zero)
        XCTAssertEqual(triangle.b, .one)
        XCTAssertEqual(triangle.c, .up)
        XCTAssertEqual(triangle.color, .white)
        XCTAssertTrue(triangle.outline)

        let circle = Circle2DCommand(center: .one, radius: 4, color: .black, outline: false)
        XCTAssertEqual(circle.center, .one)
        XCTAssertEqual(circle.radius, 4)
        XCTAssertEqual(circle.color, .black)
        XCTAssertFalse(circle.outline)

        let ring = Ring2DCommand(
            center: .one, innerRadius: 1, outerRadius: 2,
            startDegrees: 3, endDegrees: 180, segments: 16, color: .white
        )
        XCTAssertEqual(ring.center, .one)
        XCTAssertEqual(ring.innerRadius, 1)
        XCTAssertEqual(ring.outerRadius, 2)
        XCTAssertEqual(ring.startDegrees, 3)
        XCTAssertEqual(ring.endDegrees, 180)
        XCTAssertEqual(ring.segments, 16)
        XCTAssertEqual(ring.color, .white)

        let quad = TexturedQuadCommand(
            texture: texture, source: rect, destination: destination,
            origin: .one, rotationDegrees: 90, tint: .black
        )
        XCTAssertEqual(quad.texture, texture)
        XCTAssertEqual(quad.source, rect)
        XCTAssertEqual(quad.destination, destination)
        XCTAssertEqual(quad.origin, .one)
        XCTAssertEqual(quad.rotationDegrees, 90)
        XCTAssertEqual(quad.tint, .black)

        let text = TextCommand(
            text: "text", font: font, position: .one,
            size: 12, spacing: 1, color: .white, anchor: .center
        )
        XCTAssertEqual(text.text, "text")
        XCTAssertEqual(text.font, font)
        XCTAssertEqual(text.position, .one)
        XCTAssertEqual(text.size, 12)
        XCTAssertEqual(text.spacing, 1)
        XCTAssertEqual(text.color, .white)
        XCTAssertEqual(text.anchor, .center)
    }

    func testResourcePublicInitializersAndPrintedDefaultsAreAvailable() {
        let size = PixelSize2D(width: 64, height: 32)
        let bounds = RenderBounds3(minimum: .zero, maximum: .init(x: 1, y: 2, z: 3))
        let albedoTexture = TextureHandle(rawValue: 1)
        let emissionTexture = TextureHandle(rawValue: 2)
        let normalTexture = TextureHandle(rawValue: 3)
        let ormTexture = TextureHandle(rawValue: 4)
        let model = ModelHandle(rawValue: 4)
        let library = AnimationLibraryHandle(rawValue: 8)
        let surfaceShader = SurfaceShaderHandle(rawValue: 11)
        let screenShader = ScreenShaderHandle(rawValue: 12)
        let ambientMap = AmbientMapHandle(rawValue: 13)
        let skyCubemap = CubemapHandle(rawValue: 14)
        let usage: TextureUsage = [.sampled, .storage]
        XCTAssertEqual(usage.rawValue, TextureUsage.sampled.rawValue | TextureUsage.storage.rawValue)

        let textureDescriptor = TextureDescriptor(
            size: size, mipCount: 2, format: .rgba8, usage: usage,
            filter: .bilinear, wrap: .clamp
        )
        XCTAssertEqual(textureDescriptor.size, size)
        XCTAssertEqual(textureDescriptor.mipCount, 2)
        XCTAssertEqual(textureDescriptor.format, .rgba8)
        XCTAssertEqual(textureDescriptor.usage, usage)
        XCTAssertEqual(textureDescriptor.filter, .bilinear)
        XCTAssertEqual(textureDescriptor.wrap, .clamp)

        let font = FontDescriptor(path: "font", baseSize: 16, glyphs: [65])
        XCTAssertEqual(font.path, "font")
        XCTAssertEqual(font.baseSize, 16)
        XCTAssertEqual(font.glyphs, [65])

        let target = RenderTargetDescriptor(size: size, colorFormat: .rgba16Float, hasDepth: true)
        XCTAssertEqual(target.size, size)
        XCTAssertEqual(target.colorFormat, .rgba16Float)
        XCTAssertTrue(target.hasDepth)

        let mesh = MeshDescriptor(
            topology: .triangles, storage: .streamed,
            vertexCount: 3, indexCount: 6, bounds: bounds
        )
        XCTAssertEqual(mesh.topology, .triangles)
        XCTAssertEqual(mesh.storage, .streamed)
        XCTAssertEqual(mesh.vertexCount, 3)
        XCTAssertEqual(mesh.indexCount, 6)
        XCTAssertEqual(mesh.bounds, bounds)

        let importFlags: ModelImportFlags = [.meshData, .highQuality]
        XCTAssertEqual(
            importFlags.rawValue,
            ModelImportFlags.meshData.rawValue | ModelImportFlags.highQuality.rawValue
        )

        let materialDescriptor = MaterialDescriptor(
            albedoTexture: albedoTexture, albedoColor: .white,
            emissionTexture: emissionTexture, emissionColor: .black, emissionEnergy: 2,
            normalTexture: normalTexture, normalScale: 1,
            ormTexture: ormTexture, occlusion: 0.75, roughness: 0.5, metalness: 0.25,
            uvOffset: .left, uvScale: .right, alphaCutoff: 0.125,
            depthCompare: .lessEqual,
            depthOffsetFactor: 1.5,
            depthOffsetUnits: -2,
            depthRangeNear: 0.1,
            depthRangeFar: 0.9,
            stencilCompare: .notEqual,
            stencilReference: 7,
            stencilMask: 0xF0,
            stencilFail: .zero,
            stencilDepthFail: .incrementClamp,
            stencilPass: .replace,
            cullMode: .back,
            transparency: .alpha, billboard: .yAxis, blend: .mix,
            unlit: true, surfaceShader: surfaceShader
        )
        XCTAssertEqual(materialDescriptor.albedoTexture, albedoTexture)
        XCTAssertEqual(materialDescriptor.albedoColor, .white)
        XCTAssertEqual(materialDescriptor.emissionTexture, emissionTexture)
        XCTAssertEqual(materialDescriptor.emissionColor, .black)
        XCTAssertEqual(materialDescriptor.emissionEnergy, 2)
        XCTAssertEqual(materialDescriptor.normalTexture, normalTexture)
        XCTAssertEqual(materialDescriptor.normalScale, 1)
        XCTAssertEqual(materialDescriptor.ormTexture, ormTexture)
        XCTAssertEqual(materialDescriptor.occlusion, 0.75)
        XCTAssertEqual(materialDescriptor.roughness, 0.5)
        XCTAssertEqual(materialDescriptor.metalness, 0.25)
        XCTAssertEqual(materialDescriptor.uvOffset, .left)
        XCTAssertEqual(materialDescriptor.uvScale, .right)
        XCTAssertEqual(materialDescriptor.alphaCutoff, 0.125)
        XCTAssertEqual(materialDescriptor.depthCompare, .lessEqual)
        XCTAssertEqual(materialDescriptor.depthOffsetFactor, 1.5)
        XCTAssertEqual(materialDescriptor.depthOffsetUnits, -2)
        XCTAssertEqual(materialDescriptor.depthRangeNear, 0.1)
        XCTAssertEqual(materialDescriptor.depthRangeFar, 0.9)
        XCTAssertEqual(materialDescriptor.stencilCompare, .notEqual)
        XCTAssertEqual(materialDescriptor.stencilReference, 7)
        XCTAssertEqual(materialDescriptor.stencilMask, 0xF0)
        XCTAssertEqual(materialDescriptor.stencilFail, .zero)
        XCTAssertEqual(materialDescriptor.stencilDepthFail, .incrementClamp)
        XCTAssertEqual(materialDescriptor.stencilPass, .replace)
        XCTAssertEqual(materialDescriptor.cullMode, .back)
        XCTAssertEqual(materialDescriptor.transparency, .alpha)
        XCTAssertEqual(materialDescriptor.billboard, .yAxis)
        XCTAssertEqual(materialDescriptor.blend, .mix)
        XCTAssertTrue(materialDescriptor.unlit)
        XCTAssertEqual(materialDescriptor.surfaceShader, surfaceShader)

        var defaultMaterial = MaterialDescriptor.default
        XCTAssertEqual(defaultMaterial.albedoColor, .white)
        XCTAssertEqual(defaultMaterial.emissionColor, .white)
        XCTAssertEqual(defaultMaterial.normalScale, 1)
        XCTAssertEqual(defaultMaterial.occlusion, 1)
        XCTAssertEqual(defaultMaterial.roughness, 1)
        XCTAssertEqual(defaultMaterial.metalness, 0)
        XCTAssertEqual(defaultMaterial.uvScale, RenderVec2(x: 1, y: 1))
        XCTAssertEqual(defaultMaterial.alphaCutoff, 0.01)
        XCTAssertEqual(defaultMaterial.depthCompare, .less)
        XCTAssertEqual(defaultMaterial.stencilCompare, .always)
        XCTAssertEqual(defaultMaterial.stencilMask, 0xFF)
        XCTAssertEqual(defaultMaterial.stencilPass, .replace)
        XCTAssertEqual(defaultMaterial.cullMode, .back)
        defaultMaterial.albedoColor = .black
        defaultMaterial.stencilReference = 1
        XCTAssertEqual(defaultMaterial.albedoColor, .black)
        XCTAssertEqual(defaultMaterial.stencilReference, 1)

        let instanceBuffer = InstanceBufferDescriptor(
            capacity: 100, stride: 64, includesColor: true, customFloatCount: 4
        )
        XCTAssertEqual(instanceBuffer.capacity, 100)
        XCTAssertEqual(instanceBuffer.stride, 64)
        XCTAssertTrue(instanceBuffer.includesColor)
        XCTAssertEqual(instanceBuffer.customFloatCount, 4)

        let animationPlayer = AnimationPlayerDescriptor(
            model: model, library: library, initialAnimation: "idle"
        )
        XCTAssertEqual(animationPlayer.model, model)
        XCTAssertEqual(animationPlayer.library, library)
        XCTAssertEqual(animationPlayer.initialAnimation, "idle")

        let animationTree = AnimationTreeDescriptor(
            model: model,
            library: library,
            clips: [
                .init(name: "idle", animationIndex: 17, loop: true, speed: 1),
                .init(name: "walk", animationIndex: 24, loop: true, speed: 0.5),
            ],
            switches: [
                .init(
                    name: "root",
                    inputs: ["idle", "walk"],
                    activeInput: 0,
                    crossfadeSeconds: 0.24,
                    synchronized: false
                ),
            ],
            rootNode: "root",
            rootBone: nil
        )
        XCTAssertEqual(animationTree.model, model)
        XCTAssertEqual(animationTree.library, library)
        XCTAssertEqual(animationTree.clips.map(\.name), ["idle", "walk"])
        XCTAssertEqual(animationTree.clips.map(\.animationIndex), [17, 24])
        XCTAssertEqual(animationTree.switches.first?.inputs, ["idle", "walk"])
        XCTAssertEqual(animationTree.switches.first?.crossfadeSeconds, 0.24)
        XCTAssertEqual(animationTree.rootNode, "root")
        XCTAssertNil(animationTree.rootBone)

        let light = LightDescriptor(
            kind: .directional, active: true, position: .zero, direction: .up,
            color: .white, energy: 1, range: 10, castsShadow: true, shadowSoftness: 0.5
        )
        XCTAssertEqual(light.kind, .directional)
        XCTAssertTrue(light.active)
        XCTAssertEqual(light.position, .zero)
        XCTAssertEqual(light.direction, .up)
        XCTAssertEqual(light.color, .white)
        XCTAssertEqual(light.energy, 1)
        XCTAssertEqual(light.range, 10)
        XCTAssertTrue(light.castsShadow)
        XCTAssertEqual(light.shadowSoftness, 0.5)

        let environment = EnvironmentDescriptor(
            backgroundColor: .black, backgroundEnergy: 1,
            ambientColor: .white, ambientEnergy: 2,
            ambientMap: ambientMap, skyCubemap: skyCubemap,
            ssaoEnabled: true, tonemap: .reinhard, tonemapWhite: 3
        )
        XCTAssertEqual(environment.backgroundColor, .black)
        XCTAssertEqual(environment.backgroundEnergy, 1)
        XCTAssertEqual(environment.ambientColor, .white)
        XCTAssertEqual(environment.ambientEnergy, 2)
        XCTAssertEqual(environment.ambientMap, ambientMap)
        XCTAssertEqual(environment.skyCubemap, skyCubemap)
        XCTAssertTrue(environment.ssaoEnabled)
        XCTAssertEqual(environment.tonemap, .reinhard)
        XCTAssertEqual(environment.tonemapWhite, 3)

        let surface = SurfaceShaderDescriptor(
            vertexPath: "vertex", fragmentPath: "surface", parameters: ["value": .float(1)]
        )
        XCTAssertEqual(surface.vertexPath, "vertex")
        XCTAssertEqual(surface.fragmentPath, "surface")
        assertFloatParameter(surface.parameters["value"], equals: 1)

        let screen = ScreenShaderDescriptor(
            fragmentPath: "screen", parameters: ["texture": .texture(albedoTexture)]
        )
        XCTAssertEqual(screen.fragmentPath, "screen")
        assertTextureParameter(screen.parameters["texture"], equals: albedoTexture)

        let sky = ProceduralSkyDescriptor(color: .white, resolution: 256)
        XCTAssertEqual(sky.color, .white)
        XCTAssertEqual(sky.resolution, 256)

        let postChain = PostChainDescriptor(
            screenShaders: [screenShader], casSharpness: 0.8, tiltShiftEnabled: true
        )
        XCTAssertEqual(postChain.screenShaders, [screenShader])
        XCTAssertEqual(postChain.casSharpness, 0.8)
        XCTAssertTrue(postChain.tiltShiftEnabled)

        XCTAssertEqual(EnvironmentDescriptor.default.backgroundColor, .black)
        XCTAssertEqual(EnvironmentDescriptor.default.backgroundEnergy, 0)
        XCTAssertEqual(EnvironmentDescriptor.default.ambientColor, .white)
        XCTAssertEqual(EnvironmentDescriptor.default.ambientEnergy, 0)
        XCTAssertEqual(EnvironmentDescriptor.default.ambientMap, .invalid)
        XCTAssertEqual(EnvironmentDescriptor.default.skyCubemap, .invalid)
        XCTAssertFalse(EnvironmentDescriptor.default.ssaoEnabled)
        XCTAssertEqual(EnvironmentDescriptor.default.tonemap, .reinhard)
        XCTAssertEqual(EnvironmentDescriptor.default.tonemapWhite, 1)
        XCTAssertEqual(PostChainDescriptor.empty.screenShaders, [])
        XCTAssertEqual(PostChainDescriptor.empty.casSharpness, 0)
        XCTAssertFalse(PostChainDescriptor.empty.tiltShiftEnabled)
    }

    func testScenePublicInitializersAndFrameDefaultsAreAvailable() {
        let size = PixelSize2D(width: 64, height: 32)
        let model = ModelHandle(rawValue: 4)
        let mesh = MeshHandle(rawValue: 5)
        let material = MaterialHandle(rawValue: 6)
        let instances = InstanceBufferHandle(rawValue: 7)
        let player = AnimationPlayerHandle(rawValue: 9)
        let tree = AnimationTreeHandle(rawValue: 10)
        let transform = RenderTransform(matrix: .identity)
        let custom = RenderVec4(x: 1, y: 2, z: 3, w: 4)
        let instance = RenderInstance(transform: transform, color: .white, custom: custom)
        let override = MaterialOverride(slot: 2, material: material)

        XCTAssertEqual(transform.matrix, .identity)
        XCTAssertEqual(override.slot, 2)
        XCTAssertEqual(override.material, material)
        XCTAssertEqual(instance.transform, transform)
        XCTAssertEqual(instance.color, .white)
        XCTAssertEqual(instance.custom, custom)

        let modelDraw = ModelDraw(model: model, transform: transform, overrides: [override])
        XCTAssertEqual(modelDraw.model, model)
        XCTAssertEqual(modelDraw.transform, transform)
        XCTAssertEqual(modelDraw.overrides, [override])
        XCTAssertTrue(ModelDraw(model: model, transform: transform).overrides.isEmpty)

        let modelInstances = ModelInstancesDraw(
            model: model, instances: [instance], overrides: [override], persistentBuffer: instances
        )
        XCTAssertEqual(modelInstances.model, model)
        XCTAssertEqual(modelInstances.instances, [instance])
        XCTAssertEqual(modelInstances.overrides, [override])
        XCTAssertEqual(modelInstances.persistentBuffer, instances)

        let meshDraw = MeshDraw(mesh: mesh, material: material, transform: transform)
        XCTAssertEqual(meshDraw.mesh, mesh)
        XCTAssertEqual(meshDraw.material, material)
        XCTAssertEqual(meshDraw.transform, transform)

        let meshInstances = MeshInstancesDraw(
            mesh: mesh, material: material, instances: [instance], persistentBuffer: instances
        )
        XCTAssertEqual(meshInstances.mesh, mesh)
        XCTAssertEqual(meshInstances.material, material)
        XCTAssertEqual(meshInstances.instances, [instance])
        XCTAssertEqual(meshInstances.persistentBuffer, instances)

        let animatedModel = AnimatedModelDraw(
            model: model, player: player, tree: tree,
            transform: transform, overrides: [override]
        )
        XCTAssertEqual(animatedModel.model, model)
        XCTAssertEqual(animatedModel.player, player)
        XCTAssertEqual(animatedModel.tree, tree)
        XCTAssertEqual(animatedModel.transform, transform)
        XCTAssertEqual(animatedModel.overrides, [override])

        let animatedInstances = AnimatedModelInstancesDraw(
            model: model, player: player, tree: tree,
            instances: [instance], overrides: [override]
        )
        XCTAssertEqual(animatedInstances.model, model)
        XCTAssertEqual(animatedInstances.player, player)
        XCTAssertEqual(animatedInstances.tree, tree)
        XCTAssertEqual(animatedInstances.instances, [instance])
        XCTAssertEqual(animatedInstances.overrides, [override])

        let camera = RenderCamera(position: .zero, target: .up, up: .up, verticalFOVDegrees: 45, projection: .perspective)
        let frame = FrameDescriptor(camera: camera, environment: .default)
        XCTAssertEqual(frame.camera, camera)
        XCTAssertEqual(frame.environment.backgroundColor, EnvironmentDescriptor.default.backgroundColor)
        XCTAssertEqual(frame.environment.backgroundEnergy, EnvironmentDescriptor.default.backgroundEnergy)
        XCTAssertEqual(frame.environment.ambientColor, EnvironmentDescriptor.default.ambientColor)
        XCTAssertEqual(frame.environment.ambientEnergy, EnvironmentDescriptor.default.ambientEnergy)
        XCTAssertEqual(frame.environment.ambientMap, EnvironmentDescriptor.default.ambientMap)
        XCTAssertEqual(frame.environment.skyCubemap, EnvironmentDescriptor.default.skyCubemap)
        XCTAssertEqual(frame.environment.ssaoEnabled, EnvironmentDescriptor.default.ssaoEnabled)
        XCTAssertEqual(frame.environment.tonemap, EnvironmentDescriptor.default.tonemap)
        XCTAssertEqual(frame.environment.tonemapWhite, EnvironmentDescriptor.default.tonemapWhite)
        XCTAssertEqual(frame.renderScale, 1)
        XCTAssertEqual(frame.antiAliasing, .smaaMedium)
        XCTAssertEqual(frame.upscaleMode, .linear)
        XCTAssertEqual(frame.postChain.screenShaders, [])
        XCTAssertEqual(frame.postChain.casSharpness, 0)
        XCTAssertFalse(frame.postChain.tiltShiftEnabled)

        let readback = FrameReadback(size: size, rgba8: [0, 1, 2, 3])
        XCTAssertEqual(readback.size, size)
        XCTAssertEqual(readback.rgba8, [0, 1, 2, 3])

        let ray = RenderRay(origin: .zero, direction: .up)
        XCTAssertEqual(ray.origin, .zero)
        XCTAssertEqual(ray.direction, .up)
    }

    private func assertUnexpectedCall<T>(
        _ expectedFunction: String,
        file: StaticString = #filePath,
        line: UInt = #line,
        operation: () throws -> T
    ) {
        XCTAssertThrowsError(try operation(), file: file, line: line) { error in
            XCTAssertEqual(
                error as? RecordingRenderError,
                .unexpectedCall(function: expectedFunction),
                file: file,
                line: line
            )
        }
    }

    private func makeMaterialDescriptor() -> MaterialDescriptor {
        MaterialDescriptor(
            albedoTexture: .invalid, albedoColor: .clear,
            emissionTexture: .invalid, emissionColor: .clear, emissionEnergy: 0,
            normalTexture: .invalid, normalScale: 0,
            ormTexture: .invalid, occlusion: 0, roughness: 0, metalness: 0,
            uvOffset: .zero, uvScale: .zero, alphaCutoff: 0,
            depthCompare: .always, cullMode: .none,
            transparency: .disabled, billboard: .disabled, blend: .mix,
            unlit: false, surfaceShader: .invalid
        )
    }

    private func assertFloatParameter(
        _ parameter: ShaderParameterValue?,
        equals expected: Float,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        guard case let .float(value)? = parameter else {
            XCTFail("Expected float shader parameter", file: file, line: line)
            return
        }
        XCTAssertEqual(value, expected, file: file, line: line)
    }

    private func assertTextureParameter(
        _ parameter: ShaderParameterValue?,
        equals expected: TextureHandle,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        guard case let .texture(value)? = parameter else {
            XCTFail("Expected texture shader parameter", file: file, line: line)
            return
        }
        XCTAssertEqual(value, expected, file: file, line: line)
    }
}

private final class LockedBox<Value>: @unchecked Sendable {
    private let lock = NSLock()
    private var storedValue: Value

    init(_ value: Value) {
        storedValue = value
    }

    var value: Value {
        lock.lock()
        defer { lock.unlock() }
        return storedValue
    }

    func set(_ value: Value) {
        lock.lock()
        storedValue = value
        lock.unlock()
    }
}
