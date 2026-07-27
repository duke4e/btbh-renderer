import BTBHRenderer

@MainActor
final class GPUCanvas: Canvas2DBackend {
    private let platform: GPUPlatform
    private let resources: GPUResourceBackend
    private var clearColor = RenderColor.black
    private var unsupportedOperation: String?
    private var frameActive = false

    init(platform: GPUPlatform, resources: GPUResourceBackend) {
        self.platform = platform
        self.resources = resources
    }

    func beginFrame() {
        clearColor = .black
        unsupportedOperation = nil
        frameActive = true
    }

    func submit(_ command: CanvasCommand) {
        guard frameActive else {
            platform.failClosed(operation: "canvas.submitOutsideFrame")
            return
        }
        switch command {
        case .clear(let color):
            clearColor = color
        case .fillRect:
            reject("canvas.fillRect")
        case .strokeRect:
            reject("canvas.strokeRect")
        case .roundedRect:
            reject("canvas.roundedRect")
        case .horizontalGradient:
            reject("canvas.horizontalGradient")
        case .line:
            reject("canvas.line")
        case .triangle:
            reject("canvas.triangle")
        case .circle:
            reject("canvas.circle")
        case .ring:
            reject("canvas.ring")
        case .texturedQuad:
            reject("canvas.texturedQuad")
        case .text:
            reject("canvas.text")
        case .pushScissor:
            reject("canvas.pushScissor")
        case .popScissor:
            reject("canvas.popScissor")
        case .setBlendMode:
            reject("canvas.setBlendMode")
        case .pushRenderTarget:
            reject("canvas.pushRenderTarget")
        case .popRenderTarget:
            reject("canvas.popRenderTarget")
        }
    }

    func measureText(
        _ text: String,
        font: FontHandle,
        size: Float,
        spacing: Float
    ) -> RenderVec2 {
        do {
            return try resources.measureText(
                text,
                font: font,
                size: size,
                spacing: spacing
            )
        } catch {
            platform.failClosed(operation: "canvas.measureText")
            return .zero
        }
    }

    func endFrame() {
        guard frameActive else {
            platform.failClosed(operation: "canvas.endFrameWithoutBegin")
            return
        }
        frameActive = false
        if let unsupportedOperation {
            platform.failClosed(operation: unsupportedOperation)
            return
        }
        do {
            try platform.presentClear(clearColor)
        } catch {
            platform.failClosed(operation: "canvas.present")
        }
    }

    func readbackPresentedFrame() throws -> FrameReadback {
        throw GPUBackendUnsupportedError.operation(
            "canvas.readbackPresentedFrame"
        )
    }

    func writePresentedFramePNG(to path: String) throws {
        _ = path
        throw GPUBackendUnsupportedError.operation(
            "canvas.writePresentedFramePNG"
        )
    }

    private func reject(_ operation: String) {
        if unsupportedOperation == nil {
            unsupportedOperation = operation
        }
    }
}
