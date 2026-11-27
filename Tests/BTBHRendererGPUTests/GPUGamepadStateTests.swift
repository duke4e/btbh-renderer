import BTBHRenderer
import BTBHRendererGPUCore
import XCTest
@testable import BTBHRendererGPU

final class GPUGamepadStateTests: XCTestCase {
    func testButtonsSticksTriggersAndMenuEdges() {
        var state = GPUGamepadState()
        var raw = BTBHGPUGamepadSnapshot()
        raw.instance_id = 1
        raw.buttons_down = 1 << GamepadButton.south.rawValue
        raw.buttons_pressed = raw.buttons_down
        raw.left_x = 0.75
        raw.right_y = -0.8
        raw.left_trigger = 0.8
        state.advance(raw)
        XCTAssertTrue(state.snapshot.isConnected)
        XCTAssertTrue(state.snapshot.hasActiveInput)
        XCTAssertEqual(state.snapshot.leftStick.x, 0.75)
        XCTAssertTrue(state.isDown(.south))
        XCTAssertTrue(state.wasPressed(.south))
        XCTAssertTrue(state.triggerPressed(left: true, threshold: 0.5))
        XCTAssertTrue(state.navigationPressed(.right))
        XCTAssertTrue(state.navigationPressed(.up))
        raw.buttons_pressed = 0
        state.advance(raw)
        XCTAssertTrue(state.isDown(.south))
        XCTAssertFalse(state.wasPressed(.south))
        XCTAssertFalse(state.triggerPressed(left: true, threshold: 0.5))
        XCTAssertFalse(state.navigationPressed(.right))
        XCTAssertFalse(state.navigationPressed(.up))
    }

    func testDeadzoneTapAndDisconnectClearState() {
        var state = GPUGamepadState()
        var raw = BTBHGPUGamepadSnapshot()
        raw.instance_id = 8
        raw.left_x = 0.1
        raw.left_y = -0.1
        state.advance(raw)
        XCTAssertEqual(state.snapshot.leftStick, .zero)
        XCTAssertFalse(state.snapshot.hasActiveInput)
        // A down/up pair between polls must still produce one press.
        raw.buttons_pressed = 1 << GamepadButton.start.rawValue
        state.advance(raw)
        XCTAssertTrue(state.wasPressed(.start))
        XCTAssertFalse(state.isDown(.start))
        XCTAssertTrue(state.snapshot.hasActiveInput)
        state.advance(BTBHGPUGamepadSnapshot())
        XCTAssertFalse(state.snapshot.isConnected)
        XCTAssertFalse(state.wasPressed(.start))
        XCTAssertFalse(state.snapshot.hasActiveInput)
    }

    func testDPadAndReplacementControllerHaveFreshEdges() {
        var state = GPUGamepadState()
        var raw = BTBHGPUGamepadSnapshot()
        raw.instance_id = 1
        raw.buttons_down = 1 << GamepadButton.dpadDown.rawValue
        raw.buttons_pressed = raw.buttons_down
        raw.right_trigger = 0.9
        state.advance(raw)
        XCTAssertEqual(state.snapshot.dpad.y, 1)
        XCTAssertTrue(state.navigationPressed(.down))
        state.advance(raw)
        XCTAssertFalse(state.triggerPressed(left: false, threshold: 0.5))
        raw.instance_id = 2
        state.advance(raw)
        XCTAssertTrue(state.triggerPressed(left: false, threshold: 0.5))
    }

    func testNativeGamepadSnapshotABI() {
        XCTAssertEqual(MemoryLayout<BTBHGPUGamepadSnapshot>.size, 36)
        XCTAssertEqual(GamepadButton.dpadRight.rawValue, 14)
    }

    func testNativeGamepadInfoABI() {
        XCTAssertEqual(MemoryLayout<BTBHGPUGamepadInfo>.size, 256)
        XCTAssertEqual(MemoryLayout<BTBHGPUGamepadInfo>.offset(of: \.guid), 16)
        XCTAssertEqual(MemoryLayout<BTBHGPUGamepadInfo>.offset(of: \.flags), 32)
        XCTAssertEqual(MemoryLayout<BTBHGPUGamepadInfo>.offset(of: \.battery_percent), 38)
        XCTAssertEqual(MemoryLayout<BTBHGPUGamepadInfo>.offset(of: \.name), 40)
        XCTAssertEqual(MemoryLayout<BTBHGPUGamepadInfo>.offset(of: \.serial), 168)
        XCTAssertEqual(MemoryLayout<BTBHGPUGamepadInfo>.offset(of: \.reserved), 232)
        XCTAssertEqual(BTBH_GPU_WINDOW_HAS_GAMEPAD_ROSTER, 1)
        XCTAssertEqual(GPUGamepadRoster.capacity, 8)
        XCTAssertEqual(GPUGamepadType.allCases.count, 12)
        XCTAssertEqual(GPUGamepadType.gameCube.rawValue, UInt32(BTBH_GPU_GAMEPAD_TYPE_GAMECUBE.rawValue))
        XCTAssertEqual(GPUGamepadType.ps5.rawValue, UInt32(BTBH_GPU_GAMEPAD_TYPE_PS5.rawValue))
        XCTAssertEqual(GPUGamepadConnection.wireless.rawValue, UInt8(BTBH_GPU_GAMEPAD_CONNECTION_WIRELESS.rawValue))
        XCTAssertEqual(GPUGamepadPower.charged.rawValue, UInt8(BTBH_GPU_GAMEPAD_POWER_CHARGED.rawValue))
    }

    func testRosterIndicesOutsideTheCapacityHaveNoSlot() {
        XCTAssertNil(GPUGamepadRoster.slot(-1))
        XCTAssertEqual(GPUGamepadRoster.slot(0), 0)
        XCTAssertEqual(GPUGamepadRoster.slot(7), 7)
        XCTAssertNil(GPUGamepadRoster.slot(8))
        XCTAssertNil(GPUGamepadRoster.slot(Int.max))
    }

    func testGamepadInfoDecodesIdentityAndEmptySlot() throws {
        XCTAssertNil(GPUGamepadInfo(BTBHGPUGamepadInfo()))
        var native = BTBHGPUGamepadInfo()
        native.instance_id = 42
        native.type = UInt32(BTBH_GPU_GAMEPAD_TYPE_PS5.rawValue)
        native.vendor_id = 0x054c
        native.product_id = 0x0ce6
        native.product_version = 0x0100
        native.firmware_version = 0x0200
        native.flags = UInt32(BTBH_GPU_GAMEPAD_INFO_RUMBLE)
        native.connection = UInt8(BTBH_GPU_GAMEPAD_CONNECTION_WIRELESS.rawValue)
        native.power = UInt8(BTBH_GPU_GAMEPAD_POWER_ON_BATTERY.rawValue)
        native.battery_percent = 55
        withUnsafeMutableBytes(of: &native.guid) { $0.copyBytes(from: (0..<16).map { UInt8($0 * 17) }) }
        withUnsafeMutableBytes(of: &native.name) { $0.copyBytes(from: Array("PS5 Controller Ž".utf8)) }
        withUnsafeMutableBytes(of: &native.serial) { $0.copyBytes(from: Array("a0:b1:c2:d3:e4:f5".utf8)) }
        let info = try XCTUnwrap(GPUGamepadInfo(native))
        XCTAssertEqual(info.instanceID, 42)
        XCTAssertEqual(info.name, "PS5 Controller Ž")
        XCTAssertEqual(info.serial, "a0:b1:c2:d3:e4:f5")
        XCTAssertEqual(info.guid.count, 16)
        XCTAssertEqual(info.guidString, "00112233445566778899aabbccddeeff")
        XCTAssertEqual(info.vendorID, 0x054c)
        XCTAssertEqual(info.productID, 0x0ce6)
        XCTAssertEqual(info.productVersion, 0x0100)
        XCTAssertEqual(info.firmwareVersion, 0x0200)
        XCTAssertEqual(info.type, .ps5)
        XCTAssertEqual(info.connection, .wireless)
        XCTAssertEqual(info.power, .onBattery)
        XCTAssertEqual(info.batteryPercent, 55)
        XCTAssertTrue(info.canRumble)

        // Unknown values read as unknown; a battery SDL can't read is nil; no serial is "".
        native.type = 99
        native.connection = 9
        native.power = 9
        native.battery_percent = -1
        native.flags = 0
        withUnsafeMutableBytes(of: &native.serial) { $0.copyBytes(from: [UInt8](repeating: 0, count: $0.count)) }
        let unknown = try XCTUnwrap(GPUGamepadInfo(native))
        XCTAssertEqual(unknown.type, .unknown)
        XCTAssertEqual(unknown.connection, .unknown)
        XCTAssertEqual(unknown.power, .unknown)
        XCTAssertNil(unknown.batteryPercent)
        XCTAssertEqual(unknown.serial, "")
        XCTAssertFalse(unknown.canRumble)

        // A name that fills the array (no terminator) is read no further than the array.
        withUnsafeMutableBytes(of: &native.name) { $0.copyBytes(from: [UInt8](repeating: 0x41, count: $0.count)) }
        XCTAssertEqual(try XCTUnwrap(GPUGamepadInfo(native)).name, String(repeating: "A", count: 128))
    }
}

/// The roster through `GPUPlatform`. Whatever pads this machine has connected, the reads must agree with each other;
/// these tests never rumble a connected pad. The virtual-pad checks are the C test `btbh_gpu_gamepad_roster`.
@preconcurrency @MainActor
final class GPUGamepadRosterTests: XCTestCase {
    func testRosterWithoutWindowIsEmpty() {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        XCTAssertEqual(GPUPlatform.gamepadCapacity, 8)
        XCTAssertEqual(platform.gamepadCount, 0)
        for index in [-1, 0, 7, 8, 100] {
            XCTAssertFalse(platform.gamepad(at: index).isConnected)
            XCTAssertNil(platform.gamepadInfo(at: index))
            XCTAssertFalse(platform.isGamepadButtonDown(.south, at: index))
            XCTAssertFalse(platform.wasGamepadButtonPressed(.south, at: index))
            XCTAssertFalse(platform.wasGamepadTriggerPressed(left: true, threshold: 0.5, at: index))
            XCTAssertFalse(platform.wasGamepadNavigationPressed(.up, at: index))
            platform.rumbleGamepad(at: index, lowFrequency: 1, highFrequency: 1, durationSeconds: 0.1)
        }
    }

    func testRosterOnAnOpenWindowAgreesWithItself() throws {
        let platform = GPUPlatform(request: .platformDefault, debugMode: true)
        try platform.openWindow(.init(
            title: "BTBH GPU gamepad roster test",
            width: 320,
            height: 180,
            mode: .windowed,
            drawableScalePolicy: .oneToOne
        ))
        defer { platform.closeWindow() }
        platform.pollEvents()
        let count = platform.gamepadCount
        XCTAssertLessThanOrEqual(count, GPUPlatform.gamepadCapacity)
        XCTAssertEqual(platform.gamepad.isConnected, count > 0)
        var instanceIDs = Set<UInt32>()
        for index in 0..<GPUPlatform.gamepadCapacity {
            let info = platform.gamepadInfo(at: index)
            XCTAssertEqual(platform.gamepad(at: index).isConnected, info != nil, "slot \(index)")
            if index >= count { XCTAssertNil(info, "slot \(index) is past the count") }
            if let info {
                XCTAssertNotEqual(info.instanceID, 0)
                XCTAssertTrue(instanceIDs.insert(info.instanceID).inserted, "one pad, one slot")
                XCTAssertEqual(info.guid.count, 16)
            }
        }
        if count > 0 { XCTAssertNotNil(platform.gamepadInfo(at: count - 1), "the highest slot is occupied") }
        XCTAssertNil(platform.gamepadInfo(at: GPUPlatform.gamepadCapacity))
        XCTAssertFalse(platform.gamepad(at: GPUPlatform.gamepadCapacity).isConnected)
        platform.rumbleGamepad(at: GPUPlatform.gamepadCapacity, lowFrequency: 1, highFrequency: 1, durationSeconds: 0.1)
        platform.closeWindow()
        XCTAssertEqual(platform.gamepadCount, 0)
        XCTAssertNil(platform.gamepadInfo(at: 0))
    }
}
