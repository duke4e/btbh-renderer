import BTBHRenderer
import BTBHRendererGPUCore

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
