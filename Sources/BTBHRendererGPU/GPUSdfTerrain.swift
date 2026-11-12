import BTBHRenderer
import BTBHRendererGPUCore
import Dispatch
import Foundation

/// How an SDF terrain's chunk meshes are lit (`BTBHGPUSdfNormals`).
public enum GPUSdfNormals: UInt8, Hashable, Sendable {
    /// Shared vertices, normals from the field's gradient.
    case smooth = 0
    /// Every triangle with its own three vertices, its face normal and one material: a low-poly, flat-coloured
    /// look, with about six times the vertices of `.smooth`.
    case faceted = 1
}

/// What `GPUSdfTerrain.addEdit` does once the terrain holds `maxEdits` edits (`BTBHGPUSdfEditCap`).
public enum GPUSdfEditCap: UInt8, Hashable, Sendable {
    /// The oldest edits fold into the base (their ids stop being valid), so a valid edit is never refused.
    case freeze = 0
    /// `addEdit` returns nil.
    case reject = 1
}

/// One shape that adds to, cuts from, intersects or paints the ground (`BTBHGPUSdfEdit`). Edits apply in the
/// order they were added. Positions are flat world positions (before the world curvature bends them).
public struct GPUSdfEdit: Hashable, Sendable {
    public enum Shape: Hashable, Sendable {
        /// `size.x` is the radius.
        case sphere
        /// `size` holds the radii.
        case ellipsoid
        /// `size` holds the half extents; `cornerRadius` rounds it.
        case box
        /// Along local Y: `size.x` is the radius, `size.y` the half length.
        case capsule
        /// Along local Y: `size.x` is the radius, `size.y` the half height; `cornerRadius` rounds it.
        case cylinder
        /// In local XZ: `size.x` is the major radius, `size.y` the minor one (crater rims).
        case torus
    }

    public enum Operation: Hashable, Sendable {
        /// Union: mounds, rubble, rims, walls.
        case add
        /// Craters, trenches, holes.
        case subtract
        /// Keeps only what lies inside the shape, within the edit's bounds (the shape's box grown by its smoothing
        /// and a few cells); nothing outside that box changes.
        case intersect
        /// Material only: the shape of the ground is untouched.
        case paint
    }

    public var shape: Shape
    public var operation: Operation
    public var position: RenderVec3
    public var rotation: RenderQuaternion
    public var size: RenderVec3
    /// `.box` and `.cylinder`: at most the smallest half extent.
    public var cornerRadius: Float
    /// The smooth-min radius in world units; 0 is a sharp edge.
    public var smoothing: Float
    /// Written where this edit decides the surface: an index (0...7) into `materialColors`. `.subtract` ignores
    /// it unless `materialBand` > 0.
    public var material: UInt8
    /// `.subtract` and `.paint`: how far under the new surface `material` reaches (scorched walls, ash). With 0,
    /// `.subtract` exposes whatever the base has at that depth and `.paint` paints the whole shape.
    public var materialBand: Float

    public init(
        shape: Shape,
        operation: Operation,
        position: RenderVec3,
        rotation: RenderQuaternion = .identity,
        size: RenderVec3,
        cornerRadius: Float = 0,
        smoothing: Float = 0,
        material: UInt8 = 0,
        materialBand: Float = 0
    ) {
        self.shape = shape
        self.operation = operation
        self.position = position
        self.rotation = rotation
        self.size = size
        self.cornerRadius = cornerRadius
        self.smoothing = smoothing
        self.material = material
        self.materialBand = materialBand
    }
}

/// An edit's id. It stays valid until the edit is removed or frozen (`GPUSdfEditCap.freeze`).
public struct GPUSdfEditID: Hashable, Sendable {
    public let rawValue: UInt32

    public init(rawValue: UInt32) {
        self.rawValue = rawValue
    }
}

/// How to create a `GPUSdfTerrain` (`BTBHGPUSdfTerrainDesc`).
public struct GPUSdfTerrainDescriptor: Sendable {
    /// The ground before any edit.
    public enum Base: Hashable, Sendable {
        /// Flat ground whose surface is at `height`.
        case flat(height: Float)
        /// (width + 1) x (depth + 1) corner heights over the bounds' x and z, row by row along x, `cellSize`
        /// apart (copied by the C core at creation; the terrain keeps no Swift copy).
        case heightfield(heights: [Float], width: Int, depth: Int, cellSize: Float)
    }

    /// The world box the terrain may change.
    public var bounds: RenderBounds3
    /// The sample spacing, 0.1...4 world units.
    public var cellSize: Float
    /// Cells along a mesh chunk's edge, a multiple of 8 (8...256).
    public var chunkCells: Int
    public var maxEdits: Int
    public var editCap: GPUSdfEditCap
    public var normals: GPUSdfNormals
    public var base: Base
    /// The base's material (0...7) down to `subsurfaceDepth` under its surface, and below it.
    public var surfaceMaterial: UInt8
    public var subsurfaceMaterial: UInt8
    public var subsurfaceDepth: Float
    /// Each material's vertex colour (sRGB, as every colour in this layer), multiplied into the chunk material's
    /// albedo; up to 8. A missing one, or one whose four channels are all 0, is white.
    public var materialColors: [RenderColor]
    /// Builds bricks and chunk meshes on `DispatchQueue.concurrentPerform` workers; uploads stay on the thread
    /// that calls `update`. The result is the same either way.
    public var concurrentJobs: Bool

    public init(
        bounds: RenderBounds3,
        cellSize: Float,
        base: Base,
        chunkCells: Int = 64,
        maxEdits: Int = 4096,
        editCap: GPUSdfEditCap = .freeze,
        normals: GPUSdfNormals = .smooth,
        surfaceMaterial: UInt8 = 0,
        subsurfaceMaterial: UInt8 = 0,
        subsurfaceDepth: Float = 0,
        materialColors: [RenderColor] = [],
        concurrentJobs: Bool = false
    ) {
        self.bounds = bounds
        self.cellSize = cellSize
        self.base = base
        self.chunkCells = chunkCells
        self.maxEdits = maxEdits
        self.editCap = editCap
        self.normals = normals
        self.surfaceMaterial = surfaceMaterial
        self.subsurfaceMaterial = subsurfaceMaterial
        self.subsurfaceDepth = subsurfaceDepth
        self.materialColors = materialColors
        self.concurrentJobs = concurrentJobs
    }
}

/// What `GPUSdfTerrain.update` did and what the terrain holds (`BTBHGPUSdfTerrainReport`).
public struct GPUSdfTerrainReport: Hashable, Sendable {
    /// By the last update.
    public var bricksEvaluated: Int
    public var chunksMeshed: Int
    /// Still waiting after the budget ran out.
    public var pendingBricks: Int
    public var brickCount: Int
    /// Live edits.
    public var editCount: Int
    /// Folded into the base so far (`GPUSdfEditCap.freeze`).
    public var frozenEdits: Int
    /// All chunk meshes: their CPU copies, and their GPU buffers while attached.
    public var meshBytes: UInt64
    /// The dig mask (its CPU tiles, and its texture while attached); 0 until a patch exists.
    public var maskBytes: UInt64

    /// The last update found nothing to do: every edit so far shows.
    public var isSettled: Bool {
        pendingBricks == 0 && bricksEvaluated == 0 && chunksMeshed == 0
    }

    init(_ report: BTBHGPUSdfTerrainReport) {
        bricksEvaluated = Int(report.bricks_evaluated)
        chunksMeshed = Int(report.chunks_meshed)
        pendingBricks = Int(report.pending_bricks)
        brickCount = Int(report.brick_count)
        editCount = Int(report.edit_count)
        frozenEdits = Int(report.frozen_edits)
        meshBytes = report.mesh_bytes
        maskBytes = report.mask_bytes
    }
}

public enum GPUSdfTerrainError: Error, Equatable {
    case invalidDescriptor(String)
    case operationFailed(String)
}

/// One chunk to draw this frame.
struct GPUSdfTerrainChunkDraw {
    let mesh: OpaquePointer
    let bounds: RenderBounds3
    /// Its mesh changed within the last `GPUSdfTerrain.reactiveFrameCount` scene frames.
    let changedRecently: Bool
}

/// Destructible terrain from SDF edits (RFC 0007, `btbh_gpu_sdf_terrain.h`): the ground before any damage and an
/// ordered list of edits, meshed into chunk patches where the edits change it.
///
/// Create it, add edits, call `update` once a frame, and attach it with `GPUPlatform.setSdfTerrain`. While it is
/// attached, every scene frame draws its chunk meshes as ordinary batches with `material` (white albedo times the
/// vertex colour, culling back faces, casting and taking shadows, bent by the world curvature), and the geometry
/// of materials with `MaterialDescriptor.dugByTerrain` (or `ModelMaterialPatch.dugByTerrain`) is not drawn where
/// a patch has taken over from it. Under FSR 3 a chunk whose mesh just changed is also marked in the upscaler's
/// reactive mask for `reactiveFrameCount` frames, so the old ground doesn't ghost over the new.
///
/// Queries (`height`, `raycast`, `distance`) read the edit list itself: they are exact as soon as an edit is
/// added, whatever `update` has left pending, and they work without a window. Destroyed on deinit (which detaches
/// it first). Use it from the main actor, as the platform.
@MainActor
public final class GPUSdfTerrain {
    /// The material the chunks draw with: `MaterialDescriptor.default` (white, lit, back faces culled, casting
    /// shadows) unless set. Textures it names must stay alive while it is attached; its `dugByTerrain` is ignored.
    public var material: MaterialDescriptor = .default
    /// How many scene frames a chunk whose mesh changed stays in FSR 3's reactive mask (0: never).
    public var reactiveFrameCount = 4
    /// How many chunks the bounds hold.
    public let chunkCount: Int
    /// Whether a GPU platform draws it (`GPUPlatform.setSdfTerrain`).
    public var isAttached: Bool { platform != nil }

    let pointer: OpaquePointer
    private(set) weak var platform: GPUPlatform?
    private var chunkRevisions: [UInt64]
    private var chunkChangeFrames: [UInt64]
    private var drawFrame: UInt64 = 0
    private var chunkDraws: [GPUSdfTerrainChunkDraw] = []

    public init(_ descriptor: GPUSdfTerrainDescriptor) throws {
        var desc = BTBHGPUSdfTerrainDesc()
        let bounds = descriptor.bounds
        desc.bounds_min = (bounds.minimum.x, bounds.minimum.y, bounds.minimum.z)
        desc.bounds_max = (bounds.maximum.x, bounds.maximum.y, bounds.maximum.z)
        desc.cell_size = descriptor.cellSize
        guard descriptor.chunkCells > 0, descriptor.chunkCells <= Int(UInt32.max),
              descriptor.maxEdits > 0, descriptor.maxEdits <= Int(UInt32.max) else {
            throw GPUSdfTerrainError.invalidDescriptor("SDF terrain chunkCells and maxEdits must be positive")
        }
        desc.chunk_cells = UInt32(descriptor.chunkCells)
        desc.max_edits = UInt32(descriptor.maxEdits)
        desc.edit_cap = descriptor.editCap.rawValue
        desc.normals = descriptor.normals.rawValue
        desc.base_surface_material = descriptor.surfaceMaterial
        desc.base_subsurface_material = descriptor.subsurfaceMaterial
        desc.base_subsurface_depth = descriptor.subsurfaceDepth
        let heights: [Float]
        switch descriptor.base {
        case .flat(let height):
            // One heightfield cell over the whole box: a plane, with the surface and subsurface materials.
            heights = [height, height, height, height]
            desc.base_width = 1
            desc.base_depth = 1
            desc.base_cell = max(bounds.maximum.x - bounds.minimum.x, bounds.maximum.z - bounds.minimum.z)
        case .heightfield(let values, let width, let depth, let cellSize):
            guard width > 0, depth > 0, width < Int(UInt32.max), depth < Int(UInt32.max),
                  values.count == (width + 1) * (depth + 1) else {
                throw GPUSdfTerrainError.invalidDescriptor(
                    "SDF terrain heightfield needs (width + 1) x (depth + 1) heights"
                )
            }
            heights = values
            desc.base_width = UInt32(width)
            desc.base_depth = UInt32(depth)
            desc.base_cell = cellSize
        }
        guard descriptor.materialColors.count <= 8 else {
            throw GPUSdfTerrainError.invalidDescriptor("SDF terrain has at most 8 material colours")
        }
        withUnsafeMutableBytes(of: &desc.material_colors) { raw in
            let values = raw.bindMemory(to: Float.self)
            for (index, color) in descriptor.materialColors.enumerated() {
                values[index * 4] = GPUColorSpace.linearComponent(color.r)
                values[index * 4 + 1] = GPUColorSpace.linearComponent(color.g)
                values[index * 4 + 2] = GPUColorSpace.linearComponent(color.b)
                values[index * 4 + 3] = Float(color.a) / 255
            }
        }
        if descriptor.concurrentJobs {
            desc.run_jobs = { _, count, job, jobUser in
                guard let job else { return }
                // The jobs touch only the terrain's own memory (the C core's contract); the pointer goes over as
                // its address.
                let user = Int(bitPattern: jobUser)
                DispatchQueue.concurrentPerform(iterations: Int(count)) { index in
                    job(UnsafeMutableRawPointer(bitPattern: user), UInt32(index))
                }
            }
        }
        var report = BTBHGPUSdfTerrainReport()
        let created = heights.withUnsafeBufferPointer { buffer -> OpaquePointer? in
            desc.base_heights = buffer.baseAddress
            return btbh_gpu_sdf_terrain_create(&desc, &report)
        }
        guard let created else {
            throw GPUSdfTerrainError.invalidDescriptor(Self.message(&report))
        }
        pointer = created
        chunkCount = Int(btbh_gpu_sdf_terrain_chunk_count(created))
        chunkRevisions = Array(repeating: 0, count: chunkCount)
        chunkChangeFrames = Array(repeating: 0, count: chunkCount)
    }

    deinit {
        btbh_gpu_sdf_terrain_destroy(pointer)
    }

    /// Adds an edit after the others. nil for an invalid edit, or at `maxEdits` with `GPUSdfEditCap.reject`.
    public func addEdit(_ edit: GPUSdfEdit) -> GPUSdfEditID? {
        var native = Self.native(edit)
        let id = btbh_gpu_sdf_terrain_add_edit(pointer, &native)
        return id == 0 ? nil : GPUSdfEditID(rawValue: id)
    }

    /// Moves, resizes or deepens an edit; it keeps its place in the order. false for an invalid edit or an id that
    /// was removed or frozen: add a new edit instead.
    @discardableResult
    public func setEdit(_ id: GPUSdfEditID, to edit: GPUSdfEdit) -> Bool {
        var native = Self.native(edit)
        return btbh_gpu_sdf_terrain_set_edit(pointer, id.rawValue, &native) == BTBH_GPU_OK
    }

    /// false for an id that was removed or frozen.
    @discardableResult
    public func removeEdit(_ id: GPUSdfEditID) -> Bool {
        btbh_gpu_sdf_terrain_remove_edit(pointer, id.rawValue) == BTBH_GPU_OK
    }

    /// Evaluates what the edits changed, meshes the changed chunks and, while attached, uploads them, until
    /// `budgetMicroseconds` of this thread's time is spent (0: no limit); the rest waits for the next call. Chunks
    /// that touch switch to their new meshes together, so a half-built crater never shows.
    @discardableResult
    public func update(budgetMicroseconds: UInt32 = 0) throws -> GPUSdfTerrainReport {
        var report = BTBHGPUSdfTerrainReport()
        guard btbh_gpu_sdf_terrain_update(pointer, budgetMicroseconds, &report) == BTBH_GPU_OK else {
            throw GPUSdfTerrainError.operationFailed(Self.message(&report))
        }
        return GPUSdfTerrainReport(report)
    }

    /// The field at a flat world position: negative inside the ground.
    public func distance(at point: RenderVec3) -> Float {
        Self.withFloat3(point) { btbh_gpu_sdf_terrain_distance(pointer, $0) }
    }

    /// The first surface going down from (x, fromY, z), or nil when there is none (`fromY` under the ground, or a
    /// hole to the terrain's floor).
    public func height(x: Float, z: Float, fromY: Float) -> Float? {
        let height = btbh_gpu_sdf_terrain_height(pointer, x, z, fromY)
        return height.isFinite ? height : nil
    }

    /// The first surface along the ray within `maxDistance`.
    public func raycast(origin: RenderVec3, direction: RenderVec3, maxDistance: Float) -> RenderVec3? {
        var hit: (Float, Float, Float) = (0, 0, 0)
        let found = Self.withFloat3(origin) { origin in
            Self.withFloat3(direction) { direction in
                withUnsafeMutableBytes(of: &hit) { raw in
                    btbh_gpu_sdf_terrain_raycast(
                        pointer,
                        origin,
                        direction,
                        maxDistance,
                        raw.baseAddress!.assumingMemoryBound(to: Float.self)
                    )
                }
            }
        }
        return found != 0 ? RenderVec3(x: hit.0, y: hit.1, z: hit.2) : nil
    }

    /// The XZ rectangles whose look changed since the last call (`x` and `width` along world x, `y` and `height`
    /// along world z), to redo grass, decals, nav or occlusion only there. A rectangle is reported when its new
    /// mesh is ready, not when the edit is added.
    public func takeChanges() -> [RenderRect] {
        var changes: [RenderRect] = []
        var rectangles = [(Float, Float, Float, Float)](repeating: (0, 0, 0, 0), count: 64)
        while true {
            let count = rectangles.withUnsafeMutableBufferPointer {
                Int(btbh_gpu_sdf_terrain_take_changes(pointer, $0.baseAddress, UInt32($0.count)))
            }
            for rectangle in rectangles.prefix(count) {
                changes.append(.init(
                    x: rectangle.0,
                    y: rectangle.1,
                    width: rectangle.2 - rectangle.0,
                    height: rectangle.3 - rectangle.1
                ))
            }
            if count < rectangles.count { return changes }
        }
    }

    // MARK: The platform's side

    func didAttach(to platform: GPUPlatform) {
        self.platform = platform
    }

    func detachFromWindow() {
        btbh_gpu_sdf_terrain_detach(pointer)
        platform = nil
    }

    /// The chunks that have a mesh, for one scene frame; notes the chunks whose mesh changed since the last one.
    func chunksForFrame() -> [GPUSdfTerrainChunkDraw] {
        drawFrame &+= 1
        chunkDraws.removeAll(keepingCapacity: true)
        let reactiveFrames = UInt64(max(reactiveFrameCount, 0))
        var chunk = BTBHGPUSdfTerrainChunk()
        for index in 0..<chunkCount {
            guard btbh_gpu_sdf_terrain_chunk(pointer, UInt32(index), &chunk) != 0 else { continue }
            if chunk.revision != chunkRevisions[index] {
                chunkRevisions[index] = chunk.revision
                chunkChangeFrames[index] = drawFrame
            }
            guard let mesh = chunk.mesh else { continue }
            let changeFrame = chunkChangeFrames[index]
            chunkDraws.append(.init(
                mesh: mesh,
                bounds: .init(
                    minimum: .init(x: chunk.bounds_min.0, y: chunk.bounds_min.1, z: chunk.bounds_min.2),
                    maximum: .init(x: chunk.bounds_max.0, y: chunk.bounds_max.1, z: chunk.bounds_max.2)
                ),
                changedRecently: changeFrame != 0 && drawFrame - changeFrame < reactiveFrames
            ))
        }
        return chunkDraws
    }

    // MARK: Helpers

    private static func native(_ edit: GPUSdfEdit) -> BTBHGPUSdfEdit {
        var native = BTBHGPUSdfEdit()
        switch edit.shape {
        case .sphere: native.shape = BTBH_GPU_SDF_SHAPE_SPHERE
        case .ellipsoid: native.shape = BTBH_GPU_SDF_SHAPE_ELLIPSOID
        case .box: native.shape = BTBH_GPU_SDF_SHAPE_BOX
        case .capsule: native.shape = BTBH_GPU_SDF_SHAPE_CAPSULE
        case .cylinder: native.shape = BTBH_GPU_SDF_SHAPE_CYLINDER
        case .torus: native.shape = BTBH_GPU_SDF_SHAPE_TORUS
        }
        switch edit.operation {
        case .add: native.operation = BTBH_GPU_SDF_OPERATION_ADD
        case .subtract: native.operation = BTBH_GPU_SDF_OPERATION_SUBTRACT
        case .intersect: native.operation = BTBH_GPU_SDF_OPERATION_INTERSECT
        case .paint: native.operation = BTBH_GPU_SDF_OPERATION_PAINT
        }
        native.position = (edit.position.x, edit.position.y, edit.position.z)
        native.rotation = (edit.rotation.x, edit.rotation.y, edit.rotation.z, edit.rotation.w)
        native.size = (edit.size.x, edit.size.y, edit.size.z)
        native.corner_radius = edit.cornerRadius
        native.smoothing = edit.smoothing
        native.material = edit.material
        native.material_band = edit.materialBand
        return native
    }

    private static func withFloat3<Result>(
        _ vector: RenderVec3,
        _ body: (UnsafePointer<Float>) -> Result
    ) -> Result {
        var values = (vector.x, vector.y, vector.z)
        return withUnsafeBytes(of: &values) { raw in
            body(raw.baseAddress!.assumingMemoryBound(to: Float.self))
        }
    }

    private static func message(_ report: inout BTBHGPUSdfTerrainReport) -> String {
        withUnsafeBytes(of: &report.message) { raw in
            String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
    }
}
