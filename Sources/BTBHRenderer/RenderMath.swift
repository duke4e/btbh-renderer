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
    public var length: Float { sqrt(x * x + y * y) }
    public func normalized(epsilon: Float = 1e-6) -> Self {
        let value = length
        return value > epsilon ? self / value : .zero
    }
    public static func + (lhs: Self, rhs: Self) -> Self { Self(x: lhs.x + rhs.x, y: lhs.y + rhs.y) }
    public static func - (lhs: Self, rhs: Self) -> Self { Self(x: lhs.x - rhs.x, y: lhs.y - rhs.y) }
    public static func * (lhs: Self, rhs: Float) -> Self { Self(x: lhs.x * rhs, y: lhs.y * rhs) }
    public static func / (lhs: Self, rhs: Float) -> Self { Self(x: lhs.x / rhs, y: lhs.y / rhs) }

    var core: BTBHVec2 { BTBHVec2(x: x, y: y) }
    init(core: BTBHVec2) { self.init(x: core.x, y: core.y) }
}

public struct RenderVec3: Hashable, Sendable {
    public var x: Float
    public var y: Float
    public var z: Float
    public init(x: Float, y: Float, z: Float) { self.x = x; self.y = y; self.z = z }
    public static let zero = Self(x: 0, y: 0, z: 0)
    public static let up = Self(x: 0, y: 1, z: 0)
    public func dot(_ rhs: Self) -> Float { x * rhs.x + y * rhs.y + z * rhs.z }
    public func cross(_ rhs: Self) -> Self {
        Self(x: y * rhs.z - z * rhs.y, y: z * rhs.x - x * rhs.z, z: x * rhs.y - y * rhs.x)
    }

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

    var core: BTBHColor { BTBHColor(r: r, g: g, b: b, a: a) }
    init(core: BTBHColor) { self.init(r: core.r, g: core.g, b: core.b, a: core.a) }
}

public struct RenderRect: Hashable, Sendable {
    public var x: Float; public var y: Float; public var width: Float; public var height: Float
    public init(x: Float, y: Float, width: Float, height: Float) { self.x = x; self.y = y; self.width = width; self.height = height }
    public func contains(_ point: RenderVec2) -> Bool {
        point.x >= x && point.y >= y && point.x < x + width && point.y < y + height
    }

    var core: BTBHRect { BTBHRect(x: x, y: y, width: width, height: height) }
    init(core: BTBHRect) { self.init(x: core.x, y: core.y, width: core.width, height: core.height) }
}

public struct RenderBounds3: Hashable, Sendable {
    public var minimum: RenderVec3; public var maximum: RenderVec3
    public init(minimum: RenderVec3, maximum: RenderVec3) { self.minimum = minimum; self.maximum = maximum }

    var core: BTBHBounds3 { BTBHBounds3(minimum: minimum.core, maximum: maximum.core) }
    init(core: BTBHBounds3) { self.init(minimum: RenderVec3(core: core.minimum), maximum: RenderVec3(core: core.maximum)) }
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
    public init(position: RenderVec3, target: RenderVec3, up: RenderVec3, verticalFOVDegrees: Float, projection: RenderProjection) {
        self.position = position; self.target = target; self.up = up
        self.verticalFOVDegrees = verticalFOVDegrees; self.projection = projection
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
