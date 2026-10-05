public struct RenderTransform: Hashable, Sendable {
    public let matrix: RenderMatrix4

    public init(matrix: RenderMatrix4) {
        self.matrix = matrix
    }

    public static let identity = Self(matrix: .identity)
}

public struct MaterialOverride: Hashable, Sendable {
    public let slot: Int
    public let material: MaterialHandle

    public init(slot: Int, material: MaterialHandle) {
        self.slot = slot
        self.material = material
    }
}

public struct RenderInstance: Hashable, Sendable {
    public let transform: RenderTransform
    public let color: RenderColor
    public let custom: RenderVec4

    public init(transform: RenderTransform, color: RenderColor, custom: RenderVec4) {
        self.transform = transform
        self.color = color
        self.custom = custom
    }
}

public struct ModelDraw: Sendable {
    public let model: ModelHandle
    public let transform: RenderTransform
    public let overrides: [MaterialOverride]

    public init(model: ModelHandle, transform: RenderTransform, overrides: [MaterialOverride] = []) {
        self.model = model
        self.transform = transform
        self.overrides = overrides
    }
}

public struct ModelInstancesDraw: Sendable {
    public let model: ModelHandle
    public let instances: [RenderInstance]
    public let overrides: [MaterialOverride]
    public let persistentBuffer: InstanceBufferHandle?
    public let persistentRevision: UInt64?
    public let sourceStart: Int
    public let count: Int

    public init(
        model: ModelHandle,
        instances: [RenderInstance],
        overrides: [MaterialOverride],
        persistentBuffer: InstanceBufferHandle?,
        persistentRevision: UInt64? = nil,
        sourceStart: Int = 0,
        count: Int? = nil
    ) {
        self.model = model
        self.instances = instances
        self.overrides = overrides
        self.persistentBuffer = persistentBuffer
        self.persistentRevision = persistentRevision
        self.sourceStart = sourceStart
        self.count = count ?? max(0, instances.count - sourceStart)
    }
}

/// One bounded range inside a persistent semantic instance buffer.
///
/// Bounds cover exactly the selected source range and are used for camera and
/// shadow-frustum rejection without rebuilding one draw value per range.
public struct PersistentModelInstanceRange: Hashable, Sendable {
    public let sourceStart: Int
    public let count: Int
    public let bounds: RenderBounds3

    public init(
        sourceStart: Int,
        count: Int,
        bounds: RenderBounds3
    ) {
        self.sourceStart = sourceStart
        self.count = count
        self.bounds = bounds
    }

    public func isValid(instanceCount: Int) -> Bool {
        sourceStart >= 0 &&
            count > 0 &&
            sourceStart <= instanceCount &&
            count <= instanceCount - sourceStart
    }
}

/// A compact set of spatial ranges sharing one persistent model submission.
///
/// Backends can resolve the model, materials, and persistent storage once,
/// cull the ranges in concrete backend code, and emit only surviving batches.
public struct PersistentModelRangeListDraw: Sendable {
    public let model: ModelHandle
    public let instances: [RenderInstance]
    public let overrides: [MaterialOverride]
    public let persistentBuffer: InstanceBufferHandle
    public let persistentRevision: UInt64?
    public let ranges: [PersistentModelInstanceRange]

    public init(
        model: ModelHandle,
        instances: [RenderInstance],
        overrides: [MaterialOverride],
        persistentBuffer: InstanceBufferHandle,
        persistentRevision: UInt64? = nil,
        ranges: [PersistentModelInstanceRange]
    ) {
        self.model = model
        self.instances = instances
        self.overrides = overrides
        self.persistentBuffer = persistentBuffer
        self.persistentRevision = persistentRevision
        self.ranges = ranges
    }
}

/// Pre-decomposed model instance streams for allocation-sensitive render paths.
///
/// `sourceStart` and `count` select the same contiguous range in every present
/// stream. Omitting `colors` or `customs` leaves that instance channel disabled.
/// The backend consumes the streams during `SceneBackend.submit(_:)`.
public struct ModelTransformStreamsDraw: Sendable {
    public let model: ModelHandle
    public let positions: [RenderVec3]
    public let rotations: [RenderQuaternion]
    public let scales: [RenderVec3]
    public let colors: [RenderColor]?
    public let customs: [RenderVec4]?
    public let overrides: [MaterialOverride]
    public let clusterBounds: RenderBounds3?
    public let sourceStart: Int
    public let count: Int

    public init(
        model: ModelHandle,
        positions: [RenderVec3],
        rotations: [RenderQuaternion],
        scales: [RenderVec3],
        colors: [RenderColor]? = nil,
        customs: [RenderVec4]? = nil,
        overrides: [MaterialOverride] = [],
        clusterBounds: RenderBounds3? = nil,
        sourceStart: Int = 0,
        count: Int
    ) {
        self.model = model
        self.positions = positions
        self.rotations = rotations
        self.scales = scales
        self.colors = colors
        self.customs = customs
        self.overrides = overrides
        self.clusterBounds = clusterBounds
        self.sourceStart = sourceStart
        self.count = count
    }
}

public struct MeshDraw: Sendable {
    public let mesh: MeshHandle
    public let material: MaterialHandle
    public let transform: RenderTransform

    public init(mesh: MeshHandle, material: MaterialHandle, transform: RenderTransform) {
        self.mesh = mesh
        self.material = material
        self.transform = transform
    }
}

public struct MeshInstancesDraw: Sendable {
    public let mesh: MeshHandle
    public let material: MaterialHandle
    public let instances: [RenderInstance]
    public let persistentBuffer: InstanceBufferHandle?
    public let persistentRevision: UInt64?

    public init(
        mesh: MeshHandle,
        material: MaterialHandle,
        instances: [RenderInstance],
        persistentBuffer: InstanceBufferHandle?,
        persistentRevision: UInt64? = nil
    ) {
        self.mesh = mesh
        self.material = material
        self.instances = instances
        self.persistentBuffer = persistentBuffer
        self.persistentRevision = persistentRevision
    }
}

/// Pre-decomposed mesh instance streams for allocation-sensitive render paths.
///
/// `sourceStart` and `count` select the same contiguous range in every stream.
/// The backend consumes the streams during `SceneBackend.submit(_:)`.
public struct MeshTransformStreamsDraw: Sendable {
    public let mesh: MeshHandle
    public let material: MaterialHandle
    public let positions: [RenderVec3]
    public let rotations: [RenderQuaternion]
    public let scales: [RenderVec3]
    public let colors: [RenderColor]
    public let sourceStart: Int
    public let count: Int

    public init(
        mesh: MeshHandle,
        material: MaterialHandle,
        positions: [RenderVec3],
        rotations: [RenderQuaternion],
        scales: [RenderVec3],
        colors: [RenderColor],
        sourceStart: Int = 0,
        count: Int
    ) {
        self.mesh = mesh
        self.material = material
        self.positions = positions
        self.rotations = rotations
        self.scales = scales
        self.colors = colors
        self.sourceStart = sourceStart
        self.count = count
    }
}

public struct AnimatedModelDraw: Sendable {
    public let model: ModelHandle
    public let player: AnimationPlayerHandle?
    public let tree: AnimationTreeHandle?
    public let transform: RenderTransform
    public let overrides: [MaterialOverride]

    public init(
        model: ModelHandle,
        player: AnimationPlayerHandle?,
        tree: AnimationTreeHandle?,
        transform: RenderTransform,
        overrides: [MaterialOverride]
    ) {
        self.model = model
        self.player = player
        self.tree = tree
        self.transform = transform
        self.overrides = overrides
    }
}

public struct AnimatedModelInstancesDraw: Sendable {
    public let model: ModelHandle
    public let player: AnimationPlayerHandle?
    public let tree: AnimationTreeHandle?
    public let instances: [RenderInstance]
    public let overrides: [MaterialOverride]

    public init(
        model: ModelHandle,
        player: AnimationPlayerHandle?,
        tree: AnimationTreeHandle?,
        instances: [RenderInstance],
        overrides: [MaterialOverride]
    ) {
        self.model = model
        self.player = player
        self.tree = tree
        self.instances = instances
        self.overrides = overrides
    }
}

/// Pre-decomposed animated model instance streams for allocation-sensitive paths.
///
/// Exactly one of `player` and `tree` must be supplied by the caller.
public struct AnimatedModelTransformStreamsDraw: Sendable {
    public let model: ModelHandle
    public let player: AnimationPlayerHandle?
    public let tree: AnimationTreeHandle?
    public let positions: [RenderVec3]
    public let rotations: [RenderQuaternion]
    public let scales: [RenderVec3]
    public let colors: [RenderColor]?
    public let customs: [RenderVec4]?
    public let overrides: [MaterialOverride]
    public let sourceStart: Int
    public let count: Int
    /// False keeps the draw out of the sun's shadow maps, for a body whose own lamp alone
    /// throws its shadow.
    public let castsSunShadow: Bool

    public init(
        model: ModelHandle,
        player: AnimationPlayerHandle?,
        tree: AnimationTreeHandle?,
        positions: [RenderVec3],
        rotations: [RenderQuaternion],
        scales: [RenderVec3],
        colors: [RenderColor]? = nil,
        customs: [RenderVec4]? = nil,
        overrides: [MaterialOverride] = [],
        sourceStart: Int = 0,
        count: Int,
        castsSunShadow: Bool = true
    ) {
        self.model = model
        self.player = player
        self.tree = tree
        self.positions = positions
        self.rotations = rotations
        self.scales = scales
        self.colors = colors
        self.customs = customs
        self.overrides = overrides
        self.sourceStart = sourceStart
        self.count = count
        self.castsSunShadow = castsSunShadow
    }
}

public enum DebugDraw: Sendable {
    case line(start: RenderVec3, end: RenderVec3, color: RenderColor)
    case grid(slices: Int, spacing: Float)
    case plane(center: RenderVec3, size: RenderVec2, color: RenderColor)
    case sphere(center: RenderVec3, radius: Float, color: RenderColor)
    case bounds(RenderBounds3, color: RenderColor)
    case wireCube(center: RenderVec3, size: RenderVec3, color: RenderColor)
}

public enum SceneDraw: Sendable {
    case model(ModelDraw)
    case modelInstances(ModelInstancesDraw)
    case modelTransformStreams(ModelTransformStreamsDraw)
    case mesh(MeshDraw)
    case meshInstances(MeshInstancesDraw)
    case meshTransformStreams(MeshTransformStreamsDraw)
    case animatedModel(AnimatedModelDraw)
    case animatedModelInstances(AnimatedModelInstancesDraw)
    case animatedModelTransformStreams(AnimatedModelTransformStreamsDraw)
    case debug(DebugDraw)
}

public struct FrameDescriptor: Sendable {
    public let camera: RenderCamera
    public let environment: EnvironmentDescriptor
    public let renderScale: Float
    public let antiAliasing: AntiAliasing
    public let upscaleMode: UpscaleMode
    public let postChain: PostChainDescriptor
    public let viewport: NormalizedViewport

    public init(
        camera: RenderCamera,
        environment: EnvironmentDescriptor,
        renderScale: Float = 1,
        antiAliasing: AntiAliasing = .smaaMedium,
        upscaleMode: UpscaleMode = .linear,
        postChain: PostChainDescriptor = .empty,
        viewport: NormalizedViewport = .full
    ) {
        self.camera = camera
        self.environment = environment
        self.renderScale = renderScale
        self.antiAliasing = antiAliasing
        self.upscaleMode = upscaleMode
        self.postChain = postChain
        self.viewport = viewport
    }
}

public struct FrameReadback: Sendable {
    public let size: PixelSize2D
    public let rgba8: [UInt8]

    public init(size: PixelSize2D, rgba8: [UInt8]) {
        self.size = size
        self.rgba8 = rgba8
    }
}

@MainActor
public protocol SceneBackend: AnyObject {
    func initialize(drawableSize: PixelSize2D) throws
    func resize(drawableSize: PixelSize2D) throws
    func beginFrame(_ descriptor: FrameDescriptor)
    func submit(_ draw: SceneDraw)
    func submitPersistentModelRanges(_ draw: PersistentModelRangeListDraw)
    func endFrame() throws
    func projectNormalized(
        _ world: RenderVec3,
        camera: RenderCamera,
        viewport: NormalizedViewport
    ) -> NormalizedViewportPoint
    func unprojectRay(
        _ point: NormalizedViewportPoint,
        camera: RenderCamera,
        viewport: NormalizedViewport
    ) -> RenderRay
    func intersectsFrustum(_ bounds: RenderBounds3) -> Bool
    func readbackWorldFrame() throws -> FrameReadback
    func captureWorldFrameTexture() throws -> TextureHandle
    func shutdown()
}

public extension SceneBackend {
    func projectNormalized(
        _ world: RenderVec3,
        camera: RenderCamera
    ) -> NormalizedViewportPoint {
        projectNormalized(world, camera: camera, viewport: .full)
    }

    func unprojectRay(
        _ point: NormalizedViewportPoint,
        camera: RenderCamera
    ) -> RenderRay {
        unprojectRay(point, camera: camera, viewport: .full)
    }

    /// Source-compatible expansion for backends without a specialized list
    /// implementation. Specialized backends should queue the list itself.
    func submitPersistentModelRanges(_ draw: PersistentModelRangeListDraw) {
        guard !draw.instances.isEmpty else { return }
        for range in draw.ranges
        where range.isValid(instanceCount: draw.instances.count) &&
            intersectsFrustum(range.bounds) {
            submit(.modelInstances(.init(
                model: draw.model,
                instances: draw.instances,
                overrides: draw.overrides,
                persistentBuffer: draw.persistentBuffer,
                persistentRevision: draw.persistentRevision,
                sourceStart: range.sourceStart,
                count: range.count
            )))
        }
    }
}
