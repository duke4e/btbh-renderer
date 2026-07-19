public enum TextAnchor: UInt8, Sendable {
    case topLeft, top, center, bottomLeft
}

public enum CanvasBlendMode: UInt8, Sendable {
    case alpha, additive, multiplied, premultipliedAlpha
}

public enum TextureFilter: UInt8, Sendable {
    case nearest, bilinear, trilinear
}

public enum TextureWrap: UInt8, Sendable {
    case repeatMode, clamp, mirrorRepeat, mirrorClamp
}

public struct FillRectCommand: Sendable {
    public let rect: RenderRect
    public let color: RenderColor

    public init(rect: RenderRect, color: RenderColor) {
        self.rect = rect
        self.color = color
    }
}

public struct StrokeRectCommand: Sendable {
    public let rect: RenderRect
    public let thickness: Float
    public let color: RenderColor

    public init(rect: RenderRect, thickness: Float, color: RenderColor) {
        self.rect = rect
        self.thickness = thickness
        self.color = color
    }
}

public struct RoundedRectCommand: Sendable {
    public let rect: RenderRect
    public let roundness: Float
    public let segments: Int
    public let color: RenderColor

    public init(rect: RenderRect, roundness: Float, segments: Int, color: RenderColor) {
        self.rect = rect
        self.roundness = roundness
        self.segments = segments
        self.color = color
    }
}

public struct HorizontalGradientCommand: Sendable {
    public let rect: RenderRect
    public let left: RenderColor
    public let right: RenderColor

    public init(rect: RenderRect, left: RenderColor, right: RenderColor) {
        self.rect = rect
        self.left = left
        self.right = right
    }
}

public struct Line2DCommand: Sendable {
    public let start: RenderVec2
    public let end: RenderVec2
    public let thickness: Float
    public let color: RenderColor

    public init(start: RenderVec2, end: RenderVec2, thickness: Float, color: RenderColor) {
        self.start = start
        self.end = end
        self.thickness = thickness
        self.color = color
    }
}

public struct Triangle2DCommand: Sendable {
    public let a: RenderVec2
    public let b: RenderVec2
    public let c: RenderVec2
    public let color: RenderColor
    public let outline: Bool

    public init(a: RenderVec2, b: RenderVec2, c: RenderVec2, color: RenderColor, outline: Bool) {
        self.a = a
        self.b = b
        self.c = c
        self.color = color
        self.outline = outline
    }
}

public struct Circle2DCommand: Sendable {
    public let center: RenderVec2
    public let radius: Float
    public let color: RenderColor
    public let outline: Bool

    public init(center: RenderVec2, radius: Float, color: RenderColor, outline: Bool) {
        self.center = center
        self.radius = radius
        self.color = color
        self.outline = outline
    }
}

public struct Ring2DCommand: Sendable {
    public let center: RenderVec2
    public let innerRadius: Float
    public let outerRadius: Float
    public let startDegrees: Float
    public let endDegrees: Float
    public let segments: Int
    public let color: RenderColor

    public init(
        center: RenderVec2,
        innerRadius: Float,
        outerRadius: Float,
        startDegrees: Float,
        endDegrees: Float,
        segments: Int,
        color: RenderColor
    ) {
        self.center = center
        self.innerRadius = innerRadius
        self.outerRadius = outerRadius
        self.startDegrees = startDegrees
        self.endDegrees = endDegrees
        self.segments = segments
        self.color = color
    }
}

public struct TexturedQuadCommand: Sendable {
    public let texture: TextureHandle
    public let source: RenderRect
    public let destination: RenderRect
    public let origin: RenderVec2
    public let rotationDegrees: Float
    public let tint: RenderColor

    public init(
        texture: TextureHandle,
        source: RenderRect,
        destination: RenderRect,
        origin: RenderVec2,
        rotationDegrees: Float,
        tint: RenderColor
    ) {
        self.texture = texture
        self.source = source
        self.destination = destination
        self.origin = origin
        self.rotationDegrees = rotationDegrees
        self.tint = tint
    }
}

public struct TextCommand: Sendable {
    public let text: String
    public let font: FontHandle
    public let position: RenderVec2
    public let size: Float
    public let spacing: Float
    public let color: RenderColor
    public let anchor: TextAnchor

    public init(
        text: String,
        font: FontHandle,
        position: RenderVec2,
        size: Float,
        spacing: Float,
        color: RenderColor,
        anchor: TextAnchor
    ) {
        self.text = text
        self.font = font
        self.position = position
        self.size = size
        self.spacing = spacing
        self.color = color
        self.anchor = anchor
    }
}

public enum CanvasCommand: Sendable {
    case clear(RenderColor)
    case fillRect(FillRectCommand)
    case strokeRect(StrokeRectCommand)
    case roundedRect(RoundedRectCommand)
    case horizontalGradient(HorizontalGradientCommand)
    case line(Line2DCommand)
    case triangle(Triangle2DCommand)
    case circle(Circle2DCommand)
    case ring(Ring2DCommand)
    case texturedQuad(TexturedQuadCommand)
    case text(TextCommand)
    case pushScissor(RenderRect)
    case popScissor
    case setBlendMode(CanvasBlendMode)
    case pushRenderTarget(RenderTargetHandle)
    case popRenderTarget
}

@MainActor
public protocol Canvas2DBackend: AnyObject {
    func beginFrame()
    func submit(_ command: CanvasCommand)
    func measureText(_ text: String, font: FontHandle, size: Float, spacing: Float) -> RenderVec2
    func endFrame()
    func readbackPresentedFrame() throws -> FrameReadback
    func writePresentedFramePNG(to path: String) throws
}
