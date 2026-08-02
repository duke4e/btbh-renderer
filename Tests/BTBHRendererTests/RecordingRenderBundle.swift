import Foundation
import BTBHRenderer

enum RecordingRenderEvent: Equatable {
    case openWindow
    case pollEvents
    case sceneBegin
    case model
    case sceneEnd
    case canvasBegin
    case fillRect
    case canvasEnd
    case rendererShutdown
    case shutdown
}

enum RecordingRenderError: Error, Equatable {
    case unexpectedCall(function: String)
}

@MainActor
final class RecordingRenderBundle: RenderBackendBundle, GamePlatform, Canvas2DBackend, RenderResourceBackend, SceneBackend {
    private enum ActiveFrame {
        case canvas
        case scene
    }

    private(set) var events: [RecordingRenderEvent] = []
    private(set) var mousePositions: [RenderVec2] = []
    private(set) var submittedModelInstanceDraws: [ModelInstancesDraw] = []
    var frustumResult = false
    private var activeFrame: ActiveFrame?

    var platform: any GamePlatform { self }
    var canvas: any Canvas2DBackend { self }
    var resources: any RenderResourceBackend { self }
    var scene: any SceneBackend { self }

    func openWindow(_ descriptor: WindowDescriptor) throws { events.append(.openWindow) }
    func closeWindow() {}
    func pollEvents() { events.append(.pollEvents) }
    func beginFrame() -> PlatformFrame {
        PlatformFrame(
            deltaSeconds: 0,
            elapsedSeconds: 0,
            drawableSize: PixelSize2D(width: 0, height: 0),
            logicalSize: PixelSize2D(width: 0, height: 0)
        )
    }
    var shouldClose: Bool { false }
    var isWindowFocused: Bool { false }
    var refreshRate: Int { 0 }
    var framesPerSecond: Int { 0 }
    var logicalSize: PixelSize2D { .init(width: 640, height: 360) }
    var drawableSize: PixelSize2D { .init(width: 1280, height: 720) }
    var monotonicElapsedSeconds: Double { 42.5 }
    var mousePosition: RenderVec2 { .zero }
    func setMousePosition(_ position: RenderVec2) throws { mousePositions.append(position) }
    var mouseDelta: RenderVec2 { .zero }
    var mouseWheelDelta: Float { 0 }
    func isKeyDown(_ key: InputKey) -> Bool { false }
    func wasKeyPressed(_ key: InputKey) -> Bool { false }
    func isMouseButtonDown(_ button: MouseButton) -> Bool { false }
    func wasMouseButtonPressed(_ button: MouseButton) -> Bool { false }
    func wasMouseButtonReleased(_ button: MouseButton) -> Bool { false }
    var gamepad: GamepadSnapshot {
        GamepadSnapshot(
            isConnected: false,
            leftStick: .zero,
            rightStick: .zero,
            dpad: .zero,
            leftTrigger: 0,
            rightTrigger: 0,
            hasActiveInput: false
        )
    }
    func isGamepadButtonDown(_ button: GamepadButton) -> Bool { false }
    func wasGamepadButtonPressed(_ button: GamepadButton) -> Bool { false }
    func wasGamepadTriggerPressed(left: Bool, threshold: Float) -> Bool { false }
    func wasNavigationPressed(_ direction: NavigationDirection) -> Bool { false }
    func setCursorVisible(_ visible: Bool) {}
    var isCursorVisible: Bool { false }
    func setClipboardText(_ text: String) throws { throw unexpected() }
    func showOpenFileDialog(
        _ request: FileDialogRequest,
        completion: @escaping @Sendable (Result<[URL], FileDialogError>) -> Void
    ) {
        completion(.success([]))
    }

    func beginFrame() {
        activeFrame = .canvas
        events.append(.canvasBegin)
    }
    func submit(_ command: CanvasCommand) {
        if case .fillRect = command { events.append(.fillRect) }
    }
    func measureText(_ text: String, font: FontHandle, size: Float, spacing: Float) -> RenderVec2 { .zero }
    func endFrame() {
        switch activeFrame {
        case .canvas:
            events.append(.canvasEnd)
        case .scene:
            events.append(.sceneEnd)
        case nil:
            break
        }
        activeFrame = nil
    }
    func readbackPresentedFrame() throws -> FrameReadback { throw unexpected() }
    func writePresentedFramePNG(to path: String) throws { throw unexpected() }

    func loadTexture(_ descriptor: TextureLoadDescriptor) throws -> TextureHandle { throw unexpected() }
    func createTexture(_ descriptor: TextureDescriptor, bytes: UnsafeRawBufferPointer?) throws -> TextureHandle { throw unexpected() }
    func updateTexture(_ handle: TextureHandle, bytes: UnsafeRawBufferPointer) throws { throw unexpected() }
    func destroyTexture(_ handle: TextureHandle) {}
    func loadFont(_ descriptor: FontDescriptor) throws -> FontHandle { throw unexpected() }
    func destroyFont(_ handle: FontHandle) {}
    func createRenderTarget(_ descriptor: RenderTargetDescriptor) throws -> RenderTargetHandle { throw unexpected() }
    func colorTexture(of target: RenderTargetHandle) throws -> TextureHandle { throw unexpected() }
    func destroyRenderTarget(_ handle: RenderTargetHandle) {}
    func loadModel(path: String, flags: ModelImportFlags) throws -> ModelHandle { throw unexpected() }
    func modelMetadata(_ handle: ModelHandle) throws -> ModelMetadata { throw unexpected() }
    func destroyModel(_ handle: ModelHandle) {}
    func createMesh(_ descriptor: MeshDescriptor, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws -> MeshHandle { throw unexpected() }
    func updateMesh(_ handle: MeshHandle, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws { throw unexpected() }
    func destroyMesh(_ handle: MeshHandle) {}
    func createMaterial(_ descriptor: MaterialDescriptor) throws -> MaterialHandle { throw unexpected() }
    func updateMaterial(_ handle: MaterialHandle, descriptor: MaterialDescriptor) throws { throw unexpected() }
    func destroyMaterial(_ handle: MaterialHandle) {}
    func createInstanceBuffer(_ descriptor: InstanceBufferDescriptor) throws -> InstanceBufferHandle { throw unexpected() }
    func uploadInstances(_ handle: InstanceBufferHandle, instances: UnsafeRawBufferPointer) throws { throw unexpected() }
    func destroyInstanceBuffer(_ handle: InstanceBufferHandle) {}
    func loadAnimationLibrary(path: String) throws -> AnimationLibraryHandle { throw unexpected() }
    func destroyAnimationLibrary(_ handle: AnimationLibraryHandle) {}
    func createAnimationPlayer(_ descriptor: AnimationPlayerDescriptor) throws -> AnimationPlayerHandle { throw unexpected() }
    func updateAnimationPlayer(_ handle: AnimationPlayerHandle, deltaSeconds: Float) throws { throw unexpected() }
    func setAnimation(_ handle: AnimationPlayerHandle, name: String, loop: Bool, time: Float?) throws { throw unexpected() }
    func destroyAnimationPlayer(_ handle: AnimationPlayerHandle) {}
    func createAnimationTree(_ descriptor: AnimationTreeDescriptor) throws -> AnimationTreeHandle { throw unexpected() }
    func updateAnimationTree(_ handle: AnimationTreeHandle, deltaSeconds: Float) throws { throw unexpected() }
    func setAnimationTreeParameter(_ handle: AnimationTreeHandle, name: String, value: Float) throws { throw unexpected() }
    func animationTreeParameter(_ handle: AnimationTreeHandle, name: String) throws -> Float { throw unexpected() }
    func destroyAnimationTree(_ handle: AnimationTreeHandle) {}
    func createLight(_ descriptor: LightDescriptor) throws -> LightHandle { throw unexpected() }
    func updateLight(_ handle: LightHandle, descriptor: LightDescriptor) throws { throw unexpected() }
    func destroyLight(_ handle: LightHandle) {}
    func createProceduralSky(_ descriptor: ProceduralSkyDescriptor) throws -> CubemapHandle { throw unexpected() }
    func createAmbientMap(from cubemap: CubemapHandle) throws -> AmbientMapHandle { throw unexpected() }
    func destroyCubemap(_ handle: CubemapHandle) {}
    func destroyAmbientMap(_ handle: AmbientMapHandle) {}
    func createSurfaceShader(_ descriptor: SurfaceShaderDescriptor) throws -> SurfaceShaderHandle { throw unexpected() }
    func updateSurfaceShader(_ handle: SurfaceShaderHandle, parameter: String, value: ShaderParameterValue) throws { throw unexpected() }
    func destroySurfaceShader(_ handle: SurfaceShaderHandle) {}
    func createScreenShader(_ descriptor: ScreenShaderDescriptor) throws -> ScreenShaderHandle { throw unexpected() }
    func updateScreenShader(_ handle: ScreenShaderHandle, parameter: String, value: ShaderParameterValue) throws { throw unexpected() }
    func destroyScreenShader(_ handle: ScreenShaderHandle) {}

    func initialize(drawableSize: PixelSize2D) throws { throw unexpected() }
    func resize(drawableSize: PixelSize2D) throws { throw unexpected() }
    func beginFrame(_ descriptor: FrameDescriptor) {
        activeFrame = .scene
        events.append(.sceneBegin)
    }
    func submit(_ draw: SceneDraw) {
        if case .model = draw { events.append(.model) }
        if case .modelInstances(let instances) = draw {
            submittedModelInstanceDraws.append(instances)
        }
    }
    func projectNormalized(
        _ world: RenderVec3,
        camera: RenderCamera,
        viewport: NormalizedViewport
    ) -> NormalizedViewportPoint {
        _ = world; _ = camera; _ = viewport
        return .zero
    }
    func unprojectRay(
        _ point: NormalizedViewportPoint,
        camera: RenderCamera,
        viewport: NormalizedViewport
    ) -> RenderRay {
        _ = point; _ = camera; _ = viewport
        return RenderRay(origin: .zero, direction: .zero)
    }
    func intersectsFrustum(_ bounds: RenderBounds3) -> Bool { frustumResult }
    func readbackWorldFrame() throws -> FrameReadback { throw unexpected() }
    func captureWorldFrameTexture() throws -> TextureHandle { throw unexpected() }

    func shutdownRenderer() { events.append(.rendererShutdown) }
    func shutdown() { events.append(.shutdown) }

    private func unexpected(_ function: String = #function) -> RecordingRenderError {
        .unexpectedCall(function: function)
    }
}
