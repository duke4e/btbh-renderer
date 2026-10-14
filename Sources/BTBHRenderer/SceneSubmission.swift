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
    /// World translation since the previous frame, for the temporal upscaler's motion vectors.
    public let motion: RenderVec3

    public init(
        model: ModelHandle,
        transform: RenderTransform,
        overrides: [MaterialOverride] = [],
        motion: RenderVec3 = .zero
    ) {
        self.model = model
        self.transform = transform
        self.overrides = overrides
        self.motion = motion
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
    /// The bounding-sphere radius of the range's biggest instance, which sizes it on screen for
    /// a draw's `lod`. Zero keeps the full model wherever the camera sees the range.
    public let objectRadius: Float

    public init(
        sourceStart: Int,
        count: Int,
        bounds: RenderBounds3,
        objectRadius: Float = 0
    ) {
        self.sourceStart = sourceStart
        self.count = count
        self.bounds = bounds
        self.objectRadius = objectRadius
    }

    public func isValid(instanceCount: Int) -> Bool {
        sourceStart >= 0 &&
            count > 0 &&
            sourceStart <= instanceCount &&
            count <= instanceCount - sourceStart
    }
}

/// A simplified model drawn in place of a range-list draw's model. Its primitives must use the
/// base model's material slots: backends resolve materials from the base model and take only
/// the geometry from this one.
public struct PersistentModelLOD: Sendable, Equatable {
    public let model: ModelHandle
    /// A range whose biggest instance spans fewer pixels than this on screen (its bounding
    /// sphere's diameter at the range's nearest point) draws the simplified model. Ranges the
    /// camera does not see draw it into the shadow passes.
    public let maximumPixelDiameter: Float

    public init(model: ModelHandle, maximumPixelDiameter: Float) {
        self.model = model
        self.maximumPixelDiameter = maximumPixelDiameter
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
    /// Backends without LOD support draw `model` everywhere.
    public let lod: PersistentModelLOD?

    public init(
        model: ModelHandle,
        instances: [RenderInstance],
        overrides: [MaterialOverride],
        persistentBuffer: InstanceBufferHandle,
        persistentRevision: UInt64? = nil,
        ranges: [PersistentModelInstanceRange],
        lod: PersistentModelLOD? = nil
    ) {
        self.model = model
        self.instances = instances
        self.overrides = overrides
        self.persistentBuffer = persistentBuffer
        self.persistentRevision = persistentRevision
        self.ranges = ranges
        self.lod = lod
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
    /// Each instance's world translation since the previous frame (indexed like `positions`), for the
    /// temporal upscaler's motion vectors; nil when the instances hold still.
    public let motions: [RenderVec3]?
    /// Each instance's surface animation time advance since the previous frame, in seconds (indexed
    /// like `positions`): the dissolve and shatter surfaces move their vertices by the time in
    /// `customs` (dissolve x, shatter y), and the motion vectors follow that movement; nil or 0 when
    /// it holds still.
    public let surfaceMotions: [Float]?

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
        count: Int,
        motions: [RenderVec3]? = nil,
        surfaceMotions: [Float]? = nil
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
        self.motions = motions
        self.surfaceMotions = surfaceMotions
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
    /// Each instance's world translation since the previous frame (indexed like `positions`); nil when
    /// the instances hold still.
    public let motions: [RenderVec3]?

    public init(
        mesh: MeshHandle,
        material: MaterialHandle,
        positions: [RenderVec3],
        rotations: [RenderQuaternion],
        scales: [RenderVec3],
        colors: [RenderColor],
        sourceStart: Int = 0,
        count: Int,
        motions: [RenderVec3]? = nil
    ) {
        self.mesh = mesh
        self.material = material
        self.positions = positions
        self.rotations = rotations
        self.scales = scales
        self.colors = colors
        self.sourceStart = sourceStart
        self.count = count
        self.motions = motions
    }
}

public struct AnimatedModelDraw: Sendable {
    public let model: ModelHandle
    public let player: AnimationPlayerHandle?
    public let tree: AnimationTreeHandle?
    public let transform: RenderTransform
    public let overrides: [MaterialOverride]
    /// World translation since the previous frame, for the temporal upscaler's motion vectors (the
    /// skeleton's own motion comes from the pose the previous frame drew).
    public let motion: RenderVec3

    public init(
        model: ModelHandle,
        player: AnimationPlayerHandle?,
        tree: AnimationTreeHandle?,
        transform: RenderTransform,
        overrides: [MaterialOverride],
        motion: RenderVec3 = .zero
    ) {
        self.model = model
        self.player = player
        self.tree = tree
        self.transform = transform
        self.overrides = overrides
        self.motion = motion
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
    /// Each instance's world translation since the previous frame (indexed like `positions`); nil when
    /// the instances hold still. The skeleton's own motion comes from the pose its animation tree drew
    /// in the previous frame (the renderer keeps that palette while the temporal upscaler runs).
    public let motions: [RenderVec3]?
    /// Each instance's surface animation time advance since the previous frame, in seconds (indexed
    /// like `positions`; the dissolve's time is `customs.x`), as in `ModelTransformStreamsDraw`.
    public let surfaceMotions: [Float]?

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
        castsSunShadow: Bool = true,
        motions: [RenderVec3]? = nil,
        surfaceMotions: [Float]? = nil
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
        self.motions = motions
        self.surfaceMotions = surfaceMotions
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
    /// `UpscaleMode.fsr3`: true when this frame does not continue the previous one (a camera cut, a run
    /// or profile change), so the upscaler starts a new history instead of reprojecting the old one.
    public let resetsTemporalHistory: Bool
    /// Seconds since the previous frame, for the temporal upscaler.
    public let frameSeconds: Float

    public init(
        camera: RenderCamera,
        environment: EnvironmentDescriptor,
        renderScale: Float = 1,
        antiAliasing: AntiAliasing = .smaaMedium,
        upscaleMode: UpscaleMode = .linear,
        postChain: PostChainDescriptor = .empty,
        viewport: NormalizedViewport = .full,
        resetsTemporalHistory: Bool = false,
        frameSeconds: Float = 1.0 / 60.0
    ) {
        self.camera = camera
        self.environment = environment
        self.renderScale = renderScale
        self.antiAliasing = antiAliasing
        self.upscaleMode = upscaleMode
        self.postChain = postChain
        self.viewport = viewport
        self.resetsTemporalHistory = resetsTemporalHistory
        self.frameSeconds = frameSeconds
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
    /// Copies the finished world frame into `texture`, a `captureWorldFrameTexture()` capture of
    /// the current drawable size, and releases the frame so another world frame can render
    /// before the canvas presents. The canvas then shows only the last frame; earlier ones are
    /// drawn from their copies.
    func copyWorldFrame(into texture: TextureHandle) throws
    func shutdown()
}

public extension SceneBackend {
    func copyWorldFrame(into texture: TextureHandle) throws {
        throw RenderResourceBackendError.unsupportedOperation("scene.copyWorldFrame")
    }

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
