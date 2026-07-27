import BTBHRenderer
import Foundation

@MainActor
final class GPUCanvas: Canvas2DBackend {
    private let platform: GPUPlatform
    private let resources: GPUResourceBackend
    private var clearColor = RenderColor.black
    private var unsupportedOperation: String?
    private var frameActive = false
    private var vertices: [GPUCanvasVertexProbe] = []
    private var batches: [GPUCanvasBatchProbe] = []
    private var blendMode = CanvasBlendMode.alpha
    private var scissors: [RenderRect] = []

    init(platform: GPUPlatform, resources: GPUResourceBackend) {
        self.platform = platform
        self.resources = resources
    }

    func beginFrame() {
        clearColor = .black
        unsupportedOperation = nil
        frameActive = true
        vertices.removeAll(keepingCapacity: true)
        batches.removeAll(keepingCapacity: true)
        blendMode = .alpha
        scissors.removeAll(keepingCapacity: true)
    }

    func submit(_ command: CanvasCommand) {
        guard frameActive else {
            platform.failClosed(operation: "canvas.submitOutsideFrame")
            return
        }
        switch command {
        case .clear(let color):
            clearColor = color
        case .fillRect(let command):
            appendRect(command.rect, color: command.color)
        case .strokeRect(let command):
            appendStrokeRect(command)
        case .roundedRect(let command):
            appendRoundedRect(command)
        case .horizontalGradient(let command):
            appendGradient(command)
        case .line(let command):
            appendLine(command)
        case .triangle(let command):
            appendTriangle(command)
        case .circle(let command):
            appendCircle(command)
        case .ring(let command):
            appendRing(command)
        case .texturedQuad(let command):
            do {
                try appendTexturedQuad(command)
            } catch {
                reject("canvas.texturedQuad")
            }
        case .text(let command):
            do {
                try appendText(command)
            } catch {
                reject("canvas.text")
            }
        case .pushScissor(let rect):
            scissors.append(rect)
        case .popScissor:
            if scissors.isEmpty {
                reject("canvas.popScissor")
            } else {
                scissors.removeLast()
            }
        case .setBlendMode(let mode):
            blendMode = mode
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
            if vertices.isEmpty {
                try platform.presentClear(clearColor)
            } else {
                try platform.presentCanvas(
                    vertices: vertices,
                    batches: batches,
                    clearColor: clearColor
                )
            }
        } catch {
            platform.failClosed(operation: "canvas.present")
        }
    }

    func readbackPresentedFrame() throws -> FrameReadback {
        try platform.readbackPresentedFrame()
    }

    func writePresentedFramePNG(to path: String) throws {
        try platform.writePresentedFramePNG(to: path)
    }

    private func reject(_ operation: String) {
        if unsupportedOperation == nil {
            unsupportedOperation = operation
        }
    }

    private func appendRect(_ rect: RenderRect, color: RenderColor) {
        guard rect.width > 0, rect.height > 0 else { return }
        let topLeft = RenderVec2(x: rect.x, y: rect.y)
        let topRight = RenderVec2(
            x: rect.x + rect.width,
            y: rect.y
        )
        let bottomRight = RenderVec2(
            x: rect.x + rect.width,
            y: rect.y + rect.height
        )
        let bottomLeft = RenderVec2(
            x: rect.x,
            y: rect.y + rect.height
        )
        appendVertices([
            (topLeft, color), (topRight, color), (bottomRight, color),
            (topLeft, color), (bottomRight, color), (bottomLeft, color)
        ])
    }

    private func appendStrokeRect(_ command: StrokeRectCommand) {
        guard command.thickness > 0 else { return }
        var rect = command.rect
        if command.rasterization == .pixelAligned {
            let inset = command.thickness * 0.5
            rect = .init(
                x: rect.x - inset,
                y: rect.y - inset,
                width: rect.width + command.thickness,
                height: rect.height + command.thickness
            )
        }
        let thickness = min(
            command.thickness,
            min(rect.width, rect.height) * 0.5
        )
        appendRect(.init(
            x: rect.x,
            y: rect.y,
            width: rect.width,
            height: thickness
        ), color: command.color)
        appendRect(.init(
            x: rect.x,
            y: rect.y + rect.height - thickness,
            width: rect.width,
            height: thickness
        ), color: command.color)
        appendRect(.init(
            x: rect.x,
            y: rect.y + thickness,
            width: thickness,
            height: rect.height - thickness * 2
        ), color: command.color)
        appendRect(.init(
            x: rect.x + rect.width - thickness,
            y: rect.y + thickness,
            width: thickness,
            height: rect.height - thickness * 2
        ), color: command.color)
    }

    private func appendGradient(_ command: HorizontalGradientCommand) {
        let rect = command.rect
        guard rect.width > 0, rect.height > 0 else { return }
        let topLeft = RenderVec2(x: rect.x, y: rect.y)
        let topRight = RenderVec2(
            x: rect.x + rect.width,
            y: rect.y
        )
        let bottomRight = RenderVec2(
            x: rect.x + rect.width,
            y: rect.y + rect.height
        )
        let bottomLeft = RenderVec2(
            x: rect.x,
            y: rect.y + rect.height
        )
        appendVertices([
            (topLeft, command.left),
            (topRight, command.right),
            (bottomRight, command.right),
            (topLeft, command.left),
            (bottomRight, command.right),
            (bottomLeft, command.left)
        ])
    }

    private func appendLine(_ command: Line2DCommand) {
        guard command.thickness > 0 else { return }
        let dx = command.end.x - command.start.x
        let dy = command.end.y - command.start.y
        let length = sqrt(dx * dx + dy * dy)
        guard length > 0 else { return }
        let scale = command.thickness * 0.5 / length
        let perpendicular = RenderVec2(
            x: -dy * scale,
            y: dx * scale
        )
        let a = command.start + perpendicular
        let b = command.end + perpendicular
        let c = command.end - perpendicular
        let d = command.start - perpendicular
        appendVertices([
            (a, command.color), (b, command.color), (c, command.color),
            (a, command.color), (c, command.color), (d, command.color)
        ])
    }

    private func appendTriangle(_ command: Triangle2DCommand) {
        if command.outline {
            appendLine(.init(
                start: command.a,
                end: command.b,
                thickness: 1,
                color: command.color
            ))
            appendLine(.init(
                start: command.b,
                end: command.c,
                thickness: 1,
                color: command.color
            ))
            appendLine(.init(
                start: command.c,
                end: command.a,
                thickness: 1,
                color: command.color
            ))
            return
        }
        appendVertices([
            (command.a, command.color),
            (command.b, command.color),
            (command.c, command.color)
        ])
    }

    private func appendCircle(_ command: Circle2DCommand) {
        guard command.radius > 0 else { return }
        if command.outline {
            appendRing(.init(
                center: command.center,
                innerRadius: max(0, command.radius - 1),
                outerRadius: command.radius,
                startDegrees: 0,
                endDegrees: 360,
                segments: 32,
                color: command.color
            ))
            return
        }
        appendFan(
            center: command.center,
            radius: command.radius,
            startDegrees: 0,
            endDegrees: 360,
            segments: 32,
            color: command.color
        )
    }

    private func appendRoundedRect(_ command: RoundedRectCommand) {
        let rect = command.rect
        guard rect.width > 0, rect.height > 0 else { return }
        let radius = min(rect.width, rect.height)
            * min(max(command.roundness, 0), 1)
            * 0.5
        guard radius > 0 else {
            appendRect(rect, color: command.color)
            return
        }
        let segments = max(1, command.segments)
        let center = RenderVec2(
            x: rect.x + rect.width * 0.5,
            y: rect.y + rect.height * 0.5
        )
        var boundary: [RenderVec2] = []
        let corners: [(RenderVec2, Float)] = [
            (.init(
                x: rect.x + rect.width - radius,
                y: rect.y + radius
            ), -90),
            (.init(
                x: rect.x + rect.width - radius,
                y: rect.y + rect.height - radius
            ), 0),
            (.init(
                x: rect.x + radius,
                y: rect.y + rect.height - radius
            ), 90),
            (.init(
                x: rect.x + radius,
                y: rect.y + radius
            ), 180)
        ]
        for (corner, startAngle) in corners {
            for index in 0...segments {
                let angle = startAngle
                    + 90 * Float(index) / Float(segments)
                boundary.append(Self.point(
                    center: corner,
                    radius: radius,
                    degrees: angle
                ))
            }
        }
        appendPolygonFan(
            center: center,
            boundary: boundary,
            color: command.color
        )
    }

    private func appendRing(_ command: Ring2DCommand) {
        let inner = max(0, command.innerRadius)
        let outer = max(inner, command.outerRadius)
        let segments = max(1, command.segments)
        guard outer > 0,
              command.startDegrees != command.endDegrees else {
            return
        }
        var output: [(RenderVec2, RenderColor)] = []
        output.reserveCapacity(segments * 6)
        for index in 0..<segments {
            let t0 = Float(index) / Float(segments)
            let t1 = Float(index + 1) / Float(segments)
            let angle0 = command.startDegrees
                + (command.endDegrees - command.startDegrees) * t0
            let angle1 = command.startDegrees
                + (command.endDegrees - command.startDegrees) * t1
            let outer0 = Self.point(
                center: command.center,
                radius: outer,
                degrees: angle0
            )
            let outer1 = Self.point(
                center: command.center,
                radius: outer,
                degrees: angle1
            )
            let inner0 = Self.point(
                center: command.center,
                radius: inner,
                degrees: angle0
            )
            let inner1 = Self.point(
                center: command.center,
                radius: inner,
                degrees: angle1
            )
            output.append(contentsOf: [
                (outer0, command.color),
                (outer1, command.color),
                (inner1, command.color),
                (outer0, command.color),
                (inner1, command.color),
                (inner0, command.color)
            ])
        }
        appendVertices(output)
    }

    private func appendFan(
        center: RenderVec2,
        radius: Float,
        startDegrees: Float,
        endDegrees: Float,
        segments: Int,
        color: RenderColor
    ) {
        var boundary: [RenderVec2] = []
        boundary.reserveCapacity(segments + 1)
        for index in 0...segments {
            let amount = Float(index) / Float(segments)
            boundary.append(Self.point(
                center: center,
                radius: radius,
                degrees: startDegrees
                    + (endDegrees - startDegrees) * amount
            ))
        }
        appendPolygonFan(
            center: center,
            boundary: boundary,
            color: color
        )
    }

    private func appendPolygonFan(
        center: RenderVec2,
        boundary: [RenderVec2],
        color: RenderColor
    ) {
        guard boundary.count >= 2 else { return }
        var output: [(RenderVec2, RenderColor)] = []
        output.reserveCapacity((boundary.count - 1) * 3)
        for index in 0..<(boundary.count - 1) {
            output.append(contentsOf: [
                (center, color),
                (boundary[index], color),
                (boundary[index + 1], color)
            ])
        }
        appendVertices(output)
    }

    private func appendVertices(
        _ output: [(RenderVec2, RenderColor)]
    ) {
        guard !output.isEmpty else { return }
        let firstVertex = vertices.count
        for (position, color) in output {
            vertices.append(.init(position: position, color: color))
        }
        batches.append(.init(
            firstVertex: firstVertex,
            vertexCount: output.count,
            blendMode: blendMode,
            scissor: scissors.last
        ))
    }

    private func appendTexturedQuad(
        _ command: TexturedQuadCommand
    ) throws {
        let texture = try resources.canvasTexture(command.texture)
        guard texture.size.width > 0,
              texture.size.height > 0,
              command.destination.width != 0,
              command.destination.height != 0 else {
            return
        }
        let local = [
            RenderVec2(
                x: -command.origin.x,
                y: -command.origin.y
            ),
            RenderVec2(
                x: command.destination.width - command.origin.x,
                y: -command.origin.y
            ),
            RenderVec2(
                x: command.destination.width - command.origin.x,
                y: command.destination.height - command.origin.y
            ),
            RenderVec2(
                x: -command.origin.x,
                y: command.destination.height - command.origin.y
            )
        ]
        let radians = command.rotationDegrees * .pi / 180
        let cosine = cos(radians)
        let sine = sin(radians)
        let positions = local.map {
            RenderVec2(
                x: command.destination.x
                    + $0.x * cosine - $0.y * sine,
                y: command.destination.y
                    + $0.x * sine + $0.y * cosine
            )
        }
        let inverseWidth = 1 / Float(texture.size.width)
        let inverseHeight = 1 / Float(texture.size.height)
        let u0 = command.source.x * inverseWidth
        let v0 = command.source.y * inverseHeight
        let u1 = (command.source.x + command.source.width)
            * inverseWidth
        let v1 = (command.source.y + command.source.height)
            * inverseHeight
        appendTexturedVertices([
            (positions[0], .init(x: u0, y: v0)),
            (positions[1], .init(x: u1, y: v0)),
            (positions[2], .init(x: u1, y: v1)),
            (positions[0], .init(x: u0, y: v0)),
            (positions[2], .init(x: u1, y: v1)),
            (positions[3], .init(x: u0, y: v1))
        ], color: command.tint, texture: texture.pointer)
    }

    private func appendText(_ command: TextCommand) throws {
        let font = try resources.canvasFont(command.font)
        guard command.size > 0, command.size.isFinite else { return }
        let measured = try resources.measureText(
            command.text,
            font: command.font,
            size: command.size,
            spacing: command.spacing
        )
        var origin = command.position
        switch command.anchor {
        case .topLeft:
            break
        case .top:
            origin.x -= measured.x * 0.5
        case .center:
            origin.x -= measured.x * 0.5
            origin.y -= measured.y * 0.5
        case .bottomLeft:
            origin.y -= measured.y
        }
        let scale = command.size / Float(font.baseSize)
        let lineAdvance = command.size + font.lineGap * scale
        var cursorX = origin.x
        var baseline = origin.y + font.ascent * scale
        var glyphIndex = 0
        var output: [(RenderVec2, RenderVec2)] = []
        for scalar in command.text.unicodeScalars {
            if scalar.value == 10 {
                cursorX = origin.x
                baseline += lineAdvance
                glyphIndex = 0
                continue
            }
            let glyph = try resources.canvasGlyph(
                font,
                codepoint: scalar.value
            )
            if glyphIndex > 0 {
                cursorX += command.spacing
            }
            let x = cursorX + glyph.offset.x * scale
            let y = baseline + glyph.offset.y * scale
            let width = glyph.size.x * scale
            let height = glyph.size.y * scale
            if width > 0, height > 0 {
                let topLeft = RenderVec2(x: x, y: y)
                let topRight = RenderVec2(x: x + width, y: y)
                let bottomRight = RenderVec2(
                    x: x + width,
                    y: y + height
                )
                let bottomLeft = RenderVec2(x: x, y: y + height)
                output.append(contentsOf: [
                    (topLeft, glyph.uvMinimum),
                    (.init(x: topRight.x, y: topRight.y), .init(
                        x: glyph.uvMaximum.x,
                        y: glyph.uvMinimum.y
                    )),
                    (bottomRight, glyph.uvMaximum),
                    (topLeft, glyph.uvMinimum),
                    (bottomRight, glyph.uvMaximum),
                    (bottomLeft, .init(
                        x: glyph.uvMinimum.x,
                        y: glyph.uvMaximum.y
                    ))
                ])
            }
            cursorX += glyph.advance * scale
            glyphIndex += 1
        }
        appendTexturedVertices(
            output,
            color: command.color,
            texture: font.atlasTexture
        )
    }

    private func appendTexturedVertices(
        _ output: [(RenderVec2, RenderVec2)],
        color: RenderColor,
        texture: OpaquePointer
    ) {
        guard !output.isEmpty else { return }
        let firstVertex = vertices.count
        for (position, textureCoordinate) in output {
            vertices.append(.init(
                position: position,
                textureCoordinate: textureCoordinate,
                color: color
            ))
        }
        batches.append(.init(
            firstVertex: firstVertex,
            vertexCount: output.count,
            blendMode: blendMode,
            scissor: scissors.last,
            texture: texture
        ))
    }

    private static func point(
        center: RenderVec2,
        radius: Float,
        degrees: Float
    ) -> RenderVec2 {
        let radians = degrees * .pi / 180
        return .init(
            x: center.x + cos(radians) * radius,
            y: center.y + sin(radians) * radius
        )
    }
}
