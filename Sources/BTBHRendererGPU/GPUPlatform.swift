import BTBHRenderer
import BTBHRendererGPUCore
import Foundation

public enum GPUPlatformError: Error, Equatable {
    case windowAlreadyOpen
    case unsupportedWindowMode
    case windowNotOpen
    case invalidWindowDescriptor(String)
}

@MainActor
public final class GPUPlatform: GamePlatform {
    private let request: GPUBackendRequest
    private let debugMode: Bool
    private var window: GPUWindowProbe?
    private var state = GPUPlatformState.empty
    private var forcedClose = false
    private var targetFramesPerSecond = 0
    private var previousFrameStart: TimeInterval?

    public init(
        request: GPUBackendRequest = .platformDefault,
        debugMode: Bool
    ) {
        self.request = request
        self.debugMode = debugMode
    }

    public func openWindow(_ descriptor: WindowDescriptor) throws {
        guard window == nil else {
            throw GPUPlatformError.windowAlreadyOpen
        }
        guard descriptor.mode == .windowed else {
            throw GPUPlatformError.unsupportedWindowMode
        }
        guard descriptor.width > 0,
              descriptor.height > 0,
              descriptor.width <= Int(Int32.max),
              descriptor.height <= Int(Int32.max) else {
            throw GPUPlatformError.invalidWindowDescriptor(
                "GPU window dimensions must fit positive Int32 values"
            )
        }
        targetFramesPerSecond = try Self.targetFrameRate(
            policy: descriptor.frameRatePolicy,
            displayRefreshRate: 0
        )

        let window = try GPUWindowProbe(
            request: request,
            title: descriptor.title,
            width: UInt32(descriptor.width),
            height: UInt32(descriptor.height),
            hidden: false,
            highPixelDensity: descriptor.drawableScalePolicy == .system,
            debugMode: debugMode
        )
        self.window = window
        forcedClose = false
        state = try window.pollPlatform()
        targetFramesPerSecond = try Self.targetFrameRate(
            policy: descriptor.frameRatePolicy,
            displayRefreshRate: state.refreshRate
        )
        if descriptor.drawableScalePolicy == .oneToOne {
            let expected = PixelSize2D(
                width: descriptor.width,
                height: descriptor.height
            )
            guard logicalSize == expected, drawableSize == expected else {
                closeWindow()
                throw GPUPlatformError.invalidWindowDescriptor(
                    "One-to-one GPU window did not produce the requested drawable size"
                )
            }
        }
    }

    public func closeWindow() {
        window?.shutdown()
        window = nil
        state = .empty
        targetFramesPerSecond = 0
        previousFrameStart = nil
    }

    public func pollEvents() {
        guard let window else { return }
        do {
            state = try window.pollPlatform()
        } catch {
            closeWindow()
        }
    }

    public func beginFrame() -> PlatformFrame {
        throttleIfNeeded()
        if let window {
            do {
                state = try window.beginPlatformFrame()
            } catch {
                closeWindow()
            }
        }
        return PlatformFrame(
            deltaSeconds: state.deltaSeconds,
            elapsedSeconds: state.elapsedSeconds,
            drawableSize: drawableSize,
            logicalSize: logicalSize
        )
    }

    public var shouldClose: Bool {
        forcedClose || window == nil || state.shouldClose
    }

    public var isWindowFocused: Bool {
        state.isWindowFocused
    }

    public var refreshRate: Int {
        state.refreshRate
    }

    public var framesPerSecond: Int {
        state.framesPerSecond
    }

    public var logicalSize: PixelSize2D {
        guard let window else {
            return .init(width: 0, height: 0)
        }
        return .init(
            width: Int(window.windowDiagnostics.logicalWidth),
            height: Int(window.windowDiagnostics.logicalHeight)
        )
    }

    public var drawableSize: PixelSize2D {
        guard let window else {
            return .init(width: 0, height: 0)
        }
        return .init(
            width: Int(window.windowDiagnostics.drawableWidth),
            height: Int(window.windowDiagnostics.drawableHeight)
        )
    }

    public var monotonicElapsedSeconds: Double {
        window == nil ? state.elapsedSeconds : GPUWindowProbe.monotonicElapsedSeconds
    }

    public var mousePosition: RenderVec2 {
        state.mousePosition
    }

    public func setMousePosition(_ position: RenderVec2) throws {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        try window.setMousePosition(x: position.x, y: position.y)
    }

    public var mouseDelta: RenderVec2 {
        state.mouseDelta
    }

    public var mouseWheelDelta: Float {
        state.mouseWheelDelta
    }

    public func isKeyDown(_ key: InputKey) -> Bool {
        window?.isKeyDown(key) ?? false
    }

    public func wasKeyPressed(_ key: InputKey) -> Bool {
        window?.wasKeyPressed(key) ?? false
    }

    public func isMouseButtonDown(_ button: MouseButton) -> Bool {
        window?.isMouseButtonDown(button) ?? false
    }

    public func wasMouseButtonPressed(_ button: MouseButton) -> Bool {
        window?.wasMouseButtonPressed(button) ?? false
    }

    public func wasMouseButtonReleased(_ button: MouseButton) -> Bool {
        window?.wasMouseButtonReleased(button) ?? false
    }

    public var gamepad: GamepadSnapshot {
        .init(
            isConnected: false,
            leftStick: .zero,
            rightStick: .zero,
            dpad: .zero,
            leftTrigger: 0,
            rightTrigger: 0,
            hasActiveInput: false
        )
    }

    public func isGamepadButtonDown(_ button: GamepadButton) -> Bool {
        _ = button
        return false
    }

    public func wasGamepadButtonPressed(_ button: GamepadButton) -> Bool {
        _ = button
        return false
    }

    public func wasGamepadTriggerPressed(
        left: Bool,
        threshold: Float
    ) -> Bool {
        _ = left
        _ = threshold
        return false
    }

    public func wasNavigationPressed(
        _ direction: NavigationDirection
    ) -> Bool {
        switch direction {
        case .up:
            return wasKeyPressed(.up) || wasKeyPressed(.w)
        case .down:
            return wasKeyPressed(.down) || wasKeyPressed(.s)
        case .left:
            return wasKeyPressed(.left) || wasKeyPressed(.a)
        case .right:
            return wasKeyPressed(.right) || wasKeyPressed(.d)
        }
    }

    public func setCursorVisible(_ visible: Bool) {
        guard let window else { return }
        if (try? window.setCursorVisible(visible)) != nil {
            state = state.settingCursorVisible(visible)
        }
    }

    public var isCursorVisible: Bool {
        state.isCursorVisible
    }

    public func setClipboardText(_ text: String) throws {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        try window.setClipboardText(text)
    }

    public func showOpenFileDialog(
        _ request: FileDialogRequest,
        completion: @escaping @Sendable (
            Result<[URL], FileDialogError>
        ) -> Void
    ) {
        _ = request
        completion(.failure(.backendFailure(
            "GPU platform file dialogs are not implemented yet"
        )))
    }

    func presentClear(_ color: RenderColor) throws {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        try window.presentClear(
            red: color.r,
            green: color.g,
            blue: color.b,
            alpha: color.a
        )
    }

    func loadTexture(
        _ descriptor: TextureLoadDescriptor
    ) throws -> GPUTextureProbeResource {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.loadTexture(descriptor)
    }

    func destroyTexture(_ texture: OpaquePointer) {
        GPUWindowProbe.destroyTexture(texture)
    }

    func createRenderTarget(
        size: PixelSize2D,
        filter: TextureFilter
    ) throws -> GPUTextureProbeResource {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.createRenderTarget(size: size, filter: filter)
    }

    func loadFont(
        path: String,
        baseSize: UInt32,
        glyphs: [UInt32]
    ) throws -> GPUFontProbeResource {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.loadFont(
            path: path,
            baseSize: baseSize,
            glyphs: glyphs
        )
    }

    func destroyFont(_ font: OpaquePointer) {
        GPUWindowProbe.destroyFont(font)
    }

    func fontGlyph(
        _ font: OpaquePointer,
        codepoint: UInt32
    ) throws -> GPUFontGlyphProbe {
        try GPUWindowProbe.fontGlyph(font, codepoint: codepoint)
    }

    func loadModel(path: String) throws -> GPUModelProbeResource {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.loadModel(path: path)
    }

    func destroyModel(_ model: OpaquePointer) {
        GPUWindowProbe.destroyModel(model)
    }

    func createInstanceBuffer(
        capacity: UInt32,
        stride: UInt32
    ) throws -> GPUInstanceBufferProbeResource {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.createInstanceBuffer(
            capacity: capacity,
            stride: stride
        )
    }

    func uploadInstanceBuffer(
        _ buffer: OpaquePointer,
        bytes: UnsafeRawBufferPointer
    ) throws {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        try window.uploadInstanceBuffer(buffer, bytes: bytes)
    }

    func destroyInstanceBuffer(_ buffer: OpaquePointer) {
        GPUWindowProbe.destroyInstanceBuffer(buffer)
    }

    func createCustomMesh(
        vertices: UnsafeRawBufferPointer,
        vertexCount: UInt32,
        indices: UnsafeBufferPointer<UInt32>?
    ) throws -> GPUCustomMeshProbeResource {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.createCustomMesh(
            vertices: vertices,
            vertexCount: vertexCount,
            indices: indices
        )
    }

    func updateCustomMesh(
        _ mesh: OpaquePointer,
        vertices: UnsafeRawBufferPointer,
        indices: UnsafeBufferPointer<UInt32>?
    ) throws {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        try window.updateCustomMesh(
            mesh,
            vertices: vertices,
            indices: indices
        )
    }

    func destroyCustomMesh(_ mesh: OpaquePointer) {
        GPUWindowProbe.destroyCustomMesh(mesh)
    }

    func presentModelInstances(
        viewProjection: [Float],
        cameraRight: RenderVec3,
        cameraUp: RenderVec3,
        lighting: GPUSceneLightingProbe,
        instances: [Float],
        batches: [GPUSceneGeometryProbeBatch],
        clearColor: RenderColor
    ) throws {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        let shaderDirectory: URL
        if let override = ProcessInfo.processInfo.environment[
            "BTBH_GPU_SHADER_DIR"
        ], !override.isEmpty {
            shaderDirectory = URL(fileURLWithPath: override)
        } else {
            let executable = URL(
                fileURLWithPath: CommandLine.arguments[0]
            ).standardizedFileURL
            shaderDirectory = executable
                .deletingLastPathComponent()
                .appendingPathComponent("GPUShaders")
        }
        let extensionName: String
        let shaderFormat: BTBHGPUShaderFormat
        switch request {
        case .vulkan:
            extensionName = "spv"
            shaderFormat = BTBH_GPU_SHADER_FORMAT_SPIRV
        case .metal:
            extensionName = "metallib"
            shaderFormat = BTBH_GPU_SHADER_FORMAT_METALLIB
        }
        try window.presentModelInstances(
            vertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_model.vert.\(extensionName)"
                )
                .path,
            animatedVertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_animated_model.vert.\(extensionName)"
                )
                .path,
            fragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_model.frag.\(extensionName)"
                )
                .path,
            shaderFormat: shaderFormat,
            viewProjection: viewProjection,
            cameraRight: cameraRight,
            cameraUp: cameraUp,
            lighting: lighting,
            instances: instances,
            batches: batches,
            clearColor: clearColor
        )
    }

    func presentCanvas(
        vertices: [GPUCanvasVertexProbe],
        batches: [GPUCanvasBatchProbe],
        passes: [GPUCanvasPassProbe]
    ) throws {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        let shaderDirectory: URL
        if let override = ProcessInfo.processInfo.environment[
            "BTBH_GPU_SHADER_DIR"
        ], !override.isEmpty {
            shaderDirectory = URL(fileURLWithPath: override)
        } else {
            let executable = URL(
                fileURLWithPath: CommandLine.arguments[0]
            ).standardizedFileURL
            shaderDirectory = executable
                .deletingLastPathComponent()
                .appendingPathComponent("GPUShaders")
        }
        let extensionName: String
        let shaderFormat: BTBHGPUShaderFormat
        switch request {
        case .vulkan:
            extensionName = "spv"
            shaderFormat = BTBH_GPU_SHADER_FORMAT_SPIRV
        case .metal:
            extensionName = "metallib"
            shaderFormat = BTBH_GPU_SHADER_FORMAT_METALLIB
        }
        try window.presentCanvas(
            vertexShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_canvas.vert.\(extensionName)"
                )
                .path,
            fragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_canvas.frag.\(extensionName)"
                )
                .path,
            texturedFragmentShaderPath: shaderDirectory
                .appendingPathComponent(
                    "production_canvas_textured.frag.\(extensionName)"
                )
                .path,
            shaderFormat: shaderFormat,
            vertices: vertices,
            batches: batches,
            passes: passes
        )
    }

    func capturePendingSceneTexture() throws -> GPUTextureProbeResource {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.capturePendingSceneTexture()
    }

    func readbackPresentedFrame() throws -> FrameReadback {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.readbackPresentedFrame(size: drawableSize)
    }

    func writePresentedFramePNG(to path: String) throws {
        let frame = try readbackPresentedFrame()
        let result = path.withCString { pathPointer in
            frame.rgba8.withUnsafeBufferPointer { bytes in
                btbh_gpu_write_png_rgba8(
                    pathPointer,
                    UInt32(frame.size.width),
                    UInt32(frame.size.height),
                    bytes.baseAddress
                )
            }
        }
        guard result == BTBH_GPU_OK else {
            throw GPUCanvasProbeError.operationFailed(
                message: "Could not write GPU presented frame PNG"
            )
        }
    }

    func failClosed(operation: String) {
        print("BTBH_GPU_UNSUPPORTED operation=\(operation)")
        fflush(stdout)
        forcedClose = true
    }

    private func throttleIfNeeded() {
        let now = ProcessInfo.processInfo.systemUptime
        defer {
            previousFrameStart = ProcessInfo.processInfo.systemUptime
        }
        guard targetFramesPerSecond > 0,
              let previousFrameStart else {
            return
        }
        let interval = 1.0 / Double(targetFramesPerSecond)
        let remaining = interval - (now - previousFrameStart)
        if remaining > 0 {
            Thread.sleep(forTimeInterval: remaining)
        }
    }

    private static func targetFrameRate(
        policy: FrameRatePolicy,
        displayRefreshRate: Int
    ) throws -> Int {
        switch policy {
        case .displayRefresh(let minimum):
            guard minimum >= 0, minimum <= Int(Int32.max) else {
                throw GPUPlatformError.invalidWindowDescriptor(
                    "Display-refresh minimum must fit a nonnegative Int32"
                )
            }
            return max(minimum, max(0, displayRefreshRate))
        case .fixed(let framesPerSecond):
            guard framesPerSecond > 0,
                  framesPerSecond <= Int(Int32.max) else {
                throw GPUPlatformError.invalidWindowDescriptor(
                    "Fixed frame rate must fit a positive Int32"
                )
            }
            return framesPerSecond
        case .uncapped:
            return 0
        }
    }
}
