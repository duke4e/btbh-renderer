import BTBHRenderer
import BTBHRendererGPUCore
import Foundation

public enum GPUPlatformError: Error, Equatable {
    case windowAlreadyOpen
    case unsupportedWindowMode
    case windowNotOpen
    case invalidWindowDescriptor(String)
}

struct GPUSceneAdapterTelemetrySample {
    let packMilliseconds: Double
    let modelBatchBuildMilliseconds: Double
    let prepareMilliseconds: Double
    let cameraPrepareMilliseconds: Double
    let lightingPrepareMilliseconds: Double
    let lightingResolveMilliseconds: Double
    let clearColorResolveMilliseconds: Double
    let postResolveMilliseconds: Double
    let shadowCullMilliseconds: Double
    let batchOrderingMilliseconds: Double
    let batchConsolidationMilliseconds: Double
    let nativePresentMilliseconds: Double
    let pendingDrawCount: Int
    let packedInstanceCount: Int
    let batchCount: Int
    let materialResolutionCount: Int
    let overrideEntryCount: Int
}

struct GPUFramePacing {
    static func delayNanoseconds(
        targetFramesPerSecond: Int,
        previousFrameStart: Double?,
        currentTime: Double
    ) -> UInt64? {
        guard targetFramesPerSecond > 0,
              let previousFrameStart,
              previousFrameStart.isFinite,
              currentTime.isFinite else {
            return nil
        }
        let interval = 1.0 / Double(targetFramesPerSecond)
        let remaining = interval - (currentTime - previousFrameStart)
        guard remaining > 0 else { return nil }
        let nanoseconds = UInt64(
            (remaining * 1_000_000_000).rounded(.down)
        )
        return nanoseconds > 0 ? nanoseconds : nil
    }
}

enum GPUPresentedFramePNG {
    /// The presented frame as the window shows it: opaque. The swapchain's alpha is whatever blending left there
    /// (the scene's edges, a canvas overlay's own alpha), and image viewers draw a PNG's alpha over white, so the
    /// game's captures showed white outlines that the screen never had.
    static func opaqueRGBA8(_ rgba8: [UInt8]) -> [UInt8] {
        var pixels = rgba8
        var alpha = 3
        while alpha < pixels.count {
            pixels[alpha] = 255
            alpha += 4
        }
        return pixels
    }
}

struct GPUSceneShaderPaths {
    let vertexShaderPath: String
    let animatedVertexShaderPath: String
    let fastShadowVertexShaderPath: String
    let fastShadowFragmentShaderPath: String
    let shadowVertexShaderPath: String
    let animatedShadowVertexShaderPath: String
    let shadowFragmentShaderPath: String
    let fragmentShaderPath: String
    let unshadowedFragmentShaderPath: String
    let postVertexShaderPath: String
    let postFragmentShaderPath: String
    let canvasVertexShaderPath: String
    let canvasFragmentShaderPath: String
    let canvasTexturedFragmentShaderPath: String
    let shaderFormat: BTBHGPUShaderFormat

    init(
        request: GPUBackendRequest,
        environment: [String: String],
        executablePath: String
    ) {
        let shaderDirectory: URL
        if let override = environment["BTBH_GPU_SHADER_DIR"],
           !override.isEmpty {
            shaderDirectory = URL(fileURLWithPath: override)
        } else {
            shaderDirectory = URL(fileURLWithPath: executablePath)
                .standardizedFileURL
                .deletingLastPathComponent()
                .appendingPathComponent("GPUShaders")
        }
        let extensionName: String
        switch request {
        case .vulkan:
            extensionName = "spv"
            shaderFormat = BTBH_GPU_SHADER_FORMAT_SPIRV
        case .metal:
            extensionName = "metallib"
            shaderFormat = BTBH_GPU_SHADER_FORMAT_METALLIB
        }
        func path(_ name: String) -> String {
            shaderDirectory
                .appendingPathComponent("\(name).\(extensionName)")
                .path
        }
        vertexShaderPath = path("production_model.vert")
        animatedVertexShaderPath =
            path("production_animated_model.vert")
        fastShadowVertexShaderPath =
            path("production_shadow_fast.vert")
        fastShadowFragmentShaderPath =
            path("production_shadow_fast.frag")
        shadowVertexShaderPath = path("production_shadow.vert")
        animatedShadowVertexShaderPath =
            path("production_animated_shadow.vert")
        shadowFragmentShaderPath = path("production_shadow.frag")
        fragmentShaderPath = path("production_model.frag")
        unshadowedFragmentShaderPath =
            path("production_model_unshadowed.frag")
        postVertexShaderPath = path("production_post.vert")
        postFragmentShaderPath = path("production_post.frag")
        canvasVertexShaderPath = path("production_canvas.vert")
        canvasFragmentShaderPath = path("production_canvas.frag")
        canvasTexturedFragmentShaderPath =
            path("production_canvas_textured.frag")
    }
}

@MainActor
public final class GPUPlatform: GamePlatform {
    private let request: GPUBackendRequest
    private let debugMode: Bool
    private let frameTelemetryEnabled: Bool
    private let sceneShaderPaths: GPUSceneShaderPaths
    var isFrameTelemetryEnabled: Bool { frameTelemetryEnabled }
    private var window: GPUWindowProbe?
    private var gamepadState = GPUGamepadState()
    private var state = GPUPlatformState.empty
    private var forcedClose = false
    private var targetFramesPerSecond = 0
    private var previousFrameStart: TimeInterval?
    private var frameTelemetrySamples: [GPUFrameTelemetrySample] = []
    private var sceneAdapterTelemetry: [GPUSceneAdapterTelemetrySample] = []
    private var sceneEncodeTelemetry: [GPUScenePresentationProbeReport] = []
    private var canvasPresentTelemetryMilliseconds: [Double] = []

    public init(
        request: GPUBackendRequest = .platformDefault,
        debugMode: Bool
    ) {
        self.request = request
        self.debugMode = debugMode
        sceneShaderPaths = .init(
            request: request,
            environment: ProcessInfo.processInfo.environment,
            executablePath: CommandLine.arguments[0]
        )
        frameTelemetryEnabled =
            ProcessInfo.processInfo.environment[
                "BTBH_RENDER_PERF_LOG"
            ] == "1"
    }

    public func openWindow(_ descriptor: WindowDescriptor) throws {
        guard window == nil else {
            throw GPUPlatformError.windowAlreadyOpen
        }
        guard descriptor.mode == .windowed else {
            throw GPUPlatformError.unsupportedWindowMode
        }
        let displayPercent: UInt8
        switch descriptor.sizing {
        case .exact:
            displayPercent = 0
        case .displayPercent(let percent):
            guard (1...100).contains(percent),
                  descriptor.drawableScalePolicy == .system else {
                throw GPUPlatformError.invalidWindowDescriptor(
                    "Display-relative GPU windows need 1...100 percent and the system drawable scale"
                )
            }
            displayPercent = UInt8(percent)
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
            resizable: descriptor.resizable,
            fullscreenToggle: descriptor.fullscreenToggle,
            displayPercent: displayPercent,
            debugMode: debugMode
        )
        window.setFrameTelemetryEnabled(frameTelemetryEnabled)
        if frameTelemetryEnabled {
            let diagnostics = window.windowDiagnostics
#if os(macOS)
            let defaultFramesInFlight = "2"
#else
            let defaultFramesInFlight = "3"
#endif
            let configuredFramesInFlight =
                ProcessInfo.processInfo.environment[
                    "BTBH_GPU_ALLOWED_FRAMES_IN_FLIGHT"
                ] ?? defaultFramesInFlight
            let line =
                "BTBH_GPU_SWAPCHAIN_TELEMETRY present_mode=\(diagnostics.presentMode) allowed_frames_in_flight=\(configuredFramesInFlight)\n"
            FileHandle.standardError.write(Data(line.utf8))
            frameTelemetrySamples.removeAll(keepingCapacity: false)
            frameTelemetrySamples.reserveCapacity(512)
            sceneAdapterTelemetry.removeAll(keepingCapacity: false)
            sceneAdapterTelemetry.reserveCapacity(512)
            sceneEncodeTelemetry.removeAll(
                keepingCapacity: false
            )
            sceneEncodeTelemetry.reserveCapacity(512)
            canvasPresentTelemetryMilliseconds.removeAll(
                keepingCapacity: false
            )
            canvasPresentTelemetryMilliseconds.reserveCapacity(512)
        }
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
        finishFrameTelemetry()
        emitFrameTelemetry()
        window?.shutdown()
        window = nil
        gamepadState = GPUGamepadState()
        state = .empty
        targetFramesPerSecond = 0
        previousFrameStart = nil
    }

    public func pollEvents() {
        guard let window else { return }
        do {
            state = try window.pollPlatform()
            gamepadState.advance(window.gamepadSnapshot())
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

    public var isWindowMinimized: Bool {
        window?.windowDiagnostics.minimized ?? false
    }

    public var closedByFailure: Bool {
        forcedClose
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

    public var gamepad: GamepadSnapshot { gamepadState.snapshot }

    public func isGamepadButtonDown(_ button: GamepadButton) -> Bool {
        gamepadState.isDown(button)
    }

    public func wasGamepadButtonPressed(_ button: GamepadButton) -> Bool {
        gamepadState.wasPressed(button)
    }

    public func wasGamepadTriggerPressed(left: Bool, threshold: Float) -> Bool {
        gamepadState.triggerPressed(left: left, threshold: threshold)
    }

    public func rumbleGamepad(lowFrequency: Float, highFrequency: Float, durationSeconds: Float) {
        func motor(_ value: Float) -> UInt16 { UInt16(max(0, min(1, value)) * 65_535) }
        window?.rumbleGamepad(low: motor(lowFrequency), high: motor(highFrequency),
                              durationMilliseconds: UInt32(max(0, min(10, durationSeconds)) * 1_000))
    }

    public func wasNavigationPressed(
        _ direction: NavigationDirection
    ) -> Bool {
        if gamepadState.navigationPressed(direction) { return true }
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
        do {
            let configuration = try GPUFileDialogConfiguration(
                request: request
            )
            guard let window else {
                completion(.failure(.backendFailure(
                    "GPU window is not open"
                )))
                return
            }
            window.showFileDialog(
                configuration,
                completion: completion
            )
        } catch let error as FileDialogError {
            completion(.failure(error))
        } catch {
            completion(.failure(.backendFailure(
                String(describing: error)
            )))
        }
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
        collectFrameTelemetry()
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

    func loadModel(
        path: String,
        flags: ModelImportFlags
    ) throws -> GPUModelProbeResource {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.loadModel(path: path, flags: flags)
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
        renderSize: PixelSize2D,
        viewProjection: [Float],
        cameraRight: RenderVec3,
        cameraUp: RenderVec3,
        cameraPosition: RenderVec3,
        lighting: GPUSceneLightingProbe,
        post: GPUScenePostProbe,
        instances: [GPUInstanceRecord],
        logicalInstanceCount: Int,
        batches: [GPUSceneGeometryProbeBatch],
        clearColor: RenderColor,
        temporal: GPUSceneTemporalProbe? = nil
    ) throws -> GPUScenePresentationProbeReport {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        let report = try window.presentModelInstances(
            vertexShaderPath: sceneShaderPaths.vertexShaderPath,
            animatedVertexShaderPath:
                sceneShaderPaths.animatedVertexShaderPath,
            fastShadowVertexShaderPath:
                sceneShaderPaths.fastShadowVertexShaderPath,
            fastShadowFragmentShaderPath:
                sceneShaderPaths.fastShadowFragmentShaderPath,
            shadowVertexShaderPath:
                sceneShaderPaths.shadowVertexShaderPath,
            animatedShadowVertexShaderPath:
                sceneShaderPaths.animatedShadowVertexShaderPath,
            shadowFragmentShaderPath:
                sceneShaderPaths.shadowFragmentShaderPath,
            fragmentShaderPath: sceneShaderPaths.fragmentShaderPath,
            unshadowedFragmentShaderPath:
                sceneShaderPaths.unshadowedFragmentShaderPath,
            postVertexShaderPath:
                sceneShaderPaths.postVertexShaderPath,
            postFragmentShaderPath:
                sceneShaderPaths.postFragmentShaderPath,
            shaderFormat: sceneShaderPaths.shaderFormat,
            renderSize: renderSize,
            viewProjection: viewProjection,
            cameraRight: cameraRight,
            cameraUp: cameraUp,
            cameraPosition: cameraPosition,
            lighting: lighting,
            post: post,
            instances: instances,
            logicalInstanceCount: logicalInstanceCount,
            batches: batches,
            clearColor: clearColor,
            temporal: temporal
        )
        if frameTelemetryEnabled {
            sceneEncodeTelemetry.append(report)
        }
        return report
    }

    func recordSceneAdapterTelemetry(
        packMilliseconds: Double,
        modelBatchBuildMilliseconds: Double,
        prepareMilliseconds: Double,
        cameraPrepareMilliseconds: Double,
        lightingPrepareMilliseconds: Double,
        lightingResolveMilliseconds: Double,
        clearColorResolveMilliseconds: Double,
        postResolveMilliseconds: Double,
        shadowCullMilliseconds: Double,
        batchOrderingMilliseconds: Double,
        batchConsolidationMilliseconds: Double,
        nativePresentMilliseconds: Double,
        pendingDrawCount: Int,
        packedInstanceCount: Int,
        batchCount: Int,
        materialResolutionCount: Int,
        overrideEntryCount: Int
    ) {
        guard frameTelemetryEnabled else { return }
        sceneAdapterTelemetry.append(.init(
            packMilliseconds: packMilliseconds,
            modelBatchBuildMilliseconds: modelBatchBuildMilliseconds,
            prepareMilliseconds: prepareMilliseconds,
            cameraPrepareMilliseconds: cameraPrepareMilliseconds,
            lightingPrepareMilliseconds: lightingPrepareMilliseconds,
            lightingResolveMilliseconds: lightingResolveMilliseconds,
            clearColorResolveMilliseconds: clearColorResolveMilliseconds,
            postResolveMilliseconds: postResolveMilliseconds,
            shadowCullMilliseconds: shadowCullMilliseconds,
            batchOrderingMilliseconds: batchOrderingMilliseconds,
            batchConsolidationMilliseconds:
                batchConsolidationMilliseconds,
            nativePresentMilliseconds: nativePresentMilliseconds,
            pendingDrawCount: pendingDrawCount,
            packedInstanceCount: packedInstanceCount,
            batchCount: batchCount,
            materialResolutionCount: materialResolutionCount,
            overrideEntryCount: overrideEntryCount
        ))
    }

    func presentCanvas(
        vertices: [GPUCanvasVertexProbe],
        batches: [GPUCanvasBatchProbe],
        passes: [GPUCanvasPassProbe]
    ) throws {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        // Canvas shader paths resolve once in `init`, like the scene paths, instead of
        // rebuilding URLs and reading the process environment every frame.
        let presentStarted = GPUWindowProbe.monotonicElapsedSeconds
        try window.presentCanvas(
            vertexShaderPath: sceneShaderPaths.canvasVertexShaderPath,
            fragmentShaderPath: sceneShaderPaths.canvasFragmentShaderPath,
            texturedFragmentShaderPath:
                sceneShaderPaths.canvasTexturedFragmentShaderPath,
            shaderFormat: sceneShaderPaths.shaderFormat,
            vertices: vertices,
            batches: batches,
            passes: passes
        )
        if frameTelemetryEnabled {
            canvasPresentTelemetryMilliseconds.append(
                (GPUWindowProbe.monotonicElapsedSeconds - presentStarted)
                    * 1_000
            )
        }
        collectFrameTelemetry()
    }

    func capturePendingSceneTexture() throws -> GPUTextureProbeResource {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.capturePendingSceneTexture()
    }

    func copyPendingSceneTexture(into texture: GPUTextureProbeResource) throws {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        try window.copyPendingSceneTexture(into: texture)
    }

    func readbackPresentedFrame() throws -> FrameReadback {
        guard let window else {
            throw GPUPlatformError.windowNotOpen
        }
        return try window.readbackPresentedFrame(size: drawableSize)
    }

    func requestPresentedFrameReadback() {
        window?.requestPresentedFrameReadback()
    }

    func writePresentedFramePNG(to path: String) throws {
        let frame = try readbackPresentedFrame()
        let pixels = GPUPresentedFramePNG.opaqueRGBA8(frame.rgba8)
        let result = path.withCString { pathPointer in
            pixels.withUnsafeBufferPointer { bytes in
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

    private func collectFrameTelemetry() {
        guard frameTelemetryEnabled, let window else { return }
        window.drainFrameTelemetry(
            into: &frameTelemetrySamples
        )
    }

    private func finishFrameTelemetry() {
        guard frameTelemetryEnabled, let window else { return }
        window.finishFrameTelemetry(
            into: &frameTelemetrySamples
        )
    }

    private func emitFrameTelemetry() {
        guard frameTelemetryEnabled,
              !frameTelemetrySamples.isEmpty ||
              !sceneAdapterTelemetry.isEmpty ||
              !sceneEncodeTelemetry.isEmpty ||
              !canvasPresentTelemetryMilliseconds.isEmpty else {
            return
        }
        let locale = Locale(identifier: "en_US_POSIX")
        var lines = frameTelemetrySamples.map { sample in
            String(
                format:
                    "BTBH_GPU_FRAME_TELEMETRY frame=%llu cpu_submit_ms=%.6f fence_completion_ms=%.6f\n",
                locale: locale,
                sample.frameSerial,
                sample.cpuSubmitMilliseconds,
                sample.fenceCompletionMilliseconds
            )
        }.joined()
        lines += sceneAdapterTelemetry.enumerated().map {
            index, sample in
            String(
                format:
                    "BTBH_GPU_SCENE_ADAPTER_TELEMETRY sample=%llu pack_ms=%.6f model_batch_build_ms=%.6f prepare_ms=%.6f camera_prepare_ms=%.6f lighting_prepare_ms=%.6f lighting_resolve_ms=%.6f clear_color_resolve_ms=%.6f post_resolve_ms=%.6f shadow_cull_ms=%.6f batch_ordering_ms=%.6f batch_consolidation_ms=%.6f native_present_ms=%.6f pending_draws=%llu packed_instances=%llu batches=%llu material_resolutions=%llu override_entries=%llu\n",
                locale: locale,
                UInt64(index + 1),
                sample.packMilliseconds,
                sample.modelBatchBuildMilliseconds,
                sample.prepareMilliseconds,
                sample.cameraPrepareMilliseconds,
                sample.lightingPrepareMilliseconds,
                sample.lightingResolveMilliseconds,
                sample.clearColorResolveMilliseconds,
                sample.postResolveMilliseconds,
                sample.shadowCullMilliseconds,
                sample.batchOrderingMilliseconds,
                sample.batchConsolidationMilliseconds,
                sample.nativePresentMilliseconds,
                UInt64(sample.pendingDrawCount),
                UInt64(sample.packedInstanceCount),
                UInt64(sample.batchCount),
                UInt64(sample.materialResolutionCount),
                UInt64(sample.overrideEntryCount)
            )
        }.joined()
        lines += sceneEncodeTelemetry.enumerated().map {
            index, report in
            String(
                format:
                    "BTBH_GPU_SCENE_ENCODE_TELEMETRY sample=%llu prepared_geometry=%llu animated_geometry=%llu animation_samples=%llu bone_matrices=%llu shadow_pipeline_binds=%llu shadow_pipeline_bind_skips=%llu geometry_pipeline_binds=%llu geometry_pipeline_bind_skips=%llu cpu_encode_ms=%.6f cpu_prepare_ms=%.6f cpu_prepare_setup_ms=%.6f cpu_prepare_bones_ms=%.6f cpu_prepare_bone_count_ms=%.6f cpu_prepare_bone_allocate_ms=%.6f cpu_prepare_bone_sample_ms=%.6f cpu_prepare_pipelines_ms=%.6f cpu_prepare_buffers_ms=%.6f cpu_upload_ms=%.6f cpu_shadow_encode_ms=%.6f cpu_swapchain_acquire_ms=%.6f cpu_target_ensure_ms=%.6f cpu_geometry_encode_ms=%.6f cpu_post_encode_ms=%.6f rc_requested=%llu rc_active=%llu rc_requested_backend=%llu rc_active_backend=%llu rc_pending=%llu rc_fallback=%llu rc_width=%llu rc_height=%llu rc_generation=%llu rc_samples=%llu rc_cpu_encode_ms=%.6f rc_cpu_prepare_ms=%.6f rc_cpu_pipeline_prepare_ms=%.6f rc_cpu_resource_prepare_ms=%.6f hrc_cpu_trace_encode_ms=%.6f hrc_cpu_merge_up_encode_ms=%.6f hrc_cpu_merge_down_encode_ms=%.6f hrc_cpu_reproject_encode_ms=%.6f hrc_cpu_finish_encode_ms=%.6f hrc_cpu_blur_encode_ms=%.6f hrc_cpu_resolve_encode_ms=%.6f hrc_cpu_reconstruct_encode_ms=%.6f hrc_cpu_compose_encode_ms=%.6f hrc_trace_dispatches=%llu hrc_merge_up_dispatches=%llu hrc_merge_down_dispatches=%llu hrc_reproject_dispatches=%llu hrc_finish_dispatches=%llu hrc_blur_dispatches=%llu hrc_resolve_dispatches=%llu hrc_reconstruct_dispatches=%llu hrc_compose_draws=%llu hrc_history_valid=%llu hrc_temporal_phase=%llu hrc_fresh_quadrants=%llu\n",
                locale: locale,
                UInt64(index + 1),
                UInt64(report.preparedGeometryCount),
                UInt64(report.animatedGeometryCount),
                UInt64(report.animationSampleCount),
                UInt64(report.boneMatrixCount),
                UInt64(report.shadowPipelineBindCount),
                UInt64(report.shadowPipelineBindSkipCount),
                UInt64(report.geometryPipelineBindCount),
                UInt64(report.geometryPipelineBindSkipCount),
                report.cpuEncodeMilliseconds,
                report.cpuPrepareMilliseconds,
                report.cpuPrepareSetupMilliseconds,
                report.cpuPrepareBonesMilliseconds,
                report.cpuPrepareBoneCountMilliseconds,
                report.cpuPrepareBoneAllocateMilliseconds,
                report.cpuPrepareBoneSampleMilliseconds,
                report.cpuPreparePipelinesMilliseconds,
                report.cpuPrepareBuffersMilliseconds,
                report.cpuUploadMilliseconds,
                report.cpuShadowEncodeMilliseconds,
                report.cpuSwapchainAcquireMilliseconds,
                report.cpuTargetEnsureMilliseconds,
                report.cpuGeometryEncodeMilliseconds,
                report.cpuPostEncodeMilliseconds,
                UInt64(report.radianceCascadesStatus.requestedTier.rawValue),
                UInt64(report.radianceCascadesStatus.activeTier.rawValue),
                UInt64(report.radianceCascadesStatus.requestedBackend.rawValue),
                UInt64(report.radianceCascadesStatus.activeBackend.rawValue),
                UInt64(report.radianceCascadesStatus.pendingTier.rawValue),
                UInt64(report.radianceCascadesStatus.fallbackReason.rawValue),
                UInt64(report.radianceCascadesStatus.internalSize.width),
                UInt64(report.radianceCascadesStatus.internalSize.height),
                UInt64(report.radianceCascadesStatus.generation),
                UInt64(report.radianceCascadesStatus.sampleCount),
                report.radianceCascadesStatus.cpuEncodeMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuPrepareMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuPipelinePrepareMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuResourcePrepareMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuTraceEncodeMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuMergeUpEncodeMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuMergeDownEncodeMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuReprojectEncodeMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuFinishEncodeMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuBlurEncodeMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuResolveEncodeMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuReconstructEncodeMilliseconds,
                report.radianceCascadesStatus.passTelemetry
                    .cpuComposeEncodeMilliseconds,
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .traceDispatchCount),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .mergeUpDispatchCount),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .mergeDownDispatchCount),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .reprojectDispatchCount),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .finishDispatchCount),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .blurDispatchCount),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .resolveDispatchCount),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .reconstructDispatchCount),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .composeDrawCount),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .temporalHistoryValid ? 1 : 0),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .temporalPhase),
                UInt64(report.radianceCascadesStatus.passTelemetry
                    .freshQuadrantCount)
            )
        }.joined()
        lines += canvasPresentTelemetryMilliseconds.enumerated().map {
            index, milliseconds in
            String(
                format:
                    "BTBH_GPU_CANVAS_PRESENT_TELEMETRY sample=%llu cpu_present_ms=%.6f\n",
                locale: locale,
                UInt64(index + 1),
                milliseconds
            )
        }.joined()
        FileHandle.standardError.write(Data(lines.utf8))
        frameTelemetrySamples.removeAll(keepingCapacity: false)
        sceneAdapterTelemetry.removeAll(keepingCapacity: false)
        sceneEncodeTelemetry.removeAll(
            keepingCapacity: false
        )
        canvasPresentTelemetryMilliseconds.removeAll(
            keepingCapacity: false
        )
    }

    private func throttleIfNeeded() {
        let now = GPUWindowProbe.monotonicElapsedSeconds
        defer {
            previousFrameStart = GPUWindowProbe.monotonicElapsedSeconds
        }
        if let delayNanoseconds = GPUFramePacing.delayNanoseconds(
            targetFramesPerSecond: targetFramesPerSecond,
            previousFrameStart: previousFrameStart,
            currentTime: now
        ) {
            btbh_gpu_delay_precise(delayNanoseconds)
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
