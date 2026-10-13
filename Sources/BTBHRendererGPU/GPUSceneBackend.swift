import BTBHRenderer
import BTBHRendererGPUCore
import Foundation

struct GPUSceneRenderResolution {
    static func scaled(
        drawableSize: PixelSize2D,
        renderScale: Float
    ) throws -> PixelSize2D {
        guard drawableSize.width > 0,
              drawableSize.height > 0,
              drawableSize.width <= Int(Int32.max),
              drawableSize.height <= Int(Int32.max),
              renderScale.isFinite,
              renderScale > 0 else {
            throw GPUSceneProbeError.operationFailed(
                message: "Invalid GPU scene render resolution"
            )
        }
        let clampedScale = min(max(Double(renderScale), 0.1), 2)
        func scaledDimension(_ value: Int) throws -> Int {
            let scaled = Double(value) * clampedScale
            guard scaled.isFinite,
                  scaled >= 1,
                  scaled <= Double(Int32.max) else {
                throw GPUSceneProbeError.operationFailed(
                    message: "Invalid GPU scene render resolution"
                )
            }
            return Int(scaled)
        }
        return try .init(
            width: scaledDimension(drawableSize.width),
            height: scaledDimension(drawableSize.height)
        )
    }
}

struct GPUSceneFrustumCullingState {
    private enum Projection {
        case perspective(
            horizontalTangent: Float,
            verticalTangent: Float
        )
        case orthographic(halfWidth: Float, halfHeight: Float)
    }

    private let cameraPosition: RenderVec3
    private let forward: RenderVec3
    private let right: RenderVec3
    private let up: RenderVec3
    private let projection: Projection
    private let viewportPixelHeight: Float

    init?(frame: FrameDescriptor, drawableSize: PixelSize2D) {
        guard drawableSize.width > 0,
              drawableSize.height > 0 else {
            return nil
        }
        let camera = frame.camera
        let forward = (camera.target - camera.position).normalized()
        let right = forward.cross(camera.up).normalized()
        let up = right.cross(forward).normalized()
        guard forward.lengthSquared.isFinite,
              right.lengthSquared.isFinite,
              up.lengthSquared.isFinite,
              forward.lengthSquared > 0,
              right.lengthSquared > 0,
              up.lengthSquared > 0 else {
            return nil
        }
        let viewportWidth =
            Float(drawableSize.width) * frame.viewport.width
        let viewportHeight =
            Float(drawableSize.height) * frame.viewport.height
        guard viewportWidth.isFinite,
              viewportHeight.isFinite,
              viewportWidth > 0,
              viewportHeight > 0,
              camera.verticalFOVDegrees.isFinite,
              camera.verticalFOVDegrees > 0 else {
            return nil
        }
        let aspect = viewportWidth / viewportHeight
        viewportPixelHeight = viewportHeight
        switch camera.projection {
        case .perspective:
            guard camera.verticalFOVDegrees < 180 else {
                return nil
            }
            let verticalTangent = tan(
                camera.verticalFOVDegrees * .pi / 360
            )
            let horizontalTangent = verticalTangent * aspect
            guard verticalTangent.isFinite,
                  horizontalTangent.isFinite,
                  verticalTangent > 0,
                  horizontalTangent > 0 else {
                return nil
            }
            projection = .perspective(
                horizontalTangent: horizontalTangent,
                verticalTangent: verticalTangent
            )
        case .orthographic:
            let halfHeight = camera.verticalFOVDegrees * 0.5
            let halfWidth = halfHeight * aspect
            guard halfHeight.isFinite,
                  halfWidth.isFinite,
                  halfHeight > 0,
                  halfWidth > 0 else {
                return nil
            }
            projection = .orthographic(
                halfWidth: halfWidth,
                halfHeight: halfHeight
            )
        }
        cameraPosition = camera.position
        self.forward = forward
        self.right = right
        self.up = up
    }

    static func boundsAreValid(_ bounds: RenderBounds3) -> Bool {
        let minimum = bounds.minimum
        let maximum = bounds.maximum
        return minimum.x.isFinite &&
            minimum.y.isFinite &&
            minimum.z.isFinite &&
            maximum.x.isFinite &&
            maximum.y.isFinite &&
            maximum.z.isFinite &&
            minimum.x <= maximum.x &&
            minimum.y <= maximum.y &&
            minimum.z <= maximum.z
    }

    func intersects(_ bounds: RenderBounds3) -> Bool {
        guard Self.boundsAreValid(bounds) else { return false }
        return intersectsValidated(bounds)
    }

    func intersectsValidated(_ bounds: RenderBounds3) -> Bool {
        let minimum = bounds.minimum
        let maximum = bounds.maximum
        let center = (minimum + maximum) * 0.5
        let extents = (maximum - minimum) * 0.5
        let relativeCenter = center - cameraPosition
        let centerDepth = relativeCenter.dot(forward)
        let centerRight = relativeCenter.dot(right)
        let centerUp = relativeCenter.dot(up)
        let depthRadius = Self.projectedRadius(extents, on: forward)
        let rightRadius = Self.projectedRadius(extents, on: right)
        let upRadius = Self.projectedRadius(extents, on: up)
        guard centerDepth + depthRadius >= 0 else {
            return false
        }
        switch projection {
        case let .perspective(horizontalTangent, verticalTangent):
            let horizontalPlaneRadius =
                depthRadius * horizontalTangent + rightRadius
            if centerDepth * horizontalTangent - centerRight
                + horizontalPlaneRadius < 0 ||
                centerDepth * horizontalTangent + centerRight
                + horizontalPlaneRadius < 0 {
                return false
            }
            let verticalPlaneRadius =
                depthRadius * verticalTangent + upRadius
            if centerDepth * verticalTangent - centerUp
                + verticalPlaneRadius < 0 ||
                centerDepth * verticalTangent + centerUp
                + verticalPlaneRadius < 0 {
                return false
            }
        case let .orthographic(halfWidth, halfHeight):
            guard abs(centerRight) <= halfWidth + rightRadius,
                  abs(centerUp) <= halfHeight + upRadius else {
                return false
            }
        }
        return true
    }

    /// The on-screen diameter, in pixels, of a sphere of `radius` at the point of `bounds`
    /// nearest the camera. Infinite inside the bounds (a perspective camera there sees it close).
    func pixelDiameter(radius: Float, nearestPointOf bounds: RenderBounds3) -> Float {
        switch projection {
        case let .perspective(_, verticalTangent):
            let nearest = RenderVec3(
                x: min(max(cameraPosition.x, bounds.minimum.x), bounds.maximum.x),
                y: min(max(cameraPosition.y, bounds.minimum.y), bounds.maximum.y),
                z: min(max(cameraPosition.z, bounds.minimum.z), bounds.maximum.z)
            )
            let distance = (nearest - cameraPosition).dot(forward)
            guard distance > 0 else { return .infinity }
            return radius / (distance * verticalTangent) * viewportPixelHeight
        case let .orthographic(_, halfHeight):
            return radius / halfHeight * viewportPixelHeight
        }
    }

    private static func projectedRadius(
        _ extents: RenderVec3,
        on axis: RenderVec3
    ) -> Float {
        abs(axis.x) * extents.x +
            abs(axis.y) * extents.y +
            abs(axis.z) * extents.z
    }
}

@MainActor
final class GPUSceneBackend: SceneBackend {
    private enum PendingModelDraw {
        case model(ModelDraw)
        case instances(ModelInstancesDraw)
        case persistentRangeList(PersistentModelRangeListDraw)
        case transformStreams(ModelTransformStreamsDraw)
        case mesh(MeshDraw)
        case meshInstances(MeshInstancesDraw)
        case meshTransformStreams(MeshTransformStreamsDraw)
        case animated(AnimatedModelDraw)
        case animatedInstances(AnimatedModelInstancesDraw)
        case animatedTransformStreams(AnimatedModelTransformStreamsDraw)
    }

    private let platform: GPUPlatform
    private let resources: GPUResourceBackend
    private let frameCoordinator: GPUFrameCoordinator
    private var drawableSize = PixelSize2D(width: 0, height: 0)
    private var currentFrame: FrameDescriptor?
    private var frustumCullingState: GPUSceneFrustumCullingState?
    private var pendingModelDraws: [PendingModelDraw] = []
    private var pendingDebugDraws: [DebugDraw] = []
    private let submissionScratch = GPUSceneSubmissionScratch()
    private var modelBatchBuildSeconds = 0.0
    private var modelBatchOverrideEntryCount = 0
    /// The sun's direction from the previous frame, while it casts shadows. Persistent ranges the
    /// camera does not see still draw into the sun's shadow pass when the ground their shadow
    /// covers is in view: a low sun throws building shadows several times their height, and
    /// culling those casters to the view made shadows pop in and out at the screen edges as the
    /// camera followed and shook.
    private var sunShadowLightDirection: RenderVec3?
    /// Each transient instance's world translation since the previous frame, packed beside the
    /// instances while the temporal upscaler runs; empty when nothing this frame gave its motion.
    private var packedMotions: [SIMD4<Float>] = []
    /// The temporal upscaler's reactive-mask scale (`BTBH_FSR3_REACTIVE_SCALE`, default 1).
    private static let temporalReactiveScale: Float = {
        guard let value = ProcessInfo.processInfo.environment["BTBH_FSR3_REACTIVE_SCALE"]
            .flatMap(Float.init), value.isFinite, value >= 0 else {
            return 1
        }
        return value
    }()
    /// The projection's planes (`viewProjection(camera:viewport:)`).
    static let cameraNearPlane: Float = 0.01
    static let cameraFarPlane: Float = 1_000_000
    private(set) var lastPresentationReport:
        GPUScenePresentationProbeReport?
    var pendingModelDrawCountForTesting: Int {
        pendingModelDraws.count
    }

    init(
        platform: GPUPlatform,
        resources: GPUResourceBackend,
        frameCoordinator: GPUFrameCoordinator
    ) {
        self.platform = platform
        self.resources = resources
        self.frameCoordinator = frameCoordinator
    }

    func initialize(drawableSize: PixelSize2D) throws {
        self.drawableSize = drawableSize
    }

    func resize(drawableSize: PixelSize2D) throws {
        self.drawableSize = drawableSize
        frustumCullingState = currentFrame.flatMap {
            GPUSceneFrustumCullingState(
                frame: $0,
                drawableSize: drawableSize
            )
        }
    }

    func beginFrame(_ descriptor: FrameDescriptor) {
        currentFrame = descriptor
        frustumCullingState = GPUSceneFrustumCullingState(
            frame: descriptor,
            drawableSize: drawableSize
        )
        pendingModelDraws.removeAll(keepingCapacity: true)
        pendingDebugDraws.removeAll(keepingCapacity: true)
        submissionScratch.reset()
        modelBatchBuildSeconds = 0
        modelBatchOverrideEntryCount = 0
        lastPresentationReport = nil
    }

    func submit(_ draw: SceneDraw) {
        switch draw {
        case .model(let model):
            pendingModelDraws.append(.model(model))
        case .modelInstances(let modelInstances):
            if let last = pendingModelDraws.last,
               case .instances(let previous) = last,
               let merged = GPUModelInstanceDrawCoalescing.merge(
                previous,
                modelInstances
               ) {
                pendingModelDraws[pendingModelDraws.count - 1] =
                    .instances(merged)
            } else {
                pendingModelDraws.append(.instances(modelInstances))
            }
        case .modelTransformStreams(let streams):
            pendingModelDraws.append(.transformStreams(streams))
        case .mesh(let mesh):
            pendingModelDraws.append(.mesh(mesh))
        case .meshInstances(let meshInstances):
            pendingModelDraws.append(.meshInstances(meshInstances))
        case .meshTransformStreams(let streams):
            pendingModelDraws.append(.meshTransformStreams(streams))
        case .animatedModel(let animated):
            pendingModelDraws.append(.animated(animated))
        case .animatedModelInstances(let animatedInstances):
            pendingModelDraws.append(
                .animatedInstances(animatedInstances)
            )
        case .animatedModelTransformStreams(let streams):
            pendingModelDraws.append(
                .animatedTransformStreams(streams)
            )
        case .debug(let debug):
            pendingDebugDraws.append(debug)
        }
    }

    func submitPersistentModelRanges(
        _ draw: PersistentModelRangeListDraw
    ) {
        pendingModelDraws.append(.persistentRangeList(draw))
    }

    func endFrame() throws {
        do {
            try endFrameImpl()
        } catch {
            FileHandle.standardError.write(
                Data("BTBH_GPU_SCENE_FAILED error=\(error)\n".utf8)
            )
            throw error
        }
    }

    private func endFrameImpl() throws {
        guard !pendingModelDraws.isEmpty || !pendingDebugDraws.isEmpty else {
            return
        }
        let telemetryEnabled = platform.isFrameTelemetryEnabled
        let adapterStarted = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        guard let currentFrame else {
            throw GPUSceneProbeError.operationFailed(
                message: "Invalid GPU scene camera"
            )
        }
        let renderSize = try GPUSceneRenderResolution.scaled(
            drawableSize: drawableSize,
            renderScale: currentFrame.renderScale
        )
        guard let viewProjection = viewProjection(
                camera: currentFrame.camera,
                viewport: currentFrame.viewport
              ) else {
            throw GPUSceneProbeError.operationFailed(
                message: "Invalid GPU scene camera"
            )
        }
        try validatePersistentInstanceReuse()
        // FSR 3 needs a perspective camera over the whole drawable; other frames take FSR 1.
        let temporalActive =
            currentFrame.upscaleMode == .fsr3 &&
            currentFrame.camera.projection == .perspective &&
            currentFrame.viewport == .full
        packedMotions.removeAll(keepingCapacity: true)
        var packedInstances = submissionScratch.takePackedInstances()
        var batches = submissionScratch.takeBatches()
        var visiblePersistentRanges: [PersistentModelInstanceRange] = []
        var visibleLODPersistentRanges: [PersistentModelInstanceRange] = []
        var shadowOnlyPersistentRanges: [PersistentModelInstanceRange] = []
        var logicalInstanceCount = 0
        defer {
            submissionScratch.reclaim(
                packedInstances: &packedInstances,
                batches: &batches
            )
        }
        for pendingDraw in pendingModelDraws {
            switch pendingDraw {
            case .model(let draw):
                let firstInstance = packedInstances.count
                GPUInstanceRecordPacking.append(
                    matrix: draw.transform.matrix,
                    color: .white,
                    custom: GPUInstanceRecordPacking.zeroCustom,
                    to: &packedInstances
                )
                if temporalActive, draw.motion != .zero {
                    appendMotion(draw.motion, firstInstance: firstInstance)
                }
                try appendModelBatches(
                    model: draw.model,
                    overrides: draw.overrides,
                    animation: nil,
                    firstInstance: firstInstance,
                    instanceCount: 1,
                    to: &batches
                )
                logicalInstanceCount += 1
            case .instances(let draw):
                let persistentResource = try draw.persistentBuffer.map {
                    try resources.instanceBufferResource(
                        $0,
                        semanticInstances: draw.instances,
                        semanticRevision: draw.persistentRevision
                    )
                }
                let availableCount = draw.instances.count
                guard draw.sourceStart >= 0,
                      draw.count > 0,
                      draw.sourceStart <= availableCount,
                      draw.count <= availableCount - draw.sourceStart else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Invalid GPU model instance range"
                    )
                }
                let firstInstance: Int
                if persistentResource != nil {
                    firstInstance = draw.sourceStart
                } else {
                    firstInstance = packedInstances.count
                    for instance in draw.instances[
                        draw.sourceStart..<(draw.sourceStart + draw.count)
                    ] {
                        GPUInstanceRecordPacking.append(
                            instance,
                            to: &packedInstances
                        )
                    }
                }
                try appendModelBatches(
                    model: draw.model,
                    overrides: draw.overrides,
                    animation: nil,
                    legacyInstanced: true,
                    instanceBuffer: persistentResource?.storage.pointer,
                    firstInstance: firstInstance,
                    instanceCount: draw.count,
                    to: &batches
                )
                logicalInstanceCount += draw.count
            case .persistentRangeList(let draw):
                let persistentResource =
                    try resources.instanceBufferResource(
                        draw.persistentBuffer,
                        semanticInstances: draw.instances,
                        semanticRevision: draw.persistentRevision
                    )
                guard !draw.instances.isEmpty else {
                    throw GPUSceneProbeError.operationFailed(
                        message:
                            "Persistent GPU model range list requires instances"
                    )
                }
                let batchBuildStarted = telemetryEnabled
                    ? platform.monotonicElapsedSeconds
                    : nil
                if telemetryEnabled {
                    modelBatchOverrideEntryCount += draw.overrides.count
                }
                let geometry = GPUSceneGeometryProbe.model(
                    try resources.modelPointer(draw.model)
                )
                let materials = try resources.sceneModelMaterials(
                    model: draw.model,
                    overrides: draw.overrides
                )
                visiblePersistentRanges.removeAll(keepingCapacity: true)
                visibleLODPersistentRanges.removeAll(keepingCapacity: true)
                shadowOnlyPersistentRanges.removeAll(keepingCapacity: true)
                let lodGeometry = try draw.lod.map {
                    GPUSceneGeometryProbe.model(try resources.modelPointer($0.model))
                }
                for range in draw.ranges {
                    guard range.isValid(
                        instanceCount: draw.instances.count
                    ) else {
                        throw GPUSceneProbeError.operationFailed(
                            message:
                                "Invalid GPU persistent model range-list range"
                        )
                    }
                    guard intersectsFrustum(range.bounds) else {
                        if castsIntoSunShadow(range.bounds) {
                            shadowOnlyPersistentRanges.append(range)
                        }
                        continue
                    }
                    logicalInstanceCount += range.count
                    if let lod = draw.lod, lodGeometry != nil,
                       usesLOD(range: range, maximumPixelDiameter: lod.maximumPixelDiameter) {
                        visibleLODPersistentRanges.append(range)
                    } else {
                        visiblePersistentRanges.append(range)
                    }
                }
                // Materials come from the base model; an LOD swaps only the geometry (its
                // primitives keep the base model's material slots).
                for entry in materials {
                    for lodPass in 0..<2 {
                        let rangeGeometry = lodPass == 0 ? geometry : (lodGeometry ?? geometry)
                        for range in lodPass == 0 ? visiblePersistentRanges : visibleLODPersistentRanges {
                            batches.append(.init(
                                geometry: rangeGeometry,
                                animation: nil,
                                material: entry.material,
                                materialSlot: entry.materialSlot,
                                legacyInstanced: true,
                                instanceBuffer:
                                    persistentResource.storage.pointer,
                                firstInstance: range.sourceStart,
                                instanceCount: range.count,
                                shadowBounds: range.bounds,
                                spotShadowVisible: true
                            ))
                        }
                    }
                    for range in shadowOnlyPersistentRanges {
                        batches.append(.init(
                            geometry: lodGeometry ?? geometry,
                            animation: nil,
                            material: entry.material,
                            materialSlot: entry.materialSlot,
                            legacyInstanced: true,
                            instanceBuffer:
                                persistentResource.storage.pointer,
                            firstInstance: range.sourceStart,
                            instanceCount: range.count,
                            sceneVisible: false,
                            shadowBounds: range.bounds,
                            spotShadowVisible: true
                        ))
                    }
                }
                if let batchBuildStarted {
                    modelBatchBuildSeconds +=
                        platform.monotonicElapsedSeconds -
                        batchBuildStarted
                }
            case .transformStreams(let draw):
                guard draw.sourceStart >= 0,
                      draw.count > 0,
                      draw.sourceStart <= draw.positions.count,
                      draw.count <=
                        draw.positions.count - draw.sourceStart,
                      draw.sourceStart <= draw.rotations.count,
                      draw.count <=
                        draw.rotations.count - draw.sourceStart,
                      draw.sourceStart <= draw.scales.count,
                      draw.count <=
                        draw.scales.count - draw.sourceStart,
                      Self.containsRange(
                        draw.colors,
                        start: draw.sourceStart,
                        count: draw.count
                      ),
                      Self.containsRange(
                        draw.customs,
                        start: draw.sourceStart,
                        count: draw.count
                      ),
                      Self.containsRange(
                        draw.motions,
                        start: draw.sourceStart,
                        count: draw.count
                      ) else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Invalid GPU model transform stream range"
                    )
                }
                if let bounds = draw.clusterBounds,
                   !intersectsFrustum(bounds) {
                    continue
                }
                let firstInstance = packedInstances.count
                guard GPUInstanceRecordPacking.appendTransformStreams(
                    positions: draw.positions,
                    rotations: draw.rotations,
                    scales: draw.scales,
                    colors: draw.colors,
                    customs: draw.customs,
                    sourceStart: draw.sourceStart,
                    count: draw.count,
                    to: &packedInstances
                ) else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Could not pack GPU model transform streams"
                    )
                }
                if temporalActive, let motions = draw.motions {
                    appendMotions(
                        motions,
                        sourceStart: draw.sourceStart,
                        count: draw.count,
                        firstInstance: firstInstance
                    )
                }
                try appendModelBatches(
                    model: draw.model,
                    overrides: draw.overrides,
                    animation: nil,
                    legacyInstanced: true,
                    firstInstance: firstInstance,
                    instanceCount: draw.count,
                    shadowBounds: draw.clusterBounds,
                    to: &batches
                )
                logicalInstanceCount += draw.count
            case .mesh(let draw):
                let material = try resources.sceneMaterial(draw.material)
                let firstInstance = packedInstances.count
                GPUInstanceRecordPacking.append(
                    matrix: draw.transform.matrix,
                    color: .white,
                    custom: GPUInstanceRecordPacking.zeroCustom,
                    to: &packedInstances
                )
                batches.append(.init(
                    geometry: .customMesh(
                        try resources.meshPointer(draw.mesh)
                    ),
                    animation: nil,
                    material: material,
                    materialSlot: nil,
                    firstInstance: firstInstance,
                    instanceCount: 1
                ))
                logicalInstanceCount += 1
            case .meshInstances(let draw):
                guard !draw.instances.isEmpty else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "GPU mesh instance draw is empty"
                    )
                }
                let material = try resources.sceneMaterial(draw.material)
                let persistentResource = try draw.persistentBuffer.map {
                    try resources.instanceBufferResource(
                        $0,
                        semanticInstances: draw.instances,
                        semanticRevision: draw.persistentRevision
                    )
                }
                guard persistentResource == nil ||
                        draw.instances.count <=
                            persistentResource!.storage.capacity else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Invalid GPU persistent mesh instance range"
                    )
                }
                let firstInstance: Int
                if persistentResource != nil {
                    firstInstance = 0
                } else {
                    firstInstance = packedInstances.count
                    for instance in draw.instances {
                        GPUInstanceRecordPacking.append(
                            instance,
                            to: &packedInstances
                        )
                    }
                }
                batches.append(.init(
                    geometry: .customMesh(
                        try resources.meshPointer(draw.mesh)
                    ),
                    animation: nil,
                    material: material,
                    materialSlot: nil,
                    legacyInstanced: true,
                    instanceBuffer: persistentResource?.storage.pointer,
                    firstInstance: firstInstance,
                    instanceCount: draw.instances.count
                ))
                logicalInstanceCount += draw.instances.count
            case .meshTransformStreams(let draw):
                guard draw.sourceStart >= 0,
                      draw.count > 0,
                      draw.sourceStart <= draw.positions.count,
                      draw.count <=
                        draw.positions.count - draw.sourceStart,
                      draw.sourceStart <= draw.rotations.count,
                      draw.count <=
                        draw.rotations.count - draw.sourceStart,
                      draw.sourceStart <= draw.scales.count,
                      draw.count <=
                        draw.scales.count - draw.sourceStart,
                      draw.sourceStart <= draw.colors.count,
                      draw.count <=
                        draw.colors.count - draw.sourceStart,
                      Self.containsRange(
                        draw.motions,
                        start: draw.sourceStart,
                        count: draw.count
                      ) else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Invalid GPU mesh transform stream range"
                    )
                }
                let material = try resources.sceneMaterial(draw.material)
                let firstInstance = packedInstances.count
                guard GPUInstanceRecordPacking.appendTransformStreams(
                    positions: draw.positions,
                    rotations: draw.rotations,
                    scales: draw.scales,
                    colors: draw.colors,
                    customs: nil,
                    sourceStart: draw.sourceStart,
                    count: draw.count,
                    to: &packedInstances
                ) else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Could not pack GPU mesh transform streams"
                    )
                }
                if temporalActive, let motions = draw.motions {
                    appendMotions(
                        motions,
                        sourceStart: draw.sourceStart,
                        count: draw.count,
                        firstInstance: firstInstance
                    )
                }
                batches.append(.init(
                    geometry: .customMesh(
                        try resources.meshPointer(draw.mesh)
                    ),
                    animation: nil,
                    material: material,
                    materialSlot: nil,
                    legacyInstanced: true,
                    firstInstance: firstInstance,
                    instanceCount: draw.count
                ))
                logicalInstanceCount += draw.count
            case .animated(let draw):
                let firstInstance = packedInstances.count
                GPUInstanceRecordPacking.append(
                    matrix: draw.transform.matrix,
                    color: .white,
                    custom: GPUInstanceRecordPacking.zeroCustom,
                    to: &packedInstances
                )
                if temporalActive, draw.motion != .zero {
                    appendMotion(draw.motion, firstInstance: firstInstance)
                }
                try appendModelBatches(
                    model: draw.model,
                    overrides: draw.overrides,
                    animation: try resources.animationPose(
                        model: draw.model,
                        player: draw.player,
                        tree: draw.tree
                    ),
                    firstInstance: firstInstance,
                    instanceCount: 1,
                    to: &batches
                )
                logicalInstanceCount += 1
            case .animatedInstances(let draw):
                guard !draw.instances.isEmpty else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "GPU animated model instance draw is empty"
                    )
                }
                let firstInstance = packedInstances.count
                for instance in draw.instances {
                    GPUInstanceRecordPacking.append(
                        instance,
                        to: &packedInstances
                    )
                }
                try appendModelBatches(
                    model: draw.model,
                    overrides: draw.overrides,
                    animation: try resources.animationPose(
                        model: draw.model,
                        player: draw.player,
                        tree: draw.tree
                    ),
                    legacyInstanced: true,
                    firstInstance: firstInstance,
                    instanceCount: draw.instances.count,
                    to: &batches
                )
                logicalInstanceCount += draw.instances.count
            case .animatedTransformStreams(let draw):
                guard draw.sourceStart >= 0,
                      draw.count > 0,
                      draw.sourceStart <= draw.positions.count,
                      draw.count <=
                        draw.positions.count - draw.sourceStart,
                      draw.sourceStart <= draw.rotations.count,
                      draw.count <=
                        draw.rotations.count - draw.sourceStart,
                      draw.sourceStart <= draw.scales.count,
                      draw.count <=
                        draw.scales.count - draw.sourceStart,
                      Self.containsRange(
                        draw.colors,
                        start: draw.sourceStart,
                        count: draw.count
                      ),
                      Self.containsRange(
                        draw.customs,
                        start: draw.sourceStart,
                        count: draw.count
                      ),
                      Self.containsRange(
                        draw.motions,
                        start: draw.sourceStart,
                        count: draw.count
                      ) else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Invalid GPU animated transform stream range"
                    )
                }
                let firstInstance = packedInstances.count
                guard GPUInstanceRecordPacking.appendTransformStreams(
                    positions: draw.positions,
                    rotations: draw.rotations,
                    scales: draw.scales,
                    colors: draw.colors,
                    customs: draw.customs,
                    sourceStart: draw.sourceStart,
                    count: draw.count,
                    to: &packedInstances
                ) else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Could not pack GPU animated transform streams"
                    )
                }
                if temporalActive, let motions = draw.motions {
                    appendMotions(
                        motions,
                        sourceStart: draw.sourceStart,
                        count: draw.count,
                        firstInstance: firstInstance
                    )
                }
                try appendModelBatches(
                    model: draw.model,
                    overrides: draw.overrides,
                    animation: try resources.animationPose(
                        model: draw.model,
                        player: draw.player,
                        tree: draw.tree
                    ),
                    legacyInstanced: true,
                    firstInstance: firstInstance,
                    instanceCount: draw.count,
                    directionalShadowVisible: draw.castsSunShadow,
                    to: &batches
                )
                logicalInstanceCount += draw.count
            }
        }
        var transientDebugMesh: MeshHandle?
        defer {
            if let transientDebugMesh {
                resources.destroyMesh(transientDebugMesh)
            }
        }
        if !pendingDebugDraws.isEmpty {
            let debugVertices = Self.debugVertices(
                pendingDebugDraws,
                camera: currentFrame.camera
            )
            if !debugVertices.isEmpty {
                let bounds = Self.bounds(of: debugVertices)
                let mesh = try debugVertices.withUnsafeBytes {
                    try resources.createMesh(
                        .init(
                            topology: .triangles,
                            storage: .immutable,
                            vertexCount: debugVertices.count,
                            indexCount: 0,
                            bounds: bounds
                        ),
                        vertices: $0,
                        indices: nil
                    )
                }
                transientDebugMesh = mesh
                let firstInstance = packedInstances.count
                GPUInstanceRecordPacking.append(
                    matrix: .identity,
                    color: .white,
                    custom: GPUInstanceRecordPacking.zeroCustom,
                    to: &packedInstances
                )
                batches.append(.init(
                    geometry: .customMesh(
                        try resources.meshPointer(mesh)
                    ),
                    animation: nil,
                    material: nil,
                    materialSlot: nil,
                    firstInstance: firstInstance,
                    instanceCount: 1
                ))
                logicalInstanceCount += 1
            }
        }
        guard !batches.isEmpty else { return }
        if !packedMotions.isEmpty {
            padMotions(to: packedInstances.count)
        }
        let packingFinished = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        let cameraForward = (
            currentFrame.camera.target - currentFrame.camera.position
        ).normalized()
        let cameraRight = cameraForward
            .cross(currentFrame.camera.up)
            .normalized()
        let cameraUp = cameraRight
            .cross(cameraForward)
            .normalized()
        let cameraPreparationFinished = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        let lightingResolveStarted = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        let lighting = try resources.sceneLighting(
            environment: currentFrame.environment,
            camera: currentFrame.camera,
            aspect:
                Float(drawableSize.width) /
                Float(drawableSize.height)
        )
        sunShadowLightDirection = lighting.directionalShadow.enabled
            ? lighting.directionalDirection
            : nil
        let lightingResolveFinished = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        let clearColor = try resources.sceneClearColor(
            environment: currentFrame.environment
        )
        let clearColorResolveFinished = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        let post = try resources.scenePost(
            environment: currentFrame.environment,
            chain: currentFrame.postChain,
            antiAliasing: currentFrame.antiAliasing,
            upscaleMode: temporalActive
                ? .fsr3
                : (currentFrame.upscaleMode == .fsr3 ? .fsr1 : currentFrame.upscaleMode)
        )
        let temporal: GPUSceneTemporalProbe? = temporalActive
            ? GPUSceneTemporalProbe(
                instanceMotions: packedMotions.isEmpty ? nil : packedMotions,
                cameraNear: Self.cameraNearPlane,
                cameraFar: Self.cameraFarPlane,
                verticalFOVRadians: currentFrame.camera.verticalFOVDegrees * .pi / 180,
                frameSeconds: currentFrame.frameSeconds.isFinite
                    ? max(0, currentFrame.frameSeconds)
                    : 0,
                metersPerUnit: 1,
                reactiveScale: Self.temporalReactiveScale,
                resetsHistory: currentFrame.resetsTemporalHistory
            )
            : nil
        let lightingPreparationFinished = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        batches = GPUSceneBatchShadowCulling.applyingSpotFrustum(
            to: batches,
            shadow: lighting.spotShadow
        )
        let shadowCullingFinished = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        let orderedBatches =
            GPUSceneBatchOrdering.opaqueThenLegacyMaterialOrder(batches)
        let batchOrderingFinished = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        let consolidatedBatches =
            GPUSceneBatchConsolidation.adjacentPersistentRanges(
                orderedBatches
            )
        _ = frameCoordinator.claimScreenClearForScene()
        let preparationFinished = telemetryEnabled
            ? platform.monotonicElapsedSeconds
            : 0
        lastPresentationReport = try platform.presentModelInstances(
            renderSize: renderSize,
            viewProjection: viewProjection,
            cameraRight: cameraRight,
            cameraUp: cameraUp,
            cameraPosition: currentFrame.camera.position,
            lighting: lighting,
            post: post,
            instances: packedInstances,
            logicalInstanceCount: logicalInstanceCount,
            batches: consolidatedBatches,
            clearColor: clearColor,
            temporal: temporal
        )
        if telemetryEnabled {
            let presentationFinished = platform.monotonicElapsedSeconds
            platform.recordSceneAdapterTelemetry(
                packMilliseconds: (packingFinished - adapterStarted) * 1_000,
                modelBatchBuildMilliseconds: modelBatchBuildSeconds * 1_000,
                prepareMilliseconds:
                    (preparationFinished - packingFinished) * 1_000,
                cameraPrepareMilliseconds:
                    (
                        cameraPreparationFinished -
                        packingFinished
                    ) * 1_000,
                lightingPrepareMilliseconds:
                    (
                        lightingPreparationFinished -
                        cameraPreparationFinished
                    ) * 1_000,
                lightingResolveMilliseconds:
                    (
                        lightingResolveFinished -
                        lightingResolveStarted
                    ) * 1_000,
                clearColorResolveMilliseconds:
                    (
                        clearColorResolveFinished -
                        lightingResolveFinished
                    ) * 1_000,
                postResolveMilliseconds:
                    (
                        lightingPreparationFinished -
                        clearColorResolveFinished
                    ) * 1_000,
                shadowCullMilliseconds:
                    (
                        shadowCullingFinished -
                        lightingPreparationFinished
                    ) * 1_000,
                batchOrderingMilliseconds:
                    (
                        batchOrderingFinished -
                        shadowCullingFinished
                    ) * 1_000,
                batchConsolidationMilliseconds:
                    (
                        preparationFinished -
                        batchOrderingFinished
                    ) * 1_000,
                nativePresentMilliseconds:
                    (presentationFinished - preparationFinished) * 1_000,
                pendingDrawCount: pendingModelDraws.count,
                packedInstanceCount: packedInstances.count,
                batchCount: consolidatedBatches.count,
                materialResolutionCount:
                    resources.sceneModelMaterialResolutionCount,
                overrideEntryCount: modelBatchOverrideEntryCount
            )
        }
    }

    /// One instance's motion at `firstInstance`; instances packed before it without motion hold still.
    private func appendMotion(_ motion: RenderVec3, firstInstance: Int) {
        padMotions(to: firstInstance)
        packedMotions.append(SIMD4<Float>(motion.x, motion.y, motion.z, 0))
    }

    private func appendMotions(
        _ motions: [RenderVec3],
        sourceStart: Int,
        count: Int,
        firstInstance: Int
    ) {
        padMotions(to: firstInstance)
        for index in sourceStart..<(sourceStart + count) {
            let motion = motions[index]
            packedMotions.append(SIMD4<Float>(motion.x, motion.y, motion.z, 0))
        }
    }

    private func padMotions(to count: Int) {
        if packedMotions.count < count {
            packedMotions.append(
                contentsOf: repeatElement(SIMD4<Float>(repeating: 0), count: count - packedMotions.count)
            )
        }
    }

    private func appendModelBatches(
        model: ModelHandle,
        overrides: [MaterialOverride],
        animation: GPUAnimationPoseProbe?,
        legacyInstanced: Bool = false,
        instanceBuffer: OpaquePointer? = nil,
        firstInstance: Int,
        instanceCount: Int,
        sceneVisible: Bool = true,
        directionalShadowVisible: Bool = true,
        shadowBounds: RenderBounds3? = nil,
        spotShadowVisible: Bool = true,
        to batches: inout [GPUSceneGeometryProbeBatch]
    ) throws {
        let batchBuildStarted = platform.isFrameTelemetryEnabled
            ? platform.monotonicElapsedSeconds
            : nil
        if platform.isFrameTelemetryEnabled {
            modelBatchOverrideEntryCount += overrides.count
        }
        let geometry = GPUSceneGeometryProbe.model(
            try resources.modelPointer(model)
        )
        for entry in try resources.sceneModelMaterials(
            model: model,
            overrides: overrides
        ) {
            batches.append(.init(
                geometry: geometry,
                animation: animation,
                material: entry.material,
                materialSlot: entry.materialSlot,
                legacyInstanced: legacyInstanced,
                instanceBuffer: instanceBuffer,
                firstInstance: firstInstance,
                instanceCount: instanceCount,
                sceneVisible: sceneVisible,
                directionalShadowVisible:
                    directionalShadowVisible,
                shadowBounds: shadowBounds,
                spotShadowVisible: spotShadowVisible
            ))
        }
        if let batchBuildStarted {
            modelBatchBuildSeconds +=
                platform.monotonicElapsedSeconds - batchBuildStarted
        }
    }

    private static func debugVertices(
        _ draws: [DebugDraw],
        camera: RenderCamera
    ) -> [RenderVertex] {
        var vertices: [RenderVertex] = []
        for draw in draws {
            switch draw {
            case .line(let start, let end, let color):
                appendDebugLine(
                    start: start,
                    end: end,
                    color: color,
                    camera: camera,
                    to: &vertices
                )
            case .grid(let slices, let spacing):
                guard slices > 0, spacing > 0, spacing.isFinite else {
                    continue
                }
                let half = Float(slices) * spacing * 0.5
                let color = RenderColor(
                    r: 160,
                    g: 160,
                    b: 160,
                    a: 255
                )
                for index in 0...slices {
                    let offset = -half + Float(index) * spacing
                    appendDebugLine(
                        start: .init(x: -half, y: 0, z: offset),
                        end: .init(x: half, y: 0, z: offset),
                        color: color,
                        camera: camera,
                        to: &vertices
                    )
                    appendDebugLine(
                        start: .init(x: offset, y: 0, z: -half),
                        end: .init(x: offset, y: 0, z: half),
                        color: color,
                        camera: camera,
                        to: &vertices
                    )
                }
            case .plane(let center, let size, let color):
                let halfX = size.x * 0.5
                let halfZ = size.y * 0.5
                appendDebugQuad(
                    [
                        .init(
                            x: center.x - halfX,
                            y: center.y,
                            z: center.z - halfZ
                        ),
                        .init(
                            x: center.x + halfX,
                            y: center.y,
                            z: center.z - halfZ
                        ),
                        .init(
                            x: center.x + halfX,
                            y: center.y,
                            z: center.z + halfZ
                        ),
                        .init(
                            x: center.x - halfX,
                            y: center.y,
                            z: center.z + halfZ
                        )
                    ],
                    color: color,
                    to: &vertices
                )
            case .sphere(let center, let radius, let color):
                guard radius > 0, radius.isFinite else { continue }
                let segments = 24
                for axis in 0..<3 {
                    for index in 0..<segments {
                        let first = Float(index) * 2 * .pi /
                            Float(segments)
                        let second = Float(index + 1) * 2 * .pi /
                            Float(segments)
                        appendDebugLine(
                            start: debugCirclePoint(
                                center: center,
                                radius: radius,
                                radians: first,
                                axis: axis
                            ),
                            end: debugCirclePoint(
                                center: center,
                                radius: radius,
                                radians: second,
                                axis: axis
                            ),
                            color: color,
                            camera: camera,
                            to: &vertices
                        )
                    }
                }
            case .bounds(let bounds, let color):
                appendDebugBox(
                    minimum: bounds.minimum,
                    maximum: bounds.maximum,
                    color: color,
                    camera: camera,
                    to: &vertices
                )
            case .wireCube(let center, let size, let color):
                let half = size * 0.5
                appendDebugBox(
                    minimum: center - half,
                    maximum: center + half,
                    color: color,
                    camera: camera,
                    to: &vertices
                )
            }
        }
        return vertices
    }

    private static func appendDebugLine(
        start: RenderVec3,
        end: RenderVec3,
        color: RenderColor,
        camera: RenderCamera,
        to vertices: inout [RenderVertex]
    ) {
        let direction = end - start
        guard direction.lengthSquared > 0,
              direction.lengthSquared.isFinite else {
            return
        }
        let midpoint = (start + end) * 0.5
        var side = direction.cross(camera.position - midpoint)
        if side.lengthSquared < 0.000_001 {
            side = direction.cross(.up)
        }
        if side.lengthSquared < 0.000_001 {
            side = direction.cross(.right)
        }
        guard side.lengthSquared > 0 else { return }
        side = side.normalized() * 0.015
        appendDebugQuad(
            [start - side, start + side, end + side, end - side],
            color: color,
            to: &vertices
        )
    }

    private static func appendDebugQuad(
        _ points: [RenderVec3],
        color: RenderColor,
        to vertices: inout [RenderVertex]
    ) {
        guard points.count == 4 else { return }
        for index in [0, 1, 2, 0, 2, 3] {
            vertices.append(debugVertex(points[index], color: color))
        }
    }

    private static func appendDebugBox(
        minimum: RenderVec3,
        maximum: RenderVec3,
        color: RenderColor,
        camera: RenderCamera,
        to vertices: inout [RenderVertex]
    ) {
        let corners = [
            RenderVec3(x: minimum.x, y: minimum.y, z: minimum.z),
            RenderVec3(x: maximum.x, y: minimum.y, z: minimum.z),
            RenderVec3(x: maximum.x, y: maximum.y, z: minimum.z),
            RenderVec3(x: minimum.x, y: maximum.y, z: minimum.z),
            RenderVec3(x: minimum.x, y: minimum.y, z: maximum.z),
            RenderVec3(x: maximum.x, y: minimum.y, z: maximum.z),
            RenderVec3(x: maximum.x, y: maximum.y, z: maximum.z),
            RenderVec3(x: minimum.x, y: maximum.y, z: maximum.z)
        ]
        for (first, second) in [
            (0, 1), (1, 2), (2, 3), (3, 0),
            (4, 5), (5, 6), (6, 7), (7, 4),
            (0, 4), (1, 5), (2, 6), (3, 7)
        ] {
            appendDebugLine(
                start: corners[first],
                end: corners[second],
                color: color,
                camera: camera,
                to: &vertices
            )
        }
    }

    private static func debugCirclePoint(
        center: RenderVec3,
        radius: Float,
        radians: Float,
        axis: Int
    ) -> RenderVec3 {
        let first = cos(radians) * radius
        let second = sin(radians) * radius
        switch axis {
        case 0:
            return center + .init(x: 0, y: first, z: second)
        case 1:
            return center + .init(x: first, y: 0, z: second)
        default:
            return center + .init(x: first, y: second, z: 0)
        }
    }

    private static func debugVertex(
        _ position: RenderVec3,
        color: RenderColor
    ) -> RenderVertex {
        .init(
            position: position,
            textureCoordinate: .zero,
            normal: .up,
            tangent: .init(x: 1, y: 0, z: 0, w: 1),
            color: color
        )
    }

    private static func bounds(
        of vertices: [RenderVertex]
    ) -> RenderBounds3 {
        var minimum = RenderVec3(
            x: .greatestFiniteMagnitude,
            y: .greatestFiniteMagnitude,
            z: .greatestFiniteMagnitude
        )
        var maximum = RenderVec3(
            x: -.greatestFiniteMagnitude,
            y: -.greatestFiniteMagnitude,
            z: -.greatestFiniteMagnitude
        )
        for vertex in vertices {
            minimum.x = min(minimum.x, vertex.position.x)
            minimum.y = min(minimum.y, vertex.position.y)
            minimum.z = min(minimum.z, vertex.position.z)
            maximum.x = max(maximum.x, vertex.position.x)
            maximum.y = max(maximum.y, vertex.position.y)
            maximum.z = max(maximum.z, vertex.position.z)
        }
        return .init(minimum: minimum, maximum: maximum)
    }

    func projectNormalized(
        _ world: RenderVec3,
        camera: RenderCamera,
        viewport: NormalizedViewport
    ) -> NormalizedViewportPoint {
        guard let aspectRatio = viewportAspect(viewport) else {
            return .zero
        }
        return RenderCameraProjection.projectNormalized(
            world,
            camera: camera,
            aspectRatio: aspectRatio
        )
    }

    func unprojectRay(
        _ point: NormalizedViewportPoint,
        camera: RenderCamera,
        viewport: NormalizedViewport
    ) -> RenderRay {
        guard let aspectRatio = viewportAspect(viewport) else {
            return .init(origin: .zero, direction: .zero)
        }
        return RenderCameraProjection.unprojectRay(
            point,
            camera: camera,
            aspectRatio: aspectRatio
        )
    }

    func intersectsFrustum(_ bounds: RenderBounds3) -> Bool {
        guard GPUSceneFrustumCullingState.boundsAreValid(bounds) else {
            return false
        }
        guard let frustumCullingState else { return true }
        return frustumCullingState.intersectsValidated(bounds)
    }

    /// Whether a range the camera sees is small enough on screen for its draw's LOD.
    private func usesLOD(range: PersistentModelInstanceRange, maximumPixelDiameter: Float) -> Bool {
        guard range.objectRadius > 0, maximumPixelDiameter > 0,
              let frustumCullingState else { return false }
        return frustumCullingState.pixelDiameter(
            radius: range.objectRadius,
            nearestPointOf: range.bounds
        ) < maximumPixelDiameter
    }

    /// Whether bounds the camera does not see still throw sun shadow onto ground it does see.
    private func castsIntoSunShadow(_ bounds: RenderBounds3) -> Bool {
        guard let sunShadowLightDirection,
              let footprint = GPUSceneBatchShadowCulling.sunShadowFootprint(
                of: bounds,
                lightDirection: sunShadowLightDirection
              ) else {
            return false
        }
        return intersectsFrustum(footprint)
    }

    func readbackWorldFrame() throws -> FrameReadback {
        throw GPUBackendUnsupportedError.operation(
            "scene.readbackWorldFrame"
        )
    }

    func captureWorldFrameTexture() throws -> TextureHandle {
        try resources.adoptTexture(
            try platform.capturePendingSceneTexture()
        )
    }

    func copyWorldFrame(into texture: TextureHandle) throws {
        try platform.copyPendingSceneTexture(
            into: try resources.canvasTexture(texture)
        )
    }

    func shutdown() {
        currentFrame = nil
        frustumCullingState = nil
        sunShadowLightDirection = nil
        pendingModelDraws.removeAll(keepingCapacity: false)
        pendingDebugDraws.removeAll(keepingCapacity: false)
        submissionScratch.release()
        drawableSize = .init(width: 0, height: 0)
    }

    private func cameraGeometry(
        _ camera: RenderCamera,
        viewport: NormalizedViewport = .full
    ) -> (
        forward: RenderVec3,
        right: RenderVec3,
        up: RenderVec3,
        halfWidth: Float,
        halfHeight: Float
    )? {
        guard let aspect = viewportAspect(viewport),
              camera.verticalFOVDegrees.isFinite,
              camera.verticalFOVDegrees > 0 else {
            return nil
        }
        let forward = (camera.target - camera.position).normalized()
        let right = forward.cross(camera.up).normalized()
        let up = right.cross(forward).normalized()
        guard forward.lengthSquared.isFinite,
              right.lengthSquared.isFinite,
              up.lengthSquared.isFinite,
              forward.lengthSquared > 0,
              right.lengthSquared > 0,
              up.lengthSquared > 0 else {
            return nil
        }
        let halfHeight: Float
        switch camera.projection {
        case .perspective:
            guard camera.verticalFOVDegrees < 180 else { return nil }
            halfHeight = tan(
                camera.verticalFOVDegrees * .pi / 360
            )
        case .orthographic:
            halfHeight = camera.verticalFOVDegrees * 0.5
        }
        let halfWidth = halfHeight * aspect
        guard halfWidth.isFinite,
              halfHeight.isFinite,
              halfWidth > 0,
              halfHeight > 0 else {
            return nil
        }
        return (forward, right, up, halfWidth, halfHeight)
    }

    private func viewportAspect(
        _ viewport: NormalizedViewport
    ) -> Float? {
        let viewportRight = viewport.x + viewport.width
        let viewportBottom = viewport.y + viewport.height
        guard drawableSize.width > 0,
              drawableSize.height > 0,
              viewport.x.isFinite,
              viewport.y.isFinite,
              viewport.width.isFinite,
              viewport.height.isFinite,
              viewport.x >= 0,
              viewport.y >= 0,
              viewport.width > 0,
              viewport.height > 0,
              viewportRight <= 1,
              viewportBottom <= 1 else {
            return nil
        }
        let viewportWidth = Float(drawableSize.width) * viewport.width
        let viewportHeight = Float(drawableSize.height) * viewport.height
        guard viewportWidth.isFinite,
              viewportHeight.isFinite,
              viewportWidth > 0,
              viewportHeight > 0 else {
            return nil
        }
        let aspect = viewportWidth / viewportHeight
        return aspect.isFinite && aspect > 0 ? aspect : nil
    }

    private func viewProjection(
        camera: RenderCamera,
        viewport: NormalizedViewport
    ) -> [Float]? {
        guard let geometry = cameraGeometry(
            camera,
            viewport: viewport
        ) else { return nil }
        let width = Float(drawableSize.width) * viewport.width
        let height = Float(drawableSize.height) * viewport.height
        guard width.isFinite, height.isFinite, width > 0, height > 0 else {
            return nil
        }
        let position = camera.position
        let view: [Float] = [
            geometry.right.x, geometry.up.x, geometry.forward.x, 0,
            geometry.right.y, geometry.up.y, geometry.forward.y, 0,
            geometry.right.z, geometry.up.z, geometry.forward.z, 0,
            -position.dot(geometry.right),
            -position.dot(geometry.up),
            -position.dot(geometry.forward),
            1,
        ]
        let near = Self.cameraNearPlane
        let far = Self.cameraFarPlane
        let depthScale = far / (far - near)
        let depthBias = -near * depthScale
        let projection: [Float]
        switch camera.projection {
        case .perspective:
            let inverseHalfHeight = 1 / geometry.halfHeight
            projection = [
                inverseHalfHeight / (width / height), 0, 0, 0,
                0, inverseHalfHeight, 0, 0,
                0, 0, depthScale, 1,
                0, 0, depthBias, 0,
            ]
        case .orthographic:
            projection = [
                1 / geometry.halfWidth, 0, 0, 0,
                0, 1 / geometry.halfHeight, 0, 0,
                0, 0, 1 / (far - near), 0,
                0, 0, -near / (far - near), 1,
            ]
        }
        return Self.multiply(
            Self.viewportPlacement(viewport),
            Self.multiply(projection, view)
        )
    }

    /// Places the camera's clip space inside `viewport`: the frame still renders the whole
    /// drawable, but the camera's image lands exactly in that sub-rectangle (normalized, y down),
    /// so a canvas can show the region 1:1. Screen-space passes invert the full matrix, so they
    /// stay consistent. The full viewport is the identity.
    static func viewportPlacement(_ viewport: NormalizedViewport) -> [Float] {
        let centerX = 2 * viewport.x + viewport.width - 1
        let centerY = 1 - 2 * viewport.y - viewport.height
        return [
            viewport.width, 0, 0, 0,
            0, viewport.height, 0, 0,
            0, 0, 1, 0,
            centerX, centerY, 0, 1,
        ]
    }

    private static func multiply(
        _ lhs: [Float],
        _ rhs: [Float]
    ) -> [Float] {
        var result = Array(repeating: Float.zero, count: 16)
        for column in 0..<4 {
            for row in 0..<4 {
                result[column * 4 + row] =
                    lhs[row] * rhs[column * 4] +
                    lhs[4 + row] * rhs[column * 4 + 1] +
                    lhs[8 + row] * rhs[column * 4 + 2] +
                    lhs[12 + row] * rhs[column * 4 + 3]
            }
        }
        return result
    }

    private static func containsRange<Element>(
        _ values: [Element]?,
        start: Int,
        count: Int
    ) -> Bool {
        guard let values else { return true }
        return start <= values.count && count <= values.count - start
    }

    private func validatePersistentInstanceReuse() throws {
        var snapshots: [
            InstanceBufferHandle: (
                instances: [RenderInstance],
                revision: UInt64?
            )
        ] = [:]
        for pendingDraw in pendingModelDraws {
            let persistent: (
                handle: InstanceBufferHandle?,
                instances: [RenderInstance],
                revision: UInt64?
            )
            switch pendingDraw {
            case .instances(let draw):
                persistent = (
                    draw.persistentBuffer,
                    draw.instances,
                    draw.persistentRevision
                )
            case .persistentRangeList(let draw):
                persistent = (
                    draw.persistentBuffer,
                    draw.instances,
                    draw.persistentRevision
                )
            case .meshInstances(let draw):
                persistent = (
                    draw.persistentBuffer,
                    draw.instances,
                    draw.persistentRevision
                )
            default:
                continue
            }
            guard let handle = persistent.handle else { continue }
            if let existing = snapshots[handle] {
                if let existingRevision = existing.revision,
                   let revision = persistent.revision {
                    guard existingRevision == revision else {
                        throw GPUSceneProbeError.operationFailed(
                            message: "Persistent GPU instance buffer revision changed within one frame"
                        )
                    }
                    continue
                }
                guard Self.sharesStorage(
                    existing.instances,
                    persistent.instances
                ) || existing.instances == persistent.instances else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Persistent GPU instance buffer data changed within one frame"
                    )
                }
            } else {
                snapshots[handle] = (
                    persistent.instances,
                    persistent.revision
                )
            }
        }
    }

    private static func sharesStorage<T>(_ lhs: [T], _ rhs: [T]) -> Bool {
        guard lhs.count == rhs.count else { return false }
        return lhs.withUnsafeBufferPointer { lhsBuffer in
            rhs.withUnsafeBufferPointer { rhsBuffer in
                lhsBuffer.baseAddress == rhsBuffer.baseAddress
            }
        }
    }

    private static func transformMatrix(
        position: RenderVec3,
        rotation: RenderQuaternion,
        scale: RenderVec3
    ) -> RenderMatrix4 {
        let xx = rotation.x * rotation.x
        let yy = rotation.y * rotation.y
        let zz = rotation.z * rotation.z
        let xy = rotation.x * rotation.y
        let xz = rotation.x * rotation.z
        let yz = rotation.y * rotation.z
        let wx = rotation.w * rotation.x
        let wy = rotation.w * rotation.y
        let wz = rotation.w * rotation.z
        return .init(
            c0: .init(
                (1 - 2 * (yy + zz)) * scale.x,
                2 * (xy + wz) * scale.x,
                2 * (xz - wy) * scale.x,
                0
            ),
            c1: .init(
                2 * (xy - wz) * scale.y,
                (1 - 2 * (xx + zz)) * scale.y,
                2 * (yz + wx) * scale.y,
                0
            ),
            c2: .init(
                2 * (xz + wy) * scale.z,
                2 * (yz - wx) * scale.z,
                (1 - 2 * (xx + yy)) * scale.z,
                0
            ),
            c3: .init(position.x, position.y, position.z, 1)
        )
    }

}
