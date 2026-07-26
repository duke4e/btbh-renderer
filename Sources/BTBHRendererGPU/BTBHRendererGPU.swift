import BTBHRenderer
import BTBHRendererGPUCore

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

@MainActor
public final class GPUWindowProbe {
    public var deviceDiagnostics: GPUDeviceDiagnostics {
        deviceProbe.diagnostics
    }
    public private(set) var windowDiagnostics: GPUWindowDiagnostics
    public private(set) var isShutdown = false

    private let deviceProbe: GPUDeviceProbe
    private var window: OpaquePointer?

    public init(
        request: GPUBackendRequest = .platformDefault,
        title: String,
        width: UInt32,
        height: UInt32,
        hidden: Bool,
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
        descriptor.high_pixel_density = 1
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

    public func shutdown() {
        if let window {
            btbh_gpu_window_destroy(window)
            self.window = nil
        }
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
