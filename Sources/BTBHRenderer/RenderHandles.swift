import BTBHRendererCore

public protocol RenderHandle: Hashable, Sendable {
    init(rawValue: UInt64)
    var rawValue: UInt64 { get }
    static var invalid: Self { get }
}

public extension RenderHandle {
    static var invalid: Self { Self(rawValue: 0) }
    var isValid: Bool { rawValue != 0 }
}

public struct TextureHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct FontHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct RenderTargetHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct MeshHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct ModelHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct MaterialHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct InstanceBufferHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct AnimationLibraryHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct AnimationPlayerHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct AnimationTreeHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct LightHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct CubemapHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct AmbientMapHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct SurfaceShaderHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct ScreenShaderHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }

public enum HandlePoolError: Error, Equatable {
    case allocationFailed
    case exhausted

    init(coreResult: BTBHHandleResult) {
        switch coreResult {
        case BTBH_HANDLE_OUT_OF_MEMORY:
            self = .allocationFailed
        case BTBH_HANDLE_EXHAUSTED:
            self = .exhausted
        default:
            preconditionFailure("Unexpected handle acquisition result: \(coreResult)")
        }
    }
}

public final class HandlePool {
    private let raw: OpaquePointer

    public init(initialCapacity: UInt32) throws {
        guard let raw = btbh_handle_pool_create(initialCapacity) else { throw HandlePoolError.allocationFailed }
        self.raw = raw
    }

    deinit { btbh_handle_pool_destroy(raw) }

    public func acquire<H: RenderHandle>(_ type: H.Type) throws -> H {
        var value: UInt64 = 0
        let result = btbh_handle_pool_acquire(raw, &value)
        guard result == BTBH_HANDLE_OK else { throw HandlePoolError(coreResult: result) }
        return H(rawValue: value)
    }

    public func contains<H: RenderHandle>(_ handle: H) -> Bool {
        btbh_handle_pool_contains(raw, handle.rawValue) != 0
    }

    @discardableResult public func release<H: RenderHandle>(_ handle: H) -> Bool {
        btbh_handle_pool_release(raw, handle.rawValue) == BTBH_HANDLE_OK
    }
}
