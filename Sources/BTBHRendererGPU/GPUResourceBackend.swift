import BTBHRenderer
import BTBHRendererGPUCore
import Foundation

public enum GPUBackendUnsupportedError: Error, Equatable {
    case operation(String)
}

enum GPUCustomMeshProjection {
    static func textureCoordinate(
        _ coordinate: RenderVec2
    ) -> RenderVec2 {
        coordinate
    }

    static func triangleIndices(
        _ indices: [UInt32]
    ) -> [UInt32] {
        var projected = indices
        for triangleStart in stride(
            from: 0,
            to: projected.count,
            by: 3
        ) {
            projected.swapAt(triangleStart + 1, triangleStart + 2)
        }
        return projected
    }
}

enum GPUSceneSurfaceVariant: UInt32, Equatable {
    case none = 0
    case glitterStatic = 1
    case water = 2
    case dissolve = 3
    case shatter = 4
    case glitter = 5
    case softParticle = 6
    case rim = 7
}

/// A shader file's name without its directory and last extension, lower-cased (`glitter_static` for
/// `Shaders/glitter_static.glsl`). Plain string work: `URL(fileURLWithPath:)` asks the file system whether the
/// path is a directory, and on Windows that opens the file. `sceneMaterial` classified every material's surface
/// shader that way on every frame, about 0.5 ms of a growth-5 Windows frame (2026-10-10).
func gpuShaderFileStem(_ path: String) -> String {
    let file = path.split(whereSeparator: { $0 == "/" || $0 == "\\" }).last.map(String.init) ?? ""
    // As URL's `deletingPathExtension`: a leading dot names the file, it does not start an extension.
    guard let dot = file.lastIndex(of: "."), dot > file.startIndex else { return file.lowercased() }
    return String(file[..<dot]).lowercased()
}

struct GPUSceneSurfaceProjection: Equatable {
    static let none = Self(
        variant: .none,
        parameters0: .init(x: 0, y: 0, z: 0, w: 0),
        parameters1: .init(x: 0, y: 0, z: 0, w: 0)
    )

    let variant: GPUSceneSurfaceVariant
    let parameters0: RenderVec4
    let parameters1: RenderVec4

    static func classify(
        fragmentPath: String,
        parameters: [String: ShaderParameterValue]
    ) throws -> Self {
        let name = gpuShaderFileStem(fragmentPath)
        func float(_ key: String, default fallback: Float) -> Float {
            guard case .float(let value)? = parameters[key] else {
                return fallback
            }
            return value
        }
        func vec3(_ key: String, default fallback: RenderVec3) -> RenderVec3 {
            guard case .vec3(let value)? = parameters[key] else {
                return fallback
            }
            return value
        }
        switch name {
        case "glitter_static":
            let color = vec3("u_color", default: .one)
            return .init(
                variant: .glitterStatic,
                parameters0: .init(
                    x: color.x,
                    y: color.y,
                    z: color.z,
                    w: 0
                ),
                parameters1: .init(x: 0, y: 0, z: 0, w: 0)
            )
        case "water_surface":
            return .init(
                variant: .water,
                parameters0: .init(x: 0, y: 0, z: 0, w: 0),
                parameters1: .init(x: 0, y: 0, z: 0, w: 0)
            )
        case "dissolve":
            return .init(
                variant: .dissolve,
                parameters0: .init(x: 0, y: 0, z: 0, w: 0),
                parameters1: .init(x: 0, y: 0, z: 0, w: 0)
            )
        case "shatter":
            return .init(
                variant: .shatter,
                parameters0: .init(
                    x: float("u_shatterDuration", default: 1.8),
                    y: float("u_cellSize", default: 0.6),
                    z: float("u_gravity", default: 6),
                    w: float("u_spreadSpeed", default: 1)
                ),
                parameters1: .init(
                    x: float("u_spinSpeed", default: 2),
                    y: 0,
                    z: 0,
                    w: 0
                )
            )
        case "rim_light":
            let color = vec3("u_color", default: .one)
            return .init(
                variant: .rim,
                parameters0: .init(
                    x: color.x,
                    y: color.y,
                    z: color.z,
                    w: float("u_strength", default: 1.2)
                ),
                parameters1: .init(x: float("u_glow", default: 0.1), y: 0, z: 0, w: 0)
            )
        case "soft_particle":
            return .init(
                variant: .softParticle,
                parameters0: .init(
                    x: float("u_fadeDistance", default: 0.5),
                    y: 0,
                    z: 0,
                    w: 0
                ),
                parameters1: .init(x: 0, y: 0, z: 0, w: 0)
            )
        case "glitter":
            let player = vec3("u_playerPos", default: .zero)
            return .init(
                variant: .glitter,
                parameters0: .init(
                    x: player.x,
                    y: player.y,
                    z: player.z,
                    w: float("u_glitterRadius", default: 1)
                ),
                parameters1: .init(
                    x: float("u_glitterSoftness", default: 0.5),
                    y: float("u_maxLevel", default: 0),
                    z: 0,
                    w: 0
                )
            )
        default:
            throw GPUBackendUnsupportedError.operation(
                "Unsupported production GPU surface shader: \(name)"
            )
        }
    }
}

enum GPUResourceStagingError: Error, Equatable {
    case missingAsset(String)
    case invalidTexture(UInt64)
    case invalidRenderTarget(UInt64)
    case invalidFont(UInt64)
    case invalidFontDescriptor(String)
    case invalidModel(UInt64)
    case invalidMesh(UInt64)
    case invalidMeshDescriptor(String)
    case invalidMeshUpload(String)
    case invalidAnimationLibrary(UInt64)
    case invalidAnimationTree(UInt64)
    case invalidAnimationTreeDescriptor(String)
    case invalidAnimationTreeParameter(String)
    case invalidMaterial(UInt64)
    case invalidMaterialDescriptor(String)
    case invalidInstanceBuffer(UInt64)
    case invalidInstanceBufferDescriptor(String)
    case invalidInstanceUpload(String)
    case invalidLight(UInt64)
    case invalidLightDescriptor(String)
    case invalidCubemap(UInt64)
    case invalidSkyDescriptor(String)
    case invalidAmbientMap(UInt64)
    case invalidSurfaceShader(UInt64)
    case invalidScreenShader(UInt64)
}

enum GPUProductionScreenShaderKind: Equatable {
    case lut
    case cas
    case tiltShift
    case unsupported(String)

    init(fragmentPath: String) {
        let name = gpuShaderFileStem(fragmentPath)
        switch name {
        case "lut":
            self = .lut
        case "cas":
            self = .cas
        case "tiltshift":
            self = .tiltShift
        default:
            self = .unsupported(name)
        }
    }
}

private struct GPUScreenShaderResource {
    let kind: GPUProductionScreenShaderKind
    var parameters: [String: ShaderParameterValue]
}

private struct GPUSurfaceShaderResource {
    let vertexPath: String?
    let fragmentPath: String
    var parameters: [String: ShaderParameterValue]
}

private struct GPUAnimationClipState {
    let animationIndex: Int
    let loop: Bool
    let duration: Float
    var speed: Float
    var time: Float
    var play: Bool
}

private struct GPUAnimationSwitchState {
    let inputs: [String]
    let crossfadeSeconds: Float
    let synchronized: Bool
    var activeInput: Int
    var previousInput: Int
    var weights: [Float]
}

private struct GPUAnimationTreeResource {
    let model: ModelHandle
    let library: AnimationLibraryHandle
    let rootNode: String
    let rootBone: Int?
    let pose: OpaquePointer
    var clips: [String: GPUAnimationClipState]
    var switches: [String: GPUAnimationSwitchState]
}

private enum GPUMaterialStorage {
    case descriptor(MaterialDescriptor)
    case modelOverride(
        model: ModelHandle,
        materialIndex: Int,
        patch: ModelMaterialPatch
    )
}

private struct GPUMaterialResource {
    var storage: GPUMaterialStorage
    let modelDependency: ModelHandle?
    let textureDependencies: Set<TextureHandle>
    let surfaceShaderDependency: SurfaceShaderHandle?
}

final class GPUInstanceBufferResource {
    let storage: GPUInstanceBufferProbeResource
    let descriptor: InstanceBufferDescriptor
    var semanticInstances: [RenderInstance] = []
    var semanticRevision: UInt64?

    init(
        storage: GPUInstanceBufferProbeResource,
        descriptor: InstanceBufferDescriptor
    ) {
        self.storage = storage
        self.descriptor = descriptor
    }
}

struct GPUInstanceRecord {
    let matrix0: SIMD4<Float>
    let matrix1: SIMD4<Float>
    let matrix2: SIMD4<Float>
    let matrix3: SIMD4<Float>
    let color: SIMD4<Float>
    let custom: SIMD4<Float>
}

enum GPUInstanceRecordPacking {
    static let floatCount = 24
    static let byteStride = floatCount * MemoryLayout<Float>.stride
    static let zeroCustom = RenderVec4(x: 0, y: 0, z: 0, w: 0)

    static func append(
        _ instance: RenderInstance,
        to instances: inout [Float]
    ) {
        append(
            matrix: instance.transform.matrix,
            color: instance.color,
            custom: instance.custom,
            to: &instances
        )
    }

    static func append(
        _ instance: RenderInstance,
        to instances: inout [GPUInstanceRecord]
    ) {
        instances.append(record(
            matrix: instance.transform.matrix,
            color: instance.color,
            custom: instance.custom
        ))
    }

    static func append(
        matrix: RenderMatrix4,
        color: RenderColor,
        custom: RenderVec4,
        to instances: inout [GPUInstanceRecord]
    ) {
        instances.append(record(
            matrix: matrix,
            color: color,
            custom: custom
        ))
    }

    static func record(
        matrix: RenderMatrix4,
        color: RenderColor,
        custom: RenderVec4
    ) -> GPUInstanceRecord {
        GPUInstanceRecord(
            matrix0: matrix.c0,
            matrix1: matrix.c1,
            matrix2: matrix.c2,
            matrix3: matrix.c3,
            color: SIMD4(
                Float(color.r) / 255,
                Float(color.g) / 255,
                Float(color.b) / 255,
                Float(color.a) / 255
            ),
            custom: SIMD4(custom.x, custom.y, custom.z, custom.w)
        )
    }

    static func appendTransformStreams(
        positions: [RenderVec3],
        rotations: [RenderQuaternion],
        scales: [RenderVec3],
        colors: [RenderColor]?,
        customs: [RenderVec4]?,
        sourceStart: Int,
        count: Int,
        to instances: inout [GPUInstanceRecord]
    ) -> Bool {
        guard sourceStart >= 0,
              count > 0,
              sourceStart <= positions.count,
              count <= positions.count - sourceStart,
              sourceStart <= rotations.count,
              count <= rotations.count - sourceStart,
              sourceStart <= scales.count,
              count <= scales.count - sourceStart,
              colors == nil || (
                sourceStart <= colors!.count &&
                count <= colors!.count - sourceStart
              ),
              customs == nil || (
                sourceStart <= customs!.count &&
                count <= customs!.count - sourceStart
              ),
              sourceStart <= Int(UInt32.max),
              count <= Int(UInt32.max) else {
            return false
        }

        let firstInstance = instances.count
        let placeholder = record(
            matrix: .identity,
            color: .white,
            custom: zeroCustom
        )
        instances.append(
            contentsOf: repeatElement(placeholder, count: count)
        )
        let succeeded = instances.withUnsafeMutableBufferPointer {
            destination in
            positions.withUnsafeBufferPointer { positionBuffer in
                rotations.withUnsafeBufferPointer { rotationBuffer in
                    scales.withUnsafeBufferPointer { scaleBuffer in
                        func pack(
                            colorBuffer: UnsafeBufferPointer<RenderColor>?,
                            customBuffer: UnsafeBufferPointer<RenderVec4>?
                        ) -> Bool {
                            guard let destinationBase =
                                    destination.baseAddress,
                                  let positionBase =
                                    positionBuffer.baseAddress,
                                  let rotationBase =
                                    rotationBuffer.baseAddress,
                                  let scaleBase =
                                    scaleBuffer.baseAddress else {
                                return false
                            }
                            let destinationFloats = UnsafeMutableRawPointer(
                                destinationBase.advanced(by: firstInstance)
                            ).assumingMemoryBound(to: Float.self)
                            return btbh_gpu_pack_transform_stream_instances(
                                destinationFloats,
                                positionBase,
                                UInt32(MemoryLayout<RenderVec3>.stride),
                                rotationBase,
                                UInt32(MemoryLayout<RenderQuaternion>.stride),
                                scaleBase,
                                UInt32(MemoryLayout<RenderVec3>.stride),
                                colorBuffer?.baseAddress,
                                UInt32(MemoryLayout<RenderColor>.stride),
                                customBuffer?.baseAddress,
                                UInt32(MemoryLayout<RenderVec4>.stride),
                                UInt32(sourceStart),
                                UInt32(count)
                            ) != 0
                        }

                        if let colors {
                            return colors.withUnsafeBufferPointer {
                                colorBuffer in
                                if let customs {
                                    return customs.withUnsafeBufferPointer {
                                        pack(
                                            colorBuffer: colorBuffer,
                                            customBuffer: $0
                                        )
                                    }
                                }
                                return pack(
                                    colorBuffer: colorBuffer,
                                    customBuffer: nil
                                )
                            }
                        }
                        if let customs {
                            return customs.withUnsafeBufferPointer {
                                pack(
                                    colorBuffer: nil,
                                    customBuffer: $0
                                )
                            }
                        }
                        return pack(
                            colorBuffer: nil,
                            customBuffer: nil
                        )
                    }
                }
            }
        }
        if !succeeded {
            instances.removeLast(count)
        }
        return succeeded
    }

    static func append(
        matrix: RenderMatrix4,
        color: RenderColor,
        custom: RenderVec4,
        to instances: inout [Float]
    ) {
        instances.append(contentsOf: [
            matrix.c0.x, matrix.c0.y, matrix.c0.z, matrix.c0.w,
            matrix.c1.x, matrix.c1.y, matrix.c1.z, matrix.c1.w,
            matrix.c2.x, matrix.c2.y, matrix.c2.z, matrix.c2.w,
            matrix.c3.x, matrix.c3.y, matrix.c3.z, matrix.c3.w,
            Float(color.r) / 255,
            Float(color.g) / 255,
            Float(color.b) / 255,
            Float(color.a) / 255,
            custom.x, custom.y, custom.z, custom.w,
        ])
    }
}

private struct GPUCustomMeshResource {
    let storage: GPUCustomMeshProbeResource
    let descriptor: MeshDescriptor
}

private struct GPUCustomMeshUpload {
    let packedVertices: [Float]
    let indices: [UInt32]
}

struct GPUProceduralEnvironmentProjection {
    let irradianceSH: [RenderVec3]
    let sky: ProceduralSkyDescriptor

    static func project(
        _ descriptor: ProceduralSkyDescriptor
    ) -> Self {
        let resolution = min(max(descriptor.resolution, 4), 64)
        var coefficients = Array(repeating: RenderVec3.zero, count: 9)
        var totalWeight: Float = 0

        for face in 0..<6 {
            for y in 0..<resolution {
                for x in 0..<resolution {
                    let u =
                        2 * (Float(x) + 0.5) / Float(resolution) - 1
                    let v =
                        2 * (Float(y) + 0.5) / Float(resolution) - 1
                    let direction = cubemapDirection(
                        face: face,
                        u: u,
                        v: v
                    ).normalized()
                    let weight =
                        1 / pow(1 + u * u + v * v, 1.5)
                    let radiance = sampleSky(
                        descriptor,
                        direction: direction
                    )
                    let basis = sphericalHarmonicBasis(direction)
                    for index in coefficients.indices {
                        coefficients[index] =
                            coefficients[index] +
                            radiance * (basis[index] * weight)
                    }
                    totalWeight += weight
                }
            }
        }

        let normalization = 4 * Float.pi / max(totalWeight, 0.000_001)
        let cosineKernel: [Float] = [
            1,
            2 / 3, 2 / 3, 2 / 3,
            1 / 4, 1 / 4, 1 / 4, 1 / 4, 1 / 4,
        ]
        for index in coefficients.indices {
            let coefficient = coefficients[index]
            let scale = normalization * cosineKernel[index]
            coefficients[index] = coefficient * scale
        }
        return .init(irradianceSH: coefficients, sky: descriptor)
    }

    func irradiance(at direction: RenderVec3) -> RenderVec3 {
        let basis = Self.sphericalHarmonicBasis(
            direction.normalized()
        )
        var result = RenderVec3.zero
        for index in irradianceSH.indices {
            result =
                result + irradianceSH[index] * basis[index]
        }
        return .init(
            x: max(result.x, 0),
            y: max(result.y, 0),
            z: max(result.z, 0)
        )
    }

    private static func cubemapDirection(
        face: Int,
        u: Float,
        v: Float
    ) -> RenderVec3 {
        switch face {
        case 0: return .init(x: 1, y: -v, z: -u)
        case 1: return .init(x: -1, y: -v, z: u)
        case 2: return .init(x: u, y: 1, z: v)
        case 3: return .init(x: u, y: -1, z: -v)
        case 4: return .init(x: u, y: -v, z: 1)
        default: return .init(x: -u, y: -v, z: -1)
        }
    }

    private static func sphericalHarmonicBasis(
        _ direction: RenderVec3
    ) -> [Float] {
        let x = direction.x
        let y = direction.y
        let z = direction.z
        return [
            0.282_094_8,
            0.488_602_52 * y,
            0.488_602_52 * z,
            0.488_602_52 * x,
            1.092_548_5 * x * y,
            1.092_548_5 * y * z,
            0.315_391_57 * (3 * z * z - 1),
            1.092_548_5 * x * z,
            0.546_274_24 * (x * x - y * y),
        ]
    }

    private static func sampleSky(
        _ descriptor: ProceduralSkyDescriptor,
        direction: RenderVec3
    ) -> RenderVec3 {
        let verticalAngle = acos(
            min(max(direction.y, -1), 1)
        )
        let color: RenderVec3
        if direction.y >= 0 {
            let c = 1 - verticalAngle / (Float.pi * 0.5)
            let gradient = min(max(
                1 - pow(
                    1 - c,
                    1 / max(descriptor.skyHorizonCurve, 0.001)
                ),
                0
            ), 1)
            color = mix(
                rgb(descriptor.skyHorizonColor),
                rgb(descriptor.skyTopColor),
                gradient
            ) * descriptor.skyEnergy
        } else {
            let c =
                (verticalAngle - Float.pi * 0.5) /
                (Float.pi * 0.5)
            let gradient = min(max(
                1 - pow(
                    1 - c,
                    1 / max(descriptor.groundHorizonCurve, 0.001)
                ),
                0
            ), 1)
            color = mix(
                rgb(descriptor.groundHorizonColor),
                rgb(descriptor.groundBottomColor),
                gradient
            ) * descriptor.groundEnergy
        }

        let sunDirection =
            (descriptor.sunDirection * -1).normalized()
        guard sunDirection.lengthSquared > 0,
              descriptor.sunSizeRadians > 0 else {
            return color
        }
        let sunAngle = acos(
            min(max(sunDirection.dot(direction), -1), 1)
        )
        let sun = rgb(descriptor.sunColor) * descriptor.sunEnergy
        if sunAngle < descriptor.sunSizeRadians {
            return sun
        }
        if sunAngle < descriptor.sunSizeRadians * 10 {
            let c =
                (sunAngle - descriptor.sunSizeRadians) /
                (descriptor.sunSizeRadians * 9)
            let fade = min(max(
                1 - pow(
                    1 - c,
                    1 / max(descriptor.sunCurve, 0.001)
                ),
                0
            ), 1)
            return mix(sun, color, fade)
        }
        return color
    }

    private static func rgb(_ color: RenderColor) -> RenderVec3 {
        .init(
            x: GPUColorSpace.linearComponent(color.r),
            y: GPUColorSpace.linearComponent(color.g),
            z: GPUColorSpace.linearComponent(color.b)
        )
    }

    private static func mix(
        _ lhs: RenderVec3,
        _ rhs: RenderVec3,
        _ amount: Float
    ) -> RenderVec3 {
        lhs + (rhs - lhs) * amount
    }
}

private struct GPUAmbientMapResource {
    let cubemap: CubemapHandle
    let environment: GPUProceduralEnvironmentProjection
}

private struct GPURenderTargetResource {
    let colorTexture: TextureHandle
}

private struct GPUSceneModelMaterialsCacheKey: Hashable {
    let model: ModelHandle
    let overrides: [MaterialOverride]
}

@MainActor
final class GPUResourceBackend: RenderResourceBackend {
    private let platform: GPUPlatform
    private let textureHandles = try? HandlePool(initialCapacity: 64)
    private let fontHandles = try? HandlePool(initialCapacity: 16)
    private let renderTargetHandles = try? HandlePool(initialCapacity: 8)
    private let modelHandles = try? HandlePool(initialCapacity: 32)
    private let meshHandles = try? HandlePool(initialCapacity: 32)
    private let materialHandles = try? HandlePool(initialCapacity: 64)
    private let instanceBufferHandles = try? HandlePool(initialCapacity: 16)
    private let lightHandles = try? HandlePool(initialCapacity: 16)
    private let cubemapHandles = try? HandlePool(initialCapacity: 4)
    private let ambientMapHandles = try? HandlePool(initialCapacity: 4)
    private let animationLibraryHandles = try? HandlePool(
        initialCapacity: 8
    )
    private let animationTreeHandles = try? HandlePool(initialCapacity: 16)
    private let surfaceShaderHandles = try? HandlePool(initialCapacity: 8)
    private let screenShaderHandles = try? HandlePool(initialCapacity: 8)
    private var textures: [TextureHandle: GPUTextureProbeResource] = [:]
    private var fonts: [FontHandle: GPUFontProbeResource] = [:]
    private var renderTargets: [
        RenderTargetHandle: GPURenderTargetResource
    ] = [:]
    private var models: [ModelHandle: GPUModelProbeResource] = [:]
    private var meshes: [MeshHandle: GPUCustomMeshResource] = [:]
    private var materials: [MaterialHandle: GPUMaterialResource] = [:]
    private var sceneModelMaterialsCache: [
        GPUSceneModelMaterialsCacheKey: [(
            materialSlot: Int?,
            material: GPUSceneMaterialProbe?
        )]
    ] = [:]
    private(set) var sceneModelMaterialResolutionCount = 0
    private var instanceBuffers: [
        InstanceBufferHandle: GPUInstanceBufferResource
    ] = [:]
    private var semanticInstanceUploadScratch: [Float] = []
    private(set) var semanticInstanceUploadCount = 0
    private var lights: [LightHandle: LightDescriptor] = [:]
    private var cubemaps: [CubemapHandle: ProceduralSkyDescriptor] = [:]
    private var ambientMaps: [
        AmbientMapHandle: GPUAmbientMapResource
    ] = [:]
    private var animationLibraries: [
        AnimationLibraryHandle: GPUAnimationLibraryProbeResource
    ] = [:]
    private var animationTrees: [
        AnimationTreeHandle: GPUAnimationTreeResource
    ] = [:]
    private var surfaceShaders: [
        SurfaceShaderHandle: GPUSurfaceShaderResource
    ] = [:]
    private var screenShaders: [
        ScreenShaderHandle: GPUScreenShaderResource
    ] = [:]
    private var pendingTextureDestruction: Set<TextureHandle> = []
    private var pendingModelDestruction: Set<ModelHandle> = []
    private var pendingCubemapDestruction: Set<CubemapHandle> = []
    private var pendingSurfaceShaderDestruction:
        Set<SurfaceShaderHandle> = []
    private var pendingAnimationLibraryDestruction:
        Set<AnimationLibraryHandle> = []

    /// Read once: per-frame updates (lights, surface shaders) trace through this flag, and
    /// reading the process environment builds a whole dictionary on every call.
    private let resourceTraceEnabled: Bool

    init(platform: GPUPlatform) {
        self.platform = platform
        resourceTraceEnabled = ProcessInfo.processInfo.environment[
            "BTBH_GPU_RESOURCE_TRACE"
        ] == "1"
    }

    func loadTexture(
        _ descriptor: TextureLoadDescriptor
    ) throws -> TextureHandle {
        trace("loadTexture")
        guard let textureHandles else {
            throw GPUTextureProbeError.operationFailed(
                message: "Could not allocate the GPU texture handle pool"
            )
        }
        let resource = try platform.loadTexture(descriptor)
        do {
            let handle = try textureHandles.acquire(TextureHandle.self)
            textures[handle] = resource
            return handle
        } catch {
            platform.destroyTexture(resource.pointer)
            throw error
        }
    }
    func createTexture(_ descriptor: TextureDescriptor, bytes: UnsafeRawBufferPointer?) throws -> TextureHandle { throw unsupported() }
    func updateTexture(_ handle: TextureHandle, bytes: UnsafeRawBufferPointer) throws { throw unsupported() }
    func destroyTexture(_ handle: TextureHandle) {
        if isTextureReferenced(handle) {
            pendingTextureDestruction.insert(handle)
            return
        }
        destroyTextureNow(handle)
    }
    private func destroyTextureNow(_ handle: TextureHandle) {
        guard
            let textureHandles,
            let resource = textures.removeValue(forKey: handle),
            textureHandles.release(handle)
        else {
            return
        }
        platform.destroyTexture(resource.pointer)
        pendingTextureDestruction.remove(handle)
    }
    func loadFont(_ descriptor: FontDescriptor) throws -> FontHandle {
        trace("loadFont")
        guard descriptor.baseSize > 0,
              descriptor.baseSize <= Int(UInt32.max) else {
            throw GPUResourceStagingError.invalidFontDescriptor(
                "Font base size must fit a positive UInt32"
            )
        }
        guard let fontHandles else {
            throw GPUResourceStagingError.invalidFontDescriptor(
                "Could not allocate the font handle pool"
            )
        }
        var glyphs = descriptor.glyphs.isEmpty
            ? Array(UInt32(32)...UInt32(255))
            : descriptor.glyphs
        if !glyphs.contains(63) {
            glyphs.append(63)
        }
        var seen = Set<UInt32>()
        glyphs = glyphs.filter {
            $0 <= 0x10ffff && seen.insert($0).inserted
        }
        guard !glyphs.isEmpty else {
            throw GPUResourceStagingError.invalidFontDescriptor(
                "Font requires at least one valid Unicode scalar"
            )
        }
        let resource = try platform.loadFont(
            path: descriptor.path,
            baseSize: UInt32(descriptor.baseSize),
            glyphs: glyphs
        )
        do {
            let handle = try fontHandles.acquire(FontHandle.self)
            fonts[handle] = resource
            return handle
        } catch {
            platform.destroyFont(resource.pointer)
            throw error
        }
    }

    func destroyFont(_ handle: FontHandle) {
        guard let fontHandles,
              let resource = fonts.removeValue(forKey: handle),
              fontHandles.release(handle) else {
            return
        }
        platform.destroyFont(resource.pointer)
    }

    func canvasTexture(
        _ handle: TextureHandle
    ) throws -> GPUTextureProbeResource {
        guard let resource = textures[handle] else {
            throw GPUResourceStagingError.invalidTexture(handle.rawValue)
        }
        return resource
    }

    func adoptTexture(
        _ resource: GPUTextureProbeResource
    ) throws -> TextureHandle {
        guard let textureHandles else {
            platform.destroyTexture(resource.pointer)
            throw GPUTextureProbeError.operationFailed(
                message: "Could not allocate the GPU texture handle pool"
            )
        }
        do {
            let handle = try textureHandles.acquire(TextureHandle.self)
            textures[handle] = resource
            return handle
        } catch {
            platform.destroyTexture(resource.pointer)
            throw error
        }
    }

    func canvasFont(
        _ handle: FontHandle
    ) throws -> GPUFontProbeResource {
        guard let resource = fonts[handle] else {
            throw GPUResourceStagingError.invalidFont(handle.rawValue)
        }
        return resource
    }

    func canvasGlyph(
        _ font: GPUFontProbeResource,
        codepoint: UInt32
    ) throws -> GPUFontGlyphProbe {
        try platform.fontGlyph(font.pointer, codepoint: codepoint)
    }

    func measureText(
        _ text: String,
        font handle: FontHandle,
        size: Float,
        spacing: Float
    ) throws -> RenderVec2 {
        guard let resource = fonts[handle] else {
            throw GPUResourceStagingError.invalidFont(handle.rawValue)
        }
        guard size > 0, size.isFinite, spacing.isFinite else {
            throw GPUResourceStagingError.invalidFontDescriptor(
                "Text size must be positive and text metrics must be finite"
            )
        }
        let scale = size / Float(resource.baseSize)
        var maximumWidth: Float = 0
        var lineWidth: Float = 0
        var glyphCount = 0
        var lineCount = 1
        for scalar in text.unicodeScalars {
            if scalar.value == 10 {
                maximumWidth = max(maximumWidth, lineWidth)
                lineWidth = 0
                glyphCount = 0
                lineCount += 1
                continue
            }
            let glyph = try platform.fontGlyph(
                resource.pointer,
                codepoint: scalar.value
            )
            if glyphCount > 0 {
                lineWidth += spacing
            }
            lineWidth += glyph.advance * scale
            glyphCount += 1
        }
        maximumWidth = max(maximumWidth, lineWidth)
        let lineAdvance = size + resource.lineGap * scale
        let height = text.isEmpty
            ? 0
            : size + Float(lineCount - 1) * lineAdvance
        return .init(x: maximumWidth, y: height)
    }

    func createRenderTarget(
        _ descriptor: RenderTargetDescriptor
    ) throws -> RenderTargetHandle {
        guard descriptor.colorFormat == .rgba8,
              !descriptor.hasDepth,
              let renderTargetHandles,
              let textureHandles else {
            throw GPUResourceStagingError.invalidRenderTarget(0)
        }
        let resource = try platform.createRenderTarget(
            size: descriptor.size,
            filter: descriptor.filter
        )
        let texture: TextureHandle
        do {
            texture = try textureHandles.acquire(TextureHandle.self)
            textures[texture] = resource
        } catch {
            platform.destroyTexture(resource.pointer)
            throw error
        }
        do {
            let target = try renderTargetHandles.acquire(
                RenderTargetHandle.self
            )
            renderTargets[target] = .init(colorTexture: texture)
            return target
        } catch {
            destroyTextureNow(texture)
            throw error
        }
    }

    func colorTexture(
        of target: RenderTargetHandle
    ) throws -> TextureHandle {
        guard let resource = renderTargets[target] else {
            throw GPUResourceStagingError.invalidRenderTarget(
                target.rawValue
            )
        }
        return resource.colorTexture
    }

    func canvasRenderTarget(
        _ target: RenderTargetHandle
    ) throws -> GPUTextureProbeResource {
        let texture = try colorTexture(of: target)
        return try canvasTexture(texture)
    }

    func destroyRenderTarget(_ handle: RenderTargetHandle) {
        guard let renderTargetHandles,
              let resource = renderTargets.removeValue(forKey: handle),
              renderTargetHandles.release(handle) else {
            return
        }
        destroyTexture(resource.colorTexture)
    }
    func loadModel(
        path: String,
        flags: ModelImportFlags
    ) throws -> ModelHandle {
        trace("loadModel path=\(path)")
        guard let modelHandles else {
            throw GPUModelProbeError.operationFailed(
                message: "Could not allocate the GPU model handle pool"
            )
        }
        let resource = try platform.loadModel(path: path, flags: flags)
        let handle: ModelHandle
        do {
            handle = try modelHandles.acquire(ModelHandle.self)
        } catch {
            platform.destroyModel(resource.pointer)
            throw error
        }
        models[handle] = resource
        trace("loadModel.complete handle=\(handle.rawValue)")
        return handle
    }

    func modelMetadata(_ handle: ModelHandle) throws -> ModelMetadata {
        trace("modelMetadata")
        guard
            !pendingModelDestruction.contains(handle),
            let resource = models[handle]
        else {
            throw GPUResourceStagingError.invalidModel(handle.rawValue)
        }
        return resource.metadata
    }

    func modelPointer(_ handle: ModelHandle) throws -> OpaquePointer {
        guard
            !pendingModelDestruction.contains(handle),
            let resource = models[handle]
        else {
            throw GPUResourceStagingError.invalidModel(handle.rawValue)
        }
        return resource.pointer
    }

    func meshPointer(_ handle: MeshHandle) throws -> OpaquePointer {
        guard let resource = meshes[handle] else {
            throw GPUResourceStagingError.invalidMesh(handle.rawValue)
        }
        return resource.storage.pointer
    }

    func materialColor(_ handle: MaterialHandle) throws -> RenderColor {
        guard let resource = materials[handle] else {
            throw GPUResourceStagingError.invalidMaterial(handle.rawValue)
        }
        switch resource.storage {
        case .descriptor(let descriptor):
            return descriptor.albedoColor
        case .modelOverride(_, _, let patch):
            return patch.albedoColor ?? .white
        }
    }

    func sceneMaterial(
        _ handle: MaterialHandle
    ) throws -> GPUSceneMaterialProbe {
        guard let resource = materials[handle] else {
            throw GPUResourceStagingError.invalidMaterial(handle.rawValue)
        }
        guard case .descriptor(let descriptor) = resource.storage else {
            throw GPUResourceStagingError.invalidMaterialDescriptor(
                "Custom GPU meshes require a standalone material descriptor"
            )
        }
        let texture = descriptor.albedoTexture.isValid
            ? try canvasTexture(descriptor.albedoTexture).pointer
            : nil
        let emissionTexture = descriptor.emissionTexture.isValid
            ? try canvasTexture(descriptor.emissionTexture).pointer
            : nil
        let normalTexture = descriptor.normalTexture.isValid
            ? try canvasTexture(descriptor.normalTexture).pointer
            : nil
        let ormTexture = descriptor.ormTexture.isValid
            ? try canvasTexture(descriptor.ormTexture).pointer
            : nil
        let surface = try sceneSurface(descriptor.surfaceShader)
        var material = GPUSceneMaterialProbe(
            albedoTexture: texture,
            albedoColor: descriptor.albedoColor,
            uvScale: descriptor.uvScale,
            uvOffset: descriptor.uvOffset,
            normalTexture: normalTexture,
            ormTexture: ormTexture,
            normalScale: descriptor.normalScale,
            occlusion: descriptor.occlusion,
            roughness: descriptor.roughness,
            metalness: descriptor.metalness,
            emissionTexture: emissionTexture,
            emissionColor: descriptor.emissionColor,
            emissionEnergy: descriptor.emissionEnergy,
            alphaCutoff: descriptor.alphaCutoff,
            blendMode: descriptor.blend,
            billboardMode: descriptor.billboard,
            depthCompare: descriptor.depthCompare,
            stencilCompare: descriptor.stencilCompare,
            stencilReference: descriptor.stencilReference,
            stencilMask: descriptor.stencilMask,
            stencilFail: descriptor.stencilFail,
            stencilDepthFail: descriptor.stencilDepthFail,
            stencilPass: descriptor.stencilPass,
            cullMode: descriptor.cullMode,
            transparency: descriptor.transparency,
            unlit: descriptor.unlit,
            surface: surface
        )
        material.legacySortKey = .init(descriptor: descriptor)
        material.castsShadow = descriptor.castsShadow
        material.detail = descriptor.detail
        return material
    }

    private func sceneSurface(
        _ handle: SurfaceShaderHandle?
    ) throws -> GPUSceneSurfaceProjection {
        guard let handle, handle.isValid else {
            return .none
        }
        guard let resource = surfaceShaders[handle] else {
            throw GPUResourceStagingError.invalidSurfaceShader(
                handle.rawValue
            )
        }
        return try .classify(
            fragmentPath: resource.fragmentPath,
            parameters: resource.parameters
        )
    }

    func sceneModelMaterials(
        model: ModelHandle,
        overrides: [MaterialOverride]
    ) throws -> [(
        materialSlot: Int?,
        material: GPUSceneMaterialProbe?
    )] {
        guard
            !pendingModelDestruction.contains(model),
            let modelResource = models[model]
        else {
            throw GPUResourceStagingError.invalidModel(model.rawValue)
        }
        let cacheKey = GPUSceneModelMaterialsCacheKey(
            model: model,
            overrides: overrides
        )
        if let cached = sceneModelMaterialsCache[cacheKey] {
            return cached
        }
        sceneModelMaterialResolutionCount += 1
        let usedSlots = Set(modelResource.metadata.meshMaterialSlots)
            .sorted()
        guard !overrides.isEmpty else {
            let resolved = usedSlots.map { slot in
                (
                    materialSlot: Optional(slot),
                    material: modelResource.materials[slot]
                )
            }
            sceneModelMaterialsCache[cacheKey] = resolved
            return resolved
        }

        var materialsBySlot: [Int: GPUSceneMaterialProbe] = [:]
        for override in overrides {
            guard
                materialsBySlot[override.slot] == nil,
                override.slot >= 0,
                override.slot < modelResource.metadata.materialCount,
                let resource = materials[override.material]
            else {
                throw GPUResourceStagingError.invalidMaterialDescriptor(
                    "GPU model override must uniquely match a model material slot"
                )
            }
            let material: GPUSceneMaterialProbe
            switch resource.storage {
            case .descriptor:
                material = try sceneMaterial(override.material)
            case .modelOverride(
                    let overrideModel,
                    let materialIndex,
                    let patch
                ):
                guard
                    overrideModel == model,
                    materialIndex == override.slot
                else {
                    throw GPUResourceStagingError.invalidMaterialDescriptor(
                        "GPU model override material belongs to another model slot"
                    )
                }
                let texture = try patch.albedoTexture.map {
                    try canvasTexture($0).pointer
                }
                let normalTexture = try patch.normalTexture.map {
                    try canvasTexture($0).pointer
                }
                let ormTexture = try patch.ormTexture.map {
                    try canvasTexture($0).pointer
                }
                let base = modelResource.materials[override.slot]
                let surface = try sceneSurface(patch.surfaceShader)
                var patched = GPUSceneMaterialProbe(
                    albedoTexture: texture ?? base.albedoTexture,
                    albedoColor:
                        patch.albedoColor ?? base.albedoColor,
                    uvScale: base.uvScale,
                    uvOffset: base.uvOffset,
                    normalTexture:
                        normalTexture ?? base.normalTexture,
                    ormTexture: ormTexture ?? base.ormTexture,
                    normalScale: base.normalScale,
                    occlusion: base.occlusion,
                    roughness: base.roughness,
                    metalness: base.metalness,
                    emissionTexture: patch.clearEmissionTexture
                        ? nil
                        : base.emissionTexture,
                    emissionColor:
                        patch.emissionColor ?? base.emissionColor,
                    emissionEnergy: max(
                        patch.emissionEnergy ?? base.emissionEnergy,
                        patch.minimumEmissionEnergy ?? -.infinity
                    ),
                    alphaCutoff: base.alphaCutoff,
                    blendMode: base.blendMode,
                    billboardMode: base.billboardMode,
                    depthCompare: patch.depthCompare ?? base.depthCompare,
                    stencilCompare: base.stencilCompare,
                    stencilReference:
                        patch.stencilReference ?? base.stencilReference,
                    stencilMask: base.stencilMask,
                    stencilFail: base.stencilFail,
                    stencilDepthFail: base.stencilDepthFail,
                    stencilPass: base.stencilPass,
                    cullMode: patch.cullMode ?? base.cullMode,
                    transparency: patch.transparency ?? base.transparency,
                    unlit: base.unlit,
                    surface: surface
                )
                patched.castsShadow = patch.castsShadow ?? base.castsShadow
                patched.detail = patch.detail ?? base.detail
                material = patched
            }
            materialsBySlot[override.slot] = material
        }

        let resolved = usedSlots.map { slot in
                (
                    materialSlot: Optional(slot),
                    material:
                        materialsBySlot[slot] ??
                        modelResource.materials[slot]
                )
            }
        sceneModelMaterialsCache[cacheKey] = resolved
        return resolved
    }

    func sceneLighting(
        environment: EnvironmentDescriptor,
        camera: RenderCamera,
        aspect: Float
    ) throws -> GPUSceneLightingProbe {
        var projectedEnvironment:
            GPUProceduralEnvironmentProjection?
        if environment.ambientMap.isValid {
            guard
                let ambient = ambientMaps[environment.ambientMap],
                cubemaps[ambient.cubemap] != nil
            else {
                throw GPUResourceStagingError.invalidAmbientMap(
                    environment.ambientMap.rawValue
                )
            }
            projectedEnvironment = ambient.environment
        }
        let sortedLights: [LightDescriptor] = lights
            .sorted { $0.key.rawValue < $1.key.rawValue }
            .map(\.value)
        let directionals: [LightDescriptor] = sortedLights
            .filter {
                $0.active &&
                $0.kind == .directional &&
                $0.energy > 0
            }
        let primary = directionals.first
        let secondary = directionals.dropFirst().first
        let spot: LightDescriptor? = sortedLights
            .first {
                $0.active &&
                ($0.kind == .spot || $0.kind == .omni) &&
                $0.energy > 0
            }
        let spotIsOmni = spot?.kind == .omni
        let directionalShadow = Self.directionalShadow(
            primary,
            camera: camera,
            aspect: aspect,
            mapSize: GPUSceneDirectionalShadowProbe.primaryMapSize
        )
        let secondaryDirectionalShadow = Self.directionalShadow(
            secondary,
            camera: camera,
            aspect: aspect,
            mapSize: GPUSceneDirectionalShadowProbe.mapSize
        )
        let spotShadow = spot?.kind == .spot
            ? Self.spotShadow(spot)
            : .disabled
        let heroOmniShadow = spotIsOmni
            ? Self.omniShadow(spot)
            : .disabled
        return GPUSceneLightingProbe(
            ambientColor: environment.ambientColor,
            ambientEnergy: environment.ambientEnergy,
            environment: projectedEnvironment,
            directionalDirection:
                primary?.direction.normalized() ?? .down,
            directionalColor: primary?.color ?? .white,
            directionalEnergy: primary?.energy ?? 0,
            secondaryDirectionalDirection:
                secondary?.direction.normalized() ?? .down,
            secondaryDirectionalColor: secondary?.color ?? .white,
            secondaryDirectionalEnergy: secondary?.energy ?? 0,
            spotPosition: spot?.position ?? .zero,
            spotDirection: spot?.direction.normalized() ?? .forward,
            spotColor: spot?.color ?? .white,
            spotEnergy: spot?.energy ?? 0,
            spotRange: spot?.range ?? 50,
            spotFalloff: 1,
            spotInnerCutoff: spotIsOmni
                ? -1
                : GPUSceneLightingProbe.legacySpotInnerCutoff,
            spotOuterCutoff: spotIsOmni
                ? -2
                : GPUSceneLightingProbe.legacySpotOuterCutoff,
            spotOccluderShadowStrength: spot?.kind == .spot && spotShadow.enabled
                ? max(0, min(1, spot?.occluderShadowStrength ?? 0))
                : 0,
            spotOccluderShadowReach: spot?.kind == .spot && spotShadow.enabled
                ? max(0, spot?.occluderShadowReach ?? 0)
                : 0,
            directionalShadow: directionalShadow,
            secondaryDirectionalShadow: secondaryDirectionalShadow,
            spotShadow: spotShadow,
            heroOmniShadow: heroOmniShadow,
            surfaceWetness: environment.surfaceWetness,
            lamps: environment.lamps,
            lampCoreRadius: environment.lampCoreRadius
        )
    }

    func scenePost(
        environment: EnvironmentDescriptor,
        chain: PostChainDescriptor,
        antiAliasing: AntiAliasing,
        upscaleMode: UpscaleMode
    ) throws -> GPUScenePostProbe {
        var lutTexture: OpaquePointer?
        var casEnabled = false
        var tiltShiftEnabled = false
        var focusPoint = RenderVec2(x: 0.5, y: 0.55)
        var focusDirection = RenderVec2(x: 1, y: 0)
        var focusHalfWidth: Float = 0.05
        var maximumRadiusPixels: Float = 12
        var blurCurve: Float = 1
        var highlightThreshold: Float = 0.7
        var highlightGain: Float = 3

        for handle in chain.screenShaders {
            guard let shader = screenShaders[handle] else {
                throw GPUResourceStagingError.invalidScreenShader(
                    handle.rawValue
                )
            }
            switch shader.kind {
            case .lut:
                if case .texture(let handle)? =
                    shader.parameters["u_texture"]
                {
                    lutTexture = try canvasTexture(handle).pointer
                }
            case .cas:
                casEnabled = true
            case .tiltShift:
                tiltShiftEnabled = chain.tiltShiftEnabled
                if case .vec2(let value)? =
                    shader.parameters["uFocusPoint"]
                {
                    focusPoint = value
                }
                if case .vec2(let value)? =
                    shader.parameters["uFocusDir"]
                {
                    focusDirection = value
                }
                if case .float(let value)? =
                    shader.parameters["uFocusHalfWidth"]
                {
                    focusHalfWidth = value
                }
                if case .float(let value)? =
                    shader.parameters["uMaxRadiusPx"]
                {
                    maximumRadiusPixels = value
                }
                if case .float(let value)? =
                    shader.parameters["uBlurCurve"]
                {
                    blurCurve = value
                }
                if case .float(let value)? =
                    shader.parameters["uHighlightThreshold"]
                {
                    highlightThreshold = value
                }
                if case .float(let value)? =
                    shader.parameters["uHighlightGain"]
                {
                    highlightGain = value
                }
            case .unsupported(let name):
                throw GPUBackendUnsupportedError.operation(
                    "Unsupported production GPU screen shader: \(name)"
                )
            }
        }
        let radianceCascades = environment.radianceCascades
        let sortedLights = lights
            .sorted { $0.key.rawValue < $1.key.rawValue }
        let localSlot = sortedLights.first {
            $0.value.active &&
            ($0.value.kind == .spot || $0.value.kind == .omni) &&
            $0.value.energy > 0
        }
        let heroOmniHandle: LightHandle? = {
            guard let localSlot,
                  localSlot.value.kind == .omni,
                  localSlot.value.castsShadow,
                  localSlot.value.range > GPUSceneOmniShadowProbe.nearPlane else {
                return nil
            }
            return localSlot.key
        }()
        var heroOmniLightSourceIndex: Int32 = -1
        var indirectEmitters: [RadianceEmitter] = []
        indirectEmitters.reserveCapacity(sortedLights.count)
        for (handle, light) in sortedLights {
            guard light.active,
                  light.kind != .directional,
                  light.indirectEnergy > 0,
                  light.range > 0 else {
                continue
            }
            if handle == heroOmniHandle {
                heroOmniLightSourceIndex =
                    Int32(exactly: indirectEmitters.count) ?? -1
            }
            let emitterRadius = light.kind == .spot
                ? max(light.range * 0.01, 0.25)
                : max(
                    light.shadowSoftness,
                    light.range * 0.02,
                    0.05
                )
            indirectEmitters.append(RadianceEmitter(
                stableID: handle.rawValue,
                position: light.position,
                color: light.color,
                energy: light.indirectEnergy,
                // The RC-native direct field stores radiance, while the
                // public light energy is an engine-space illuminance scale.
                // Convert once here instead of hiding a gain in the shader.
                directEnergy: light.kind == .spot ? light.energy * 1.50 : 0,
                radius: emitterRadius,
                height: light.kind == .spot
                    ? light.range * 0.65
                    : emitterRadius * 2,
                lightClass: handle == localSlot?.key ? .hero : .world,
                direction: light.direction.normalized(),
                innerCutoff: light.kind == .spot
                    ? GPUSceneLightingProbe.radianceSpotInnerCutoff
                    : -1,
                outerCutoff: light.kind == .spot
                    ? GPUSceneLightingProbe.radianceSpotOuterCutoff
                    : -2
            ))
        }
        let mergedEmitters = RadianceEmitterBudgetPolicy.production.applying(
            to: indirectEmitters + radianceCascades.emitters
        )
        let mergedRadianceCascades = RadianceCascadesDescriptor(
            tier: radianceCascades.tier,
            backend: radianceCascades.backend,
            debugView: radianceCascades.debugView,
            autoFrameBudgetMilliseconds:
                radianceCascades.autoFrameBudgetMilliseconds,
            indirectIntensity: radianceCascades.indirectIntensity,
            emitters: mergedEmitters,
            occlusionCascades: radianceCascades.occlusionCascades,
            allowAutoTransition: radianceCascades.allowAutoTransition,
            invalidate: radianceCascades.invalidate
        )
        return .init(
            lutTexture: lutTexture,
            tonemapMode: environment.tonemap,
            tonemapWhite: environment.tonemapWhite,
            exposure: environment.exposure,
            contrast: environment.contrast,
            saturation: environment.saturation,
            casSharpness: chain.casSharpness,
            focusPoint: focusPoint,
            focusDirection: focusDirection,
            focusHalfWidth: focusHalfWidth,
            maximumRadiusPixels: maximumRadiusPixels,
            blurCurve: blurCurve,
            highlightThreshold: highlightThreshold,
            highlightGain: highlightGain,
            lutEnabled: lutTexture != nil,
            casEnabled: casEnabled,
            tiltShiftEnabled: tiltShiftEnabled,
            ssaoEnabled: environment.ssaoEnabled,
            antiAliasing: antiAliasing,
            upscaleMode: upscaleMode,
            radianceCascades: mergedRadianceCascades,
            heroOmniLightSourceIndex: heroOmniLightSourceIndex,
            mirrorHorizontally: chain.mirrorHorizontally,
            fog: environment.fog,
            bloom: chain.bloom,
            vignette: chain.vignette
        )
    }

    private static func directionalShadow(
        _ descriptor: LightDescriptor?,
        camera: RenderCamera,
        aspect: Float,
        mapSize: Float
    ) -> GPUSceneDirectionalShadowProbe {
        guard let descriptor else { return .disabled }
        let disabled = GPUSceneDirectionalShadowProbe(
            enabled: false,
            range: descriptor.range,
            softness: descriptor.shadowSoftness,
            updatePeriodFrames: descriptor.shadowUpdatePeriodFrames,
            viewProjection:
                GPUSceneDirectionalShadowProbe.disabled.viewProjection
        )
        let direction = descriptor.direction.normalized()
        let cameraForward = (camera.target - camera.position).normalized()
        guard descriptor.castsShadow,
              descriptor.energy > 0,
              direction.lengthSquared > 0,
              cameraForward.lengthSquared > 0 else {
            return disabled
        }

        let radius: Float
        let frustumCenter: RenderVec3
        if let focus = descriptor.shadowFocus {
            guard focus.radius.isFinite,
                  focus.radius > 0,
                  focus.forwardShift.isFinite else {
                return disabled
            }
            let flatForward =
                cameraForward - RenderVec3.up * cameraForward.dot(.up)
            let ahead = flatForward.lengthSquared > 0.000_001
                ? flatForward.normalized()
                : RenderVec3.zero
            radius = focus.radius
            frustumCenter = camera.target + ahead * focus.forwardShift
        } else {
            guard descriptor.range > 0,
                  aspect.isFinite,
                  aspect > 0,
                  camera.verticalFOVDegrees.isFinite,
                  camera.verticalFOVDegrees > 0,
                  camera.verticalFOVDegrees < 180 else {
                return disabled
            }
            let shadowNear = descriptor.range / 1_000
            let shadowFar = descriptor.range
            let farHeight = shadowFar * tan(
                camera.verticalFOVDegrees * .pi / 360
            )
            let halfDepth = (shadowFar - shadowNear) * 0.5
            radius = sqrt(
                farHeight * farHeight * (1 + aspect * aspect) +
                halfDepth * halfDepth
            )
            frustumCenter =
                camera.position +
                cameraForward * ((shadowNear + shadowFar) * 0.5)
        }
        guard radius.isFinite, radius > 0 else { return disabled }

        let absolute = RenderVec3(
            x: abs(direction.x),
            y: abs(direction.y),
            z: abs(direction.z)
        )
        let leastAligned: RenderVec3
        if absolute.x <= absolute.y && absolute.x <= absolute.z {
            leastAligned = .right
        } else if absolute.y <= absolute.z {
            leastAligned = .up
        } else {
            leastAligned = .forward
        }
        let lightRight = direction.cross(leastAligned).normalized()
        let lightUp = lightRight.cross(direction).normalized()
        guard lightRight.lengthSquared > 0,
              lightUp.lengthSquared > 0 else {
            return disabled
        }

        let texelSize = radius * 2 / mapSize
        let snappedX =
            floor(frustumCenter.dot(lightRight) / texelSize) * texelSize
        let snappedY =
            floor(frustumCenter.dot(lightUp) / texelSize) * texelSize
        let snappedZ = frustumCenter.dot(direction)
        let center =
            lightRight * snappedX +
            lightUp * snappedY +
            direction * snappedZ
        let depthExtension: Float = 100
        let eye =
            center - direction * (radius + depthExtension)
        let view: [Float] = [
            lightRight.x, lightUp.x, direction.x, 0,
            lightRight.y, lightUp.y, direction.y, 0,
            lightRight.z, lightUp.z, direction.z, 0,
            -eye.dot(lightRight),
            -eye.dot(lightUp),
            -eye.dot(direction),
            1,
        ]
        let depth = depthExtension + radius * 2
        let projection: [Float] = [
            1 / radius, 0, 0, 0,
            0, 1 / radius, 0, 0,
            0, 0, 1 / depth, 0,
            0, 0, 0, 1,
        ]
        let viewProjection = multiplyShadowMatrix(projection, view)
        guard viewProjection.allSatisfy(\.isFinite) else {
            return disabled
        }
        return .init(
            enabled: true,
            range: descriptor.range,
            softness: descriptor.shadowSoftness,
            updatePeriodFrames: descriptor.shadowUpdatePeriodFrames,
            viewProjection: viewProjection
        )
    }

    private static func spotShadow(
        _ descriptor: LightDescriptor?
    ) -> GPUSceneDirectionalShadowProbe {
        guard let descriptor else { return .disabled }
        let disabled = GPUSceneDirectionalShadowProbe(
            enabled: false,
            range: descriptor.range,
            softness: descriptor.shadowSoftness,
            updatePeriodFrames: descriptor.shadowUpdatePeriodFrames,
            viewProjection:
                GPUSceneDirectionalShadowProbe.disabled.viewProjection
        )
        let direction = descriptor.direction.normalized()
        guard descriptor.castsShadow,
              descriptor.energy > 0,
              descriptor.range > 0.05,
              direction.lengthSquared > 0 else {
            return disabled
        }

        var authoredUp = RenderVec3.up
        if abs(direction.dot(authoredUp)) > 0.99 {
            authoredUp = .right
        }
        let right = direction.cross(authoredUp).normalized()
        let up = right.cross(direction).normalized()
        guard right.lengthSquared > 0, up.lengthSquared > 0 else {
            return disabled
        }
        let position = descriptor.position
        let view: [Float] = [
            right.x, up.x, direction.x, 0,
            right.y, up.y, direction.y, 0,
            right.z, up.z, direction.z, 0,
            -position.dot(right),
            -position.dot(up),
            -position.dot(direction),
            1,
        ]
        let near: Float = 0.05
        let depthScale = descriptor.range / (descriptor.range - near)
        let depthBias = -near * depthScale
        let projection: [Float] = [
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, depthScale, 1,
            0, 0, depthBias, 0,
        ]
        let viewProjection = multiplyShadowMatrix(projection, view)
        guard viewProjection.allSatisfy(\.isFinite) else {
            return disabled
        }
        return .init(
            enabled: true,
            range: descriptor.range,
            softness: descriptor.shadowSoftness,
            updatePeriodFrames: descriptor.shadowUpdatePeriodFrames,
            viewProjection: viewProjection
        )
    }

    private static func omniShadow(
        _ descriptor: LightDescriptor?
    ) -> GPUSceneOmniShadowProbe {
        guard let descriptor else { return .disabled }
        let disabled = GPUSceneOmniShadowProbe(
            enabled: false,
            range: descriptor.range,
            softness: descriptor.shadowSoftness,
            updatePeriodFrames: descriptor.shadowUpdatePeriodFrames,
            viewProjections: GPUSceneOmniShadowProbe.disabled.viewProjections
        )
        guard descriptor.castsShadow,
              descriptor.energy > 0,
              descriptor.range > GPUSceneOmniShadowProbe.nearPlane else {
            return disabled
        }

        let directions: [RenderVec3] = [
            .init(x: 1, y: 0, z: 0),
            .init(x: -1, y: 0, z: 0),
            .init(x: 0, y: 1, z: 0),
            .init(x: 0, y: -1, z: 0),
            .init(x: 0, y: 0, z: 1),
            .init(x: 0, y: 0, z: -1),
        ]
        let authoredUps: [RenderVec3] = [
            .init(x: 0, y: -1, z: 0),
            .init(x: 0, y: -1, z: 0),
            .init(x: 0, y: 0, z: 1),
            .init(x: 0, y: 0, z: -1),
            .init(x: 0, y: -1, z: 0),
            .init(x: 0, y: -1, z: 0),
        ]
        let near = GPUSceneOmniShadowProbe.nearPlane
        let depthScale = descriptor.range / (descriptor.range - near)
        let depthBias = -near * depthScale
        let projection: [Float] = [
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, depthScale, 1,
            0, 0, depthBias, 0,
        ]
        var viewProjections: [Float] = []
        viewProjections.reserveCapacity(
            GPUSceneOmniShadowProbe.faceCount * 16
        )
        for (direction, authoredUp) in zip(directions, authoredUps) {
            let right = direction.cross(authoredUp).normalized()
            let up = right.cross(direction).normalized()
            guard right.lengthSquared > 0, up.lengthSquared > 0 else {
                return disabled
            }
            let position = descriptor.position
            let view: [Float] = [
                right.x, up.x, direction.x, 0,
                right.y, up.y, direction.y, 0,
                right.z, up.z, direction.z, 0,
                -position.dot(right),
                -position.dot(up),
                -position.dot(direction),
                1,
            ]
            viewProjections.append(
                contentsOf: multiplyShadowMatrix(projection, view)
            )
        }
        guard viewProjections.count ==
                GPUSceneOmniShadowProbe.faceCount * 16,
              viewProjections.allSatisfy(\.isFinite) else {
            return disabled
        }
        return .init(
            enabled: true,
            range: descriptor.range,
            softness: descriptor.shadowSoftness,
            updatePeriodFrames: descriptor.shadowUpdatePeriodFrames,
            viewProjections: viewProjections
        )
    }

    private static func multiplyShadowMatrix(
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

    func sceneClearColor(
        environment: EnvironmentDescriptor
    ) throws -> RenderColor {
        let source: RenderColor
        let energy: Float
        if environment.skyCubemap.isValid {
            guard let sky = cubemaps[environment.skyCubemap] else {
                throw GPUResourceStagingError.invalidCubemap(
                    environment.skyCubemap.rawValue
                )
            }
            source = sky.color
            energy = environment.backgroundEnergy * sky.skyEnergy
        } else {
            source = environment.backgroundColor
            energy = environment.backgroundEnergy
        }
        return Self.scaled(
            GPUColorSpace.linearized(source),
            by: energy
        )
    }

    private static func scaled(
        _ color: RenderColor,
        by energy: Float
    ) -> RenderColor {
        func channel(_ value: UInt8) -> UInt8 {
            UInt8(clamping: Int((Float(value) * energy).rounded()))
        }
        return .init(
            r: channel(color.r),
            g: channel(color.g),
            b: channel(color.b),
            a: color.a
        )
    }

    func animationPose(
        model: ModelHandle,
        player: AnimationPlayerHandle?,
        tree treeHandle: AnimationTreeHandle?
    ) throws -> GPUAnimationPoseProbe {
        guard player == nil,
              let treeHandle,
              let tree = animationTrees[treeHandle],
              tree.model == model,
              let library = animationLibraries[tree.library] else {
            throw GPUResourceStagingError.invalidAnimationTree(
                treeHandle?.rawValue ?? 0
            )
        }
        let samples = weightedAnimationClips(
            named: tree.rootNode,
            tree: tree,
            weight: 1,
            visited: []
        )
        let weightSum = samples.reduce(Float.zero) {
            $0 + $1.weight
        }
        guard !samples.isEmpty,
              weightSum.isFinite,
              weightSum > 0 else {
            throw GPUResourceStagingError.invalidAnimationTree(
                treeHandle.rawValue
            )
        }
        let normalizedSamples = samples.map {
            GPUAnimationSampleProbe(
                animationIndex: $0.animationIndex,
                time: $0.time,
                weight: $0.weight / weightSum
            )
        }
        let indices = normalizedSamples.map {
            UInt32($0.animationIndex)
        }
        let times = normalizedSamples.map(\.time)
        let weights = normalizedSamples.map(\.weight)
        let updated = indices.withUnsafeBufferPointer {
            indexBuffer in
            times.withUnsafeBufferPointer { timeBuffer in
                weights.withUnsafeBufferPointer { weightBuffer in
                    btbh_gpu_animation_pose_set_samples(
                        tree.pose,
                        indexBuffer.baseAddress,
                        timeBuffer.baseAddress,
                        weightBuffer.baseAddress,
                        UInt32(normalizedSamples.count)
                    )
                }
            }
        }
        guard updated != 0 else {
            throw GPUResourceStagingError.invalidAnimationTree(
                treeHandle.rawValue
            )
        }
        return .init(
            pointer: tree.pose,
            library: library.pointer,
            samples: normalizedSamples
        )
    }

    func destroyModel(_ handle: ModelHandle) {
        if isModelReferenced(handle) {
            pendingModelDestruction.insert(handle)
            return
        }
        destroyModelNow(handle)
    }

    private func destroyModelNow(_ handle: ModelHandle) {
        guard
            let modelHandles,
            let resource = models.removeValue(forKey: handle),
            modelHandles.release(handle)
        else {
            return
        }
        invalidateSceneModelMaterials(model: handle)
        platform.destroyModel(resource.pointer)
        pendingModelDestruction.remove(handle)
    }
    func createMesh(
        _ descriptor: MeshDescriptor,
        vertices: UnsafeRawBufferPointer,
        indices: UnsafeRawBufferPointer?
    ) throws -> MeshHandle {
        trace("createMesh")
        guard let meshHandles else {
            throw GPUResourceStagingError.invalidMeshDescriptor(
                "Could not allocate the GPU mesh handle pool"
            )
        }
        let upload = try Self.meshUpload(
            descriptor,
            vertices: vertices,
            indices: indices
        )
        let storage = try upload.packedVertices.withUnsafeBytes {
            vertexBytes in
            if upload.indices.isEmpty {
                return try platform.createCustomMesh(
                    vertices: vertexBytes,
                    vertexCount: UInt32(descriptor.vertexCount),
                    indices: nil
                )
            }
            return try upload.indices.withUnsafeBufferPointer {
                indexBuffer in
                try platform.createCustomMesh(
                    vertices: vertexBytes,
                    vertexCount: UInt32(descriptor.vertexCount),
                    indices: indexBuffer
                )
            }
        }
        do {
            let handle = try meshHandles.acquire(MeshHandle.self)
            meshes[handle] = .init(
                storage: storage,
                descriptor: descriptor
            )
            return handle
        } catch {
            platform.destroyCustomMesh(storage.pointer)
            throw error
        }
    }

    func updateMesh(
        _ handle: MeshHandle,
        vertices: UnsafeRawBufferPointer,
        indices: UnsafeRawBufferPointer?
    ) throws {
        trace("updateMesh")
        guard let resource = meshes[handle] else {
            throw GPUResourceStagingError.invalidMesh(handle.rawValue)
        }
        guard case .streamed = resource.descriptor.storage else {
            throw GPUResourceStagingError.invalidMeshUpload(
                "Only streamed meshes can be updated"
            )
        }
        guard vertices.count % MemoryLayout<RenderVertex>.stride == 0,
              (indices?.count ?? 0) % MemoryLayout<UInt32>.stride == 0 else {
            throw GPUResourceStagingError.invalidMeshUpload(
                "Mesh updates must contain whole vertices and indices"
            )
        }
        let updatedDescriptor = MeshDescriptor(
            topology: resource.descriptor.topology,
            storage: resource.descriptor.storage,
            vertexCount:
                vertices.count / MemoryLayout<RenderVertex>.stride,
            indexCount:
                (indices?.count ?? 0) / MemoryLayout<UInt32>.stride,
            bounds: resource.descriptor.bounds
        )
        let upload = try Self.meshUpload(
            updatedDescriptor,
            vertices: vertices,
            indices: indices
        )
        try upload.packedVertices.withUnsafeBytes { vertexBytes in
            if upload.indices.isEmpty {
                try platform.updateCustomMesh(
                    resource.storage.pointer,
                    vertices: vertexBytes,
                    indices: nil
                )
            } else {
                try upload.indices.withUnsafeBufferPointer {
                    indexBuffer in
                    try platform.updateCustomMesh(
                        resource.storage.pointer,
                        vertices: vertexBytes,
                        indices: indexBuffer
                    )
                }
            }
        }
        meshes[handle] = .init(
            storage: .init(
                pointer: resource.storage.pointer,
                vertexCount: updatedDescriptor.vertexCount,
                indexCount: updatedDescriptor.indexCount,
                vertexByteCount:
                    upload.packedVertices.count *
                    MemoryLayout<Float>.stride
            ),
            descriptor: updatedDescriptor
        )
    }

    func destroyMesh(_ handle: MeshHandle) {
        guard
            let meshHandles,
            let resource = meshes.removeValue(forKey: handle),
            meshHandles.release(handle)
        else {
            return
        }
        platform.destroyCustomMesh(resource.storage.pointer)
    }
    func createMaterial(
        _ descriptor: MaterialDescriptor
    ) throws -> MaterialHandle {
        trace("createMaterial")
        let resource = try materialResource(descriptor)
        return try storeMaterial(resource)
    }

    func createMaterialOverride(
        from model: ModelHandle,
        materialIndex: Int,
        patch: ModelMaterialPatch
    ) throws -> MaterialHandle {
        trace("createMaterialOverride")
        guard
            !pendingModelDestruction.contains(model),
            let modelResource = models[model]
        else {
            throw GPUResourceStagingError.invalidModel(model.rawValue)
        }
        guard
            materialIndex >= 0,
            materialIndex < modelResource.metadata.materialCount
        else {
            throw GPUResourceStagingError.invalidMaterialDescriptor(
                "Model material index \(materialIndex) is outside 0..<\(modelResource.metadata.materialCount)"
            )
        }
        let textureDependencies = Set(
            [
                patch.albedoTexture,
                patch.normalTexture,
                patch.ormTexture,
            ].compactMap { $0 }
        )
        try validateMaterialDependencies(
            textures: textureDependencies,
            surfaceShader: patch.surfaceShader
        )
        return try storeMaterial(.init(
            storage: .modelOverride(
                model: model,
                materialIndex: materialIndex,
                patch: patch
            ),
            modelDependency: model,
            textureDependencies: textureDependencies,
            surfaceShaderDependency: patch.surfaceShader
        ))
    }

    func updateMaterial(
        _ handle: MaterialHandle,
        descriptor: MaterialDescriptor
    ) throws {
        trace("updateMaterial")
        guard let previous = materials[handle] else {
            throw GPUResourceStagingError.invalidMaterial(handle.rawValue)
        }
        let replacement = try materialResource(descriptor)
        materials[handle] = replacement
        invalidateSceneModelMaterials(using: handle)
        collectMaterialDependencies(previous)
    }

    func destroyMaterial(_ handle: MaterialHandle) {
        guard
            let materialHandles,
            let resource = materials.removeValue(forKey: handle),
            materialHandles.release(handle)
        else {
            return
        }
        invalidateSceneModelMaterials(using: handle)
        collectMaterialDependencies(resource)
    }
    func createInstanceBuffer(
        _ descriptor: InstanceBufferDescriptor
    ) throws -> InstanceBufferHandle {
        trace("createInstanceBuffer")
        let stride = try Self.instanceBufferStride(descriptor)
        guard let instanceBufferHandles else {
            throw GPUResourceStagingError.invalidInstanceBufferDescriptor(
                "Could not allocate the GPU instance buffer handle pool"
            )
        }
        let storage = try platform.createInstanceBuffer(
            capacity: UInt32(descriptor.capacity),
            stride: UInt32(stride)
        )
        do {
            let handle = try instanceBufferHandles.acquire(
                InstanceBufferHandle.self
            )
            instanceBuffers[handle] = .init(
                storage: storage,
                descriptor: descriptor
            )
            return handle
        } catch {
            platform.destroyInstanceBuffer(storage.pointer)
            throw error
        }
    }

    func uploadInstances(
        _ handle: InstanceBufferHandle,
        instances: UnsafeRawBufferPointer
    ) throws {
        trace("uploadInstances")
        guard let resource = instanceBuffers[handle] else {
            throw GPUResourceStagingError.invalidInstanceBuffer(
                handle.rawValue
            )
        }
        guard instances.count > 0 else {
            throw GPUResourceStagingError.invalidInstanceUpload(
                "Instance upload must contain whole records within the allocated buffer"
            )
        }
        if resource.descriptor.stride == 0 {
            let recordStride = MemoryLayout<RenderInstance>.stride
            guard recordStride == GPUInstanceRecordPacking.byteStride,
                  instances.count.isMultiple(of: recordStride),
                  instances.count / recordStride <=
                    resource.descriptor.capacity,
                  let baseAddress = instances.baseAddress,
                  Int(bitPattern: baseAddress).isMultiple(
                    of: MemoryLayout<RenderInstance>.alignment
                  ) else {
                throw GPUResourceStagingError.invalidInstanceUpload(
                    "Semantic instance upload must contain aligned RenderInstance records"
                )
            }
            var semanticInstances: [RenderInstance] = []
            semanticInstances.reserveCapacity(instances.count / recordStride)
            for offset in stride(
                from: 0,
                to: instances.count,
                by: recordStride
            ) {
                semanticInstances.append(
                    instances.load(
                        fromByteOffset: offset,
                        as: RenderInstance.self
                    )
                )
            }
            try uploadSemanticInstances(semanticInstances, to: resource)
        } else {
            guard instances.count <= resource.storage.byteCapacity,
                  instances.count % resource.storage.stride == 0 else {
                throw GPUResourceStagingError.invalidInstanceUpload(
                    "Instance upload must contain whole records within the allocated buffer"
                )
            }
            try platform.uploadInstanceBuffer(
                resource.storage.pointer,
                bytes: instances
            )
        }
    }

    func instanceBufferResource(
        _ handle: InstanceBufferHandle,
        semanticInstances: [RenderInstance],
        semanticRevision: UInt64?
    ) throws -> GPUInstanceBufferResource {
        guard let resource = instanceBuffers[handle] else {
            throw GPUResourceStagingError.invalidInstanceBuffer(
                handle.rawValue
            )
        }
        guard resource.descriptor.stride == 0,
              resource.descriptor.includesColor,
              resource.descriptor.customFloatCount == 4,
              !semanticInstances.isEmpty,
              semanticInstances.count <= resource.descriptor.capacity else {
            throw GPUResourceStagingError.invalidInstanceUpload(
                "Persistent scene instances require a semantic buffer within capacity"
            )
        }
        if let semanticRevision,
           semanticRevision == resource.semanticRevision {
            return resource
        }
        if Self.sharesStorage(
            semanticInstances,
            resource.semanticInstances
        ) || semanticInstances == resource.semanticInstances {
            resource.semanticRevision = semanticRevision
            return resource
        }
        try uploadSemanticInstances(
            semanticInstances,
            revision: semanticRevision,
            to: resource
        )
        return resource
    }

    private func uploadSemanticInstances(
        _ instances: [RenderInstance],
        revision: UInt64? = nil,
        to resource: GPUInstanceBufferResource
    ) throws {
        semanticInstanceUploadScratch.removeAll(keepingCapacity: true)
        semanticInstanceUploadScratch.reserveCapacity(
            instances.count * GPUInstanceRecordPacking.floatCount
        )
        for instance in instances {
            GPUInstanceRecordPacking.append(
                instance,
                to: &semanticInstanceUploadScratch
            )
        }
        try semanticInstanceUploadScratch.withUnsafeBytes {
            try platform.uploadInstanceBuffer(
                resource.storage.pointer,
                bytes: $0
            )
        }
        resource.semanticInstances = instances
        resource.semanticRevision = revision
        semanticInstanceUploadCount += 1
    }

    private static func sharesStorage<T>(_ lhs: [T], _ rhs: [T]) -> Bool {
        guard lhs.count == rhs.count else { return false }
        return lhs.withUnsafeBufferPointer { lhsBuffer in
            rhs.withUnsafeBufferPointer { rhsBuffer in
                lhsBuffer.baseAddress == rhsBuffer.baseAddress
            }
        }
    }

    func destroyInstanceBuffer(_ handle: InstanceBufferHandle) {
        guard
            let instanceBufferHandles,
            let resource = instanceBuffers.removeValue(forKey: handle),
            instanceBufferHandles.release(handle)
        else {
            return
        }
        platform.destroyInstanceBuffer(resource.storage.pointer)
    }
    func loadAnimationLibrary(
        path: String
    ) throws -> AnimationLibraryHandle {
        trace("loadAnimationLibrary")
        guard let animationLibraryHandles else {
            throw GPUAnimationLibraryProbeError.operationFailed(
                message: "Could not allocate the animation library handle pool"
            )
        }
        let resource = try GPUAnimationLibraryProbe.load(path: path)
        let handle: AnimationLibraryHandle
        do {
            handle = try animationLibraryHandles.acquire(
                AnimationLibraryHandle.self
            )
        } catch {
            GPUAnimationLibraryProbe.destroy(resource.pointer)
            throw error
        }
        animationLibraries[handle] = resource
        return handle
    }

    func destroyAnimationLibrary(_ handle: AnimationLibraryHandle) {
        if animationTrees.values.contains(where: { $0.library == handle }) {
            pendingAnimationLibraryDestruction.insert(handle)
            return
        }
        destroyAnimationLibraryNow(handle)
    }

    private func destroyAnimationLibraryNow(
        _ handle: AnimationLibraryHandle
    ) {
        guard
            let animationLibraryHandles,
            let resource = animationLibraries.removeValue(forKey: handle),
            animationLibraryHandles.release(handle)
        else {
            return
        }
        GPUAnimationLibraryProbe.destroy(resource.pointer)
        pendingAnimationLibraryDestruction.remove(handle)
    }
    func createAnimationPlayer(_ descriptor: AnimationPlayerDescriptor) throws -> AnimationPlayerHandle { throw unsupported() }
    func updateAnimationPlayer(_ handle: AnimationPlayerHandle, deltaSeconds: Float) throws { throw unsupported() }
    func setAnimation(_ handle: AnimationPlayerHandle, name: String, loop: Bool, time: Float?) throws { throw unsupported() }
    func destroyAnimationPlayer(_ handle: AnimationPlayerHandle) {}
    func createAnimationTree(
        _ descriptor: AnimationTreeDescriptor
    ) throws -> AnimationTreeHandle {
        trace("createAnimationTree")
        guard
            !pendingModelDestruction.contains(descriptor.model),
            models[descriptor.model] != nil
        else {
            throw GPUResourceStagingError.invalidModel(
                descriptor.model.rawValue
            )
        }
        guard
            !pendingAnimationLibraryDestruction.contains(descriptor.library),
            let library = animationLibraries[descriptor.library]
        else {
            throw GPUResourceStagingError.invalidAnimationLibrary(
                descriptor.library.rawValue
            )
        }
        try validateAnimationTreeDescriptor(
            descriptor,
            animationCount: library.animationCount
        )
        guard let animationTreeHandles else {
            throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                "Could not allocate the animation tree handle pool"
            )
        }
        let clips = Dictionary(uniqueKeysWithValues: descriptor.clips.map {
            (
                $0.name,
                GPUAnimationClipState(
                    animationIndex: $0.animationIndex,
                    loop: $0.loop,
                    duration: library.durations[$0.animationIndex],
                    speed: $0.speed,
                    time: 0,
                    play: true
                )
            )
        })
        let switches = Dictionary(
            uniqueKeysWithValues: descriptor.switches.map {
                let descriptor = $0
                return (
                    descriptor.name,
                    GPUAnimationSwitchState(
                        inputs: descriptor.inputs,
                        crossfadeSeconds:
                            descriptor.crossfadeSeconds,
                        synchronized: descriptor.synchronized,
                        activeInput: descriptor.activeInput,
                        previousInput: 0,
                        weights: descriptor.inputs.indices.map {
                            $0 == descriptor.activeInput ? 1 : 0
                        }
                    )
                )
            }
        )
        guard let pose = btbh_gpu_animation_pose_create(
            library.pointer
        ) else {
            throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                "Could not allocate the GPU animation pose"
            )
        }
        let handle: AnimationTreeHandle
        do {
            handle = try animationTreeHandles.acquire(
                AnimationTreeHandle.self
            )
        } catch {
            btbh_gpu_animation_pose_destroy(pose)
            throw error
        }
        animationTrees[handle] = .init(
            model: descriptor.model,
            library: descriptor.library,
            rootNode: descriptor.rootNode,
            rootBone: descriptor.rootBone,
            pose: pose,
            clips: clips,
            switches: switches
        )
        return handle
    }

    func updateAnimationTree(
        _ handle: AnimationTreeHandle,
        deltaSeconds: Float
    ) throws {
        guard deltaSeconds.isFinite, deltaSeconds >= 0 else {
            throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                "Animation tree delta must be finite and nonnegative"
            )
        }
        guard var tree = animationTrees[handle] else {
            throw GPUResourceStagingError.invalidAnimationTree(
                handle.rawValue
            )
        }
        updateAnimationNode(
            tree.rootNode,
            deltaSeconds: deltaSeconds,
            tree: &tree
        )
        animationTrees[handle] = tree
    }

    func setAnimationTreeParameter(
        _ handle: AnimationTreeHandle,
        name: String,
        value: Float
    ) throws {
        guard value.isFinite else {
            throw GPUResourceStagingError.invalidAnimationTreeParameter(name)
        }
        guard var tree = animationTrees[handle] else {
            throw GPUResourceStagingError.invalidAnimationTree(
                handle.rawValue
            )
        }
        let components = Self.animationTreeParameterComponents(name)
        switch (components.kind, components.field) {
        case ("clip", "speed"), ("clip", "time"), ("clip", "play"):
            guard var clip = tree.clips[components.node] else {
                throw GPUResourceStagingError
                    .invalidAnimationTreeParameter(name)
            }
            switch components.field {
            case "speed":
                clip.speed = value
            case "time":
                clip.time = max(0, value)
            default:
                clip.play = value > 0
            }
            tree.clips[components.node] = clip
        case ("switch", "activeInput"):
            guard
                var animationSwitch = tree.switches[components.node],
                value.rounded(.towardZero) == value,
                value >= 0,
                value < Float(animationSwitch.inputs.count)
            else {
                throw GPUResourceStagingError
                    .invalidAnimationTreeParameter(name)
            }
            animationSwitch.activeInput = Int(value)
            tree.switches[components.node] = animationSwitch
        default:
            throw GPUResourceStagingError.invalidAnimationTreeParameter(name)
        }
        animationTrees[handle] = tree
    }

    func animationTreeParameter(
        _ handle: AnimationTreeHandle,
        name: String
    ) throws -> Float {
        guard let tree = animationTrees[handle] else {
            throw GPUResourceStagingError.invalidAnimationTree(
                handle.rawValue
            )
        }
        let components = Self.animationTreeParameterComponents(name)
        switch (components.kind, components.field) {
        case ("clip", "speed"):
            guard let clip = tree.clips[components.node] else {
                throw GPUResourceStagingError
                    .invalidAnimationTreeParameter(name)
            }
            return clip.speed
        case ("clip", "time"):
            guard let clip = tree.clips[components.node] else {
                throw GPUResourceStagingError
                    .invalidAnimationTreeParameter(name)
            }
            return clip.time
        case ("clip", "play"):
            guard let clip = tree.clips[components.node] else {
                throw GPUResourceStagingError
                    .invalidAnimationTreeParameter(name)
            }
            return clip.play ? 1 : 0
        case ("switch", "activeInput"):
            guard let animationSwitch = tree.switches[components.node] else {
                throw GPUResourceStagingError
                    .invalidAnimationTreeParameter(name)
            }
            return Float(animationSwitch.activeInput)
        default:
            throw GPUResourceStagingError.invalidAnimationTreeParameter(name)
        }
    }

    func destroyAnimationTree(_ handle: AnimationTreeHandle) {
        guard
            let animationTreeHandles,
            let tree = animationTrees.removeValue(forKey: handle),
            animationTreeHandles.release(handle)
        else {
            return
        }
        btbh_gpu_animation_pose_destroy(tree.pose)
        collectPendingModel(tree.model)
        collectPendingAnimationLibrary(tree.library)
    }
    func createLight(_ descriptor: LightDescriptor) throws -> LightHandle {
        trace("createLight")
        try Self.validateLightDescriptor(descriptor)
        guard let lightHandles else {
            throw GPUResourceStagingError.invalidLightDescriptor(
                "Could not allocate the GPU light handle pool"
            )
        }
        let handle = try lightHandles.acquire(LightHandle.self)
        lights[handle] = descriptor
        return handle
    }

    func updateLight(
        _ handle: LightHandle,
        descriptor: LightDescriptor
    ) throws {
        trace("updateLight")
        guard lights[handle] != nil else {
            throw GPUResourceStagingError.invalidLight(handle.rawValue)
        }
        try Self.validateLightDescriptor(descriptor)
        lights[handle] = descriptor
    }

    func destroyLight(_ handle: LightHandle) {
        guard
            let lightHandles,
            lights.removeValue(forKey: handle) != nil,
            lightHandles.release(handle)
        else {
            return
        }
    }

    func createProceduralSky(
        _ descriptor: ProceduralSkyDescriptor
    ) throws -> CubemapHandle {
        trace("createProceduralSky")
        try Self.validateSkyDescriptor(descriptor)
        guard let cubemapHandles else {
            throw GPUResourceStagingError.invalidSkyDescriptor(
                "Could not allocate the GPU cubemap handle pool"
            )
        }
        let handle = try cubemapHandles.acquire(CubemapHandle.self)
        cubemaps[handle] = descriptor
        return handle
    }

    func createAmbientMap(
        from cubemap: CubemapHandle
    ) throws -> AmbientMapHandle {
        trace("createAmbientMap")
        guard
            !pendingCubemapDestruction.contains(cubemap),
            let sky = cubemaps[cubemap]
        else {
            throw GPUResourceStagingError.invalidCubemap(cubemap.rawValue)
        }
        guard let ambientMapHandles else {
            throw GPUResourceStagingError.invalidAmbientMap(0)
        }
        let handle = try ambientMapHandles.acquire(AmbientMapHandle.self)
        ambientMaps[handle] = .init(
            cubemap: cubemap,
            environment:
                GPUProceduralEnvironmentProjection.project(sky)
        )
        return handle
    }

    func destroyCubemap(_ handle: CubemapHandle) {
        if ambientMaps.values.contains(where: { $0.cubemap == handle }) {
            pendingCubemapDestruction.insert(handle)
            return
        }
        destroyCubemapNow(handle)
    }

    private func destroyCubemapNow(_ handle: CubemapHandle) {
        guard
            let cubemapHandles,
            cubemaps.removeValue(forKey: handle) != nil,
            cubemapHandles.release(handle)
        else {
            return
        }
        pendingCubemapDestruction.remove(handle)
    }

    func destroyAmbientMap(_ handle: AmbientMapHandle) {
        guard
            let ambientMapHandles,
            let resource = ambientMaps.removeValue(forKey: handle),
            ambientMapHandles.release(handle)
        else {
            return
        }
        collectPendingCubemap(resource.cubemap)
    }
    func createSurfaceShader(
        _ descriptor: SurfaceShaderDescriptor
    ) throws -> SurfaceShaderHandle {
        trace("createSurfaceShader")
        if let vertexPath = descriptor.vertexPath {
            try validateShaderAsset(vertexPath)
        }
        try validateShaderAsset(descriptor.fragmentPath)
        try validateTextureDependencies(descriptor.parameters)
        guard let surfaceShaderHandles else {
            throw GPUResourceStagingError.invalidSurfaceShader(0)
        }
        let handle = try surfaceShaderHandles.acquire(
            SurfaceShaderHandle.self
        )
        surfaceShaders[handle] = .init(
            vertexPath: descriptor.vertexPath,
            fragmentPath: descriptor.fragmentPath,
            parameters: descriptor.parameters
        )
        trace("createSurfaceShader.complete")
        return handle
    }

    func updateSurfaceShader(
        _ handle: SurfaceShaderHandle,
        parameter: String,
        value: ShaderParameterValue
    ) throws {
        trace("updateSurfaceShader")
        guard
            !pendingSurfaceShaderDestruction.contains(handle),
            var resource = surfaceShaders[handle]
        else {
            throw GPUResourceStagingError.invalidSurfaceShader(
                handle.rawValue
            )
        }
        try validateTextureDependencies([parameter: value])
        let oldDependencies = Self.textureDependencies(
            in: resource.parameters
        )
        resource.parameters[parameter] = value
        surfaceShaders[handle] = resource
        invalidateSceneModelMaterials(surfaceShader: handle)
        collectPendingTextures(oldDependencies)
    }

    func destroySurfaceShader(_ handle: SurfaceShaderHandle) {
        if isSurfaceShaderReferenced(handle) {
            pendingSurfaceShaderDestruction.insert(handle)
            return
        }
        destroySurfaceShaderNow(handle)
    }

    private func destroySurfaceShaderNow(_ handle: SurfaceShaderHandle) {
        guard
            let surfaceShaderHandles,
            let resource = surfaceShaders.removeValue(forKey: handle),
            surfaceShaderHandles.release(handle)
        else {
            return
        }
        pendingSurfaceShaderDestruction.remove(handle)
        collectPendingTextures(
            Self.textureDependencies(in: resource.parameters)
        )
    }

    private func invalidateSceneModelMaterials(model: ModelHandle) {
        let keys = sceneModelMaterialsCache.keys.filter {
            $0.model == model
        }
        for key in keys {
            sceneModelMaterialsCache.removeValue(forKey: key)
        }
    }

    private func invalidateSceneModelMaterials(using material: MaterialHandle) {
        let keys = sceneModelMaterialsCache.keys.filter { key in
            key.overrides.contains { $0.material == material }
        }
        for key in keys {
            sceneModelMaterialsCache.removeValue(forKey: key)
        }
    }

    private func invalidateSceneModelMaterials(
        surfaceShader: SurfaceShaderHandle
    ) {
        let dependentMaterials = Set(materials.compactMap {
            handle, resource in
            resource.surfaceShaderDependency == surfaceShader
                ? handle
                : nil
        })
        guard !dependentMaterials.isEmpty else { return }
        let keys = sceneModelMaterialsCache.keys.filter { key in
            key.overrides.contains {
                dependentMaterials.contains($0.material)
            }
        }
        for key in keys {
            sceneModelMaterialsCache.removeValue(forKey: key)
        }
    }
    func createScreenShader(
        _ descriptor: ScreenShaderDescriptor
    ) throws -> ScreenShaderHandle {
        var isDirectory: ObjCBool = false
        guard
            FileManager.default.fileExists(
                atPath: descriptor.fragmentPath,
                isDirectory: &isDirectory
            ),
            !isDirectory.boolValue
        else {
            throw GPUResourceStagingError.missingAsset(
                descriptor.fragmentPath
            )
        }
        try validateTextureDependencies(descriptor.parameters)
        guard let screenShaderHandles else {
            throw GPUTextureProbeError.operationFailed(
                message: "Could not allocate the GPU screen shader handle pool"
            )
        }
        let handle = try screenShaderHandles.acquire(
            ScreenShaderHandle.self
        )
        screenShaders[handle] = .init(
            kind: .init(fragmentPath: descriptor.fragmentPath),
            parameters: descriptor.parameters
        )
        return handle
    }

    func updateScreenShader(
        _ handle: ScreenShaderHandle,
        parameter: String,
        value: ShaderParameterValue
    ) throws {
        guard var resource = screenShaders[handle] else {
            throw GPUResourceStagingError.invalidScreenShader(
                handle.rawValue
            )
        }
        try validateTextureDependencies([parameter: value])
        let oldDependencies = Self.textureDependencies(
            in: resource.parameters
        )
        resource.parameters[parameter] = value
        screenShaders[handle] = resource
        collectPendingTextures(oldDependencies)
    }

    func destroyScreenShader(_ handle: ScreenShaderHandle) {
        guard
            let screenShaderHandles,
            let resource = screenShaders.removeValue(forKey: handle),
            screenShaderHandles.release(handle)
        else {
            return
        }
        collectPendingTextures(
            Self.textureDependencies(in: resource.parameters)
        )
    }

    private func updateAnimationNode(
        _ name: String,
        deltaSeconds: Float,
        tree: inout GPUAnimationTreeResource
    ) {
        if var clip = tree.clips[name] {
            guard clip.play else { return }
            let duration = clip.duration
            let advancedTime = clip.time + deltaSeconds * clip.speed
            if !advancedTime.isFinite {
                let wideDuration = Double(duration)
                let wideAdvancedTime = Double(clip.time) +
                    Double(deltaSeconds) * Double(clip.speed)
                if clip.loop, wideDuration > 0 {
                    var wrappedTime = wideAdvancedTime
                        .truncatingRemainder(dividingBy: wideDuration)
                    if wrappedTime < 0 {
                        wrappedTime += wideDuration
                    }
                    clip.time = Float(wrappedTime)
                } else {
                    clip.play = false
                    clip.time = Float(
                        min(max(0, wideAdvancedTime), wideDuration)
                    )
                }
                tree.clips[name] = clip
                return
            }
            let crossedBoundary =
                (clip.speed < 0 && advancedTime <= 0) ||
                (clip.speed > 0 && advancedTime >= duration)
            if crossedBoundary {
                if clip.loop {
                    if duration > 0 {
                        let singleWrappedTime = clip.speed < 0
                            ? advancedTime + duration
                            : advancedTime - duration
                        let singleWrapIsSufficient = clip.speed < 0
                            ? singleWrappedTime > 0 &&
                                singleWrappedTime <= duration
                            : singleWrappedTime >= 0 &&
                                singleWrappedTime < duration
                        if singleWrapIsSufficient {
                            clip.time = singleWrappedTime
                        } else {
                            var wrappedTime = advancedTime
                                .truncatingRemainder(
                                    dividingBy: duration
                                )
                            if wrappedTime < 0 {
                                wrappedTime += duration
                            }
                            clip.time = wrappedTime
                        }
                    } else {
                        clip.time = 0
                    }
                } else {
                    clip.play = false
                    clip.time = min(max(0, advancedTime), duration)
                }
            } else {
                clip.time = advancedTime
            }
            tree.clips[name] = clip
            return
        }
        guard var animationSwitch = tree.switches[name] else {
            return
        }
        if
            animationSwitch.activeInput != animationSwitch.previousInput,
            !animationSwitch.synchronized
        {
            resetAnimationNode(
                animationSwitch.inputs[animationSwitch.activeInput],
                tree: &tree
            )
        }
        for input in animationSwitch.inputs {
            updateAnimationNode(
                input,
                deltaSeconds: deltaSeconds,
                tree: &tree
            )
        }
        if animationSwitch.crossfadeSeconds <= deltaSeconds {
            animationSwitch.weights =
                animationSwitch.inputs.indices.map {
                    $0 == animationSwitch.activeInput ? 1 : 0
                }
        } else if animationSwitch.crossfadeSeconds > 0 {
            let fade =
                deltaSeconds /
                animationSwitch.crossfadeSeconds
            for index in animationSwitch.weights.indices {
                let direction: Float =
                    index == animationSwitch.activeInput ? 1 : -1
                animationSwitch.weights[index] = min(
                    1,
                    max(
                        0,
                        animationSwitch.weights[index] +
                            direction * fade
                    )
                )
            }
        }
        animationSwitch.previousInput = animationSwitch.activeInput
        tree.switches[name] = animationSwitch
    }

    private func resetAnimationNode(
        _ name: String,
        tree: inout GPUAnimationTreeResource
    ) {
        if var clip = tree.clips[name] {
            clip.time = clip.speed >= 0 ? 0 : clip.duration
            tree.clips[name] = clip
            return
        }
        guard let animationSwitch = tree.switches[name] else {
            return
        }
        for input in animationSwitch.inputs {
            resetAnimationNode(input, tree: &tree)
        }
    }

    private func weightedAnimationClips(
        named name: String,
        tree: GPUAnimationTreeResource,
        weight: Float,
        visited: Set<String>
    ) -> [GPUAnimationSampleProbe] {
        guard weight > 0, !visited.contains(name) else {
            return []
        }
        if let clip = tree.clips[name] {
            return [
                .init(
                    animationIndex: clip.animationIndex,
                    time: clip.time,
                    weight: weight
                )
            ]
        }
        guard let animationSwitch = tree.switches[name] else {
            return []
        }
        var visited = visited
        visited.insert(name)
        return animationSwitch.inputs.indices.flatMap { index in
            weightedAnimationClips(
                named: animationSwitch.inputs[index],
                tree: tree,
                weight: weight * animationSwitch.weights[index],
                visited: visited
            )
        }
    }

    private func materialResource(
        _ descriptor: MaterialDescriptor
    ) throws -> GPUMaterialResource {
        let textureDependencies = Set(
            [
                descriptor.albedoTexture,
                descriptor.emissionTexture,
                descriptor.normalTexture,
                descriptor.ormTexture,
            ].filter(\.isValid)
        )
        let surfaceShader = descriptor.surfaceShader.isValid
            ? descriptor.surfaceShader
            : nil
        try validateMaterialDependencies(
            textures: textureDependencies,
            surfaceShader: surfaceShader
        )
        return .init(
            storage: .descriptor(descriptor),
            modelDependency: nil,
            textureDependencies: textureDependencies,
            surfaceShaderDependency: surfaceShader
        )
    }

    private func storeMaterial(
        _ resource: GPUMaterialResource
    ) throws -> MaterialHandle {
        guard let materialHandles else {
            throw GPUResourceStagingError.invalidMaterialDescriptor(
                "Could not allocate the material handle pool"
            )
        }
        let handle = try materialHandles.acquire(MaterialHandle.self)
        materials[handle] = resource
        return handle
    }

    private func validateMaterialDependencies(
        textures textureHandles: Set<TextureHandle>,
        surfaceShader: SurfaceShaderHandle?
    ) throws {
        for handle in textureHandles
        where
            pendingTextureDestruction.contains(handle) ||
            textures[handle] == nil
        {
            throw GPUResourceStagingError.invalidTexture(handle.rawValue)
        }
        if let surfaceShader,
            pendingSurfaceShaderDestruction.contains(surfaceShader) ||
            surfaceShaders[surfaceShader] == nil
        {
            throw GPUResourceStagingError.invalidSurfaceShader(
                surfaceShader.rawValue
            )
        }
    }

    private func collectMaterialDependencies(
        _ resource: GPUMaterialResource
    ) {
        if let model = resource.modelDependency {
            collectPendingModel(model)
        }
        collectPendingTextures(resource.textureDependencies)
        if let surfaceShader = resource.surfaceShaderDependency {
            collectPendingSurfaceShader(surfaceShader)
        }
    }

    private func isModelReferenced(_ handle: ModelHandle) -> Bool {
        animationTrees.values.contains(where: { $0.model == handle }) ||
            materials.values.contains(where: {
                $0.modelDependency == handle
            })
    }

    private func isTextureReferenced(_ handle: TextureHandle) -> Bool {
        screenShaders.values.contains(where: {
            Self.textureDependencies(in: $0.parameters).contains(handle)
        }) || surfaceShaders.values.contains(where: {
            Self.textureDependencies(in: $0.parameters).contains(handle)
        }) || materials.values.contains(where: {
            $0.textureDependencies.contains(handle)
        })
    }

    private func isSurfaceShaderReferenced(
        _ handle: SurfaceShaderHandle
    ) -> Bool {
        materials.values.contains(where: {
            $0.surfaceShaderDependency == handle
        })
    }

    private func validateAnimationTreeDescriptor(
        _ descriptor: AnimationTreeDescriptor,
        animationCount: Int
    ) throws {
        guard !descriptor.clips.isEmpty else {
            throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                "Animation tree requires at least one clip node"
            )
        }
        var knownNodes = Set<String>()
        for clip in descriptor.clips {
            guard
                !clip.name.isEmpty,
                !clip.name.contains("."),
                knownNodes.insert(clip.name).inserted
            else {
                throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                    "Animation tree node names must be nonempty and unique: \(clip.name)"
                )
            }
            guard
                clip.animationIndex >= 0,
                clip.animationIndex < animationCount
            else {
                throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                    "Animation clip index out of range for \(clip.name): \(clip.animationIndex)"
                )
            }
            guard clip.speed.isFinite else {
                throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                    "Animation clip speed must be finite: \(clip.name)"
                )
            }
        }
        for animationSwitch in descriptor.switches {
            guard
                !animationSwitch.name.isEmpty,
                !animationSwitch.name.contains("."),
                knownNodes.insert(animationSwitch.name).inserted
            else {
                throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                    "Animation tree node names must be nonempty and unique: \(animationSwitch.name)"
                )
            }
            guard
                !animationSwitch.inputs.isEmpty,
                animationSwitch.inputs.allSatisfy({
                    knownNodes.contains($0) && $0 != animationSwitch.name
                })
            else {
                throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                    "Animation switch inputs must reference previously declared nodes: \(animationSwitch.name)"
                )
            }
            guard animationSwitch.inputs.indices.contains(
                animationSwitch.activeInput
            ) else {
                throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                    "Animation switch active input is out of range: \(animationSwitch.name)"
                )
            }
            guard
                animationSwitch.crossfadeSeconds.isFinite,
                animationSwitch.crossfadeSeconds >= 0
            else {
                throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                    "Animation switch crossfade must be finite and nonnegative: \(animationSwitch.name)"
                )
            }
        }
        guard knownNodes.contains(descriptor.rootNode) else {
            throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                "Unknown animation tree root: \(descriptor.rootNode)"
            )
        }
        if let rootBone = descriptor.rootBone, rootBone < 0 {
            throw GPUResourceStagingError.invalidAnimationTreeDescriptor(
                "Animation tree root bone must be nonnegative when present"
            )
        }
    }

    private static func animationTreeParameterComponents(
        _ name: String
    ) -> (kind: String, node: String, field: String) {
        let components = name.split(
            separator: ".",
            omittingEmptySubsequences: false
        ).map(String.init)
        guard components.count == 3 else {
            return ("", "", "")
        }
        return (components[0], components[1], components[2])
    }

    private static func instanceBufferStride(
        _ descriptor: InstanceBufferDescriptor
    ) throws -> Int {
        guard descriptor.capacity > 0,
              descriptor.capacity <= Int(UInt32.max),
              descriptor.stride >= 0,
              descriptor.stride <= Int(UInt32.max),
              descriptor.customFloatCount >= 0,
              descriptor.customFloatCount <= 4 else {
            throw GPUResourceStagingError.invalidInstanceBufferDescriptor(
                "Instance buffer fields must fit supported positive ranges"
            )
        }
        let stride: Int
        if descriptor.stride == 0 {
            guard descriptor.includesColor,
                  descriptor.customFloatCount == 4 else {
                throw GPUResourceStagingError.invalidInstanceBufferDescriptor(
                    "Semantic instance buffers require color and four custom floats"
                )
            }
            stride = 96
        } else {
            stride = descriptor.stride
        }
        guard descriptor.capacity <= Int(UInt32.max) / stride else {
            throw GPUResourceStagingError.invalidInstanceBufferDescriptor(
                "Instance buffer allocation exceeds SDL_GPU limits"
            )
        }
        return stride
    }

    private static func meshUpload(
        _ descriptor: MeshDescriptor,
        vertices: UnsafeRawBufferPointer,
        indices: UnsafeRawBufferPointer?
    ) throws -> GPUCustomMeshUpload {
        guard descriptor.vertexCount > 0,
              descriptor.vertexCount <=
                  Int(UInt32.max) / (24 * MemoryLayout<Float>.stride),
              descriptor.indexCount >= 0,
              descriptor.indexCount <=
                  Int(UInt32.max) / MemoryLayout<UInt32>.stride else {
            throw GPUResourceStagingError.invalidMeshDescriptor(
                "Mesh GPU buffer sizes must fit supported UInt32 ranges"
            )
        }
        let (vertexByteCount, vertexByteOverflow) =
            descriptor.vertexCount.multipliedReportingOverflow(
                by: MemoryLayout<RenderVertex>.stride
            )
        let (indexByteCount, indexByteOverflow) =
            descriptor.indexCount.multipliedReportingOverflow(
                by: MemoryLayout<UInt32>.stride
            )
        guard !vertexByteOverflow,
              !indexByteOverflow,
              vertices.count == vertexByteCount,
              (indices?.count ?? 0) == indexByteCount else {
            throw GPUResourceStagingError.invalidMeshUpload(
                "Mesh upload byte counts do not match its descriptor"
            )
        }

        var vertexValues = Array(
            repeating: RenderVertex(
                position: .zero,
                textureCoordinate: .zero,
                normal: .zero,
                tangent: .init(x: 0, y: 0, z: 0, w: 0),
                color: .init(r: 0, g: 0, b: 0, a: 0)
            ),
            count: descriptor.vertexCount
        )
        vertexValues.withUnsafeMutableBytes {
            $0.copyBytes(from: vertices)
        }
        let bounds = descriptor.bounds
        let boundsScalars = [
            bounds.minimum.x,
            bounds.minimum.y,
            bounds.minimum.z,
            bounds.maximum.x,
            bounds.maximum.y,
            bounds.maximum.z,
        ]
        guard boundsScalars.allSatisfy(\.isFinite),
              bounds.minimum.x <= bounds.maximum.x,
              bounds.minimum.y <= bounds.maximum.y,
              bounds.minimum.z <= bounds.maximum.z else {
            throw GPUResourceStagingError.invalidMeshDescriptor(
                "Mesh bounds must be finite and ordered"
            )
        }

        var packedVertices: [Float] = []
        packedVertices.reserveCapacity(descriptor.vertexCount * 24)
        for vertex in vertexValues {
            let textureCoordinate =
                GPUCustomMeshProjection.textureCoordinate(
                    vertex.textureCoordinate
                )
            let scalars = [
                vertex.position.x,
                vertex.position.y,
                vertex.position.z,
                vertex.normal.x,
                vertex.normal.y,
                vertex.normal.z,
                textureCoordinate.x,
                textureCoordinate.y,
                vertex.tangent.x,
                vertex.tangent.y,
                vertex.tangent.z,
                vertex.tangent.w,
            ]
            guard scalars.allSatisfy(\.isFinite) else {
                throw GPUResourceStagingError.invalidMeshUpload(
                    "Mesh vertex vectors must contain only finite values"
                )
            }
            packedVertices.append(contentsOf: scalars)
            packedVertices.append(contentsOf: [
                Float(vertex.color.r) / 255,
                Float(vertex.color.g) / 255,
                Float(vertex.color.b) / 255,
                Float(vertex.color.a) / 255,
                0, 0, 0, 0,
                0, 0, 0, 0,
            ])
        }

        let indexValues: [UInt32]
        if descriptor.indexCount == 0 {
            indexValues = []
        } else if let indices {
            var values = Array(
                repeating: UInt32(0),
                count: descriptor.indexCount
            )
            values.withUnsafeMutableBytes {
                $0.copyBytes(from: indices)
            }
            indexValues = GPUCustomMeshProjection.triangleIndices(values)
        } else {
            throw GPUResourceStagingError.invalidMeshUpload(
                "Indexed mesh upload is missing its index bytes"
            )
        }
        guard indexValues.count == descriptor.indexCount,
              indexValues.allSatisfy({
                  $0 < UInt32(descriptor.vertexCount)
              }) else {
            throw GPUResourceStagingError.invalidMeshUpload(
                "Mesh indices must reference existing vertices"
            )
        }
        return .init(
            packedVertices: packedVertices,
            indices: indexValues
        )
    }

    private static func validateLightDescriptor(
        _ descriptor: LightDescriptor
    ) throws {
        guard
            descriptor.position.x.isFinite,
            descriptor.position.y.isFinite,
            descriptor.position.z.isFinite,
            descriptor.direction.x.isFinite,
            descriptor.direction.y.isFinite,
            descriptor.direction.z.isFinite,
            descriptor.energy.isFinite,
            descriptor.energy >= 0,
            descriptor.indirectEnergy.isFinite,
            descriptor.indirectEnergy >= 0,
            descriptor.range.isFinite,
            descriptor.range >= 0,
            descriptor.shadowSoftness.isFinite,
            descriptor.shadowSoftness >= 0,
            descriptor.shadowUpdatePeriodFrames >= 1,
            descriptor.shadowUpdatePeriodFrames <= 60,
            (0...1).contains(descriptor.occluderShadowStrength),
            descriptor.occluderShadowReach.isFinite,
            descriptor.occluderShadowReach >= 0
        else {
            throw GPUResourceStagingError.invalidLightDescriptor(
                "Light vectors and nonnegative scalar fields must be finite; shadow update period must be 1...60 frames; occluder shadow strength must be 0...1"
            )
        }
        if let focus = descriptor.shadowFocus {
            guard focus.radius.isFinite,
                  focus.radius >= 0,
                  focus.forwardShift.isFinite else {
                throw GPUResourceStagingError.invalidLightDescriptor(
                    "Shadow focus radius must be finite and nonnegative and its forward shift finite"
                )
            }
        }
    }

    private static func validateSkyDescriptor(
        _ descriptor: ProceduralSkyDescriptor
    ) throws {
        let scalars = [
            descriptor.groundEnergy,
            descriptor.groundHorizonCurve,
            descriptor.skyEnergy,
            descriptor.skyHorizonCurve,
            descriptor.sunCurve,
            descriptor.sunEnergy,
            descriptor.sunSizeRadians,
            descriptor.sunDirection.x,
            descriptor.sunDirection.y,
            descriptor.sunDirection.z,
        ]
        guard descriptor.resolution > 0,
              descriptor.resolution <= Int(UInt16.max),
              scalars.allSatisfy(\.isFinite) else {
            throw GPUResourceStagingError.invalidSkyDescriptor(
                "Sky resolution must be positive and all scalar fields finite"
            )
        }
    }

    private func collectPendingCubemap(_ handle: CubemapHandle) {
        guard
            pendingCubemapDestruction.contains(handle),
            !ambientMaps.values.contains(where: { $0.cubemap == handle })
        else {
            return
        }
        destroyCubemapNow(handle)
    }

    private func collectPendingModel(_ handle: ModelHandle) {
        guard
            pendingModelDestruction.contains(handle),
            !isModelReferenced(handle)
        else {
            return
        }
        destroyModelNow(handle)
    }

    private func collectPendingAnimationLibrary(
        _ handle: AnimationLibraryHandle
    ) {
        guard
            pendingAnimationLibraryDestruction.contains(handle),
            !animationTrees.values.contains(where: { $0.library == handle })
        else {
            return
        }
        destroyAnimationLibraryNow(handle)
    }

    private func unsupported(
        _ function: String = #function
    ) -> GPUBackendUnsupportedError {
        let operation = "resources.\(function)"
        platform.failClosed(operation: operation)
        return .operation(operation)
    }

    private func trace(_ operation: @autoclosure () -> String) {
        guard resourceTraceEnabled else {
            return
        }
        let operation = operation()
        FileHandle.standardError.write(
            Data("BTBH_GPU_RESOURCE_TRACE operation=\(operation)\n".utf8)
        )
    }

    func shutdown() {
        screenShaders.removeAll()
        surfaceShaders.removeAll()
        pendingTextureDestruction.removeAll()
        pendingSurfaceShaderDestruction.removeAll()
        animationTrees.removeAll()
        materials.removeAll()
        pendingAnimationLibraryDestruction.removeAll()
        pendingModelDestruction.removeAll()
        lights.removeAll()
        ambientMaps.removeAll()
        pendingCubemapDestruction.removeAll()
        cubemaps.removeAll()
        renderTargets.removeAll()
        for resource in meshes.values {
            platform.destroyCustomMesh(resource.storage.pointer)
        }
        meshes.removeAll()
        for resource in instanceBuffers.values {
            platform.destroyInstanceBuffer(resource.storage.pointer)
        }
        instanceBuffers.removeAll()
        semanticInstanceUploadScratch = []
        for resource in animationLibraries.values {
            GPUAnimationLibraryProbe.destroy(resource.pointer)
        }
        animationLibraries.removeAll()
        for resource in models.values {
            platform.destroyModel(resource.pointer)
        }
        models.removeAll()
        for resource in textures.values {
            platform.destroyTexture(resource.pointer)
        }
        textures.removeAll()
        for resource in fonts.values {
            platform.destroyFont(resource.pointer)
        }
        fonts.removeAll()
    }

    private func validateTextureDependencies(
        _ parameters: [String: ShaderParameterValue]
    ) throws {
        for handle in Self.textureDependencies(in: parameters)
        where
            pendingTextureDestruction.contains(handle) ||
            textures[handle] == nil
        {
            throw GPUResourceStagingError.invalidTexture(handle.rawValue)
        }
    }

    private func validateShaderAsset(_ path: String) throws {
        var isDirectory: ObjCBool = false
        guard
            FileManager.default.fileExists(
                atPath: path,
                isDirectory: &isDirectory
            ),
            !isDirectory.boolValue
        else {
            throw GPUResourceStagingError.missingAsset(path)
        }
    }

    private func collectPendingSurfaceShader(
        _ handle: SurfaceShaderHandle
    ) {
        guard
            pendingSurfaceShaderDestruction.contains(handle),
            !isSurfaceShaderReferenced(handle)
        else {
            return
        }
        destroySurfaceShaderNow(handle)
    }

    private func collectPendingTextures(
        _ candidates: Set<TextureHandle>
    ) {
        for handle in candidates
        where pendingTextureDestruction.contains(handle) &&
            !isTextureReferenced(handle) {
            destroyTextureNow(handle)
        }
    }

    private static func textureDependencies(
        in parameters: [String: ShaderParameterValue]
    ) -> Set<TextureHandle> {
        Set(parameters.values.compactMap { value in
            guard case .texture(let handle) = value else { return nil }
            return handle
        })
    }
}
