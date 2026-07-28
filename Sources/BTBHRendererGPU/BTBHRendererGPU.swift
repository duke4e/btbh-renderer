import BTBHRenderer
import BTBHRendererGPUCore
import Foundation

public enum GPUBackendRequest: String, Equatable, Sendable {
    case vulkan
    case metal

    public init?(exactDriverName: String) {
        self.init(rawValue: exactDriverName)
    }

    public var driverName: String {
        rawValue
    }

    public static var platformDefault: Self {
        #if os(Windows)
        .vulkan
        #elseif os(macOS)
        .metal
        #else
        preconditionFailure(
            "The production GPU renderer supports Windows and macOS only"
        )
        #endif
    }
}

public struct GPUDeviceDiagnostics: Equatable, Sendable {
    public let requestedDriver: String
    public let actualDriver: String
    public let hardwareAccelerated: Bool
    public let supportedFeatureMask: UInt64
    public let missingFeatureMask: UInt64

    public init(
        requestedDriver: String,
        actualDriver: String,
        hardwareAccelerated: Bool,
        supportedFeatureMask: UInt64,
        missingFeatureMask: UInt64
    ) {
        self.requestedDriver = requestedDriver
        self.actualDriver = actualDriver
        self.hardwareAccelerated = hardwareAccelerated
        self.supportedFeatureMask = supportedFeatureMask
        self.missingFeatureMask = missingFeatureMask
    }
}

public enum GPUDeviceProbeError: Error, Equatable {
    case creationFailed(stage: Int32, message: String)
}

@MainActor
public final class GPUDeviceProbe {
    public let diagnostics: GPUDeviceDiagnostics
    public private(set) var isShutdown = false
    fileprivate var device: OpaquePointer?

    public init(
        request: GPUBackendRequest = .platformDefault,
        debugMode: Bool
    ) throws {
        var descriptor = BTBHGPUDeviceDescriptor()
        descriptor.backend = request == .vulkan
            ? BTBH_GPU_BACKEND_VULKAN
            : BTBH_GPU_BACKEND_METAL
        descriptor.required_features = 0x1ff
        descriptor.debug_mode = debugMode ? 1 : 0

        var report = BTBHGPUDeviceReport()
        guard let createdDevice = btbh_gpu_device_create(
            &descriptor,
            &report
        ) else {
            throw GPUDeviceProbeError.creationFailed(
                stage: Int32(report.error_stage.rawValue),
                message: Self.string(from: &report.message)
            )
        }
        device = createdDevice
        diagnostics = GPUDeviceDiagnostics(
            requestedDriver: request.driverName,
            actualDriver: Self.string(
                from: &report.capabilities.driver_name
            ),
            hardwareAccelerated:
                report.capabilities.hardware_accelerated != 0,
            supportedFeatureMask: report.capabilities.features,
            missingFeatureMask: report.validation.missing_features
        )
    }

    public func shutdown() {
        guard let device else {
            isShutdown = true
            return
        }
        btbh_gpu_device_shutdown(device)
        btbh_gpu_device_destroy(device)
        self.device = nil
        isShutdown = true
    }

    deinit {
        if let device {
            btbh_gpu_device_shutdown(device)
            btbh_gpu_device_destroy(device)
        }
    }

    fileprivate static func string<T>(from value: inout T) -> String {
        withUnsafePointer(to: &value) { pointer in
            pointer.withMemoryRebound(
                to: CChar.self,
                capacity: MemoryLayout<T>.size
            ) {
                String(cString: $0)
            }
        }
    }
}

public struct GPUWindowDiagnostics: Equatable, Sendable {
    public let logicalWidth: UInt32
    public let logicalHeight: UInt32
    public let drawableWidth: UInt32
    public let drawableHeight: UInt32
    public let claimed: Bool
    public let minimized: Bool
    public let hidden: Bool
    public let swapchainFormat: Int32
    public let presentMode: Int32

    fileprivate init(report: inout BTBHGPUWindowReport) {
        logicalWidth = report.logical_width
        logicalHeight = report.logical_height
        drawableWidth = report.drawable_width
        drawableHeight = report.drawable_height
        claimed = report.claimed != 0
        minimized = report.minimized != 0
        hidden = report.hidden != 0
        swapchainFormat = Int32(report.swapchain_format.rawValue)
        presentMode = Int32(report.present_mode.rawValue)
    }
}

public enum GPUWindowProbeError: Error, Equatable {
    case operationFailed(
        operation: String,
        stage: Int32,
        message: String
    )
}

enum GPUTextureProbeError: Error, Equatable {
    case unsupportedWrap
    case operationFailed(message: String)
}

struct GPUTextureProbeResource {
    let pointer: OpaquePointer
    let size: PixelSize2D
    let mipCount: Int
}

enum GPUFontProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUFontGlyphProbe {
    let advance: Float
    let offset: RenderVec2
    let size: RenderVec2
    let uvMinimum: RenderVec2
    let uvMaximum: RenderVec2
}

struct GPUFontProbeResource {
    let pointer: OpaquePointer
    let atlasTexture: OpaquePointer
    let baseSize: Int
    let ascent: Float
    let descent: Float
    let lineGap: Float
}

enum GPUModelProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUModelProbeResource {
    let pointer: OpaquePointer
    let metadata: ModelMetadata
    let materials: [GPUSceneMaterialProbe]
}

enum GPUInstanceBufferProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUInstanceBufferProbeResource {
    let pointer: OpaquePointer
    let capacity: Int
    let stride: Int
    let byteCapacity: Int
}

enum GPUCustomMeshProbeError: Error, Equatable {
    case operationFailed(message: String)
}

enum GPUSceneProbeError: Error, Equatable {
    case operationFailed(message: String)
}

enum GPUCanvasProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUCanvasVertexProbe {
    let position: RenderVec2
    let textureCoordinate: RenderVec2
    let color: RenderColor

    init(
        position: RenderVec2,
        textureCoordinate: RenderVec2 = .zero,
        color: RenderColor
    ) {
        self.position = position
        self.textureCoordinate = textureCoordinate
        self.color = color
    }
}

struct GPUCanvasBatchProbe {
    let firstVertex: Int
    let vertexCount: Int
    let blendMode: CanvasBlendMode
    let scissor: RenderRect?
    let texture: OpaquePointer?

    init(
        firstVertex: Int,
        vertexCount: Int,
        blendMode: CanvasBlendMode,
        scissor: RenderRect?,
        texture: OpaquePointer? = nil
    ) {
        self.firstVertex = firstVertex
        self.vertexCount = vertexCount
        self.blendMode = blendMode
        self.scissor = scissor
        self.texture = texture
    }
}

struct GPUCanvasPassProbe {
    let firstBatch: Int
    let batchCount: Int
    let target: OpaquePointer?
    let clearColor: RenderColor?
    let screenClearToken: UInt64?
}

enum GPUSceneGeometryProbe {
    case model(OpaquePointer)
    case customMesh(OpaquePointer)
}

struct GPUAnimationPoseProbe {
    let library: OpaquePointer
    let animationIndex: Int
    let time: Float
}

struct GPUSceneGeometryProbeBatch {
    let geometry: GPUSceneGeometryProbe
    let animation: GPUAnimationPoseProbe?
    let material: GPUSceneMaterialProbe?
    let materialSlot: Int?
    let instanceBuffer: OpaquePointer?
    let firstInstance: Int
    let instanceCount: Int

    init(
        geometry: GPUSceneGeometryProbe,
        animation: GPUAnimationPoseProbe?,
        material: GPUSceneMaterialProbe?,
        materialSlot: Int?,
        instanceBuffer: OpaquePointer? = nil,
        firstInstance: Int,
        instanceCount: Int
    ) {
        self.geometry = geometry
        self.animation = animation
        self.material = material
        self.materialSlot = materialSlot
        self.instanceBuffer = instanceBuffer
        self.firstInstance = firstInstance
        self.instanceCount = instanceCount
    }
}

final class GPUSceneSubmissionScratch {
    var packedInstances: [Float] = []
    var batches: [GPUSceneGeometryProbeBatch] = []

    func reset() {
        packedInstances.removeAll(keepingCapacity: true)
        batches.removeAll(keepingCapacity: true)
    }

    func release() {
        packedInstances = []
        batches = []
    }

    func takePackedInstances() -> [Float] {
        var result: [Float] = []
        swap(&result, &packedInstances)
        return result
    }

    func takeBatches() -> [GPUSceneGeometryProbeBatch] {
        var result: [GPUSceneGeometryProbeBatch] = []
        swap(&result, &batches)
        return result
    }

    func reclaim(
        packedInstances: inout [Float],
        batches: inout [GPUSceneGeometryProbeBatch]
    ) {
        swap(&self.packedInstances, &packedInstances)
        swap(&self.batches, &batches)
    }
}

struct GPUSceneMaterialProbe {
    let albedoTexture: OpaquePointer?
    let albedoColor: RenderColor
    let normalTexture: OpaquePointer?
    let ormTexture: OpaquePointer?
    let normalScale: Float
    let occlusion: Float
    let roughness: Float
    let metalness: Float
    let emissionTexture: OpaquePointer?
    let emissionColor: RenderColor
    let emissionEnergy: Float
    let blendMode: MaterialBlendMode
    let billboardMode: BillboardMode
    let depthCompare: CompareFunction
    let transparency: TransparencyMode
    let unlit: Bool
}

struct GPUSceneLightingProbe {
    let ambientColor: RenderColor
    let ambientEnergy: Float
    let environment: GPUProceduralEnvironmentProjection?
    let directionalDirection: RenderVec3
    let directionalColor: RenderColor
    let directionalEnergy: Float
    let secondaryDirectionalDirection: RenderVec3
    let secondaryDirectionalColor: RenderColor
    let secondaryDirectionalEnergy: Float

    init(
        ambientColor: RenderColor,
        ambientEnergy: Float,
        environment: GPUProceduralEnvironmentProjection? = nil,
        directionalDirection: RenderVec3,
        directionalColor: RenderColor,
        directionalEnergy: Float,
        secondaryDirectionalDirection: RenderVec3,
        secondaryDirectionalColor: RenderColor,
        secondaryDirectionalEnergy: Float
    ) {
        self.ambientColor = ambientColor
        self.ambientEnergy = ambientEnergy
        self.environment = environment
        self.directionalDirection = directionalDirection
        self.directionalColor = directionalColor
        self.directionalEnergy = directionalEnergy
        self.secondaryDirectionalDirection =
            secondaryDirectionalDirection
        self.secondaryDirectionalColor = secondaryDirectionalColor
        self.secondaryDirectionalEnergy = secondaryDirectionalEnergy
    }
}

struct GPUScenePresentationProbeReport {
    let drawableSize: PixelSize2D
    let batchCount: Int
    let primitiveDrawCount: Int
    let preparedGeometryCount: Int
    let instanceCount: Int
    let transientInstanceCount: Int
    let persistentBatchCount: Int
}

struct GPUFrameTelemetrySample: Equatable {
    let frameSerial: UInt64
    let cpuSubmitMilliseconds: Double
    let fenceCompletionMilliseconds: Double
}

struct GPUCustomMeshProbeResource {
    let pointer: OpaquePointer
    let vertexCount: Int
    let indexCount: Int
    let vertexByteCount: Int
}

enum GPUAnimationLibraryProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUAnimationLibraryProbeResource {
    let pointer: OpaquePointer
    let animationCount: Int
    let durations: [Float]
}

@MainActor
enum GPUAnimationLibraryProbe {
    static func load(
        path: String
    ) throws -> GPUAnimationLibraryProbeResource {
        var report = BTBHGPUAnimationLibraryReport()
        let library = path.withCString { pathPointer in
            btbh_gpu_animation_library_load(pathPointer, &report)
        }
        guard let library else {
            throw GPUAnimationLibraryProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        let animationCount = Int(report.animation_count)
        return .init(
            pointer: library,
            animationCount: animationCount,
            durations: (0..<animationCount).map {
                btbh_gpu_animation_library_duration(library, UInt32($0))
            }
        )
    }

    static func destroy(_ library: OpaquePointer) {
        btbh_gpu_animation_library_destroy(library)
    }
}

@MainActor
public final class GPUWindowProbe {
    static var monotonicElapsedSeconds: Double {
        btbh_gpu_monotonic_elapsed_seconds()
    }

    public var deviceDiagnostics: GPUDeviceDiagnostics {
        deviceProbe.diagnostics
    }
    public private(set) var windowDiagnostics: GPUWindowDiagnostics
    public private(set) var isShutdown = false

    private let deviceProbe: GPUDeviceProbe
    private var window: OpaquePointer?
    private var nativeSceneBatches: [BTBHGPUSceneGeometryBatch] = []
    private var sceneCameraRightValues: [Float] = [0, 0, 0]
    private var sceneCameraUpValues: [Float] = [0, 0, 0]
    private var sceneCameraPositionValues: [Float] = [0, 0, 0]
    private var nativeFrameTelemetrySamples =
        [BTBHGPUFrameTelemetrySample](
            repeating: .init(),
            count: 64
        )

    public init(
        request: GPUBackendRequest = .platformDefault,
        title: String,
        width: UInt32,
        height: UInt32,
        hidden: Bool,
        highPixelDensity: Bool = true,
        debugMode: Bool
    ) throws {
        deviceProbe = try GPUDeviceProbe(
            request: request,
            debugMode: debugMode
        )
        var report = BTBHGPUWindowReport()
        var createdWindow: OpaquePointer?
        var descriptor = BTBHGPUWindowDescriptor()
        descriptor.width = width
        descriptor.height = height
        descriptor.hidden = hidden ? 1 : 0
        descriptor.high_pixel_density = highPixelDensity ? 1 : 0
        createdWindow = title.withCString { titlePointer in
            descriptor.title = titlePointer
            return btbh_gpu_window_create(&descriptor, &report)
        }
        guard let createdWindow else {
            deviceProbe.shutdown()
            throw Self.error(
                operation: "create",
                report: &report
            )
        }
        guard let device = deviceProbe.device,
              btbh_gpu_window_claim(
                device,
                createdWindow,
                &report
              ) == BTBH_GPU_OK else {
            btbh_gpu_window_destroy(createdWindow)
            deviceProbe.shutdown()
            throw Self.error(
                operation: "claim",
                report: &report
            )
        }
        window = createdWindow
        windowDiagnostics = GPUWindowDiagnostics(report: &report)
    }

    public func presentClear(
        red: UInt8,
        green: UInt8,
        blue: UInt8,
        alpha: UInt8
    ) throws {
        try perform("present") { window, report in
            btbh_gpu_window_present_clear(
                window,
                red,
                green,
                blue,
                alpha,
                report
            )
        }
    }

    func setFrameTelemetryEnabled(_ enabled: Bool) {
        guard let window, !isShutdown else { return }
        btbh_gpu_window_set_frame_telemetry_enabled(
            window,
            enabled ? 1 : 0
        )
    }

    func pollFrameTelemetry(
        maximumSampleCount: Int = 64
    ) -> [GPUFrameTelemetrySample] {
        var samples: [GPUFrameTelemetrySample] = []
        samples.reserveCapacity(max(0, maximumSampleCount))
        drainFrameTelemetry(
            into: &samples,
            maximumSampleCount: maximumSampleCount
        )
        return samples
    }

    func drainFrameTelemetry(
        into samples: inout [GPUFrameTelemetrySample],
        maximumSampleCount: Int = 64
    ) {
        drainFrameTelemetry(
            into: &samples,
            maximumSampleCount: maximumSampleCount,
            finish: false
        )
    }

    func finishFrameTelemetry(
        into samples: inout [GPUFrameTelemetrySample],
        maximumSampleCount: Int = 256
    ) {
        drainFrameTelemetry(
            into: &samples,
            maximumSampleCount: maximumSampleCount,
            finish: true
        )
    }

    private func drainFrameTelemetry(
        into samples: inout [GPUFrameTelemetrySample],
        maximumSampleCount: Int,
        finish: Bool
    ) {
        guard let window, !isShutdown,
              maximumSampleCount > 0 else {
            return
        }
        let capacity = min(maximumSampleCount, Int(UInt32.max))
        if nativeFrameTelemetrySamples.count < capacity {
            nativeFrameTelemetrySamples = .init(
                repeating: .init(),
                count: capacity
            )
        }
        let count = nativeFrameTelemetrySamples
            .withUnsafeMutableBufferPointer {
            samples in
            if finish {
                return Int(
                    btbh_gpu_window_finish_frame_telemetry(
                        window,
                        samples.baseAddress,
                        UInt32(capacity)
                    )
                )
            }
            return Int(btbh_gpu_window_poll_frame_telemetry(
                window,
                samples.baseAddress,
                UInt32(capacity)
            ))
        }
        for native in nativeFrameTelemetrySamples.prefix(count) {
            samples.append(GPUFrameTelemetrySample(
                frameSerial: native.frame_serial,
                cpuSubmitMilliseconds:
                    native.cpu_submit_milliseconds,
                fenceCompletionMilliseconds:
                    native.fence_completion_milliseconds
            ))
        }
    }

    public func resize(width: UInt32, height: UInt32) throws {
        try perform("resize") { window, report in
            btbh_gpu_window_resize(
                window,
                width,
                height,
                report
            )
        }
    }

    public func minimize() throws {
        try perform("minimize") { window, report in
            btbh_gpu_window_minimize(window, report)
        }
    }

    public func restore() throws {
        try perform("restore") { window, report in
            btbh_gpu_window_restore(window, report)
        }
    }

    func pollPlatform() throws -> GPUPlatformState {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "pollEvents",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var snapshot = BTBHGPUPlatformSnapshot()
        guard btbh_gpu_window_poll_events(
            window,
            &snapshot
        ) == BTBH_GPU_OK else {
            throw GPUWindowProbeError.operationFailed(
                operation: "pollEvents",
                stage: -1,
                message: "SDL GPU event polling failed"
            )
        }
        return GPUPlatformState(snapshot: snapshot)
    }

    func beginPlatformFrame() throws -> GPUPlatformState {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "beginFrame",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUWindowReport()
        var snapshot = BTBHGPUPlatformSnapshot()
        guard btbh_gpu_window_begin_frame(
            window,
            &report,
            &snapshot
        ) == BTBH_GPU_OK else {
            throw Self.error(
                operation: "beginFrame",
                report: &report
            )
        }
        windowDiagnostics = GPUWindowDiagnostics(report: &report)
        return GPUPlatformState(snapshot: snapshot)
    }

    func setMousePosition(x: Float, y: Float) throws {
        try performPlatformOperation("setMousePosition") { window in
            btbh_gpu_window_set_mouse_position(window, x, y)
        }
    }

    func setCursorVisible(_ visible: Bool) throws {
        try performPlatformOperation("setCursorVisible") { window in
            btbh_gpu_window_set_cursor_visible(
                window,
                visible ? 1 : 0
            )
        }
    }

    func setClipboardText(_ text: String) throws {
        try text.withCString { textPointer in
            try performPlatformOperation("setClipboardText") { window in
                btbh_gpu_window_set_clipboard_text(
                    window,
                    textPointer
                )
            }
        }
    }

    func showFileDialog(
        _ configuration: GPUFileDialogConfiguration,
        completion: @escaping @Sendable (
            Result<[URL], FileDialogError>
        ) -> Void
    ) {
        guard let window, !isShutdown else {
            completion(.failure(.backendFailure(
                "GPU window is not open"
            )))
            return
        }
        GPUFileDialogNativeLauncher.launch(
            window: window,
            configuration: configuration,
            completion: completion
        )
    }

    func isKeyDown(_ key: InputKey) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_key_down(window, key.rawValue) != 0
    }

    func wasKeyPressed(_ key: InputKey) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_key_pressed(window, key.rawValue) != 0
    }

    func isMouseButtonDown(_ button: MouseButton) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_mouse_button_down(
            window,
            button.rawValue
        ) != 0
    }

    func wasMouseButtonPressed(_ button: MouseButton) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_mouse_button_pressed(
            window,
            button.rawValue
        ) != 0
    }

    func wasMouseButtonReleased(_ button: MouseButton) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_mouse_button_released(
            window,
            button.rawValue
        ) != 0
    }

    func loadTexture(
        _ descriptor: TextureLoadDescriptor
    ) throws -> GPUTextureProbeResource {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "loadTexture",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        let filter: BTBHGPUTextureFilter
        switch descriptor.filter ?? .bilinear {
        case .nearest:
            filter = BTBH_GPU_TEXTURE_FILTER_NEAREST
        case .bilinear:
            filter = BTBH_GPU_TEXTURE_FILTER_LINEAR
        case .trilinear:
            filter = BTBH_GPU_TEXTURE_FILTER_TRILINEAR
        }
        let wrap: BTBHGPUTextureWrap
        switch descriptor.wrap ?? .clamp {
        case .repeatMode:
            wrap = BTBH_GPU_TEXTURE_WRAP_REPEAT
        case .clamp:
            wrap = BTBH_GPU_TEXTURE_WRAP_CLAMP
        case .mirrorRepeat:
            wrap = BTBH_GPU_TEXTURE_WRAP_MIRROR
        case .mirrorClamp:
            throw GPUTextureProbeError.unsupportedWrap
        }

        var report = BTBHGPUTextureReport()
        let texture = descriptor.path.withCString { path in
            btbh_gpu_window_texture_load(
                window,
                path,
                descriptor.generateMipmaps ? 1 : 0,
                filter,
                wrap,
                &report
            )
        }
        guard let texture else {
            throw GPUTextureProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return GPUTextureProbeResource(
            pointer: texture,
            size: .init(
                width: Int(report.width),
                height: Int(report.height)
            ),
            mipCount: Int(report.mip_count)
        )
    }

    static func destroyTexture(_ texture: OpaquePointer) {
        btbh_gpu_texture_destroy(texture)
    }

    func createRenderTarget(
        size: PixelSize2D,
        filter: TextureFilter
    ) throws -> GPUTextureProbeResource {
        guard let window, !isShutdown,
              size.width > 0,
              size.height > 0,
              size.width <= Int(UInt32.max),
              size.height <= Int(UInt32.max) else {
            throw GPUTextureProbeError.operationFailed(
                message: "Invalid GPU render-target descriptor"
            )
        }
        let nativeFilter: BTBHGPUTextureFilter
        switch filter {
        case .nearest:
            nativeFilter = BTBH_GPU_TEXTURE_FILTER_NEAREST
        case .bilinear:
            nativeFilter = BTBH_GPU_TEXTURE_FILTER_LINEAR
        case .trilinear:
            nativeFilter = BTBH_GPU_TEXTURE_FILTER_TRILINEAR
        }
        var report = BTBHGPUTextureReport()
        guard let texture = btbh_gpu_window_render_target_create(
            window,
            UInt32(size.width),
            UInt32(size.height),
            nativeFilter,
            &report
        ) else {
            throw GPUTextureProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(pointer: texture, size: size, mipCount: 1)
    }

    func loadFont(
        path: String,
        baseSize: UInt32,
        glyphs: [UInt32]
    ) throws -> GPUFontProbeResource {
        guard let window, !isShutdown, !glyphs.isEmpty else {
            throw GPUWindowProbeError.operationFailed(
                operation: "loadFont",
                stage: -1,
                message: "Invalid GPU font load request"
            )
        }
        var report = BTBHGPUFontReport()
        let font = path.withCString { pathPointer in
            glyphs.withUnsafeBufferPointer { glyphBuffer in
                btbh_gpu_window_font_load(
                    window,
                    pathPointer,
                    baseSize,
                    glyphBuffer.baseAddress,
                    UInt32(glyphs.count),
                    &report
                )
            }
        }
        guard let font, let atlas = btbh_gpu_font_atlas(font) else {
            throw GPUFontProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            pointer: font,
            atlasTexture: atlas,
            baseSize: Int(report.base_size),
            ascent: report.ascent,
            descent: report.descent,
            lineGap: report.line_gap
        )
    }

    static func fontGlyph(
        _ font: OpaquePointer,
        codepoint: UInt32
    ) throws -> GPUFontGlyphProbe {
        var glyph = BTBHGPUFontGlyph()
        guard btbh_gpu_font_glyph(
            font,
            codepoint,
            &glyph
        ) == BTBH_GPU_OK else {
            throw GPUFontProbeError.operationFailed(
                message: "Font does not contain codepoint \(codepoint)"
            )
        }
        return .init(
            advance: glyph.advance,
            offset: .init(x: glyph.offset_x, y: glyph.offset_y),
            size: .init(x: glyph.width, y: glyph.height),
            uvMinimum: .init(x: glyph.u0, y: glyph.v0),
            uvMaximum: .init(x: glyph.u1, y: glyph.v1)
        )
    }

    static func destroyFont(_ font: OpaquePointer) {
        btbh_gpu_font_destroy(font)
    }

    func loadModel(path: String) throws -> GPUModelProbeResource {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "loadModel",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUModelReport()
        let model = path.withCString { pathPointer in
            btbh_gpu_window_model_load(
                window,
                pathPointer,
                &report
            )
        }
        guard let model else {
            throw GPUModelProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        let meshCount = Int(report.mesh_count)
        let slots = (0..<meshCount).map {
            Int(btbh_gpu_model_material_slot(model, UInt32($0)))
        }
        let materials = try (0..<Int(report.material_count)).map {
            index in
            var info = BTBHGPUModelMaterialInfo()
            guard btbh_gpu_model_material_info(
                model,
                UInt32(index),
                &info
            ) == BTBH_GPU_OK else {
                btbh_gpu_model_destroy(model)
                throw GPUModelProbeError.operationFailed(
                    message: "Could not query imported material \(index)"
                )
            }
            return GPUSceneMaterialProbe(
                albedoTexture: info.albedo_texture,
                albedoColor: .init(
                    r: info.albedo_red,
                    g: info.albedo_green,
                    b: info.albedo_blue,
                    a: info.albedo_alpha
                ),
                normalTexture: info.normal_texture,
                ormTexture: info.orm_texture,
                normalScale: info.normal_scale,
                occlusion: info.occlusion,
                roughness: info.roughness,
                metalness: info.metalness,
                emissionTexture: info.emission_texture,
                emissionColor: .init(
                    r: info.emission_red,
                    g: info.emission_green,
                    b: info.emission_blue,
                    a: info.emission_alpha
                ),
                emissionEnergy: info.emission_energy,
                blendMode: .mix,
                billboardMode: .disabled,
                depthCompare: .less,
                transparency: .disabled,
                unlit: info.unlit_enabled != 0
            )
        }
        return GPUModelProbeResource(
            pointer: model,
            metadata: .init(
                bounds: .init(
                    minimum: .init(
                        x: report.minimum_x,
                        y: report.minimum_y,
                        z: report.minimum_z
                    ),
                    maximum: .init(
                        x: report.maximum_x,
                        y: report.maximum_y,
                        z: report.maximum_z
                    )
                ),
                meshCount: meshCount,
                materialCount: Int(report.material_count),
                meshMaterialSlots: slots
            ),
            materials: materials
        )
    }

    static func destroyModel(_ model: OpaquePointer) {
        btbh_gpu_model_destroy(model)
    }

    func createInstanceBuffer(
        capacity: UInt32,
        stride: UInt32
    ) throws -> GPUInstanceBufferProbeResource {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "createInstanceBuffer",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUInstanceBufferReport()
        guard let buffer = btbh_gpu_window_instance_buffer_create(
            window,
            capacity,
            stride,
            &report
        ) else {
            throw GPUInstanceBufferProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            pointer: buffer,
            capacity: Int(report.capacity),
            stride: Int(report.stride),
            byteCapacity: Int(report.byte_capacity)
        )
    }

    func uploadInstanceBuffer(
        _ buffer: OpaquePointer,
        bytes: UnsafeRawBufferPointer
    ) throws {
        guard let baseAddress = bytes.baseAddress,
              bytes.count > 0,
              bytes.count <= Int(UInt32.max) else {
            throw GPUInstanceBufferProbeError.operationFailed(
                message: "Invalid GPU instance buffer upload bytes"
            )
        }
        var report = BTBHGPUInstanceBufferReport()
        guard btbh_gpu_instance_buffer_upload(
            buffer,
            0,
            baseAddress,
            UInt32(bytes.count),
            &report
        ) == BTBH_GPU_OK else {
            throw GPUInstanceBufferProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
    }

    static func destroyInstanceBuffer(_ buffer: OpaquePointer) {
        btbh_gpu_instance_buffer_destroy(buffer)
    }

    func createCustomMesh(
        vertices: UnsafeRawBufferPointer,
        vertexCount: UInt32,
        indices: UnsafeBufferPointer<UInt32>?
    ) throws -> GPUCustomMeshProbeResource {
        guard let window, !isShutdown,
              let vertexBaseAddress = vertices.baseAddress,
              vertices.count > 0,
              vertices.count <= Int(UInt32.max) else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: "Invalid GPU custom mesh upload bytes"
            )
        }
        let indexCount = indices?.count ?? 0
        guard indexCount <= Int(UInt32.max) else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: "GPU custom mesh index count exceeds UInt32"
            )
        }
        var report = BTBHGPUCustomMeshReport()
        guard let mesh = btbh_gpu_window_custom_mesh_create(
            window,
            vertexBaseAddress,
            UInt32(vertices.count),
            vertexCount,
            indices?.baseAddress,
            UInt32(indexCount),
            &report
        ) else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            pointer: mesh,
            vertexCount: Int(report.vertex_count),
            indexCount: Int(report.index_count),
            vertexByteCount: Int(report.vertex_byte_count)
        )
    }

    func updateCustomMesh(
        _ mesh: OpaquePointer,
        vertices: UnsafeRawBufferPointer,
        indices: UnsafeBufferPointer<UInt32>?
    ) throws {
        guard let vertexBaseAddress = vertices.baseAddress,
              vertices.count > 0,
              vertices.count <= Int(UInt32.max),
              (indices?.count ?? 0) <= Int(UInt32.max) else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: "Invalid GPU custom mesh update bytes"
            )
        }
        var report = BTBHGPUCustomMeshReport()
        guard btbh_gpu_custom_mesh_update(
            mesh,
            vertexBaseAddress,
            UInt32(vertices.count),
            indices?.baseAddress,
            UInt32(indices?.count ?? 0),
            &report
        ) == BTBH_GPU_OK else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
    }

    static func destroyCustomMesh(_ mesh: OpaquePointer) {
        btbh_gpu_custom_mesh_destroy(mesh)
    }

    func presentModelInstances(
        vertexShaderPath: String,
        animatedVertexShaderPath: String,
        fragmentShaderPath: String,
        shaderFormat: BTBHGPUShaderFormat,
        viewProjection: [Float],
        cameraRight: RenderVec3,
        cameraUp: RenderVec3,
        cameraPosition: RenderVec3,
        lighting: GPUSceneLightingProbe,
        instances: [Float],
        logicalInstanceCount: Int? = nil,
        batches: [GPUSceneGeometryProbeBatch],
        clearColor: RenderColor
    ) throws -> GPUScenePresentationProbeReport {
        let transientInstanceCount = instances.count / 24
        let resolvedLogicalInstanceCount =
            logicalInstanceCount ?? transientInstanceCount
        guard let window, !isShutdown,
              viewProjection.count == 16,
              instances.count.isMultiple(of: 24),
              transientInstanceCount <= Int(UInt32.max),
              resolvedLogicalInstanceCount > 0,
              resolvedLogicalInstanceCount <= Int(UInt32.max),
              cameraPosition.x.isFinite,
              cameraPosition.y.isFinite,
              cameraPosition.z.isFinite,
              Self.sceneLightingIsValid(lighting),
              !batches.isEmpty,
              batches.count <= Int(UInt32.max) else {
            throw GPUSceneProbeError.operationFailed(
                message: "Invalid GPU model instance scene data"
            )
        }
        sceneCameraRightValues[0] = cameraRight.x
        sceneCameraRightValues[1] = cameraRight.y
        sceneCameraRightValues[2] = cameraRight.z
        sceneCameraUpValues[0] = cameraUp.x
        sceneCameraUpValues[1] = cameraUp.y
        sceneCameraUpValues[2] = cameraUp.z
        sceneCameraPositionValues[0] = cameraPosition.x
        sceneCameraPositionValues[1] = cameraPosition.y
        sceneCameraPositionValues[2] = cameraPosition.z
        var nativeLighting = BTBHGPUSceneLighting()
        nativeLighting.ambient_red =
            Float(lighting.ambientColor.r) / 255
        nativeLighting.ambient_green =
            Float(lighting.ambientColor.g) / 255
        nativeLighting.ambient_blue =
            Float(lighting.ambientColor.b) / 255
        nativeLighting.ambient_energy = lighting.ambientEnergy
        nativeLighting.directional_x = lighting.directionalDirection.x
        nativeLighting.directional_y = lighting.directionalDirection.y
        nativeLighting.directional_z = lighting.directionalDirection.z
        nativeLighting.directional_energy = lighting.directionalEnergy
        nativeLighting.directional_red =
            Float(lighting.directionalColor.r) / 255
        nativeLighting.directional_green =
            Float(lighting.directionalColor.g) / 255
        nativeLighting.directional_blue =
            Float(lighting.directionalColor.b) / 255
        nativeLighting.secondary_directional_energy =
            lighting.secondaryDirectionalEnergy
        nativeLighting.secondary_directional_x =
            lighting.secondaryDirectionalDirection.x
        nativeLighting.secondary_directional_y =
            lighting.secondaryDirectionalDirection.y
        nativeLighting.secondary_directional_z =
            lighting.secondaryDirectionalDirection.z
        nativeLighting.secondary_directional_red =
            Float(lighting.secondaryDirectionalColor.r) / 255
        nativeLighting.secondary_directional_green =
            Float(lighting.secondaryDirectionalColor.g) / 255
        nativeLighting.secondary_directional_blue =
            Float(lighting.secondaryDirectionalColor.b) / 255
        if let environment = lighting.environment {
            nativeLighting.ibl_enabled = 1
            withUnsafeMutablePointer(
                to: &nativeLighting.irradiance_sh
            ) {
                $0.withMemoryRebound(
                    to: Float.self,
                    capacity: 27
                ) { values in
                    for (index, coefficient) in
                        environment.irradianceSH.enumerated()
                    {
                        values[index * 3] = coefficient.x
                        values[index * 3 + 1] = coefficient.y
                        values[index * 3 + 2] = coefficient.z
                    }
                }
            }
            Self.populateProceduralEnvironment(
                environment.sky,
                lighting: &nativeLighting
            )
        }
        nativeSceneBatches.removeAll(keepingCapacity: true)
        nativeSceneBatches.reserveCapacity(batches.count)
        for batch in batches {
            guard batch.firstInstance >= 0,
                  batch.firstInstance <= Int(UInt32.max),
                  batch.instanceCount > 0,
                  batch.instanceCount <= Int(UInt32.max),
                  batch.materialSlot == nil ||
                    (batch.materialSlot! >= 0 &&
                     batch.materialSlot! <= Int(Int32.max)) else {
                throw GPUSceneProbeError.operationFailed(
                    message: "GPU model instance batch exceeds UInt32"
                )
            }
            let kind: BTBHGPUSceneGeometryKind
            let model: OpaquePointer?
            let customMesh: OpaquePointer?
            switch batch.geometry {
            case .model(let pointer):
                kind = BTBH_GPU_SCENE_GEOMETRY_MODEL
                model = pointer
                customMesh = nil
            case .customMesh(let pointer):
                kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH
                model = nil
                customMesh = pointer
            }
            if let animation = batch.animation {
                guard animation.animationIndex >= 0,
                      animation.animationIndex <= Int(UInt32.max),
                      animation.time.isFinite else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Invalid GPU animation pose"
                    )
                }
            }
            nativeSceneBatches.append(BTBHGPUSceneGeometryBatch(
                kind: kind,
                model: model,
                custom_mesh: customMesh,
                animation_library: batch.animation?.library,
                animation_index: UInt32(
                    batch.animation?.animationIndex ?? 0
                ),
                animation_time: batch.animation?.time ?? 0,
                material_slot: Int32(batch.materialSlot ?? -1),
                instance_buffer: batch.instanceBuffer,
                first_instance: UInt32(batch.firstInstance),
                instance_count: UInt32(batch.instanceCount),
                albedo_texture: batch.material?.albedoTexture,
                albedo_red: batch.material?.albedoColor.r ?? 255,
                albedo_green: batch.material?.albedoColor.g ?? 255,
                albedo_blue: batch.material?.albedoColor.b ?? 255,
                albedo_alpha: batch.material?.albedoColor.a ?? 255,
                normal_texture: batch.material?.normalTexture,
                orm_texture: batch.material?.ormTexture,
                normal_scale: batch.material?.normalScale ?? 1,
                occlusion: batch.material?.occlusion ?? 1,
                roughness: batch.material?.roughness ?? 1,
                metalness: batch.material?.metalness ?? 0,
                emission_texture: batch.material?.emissionTexture,
                emission_red: batch.material?.emissionColor.r ?? 255,
                emission_green: batch.material?.emissionColor.g ?? 255,
                emission_blue: batch.material?.emissionColor.b ?? 255,
                emission_alpha: batch.material?.emissionColor.a ?? 255,
                emission_energy: batch.material?.emissionEnergy ?? 0,
                blend_mode: Self.sceneBlendMode(
                    batch.material?.blendMode ?? .mix
                ),
                billboard_mode: Self.sceneBillboardMode(
                    batch.material?.billboardMode ?? .disabled
                ),
                depth_compare: Self.sceneDepthCompare(
                    batch.material?.depthCompare ?? .less
                ),
                transparency_enabled:
                    batch.material?.transparency == .alpha ? 1 : 0,
                unlit_enabled:
                    batch.material?.unlit == true ||
                    (batch.material == nil &&
                     kind == BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH)
                        ? 1 : 0,
                reserved: 0
            ))
        }
        var report = BTBHGPUSceneReport()
        let result = vertexShaderPath.withCString { vertexPath in
            animatedVertexShaderPath.withCString { animatedVertexPath in
                fragmentShaderPath.withCString { fragmentPath in
                    viewProjection.withUnsafeBufferPointer { matrix in
                        sceneCameraRightValues.withUnsafeBufferPointer { right in
                            sceneCameraUpValues.withUnsafeBufferPointer { up in
                                sceneCameraPositionValues
                                    .withUnsafeBufferPointer { position in
                                    instances.withUnsafeBufferPointer { instanceData in
                                        nativeSceneBatches.withUnsafeBufferPointer { batchData in
                                            btbh_gpu_window_scene_present_instances(
                                                window,
                                                vertexPath,
                                                animatedVertexPath,
                                                fragmentPath,
                                                shaderFormat,
                                                matrix.baseAddress,
                                                right.baseAddress,
                                                up.baseAddress,
                                                position.baseAddress,
                                                &nativeLighting,
                                                instanceData.baseAddress,
                                                UInt32(transientInstanceCount),
                                                UInt32(
                                                    resolvedLogicalInstanceCount
                                                ),
                                                batchData.baseAddress,
                                                UInt32(nativeSceneBatches.count),
                                                clearColor.r,
                                                clearColor.g,
                                                clearColor.b,
                                                clearColor.a,
                                                &report
                                            )
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        guard result == BTBH_GPU_OK else {
            throw GPUSceneProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            drawableSize: .init(
                width: Int(report.drawable_width),
                height: Int(report.drawable_height)
            ),
            batchCount: Int(report.batch_count),
            primitiveDrawCount: Int(report.primitive_draw_count),
            preparedGeometryCount: Int(report.prepared_geometry_count),
            instanceCount: Int(report.instance_count),
            transientInstanceCount: Int(
                report.transient_instance_count
            ),
            persistentBatchCount: Int(report.persistent_batch_count)
        )
    }

    private static func populateProceduralEnvironment(
        _ sky: ProceduralSkyDescriptor,
        lighting: inout BTBHGPUSceneLighting
    ) {
        lighting.ground_bottom_red =
            Float(sky.groundBottomColor.r) / 255
        lighting.ground_bottom_green =
            Float(sky.groundBottomColor.g) / 255
        lighting.ground_bottom_blue =
            Float(sky.groundBottomColor.b) / 255
        lighting.ground_energy = sky.groundEnergy
        lighting.ground_horizon_red =
            Float(sky.groundHorizonColor.r) / 255
        lighting.ground_horizon_green =
            Float(sky.groundHorizonColor.g) / 255
        lighting.ground_horizon_blue =
            Float(sky.groundHorizonColor.b) / 255
        lighting.ground_horizon_curve = sky.groundHorizonCurve
        lighting.sky_top_red = Float(sky.skyTopColor.r) / 255
        lighting.sky_top_green = Float(sky.skyTopColor.g) / 255
        lighting.sky_top_blue = Float(sky.skyTopColor.b) / 255
        lighting.sky_energy = sky.skyEnergy
        lighting.sky_horizon_red =
            Float(sky.skyHorizonColor.r) / 255
        lighting.sky_horizon_green =
            Float(sky.skyHorizonColor.g) / 255
        lighting.sky_horizon_blue =
            Float(sky.skyHorizonColor.b) / 255
        lighting.sky_horizon_curve = sky.skyHorizonCurve
        lighting.sun_red = Float(sky.sunColor.r) / 255
        lighting.sun_green = Float(sky.sunColor.g) / 255
        lighting.sun_blue = Float(sky.sunColor.b) / 255
        lighting.sun_energy = sky.sunEnergy
        let authoredSunDirection = sky.sunDirection
        let sunDirection = RenderVec3(
            x: -authoredSunDirection.x,
            y: -authoredSunDirection.y,
            z: -authoredSunDirection.z
        ).normalized()
        lighting.sun_direction_x = sunDirection.x
        lighting.sun_direction_y = sunDirection.y
        lighting.sun_direction_z = sunDirection.z
        lighting.sun_size_radians =
            sunDirection.lengthSquared > 0
                ? sky.sunSizeRadians
                : 0
        lighting.sun_curve = sky.sunCurve
    }

    private static func sceneLightingIsValid(
        _ lighting: GPUSceneLightingProbe
    ) -> Bool {
        let values = [
            lighting.ambientEnergy,
            lighting.directionalDirection.x,
            lighting.directionalDirection.y,
            lighting.directionalDirection.z,
            lighting.directionalEnergy,
            lighting.secondaryDirectionalDirection.x,
            lighting.secondaryDirectionalDirection.y,
            lighting.secondaryDirectionalDirection.z,
            lighting.secondaryDirectionalEnergy,
        ]
        guard values.allSatisfy(\.isFinite),
              lighting.ambientEnergy >= 0,
              lighting.directionalEnergy >= 0,
              lighting.secondaryDirectionalEnergy >= 0 else {
            return false
        }
        guard let environment = lighting.environment else {
            return true
        }
        guard environment.irradianceSH.count == 9,
              environment.irradianceSH.allSatisfy({
                $0.x.isFinite && $0.y.isFinite && $0.z.isFinite
              }) else {
            return false
        }
        let sky = environment.sky
        return [
            sky.groundEnergy,
            sky.groundHorizonCurve,
            sky.skyEnergy,
            sky.skyHorizonCurve,
            sky.sunCurve,
            sky.sunDirection.x,
            sky.sunDirection.y,
            sky.sunDirection.z,
            sky.sunEnergy,
            sky.sunSizeRadians,
        ].allSatisfy(\.isFinite)
    }

    private static func sceneBlendMode(
        _ mode: MaterialBlendMode
    ) -> BTBHGPUSceneBlendMode {
        switch mode {
        case .mix:
            BTBH_GPU_SCENE_BLEND_ALPHA
        case .additive:
            BTBH_GPU_SCENE_BLEND_ADDITIVE
        case .multiplied:
            BTBH_GPU_SCENE_BLEND_MULTIPLIED
        case .premultipliedAlpha:
            BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA
        }
    }

    private static func sceneBillboardMode(
        _ mode: BillboardMode
    ) -> BTBHGPUSceneBillboardMode {
        switch mode {
        case .disabled:
            BTBH_GPU_SCENE_BILLBOARD_DISABLED
        case .front:
            BTBH_GPU_SCENE_BILLBOARD_FRONT
        case .yAxis:
            BTBH_GPU_SCENE_BILLBOARD_Y_AXIS
        }
    }

    private static func sceneDepthCompare(
        _ compare: CompareFunction
    ) -> BTBHGPUSceneDepthCompare {
        switch compare {
        case .always:
            BTBH_GPU_SCENE_DEPTH_ALWAYS
        case .lessEqual:
            BTBH_GPU_SCENE_DEPTH_LESS_EQUAL
        case .greater:
            BTBH_GPU_SCENE_DEPTH_GREATER
        case .notEqual:
            BTBH_GPU_SCENE_DEPTH_NOT_EQUAL
        case .less:
            BTBH_GPU_SCENE_DEPTH_LESS
        case .equal:
            BTBH_GPU_SCENE_DEPTH_EQUAL
        case .greaterEqual:
            BTBH_GPU_SCENE_DEPTH_GREATER_EQUAL
        case .never:
            BTBH_GPU_SCENE_DEPTH_NEVER
        }
    }

    func presentCanvas(
        vertexShaderPath: String,
        fragmentShaderPath: String,
        texturedFragmentShaderPath: String,
        shaderFormat: BTBHGPUShaderFormat,
        vertices: [GPUCanvasVertexProbe],
        batches: [GPUCanvasBatchProbe],
        passes: [GPUCanvasPassProbe]
    ) throws {
        guard let window, !isShutdown,
              vertices.count <= Int(UInt32.max),
              batches.count <= Int(UInt32.max),
              !passes.isEmpty,
              passes.count <= Int(UInt32.max) else {
            throw GPUCanvasProbeError.operationFailed(
                message: "Invalid GPU canvas data"
            )
        }
        let nativeVertices = vertices.map {
            BTBHGPUCanvasVertex(
                x: $0.position.x,
                y: $0.position.y,
                u: $0.textureCoordinate.x,
                v: $0.textureCoordinate.y,
                red: Float($0.color.r) / 255,
                green: Float($0.color.g) / 255,
                blue: Float($0.color.b) / 255,
                alpha: Float($0.color.a) / 255
            )
        }
        let nativeBatches = try batches.map { batch in
            guard batch.firstVertex >= 0,
                  batch.firstVertex <= Int(UInt32.max),
                  batch.vertexCount > 0,
                  batch.vertexCount <= Int(UInt32.max) else {
                throw GPUCanvasProbeError.operationFailed(
                    message: "GPU canvas batch exceeds UInt32"
                )
            }
            let nativeBlend: BTBHGPUCanvasBlendMode
            switch batch.blendMode {
            case .alpha:
                nativeBlend = BTBH_GPU_CANVAS_BLEND_ALPHA
            case .additive:
                nativeBlend = BTBH_GPU_CANVAS_BLEND_ADDITIVE
            case .multiplied:
                nativeBlend = BTBH_GPU_CANVAS_BLEND_MULTIPLIED
            case .premultipliedAlpha:
                nativeBlend = BTBH_GPU_CANVAS_BLEND_PREMULTIPLIED_ALPHA
            }
            var native = BTBHGPUCanvasBatch()
            native.first_vertex = UInt32(batch.firstVertex)
            native.vertex_count = UInt32(batch.vertexCount)
            native.texture = batch.texture
            native.blend_mode = nativeBlend
            if let scissor = batch.scissor {
                native.scissor_x = Int32(scissor.x.rounded(.down))
                native.scissor_y = Int32(scissor.y.rounded(.down))
                native.scissor_width = Int32(
                    scissor.width.rounded(.up)
                )
                native.scissor_height = Int32(
                    scissor.height.rounded(.up)
                )
                native.scissor_enabled = 1
            }
            return native
        }
        let nativePasses = try passes.map { pass in
            guard pass.firstBatch >= 0,
                  pass.firstBatch <= Int(UInt32.max),
                  pass.batchCount >= 0,
                  pass.batchCount <= Int(UInt32.max) else {
                throw GPUCanvasProbeError.operationFailed(
                    message: "GPU canvas pass exceeds UInt32"
                )
            }
            var native = BTBHGPUCanvasPass()
            native.first_batch = UInt32(pass.firstBatch)
            native.batch_count = UInt32(pass.batchCount)
            native.target = pass.target
            if let clear = pass.clearColor {
                native.clear_enabled = 1
                native.clear_red = clear.r
                native.clear_green = clear.g
                native.clear_blue = clear.b
                native.clear_alpha = clear.a
            }
            return native
        }
        var report = BTBHGPUCanvasReport()
        let result = vertexShaderPath.withCString { vertexPath in
            fragmentShaderPath.withCString { fragmentPath in
                texturedFragmentShaderPath.withCString {
                    texturedFragmentPath in
                    nativeVertices.withUnsafeBufferPointer { vertexData in
                        nativeBatches.withUnsafeBufferPointer { batchData in
                            nativePasses.withUnsafeBufferPointer {
                                passData in
                                btbh_gpu_window_canvas_present(
                                    window,
                                    vertexPath,
                                    fragmentPath,
                                    texturedFragmentPath,
                                    shaderFormat,
                                    vertexData.baseAddress,
                                    UInt32(nativeVertices.count),
                                    batchData.baseAddress,
                                    UInt32(nativeBatches.count),
                                    passData.baseAddress,
                                    UInt32(nativePasses.count),
                                    &report
                                )
                            }
                        }
                    }
                }
            }
        }
        guard result == BTBH_GPU_OK else {
            throw GPUCanvasProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
    }

    func capturePendingSceneTexture() throws -> GPUTextureProbeResource {
        guard let window, !isShutdown else {
            throw GPUTextureProbeError.operationFailed(
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUTextureReport()
        guard let texture =
            btbh_gpu_window_capture_pending_scene_texture(
                window,
                &report
            ) else {
            throw GPUTextureProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            pointer: texture,
            size: .init(
                width: Int(report.width),
                height: Int(report.height)
            ),
            mipCount: Int(report.mip_count)
        )
    }

    func requestPresentedFrameReadback() {
        guard let window, !isShutdown else { return }
        btbh_gpu_window_request_presented_frame_readback(window)
    }

    func readbackPresentedFrame(
        size: PixelSize2D
    ) throws -> FrameReadback {
        guard let window, !isShutdown,
              size.width > 0,
              size.height > 0,
              size.width <= Int(UInt32.max),
              size.height <= Int(UInt32.max),
              size.width <= Int.max / size.height,
              size.width * size.height <= Int.max / 4 else {
            throw GPUCanvasProbeError.operationFailed(
                message: "Invalid GPU presented-frame readback size"
            )
        }
        var bytes = [UInt8](
            repeating: 0,
            count: size.width * size.height * 4
        )
        guard bytes.count <= Int(UInt32.max) else {
            throw GPUCanvasProbeError.operationFailed(
                message: "GPU presented-frame readback exceeds UInt32"
            )
        }
        let byteCapacity = UInt32(bytes.count)
        var report = BTBHGPUCanvasReport()
        let result = bytes.withUnsafeMutableBufferPointer {
            btbh_gpu_window_readback_presented_frame(
                window,
                $0.baseAddress,
                byteCapacity,
                &report
            )
        }
        guard result == BTBH_GPU_OK else {
            throw GPUCanvasProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        let actualSize = PixelSize2D(
            width: Int(report.drawable_width),
            height: Int(report.drawable_height)
        )
        guard actualSize == size else {
            throw GPUCanvasProbeError.operationFailed(
                message: "GPU presented-frame size changed during readback"
            )
        }
        return .init(size: actualSize, rgba8: bytes)
    }

    public func shutdown() {
        if let window {
            btbh_gpu_window_destroy(window)
            self.window = nil
        }
        nativeSceneBatches = []
        nativeFrameTelemetrySamples = []
        deviceProbe.shutdown()
        isShutdown = true
    }

    deinit {
        if let window {
            btbh_gpu_window_destroy(window)
        }
    }

    private func perform(
        _ operation: String,
        _ body: (
            OpaquePointer,
            UnsafeMutablePointer<BTBHGPUWindowReport>
        ) -> BTBHGPUResult
    ) throws {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: operation,
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUWindowReport()
        guard body(window, &report) == BTBH_GPU_OK else {
            throw Self.error(
                operation: operation,
                report: &report
            )
        }
        windowDiagnostics = GPUWindowDiagnostics(report: &report)
    }

    private func performPlatformOperation(
        _ operation: String,
        _ body: (OpaquePointer) -> BTBHGPUResult
    ) throws {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: operation,
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        guard body(window) == BTBH_GPU_OK else {
            throw GPUWindowProbeError.operationFailed(
                operation: operation,
                stage: -1,
                message: "SDL GPU platform operation failed"
            )
        }
    }

    private static func error(
        operation: String,
        report: inout BTBHGPUWindowReport
    ) -> GPUWindowProbeError {
        .operationFailed(
            operation: operation,
            stage: Int32(report.error_stage.rawValue),
            message: GPUDeviceProbe.string(from: &report.message)
        )
    }
}

struct GPUPlatformState {
    let elapsedSeconds: Double
    let deltaSeconds: Float
    let mousePosition: RenderVec2
    let mouseDelta: RenderVec2
    let mouseWheelDelta: Float
    let refreshRate: Int
    let framesPerSecond: Int
    let shouldClose: Bool
    let isWindowFocused: Bool
    let isCursorVisible: Bool

    init(snapshot: BTBHGPUPlatformSnapshot) {
        elapsedSeconds = snapshot.elapsed_seconds
        deltaSeconds = snapshot.delta_seconds
        mousePosition = .init(
            x: snapshot.mouse_x,
            y: snapshot.mouse_y
        )
        mouseDelta = .init(
            x: snapshot.mouse_delta_x,
            y: snapshot.mouse_delta_y
        )
        mouseWheelDelta = snapshot.mouse_wheel_y
        refreshRate = Int(snapshot.refresh_rate)
        framesPerSecond = Int(snapshot.frames_per_second)
        shouldClose = snapshot.should_close != 0
        isWindowFocused = snapshot.focused != 0
        isCursorVisible = snapshot.cursor_visible != 0
    }

    static let empty = Self(
        elapsedSeconds: 0,
        deltaSeconds: 0,
        mousePosition: .zero,
        mouseDelta: .zero,
        mouseWheelDelta: 0,
        refreshRate: 0,
        framesPerSecond: 0,
        shouldClose: false,
        isWindowFocused: false,
        isCursorVisible: true
    )

    func settingCursorVisible(_ visible: Bool) -> Self {
        Self(
            elapsedSeconds: elapsedSeconds,
            deltaSeconds: deltaSeconds,
            mousePosition: mousePosition,
            mouseDelta: mouseDelta,
            mouseWheelDelta: mouseWheelDelta,
            refreshRate: refreshRate,
            framesPerSecond: framesPerSecond,
            shouldClose: shouldClose,
            isWindowFocused: isWindowFocused,
            isCursorVisible: visible
        )
    }

    init(
        elapsedSeconds: Double,
        deltaSeconds: Float,
        mousePosition: RenderVec2,
        mouseDelta: RenderVec2,
        mouseWheelDelta: Float,
        refreshRate: Int,
        framesPerSecond: Int,
        shouldClose: Bool,
        isWindowFocused: Bool,
        isCursorVisible: Bool
    ) {
        self.elapsedSeconds = elapsedSeconds
        self.deltaSeconds = deltaSeconds
        self.mousePosition = mousePosition
        self.mouseDelta = mouseDelta
        self.mouseWheelDelta = mouseWheelDelta
        self.refreshRate = refreshRate
        self.framesPerSecond = framesPerSecond
        self.shouldClose = shouldClose
        self.isWindowFocused = isWindowFocused
        self.isCursorVisible = isCursorVisible
    }
}
