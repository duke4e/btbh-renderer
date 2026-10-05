import BTBHRenderer
import BTBHRendererGPUCore
import Foundation

enum GPUColorSpace {
    static func linearComponent(_ srgb: Float) -> Float {
        let clamped = min(max(srgb, 0), 1)
        return clamped <= 0.04045
            ? clamped / 12.92
            : pow((clamped + 0.055) / 1.055, 2.4)
    }

    static func linearComponent(_ srgb: UInt8) -> Float {
        linearComponent(Float(srgb) / 255)
    }

    static func legacyAmbientComponent(_ authored: UInt8) -> Float {
        Float(authored) / 255
    }

    static func linearized(_ color: RenderColor) -> RenderColor {
        func channel(_ value: UInt8) -> UInt8 {
            UInt8(clamping: Int(
                (linearComponent(value) * 255).rounded()
            ))
        }
        return .init(
            r: channel(color.r),
            g: channel(color.g),
            b: channel(color.b),
            a: color.a
        )
    }
}

enum GPUTextureSamplingProjection {
    static func filter(
        _ descriptor: TextureLoadDescriptor
    ) -> TextureFilter {
        descriptor.filter ?? .nearest
    }

}

enum GPUAntiAliasingProjection {
    static func native(_ antiAliasing: AntiAliasing) -> UInt8 {
        switch antiAliasing {
        case .off: 0
        case .smaaMedium: 1
        case .smaaHigh: 2
        case .fxaa: 3
        }
    }
}

enum GPUUpscaleModeProjection {
    static func native(_ upscaleMode: UpscaleMode) -> UInt8 {
        switch upscaleMode {
        case .linear: 0
        case .lanczos: 1
        }
    }
}

public enum GPUBackendRequest: String, Equatable, Sendable {
    case vulkan
    case metal

    public init?(exactDriverName: String) {
        self.init(rawValue: exactDriverName)
    }

    public var driverName: String {
        rawValue
    }

    public static var platformDefault: Self {
        #if os(Windows)
        .vulkan
        #elseif os(macOS)
        .metal
        #else
        preconditionFailure(
            "The production GPU renderer supports Windows and macOS only"
        )
        #endif
    }
}

public struct GPUDeviceDiagnostics: Equatable, Sendable {
    public let requestedDriver: String
    public let actualDriver: String
    public let hardwareAccelerated: Bool
    public let supportedFeatureMask: UInt64
    public let missingFeatureMask: UInt64

    public init(
        requestedDriver: String,
        actualDriver: String,
        hardwareAccelerated: Bool,
        supportedFeatureMask: UInt64,
        missingFeatureMask: UInt64
    ) {
        self.requestedDriver = requestedDriver
        self.actualDriver = actualDriver
        self.hardwareAccelerated = hardwareAccelerated
        self.supportedFeatureMask = supportedFeatureMask
        self.missingFeatureMask = missingFeatureMask
    }
}

public enum GPUDeviceProbeError: Error, Equatable {
    case creationFailed(stage: Int32, message: String)
}

@MainActor
public final class GPUDeviceProbe {
    public let diagnostics: GPUDeviceDiagnostics
    public private(set) var isShutdown = false
    fileprivate var device: OpaquePointer?

    public init(
        request: GPUBackendRequest = .platformDefault,
        debugMode: Bool
    ) throws {
        var descriptor = BTBHGPUDeviceDescriptor()
        descriptor.backend = request == .vulkan
            ? BTBH_GPU_BACKEND_VULKAN
            : BTBH_GPU_BACKEND_METAL
        descriptor.required_features = 0x1ff
        descriptor.debug_mode = debugMode ? 1 : 0

        var report = BTBHGPUDeviceReport()
        guard let createdDevice = btbh_gpu_device_create(
            &descriptor,
            &report
        ) else {
            throw GPUDeviceProbeError.creationFailed(
                stage: Int32(report.error_stage.rawValue),
                message: Self.string(from: &report.message)
            )
        }
        device = createdDevice
        diagnostics = GPUDeviceDiagnostics(
            requestedDriver: request.driverName,
            actualDriver: Self.string(
                from: &report.capabilities.driver_name
            ),
            hardwareAccelerated:
                report.capabilities.hardware_accelerated != 0,
            supportedFeatureMask: report.capabilities.features,
            missingFeatureMask: report.validation.missing_features
        )
    }

    public func shutdown() {
        guard let device else {
            isShutdown = true
            return
        }
        btbh_gpu_device_shutdown(device)
        btbh_gpu_device_destroy(device)
        self.device = nil
        isShutdown = true
    }

    deinit {
        if let device {
            btbh_gpu_device_shutdown(device)
            btbh_gpu_device_destroy(device)
        }
    }

    fileprivate static func string<T>(from value: inout T) -> String {
        withUnsafePointer(to: &value) { pointer in
            pointer.withMemoryRebound(
                to: CChar.self,
                capacity: MemoryLayout<T>.size
            ) {
                String(cString: $0)
            }
        }
    }
}

public struct GPUWindowDiagnostics: Equatable, Sendable {
    public let logicalWidth: UInt32
    public let logicalHeight: UInt32
    public let drawableWidth: UInt32
    public let drawableHeight: UInt32
    public let claimed: Bool
    public let minimized: Bool
    public let hidden: Bool
    public let swapchainFormat: Int32
    public let presentMode: Int32

    fileprivate init(report: inout BTBHGPUWindowReport) {
        logicalWidth = report.logical_width
        logicalHeight = report.logical_height
        drawableWidth = report.drawable_width
        drawableHeight = report.drawable_height
        claimed = report.claimed != 0
        minimized = report.minimized != 0
        hidden = report.hidden != 0
        swapchainFormat = Int32(report.swapchain_format.rawValue)
        presentMode = Int32(report.present_mode.rawValue)
    }
}

public enum GPUWindowProbeError: Error, Equatable {
    case operationFailed(
        operation: String,
        stage: Int32,
        message: String
    )
}

enum GPUTextureProbeError: Error, Equatable {
    case unsupportedWrap
    case operationFailed(message: String)
}

struct GPUTextureProbeResource {
    let pointer: OpaquePointer
    let size: PixelSize2D
    let mipCount: Int
    let isRenderTarget: Bool
}

enum GPUFontProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUFontGlyphProbe {
    let advance: Float
    let offset: RenderVec2
    let size: RenderVec2
    let uvMinimum: RenderVec2
    let uvMaximum: RenderVec2
}

struct GPUFontProbeResource {
    let pointer: OpaquePointer
    let atlasTexture: OpaquePointer
    let baseSize: Int
    let ascent: Float
    let descent: Float
    let lineGap: Float
}

enum GPUModelProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUModelProbeResource {
    let pointer: OpaquePointer
    let metadata: ModelMetadata
    let materials: [GPUSceneMaterialProbe]
}

enum GPUInstanceBufferProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUInstanceBufferProbeResource {
    let pointer: OpaquePointer
    let capacity: Int
    let stride: Int
    let byteCapacity: Int
}

enum GPUCustomMeshProbeError: Error, Equatable {
    case operationFailed(message: String)
}

enum GPUSceneProbeError: Error, Equatable {
    case operationFailed(message: String)
}

enum GPUCanvasProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUCanvasVertexProbe {
    let position: RenderVec2
    let textureCoordinate: RenderVec2
    let color: RenderColor

    init(
        position: RenderVec2,
        textureCoordinate: RenderVec2 = .zero,
        color: RenderColor
    ) {
        self.position = position
        self.textureCoordinate = textureCoordinate
        self.color = color
    }
}

struct GPUCanvasBatchProbe {
    let firstVertex: Int
    let vertexCount: Int
    let blendMode: CanvasBlendMode
    let scissor: RenderRect?
    let texture: OpaquePointer?

    init(
        firstVertex: Int,
        vertexCount: Int,
        blendMode: CanvasBlendMode,
        scissor: RenderRect?,
        texture: OpaquePointer? = nil
    ) {
        self.firstVertex = firstVertex
        self.vertexCount = vertexCount
        self.blendMode = blendMode
        self.scissor = scissor
        self.texture = texture
    }

    func merged(
        with next: GPUCanvasBatchProbe,
        withinSamePass: Bool
    ) -> GPUCanvasBatchProbe? {
        guard withinSamePass,
              vertexCount <= Int.max - firstVertex,
              firstVertex + vertexCount == next.firstVertex,
              next.vertexCount <= Int.max - vertexCount,
              blendMode.rawValue == next.blendMode.rawValue,
              scissor == next.scissor,
              texture == next.texture else {
            return nil
        }
        return .init(
            firstVertex: firstVertex,
            vertexCount: vertexCount + next.vertexCount,
            blendMode: blendMode,
            scissor: scissor,
            texture: texture
        )
    }
}

struct GPUCanvasPassProbe {
    let firstBatch: Int
    let batchCount: Int
    let target: OpaquePointer?
    let clearColor: RenderColor?
    let screenClearToken: UInt64?
}

enum GPUSceneGeometryProbe: Equatable {
    case model(OpaquePointer)
    case customMesh(OpaquePointer)
}

struct GPUAnimationSampleProbe: Equatable {
    let animationIndex: Int
    let time: Float
    let weight: Float
}

struct GPUAnimationPoseProbe {
    let pointer: OpaquePointer
    let library: OpaquePointer
    let samples: [GPUAnimationSampleProbe]
}

struct GPUSceneGeometryProbeBatch {
    let geometry: GPUSceneGeometryProbe
    let animation: GPUAnimationPoseProbe?
    let material: GPUSceneMaterialProbe?
    let materialSlot: Int?
    let legacyInstanced: Bool
    let instanceBuffer: OpaquePointer?
    let firstInstance: Int
    let instanceCount: Int
    let sceneVisible: Bool
    let directionalShadowVisible: Bool
    var shadowBounds: RenderBounds3?
    var spotShadowVisible: Bool

    init(
        geometry: GPUSceneGeometryProbe,
        animation: GPUAnimationPoseProbe?,
        material: GPUSceneMaterialProbe?,
        materialSlot: Int?,
        legacyInstanced: Bool = false,
        instanceBuffer: OpaquePointer? = nil,
        firstInstance: Int,
        instanceCount: Int,
        sceneVisible: Bool = true,
        directionalShadowVisible: Bool = true,
        shadowBounds: RenderBounds3? = nil,
        spotShadowVisible: Bool = true
    ) {
        self.geometry = geometry
        self.animation = animation
        self.material = material
        self.materialSlot = materialSlot
        self.legacyInstanced = legacyInstanced
        self.instanceBuffer = instanceBuffer
        self.firstInstance = firstInstance
        self.instanceCount = instanceCount
        self.sceneVisible = sceneVisible
        self.directionalShadowVisible = directionalShadowVisible
        self.shadowBounds = shadowBounds
        self.spotShadowVisible = spotShadowVisible
    }
}

enum GPUSceneBatchOrdering {
    static func opaqueThenLegacyMaterialOrder(
        _ batches: [GPUSceneGeometryProbeBatch]
    ) -> [GPUSceneGeometryProbeBatch] {
        let keyed = batches.enumerated().map { index, batch in
            let transparent = batch.material.map {
                $0.transparency == .alpha || $0.blendMode != .mix
            } ?? false
            return (
                index: index,
                batch: batch,
                transparent: transparent,
                renderList: transparent
                    ? (batch.legacyInstanced ? 2 : 3)
                    : (batch.legacyInstanced ? 0 : 1)
            )
        }
        return keyed.sorted { left, right in
            if left.renderList != right.renderList {
                return left.renderList < right.renderList
            }
            if left.transparent, left.batch.legacyInstanced,
               let leftKey = left.batch.material?.legacySortKey,
               let rightKey = right.batch.material?.legacySortKey,
               leftKey != rightKey {
                return leftKey.isOrderedBefore(rightKey)
            }
            return left.index < right.index
        }.map(\.batch)
    }
}

enum GPUSceneBatchConsolidation {
    static func adjacentPersistentRanges(
        _ batches: [GPUSceneGeometryProbeBatch]
    ) -> [GPUSceneGeometryProbeBatch] {
        var consolidated: [GPUSceneGeometryProbeBatch] = []
        consolidated.reserveCapacity(batches.count)
        for batch in batches {
            guard let previous = consolidated.last,
                  canMerge(previous, batch) else {
                consolidated.append(batch)
                continue
            }
            consolidated[consolidated.count - 1] = .init(
                geometry: previous.geometry,
                animation: nil,
                material: previous.material,
                materialSlot: previous.materialSlot,
                legacyInstanced: true,
                instanceBuffer: previous.instanceBuffer,
                firstInstance: previous.firstInstance,
                instanceCount:
                    previous.instanceCount + batch.instanceCount,
                sceneVisible: previous.sceneVisible,
                directionalShadowVisible:
                    previous.directionalShadowVisible,
                spotShadowVisible: previous.spotShadowVisible
            )
        }
        return consolidated
    }

    private static func canMerge(
        _ left: GPUSceneGeometryProbeBatch,
        _ right: GPUSceneGeometryProbeBatch
    ) -> Bool {
        left.legacyInstanced &&
            right.legacyInstanced &&
            left.instanceBuffer != nil &&
            left.instanceBuffer == right.instanceBuffer &&
            left.geometry == right.geometry &&
            left.animation == nil &&
            right.animation == nil &&
            left.material == right.material &&
            left.materialSlot == right.materialSlot &&
            left.sceneVisible == right.sceneVisible &&
            left.directionalShadowVisible ==
                right.directionalShadowVisible &&
            left.spotShadowVisible == right.spotShadowVisible &&
            left.firstInstance <= Int.max - left.instanceCount &&
            left.firstInstance + left.instanceCount == right.firstInstance &&
            left.instanceCount <= Int.max - right.instanceCount
    }
}

enum GPUModelInstanceDrawCoalescing {
    static func merge(
        _ left: ModelInstancesDraw,
        _ right: ModelInstancesDraw
    ) -> ModelInstancesDraw? {
        guard
            left.model == right.model,
            left.overrides == right.overrides,
            let persistentBuffer = left.persistentBuffer,
            persistentBuffer == right.persistentBuffer,
            let persistentRevision = left.persistentRevision,
            persistentRevision == right.persistentRevision,
            left.sourceStart >= 0,
            left.count > 0,
            left.sourceStart <= Int.max - left.count,
            left.sourceStart + left.count == right.sourceStart,
            right.count > 0,
            left.count <= Int.max - right.count,
            shareStorage(left.instances, right.instances),
            left.sourceStart <= left.instances.count,
            left.count + right.count <=
                left.instances.count - left.sourceStart
        else {
            return nil
        }
        return .init(
            model: left.model,
            instances: left.instances,
            overrides: left.overrides,
            persistentBuffer: persistentBuffer,
            persistentRevision: persistentRevision,
            sourceStart: left.sourceStart,
            count: left.count + right.count
        )
    }

    private static func shareStorage(
        _ left: [RenderInstance],
        _ right: [RenderInstance]
    ) -> Bool {
        guard left.count == right.count, !left.isEmpty else {
            return false
        }
        return left.withUnsafeBufferPointer { leftBuffer in
            right.withUnsafeBufferPointer { rightBuffer in
                leftBuffer.baseAddress == rightBuffer.baseAddress
            }
        }
    }
}

enum GPUSceneBatchShadowCulling {
    /// A batch whose material does not cast shadows stays out of both shadow passes.
    static func castsDirectionalShadow(_ batch: GPUSceneGeometryProbeBatch) -> Bool {
        batch.directionalShadowVisible && batch.material?.castsShadow != false
    }

    static func castsSpotShadow(_ batch: GPUSceneGeometryProbeBatch) -> Bool {
        batch.spotShadowVisible && batch.material?.castsShadow != false
    }

    /// The ground a box can darken under a directional light travelling along `lightDirection`:
    /// the box joined with its top swept down to its base. Nil for invalid bounds or a light at
    /// or below the horizon; a light lower than about 5 degrees reaches no further than that.
    static func sunShadowFootprint(
        of bounds: RenderBounds3,
        lightDirection: RenderVec3
    ) -> RenderBounds3? {
        let length = (
            lightDirection.x * lightDirection.x +
            lightDirection.y * lightDirection.y +
            lightDirection.z * lightDirection.z
        ).squareRoot()
        guard length.isFinite, length > 0,
              GPUSceneFrustumCullingState.boundsAreValid(bounds) else {
            return nil
        }
        let descent = -lightDirection.y / length
        guard descent > 0.01 else { return nil }
        let reach = (bounds.maximum.y - bounds.minimum.y) / max(descent, 0.08)
        let dx = lightDirection.x / length * reach
        let dz = lightDirection.z / length * reach
        return RenderBounds3(
            minimum: RenderVec3(
                x: bounds.minimum.x + min(0, dx),
                y: bounds.minimum.y,
                z: bounds.minimum.z + min(0, dz)
            ),
            maximum: RenderVec3(
                x: bounds.maximum.x + max(0, dx),
                y: bounds.maximum.y,
                z: bounds.maximum.z + max(0, dz)
            )
        )
    }

    static func applyingSpotFrustum(
        to batches: [GPUSceneGeometryProbeBatch],
        shadow: GPUSceneDirectionalShadowProbe
    ) -> [GPUSceneGeometryProbeBatch] {
        guard shadow.enabled, shadow.viewProjection.count == 16 else {
            return batches
        }
        return batches.map { batch in
            var result = batch
            if let bounds = batch.shadowBounds {
                result.spotShadowVisible = intersectsClipFrustum(
                    bounds,
                    viewProjection: shadow.viewProjection
                )
            }
            result.shadowBounds = nil
            return result
        }
    }

    static func intersectsClipFrustum(
        _ bounds: RenderBounds3,
        viewProjection matrix: [Float]
    ) -> Bool {
        let minimum = bounds.minimum
        let maximum = bounds.maximum
        let values = [
            minimum.x, minimum.y, minimum.z,
            maximum.x, maximum.y, maximum.z,
        ]
        guard values.allSatisfy(\.isFinite),
              minimum.x <= maximum.x,
              minimum.y <= maximum.y,
              minimum.z <= maximum.z,
              matrix.allSatisfy(\.isFinite) else {
            return true
        }

        var allLeft = true
        var allRight = true
        var allBottom = true
        var allTop = true
        var allNear = true
        var allFar = true
        for x in [minimum.x, maximum.x] {
            for y in [minimum.y, maximum.y] {
                for z in [minimum.z, maximum.z] {
                    let clipX =
                        matrix[0] * x + matrix[4] * y +
                        matrix[8] * z + matrix[12]
                    let clipY =
                        matrix[1] * x + matrix[5] * y +
                        matrix[9] * z + matrix[13]
                    let clipZ =
                        matrix[2] * x + matrix[6] * y +
                        matrix[10] * z + matrix[14]
                    let clipW =
                        matrix[3] * x + matrix[7] * y +
                        matrix[11] * z + matrix[15]
                    guard clipX.isFinite, clipY.isFinite,
                          clipZ.isFinite, clipW.isFinite else {
                        return true
                    }
                    allLeft = allLeft && clipX < -clipW
                    allRight = allRight && clipX > clipW
                    allBottom = allBottom && clipY < -clipW
                    allTop = allTop && clipY > clipW
                    allNear = allNear && clipZ < 0
                    allFar = allFar && clipZ > clipW
                }
            }
        }
        return !(allLeft || allRight || allBottom || allTop ||
                 allNear || allFar)
    }
}

struct GPULegacyMaterialSortKey: Equatable {
    let priority: Int32
    let shader: UInt64
    let shading: UInt32
    let albedo: UInt32
    let normal: UInt32
    let orm: UInt32
    let emission: UInt32
    let stencilHash: UInt32
    let depthHash: UInt32
    let blend: UInt8
    let cull: UInt8
    let transparency: UInt8
    let billboard: UInt8

    init(descriptor: MaterialDescriptor) {
        priority = 0
        shader = descriptor.surfaceShader.rawValue
        shading = descriptor.unlit ? 1 : 0
        albedo = Self.resourceOrder(descriptor.albedoTexture.rawValue)
        normal = Self.resourceOrder(descriptor.normalTexture.rawValue)
        orm = Self.resourceOrder(descriptor.ormTexture.rawValue)
        emission = Self.resourceOrder(descriptor.emissionTexture.rawValue)
        stencilHash = Self.stencilStateHash(descriptor)
        depthHash = Self.depthStateHash(descriptor)
        blend = descriptor.blend.rawValue
        switch descriptor.cullMode {
        case .none: cull = 0
        case .back: cull = 1
        case .front: cull = 2
        }
        switch descriptor.transparency {
        case .disabled: transparency = 0
        case .alpha: transparency = 2
        }
        billboard = descriptor.billboard.rawValue
    }

    func isOrderedBefore(_ other: Self) -> Bool {
        if priority != other.priority { return priority < other.priority }
        if shader != other.shader {
            return shader.byteSwapped < other.shader.byteSwapped
        }
        if shading != other.shading {
            return shading.byteSwapped < other.shading.byteSwapped
        }
        if albedo != other.albedo {
            return albedo.byteSwapped < other.albedo.byteSwapped
        }
        if normal != other.normal {
            return normal.byteSwapped < other.normal.byteSwapped
        }
        if orm != other.orm {
            return orm.byteSwapped < other.orm.byteSwapped
        }
        if emission != other.emission {
            return emission.byteSwapped < other.emission.byteSwapped
        }
        if stencilHash != other.stencilHash {
            return stencilHash.byteSwapped < other.stencilHash.byteSwapped
        }
        if depthHash != other.depthHash {
            return depthHash.byteSwapped < other.depthHash.byteSwapped
        }
        if blend != other.blend { return blend < other.blend }
        if cull != other.cull { return cull < other.cull }
        if transparency != other.transparency {
            return transparency < other.transparency
        }
        return billboard < other.billboard
    }

    private static func resourceOrder(_ rawValue: UInt64) -> UInt32 {
        UInt32(truncatingIfNeeded: rawValue)
    }

    private static func legacyCompare(_ value: CompareFunction) -> UInt32 {
        switch value {
        case .less: 0
        case .lessEqual: 1
        case .equal: 2
        case .greater: 3
        case .greaterEqual: 4
        case .notEqual: 5
        case .always: 6
        case .never: 7
        }
    }

    private static func depthStateHash(
        _ descriptor: MaterialDescriptor
    ) -> UInt32 {
        var hash: UInt32 = 2_166_136_261
        append(legacyCompare(descriptor.depthCompare), to: &hash)
        append(descriptor.depthOffsetFactor.bitPattern, to: &hash)
        append(descriptor.depthOffsetUnits.bitPattern, to: &hash)
        append(descriptor.depthRangeNear.bitPattern, to: &hash)
        append(descriptor.depthRangeFar.bitPattern, to: &hash)
        return hash
    }

    private static func stencilStateHash(
        _ descriptor: MaterialDescriptor
    ) -> UInt32 {
        var hash: UInt32 = 2_166_136_261
        append(legacyCompare(descriptor.stencilCompare), to: &hash)
        append(descriptor.stencilReference, to: &hash)
        append(descriptor.stencilMask, to: &hash)
        append(UInt8(0), to: &hash)
        append(UInt8(0), to: &hash)
        append(UInt32(descriptor.stencilFail.rawValue), to: &hash)
        append(UInt32(descriptor.stencilDepthFail.rawValue), to: &hash)
        append(UInt32(descriptor.stencilPass.rawValue), to: &hash)
        return hash
    }

    private static func append(_ value: UInt32, to hash: inout UInt32) {
        append(UInt8(truncatingIfNeeded: value), to: &hash)
        append(UInt8(truncatingIfNeeded: value >> 8), to: &hash)
        append(UInt8(truncatingIfNeeded: value >> 16), to: &hash)
        append(UInt8(truncatingIfNeeded: value >> 24), to: &hash)
    }

    private static func append(_ value: UInt8, to hash: inout UInt32) {
        hash ^= UInt32(value)
        hash &*= 16_777_619
    }
}

final class GPUSceneSubmissionScratch {
    var packedInstances: [GPUInstanceRecord] = []
    var batches: [GPUSceneGeometryProbeBatch] = []

    func reset() {
        packedInstances.removeAll(keepingCapacity: true)
        batches.removeAll(keepingCapacity: true)
    }

    func release() {
        packedInstances = []
        batches = []
    }

    func takePackedInstances() -> [GPUInstanceRecord] {
        var result: [GPUInstanceRecord] = []
        swap(&result, &packedInstances)
        return result
    }

    func takeBatches() -> [GPUSceneGeometryProbeBatch] {
        var result: [GPUSceneGeometryProbeBatch] = []
        swap(&result, &batches)
        return result
    }

    func reclaim(
        packedInstances: inout [GPUInstanceRecord],
        batches: inout [GPUSceneGeometryProbeBatch]
    ) {
        swap(&self.packedInstances, &packedInstances)
        swap(&self.batches, &batches)
    }
}

struct GPUSceneMaterialProbe: Equatable {
    let albedoTexture: OpaquePointer?
    let albedoColor: RenderColor
    let uvScale: RenderVec2
    let uvOffset: RenderVec2
    let normalTexture: OpaquePointer?
    let ormTexture: OpaquePointer?
    let normalScale: Float
    let occlusion: Float
    let roughness: Float
    let metalness: Float
    let emissionTexture: OpaquePointer?
    let emissionColor: RenderColor
    let emissionEnergy: Float
    let alphaCutoff: Float
    let blendMode: MaterialBlendMode
    let billboardMode: BillboardMode
    let depthCompare: CompareFunction
    let stencilCompare: CompareFunction
    let stencilReference: UInt8
    let stencilMask: UInt8
    let stencilFail: StencilOperation
    let stencilDepthFail: StencilOperation
    let stencilPass: StencilOperation
    let cullMode: CullMode
    let transparency: TransparencyMode
    let unlit: Bool
    let surface: GPUSceneSurfaceProjection
    var legacySortKey: GPULegacyMaterialSortKey? = nil
    var castsShadow = true
    var detail = SurfaceDetail.none
}

enum GPUSceneSurfaceDetailParameters {
    /// (albedo variation, bump height, cells per render unit, pattern); disabled detail is all zero
    /// but for a frequency of one, so the shader's pattern test skips it.
    static func native(_ detail: SurfaceDetail) -> (Float, Float, Float, Float) {
        guard detail.isEnabled else { return (0, 0, 1, 0) }
        return (detail.albedoVariation, detail.bumpHeight, detail.frequency, Float(detail.pattern.rawValue))
    }
}

struct GPUSceneLightingProbe {
    static let legacySpotInnerCutoff: Float =
        cos(22.5 * .pi / 180)
    static let legacySpotOuterCutoff: Float =
        cos(45 * .pi / 180)
    static let radianceSpotInnerCutoff: Float =
        cos(12 * .pi / 180)
    static let radianceSpotOuterCutoff: Float =
        cos(30 * .pi / 180)

    let ambientColor: RenderColor
    let ambientEnergy: Float
    let environment: GPUProceduralEnvironmentProjection?
    let directionalDirection: RenderVec3
    let directionalColor: RenderColor
    let directionalEnergy: Float
    let secondaryDirectionalDirection: RenderVec3
    let secondaryDirectionalColor: RenderColor
    let secondaryDirectionalEnergy: Float
    let spotPosition: RenderVec3
    let spotDirection: RenderVec3
    let spotColor: RenderColor
    let spotEnergy: Float
    let spotRange: Float
    let spotFalloff: Float
    let spotInnerCutoff: Float
    let spotOuterCutoff: Float
    let directionalShadow: GPUSceneDirectionalShadowProbe
    let secondaryDirectionalShadow: GPUSceneDirectionalShadowProbe
    let spotShadow: GPUSceneDirectionalShadowProbe
    let heroOmniShadow: GPUSceneOmniShadowProbe

    init(
        ambientColor: RenderColor,
        ambientEnergy: Float,
        environment: GPUProceduralEnvironmentProjection? = nil,
        directionalDirection: RenderVec3,
        directionalColor: RenderColor,
        directionalEnergy: Float,
        secondaryDirectionalDirection: RenderVec3,
        secondaryDirectionalColor: RenderColor,
        secondaryDirectionalEnergy: Float,
        spotPosition: RenderVec3 = .zero,
        spotDirection: RenderVec3 = .forward,
        spotColor: RenderColor = .white,
        spotEnergy: Float = 0,
        spotRange: Float = 50,
        spotFalloff: Float = 1,
        spotInnerCutoff: Float = Self.legacySpotInnerCutoff,
        spotOuterCutoff: Float = Self.legacySpotOuterCutoff,
        directionalShadow: GPUSceneDirectionalShadowProbe = .disabled,
        secondaryDirectionalShadow: GPUSceneDirectionalShadowProbe = .disabled,
        spotShadow: GPUSceneDirectionalShadowProbe = .disabled,
        heroOmniShadow: GPUSceneOmniShadowProbe = .disabled
    ) {
        self.ambientColor = ambientColor
        self.ambientEnergy = ambientEnergy
        self.environment = environment
        self.directionalDirection = directionalDirection
        self.directionalColor = directionalColor
        self.directionalEnergy = directionalEnergy
        self.secondaryDirectionalDirection =
            secondaryDirectionalDirection
        self.secondaryDirectionalColor = secondaryDirectionalColor
        self.secondaryDirectionalEnergy = secondaryDirectionalEnergy
        self.spotPosition = spotPosition
        self.spotDirection = spotDirection
        self.spotColor = spotColor
        self.spotEnergy = spotEnergy
        self.spotRange = spotRange
        self.spotFalloff = spotFalloff
        self.spotInnerCutoff = spotInnerCutoff
        self.spotOuterCutoff = spotOuterCutoff
        self.directionalShadow = directionalShadow
        self.secondaryDirectionalShadow = secondaryDirectionalShadow
        self.spotShadow = spotShadow
        self.heroOmniShadow = heroOmniShadow
    }
}

struct GPUScenePostProbe {
    static let `default` = Self(
        lutTexture: nil,
        tonemapMode: .reinhard,
        tonemapWhite: 1,
        exposure: 1,
        contrast: 1,
        saturation: 1,
        casSharpness: 0,
        focusPoint: .init(x: 0.5, y: 0.55),
        focusDirection: .init(x: 1, y: 0),
        focusHalfWidth: 0.05,
        maximumRadiusPixels: 0,
        blurCurve: 1,
        highlightThreshold: 0.7,
        highlightGain: 0,
        lutEnabled: false,
        casEnabled: false,
        tiltShiftEnabled: false,
        ssaoEnabled: false,
        antiAliasing: .off,
        upscaleMode: .linear,
        radianceCascades: .off,
        heroOmniLightSourceIndex: -1,
        mirrorHorizontally: false,
        fog: .disabled,
        bloom: .disabled,
        vignette: .disabled
    )

    let lutTexture: OpaquePointer?
    let tonemapMode: TonemapMode
    let tonemapWhite: Float
    let exposure: Float
    let contrast: Float
    let saturation: Float
    let casSharpness: Float
    let focusPoint: RenderVec2
    let focusDirection: RenderVec2
    let focusHalfWidth: Float
    let maximumRadiusPixels: Float
    let blurCurve: Float
    let highlightThreshold: Float
    let highlightGain: Float
    let lutEnabled: Bool
    let casEnabled: Bool
    let tiltShiftEnabled: Bool
    let ssaoEnabled: Bool
    let antiAliasing: AntiAliasing
    let upscaleMode: UpscaleMode
    let radianceCascades: RadianceCascadesDescriptor
    let heroOmniLightSourceIndex: Int32
    let mirrorHorizontally: Bool
    let fog: FogDescriptor
    let bloom: BloomDescriptor
    let vignette: VignetteDescriptor

    func withRadianceCascades(
        _ radianceCascades: RadianceCascadesDescriptor
    ) -> Self {
        .init(
            lutTexture: lutTexture,
            tonemapMode: tonemapMode,
            tonemapWhite: tonemapWhite,
            exposure: exposure,
            contrast: contrast,
            saturation: saturation,
            casSharpness: casSharpness,
            focusPoint: focusPoint,
            focusDirection: focusDirection,
            focusHalfWidth: focusHalfWidth,
            maximumRadiusPixels: maximumRadiusPixels,
            blurCurve: blurCurve,
            highlightThreshold: highlightThreshold,
            highlightGain: highlightGain,
            lutEnabled: lutEnabled,
            casEnabled: casEnabled,
            tiltShiftEnabled: tiltShiftEnabled,
            ssaoEnabled: ssaoEnabled,
            antiAliasing: antiAliasing,
            upscaleMode: upscaleMode,
            radianceCascades: radianceCascades,
            heroOmniLightSourceIndex: heroOmniLightSourceIndex,
            mirrorHorizontally: mirrorHorizontally,
            fog: fog,
            bloom: bloom,
            vignette: vignette
        )
    }
}

struct GPUSceneDirectionalShadowProbe: Equatable {
    /// The primary directional light (the sun) covers the whole view, so it gets the large map
    /// (BTBH_GPU_PRIMARY_DIRECTIONAL_SHADOW_MAP_SIZE); the secondary keeps `mapSize`.
    static let primaryMapSize: Float = 2048
    static let mapSize: Float = 512
    static let disabled = Self(
        enabled: false,
        range: 0,
        softness: 0,
        updatePeriodFrames: 1,
        viewProjection: [
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
        ]
    )

    let enabled: Bool
    let range: Float
    let softness: Float
    let updatePeriodFrames: UInt32
    let viewProjection: [Float]

    init(
        enabled: Bool,
        range: Float,
        softness: Float,
        updatePeriodFrames: UInt32 = 1,
        viewProjection: [Float]
    ) {
        self.enabled = enabled
        self.range = range
        self.softness = softness
        self.updatePeriodFrames = updatePeriodFrames
        self.viewProjection = viewProjection
    }
}

struct GPUSceneOmniShadowProbe: Equatable {
    static let faceCount = 6
    static let mapSize: Float = 512
    static let nearPlane: Float = 0.05
    static let disabled = Self(
        enabled: false,
        range: 0,
        softness: 0,
        updatePeriodFrames: 1,
        viewProjections: Array(
            repeating: GPUSceneDirectionalShadowProbe.disabled.viewProjection,
            count: faceCount
        ).flatMap { $0 }
    )

    let enabled: Bool
    let range: Float
    let softness: Float
    let updatePeriodFrames: UInt32
    let viewProjections: [Float]
}

struct GPUScenePresentationProbeReport {
    let drawableSize: PixelSize2D
    let renderSize: PixelSize2D
    let batchCount: Int
    let primitiveDrawCount: Int
    let preparedGeometryCount: Int
    let animatedGeometryCount: Int
    let animationSampleCount: Int
    let boneMatrixCount: Int
    let instanceCount: Int
    let transientInstanceCount: Int
    let persistentBatchCount: Int
    let shadowPipelineBindCount: Int
    let shadowPipelineBindSkipCount: Int
    let geometryPipelineBindCount: Int
    let geometryPipelineBindSkipCount: Int
    let shadowResourcesReady: Bool
    let cpuEncodeMilliseconds: Double
    let cpuPrepareMilliseconds: Double
    let cpuPrepareSetupMilliseconds: Double
    let cpuPrepareBonesMilliseconds: Double
    let cpuPrepareBoneCountMilliseconds: Double
    let cpuPrepareBoneAllocateMilliseconds: Double
    let cpuPrepareBoneSampleMilliseconds: Double
    let cpuPreparePipelinesMilliseconds: Double
    let cpuPrepareBuffersMilliseconds: Double
    let cpuUploadMilliseconds: Double
    let cpuShadowEncodeMilliseconds: Double
    let cpuSwapchainAcquireMilliseconds: Double
    let cpuTargetEnsureMilliseconds: Double
    let cpuGeometryEncodeMilliseconds: Double
    let cpuPostEncodeMilliseconds: Double
    let radianceCascadesStatus: RadianceCascadesStatus
}

struct GPUFrameTelemetrySample: Equatable {
    let frameSerial: UInt64
    let cpuSubmitMilliseconds: Double
    let fenceCompletionMilliseconds: Double
}

struct GPUCustomMeshProbeResource {
    let pointer: OpaquePointer
    let vertexCount: Int
    let indexCount: Int
    let vertexByteCount: Int
}

enum GPUAnimationLibraryProbeError: Error, Equatable {
    case operationFailed(message: String)
}

struct GPUAnimationLibraryProbeResource {
    let pointer: OpaquePointer
    let animationCount: Int
    let durations: [Float]
}

@MainActor
enum GPUAnimationLibraryProbe {
    static func load(
        path: String
    ) throws -> GPUAnimationLibraryProbeResource {
        var report = BTBHGPUAnimationLibraryReport()
        let library = path.withCString { pathPointer in
            btbh_gpu_animation_library_load(pathPointer, &report)
        }
        guard let library else {
            throw GPUAnimationLibraryProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        let animationCount = Int(report.animation_count)
        return .init(
            pointer: library,
            animationCount: animationCount,
            durations: (0..<animationCount).map {
                btbh_gpu_animation_library_duration(library, UInt32($0))
            }
        )
    }

    static func destroy(_ library: OpaquePointer) {
        btbh_gpu_animation_library_destroy(library)
    }
}

@MainActor
public final class GPUWindowProbe {
    private struct RadianceCascadesOcclusionStorageKey: Equatable {
        let originWorldXZ: RenderVec2
        let cellSize: Float
        let width: Int
        let height: Int
        let revision: UInt64
    }

    private struct RadianceCascadesOcclusionStorage {
        let key: RadianceCascadesOcclusionStorageKey
        let samples: UnsafeMutablePointer<Float>
        let count: Int
    }

    static var monotonicElapsedSeconds: Double {
        btbh_gpu_monotonic_elapsed_seconds()
    }

    public var deviceDiagnostics: GPUDeviceDiagnostics {
        deviceProbe.diagnostics
    }
    public private(set) var windowDiagnostics: GPUWindowDiagnostics
    public private(set) var isShutdown = false

    private let deviceProbe: GPUDeviceProbe
    private var window: OpaquePointer?
    private var nativeSceneBatches: [BTBHGPUSceneGeometryBatch] = []
    private var nativeSceneVisibility: [UInt8] = []
    private var nativeDirectionalShadowVisibility: [UInt8] = []
    private var radianceCascadesOcclusionStorage:
        [RadianceCascadesOcclusionStorage?] =
            Array(
                repeating: nil,
                count: RadianceCascadesDescriptor
                    .maximumOcclusionCascades
            )
    private var sceneCameraRightValues: [Float] = [0, 0, 0]
    private var sceneCameraUpValues: [Float] = [0, 0, 0]
    private var sceneCameraPositionValues: [Float] = [0, 0, 0]
    private var nativeFrameTelemetrySamples =
        [BTBHGPUFrameTelemetrySample](
            repeating: .init(),
            count: 64
        )

    public init(
        request: GPUBackendRequest = .platformDefault,
        title: String,
        width: UInt32,
        height: UInt32,
        hidden: Bool,
        highPixelDensity: Bool = true,
        resizable: Bool = false,
        fullscreenToggle: Bool = false,
        displayPercent: UInt8 = 0,
        debugMode: Bool
    ) throws {
        deviceProbe = try GPUDeviceProbe(
            request: request,
            debugMode: debugMode
        )
        var report = BTBHGPUWindowReport()
        var createdWindow: OpaquePointer?
        var descriptor = BTBHGPUWindowDescriptor()
        descriptor.width = width
        descriptor.height = height
        descriptor.hidden = hidden ? 1 : 0
        descriptor.high_pixel_density = highPixelDensity ? 1 : 0
        descriptor.resizable = resizable ? 1 : 0
        descriptor.fullscreen_toggle = fullscreenToggle ? 1 : 0
        descriptor.display_percent = min(displayPercent, 100)
        createdWindow = title.withCString { titlePointer in
            descriptor.title = titlePointer
            return btbh_gpu_window_create(&descriptor, &report)
        }
        guard let createdWindow else {
            deviceProbe.shutdown()
            throw Self.error(
                operation: "create",
                report: &report
            )
        }
        guard let device = deviceProbe.device,
              btbh_gpu_window_claim(
                device,
                createdWindow,
                &report
              ) == BTBH_GPU_OK else {
            btbh_gpu_window_destroy(createdWindow)
            deviceProbe.shutdown()
            throw Self.error(
                operation: "claim",
                report: &report
            )
        }
        window = createdWindow
        windowDiagnostics = GPUWindowDiagnostics(report: &report)
    }

    public func presentClear(
        red: UInt8,
        green: UInt8,
        blue: UInt8,
        alpha: UInt8
    ) throws {
        try perform("present") { window, report in
            btbh_gpu_window_present_clear(
                window,
                red,
                green,
                blue,
                alpha,
                report
            )
        }
    }

    func setFrameTelemetryEnabled(_ enabled: Bool) {
        guard let window, !isShutdown else { return }
        btbh_gpu_window_set_frame_telemetry_enabled(
            window,
            enabled ? 1 : 0
        )
    }

    func pollFrameTelemetry(
        maximumSampleCount: Int = 64
    ) -> [GPUFrameTelemetrySample] {
        var samples: [GPUFrameTelemetrySample] = []
        samples.reserveCapacity(max(0, maximumSampleCount))
        drainFrameTelemetry(
            into: &samples,
            maximumSampleCount: maximumSampleCount
        )
        return samples
    }

    func drainFrameTelemetry(
        into samples: inout [GPUFrameTelemetrySample],
        maximumSampleCount: Int = 64
    ) {
        drainFrameTelemetry(
            into: &samples,
            maximumSampleCount: maximumSampleCount,
            finish: false
        )
    }

    func finishFrameTelemetry(
        into samples: inout [GPUFrameTelemetrySample],
        maximumSampleCount: Int = 256
    ) {
        drainFrameTelemetry(
            into: &samples,
            maximumSampleCount: maximumSampleCount,
            finish: true
        )
    }

    private func drainFrameTelemetry(
        into samples: inout [GPUFrameTelemetrySample],
        maximumSampleCount: Int,
        finish: Bool
    ) {
        guard let window, !isShutdown,
              maximumSampleCount > 0 else {
            return
        }
        let capacity = min(maximumSampleCount, Int(UInt32.max))
        if nativeFrameTelemetrySamples.count < capacity {
            nativeFrameTelemetrySamples = .init(
                repeating: .init(),
                count: capacity
            )
        }
        let count = nativeFrameTelemetrySamples
            .withUnsafeMutableBufferPointer {
            samples in
            if finish {
                return Int(
                    btbh_gpu_window_finish_frame_telemetry(
                        window,
                        samples.baseAddress,
                        UInt32(capacity)
                    )
                )
            }
            return Int(btbh_gpu_window_poll_frame_telemetry(
                window,
                samples.baseAddress,
                UInt32(capacity)
            ))
        }
        for native in nativeFrameTelemetrySamples.prefix(count) {
            samples.append(GPUFrameTelemetrySample(
                frameSerial: native.frame_serial,
                cpuSubmitMilliseconds:
                    native.cpu_submit_milliseconds,
                fenceCompletionMilliseconds:
                    native.fence_completion_milliseconds
            ))
        }
    }

    public func resize(width: UInt32, height: UInt32) throws {
        try perform("resize") { window, report in
            btbh_gpu_window_resize(
                window,
                width,
                height,
                report
            )
        }
    }

    public func minimize() throws {
        try perform("minimize") { window, report in
            btbh_gpu_window_minimize(window, report)
        }
    }

    public func restore() throws {
        try perform("restore") { window, report in
            btbh_gpu_window_restore(window, report)
        }
    }

    func pollPlatform() throws -> GPUPlatformState {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "pollEvents",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var snapshot = BTBHGPUPlatformSnapshot()
        guard btbh_gpu_window_poll_events(
            window,
            &snapshot
        ) == BTBH_GPU_OK else {
            throw GPUWindowProbeError.operationFailed(
                operation: "pollEvents",
                stage: -1,
                message: "SDL GPU event polling failed"
            )
        }
        return GPUPlatformState(snapshot: snapshot)
    }

    func beginPlatformFrame() throws -> GPUPlatformState {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "beginFrame",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUWindowReport()
        var snapshot = BTBHGPUPlatformSnapshot()
        guard btbh_gpu_window_begin_frame(
            window,
            &report,
            &snapshot
        ) == BTBH_GPU_OK else {
            throw Self.error(
                operation: "beginFrame",
                report: &report
            )
        }
        windowDiagnostics = GPUWindowDiagnostics(report: &report)
        return GPUPlatformState(snapshot: snapshot)
    }

    func setMousePosition(x: Float, y: Float) throws {
        try performPlatformOperation("setMousePosition") { window in
            btbh_gpu_window_set_mouse_position(window, x, y)
        }
    }

    func setCursorVisible(_ visible: Bool) throws {
        try performPlatformOperation("setCursorVisible") { window in
            btbh_gpu_window_set_cursor_visible(
                window,
                visible ? 1 : 0
            )
        }
    }

    func setClipboardText(_ text: String) throws {
        try text.withCString { textPointer in
            try performPlatformOperation("setClipboardText") { window in
                btbh_gpu_window_set_clipboard_text(
                    window,
                    textPointer
                )
            }
        }
    }

    func showFileDialog(
        _ configuration: GPUFileDialogConfiguration,
        completion: @escaping @Sendable (
            Result<[URL], FileDialogError>
        ) -> Void
    ) {
        guard let window, !isShutdown else {
            completion(.failure(.backendFailure(
                "GPU window is not open"
            )))
            return
        }
        GPUFileDialogNativeLauncher.launch(
            window: window,
            configuration: configuration,
            completion: completion
        )
    }

    func gamepadSnapshot() -> BTBHGPUGamepadSnapshot {
        var snapshot = BTBHGPUGamepadSnapshot()
        guard let window, !isShutdown else { return snapshot }
        _ = btbh_gpu_window_gamepad_snapshot(window, &snapshot)
        return snapshot
    }

    func isKeyDown(_ key: InputKey) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_key_down(window, key.rawValue) != 0
    }

    func wasKeyPressed(_ key: InputKey) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_key_pressed(window, key.rawValue) != 0
    }

    func isMouseButtonDown(_ button: MouseButton) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_mouse_button_down(
            window,
            button.rawValue
        ) != 0
    }

    func wasMouseButtonPressed(_ button: MouseButton) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_mouse_button_pressed(
            window,
            button.rawValue
        ) != 0
    }

    func wasMouseButtonReleased(_ button: MouseButton) -> Bool {
        guard let window, !isShutdown else { return false }
        return btbh_gpu_window_mouse_button_released(
            window,
            button.rawValue
        ) != 0
    }

    func loadTexture(
        _ descriptor: TextureLoadDescriptor
    ) throws -> GPUTextureProbeResource {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "loadTexture",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        let filter: BTBHGPUTextureFilter
        switch GPUTextureSamplingProjection.filter(descriptor) {
        case .nearest:
            filter = BTBH_GPU_TEXTURE_FILTER_NEAREST
        case .bilinear:
            filter = BTBH_GPU_TEXTURE_FILTER_LINEAR
        case .trilinear:
            filter = BTBH_GPU_TEXTURE_FILTER_TRILINEAR
        }
        let wrap: BTBHGPUTextureWrap
        switch descriptor.wrap ?? .clamp {
        case .repeatMode:
            wrap = BTBH_GPU_TEXTURE_WRAP_REPEAT
        case .clamp:
            wrap = BTBH_GPU_TEXTURE_WRAP_CLAMP
        case .mirrorRepeat:
            wrap = BTBH_GPU_TEXTURE_WRAP_MIRROR
        case .mirrorClamp:
            throw GPUTextureProbeError.unsupportedWrap
        }

        var report = BTBHGPUTextureReport()
        let texture = descriptor.path.withCString { path in
            btbh_gpu_window_texture_load_ex(
                window,
                path,
                descriptor.generateMipmaps ? 1 : 0,
                filter,
                wrap,
                descriptor.colorSpace == .sRGB
                    ? BTBH_GPU_TEXTURE_COLOR_SPACE_SRGB
                    : BTBH_GPU_TEXTURE_COLOR_SPACE_LINEAR,
                &report
            )
        }
        guard let texture else {
            throw GPUTextureProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return GPUTextureProbeResource(
            pointer: texture,
            size: .init(
                width: Int(report.width),
                height: Int(report.height)
            ),
            mipCount: Int(report.mip_count),
            isRenderTarget: false
        )
    }

    static func destroyTexture(_ texture: OpaquePointer) {
        btbh_gpu_texture_destroy(texture)
    }

    func createRenderTarget(
        size: PixelSize2D,
        filter: TextureFilter
    ) throws -> GPUTextureProbeResource {
        guard let window, !isShutdown,
              size.width > 0,
              size.height > 0,
              size.width <= Int(UInt32.max),
              size.height <= Int(UInt32.max) else {
            throw GPUTextureProbeError.operationFailed(
                message: "Invalid GPU render-target descriptor"
            )
        }
        let nativeFilter: BTBHGPUTextureFilter
        switch filter {
        case .nearest:
            nativeFilter = BTBH_GPU_TEXTURE_FILTER_NEAREST
        case .bilinear:
            nativeFilter = BTBH_GPU_TEXTURE_FILTER_LINEAR
        case .trilinear:
            nativeFilter = BTBH_GPU_TEXTURE_FILTER_TRILINEAR
        }
        var report = BTBHGPUTextureReport()
        guard let texture = btbh_gpu_window_render_target_create(
            window,
            UInt32(size.width),
            UInt32(size.height),
            nativeFilter,
            &report
        ) else {
            throw GPUTextureProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            pointer: texture,
            size: size,
            mipCount: 1,
            isRenderTarget: true
        )
    }

    func loadFont(
        path: String,
        baseSize: UInt32,
        glyphs: [UInt32]
    ) throws -> GPUFontProbeResource {
        guard let window, !isShutdown, !glyphs.isEmpty else {
            throw GPUWindowProbeError.operationFailed(
                operation: "loadFont",
                stage: -1,
                message: "Invalid GPU font load request"
            )
        }
        var report = BTBHGPUFontReport()
        let font = path.withCString { pathPointer in
            glyphs.withUnsafeBufferPointer { glyphBuffer in
                btbh_gpu_window_font_load(
                    window,
                    pathPointer,
                    baseSize,
                    glyphBuffer.baseAddress,
                    UInt32(glyphs.count),
                    &report
                )
            }
        }
        guard let font, let atlas = btbh_gpu_font_atlas(font) else {
            throw GPUFontProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            pointer: font,
            atlasTexture: atlas,
            baseSize: Int(report.base_size),
            ascent: report.ascent,
            descent: report.descent,
            lineGap: report.line_gap
        )
    }

    static func fontGlyph(
        _ font: OpaquePointer,
        codepoint: UInt32
    ) throws -> GPUFontGlyphProbe {
        var glyph = BTBHGPUFontGlyph()
        guard btbh_gpu_font_glyph(
            font,
            codepoint,
            &glyph
        ) == BTBH_GPU_OK else {
            throw GPUFontProbeError.operationFailed(
                message: "Font does not contain codepoint \(codepoint)"
            )
        }
        return .init(
            advance: glyph.advance,
            offset: .init(x: glyph.offset_x, y: glyph.offset_y),
            size: .init(x: glyph.width, y: glyph.height),
            uvMinimum: .init(x: glyph.u0, y: glyph.v0),
            uvMaximum: .init(x: glyph.u1, y: glyph.v1)
        )
    }

    static func destroyFont(_ font: OpaquePointer) {
        btbh_gpu_font_destroy(font)
    }

    func loadModel(
        path: String,
        flags: ModelImportFlags = []
    ) throws -> GPUModelProbeResource {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "loadModel",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUModelReport()
        let model = path.withCString { pathPointer in
            btbh_gpu_window_model_load_ex(
                window,
                pathPointer,
                flags.rawValue,
                &report
            )
        }
        guard let model else {
            throw GPUModelProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        let meshCount = Int(report.mesh_count)
        let slots = (0..<meshCount).map {
            Int(btbh_gpu_model_material_slot(model, UInt32($0)))
        }
        let bounds = RenderBounds3(
            minimum: .init(
                x: report.minimum_x,
                y: report.minimum_y,
                z: report.minimum_z
            ),
            maximum: .init(
                x: report.maximum_x,
                y: report.maximum_y,
                z: report.maximum_z
            )
        )
        let meshes = try (0..<meshCount).map { meshIndex in
            var info = BTBHGPUModelMeshInfo()
            guard btbh_gpu_model_mesh_info(
                model,
                UInt32(meshIndex),
                &info
            ) == BTBH_GPU_OK else {
                btbh_gpu_model_destroy(model)
                throw GPUModelProbeError.operationFailed(
                    message: "Could not query imported mesh \(meshIndex)"
                )
            }

            var positionComponents = [Float](
                repeating: 0,
                count: Int(info.vertex_count) * 3
            )
            let positionResult = positionComponents
                .withUnsafeMutableBufferPointer { buffer in
                    btbh_gpu_model_mesh_copy_positions(
                        model,
                        UInt32(meshIndex),
                        buffer.baseAddress,
                        UInt32(buffer.count)
                    )
                }
            guard positionResult == BTBH_GPU_OK else {
                btbh_gpu_model_destroy(model)
                throw GPUModelProbeError.operationFailed(
                    message:
                        "Could not copy imported mesh \(meshIndex) positions"
                )
            }
            let positions = stride(
                from: 0,
                to: positionComponents.count,
                by: 3
            ).map {
                RenderVec3(
                    x: positionComponents[$0],
                    y: positionComponents[$0 + 1],
                    z: positionComponents[$0 + 2]
                )
            }

            var indices = [UInt32](
                repeating: 0,
                count: Int(info.index_count)
            )
            if !indices.isEmpty {
                let indexResult = indices.withUnsafeMutableBufferPointer {
                    buffer in
                    btbh_gpu_model_mesh_copy_indices(
                        model,
                        UInt32(meshIndex),
                        buffer.baseAddress,
                        UInt32(buffer.count)
                    )
                }
                guard indexResult == BTBH_GPU_OK else {
                    btbh_gpu_model_destroy(model)
                    throw GPUModelProbeError.operationFailed(
                        message:
                            "Could not copy imported mesh \(meshIndex) indices"
                    )
                }
            }
            return GPUModelMeshGeometry(
                positions: positions,
                indices: indices
            )
        }
        let footprint = GPUModelFootprintExtractor.extract(
            bounds: bounds,
            meshes: meshes
        )
        let materials = try (0..<Int(report.material_count)).map {
            index in
            var info = BTBHGPUModelMaterialInfo()
            guard btbh_gpu_model_material_info(
                model,
                UInt32(index),
                &info
            ) == BTBH_GPU_OK else {
                btbh_gpu_model_destroy(model)
                throw GPUModelProbeError.operationFailed(
                    message: "Could not query imported material \(index)"
                )
            }
            return GPUSceneMaterialProbe(
                albedoTexture: info.albedo_texture,
                albedoColor: .init(
                    r: info.albedo_red,
                    g: info.albedo_green,
                    b: info.albedo_blue,
                    a: info.albedo_alpha
                ),
                uvScale: .init(x: 1, y: 1),
                uvOffset: .zero,
                normalTexture: info.normal_texture,
                ormTexture: info.orm_texture,
                normalScale: info.normal_scale,
                occlusion: info.occlusion,
                roughness: info.roughness,
                metalness: info.metalness,
                emissionTexture: info.emission_texture,
                emissionColor: .init(
                    r: info.emission_red,
                    g: info.emission_green,
                    b: info.emission_blue,
                    a: info.emission_alpha
                ),
                emissionEnergy: info.emission_energy,
                alphaCutoff: info.alpha_cutoff,
                blendMode: .mix,
                billboardMode: .disabled,
                depthCompare: .less,
                stencilCompare: .always,
                stencilReference: 0,
                stencilMask: 0xFF,
                stencilFail: .keep,
                stencilDepthFail: .keep,
                stencilPass: .replace,
                cullMode: info.double_sided_enabled != 0 ? .none : .back,
                transparency:
                    info.transparency_enabled != 0 ? .alpha : .disabled,
                unlit: info.unlit_enabled != 0,
                surface: .none
            )
        }
        return GPUModelProbeResource(
            pointer: model,
            metadata: .init(
                bounds: bounds,
                meshCount: meshCount,
                materialCount: Int(report.material_count),
                meshMaterialSlots: slots,
                baseFootprint: footprint.points,
                footprintKind: footprint.kind
            ),
            materials: materials
        )
    }

    static func destroyModel(_ model: OpaquePointer) {
        btbh_gpu_model_destroy(model)
    }

    func createInstanceBuffer(
        capacity: UInt32,
        stride: UInt32
    ) throws -> GPUInstanceBufferProbeResource {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: "createInstanceBuffer",
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUInstanceBufferReport()
        guard let buffer = btbh_gpu_window_instance_buffer_create(
            window,
            capacity,
            stride,
            &report
        ) else {
            throw GPUInstanceBufferProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            pointer: buffer,
            capacity: Int(report.capacity),
            stride: Int(report.stride),
            byteCapacity: Int(report.byte_capacity)
        )
    }

    func uploadInstanceBuffer(
        _ buffer: OpaquePointer,
        bytes: UnsafeRawBufferPointer
    ) throws {
        guard let baseAddress = bytes.baseAddress,
              bytes.count > 0,
              bytes.count <= Int(UInt32.max) else {
            throw GPUInstanceBufferProbeError.operationFailed(
                message: "Invalid GPU instance buffer upload bytes"
            )
        }
        var report = BTBHGPUInstanceBufferReport()
        guard btbh_gpu_instance_buffer_upload(
            buffer,
            0,
            baseAddress,
            UInt32(bytes.count),
            &report
        ) == BTBH_GPU_OK else {
            throw GPUInstanceBufferProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
    }

    static func destroyInstanceBuffer(_ buffer: OpaquePointer) {
        btbh_gpu_instance_buffer_destroy(buffer)
    }

    func createCustomMesh(
        vertices: UnsafeRawBufferPointer,
        vertexCount: UInt32,
        indices: UnsafeBufferPointer<UInt32>?
    ) throws -> GPUCustomMeshProbeResource {
        guard let window, !isShutdown,
              let vertexBaseAddress = vertices.baseAddress,
              vertices.count > 0,
              vertices.count <= Int(UInt32.max) else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: "Invalid GPU custom mesh upload bytes"
            )
        }
        let indexCount = indices?.count ?? 0
        guard indexCount <= Int(UInt32.max) else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: "GPU custom mesh index count exceeds UInt32"
            )
        }
        var report = BTBHGPUCustomMeshReport()
        guard let mesh = btbh_gpu_window_custom_mesh_create(
            window,
            vertexBaseAddress,
            UInt32(vertices.count),
            vertexCount,
            indices?.baseAddress,
            UInt32(indexCount),
            &report
        ) else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            pointer: mesh,
            vertexCount: Int(report.vertex_count),
            indexCount: Int(report.index_count),
            vertexByteCount: Int(report.vertex_byte_count)
        )
    }

    func updateCustomMesh(
        _ mesh: OpaquePointer,
        vertices: UnsafeRawBufferPointer,
        indices: UnsafeBufferPointer<UInt32>?
    ) throws {
        guard let vertexBaseAddress = vertices.baseAddress,
              vertices.count > 0,
              vertices.count <= Int(UInt32.max),
              (indices?.count ?? 0) <= Int(UInt32.max) else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: "Invalid GPU custom mesh update bytes"
            )
        }
        var report = BTBHGPUCustomMeshReport()
        guard btbh_gpu_custom_mesh_update(
            mesh,
            vertexBaseAddress,
            UInt32(vertices.count),
            indices?.baseAddress,
            UInt32(indices?.count ?? 0),
            &report
        ) == BTBH_GPU_OK else {
            throw GPUCustomMeshProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
    }

    static func destroyCustomMesh(_ mesh: OpaquePointer) {
        btbh_gpu_custom_mesh_destroy(mesh)
    }

    func presentModelInstances(
        vertexShaderPath: String,
        animatedVertexShaderPath: String,
        fastShadowVertexShaderPath: String,
        fastShadowFragmentShaderPath: String,
        shadowVertexShaderPath: String,
        animatedShadowVertexShaderPath: String,
        shadowFragmentShaderPath: String,
        fragmentShaderPath: String,
        unshadowedFragmentShaderPath: String,
        postVertexShaderPath: String,
        postFragmentShaderPath: String,
        shaderFormat: BTBHGPUShaderFormat,
        renderSize: PixelSize2D,
        viewProjection: [Float],
        cameraRight: RenderVec3,
        cameraUp: RenderVec3,
        cameraPosition: RenderVec3,
        lighting: GPUSceneLightingProbe,
        post: GPUScenePostProbe,
        instances: [GPUInstanceRecord],
        logicalInstanceCount: Int? = nil,
        batches: [GPUSceneGeometryProbeBatch],
        clearColor: RenderColor
    ) throws -> GPUScenePresentationProbeReport {
        let transientInstanceCount = instances.count
        let resolvedLogicalInstanceCount =
            logicalInstanceCount ?? transientInstanceCount
        guard let window, !isShutdown,
              viewProjection.count == 16,
              renderSize.width > 0,
              renderSize.height > 0,
              renderSize.width <= Int(UInt32.max),
              renderSize.height <= Int(UInt32.max),
              MemoryLayout<GPUInstanceRecord>.stride ==
                GPUInstanceRecordPacking.byteStride,
              transientInstanceCount <= Int(UInt32.max),
              resolvedLogicalInstanceCount > 0,
              resolvedLogicalInstanceCount <= Int(UInt32.max),
              cameraPosition.x.isFinite,
              cameraPosition.y.isFinite,
              cameraPosition.z.isFinite,
              Self.sceneLightingIsValid(lighting),
              Self.scenePostIsValid(post),
              !batches.isEmpty,
              batches.count <= Int(UInt32.max) else {
            throw GPUSceneProbeError.operationFailed(
                message: "Invalid GPU model instance scene data"
            )
        }
        try prepareRadianceCascadesOcclusionStorage(
            post.radianceCascades.occlusionCascades
        )
        sceneCameraRightValues[0] = cameraRight.x
        sceneCameraRightValues[1] = cameraRight.y
        sceneCameraRightValues[2] = cameraRight.z
        sceneCameraUpValues[0] = cameraUp.x
        sceneCameraUpValues[1] = cameraUp.y
        sceneCameraUpValues[2] = cameraUp.z
        sceneCameraPositionValues[0] = cameraPosition.x
        sceneCameraPositionValues[1] = cameraPosition.y
        sceneCameraPositionValues[2] = cameraPosition.z
        var nativeLighting = BTBHGPUSceneLighting()
        nativeLighting.ambient_red =
            GPUColorSpace.legacyAmbientComponent(lighting.ambientColor.r)
        nativeLighting.ambient_green =
            GPUColorSpace.legacyAmbientComponent(lighting.ambientColor.g)
        nativeLighting.ambient_blue =
            GPUColorSpace.legacyAmbientComponent(lighting.ambientColor.b)
        nativeLighting.ambient_energy = lighting.ambientEnergy
        nativeLighting.directional_x = lighting.directionalDirection.x
        nativeLighting.directional_y = lighting.directionalDirection.y
        nativeLighting.directional_z = lighting.directionalDirection.z
        nativeLighting.directional_energy = lighting.directionalEnergy
        nativeLighting.directional_red =
            GPUColorSpace.linearComponent(lighting.directionalColor.r)
        nativeLighting.directional_green =
            GPUColorSpace.linearComponent(lighting.directionalColor.g)
        nativeLighting.directional_blue =
            GPUColorSpace.linearComponent(lighting.directionalColor.b)
        nativeLighting.secondary_directional_energy =
            lighting.secondaryDirectionalEnergy
        nativeLighting.secondary_directional_x =
            lighting.secondaryDirectionalDirection.x
        nativeLighting.secondary_directional_y =
            lighting.secondaryDirectionalDirection.y
        nativeLighting.secondary_directional_z =
            lighting.secondaryDirectionalDirection.z
        nativeLighting.secondary_directional_red =
            GPUColorSpace.linearComponent(
                lighting.secondaryDirectionalColor.r
            )
        nativeLighting.secondary_directional_green =
            GPUColorSpace.linearComponent(
                lighting.secondaryDirectionalColor.g
            )
        nativeLighting.secondary_directional_blue =
            GPUColorSpace.linearComponent(
                lighting.secondaryDirectionalColor.b
            )
        nativeLighting.spot_position_x = lighting.spotPosition.x
        nativeLighting.spot_position_y = lighting.spotPosition.y
        nativeLighting.spot_position_z = lighting.spotPosition.z
        nativeLighting.spot_range = lighting.spotRange
        nativeLighting.spot_direction_x = lighting.spotDirection.x
        nativeLighting.spot_direction_y = lighting.spotDirection.y
        nativeLighting.spot_direction_z = lighting.spotDirection.z
        nativeLighting.spot_energy = lighting.spotEnergy
        nativeLighting.spot_red =
            GPUColorSpace.linearComponent(lighting.spotColor.r)
        nativeLighting.spot_green =
            GPUColorSpace.linearComponent(lighting.spotColor.g)
        nativeLighting.spot_blue =
            GPUColorSpace.linearComponent(lighting.spotColor.b)
        nativeLighting.spot_falloff = lighting.spotFalloff
        nativeLighting.spot_inner_cutoff = lighting.spotInnerCutoff
        nativeLighting.spot_outer_cutoff = lighting.spotOuterCutoff
        nativeLighting.directional_shadow_enabled =
            lighting.directionalShadow.enabled ? 1 : 0
        nativeLighting.directional_shadow_softness =
            lighting.directionalShadow.softness
        nativeLighting.secondary_directional_shadow_enabled =
            lighting.secondaryDirectionalShadow.enabled ? 1 : 0
        nativeLighting.secondary_directional_shadow_softness =
            lighting.secondaryDirectionalShadow.softness
        nativeLighting.spot_shadow_enabled =
            lighting.spotShadow.enabled || lighting.heroOmniShadow.enabled
                ? 1
                : 0
        nativeLighting.spot_shadow_softness =
            lighting.heroOmniShadow.enabled
                ? lighting.heroOmniShadow.softness
                : lighting.spotShadow.softness
        nativeLighting.spot_shadow_is_omni =
            lighting.heroOmniShadow.enabled ? 1 : 0
        nativeLighting.spot_shadow_near = GPUSceneOmniShadowProbe.nearPlane
        withUnsafeMutablePointer(
            to: &nativeLighting.directional_shadow_view_projection
        ) {
            $0.withMemoryRebound(to: Float.self, capacity: 16) { values in
                for index in 0..<16 {
                    values[index] =
                        lighting.directionalShadow.viewProjection[index]
                }
            }
        }
        withUnsafeMutablePointer(
            to:
                &nativeLighting
                    .secondary_directional_shadow_view_projection
        ) {
            $0.withMemoryRebound(to: Float.self, capacity: 16) { values in
                for index in 0..<16 {
                    values[index] =
                        lighting.secondaryDirectionalShadow
                            .viewProjection[index]
                }
            }
        }
        withUnsafeMutablePointer(
            to: &nativeLighting.spot_shadow_view_projection
        ) {
            $0.withMemoryRebound(to: Float.self, capacity: 16) { values in
                for index in 0..<16 {
                    values[index] =
                        lighting.spotShadow.viewProjection[index]
                }
            }
        }
        withUnsafeMutablePointer(
            to: &nativeLighting.omni_shadow_view_projection
        ) {
            $0.withMemoryRebound(
                to: Float.self,
                capacity: GPUSceneOmniShadowProbe.faceCount * 16
            ) { values in
                for index in 0..<(GPUSceneOmniShadowProbe.faceCount * 16) {
                    values[index] =
                        lighting.heroOmniShadow.viewProjections[index]
                }
            }
        }
        if let environment = lighting.environment {
            nativeLighting.ibl_enabled = 1
            withUnsafeMutablePointer(
                to: &nativeLighting.irradiance_sh
            ) {
                $0.withMemoryRebound(
                    to: Float.self,
                    capacity: 27
                ) { values in
                    for (index, coefficient) in
                        environment.irradianceSH.enumerated()
                    {
                        values[index * 3] = coefficient.x
                        values[index * 3 + 1] = coefficient.y
                        values[index * 3 + 2] = coefficient.z
                    }
                }
            }
            Self.populateProceduralEnvironment(
                environment.sky,
                lighting: &nativeLighting
            )
        }
        guard btbh_gpu_window_scene_set_shadow_update_periods(
            window,
            lighting.directionalShadow.updatePeriodFrames,
            lighting.secondaryDirectionalShadow.updatePeriodFrames,
            lighting.heroOmniShadow.enabled
                ? lighting.heroOmniShadow.updatePeriodFrames
                : lighting.spotShadow.updatePeriodFrames
        ) != 0 else {
            throw GPUSceneProbeError.operationFailed(
                message: "Invalid GPU shadow update period"
            )
        }
        var nativePost = BTBHGPUScenePost()
        nativePost.lut_texture = post.lutTexture
        nativePost.tonemap_mode = post.tonemapMode.rawValue
        nativePost.tonemap_white = post.tonemapWhite
        nativePost.exposure = post.exposure
        nativePost.contrast = post.contrast
        nativePost.saturation = post.saturation
        nativePost.cas_sharpness = post.casSharpness
        nativePost.focus_point_x = post.focusPoint.x
        nativePost.focus_point_y = post.focusPoint.y
        nativePost.focus_direction_x = post.focusDirection.x
        nativePost.focus_direction_y = post.focusDirection.y
        nativePost.focus_half_width = post.focusHalfWidth
        nativePost.maximum_radius_pixels = post.maximumRadiusPixels
        nativePost.blur_curve = post.blurCurve
        nativePost.highlight_threshold = post.highlightThreshold
        nativePost.highlight_gain = post.highlightGain
        nativePost.lut_enabled = post.lutEnabled ? 1 : 0
        nativePost.cas_enabled = post.casEnabled ? 1 : 0
        nativePost.tilt_shift_enabled =
            post.tiltShiftEnabled ? 1 : 0
        nativePost.ssao_enabled = post.ssaoEnabled ? 1 : 0
        nativePost.anti_aliasing =
            GPUAntiAliasingProjection.native(post.antiAliasing)
        nativePost.upscale_mode =
            GPUUpscaleModeProjection.native(post.upscaleMode)
        nativePost.mirror_horizontal = post.mirrorHorizontally ? 1 : 0
        nativePost.fog_enabled = post.fog.isEnabled ? 1 : 0
        nativePost.fog_color = (
            GPUColorSpace.linearComponent(post.fog.color.r),
            GPUColorSpace.linearComponent(post.fog.color.g),
            GPUColorSpace.linearComponent(post.fog.color.b)
        )
        nativePost.fog_density = post.fog.density
        nativePost.fog_start_distance = post.fog.startDistance
        nativePost.fog_maximum_opacity = post.fog.maximumOpacity
        if let edge = post.fog.edge {
            nativePost.fog_edge_enabled = post.fog.isEnabled ? 1 : 0
            nativePost.fog_edge_bounds = (edge.minimumX, edge.minimumZ, edge.maximumX, edge.maximumZ)
            nativePost.fog_edge_fade_distance = edge.fadeDistance
            nativePost.fog_edge_clear_distance = edge.clearDistance
        } else {
            nativePost.fog_edge_enabled = 0
        }
        nativePost.bloom_enabled = post.bloom.isEnabled ? 1 : 0
        nativePost.bloom_intensity = post.bloom.intensity
        nativePost.bloom_threshold = post.bloom.threshold
        nativePost.bloom_soft_knee = post.bloom.softKnee
        nativePost.bloom_scatter = post.bloom.scatter
        nativePost.vignette_enabled = post.vignette.isEnabled ? 1 : 0
        nativePost.vignette_intensity = post.vignette.intensity
        nativePost.vignette_smoothness = post.vignette.smoothness
        nativePost.radiance_cascades.tier =
            Self.radianceCascadesTier(post.radianceCascades.tier)
        nativePost.radiance_cascades.backend =
            Self.radianceCascadesBackend(post.radianceCascades.backend)
        nativePost.radiance_cascades.debug_view =
            Self.radianceCascadesDebugView(
                post.radianceCascades.debugView
            )
        nativePost.radiance_cascades.auto_frame_budget_milliseconds =
            post.radianceCascades.autoFrameBudgetMilliseconds
        nativePost.radiance_cascades.indirect_intensity =
            post.radianceCascades.indirectIntensity
        nativePost.radiance_cascades.hero_omni_emitter_index =
            post.heroOmniLightSourceIndex
        let radianceEmitters = post.radianceCascades.emitters
        guard radianceEmitters.count <= Int(UInt32.max) else {
            throw GPUSceneProbeError.operationFailed(
                message: "Radiance emitter count exceeds UInt32"
            )
        }
        let nativeRadianceEmitters: UnsafeMutablePointer<
            BTBHGPUSceneRadianceEmitter
        >? = radianceEmitters.isEmpty
            ? nil
            : .allocate(capacity: radianceEmitters.count)
        defer { nativeRadianceEmitters?.deallocate() }
        for (index, emitter) in radianceEmitters.enumerated() {
            var nativeEmitter = BTBHGPUSceneRadianceEmitter()
            nativeEmitter.stable_id = emitter.stableID
            nativeEmitter.position_x = emitter.position.x
            nativeEmitter.position_y = emitter.position.y
            nativeEmitter.position_z = emitter.position.z
            nativeEmitter.radius = emitter.radius
            nativeEmitter.height = emitter.height
            nativeEmitter.red = GPUColorSpace.linearComponent(
                emitter.color.r
            )
            nativeEmitter.green = GPUColorSpace.linearComponent(
                emitter.color.g
            )
            nativeEmitter.blue = GPUColorSpace.linearComponent(
                emitter.color.b
            )
            nativeEmitter.energy = emitter.energy
            nativeEmitter.direct_energy = emitter.directEnergy
            nativeEmitter.direction_x = emitter.direction.x
            nativeEmitter.direction_y = emitter.direction.y
            nativeEmitter.direction_z = emitter.direction.z
            nativeEmitter.inner_cutoff = emitter.innerCutoff
            nativeEmitter.outer_cutoff = emitter.outerCutoff
            switch emitter.shape {
            case .disk:
                nativeEmitter.shape =
                    BTBH_GPU_SCENE_RADIANCE_EMITTER_DISK
            case .rectangle:
                nativeEmitter.shape =
                    BTBH_GPU_SCENE_RADIANCE_EMITTER_RECTANGLE
            case .segment:
                nativeEmitter.shape =
                    BTBH_GPU_SCENE_RADIANCE_EMITTER_SEGMENT
            }
            switch emitter.lightClass {
            case .hero:
                nativeEmitter.light_class =
                    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_HERO
            case .world:
                nativeEmitter.light_class =
                    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_WORLD
            case .pickup:
                nativeEmitter.light_class =
                    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_PICKUP
            case .transient:
                nativeEmitter.light_class =
                    BTBH_GPU_SCENE_RADIANCE_EMITTER_CLASS_TRANSIENT
            }
            nativeRadianceEmitters?[index] = nativeEmitter
        }
        nativePost.radiance_cascades.emitter_count =
            UInt32(radianceEmitters.count)
        nativePost.radiance_cascades.emitters = UnsafePointer(
            nativeRadianceEmitters
        )
        let radianceCascadesOcclusionCascades = post.radianceCascades
            .occlusionCascades
            .prefix(RadianceCascadesDescriptor.maximumOcclusionCascades)
        nativePost.radiance_cascades.occlusion_cascade_count =
            UInt32(radianceCascadesOcclusionCascades.count)
        withUnsafeMutablePointer(
            to: &nativePost.radiance_cascades.occlusion_cascades
        ) { tuple in
            tuple.withMemoryRebound(
                to: BTBHGPUSceneRadianceCascadesOcclusionCascade.self,
                capacity:
                    RadianceCascadesDescriptor.maximumOcclusionCascades
            ) { nativeCascades in
                for (index, cascade) in
                    radianceCascadesOcclusionCascades.enumerated()
                {
                    var nativeCascade =
                        BTBHGPUSceneRadianceCascadesOcclusionCascade()
                    nativeCascade.origin_world_x =
                        cascade.originWorldXZ.x
                    nativeCascade.origin_world_z =
                        cascade.originWorldXZ.y
                    nativeCascade.cell_size = cascade.cellSize
                    nativeCascade.width = UInt32(cascade.width)
                    nativeCascade.height = UInt32(cascade.height)
                    nativeCascade.revision = cascade.revision
                    nativeCascade.samples = UnsafePointer(
                        radianceCascadesOcclusionStorage[index]!.samples
                    )
                    nativeCascades[index] = nativeCascade
                }
            }
        }
        nativePost.radiance_cascades.allow_auto_transition =
            post.radianceCascades.allowAutoTransition ? 1 : 0
        nativePost.radiance_cascades.invalidate =
            post.radianceCascades.invalidate ? 1 : 0
        nativeSceneBatches.removeAll(keepingCapacity: true)
        nativeSceneVisibility.removeAll(keepingCapacity: true)
        nativeDirectionalShadowVisibility.removeAll(keepingCapacity: true)
        nativeSceneBatches.reserveCapacity(batches.count)
        nativeSceneVisibility.reserveCapacity(batches.count)
        nativeDirectionalShadowVisibility.reserveCapacity(batches.count)
        let surfaceTime = Float(Self.monotonicElapsedSeconds)
        for batch in batches {
            guard batch.firstInstance >= 0,
                  batch.firstInstance <= Int(UInt32.max),
                  batch.instanceCount > 0,
                  batch.instanceCount <= Int(UInt32.max),
                  batch.materialSlot == nil ||
                    (batch.materialSlot! >= 0 &&
                     batch.materialSlot! <= Int(Int32.max)) else {
                throw GPUSceneProbeError.operationFailed(
                    message: "GPU model instance batch exceeds UInt32"
                )
            }
            let kind: BTBHGPUSceneGeometryKind
            let model: OpaquePointer?
            let customMesh: OpaquePointer?
            switch batch.geometry {
            case .model(let pointer):
                kind = BTBH_GPU_SCENE_GEOMETRY_MODEL
                model = pointer
                customMesh = nil
            case .customMesh(let pointer):
                kind = BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH
                model = nil
                customMesh = pointer
            }
            if let animation = batch.animation {
                guard !animation.samples.isEmpty,
                      animation.samples.allSatisfy({
                          $0.animationIndex >= 0 &&
                          $0.animationIndex <= Int(UInt32.max) &&
                          $0.time.isFinite &&
                          $0.weight.isFinite &&
                          $0.weight >= 0
                      }) else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Invalid GPU animation pose"
                    )
                }
            }
            let material: GPUSceneMaterialProbe? = batch.material
            let surface: GPUSceneSurfaceProjection =
                material?.surface ?? .none
            let linearAlbedo: RenderColor = GPUColorSpace.linearized(
                material?.albedoColor ?? .white
            )
            let linearEmission: RenderColor = GPUColorSpace.linearized(
                material?.emissionColor ?? .white
            )
            let materialSlot: Int32 = Int32(batch.materialSlot ?? -1)
            let firstInstance: UInt32 = UInt32(batch.firstInstance)
            let instanceCount: UInt32 = UInt32(batch.instanceCount)
            let blendMode: BTBHGPUSceneBlendMode = Self.sceneBlendMode(
                material?.blendMode ?? .mix
            )
            let billboardMode: BTBHGPUSceneBillboardMode =
                Self.sceneBillboardMode(
                    material?.billboardMode ?? .disabled
                )
            let depthCompare: BTBHGPUSceneDepthCompare =
                Self.sceneDepthCompare(material?.depthCompare ?? .less)
            let alphaCutoff: Float =
                material?.alphaCutoff ??
                MaterialDescriptor.default.alphaCutoff
            let stencilCompare: BTBHGPUSceneDepthCompare =
                Self.sceneDepthCompare(
                    material?.stencilCompare ?? .always
                )
            let cullMode: BTBHGPUSceneCullMode = Self.sceneCullMode(
                material?.cullMode ?? .back
            )
            let stencilFail: BTBHGPUSceneStencilOperation =
                Self.sceneStencilOperation(material?.stencilFail ?? .keep)
            let stencilDepthFail: BTBHGPUSceneStencilOperation =
                Self.sceneStencilOperation(
                    material?.stencilDepthFail ?? .keep
                )
            let stencilPass: BTBHGPUSceneStencilOperation =
                Self.sceneStencilOperation(
                    material?.stencilPass ?? .replace
                )
            let transparencyEnabled: UInt8 =
                material?.transparency == .alpha ? 1 : 0
            let unlitEnabled: UInt8 =
                material?.unlit == true ||
                (material == nil &&
                 kind == BTBH_GPU_SCENE_GEOMETRY_CUSTOM_MESH)
                    ? 1 : 0
            let spotShadowVisible: UInt8 =
                GPUSceneBatchShadowCulling.castsSpotShadow(batch) ? 1 : 0
            let surfaceVariant: BTBHGPUSceneSurfaceVariant =
                Self.sceneSurfaceVariant(surface.variant)
            let surfaceParameters0: (Float, Float, Float, Float) = (
                surface.parameters0.x,
                surface.parameters0.y,
                surface.parameters0.z,
                surface.parameters0.w
            )
            let surfaceParameters1: (Float, Float, Float, Float) = (
                surface.parameters1.x,
                surface.parameters1.y,
                surface.parameters1.z,
                surface.parameters1.w
            )
            nativeSceneVisibility.append(batch.sceneVisible ? 1 : 0)
            nativeDirectionalShadowVisibility.append(
                GPUSceneBatchShadowCulling.castsDirectionalShadow(batch) ? 1 : 0
            )
            var nativeBatch = BTBHGPUSceneGeometryBatch()
            nativeBatch.kind = kind
            nativeBatch.model = model
            nativeBatch.custom_mesh = customMesh
            nativeBatch.animation_library = nil
            nativeBatch.animation_pose = batch.animation?.pointer
            nativeBatch.animation_index = 0
            nativeBatch.animation_time = 0
            nativeBatch.material_slot = materialSlot
            nativeBatch.instance_buffer = batch.instanceBuffer
            nativeBatch.first_instance = firstInstance
            nativeBatch.instance_count = instanceCount
            nativeBatch.albedo_texture = material?.albedoTexture
            nativeBatch.albedo_red = linearAlbedo.r
            nativeBatch.albedo_green = linearAlbedo.g
            nativeBatch.albedo_blue = linearAlbedo.b
            nativeBatch.albedo_alpha = linearAlbedo.a
            nativeBatch.uv_scale_x = material?.uvScale.x ?? 1
            nativeBatch.uv_scale_y = material?.uvScale.y ?? 1
            nativeBatch.uv_offset_x = material?.uvOffset.x ?? 0
            nativeBatch.uv_offset_y = material?.uvOffset.y ?? 0
            nativeBatch.normal_texture = material?.normalTexture
            nativeBatch.orm_texture = material?.ormTexture
            nativeBatch.normal_scale = material?.normalScale ?? 1
            nativeBatch.occlusion = material?.occlusion ?? 1
            nativeBatch.roughness = material?.roughness ?? 1
            nativeBatch.metalness = material?.metalness ?? 0
            nativeBatch.emission_texture = material?.emissionTexture
            nativeBatch.emission_red = linearEmission.r
            nativeBatch.emission_green = linearEmission.g
            nativeBatch.emission_blue = linearEmission.b
            nativeBatch.emission_alpha = linearEmission.a
            nativeBatch.emission_energy = material?.emissionEnergy ?? 0
            nativeBatch.blend_mode = blendMode
            nativeBatch.billboard_mode = billboardMode
            nativeBatch.depth_compare = depthCompare
            nativeBatch.alpha_cutoff = alphaCutoff
            nativeBatch.stencil_compare = stencilCompare
            nativeBatch.cull_mode = cullMode
            nativeBatch.stencil_fail = stencilFail
            nativeBatch.stencil_depth_fail = stencilDepthFail
            nativeBatch.stencil_pass = stencilPass
            nativeBatch.stencil_reference =
                material?.stencilReference ?? 0
            nativeBatch.stencil_mask = material?.stencilMask ?? 0xFF
            nativeBatch.transparency_enabled = transparencyEnabled
            nativeBatch.unlit_enabled = unlitEnabled
            nativeBatch.spot_shadow_visible = spotShadowVisible
            nativeBatch.surface_variant = surfaceVariant
            nativeBatch.surface_time = surfaceTime
            nativeBatch.surface_parameters0 = surfaceParameters0
            nativeBatch.surface_parameters1 = surfaceParameters1
            nativeBatch.detail_parameters = GPUSceneSurfaceDetailParameters.native(
                material?.detail ?? .none
            )
            nativeSceneBatches.append(nativeBatch)
        }
        var report = BTBHGPUSceneReport()
        let encodeStarted = Self.monotonicElapsedSeconds
        let result = vertexShaderPath.withCString { vertexPath in
            animatedVertexShaderPath.withCString { animatedVertexPath in
                fastShadowVertexShaderPath.withCString {
                    fastShadowVertexPath in
                    fastShadowFragmentShaderPath.withCString {
                        fastShadowFragmentPath in
                shadowVertexShaderPath.withCString { shadowVertexPath in
                    animatedShadowVertexShaderPath.withCString {
                        animatedShadowVertexPath in
                        shadowFragmentShaderPath.withCString {
                            shadowFragmentPath in
                            fragmentShaderPath.withCString {
                                fragmentPath in
                                unshadowedFragmentShaderPath.withCString {
                                    unshadowedFragmentPath in
                                postVertexShaderPath.withCString {
                                    postVertexPath in
                                postFragmentShaderPath.withCString {
                                    postFragmentPath in
                                viewProjection.withUnsafeBufferPointer {
                                    matrix in
                                    sceneCameraRightValues
                                        .withUnsafeBufferPointer { right in
                                        sceneCameraUpValues
                                            .withUnsafeBufferPointer { up in
                                            sceneCameraPositionValues
                                                .withUnsafeBufferPointer {
                                                    position in
                                            instances.withUnsafeBufferPointer {
                                                instanceData in
                                            let instanceFloats =
                                                instanceData.baseAddress.map {
                                                    UnsafeRawPointer($0)
                                                        .assumingMemoryBound(
                                                            to: Float.self
                                                        )
                                                }
                                            return nativeSceneBatches
                                                .withUnsafeBufferPointer {
                                                    batchData in
                                            nativeSceneVisibility
                                                .withUnsafeBufferPointer {
                                                    sceneVisibility in
                                            nativeDirectionalShadowVisibility
                                                .withUnsafeBufferPointer {
                                                    directionalVisibility in
                                                btbh_gpu_window_scene_present_instances_with_visibility(
                                                    window,
                                                    vertexPath,
                                                    animatedVertexPath,
                                                    fastShadowVertexPath,
                                                    fastShadowFragmentPath,
                                                    shadowVertexPath,
                                                    animatedShadowVertexPath,
                                                    shadowFragmentPath,
                                                    fragmentPath,
                                                    unshadowedFragmentPath,
                                                    postVertexPath,
                                                    postFragmentPath,
                                                    shaderFormat,
                                                    UInt32(renderSize.width),
                                                    UInt32(renderSize.height),
                                                    matrix.baseAddress,
                                                    right.baseAddress,
                                                    up.baseAddress,
                                                    position.baseAddress,
                                                    &nativeLighting,
                                                    &nativePost,
                                                    instanceFloats,
                                                    UInt32(
                                                        transientInstanceCount
                                                    ),
                                                    UInt32(
                                                        resolvedLogicalInstanceCount
                                                    ),
                                                    batchData.baseAddress,
                                                    sceneVisibility.baseAddress,
                                                    directionalVisibility
                                                        .baseAddress,
                                                    UInt32(
                                                        nativeSceneBatches.count
                                                    ),
                                                    clearColor.r,
                                                    clearColor.g,
                                                    clearColor.b,
                                                    clearColor.a,
                                                    &report
                                                )
                                            }
                                            }
                                            }
                                            }
                                        }
                                        }
                                    }
                                }
                                }
                                }
                            }
                        }
                    }
                }
            }
        }
        }
        }
        }
        let cpuEncodeMilliseconds =
            (Self.monotonicElapsedSeconds - encodeStarted) * 1_000
        guard result == BTBH_GPU_OK else {
            throw GPUSceneProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        var telemetry = BTBHGPUSceneTelemetryReport()
        guard btbh_gpu_window_scene_read_telemetry(
            window,
            &telemetry
        ) != 0 else {
            throw GPUSceneProbeError.operationFailed(
                message: "GPU scene telemetry is unavailable"
            )
        }
        return .init(
            drawableSize: .init(
                width: Int(report.drawable_width),
                height: Int(report.drawable_height)
            ),
            renderSize: renderSize,
            batchCount: Int(report.batch_count),
            primitiveDrawCount: Int(report.primitive_draw_count),
            preparedGeometryCount: Int(report.prepared_geometry_count),
            animatedGeometryCount: Int(telemetry.animated_geometry_count),
            animationSampleCount: Int(telemetry.animation_sample_count),
            boneMatrixCount: Int(telemetry.bone_matrix_count),
            instanceCount: Int(report.instance_count),
            transientInstanceCount: Int(
                report.transient_instance_count
            ),
            persistentBatchCount: Int(report.persistent_batch_count),
            shadowPipelineBindCount: Int(
                telemetry.shadow_pipeline_bind_count
            ),
            shadowPipelineBindSkipCount: Int(
                telemetry.shadow_pipeline_bind_skip_count
            ),
            geometryPipelineBindCount: Int(
                telemetry.geometry_pipeline_bind_count
            ),
            geometryPipelineBindSkipCount: Int(
                telemetry.geometry_pipeline_bind_skip_count
            ),
            shadowResourcesReady:
                btbh_gpu_window_scene_shadow_resources_ready(window) != 0,
            cpuEncodeMilliseconds: cpuEncodeMilliseconds,
            cpuPrepareMilliseconds: report.cpu_prepare_milliseconds,
            cpuPrepareSetupMilliseconds:
                telemetry.cpu_prepare_setup_milliseconds,
            cpuPrepareBonesMilliseconds:
                telemetry.cpu_prepare_bones_milliseconds,
            cpuPrepareBoneCountMilliseconds:
                telemetry.cpu_prepare_bone_count_milliseconds,
            cpuPrepareBoneAllocateMilliseconds:
                telemetry.cpu_prepare_bone_allocate_milliseconds,
            cpuPrepareBoneSampleMilliseconds:
                telemetry.cpu_prepare_bone_sample_milliseconds,
            cpuPreparePipelinesMilliseconds:
                telemetry.cpu_prepare_pipelines_milliseconds,
            cpuPrepareBuffersMilliseconds:
                telemetry.cpu_prepare_buffers_milliseconds,
            cpuUploadMilliseconds: report.cpu_upload_milliseconds,
            cpuShadowEncodeMilliseconds:
                report.cpu_shadow_encode_milliseconds,
            cpuSwapchainAcquireMilliseconds:
                report.cpu_swapchain_acquire_milliseconds,
            cpuTargetEnsureMilliseconds:
                report.cpu_target_ensure_milliseconds,
            cpuGeometryEncodeMilliseconds:
                report.cpu_geometry_encode_milliseconds,
            cpuPostEncodeMilliseconds:
                report.cpu_post_encode_milliseconds,
            radianceCascadesStatus: .init(
                requestedTier: Self.radianceCascadesTier(
                    report.radiance_cascades.requested_tier
                ),
                activeTier: Self.radianceCascadesTier(
                    report.radiance_cascades.active_tier
                ),
                pendingTier: Self.radianceCascadesTier(
                    report.radiance_cascades.pending_tier
                ),
                requestedBackend: Self.radianceCascadesBackend(
                    report.radiance_cascades.requested_backend
                ),
                activeBackend: Self.radianceCascadesBackend(
                    report.radiance_cascades.active_backend
                ),
                fallbackReason: Self.radianceCascadesFallbackReason(
                    report.radiance_cascades.fallback_reason
                ),
                internalSize: .init(
                    width: Int(report.radiance_cascades.internal_width),
                    height: Int(report.radiance_cascades.internal_height)
                ),
                generation: report.radiance_cascades.generation,
                sampleCount: Int(report.radiance_cascades.sample_count),
                cpuEncodeMilliseconds:
                    report.radiance_cascades.cpu_encode_milliseconds,
                passTelemetry: .init(
                    cpuPrepareMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_prepare_milliseconds,
                    cpuPipelinePrepareMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_pipeline_prepare_milliseconds,
                    cpuResourcePrepareMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_resource_prepare_milliseconds,
                    cpuTraceEncodeMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_trace_encode_milliseconds,
                    cpuMergeUpEncodeMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_merge_up_encode_milliseconds,
                    cpuMergeDownEncodeMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_merge_down_encode_milliseconds,
                    cpuReprojectEncodeMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_reproject_encode_milliseconds,
                    cpuFinishEncodeMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_finish_encode_milliseconds,
                    cpuBlurEncodeMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_blur_encode_milliseconds,
                    cpuResolveEncodeMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_resolve_encode_milliseconds,
                    cpuReconstructEncodeMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_reconstruct_encode_milliseconds,
                    cpuComposeEncodeMilliseconds: report.radiance_cascades
                        .pass_telemetry.cpu_compose_encode_milliseconds,
                    traceDispatchCount: Int(report.radiance_cascades
                        .pass_telemetry.trace_dispatch_count),
                    mergeUpDispatchCount: Int(report.radiance_cascades
                        .pass_telemetry.merge_up_dispatch_count),
                    mergeDownDispatchCount: Int(report.radiance_cascades
                        .pass_telemetry.merge_down_dispatch_count),
                    reprojectDispatchCount: Int(report.radiance_cascades
                        .pass_telemetry.reproject_dispatch_count),
                    finishDispatchCount: Int(report.radiance_cascades
                        .pass_telemetry.finish_dispatch_count),
                    blurDispatchCount: Int(report.radiance_cascades
                        .pass_telemetry.blur_dispatch_count),
                    resolveDispatchCount: Int(report.radiance_cascades
                        .pass_telemetry.resolve_dispatch_count),
                    reconstructDispatchCount: Int(report.radiance_cascades
                        .pass_telemetry.reconstruct_dispatch_count),
                    composeDrawCount: Int(report.radiance_cascades
                        .pass_telemetry.compose_draw_count),
                    temporalHistoryValid: report.radiance_cascades
                        .pass_telemetry.temporal_history_valid != 0,
                    temporalPhase: Int(report.radiance_cascades
                        .pass_telemetry.temporal_phase),
                    freshQuadrantCount: Int(report.radiance_cascades
                        .pass_telemetry.fresh_quadrant_count)
                )
            )
        )
    }

    private static func populateProceduralEnvironment(
        _ sky: ProceduralSkyDescriptor,
        lighting: inout BTBHGPUSceneLighting
    ) {
        lighting.ground_bottom_red =
            GPUColorSpace.linearComponent(sky.groundBottomColor.r)
        lighting.ground_bottom_green =
            GPUColorSpace.linearComponent(sky.groundBottomColor.g)
        lighting.ground_bottom_blue =
            GPUColorSpace.linearComponent(sky.groundBottomColor.b)
        lighting.ground_energy = sky.groundEnergy
        lighting.ground_horizon_red =
            GPUColorSpace.linearComponent(sky.groundHorizonColor.r)
        lighting.ground_horizon_green =
            GPUColorSpace.linearComponent(sky.groundHorizonColor.g)
        lighting.ground_horizon_blue =
            GPUColorSpace.linearComponent(sky.groundHorizonColor.b)
        lighting.ground_horizon_curve = sky.groundHorizonCurve
        lighting.sky_top_red =
            GPUColorSpace.linearComponent(sky.skyTopColor.r)
        lighting.sky_top_green =
            GPUColorSpace.linearComponent(sky.skyTopColor.g)
        lighting.sky_top_blue =
            GPUColorSpace.linearComponent(sky.skyTopColor.b)
        lighting.sky_energy = sky.skyEnergy
        lighting.sky_horizon_red =
            GPUColorSpace.linearComponent(sky.skyHorizonColor.r)
        lighting.sky_horizon_green =
            GPUColorSpace.linearComponent(sky.skyHorizonColor.g)
        lighting.sky_horizon_blue =
            GPUColorSpace.linearComponent(sky.skyHorizonColor.b)
        lighting.sky_horizon_curve = sky.skyHorizonCurve
        lighting.sun_red =
            GPUColorSpace.linearComponent(sky.sunColor.r)
        lighting.sun_green =
            GPUColorSpace.linearComponent(sky.sunColor.g)
        lighting.sun_blue =
            GPUColorSpace.linearComponent(sky.sunColor.b)
        lighting.sun_energy = sky.sunEnergy
        let sunDirection =
            (sky.sunDirection * -1).normalized()
        lighting.sun_direction_x = sunDirection.x
        lighting.sun_direction_y = sunDirection.y
        lighting.sun_direction_z = sunDirection.z
        lighting.sun_size_radians =
            sunDirection.lengthSquared > 0
                ? sky.sunSizeRadians
                : 0
        lighting.sun_curve = sky.sunCurve
    }

    private static func sceneLightingIsValid(
        _ lighting: GPUSceneLightingProbe
    ) -> Bool {
        let values = [
            lighting.ambientEnergy,
            lighting.directionalDirection.x,
            lighting.directionalDirection.y,
            lighting.directionalDirection.z,
            lighting.directionalEnergy,
            lighting.secondaryDirectionalDirection.x,
            lighting.secondaryDirectionalDirection.y,
            lighting.secondaryDirectionalDirection.z,
            lighting.secondaryDirectionalEnergy,
            lighting.spotPosition.x,
            lighting.spotPosition.y,
            lighting.spotPosition.z,
            lighting.spotDirection.x,
            lighting.spotDirection.y,
            lighting.spotDirection.z,
            lighting.spotEnergy,
            lighting.spotRange,
            lighting.spotFalloff,
            lighting.spotInnerCutoff,
            lighting.spotOuterCutoff,
        ]
        guard values.allSatisfy(\.isFinite),
              lighting.ambientEnergy >= 0,
              lighting.directionalEnergy >= 0,
              lighting.secondaryDirectionalEnergy >= 0,
              lighting.spotEnergy >= 0,
              lighting.spotRange >= 0,
              lighting.spotFalloff > 0,
              lighting.spotInnerCutoff > lighting.spotOuterCutoff else {
            return false
        }
        let shadows = [
            lighting.directionalShadow,
            lighting.secondaryDirectionalShadow,
            lighting.spotShadow,
        ]
        guard shadows.allSatisfy({
            $0.range.isFinite &&
            $0.range >= 0 &&
            $0.softness.isFinite &&
            $0.softness >= 0 &&
            $0.updatePeriodFrames >= 1 &&
            $0.updatePeriodFrames <= 60 &&
            $0.viewProjection.count == 16 &&
            $0.viewProjection.allSatisfy(\.isFinite)
        }) else {
            return false
        }
        let heroOmniShadow = lighting.heroOmniShadow
        guard heroOmniShadow.range.isFinite,
              heroOmniShadow.range >= 0,
              heroOmniShadow.softness.isFinite,
              heroOmniShadow.softness >= 0,
              heroOmniShadow.updatePeriodFrames >= 1,
              heroOmniShadow.updatePeriodFrames <= 60,
              heroOmniShadow.viewProjections.count ==
                GPUSceneOmniShadowProbe.faceCount * 16,
              heroOmniShadow.viewProjections.allSatisfy(\.isFinite) else {
            return false
        }
        guard let environment = lighting.environment else {
            return true
        }
        guard environment.irradianceSH.count == 9,
              environment.irradianceSH.allSatisfy({
                $0.x.isFinite && $0.y.isFinite && $0.z.isFinite
              }) else {
            return false
        }
        let sky = environment.sky
        return [
            sky.groundEnergy,
            sky.groundHorizonCurve,
            sky.skyEnergy,
            sky.skyHorizonCurve,
            sky.sunCurve,
            sky.sunDirection.x,
            sky.sunDirection.y,
            sky.sunDirection.z,
            sky.sunEnergy,
            sky.sunSizeRadians,
        ].allSatisfy(\.isFinite)
    }

    /// Fog, bloom and vignette ranges; the native validator checks the same bounds.
    static func hdrCompositeIsValid(
        fog: FogDescriptor,
        bloom: BloomDescriptor,
        vignette: VignetteDescriptor
    ) -> Bool {
        fog.density.isFinite && fog.density >= 0 &&
            fog.startDistance.isFinite && fog.startDistance >= 0 &&
            fog.maximumOpacity.isFinite && fog.maximumOpacity >= 0 && fog.maximumOpacity <= 1 &&
            (fog.edge.map { edge in
                [edge.minimumX, edge.minimumZ, edge.maximumX, edge.maximumZ].allSatisfy(\.isFinite) &&
                    edge.maximumX > edge.minimumX && edge.maximumZ > edge.minimumZ &&
                    edge.fadeDistance.isFinite && edge.fadeDistance > 0 &&
                    edge.clearDistance.isFinite && edge.clearDistance >= 0
            } ?? true) &&
            bloom.intensity.isFinite && bloom.intensity >= 0 &&
            bloom.threshold.isFinite && bloom.threshold >= 0 &&
            bloom.softKnee.isFinite && bloom.softKnee >= 0 && bloom.softKnee <= 1 &&
            bloom.scatter.isFinite && bloom.scatter >= 0 && bloom.scatter <= 1 &&
            vignette.intensity.isFinite && vignette.intensity >= 0 && vignette.intensity <= 1 &&
            vignette.smoothness.isFinite && vignette.smoothness > 0 && vignette.smoothness <= 1
    }

    private static func scenePostIsValid(
        _ post: GPUScenePostProbe
    ) -> Bool {
        let scalarFieldsValid = [
            post.tonemapWhite,
            post.exposure,
            post.contrast,
            post.saturation,
            post.casSharpness,
            post.focusPoint.x,
            post.focusPoint.y,
            post.focusDirection.x,
            post.focusDirection.y,
            post.focusHalfWidth,
            post.maximumRadiusPixels,
            post.blurCurve,
            post.highlightThreshold,
            post.highlightGain,
        ].allSatisfy(\.isFinite) &&
        post.tonemapWhite > 0 &&
        post.exposure >= 0 &&
        post.contrast >= 0 &&
        post.saturation >= 0 &&
        post.casSharpness >= 0 &&
        post.casSharpness <= 1 &&
        post.focusHalfWidth >= 0 &&
        post.focusHalfWidth < 1 &&
        post.maximumRadiusPixels >= 0 &&
        post.blurCurve > 0 &&
        (!post.lutEnabled || post.lutTexture != nil)
        guard scalarFieldsValid,
              Self.hdrCompositeIsValid(fog: post.fog, bloom: post.bloom, vignette: post.vignette) else {
            return false
        }
        let emittersValid = post.radianceCascades.emitters
            .allSatisfy { emitter in
                emitter.position.x.isFinite &&
                emitter.position.y.isFinite &&
                emitter.position.z.isFinite &&
                emitter.energy.isFinite && emitter.energy > 0 &&
                emitter.radius.isFinite && emitter.radius > 0 &&
                emitter.height.isFinite && emitter.height >= 0 &&
                emitter.direction.x.isFinite &&
                emitter.direction.y.isFinite &&
                emitter.direction.z.isFinite &&
                emitter.direction.lengthSquared > 0 &&
                emitter.innerCutoff.isFinite &&
                emitter.outerCutoff.isFinite &&
                emitter.innerCutoff > emitter.outerCutoff
            }
        guard emittersValid else { return false }
        guard post.heroOmniLightSourceIndex >= -1,
              post.heroOmniLightSourceIndex <
                (Int32(exactly: post.radianceCascades.emitters.count) ?? -1) ||
                post.heroOmniLightSourceIndex == -1 else {
            return false
        }
        return post.radianceCascades.occlusionCascades
            .prefix(RadianceCascadesDescriptor.maximumOcclusionCascades)
            .allSatisfy { cascade in
                cascade.originWorldXZ.x.isFinite &&
                cascade.originWorldXZ.y.isFinite &&
                cascade.cellSize.isFinite && cascade.cellSize > 0 &&
                cascade.width > 0 && cascade.width <= 2_048 &&
                cascade.height > 0 && cascade.height <= 2_048 &&
                cascade.samples.count == cascade.width * cascade.height * 2
            }
    }

    private static func radianceCascadesTier(
        _ tier: RadianceCascadesTier
    ) -> BTBHGPUSceneRadianceCascadesTier {
        switch tier {
        case .off: BTBH_GPU_SCENE_RC_OFF
        case .low: BTBH_GPU_SCENE_RC_LOW
        case .high: BTBH_GPU_SCENE_RC_HIGH
        case .auto: BTBH_GPU_SCENE_RC_AUTO
        }
    }

    private static func radianceCascadesTier(
        _ tier: BTBHGPUSceneRadianceCascadesTier
    ) -> RadianceCascadesTier {
        switch tier {
        case BTBH_GPU_SCENE_RC_LOW: .low
        case BTBH_GPU_SCENE_RC_HIGH: .high
        case BTBH_GPU_SCENE_RC_AUTO: .auto
        default: .off
        }
    }

    private static func radianceCascadesBackend(
        _ backend: RadianceCascadesBackend
    ) -> BTBHGPUSceneRadianceCascadesBackend {
        switch backend {
        case .holographic: BTBH_GPU_SCENE_RC_BACKEND_HOLOGRAPHIC
        }
    }

    private static func radianceCascadesBackend(
        _ backend: BTBHGPUSceneRadianceCascadesBackend
    ) -> RadianceCascadesBackend {
        .holographic
    }

    private static func radianceCascadesFallbackReason(
        _ reason: BTBHGPUSceneRadianceCascadesFallbackReason
    ) -> RadianceCascadesFallbackReason {
        switch reason {
        case BTBH_GPU_SCENE_RC_FALLBACK_DISABLED:
            .disabled
        case BTBH_GPU_SCENE_RC_FALLBACK_CAPABILITY_UNAVAILABLE:
            .capabilityUnavailable
        case BTBH_GPU_SCENE_RC_FALLBACK_SHADER_UNAVAILABLE:
            .shaderUnavailable
        case BTBH_GPU_SCENE_RC_FALLBACK_PIPELINE_CREATION_FAILED:
            .pipelineCreationFailed
        case BTBH_GPU_SCENE_RC_FALLBACK_RESOURCE_ALLOCATION_FAILED:
            .resourceAllocationFailed
        case BTBH_GPU_SCENE_RC_FALLBACK_DISPATCH_FAILED:
            .dispatchFailed
        case BTBH_GPU_SCENE_RC_FALLBACK_INVALID_CONFIGURATION:
            .invalidConfiguration
        default:
            .none
        }
    }

    private static func radianceCascadesDebugView(
        _ view: RadianceCascadesDebugView
    ) -> BTBHGPUSceneRadianceCascadesDebugView {
        switch view {
        case .none: BTBH_GPU_SCENE_RC_DEBUG_NONE
        case .cascade0: BTBH_GPU_SCENE_RC_DEBUG_CASCADE_0
        case .cascade1: BTBH_GPU_SCENE_RC_DEBUG_CASCADE_1
        case .cascade2: BTBH_GPU_SCENE_RC_DEBUG_CASCADE_2
        case .cascade3: BTBH_GPU_SCENE_RC_DEBUG_CASCADE_3
        case .hitDistance: BTBH_GPU_SCENE_RC_DEBUG_HIT_DISTANCE
        case .confidence: BTBH_GPU_SCENE_RC_DEBUG_CONFIDENCE
        case .finalIndirect: BTBH_GPU_SCENE_RC_DEBUG_FINAL_INDIRECT
        case .fallbackReason: BTBH_GPU_SCENE_RC_DEBUG_FALLBACK_REASON
        case .directionalSource: BTBH_GPU_SCENE_RC_DEBUG_DIRECTIONAL_SOURCE
        case .directLight: BTBH_GPU_SCENE_RC_DEBUG_DIRECT_LIGHT
        }
    }

    private static func sceneBlendMode(
        _ mode: MaterialBlendMode
    ) -> BTBHGPUSceneBlendMode {
        switch mode {
        case .mix:
            BTBH_GPU_SCENE_BLEND_ALPHA
        case .additive:
            BTBH_GPU_SCENE_BLEND_ADDITIVE
        case .multiplied:
            BTBH_GPU_SCENE_BLEND_MULTIPLIED
        case .premultipliedAlpha:
            BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA
        }
    }

    private static func sceneSurfaceVariant(
        _ variant: GPUSceneSurfaceVariant
    ) -> BTBHGPUSceneSurfaceVariant {
        switch variant {
        case .none:
            BTBH_GPU_SCENE_SURFACE_NONE
        case .glitterStatic:
            BTBH_GPU_SCENE_SURFACE_GLITTER_STATIC
        case .water:
            BTBH_GPU_SCENE_SURFACE_WATER
        case .dissolve:
            BTBH_GPU_SCENE_SURFACE_DISSOLVE
        case .shatter:
            BTBH_GPU_SCENE_SURFACE_SHATTER
        case .glitter:
            BTBH_GPU_SCENE_SURFACE_GLITTER
        }
    }

    private static func sceneBillboardMode(
        _ mode: BillboardMode
    ) -> BTBHGPUSceneBillboardMode {
        switch mode {
        case .disabled:
            BTBH_GPU_SCENE_BILLBOARD_DISABLED
        case .front:
            BTBH_GPU_SCENE_BILLBOARD_FRONT
        case .yAxis:
            BTBH_GPU_SCENE_BILLBOARD_Y_AXIS
        }
    }

    private static func sceneDepthCompare(
        _ compare: CompareFunction
    ) -> BTBHGPUSceneDepthCompare {
        switch compare {
        case .always:
            BTBH_GPU_SCENE_DEPTH_ALWAYS
        case .lessEqual:
            BTBH_GPU_SCENE_DEPTH_LESS_EQUAL
        case .greater:
            BTBH_GPU_SCENE_DEPTH_GREATER
        case .notEqual:
            BTBH_GPU_SCENE_DEPTH_NOT_EQUAL
        case .less:
            BTBH_GPU_SCENE_DEPTH_LESS
        case .equal:
            BTBH_GPU_SCENE_DEPTH_EQUAL
        case .greaterEqual:
            BTBH_GPU_SCENE_DEPTH_GREATER_EQUAL
        case .never:
            BTBH_GPU_SCENE_DEPTH_NEVER
        }
    }

    private static func sceneCullMode(
        _ mode: CullMode
    ) -> BTBHGPUSceneCullMode {
        switch mode {
        case .none:
            BTBH_GPU_SCENE_CULL_NONE
        case .front:
            BTBH_GPU_SCENE_CULL_FRONT
        case .back:
            BTBH_GPU_SCENE_CULL_BACK
        }
    }

    private static func sceneStencilOperation(
        _ operation: StencilOperation
    ) -> BTBHGPUSceneStencilOperation {
        switch operation {
        case .keep:
            BTBH_GPU_SCENE_STENCIL_KEEP
        case .zero:
            BTBH_GPU_SCENE_STENCIL_ZERO
        case .replace:
            BTBH_GPU_SCENE_STENCIL_REPLACE
        case .incrementClamp:
            BTBH_GPU_SCENE_STENCIL_INCREMENT_CLAMP
        case .decrementClamp:
            BTBH_GPU_SCENE_STENCIL_DECREMENT_CLAMP
        }
    }

    func presentCanvas(
        vertexShaderPath: String,
        fragmentShaderPath: String,
        texturedFragmentShaderPath: String,
        shaderFormat: BTBHGPUShaderFormat,
        vertices: [GPUCanvasVertexProbe],
        batches: [GPUCanvasBatchProbe],
        passes: [GPUCanvasPassProbe]
    ) throws {
        guard let window, !isShutdown,
              vertices.count <= Int(UInt32.max),
              batches.count <= Int(UInt32.max),
              !passes.isEmpty,
              passes.count <= Int(UInt32.max) else {
            throw GPUCanvasProbeError.operationFailed(
                message: "Invalid GPU canvas data"
            )
        }
        let nativeVertices = vertices.map {
            BTBHGPUCanvasVertex(
                x: $0.position.x,
                y: $0.position.y,
                u: $0.textureCoordinate.x,
                v: $0.textureCoordinate.y,
                red: Float($0.color.r) / 255,
                green: Float($0.color.g) / 255,
                blue: Float($0.color.b) / 255,
                alpha: Float($0.color.a) / 255
            )
        }
        let nativeBatches = try batches.map { batch in
            guard batch.firstVertex >= 0,
                  batch.firstVertex <= Int(UInt32.max),
                  batch.vertexCount > 0,
                  batch.vertexCount <= Int(UInt32.max) else {
                throw GPUCanvasProbeError.operationFailed(
                    message: "GPU canvas batch exceeds UInt32"
                )
            }
            let nativeBlend: BTBHGPUCanvasBlendMode
            switch batch.blendMode {
            case .alpha:
                nativeBlend = BTBH_GPU_CANVAS_BLEND_ALPHA
            case .additive:
                nativeBlend = BTBH_GPU_CANVAS_BLEND_ADDITIVE
            case .multiplied:
                nativeBlend = BTBH_GPU_CANVAS_BLEND_MULTIPLIED
            case .premultipliedAlpha:
                nativeBlend = BTBH_GPU_CANVAS_BLEND_PREMULTIPLIED_ALPHA
            }
            var native = BTBHGPUCanvasBatch()
            native.first_vertex = UInt32(batch.firstVertex)
            native.vertex_count = UInt32(batch.vertexCount)
            native.texture = batch.texture
            native.blend_mode = nativeBlend
            if let scissor = batch.scissor {
                native.scissor_x = Int32(scissor.x.rounded(.down))
                native.scissor_y = Int32(scissor.y.rounded(.down))
                native.scissor_width = Int32(
                    scissor.width.rounded(.up)
                )
                native.scissor_height = Int32(
                    scissor.height.rounded(.up)
                )
                native.scissor_enabled = 1
            }
            return native
        }
        let nativePasses = try passes.map { pass in
            guard pass.firstBatch >= 0,
                  pass.firstBatch <= Int(UInt32.max),
                  pass.batchCount >= 0,
                  pass.batchCount <= Int(UInt32.max) else {
                throw GPUCanvasProbeError.operationFailed(
                    message: "GPU canvas pass exceeds UInt32"
                )
            }
            var native = BTBHGPUCanvasPass()
            native.first_batch = UInt32(pass.firstBatch)
            native.batch_count = UInt32(pass.batchCount)
            native.target = pass.target
            if let clear = pass.clearColor {
                native.clear_enabled = 1
                native.clear_red = clear.r
                native.clear_green = clear.g
                native.clear_blue = clear.b
                native.clear_alpha = clear.a
            }
            return native
        }
        var report = BTBHGPUCanvasReport()
        let result = vertexShaderPath.withCString { vertexPath in
            fragmentShaderPath.withCString { fragmentPath in
                texturedFragmentShaderPath.withCString {
                    texturedFragmentPath in
                    nativeVertices.withUnsafeBufferPointer { vertexData in
                        nativeBatches.withUnsafeBufferPointer { batchData in
                            nativePasses.withUnsafeBufferPointer {
                                passData in
                                btbh_gpu_window_canvas_present(
                                    window,
                                    vertexPath,
                                    fragmentPath,
                                    texturedFragmentPath,
                                    shaderFormat,
                                    vertexData.baseAddress,
                                    UInt32(nativeVertices.count),
                                    batchData.baseAddress,
                                    UInt32(nativeBatches.count),
                                    passData.baseAddress,
                                    UInt32(nativePasses.count),
                                    &report
                                )
                            }
                        }
                    }
                }
            }
        }
        guard result == BTBH_GPU_OK else {
            throw GPUCanvasProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
    }

    func capturePendingSceneTexture() throws -> GPUTextureProbeResource {
        guard let window, !isShutdown else {
            throw GPUTextureProbeError.operationFailed(
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUTextureReport()
        guard let texture =
            btbh_gpu_window_capture_pending_scene_texture(
                window,
                &report
            ) else {
            throw GPUTextureProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        return .init(
            pointer: texture,
            size: .init(
                width: Int(report.width),
                height: Int(report.height)
            ),
            mipCount: Int(report.mip_count),
            isRenderTarget: false
        )
    }

    func requestPresentedFrameReadback() {
        guard let window, !isShutdown else { return }
        btbh_gpu_window_request_presented_frame_readback(window)
    }

    func readbackPresentedFrame(
        size: PixelSize2D
    ) throws -> FrameReadback {
        guard let window, !isShutdown,
              size.width > 0,
              size.height > 0,
              size.width <= Int(UInt32.max),
              size.height <= Int(UInt32.max),
              size.width <= Int.max / size.height,
              size.width * size.height <= Int.max / 4 else {
            throw GPUCanvasProbeError.operationFailed(
                message: "Invalid GPU presented-frame readback size"
            )
        }
        var bytes = [UInt8](
            repeating: 0,
            count: size.width * size.height * 4
        )
        guard bytes.count <= Int(UInt32.max) else {
            throw GPUCanvasProbeError.operationFailed(
                message: "GPU presented-frame readback exceeds UInt32"
            )
        }
        let byteCapacity = UInt32(bytes.count)
        var report = BTBHGPUCanvasReport()
        let result = bytes.withUnsafeMutableBufferPointer {
            btbh_gpu_window_readback_presented_frame(
                window,
                $0.baseAddress,
                byteCapacity,
                &report
            )
        }
        guard result == BTBH_GPU_OK else {
            throw GPUCanvasProbeError.operationFailed(
                message: GPUDeviceProbe.string(from: &report.message)
            )
        }
        let actualSize = PixelSize2D(
            width: Int(report.drawable_width),
            height: Int(report.drawable_height)
        )
        guard actualSize == size else {
            throw GPUCanvasProbeError.operationFailed(
                message: "GPU presented-frame size changed during readback"
            )
        }
        return .init(size: actualSize, rgba8: bytes)
    }

    public func shutdown() {
        if let window {
            btbh_gpu_window_destroy(window)
            self.window = nil
        }
        nativeSceneBatches = []
        nativeSceneVisibility = []
        nativeDirectionalShadowVisibility = []
        releaseRadianceCascadesOcclusionStorage()
        nativeFrameTelemetrySamples = []
        deviceProbe.shutdown()
        isShutdown = true
    }

    deinit {
        for storage in radianceCascadesOcclusionStorage.compactMap({ $0 }) {
            storage.samples.deinitialize(count: storage.count)
            storage.samples.deallocate()
        }
        if let window {
            btbh_gpu_window_destroy(window)
        }
    }

    private func prepareRadianceCascadesOcclusionStorage(
        _ cascades: [RadianceCascadesOcclusionCascade]
    ) throws {
        for (index, cascade) in cascades
            .prefix(RadianceCascadesDescriptor.maximumOcclusionCascades)
            .enumerated()
        {
            let key = RadianceCascadesOcclusionStorageKey(
                originWorldXZ: cascade.originWorldXZ,
                cellSize: cascade.cellSize,
                width: cascade.width,
                height: cascade.height,
                revision: cascade.revision
            )
            if radianceCascadesOcclusionStorage[index]?.key == key {
                continue
            }
            guard cascade.samples.allSatisfy(\.isFinite) else {
                throw GPUSceneProbeError.operationFailed(
                    message: "Radiance Cascades occlusion contains non-finite samples"
                )
            }
            let samples = UnsafeMutablePointer<Float>.allocate(
                capacity: cascade.samples.count
            )
            cascade.samples.withUnsafeBufferPointer { source in
                samples.initialize(
                    from: source.baseAddress!,
                    count: source.count
                )
            }
            if let previous = radianceCascadesOcclusionStorage[index] {
                previous.samples.deinitialize(count: previous.count)
                previous.samples.deallocate()
            }
            radianceCascadesOcclusionStorage[index] = .init(
                key: key,
                samples: samples,
                count: cascade.samples.count
            )
        }
    }

    private func releaseRadianceCascadesOcclusionStorage() {
        for index in radianceCascadesOcclusionStorage.indices {
            guard let storage = radianceCascadesOcclusionStorage[index]
            else { continue }
            storage.samples.deinitialize(count: storage.count)
            storage.samples.deallocate()
            radianceCascadesOcclusionStorage[index] = nil
        }
    }

    private func perform(
        _ operation: String,
        _ body: (
            OpaquePointer,
            UnsafeMutablePointer<BTBHGPUWindowReport>
        ) -> BTBHGPUResult
    ) throws {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: operation,
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        var report = BTBHGPUWindowReport()
        guard body(window, &report) == BTBH_GPU_OK else {
            throw Self.error(
                operation: operation,
                report: &report
            )
        }
        windowDiagnostics = GPUWindowDiagnostics(report: &report)
    }

    private func performPlatformOperation(
        _ operation: String,
        _ body: (OpaquePointer) -> BTBHGPUResult
    ) throws {
        guard let window, !isShutdown else {
            throw GPUWindowProbeError.operationFailed(
                operation: operation,
                stage: -1,
                message: "GPU window probe is shut down"
            )
        }
        guard body(window) == BTBH_GPU_OK else {
            throw GPUWindowProbeError.operationFailed(
                operation: operation,
                stage: -1,
                message: "SDL GPU platform operation failed"
            )
        }
    }

    private static func error(
        operation: String,
        report: inout BTBHGPUWindowReport
    ) -> GPUWindowProbeError {
        .operationFailed(
            operation: operation,
            stage: Int32(report.error_stage.rawValue),
            message: GPUDeviceProbe.string(from: &report.message)
        )
    }
}

struct GPUPlatformState {
    let elapsedSeconds: Double
    let deltaSeconds: Float
    let mousePosition: RenderVec2
    let mouseDelta: RenderVec2
    let mouseWheelDelta: Float
    let refreshRate: Int
    let framesPerSecond: Int
    let shouldClose: Bool
    let isWindowFocused: Bool
    let isCursorVisible: Bool

    init(snapshot: BTBHGPUPlatformSnapshot) {
        elapsedSeconds = snapshot.elapsed_seconds
        deltaSeconds = snapshot.delta_seconds
        mousePosition = .init(
            x: snapshot.mouse_x,
            y: snapshot.mouse_y
        )
        mouseDelta = .init(
            x: snapshot.mouse_delta_x,
            y: snapshot.mouse_delta_y
        )
        mouseWheelDelta = snapshot.mouse_wheel_y
        refreshRate = Int(snapshot.refresh_rate)
        framesPerSecond = Int(snapshot.frames_per_second)
        shouldClose = snapshot.should_close != 0
        isWindowFocused = snapshot.focused != 0
        isCursorVisible = snapshot.cursor_visible != 0
    }

    static let empty = Self(
        elapsedSeconds: 0,
        deltaSeconds: 0,
        mousePosition: .zero,
        mouseDelta: .zero,
        mouseWheelDelta: 0,
        refreshRate: 0,
        framesPerSecond: 0,
        shouldClose: false,
        isWindowFocused: false,
        isCursorVisible: true
    )

    func settingCursorVisible(_ visible: Bool) -> Self {
        Self(
            elapsedSeconds: elapsedSeconds,
            deltaSeconds: deltaSeconds,
            mousePosition: mousePosition,
            mouseDelta: mouseDelta,
            mouseWheelDelta: mouseWheelDelta,
            refreshRate: refreshRate,
            framesPerSecond: framesPerSecond,
            shouldClose: shouldClose,
            isWindowFocused: isWindowFocused,
            isCursorVisible: visible
        )
    }

    init(
        elapsedSeconds: Double,
        deltaSeconds: Float,
        mousePosition: RenderVec2,
        mouseDelta: RenderVec2,
        mouseWheelDelta: Float,
        refreshRate: Int,
        framesPerSecond: Int,
        shouldClose: Bool,
        isWindowFocused: Bool,
        isCursorVisible: Bool
    ) {
        self.elapsedSeconds = elapsedSeconds
        self.deltaSeconds = deltaSeconds
        self.mousePosition = mousePosition
        self.mouseDelta = mouseDelta
        self.mouseWheelDelta = mouseWheelDelta
        self.refreshRate = refreshRate
        self.framesPerSecond = framesPerSecond
        self.shouldClose = shouldClose
        self.isWindowFocused = isWindowFocused
        self.isCursorVisible = isCursorVisible
    }
}
