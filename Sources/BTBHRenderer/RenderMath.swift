import BTBHRendererCore
import Foundation

public struct RenderVec2: Hashable, Sendable {
    public var x: Float
    public var y: Float
    public init(x: Float, y: Float) { self.x = x; self.y = y }
    public static let zero = Self(x: 0, y: 0)
    public static let one = Self(x: 1, y: 1)
    public static let up = Self(x: 0, y: -1)
    public static let down = Self(x: 0, y: 1)
    public static let left = Self(x: -1, y: 0)
    public static let right = Self(x: 1, y: 0)
    public var lengthSquared: Float { x * x + y * y }
    public var length: Float { sqrt(lengthSquared) }

    public func distance(to target: Self) -> Float {
        sqrt(distanceSquared(to: target))
    }

    public func distanceSquared(to target: Self) -> Float {
        let dx = x - target.x
        let dy = y - target.y
        return dx * dx + dy * dy
    }

    public func normalized() -> Self {
        let magnitude = length
        guard magnitude > 0 else { return .zero }
        let inverseMagnitude = 1 / magnitude
        return Self(x: x * inverseMagnitude, y: y * inverseMagnitude)
    }

    public func lerp(to target: Self, amount: Float) -> Self {
        Self(
            x: x + amount * (target.x - x),
            y: y + amount * (target.y - y)
        )
    }

    public func rotated(by radians: Float) -> Self {
        let cosine = cos(radians)
        let sine = sin(radians)
        return Self(
            x: x * cosine - y * sine,
            y: x * sine + y * cosine
        )
    }

    public func isInsideCircle(center: Self, radius: Float) -> Bool {
        distanceSquared(to: center) <= radius * radius
    }

    public static func circlesIntersect(
        center1: Self,
        radius1: Float,
        center2: Self,
        radius2: Float
    ) -> Bool {
        let radiusSum = radius1 + radius2
        return center1.distanceSquared(to: center2) <= radiusSum * radiusSum
    }

    public static func + (lhs: Self, rhs: Self) -> Self { Self(x: lhs.x + rhs.x, y: lhs.y + rhs.y) }
    public static func - (lhs: Self, rhs: Self) -> Self { Self(x: lhs.x - rhs.x, y: lhs.y - rhs.y) }
    public static func * (lhs: Self, rhs: Float) -> Self { Self(x: lhs.x * rhs, y: lhs.y * rhs) }
    public static func * (lhs: Float, rhs: Self) -> Self { rhs * lhs }
    public static func * (lhs: Self, rhs: Self) -> Self { Self(x: lhs.x * rhs.x, y: lhs.y * rhs.y) }
    public static func / (lhs: Self, rhs: Float) -> Self { Self(x: lhs.x / rhs, y: lhs.y / rhs) }
    public static func / (lhs: Self, rhs: Self) -> Self { Self(x: lhs.x / rhs.x, y: lhs.y / rhs.y) }
    public static func += (lhs: inout Self, rhs: Self) { lhs = lhs + rhs }
    public static func -= (lhs: inout Self, rhs: Self) { lhs = lhs - rhs }

    var core: BTBHVec2 { BTBHVec2(x: x, y: y) }
    init(core: BTBHVec2) { self.init(x: core.x, y: core.y) }
}

public struct RenderVec3: Hashable, Sendable {
    public var x: Float
    public var y: Float
    public var z: Float
    public init(x: Float, y: Float, z: Float) { self.x = x; self.y = y; self.z = z }
    public static let zero = Self(x: 0, y: 0, z: 0)
    public static let one = Self(x: 1, y: 1, z: 1)
    public static let forward = Self(x: 0, y: 0, z: 1)
    public static let backward = Self(x: 0, y: 0, z: -1)
    public static let up = Self(x: 0, y: 1, z: 0)
    public static let down = Self(x: 0, y: -1, z: 0)
    public static let left = Self(x: 1, y: 0, z: 0)
    public static let right = Self(x: -1, y: 0, z: 0)

    public var lengthSquared: Float { x * x + y * y + z * z }
    public var length: Float { sqrt(lengthSquared) }

    public func distance(to target: Self) -> Float {
        sqrt(distanceSquared(to: target))
    }

    public func distanceSquared(to target: Self) -> Float {
        let dx = target.x - x
        let dy = target.y - y
        let dz = target.z - z
        return dx * dx + dy * dy + dz * dz
    }

    public func normalized() -> Self {
        let magnitude = length
        guard magnitude != 0 else { return self }
        let inverseMagnitude = 1 / magnitude
        return Self(x: x * inverseMagnitude, y: y * inverseMagnitude, z: z * inverseMagnitude)
    }

    public func lerp(to target: Self, amount: Float) -> Self {
        Self(
            x: x + amount * (target.x - x),
            y: y + amount * (target.y - y),
            z: z + amount * (target.z - z)
        )
    }

    public func dot(_ rhs: Self) -> Float { x * rhs.x + y * rhs.y + z * rhs.z }
    public func cross(_ rhs: Self) -> Self {
        Self(x: y * rhs.z - z * rhs.y, y: z * rhs.x - x * rhs.z, z: x * rhs.y - y * rhs.x)
    }

    public func rotated(by quaternion: RenderQuaternion) -> Self {
        let qx = quaternion.x
        let qy = quaternion.y
        let qz = quaternion.z
        let qw = quaternion.w
        return Self(
            x: x * (qx * qx + qw * qw - qy * qy - qz * qz)
                + y * (2 * qx * qy - 2 * qw * qz)
                + z * (2 * qx * qz + 2 * qw * qy),
            y: x * (2 * qw * qz + 2 * qx * qy)
                + y * (qw * qw - qx * qx + qy * qy - qz * qz)
                + z * (-2 * qw * qx + 2 * qy * qz),
            z: x * (-2 * qw * qy + 2 * qx * qz)
                + y * (2 * qw * qx + 2 * qy * qz)
                + z * (qw * qw - qx * qx - qy * qy + qz * qz)
        )
    }

    public static func + (lhs: Self, rhs: Self) -> Self {
        Self(x: lhs.x + rhs.x, y: lhs.y + rhs.y, z: lhs.z + rhs.z)
    }

    public static func - (lhs: Self, rhs: Self) -> Self {
        Self(x: lhs.x - rhs.x, y: lhs.y - rhs.y, z: lhs.z - rhs.z)
    }

    public static func * (lhs: Self, rhs: Float) -> Self {
        Self(x: lhs.x * rhs, y: lhs.y * rhs, z: lhs.z * rhs)
    }

    public static func * (lhs: Float, rhs: Self) -> Self { rhs * lhs }

    public static func * (lhs: Self, rhs: Self) -> Self {
        Self(x: lhs.x * rhs.x, y: lhs.y * rhs.y, z: lhs.z * rhs.z)
    }

    public static func / (lhs: Self, rhs: Float) -> Self {
        Self(x: lhs.x / rhs, y: lhs.y / rhs, z: lhs.z / rhs)
    }

    public static func / (lhs: Self, rhs: Self) -> Self {
        Self(x: lhs.x / rhs.x, y: lhs.y / rhs.y, z: lhs.z / rhs.z)
    }

    public static func += (lhs: inout Self, rhs: Self) { lhs = lhs + rhs }

    var core: BTBHVec3 { BTBHVec3(x: x, y: y, z: z) }
    init(core: BTBHVec3) { self.init(x: core.x, y: core.y, z: core.z) }
}

public struct RenderVec4: Hashable, Sendable {
    public var x: Float; public var y: Float; public var z: Float; public var w: Float
    public init(x: Float, y: Float, z: Float, w: Float) { self.x = x; self.y = y; self.z = z; self.w = w }

    var core: BTBHVec4 { BTBHVec4(x: x, y: y, z: z, w: w) }
    init(core: BTBHVec4) { self.init(x: core.x, y: core.y, z: core.z, w: core.w) }
}

public struct RenderQuaternion: Hashable, Sendable {
    public var x: Float; public var y: Float; public var z: Float; public var w: Float
    public init(x: Float, y: Float, z: Float, w: Float) { self.x = x; self.y = y; self.z = z; self.w = w }
    public static let identity = Self(x: 0, y: 0, z: 0, w: 1)

    public init(axis: RenderVec3, radians: Float) {
        let axisLength = axis.length
        guard axisLength != 0 else {
            self = .identity
            return
        }

        let inverseAxisLength = 1 / axisLength
        let halfAngle = radians * 0.5
        let sine = sin(halfAngle)
        var result = Self(
            x: axis.x * inverseAxisLength * sine,
            y: axis.y * inverseAxisLength * sine,
            z: axis.z * inverseAxisLength * sine,
            w: cos(halfAngle)
        )

        let length = sqrt(
            result.x * result.x
                + result.y * result.y
                + result.z * result.z
                + result.w * result.w
        )
        let inverseLength = 1 / (length == 0 ? 1 : length)
        result.x *= inverseLength
        result.y *= inverseLength
        result.z *= inverseLength
        result.w *= inverseLength
        self = result
    }

    public static func * (lhs: Self, rhs: Self) -> Self {
        Self(
            x: lhs.x * rhs.w + lhs.w * rhs.x + lhs.y * rhs.z - lhs.z * rhs.y,
            y: lhs.y * rhs.w + lhs.w * rhs.y + lhs.z * rhs.x - lhs.x * rhs.z,
            z: lhs.z * rhs.w + lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x,
            w: lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z
        )
    }

    var core: BTBHQuaternion { BTBHQuaternion(x: x, y: y, z: z, w: w) }
    init(core: BTBHQuaternion) { self.init(x: core.x, y: core.y, z: core.z, w: core.w) }
}

public struct RenderMatrix4: Hashable, Sendable {
    public var c0: SIMD4<Float>; public var c1: SIMD4<Float>; public var c2: SIMD4<Float>; public var c3: SIMD4<Float>
    public init(c0: SIMD4<Float>, c1: SIMD4<Float>, c2: SIMD4<Float>, c3: SIMD4<Float>) {
        self.c0 = c0; self.c1 = c1; self.c2 = c2; self.c3 = c3
    }
    public static let identity = Self(
        c0: SIMD4(1, 0, 0, 0), c1: SIMD4(0, 1, 0, 0),
        c2: SIMD4(0, 0, 1, 0), c3: SIMD4(0, 0, 0, 1)
    )

    public static func translation(_ value: RenderVec3) -> Self {
        Self(
            c0: SIMD4(1, 0, 0, 0),
            c1: SIMD4(0, 1, 0, 0),
            c2: SIMD4(0, 0, 1, 0),
            c3: SIMD4(value.x, value.y, value.z, 1)
        )
    }

    public static func rotation(axis: RenderVec3, radians: Float) -> Self {
        var x = axis.x
        var y = axis.y
        var z = axis.z
        let lengthSquared = x * x + y * y + z * z
        if lengthSquared != 1, lengthSquared != 0 {
            let inverseLength = 1 / sqrt(lengthSquared)
            x *= inverseLength
            y *= inverseLength
            z *= inverseLength
        }

        let sine = sin(radians)
        let cosine = cos(radians)
        let oneMinusCosine = 1 - cosine
        return Self(
            c0: SIMD4(
                x * x * oneMinusCosine + cosine,
                y * x * oneMinusCosine + z * sine,
                z * x * oneMinusCosine - y * sine,
                0
            ),
            c1: SIMD4(
                x * y * oneMinusCosine - z * sine,
                y * y * oneMinusCosine + cosine,
                z * y * oneMinusCosine + x * sine,
                0
            ),
            c2: SIMD4(
                x * z * oneMinusCosine + y * sine,
                y * z * oneMinusCosine - x * sine,
                z * z * oneMinusCosine + cosine,
                0
            ),
            c3: SIMD4(0, 0, 0, 1)
        )
    }

    public static func scale(_ value: RenderVec3) -> Self {
        Self(
            c0: SIMD4(value.x, 0, 0, 0),
            c1: SIMD4(0, value.y, 0, 0),
            c2: SIMD4(0, 0, value.z, 0),
            c3: SIMD4(0, 0, 0, 1)
        )
    }

    public static func * (lhs: Self, rhs: Self) -> Self {
        Self(
            c0: multiply(column: lhs.c0, by: rhs),
            c1: multiply(column: lhs.c1, by: rhs),
            c2: multiply(column: lhs.c2, by: rhs),
            c3: multiply(column: lhs.c3, by: rhs)
        )
    }

    private static func multiply(column: SIMD4<Float>, by matrix: Self) -> SIMD4<Float> {
        SIMD4(
            column.x * matrix.c0.x + column.y * matrix.c1.x
                + column.z * matrix.c2.x + column.w * matrix.c3.x,
            column.x * matrix.c0.y + column.y * matrix.c1.y
                + column.z * matrix.c2.y + column.w * matrix.c3.y,
            column.x * matrix.c0.z + column.y * matrix.c1.z
                + column.z * matrix.c2.z + column.w * matrix.c3.z,
            column.x * matrix.c0.w + column.y * matrix.c1.w
                + column.z * matrix.c2.w + column.w * matrix.c3.w
        )
    }

    var core: BTBHMatrix4 {
        var result = BTBHMatrix4()
        result.elements.0 = c0.x
        result.elements.1 = c0.y
        result.elements.2 = c0.z
        result.elements.3 = c0.w
        result.elements.4 = c1.x
        result.elements.5 = c1.y
        result.elements.6 = c1.z
        result.elements.7 = c1.w
        result.elements.8 = c2.x
        result.elements.9 = c2.y
        result.elements.10 = c2.z
        result.elements.11 = c2.w
        result.elements.12 = c3.x
        result.elements.13 = c3.y
        result.elements.14 = c3.z
        result.elements.15 = c3.w
        return result
    }

    init(core: BTBHMatrix4) {
        self.init(
            c0: SIMD4(core.elements.0, core.elements.1, core.elements.2, core.elements.3),
            c1: SIMD4(core.elements.4, core.elements.5, core.elements.6, core.elements.7),
            c2: SIMD4(core.elements.8, core.elements.9, core.elements.10, core.elements.11),
            c3: SIMD4(core.elements.12, core.elements.13, core.elements.14, core.elements.15)
        )
    }
}

public struct RenderColor: Hashable, Sendable {
    public var r: UInt8; public var g: UInt8; public var b: UInt8; public var a: UInt8
    public init(r: UInt8, g: UInt8, b: UInt8, a: UInt8 = 255) { self.r = r; self.g = g; self.b = b; self.a = a }
    public static let white = Self(r: 255, g: 255, b: 255, a: 255)
    public static let black = Self(r: 0, g: 0, b: 0, a: 255)
    public static let clear = Self(r: 0, g: 0, b: 0, a: 0)

    public func withAlpha(_ alpha: UInt8) -> Self {
        Self(r: r, g: g, b: b, a: alpha)
    }

    var core: BTBHColor { BTBHColor(r: r, g: g, b: b, a: a) }
    init(core: BTBHColor) { self.init(r: core.r, g: core.g, b: core.b, a: core.a) }
}

public struct RenderRect: Hashable, Sendable {
    public var x: Float; public var y: Float; public var width: Float; public var height: Float
    public init(x: Float, y: Float, width: Float, height: Float) { self.x = x; self.y = y; self.width = width; self.height = height }
    public func contains(_ point: RenderVec2) -> Bool {
        point.x >= x && point.y >= y && point.x < x + width && point.y < y + height
    }

    public func intersects(_ other: Self) -> Bool {
        x < other.x + other.width && x + width > other.x
            && y < other.y + other.height && y + height > other.y
    }

    public func intersectsCircle(center: RenderVec2, radius: Float) -> Bool {
        let centerX = x + width / 2
        let centerY = y + height / 2
        let dx = abs(center.x - centerX)
        let dy = abs(center.y - centerY)

        if dx > width / 2 + radius { return false }
        if dy > height / 2 + radius { return false }
        if dx <= width / 2 { return true }
        if dy <= height / 2 { return true }

        let cornerX = dx - width / 2
        let cornerY = dy - height / 2
        return cornerX * cornerX + cornerY * cornerY <= radius * radius
    }

    var core: BTBHRect { BTBHRect(x: x, y: y, width: width, height: height) }
    init(core: BTBHRect) { self.init(x: core.x, y: core.y, width: core.width, height: core.height) }
}

public func renderLerp(_ start: Float, _ end: Float, amount: Float) -> Float {
    start + amount * (end - start)
}

public func renderRemap(
    _ value: Float,
    inputStart: Float,
    inputEnd: Float,
    outputStart: Float,
    outputEnd: Float
) -> Float {
    (value - inputStart) / (inputEnd - inputStart) * (outputEnd - outputStart) + outputStart
}

public struct RenderBounds3: Hashable, Sendable {
    public var minimum: RenderVec3; public var maximum: RenderVec3
    public init(minimum: RenderVec3, maximum: RenderVec3) { self.minimum = minimum; self.maximum = maximum }

    var core: BTBHBounds3 { BTBHBounds3(minimum: minimum.core, maximum: maximum.core) }
    init(core: BTBHBounds3) { self.init(minimum: RenderVec3(core: core.minimum), maximum: RenderVec3(core: core.maximum)) }
}

public struct RenderRay: Hashable, Sendable {
    public let origin: RenderVec3
    public let direction: RenderVec3

    public init(origin: RenderVec3, direction: RenderVec3) {
        self.origin = origin
        self.direction = direction
    }
}

public enum RenderProjection: UInt8, Hashable, Sendable {
    case perspective
    case orthographic

    var core: BTBHProjection {
        switch self {
        case .perspective: BTBH_PROJECTION_PERSPECTIVE
        case .orthographic: BTBH_PROJECTION_ORTHOGRAPHIC
        }
    }

    init(core: BTBHProjection) {
        switch core {
        case BTBH_PROJECTION_PERSPECTIVE:
            self = .perspective
        case BTBH_PROJECTION_ORTHOGRAPHIC:
            self = .orthographic
        default:
            preconditionFailure("Unknown projection value: \(core.rawValue)")
        }
    }
}

public struct RenderCamera: Hashable, Sendable {
    public var position: RenderVec3; public var target: RenderVec3; public var up: RenderVec3
    public var verticalFOVDegrees: Float; public var projection: RenderProjection
    /// Render-only world curvature (`RenderWorldCurvature`): 1 / radius in world units, positive for a small
    /// planet, negative for a hyperbolic look, 0 for the flat world.
    public var worldCurvature: Float
    /// The world point the curvature bends around; nil bends around `target`.
    public var worldCurvatureCenter: RenderVec3?
    public init(position: RenderVec3, target: RenderVec3, up: RenderVec3, verticalFOVDegrees: Float, projection: RenderProjection,
                worldCurvature: Float = 0, worldCurvatureCenter: RenderVec3? = nil) {
        self.position = position; self.target = target; self.up = up
        self.verticalFOVDegrees = verticalFOVDegrees; self.projection = projection
        self.worldCurvature = worldCurvature.isFinite ? worldCurvature : 0
        self.worldCurvatureCenter = worldCurvatureCenter
    }

    var core: BTBHCamera {
        BTBHCamera(
            position: position.core,
            target: target.core,
            up: up.core,
            vertical_fov_degrees: verticalFOVDegrees,
            projection: projection.core
        )
    }

    init(core: BTBHCamera) {
        self.init(
            position: RenderVec3(core: core.position),
            target: RenderVec3(core: core.target),
            up: RenderVec3(core: core.up),
            verticalFOVDegrees: core.vertical_fov_degrees,
            projection: RenderProjection(core: core.projection)
        )
    }
}
