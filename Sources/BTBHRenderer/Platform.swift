import Foundation

public enum FrameRatePolicy: Equatable, Sendable {
    case displayRefresh(minimum: Int)
    case fixed(Int)
    case uncapped
}

public enum DrawableScalePolicy: Equatable, Sendable {
    case system
    case oneToOne
}

public struct WindowDescriptor: Equatable, Sendable {
    public enum Mode: Equatable, Sendable {
        case windowed
        case fullscreen
        case borderless
    }

    public let title: String
    public let width: Int
    public let height: Int
    public let mode: Mode
    public let frameRatePolicy: FrameRatePolicy
    public let drawableScalePolicy: DrawableScalePolicy

    public init(
        title: String,
        width: Int,
        height: Int,
        mode: Mode,
        frameRatePolicy: FrameRatePolicy = .displayRefresh(minimum: 30),
        drawableScalePolicy: DrawableScalePolicy = .system
    ) {
        self.title = title
        self.width = width
        self.height = height
        self.mode = mode
        self.frameRatePolicy = frameRatePolicy
        self.drawableScalePolicy = drawableScalePolicy
    }
}

public enum InputKey: UInt16, CaseIterable, Sendable {
    case up, down, left, right, enter, keypadEnter, escape, tab, space, backspace, pause
    case leftControl, rightControl, leftShift, rightShift, leftSuper, rightSuper
    case f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12
    case keypad0, keypad1, keypad2, keypad3, keypadAdd, keypadSubtract
    case one, two, three, four, five, six, seven, eight
    case a, b, c, d, e, g, h, i, j, k, m, n, o, p, q, r, s, u, v, w, x, y, z
    case apostrophe, semicolon, leftBracket, rightBracket, minus, equal, comma, period
}

public enum MouseButton: UInt8, Sendable {
    case left, right, middle
}

public enum GamepadButton: UInt8, Sendable {
    case south, east, west, north, leftShoulder, rightShoulder
    case leftStick, rightStick, start, back, guide, dpadUp, dpadDown, dpadLeft, dpadRight
}

public enum NavigationDirection: UInt8, Sendable {
    case up, down, left, right
}

public struct GamepadSnapshot: Equatable, Sendable {
    public let isConnected: Bool
    public let leftStick: RenderVec2
    public let rightStick: RenderVec2
    public let dpad: RenderVec2
    public let leftTrigger: Float
    public let rightTrigger: Float
    public let hasActiveInput: Bool

    public init(
        isConnected: Bool,
        leftStick: RenderVec2,
        rightStick: RenderVec2,
        dpad: RenderVec2,
        leftTrigger: Float,
        rightTrigger: Float,
        hasActiveInput: Bool
    ) {
        self.isConnected = isConnected
        self.leftStick = leftStick
        self.rightStick = rightStick
        self.dpad = dpad
        self.leftTrigger = leftTrigger
        self.rightTrigger = rightTrigger
        self.hasActiveInput = hasActiveInput
    }
}

public struct PlatformFrame: Equatable, Sendable {
    public let deltaSeconds: Float
    public let elapsedSeconds: Double
    public let drawableSize: PixelSize2D
    public let logicalSize: PixelSize2D

    public init(deltaSeconds: Float, elapsedSeconds: Double, drawableSize: PixelSize2D, logicalSize: PixelSize2D) {
        self.deltaSeconds = deltaSeconds
        self.elapsedSeconds = elapsedSeconds
        self.drawableSize = drawableSize
        self.logicalSize = logicalSize
    }
}

public struct PixelSize2D: Equatable, Sendable {
    public let width: Int
    public let height: Int

    public init(width: Int, height: Int) {
        self.width = width
        self.height = height
    }
}

public enum FileDialogKind: Equatable, Sendable {
    case open
    case save
}

public enum FileDialogError: Error, Equatable, Sendable {
    case invalidRequest(String)
    case backendFailure(String)

    public var message: String {
        switch self {
        case let .invalidRequest(message), let .backendFailure(message): message
        }
    }
}

public struct FileDialogRequest: Equatable, Sendable {
    public let kind: FileDialogKind
    public let title: String
    public let allowedExtensions: [String]
    public let allowsMultipleSelection: Bool
    public let initialDirectory: URL?
    public let suggestedFileName: String?

    public init(
        kind: FileDialogKind = .open,
        title: String,
        allowedExtensions: [String],
        allowsMultipleSelection: Bool = false,
        initialDirectory: URL? = nil,
        suggestedFileName: String? = nil
    ) {
        self.kind = kind
        self.title = title
        self.allowedExtensions = allowedExtensions
        self.allowsMultipleSelection = allowsMultipleSelection
        self.initialDirectory = initialDirectory
        self.suggestedFileName = suggestedFileName
    }
}

@MainActor
public protocol GamePlatform: AnyObject {
    func openWindow(_ descriptor: WindowDescriptor) throws
    func closeWindow()
    func pollEvents()
    func beginFrame() -> PlatformFrame
    var shouldClose: Bool { get }
    var isWindowFocused: Bool { get }
    var refreshRate: Int { get }
    var framesPerSecond: Int { get }
    var logicalSize: PixelSize2D { get }
    var drawableSize: PixelSize2D { get }
    var monotonicElapsedSeconds: Double { get }
    var mousePosition: RenderVec2 { get }
    func setMousePosition(_ position: RenderVec2) throws
    var mouseDelta: RenderVec2 { get }
    var mouseWheelDelta: Float { get }
    func isKeyDown(_ key: InputKey) -> Bool
    func wasKeyPressed(_ key: InputKey) -> Bool
    func isMouseButtonDown(_ button: MouseButton) -> Bool
    func wasMouseButtonPressed(_ button: MouseButton) -> Bool
    func wasMouseButtonReleased(_ button: MouseButton) -> Bool
    var gamepad: GamepadSnapshot { get }
    func isGamepadButtonDown(_ button: GamepadButton) -> Bool
    func wasGamepadButtonPressed(_ button: GamepadButton) -> Bool
    func wasGamepadTriggerPressed(left: Bool, threshold: Float) -> Bool
    func wasNavigationPressed(_ direction: NavigationDirection) -> Bool
    func setCursorVisible(_ visible: Bool)
    var isCursorVisible: Bool { get }
    func setClipboardText(_ text: String) throws
    func showOpenFileDialog(
        _ request: FileDialogRequest,
        completion: @escaping @Sendable (Result<[URL], FileDialogError>) -> Void
    )
}
