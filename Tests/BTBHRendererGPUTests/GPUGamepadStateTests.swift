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
}
