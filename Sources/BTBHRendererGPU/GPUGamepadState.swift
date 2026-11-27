import BTBHRenderer
import BTBHRendererGPUCore

/// The window's gamepad roster (since 0.15.0, `BTBH_GPU_WINDOW_HAS_GAMEPAD_ROSTER`): its slots and their indices.
enum GPUGamepadRoster {
    static let capacity = Int(BTBH_GPU_GAMEPAD_CAPACITY)

    /// The C slot for a Swift index, or nil when it is out of range.
    static func slot(_ index: Int) -> UInt32? {
        index >= 0 && index < capacity ? UInt32(index) : nil
    }
}

/// What kind of pad a roster slot holds (`BTBHGPUGamepadType`, SDL's gamepad type), for button glyphs.
public enum GPUGamepadType: UInt32, Equatable, Sendable, CaseIterable {
    case unknown = 0
    /// A pad SDL knows by its mapping only.
    case standard
    case xbox360
    case xboxOne
    case ps3
    case ps4
    case ps5
    case switchPro
    case joyConLeft
    case joyConRight
    case joyConPair
    case gameCube
}

/// How a pad is connected (`BTBHGPUGamepadConnection`).
public enum GPUGamepadConnection: UInt8, Equatable, Sendable {
    case unknown = 0
    case wired
    case wireless
}

/// A pad's power source (`BTBHGPUGamepadPower`), as SDL reports it when read.
public enum GPUGamepadPower: UInt8, Equatable, Sendable {
    case unknown = 0
    case onBattery
    case noBattery
    case charging
    case charged
}

/// One connected pad's identity and state of charge (`btbh_gpu_window_gamepad_info_at`), from
/// `GPUPlatform.gamepadInfo(at:)`. A slot is a seat, not an identity: to give a returning pad its own player when
/// several pads come and go, compare `serial` (where the pad has one), `guid`, `vendorID` and `productID`.
public struct GPUGamepadInfo: Equatable, Sendable {
    /// SDL's joystick instance id: never 0, and never reused for another pad while the program runs.
    public let instanceID: UInt32
    /// The pad's name as SDL gives it ("Xbox Series X Controller", "PS5 Controller").
    public let name: String
    /// The pad's serial number, or "" where its driver reports none.
    public let serial: String
    /// SDL's GUID for the pad, 16 bytes: the same for every pad of one model on one driver.
    public let guid: [UInt8]
    public let vendorID: UInt16
    public let productID: UInt16
    public let productVersion: UInt16
    public let firmwareVersion: UInt16
    public let type: GPUGamepadType
    public let connection: GPUGamepadConnection
    public let power: GPUGamepadPower
    /// 0...100, or nil when SDL can't tell.
    public let batteryPercent: Int?
    /// The pad has rumble motors.
    public let canRumble: Bool

    /// The GUID as SDL prints it (`SDL_GUIDToString`): 32 lowercase hex digits.
    public var guidString: String {
        guid.map { byte in
            let digits = String(byte, radix: 16)
            return digits.count == 1 ? "0" + digits : digits
        }.joined()
    }

    /// Nil for an empty slot (instance id 0).
    init?(_ native: BTBHGPUGamepadInfo) {
        guard native.instance_id != 0 else { return nil }
        var native = native
        instanceID = native.instance_id
        name = Self.string(from: &native.name)
        serial = Self.string(from: &native.serial)
        guid = withUnsafeBytes(of: &native.guid) { Array($0) }
        vendorID = native.vendor_id
        productID = native.product_id
        productVersion = native.product_version
        firmwareVersion = native.firmware_version
        type = GPUGamepadType(rawValue: native.type) ?? .unknown
        connection = GPUGamepadConnection(rawValue: native.connection) ?? .unknown
        power = GPUGamepadPower(rawValue: native.power) ?? .unknown
        batteryPercent = (0...100).contains(native.battery_percent) ? Int(native.battery_percent) : nil
        canRumble = native.flags & UInt32(BTBH_GPU_GAMEPAD_INFO_RUMBLE) != 0
    }

    /// A NUL-terminated UTF-8 char array, read no further than its size.
    private static func string<T>(from value: inout T) -> String {
        withUnsafeBytes(of: &value) { bytes in
            let end = bytes.firstIndex(of: 0) ?? bytes.count
            return String(decoding: bytes[..<end], as: UTF8.self)
        }
    }
}

/// One snapshot per event poll; reads never consume an edge or poll SDL again.
struct GPUGamepadState {
    private var current = BTBHGPUGamepadSnapshot()
    private var previous = BTBHGPUGamepadSnapshot()

    mutating func advance(_ sample: BTBHGPUGamepadSnapshot) {
        previous = current.instance_id == sample.instance_id ? current : BTBHGPUGamepadSnapshot()
        current = sample
    }

    private func stick(_ x: Float, _ y: Float) -> RenderVec2 {
        x * x + y * y < 0.2 * 0.2 ? .zero : .init(x: x, y: y)
    }

    var snapshot: GamepadSnapshot {
        let left = stick(current.left_x, current.left_y)
        let right = stick(current.right_x, current.right_y)
        let dpad = RenderVec2(
            x: (isDown(.dpadRight) ? 1 : 0) - (isDown(.dpadLeft) ? 1 : 0),
            y: (isDown(.dpadDown) ? 1 : 0) - (isDown(.dpadUp) ? 1 : 0)
        )
        return .init(
            isConnected: current.instance_id != 0,
            leftStick: left, rightStick: right, dpad: dpad,
            leftTrigger: current.left_trigger, rightTrigger: current.right_trigger,
            hasActiveInput: current.buttons_down != 0 || current.buttons_pressed != 0
                || left != .zero || right != .zero
                || current.left_trigger >= 0.2 || current.right_trigger >= 0.2
        )
    }

    func isDown(_ button: GamepadButton) -> Bool {
        current.buttons_down & (1 << button.rawValue) != 0
    }

    func wasPressed(_ button: GamepadButton) -> Bool {
        current.buttons_pressed & (1 << button.rawValue) != 0
    }

    func triggerPressed(left: Bool, threshold: Float) -> Bool {
        let before = left ? previous.left_trigger : previous.right_trigger
        let after = left ? current.left_trigger : current.right_trigger
        return before < threshold && after >= threshold
    }

    func navigationPressed(_ direction: NavigationDirection) -> Bool {
        let beforeLeft = stick(previous.left_x, previous.left_y)
        let afterLeft = stick(current.left_x, current.left_y)
        let beforeRight = stick(previous.right_x, previous.right_y)
        let afterRight = stick(current.right_x, current.right_y)
        func crossed(_ before: Float, _ after: Float, _ threshold: Float) -> Bool {
            threshold < 0 ? before > threshold && after <= threshold : before < threshold && after >= threshold
        }
        switch direction {
        case .up:
            return wasPressed(.dpadUp) || crossed(beforeLeft.y, afterLeft.y, -0.5) || crossed(beforeRight.y, afterRight.y, -0.5)
        case .down:
            return wasPressed(.dpadDown) || crossed(beforeLeft.y, afterLeft.y, 0.5) || crossed(beforeRight.y, afterRight.y, 0.5)
        case .left:
            return wasPressed(.dpadLeft) || crossed(beforeLeft.x, afterLeft.x, -0.5) || crossed(beforeRight.x, afterRight.x, -0.5)
        case .right:
            return wasPressed(.dpadRight) || crossed(beforeLeft.x, afterLeft.x, 0.5) || crossed(beforeRight.x, afterRight.x, 0.5)
        }
    }
}
