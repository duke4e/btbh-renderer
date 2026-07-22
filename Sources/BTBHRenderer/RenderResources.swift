public enum PixelFormat: UInt8, Sendable {
    case r8, rg8, rgba8, rgba8SRGB, rgba16Float, depth32Float
}

public struct TextureUsage: OptionSet, Sendable {
    public let rawValue: UInt8

    public init(rawValue: UInt8) {
        self.rawValue = rawValue
    }

    public static let sampled = Self(rawValue: 1 << 0)
    public static let renderTarget = Self(rawValue: 1 << 1)
    public static let storage = Self(rawValue: 1 << 2)
}

public struct TextureDescriptor: Sendable {
    public let size: PixelSize2D
    public let mipCount: Int
    public let format: PixelFormat
    public let usage: TextureUsage
    public let filter: TextureFilter
    public let wrap: TextureWrap

    public init(size: PixelSize2D, mipCount: Int, format: PixelFormat, usage: TextureUsage, filter: TextureFilter, wrap: TextureWrap) {
        self.size = size
        self.mipCount = mipCount
        self.format = format
        self.usage = usage
        self.filter = filter
        self.wrap = wrap
    }
}

public struct FontDescriptor: Sendable {
    public let path: String
    public let baseSize: Int
    public let glyphs: [UInt32]

    public init(path: String, baseSize: Int, glyphs: [UInt32]) {
        self.path = path
        self.baseSize = baseSize
        self.glyphs = glyphs
    }
}

public struct RenderTargetDescriptor: Sendable {
    public let size: PixelSize2D
    public let colorFormat: PixelFormat
    public let hasDepth: Bool

    public init(size: PixelSize2D, colorFormat: PixelFormat, hasDepth: Bool) {
        self.size = size
        self.colorFormat = colorFormat
        self.hasDepth = hasDepth
    }
}

public enum PrimitiveTopology: UInt8, Sendable {
    case triangles, lines, points
}

public enum MeshStorage: UInt8, Sendable {
    case immutable, streamed
}

public struct MeshDescriptor: Sendable {
    public let topology: PrimitiveTopology
    public let storage: MeshStorage
    public let vertexCount: Int
    public let indexCount: Int
    public let bounds: RenderBounds3

    public init(
        topology: PrimitiveTopology,
        storage: MeshStorage,
        vertexCount: Int,
        indexCount: Int,
        bounds: RenderBounds3
    ) {
        self.topology = topology
        self.storage = storage
        self.vertexCount = vertexCount
        self.indexCount = indexCount
        self.bounds = bounds
    }
}

public struct ModelImportFlags: OptionSet, Sendable {
    public let rawValue: UInt32

    public init(rawValue: UInt32) {
        self.rawValue = rawValue
    }

    public static let meshData = Self(rawValue: 1 << 0)
    public static let highQuality = Self(rawValue: 1 << 1)
}

public enum CompareFunction: UInt8, Hashable, Sendable {
    case always, lessEqual, greater, notEqual
}

public enum CullMode: UInt8, Hashable, Sendable {
    case none, front, back
}

public enum TransparencyMode: UInt8, Hashable, Sendable {
    case disabled, alpha
}

public enum BillboardMode: UInt8, Hashable, Sendable {
    case disabled, front, yAxis
}

public enum MaterialBlendMode: UInt8, Hashable, Sendable {
    case mix, additive, multiplied, premultipliedAlpha
}

public struct MaterialDescriptor: Hashable, Sendable {
    public let albedoTexture: TextureHandle
    public let albedoColor: RenderColor
    public let emissionTexture: TextureHandle
    public let emissionColor: RenderColor
    public let emissionEnergy: Float
    public let normalTexture: TextureHandle
    public let normalScale: Float
    public let ormTexture: TextureHandle
    public let occlusion: Float
    public let roughness: Float
    public let metalness: Float
    public let uvOffset: RenderVec2
    public let uvScale: RenderVec2
    public let alphaCutoff: Float
    public let depthCompare: CompareFunction
    public let cullMode: CullMode
    public let transparency: TransparencyMode
    public let billboard: BillboardMode
    public let blend: MaterialBlendMode
    public let unlit: Bool
    public let surfaceShader: SurfaceShaderHandle

    public init(
        albedoTexture: TextureHandle,
        albedoColor: RenderColor,
        emissionTexture: TextureHandle,
        emissionColor: RenderColor,
        emissionEnergy: Float,
        normalTexture: TextureHandle,
        normalScale: Float,
        ormTexture: TextureHandle,
        occlusion: Float,
        roughness: Float,
        metalness: Float,
        uvOffset: RenderVec2,
        uvScale: RenderVec2,
        alphaCutoff: Float,
        depthCompare: CompareFunction,
        cullMode: CullMode,
        transparency: TransparencyMode,
        billboard: BillboardMode,
        blend: MaterialBlendMode,
        unlit: Bool,
        surfaceShader: SurfaceShaderHandle
    ) {
        self.albedoTexture = albedoTexture
        self.albedoColor = albedoColor
        self.emissionTexture = emissionTexture
        self.emissionColor = emissionColor
        self.emissionEnergy = emissionEnergy
        self.normalTexture = normalTexture
        self.normalScale = normalScale
        self.ormTexture = ormTexture
        self.occlusion = occlusion
        self.roughness = roughness
        self.metalness = metalness
        self.uvOffset = uvOffset
        self.uvScale = uvScale
        self.alphaCutoff = alphaCutoff
        self.depthCompare = depthCompare
        self.cullMode = cullMode
        self.transparency = transparency
        self.billboard = billboard
        self.blend = blend
        self.unlit = unlit
        self.surfaceShader = surfaceShader
    }
}

public struct InstanceBufferDescriptor: Sendable {
    public let capacity: Int
    public let stride: Int
    public let includesColor: Bool
    public let customFloatCount: Int

    public init(capacity: Int, stride: Int, includesColor: Bool, customFloatCount: Int) {
        self.capacity = capacity
        self.stride = stride
        self.includesColor = includesColor
        self.customFloatCount = customFloatCount
    }
}

public struct AnimationPlayerDescriptor: Sendable {
    public let model: ModelHandle
    public let library: AnimationLibraryHandle
    public let initialAnimation: String

    public init(model: ModelHandle, library: AnimationLibraryHandle, initialAnimation: String) {
        self.model = model
        self.library = library
        self.initialAnimation = initialAnimation
    }
}

public struct AnimationTreeDescriptor: Sendable {
    public let model: ModelHandle
    public let library: AnimationLibraryHandle
    public let rootAnimation: String

    public init(model: ModelHandle, library: AnimationLibraryHandle, rootAnimation: String) {
        self.model = model
        self.library = library
        self.rootAnimation = rootAnimation
    }
}

public enum LightKind: UInt8, Sendable {
    case directional, spot
}

public struct LightDescriptor: Sendable {
    public let kind: LightKind
    public let active: Bool
    public let position: RenderVec3
    public let direction: RenderVec3
    public let color: RenderColor
    public let energy: Float
    public let range: Float
    public let castsShadow: Bool
    public let shadowSoftness: Float

    public init(
        kind: LightKind,
        active: Bool,
        position: RenderVec3,
        direction: RenderVec3,
        color: RenderColor,
        energy: Float,
        range: Float,
        castsShadow: Bool,
        shadowSoftness: Float
    ) {
        self.kind = kind
        self.active = active
        self.position = position
        self.direction = direction
        self.color = color
        self.energy = energy
        self.range = range
        self.castsShadow = castsShadow
        self.shadowSoftness = shadowSoftness
    }
}

public enum TonemapMode: UInt8, Sendable {
    case reinhard
}

public struct EnvironmentDescriptor: Sendable {
    public let backgroundColor: RenderColor
    public let backgroundEnergy: Float
    public let ambientColor: RenderColor
    public let ambientEnergy: Float
    public let ambientMap: AmbientMapHandle
    public let skyCubemap: CubemapHandle
    public let ssaoEnabled: Bool
    public let tonemap: TonemapMode
    public let tonemapWhite: Float

    public init(
        backgroundColor: RenderColor,
        backgroundEnergy: Float,
        ambientColor: RenderColor,
        ambientEnergy: Float,
        ambientMap: AmbientMapHandle,
        skyCubemap: CubemapHandle,
        ssaoEnabled: Bool,
        tonemap: TonemapMode,
        tonemapWhite: Float
    ) {
        self.backgroundColor = backgroundColor
        self.backgroundEnergy = backgroundEnergy
        self.ambientColor = ambientColor
        self.ambientEnergy = ambientEnergy
        self.ambientMap = ambientMap
        self.skyCubemap = skyCubemap
        self.ssaoEnabled = ssaoEnabled
        self.tonemap = tonemap
        self.tonemapWhite = tonemapWhite
    }

    public static let `default` = Self(
        backgroundColor: .black,
        backgroundEnergy: 0,
        ambientColor: .white,
        ambientEnergy: 0,
        ambientMap: .invalid,
        skyCubemap: .invalid,
        ssaoEnabled: false,
        tonemap: .reinhard,
        tonemapWhite: 1
    )
}

public enum ShaderParameterValue: Sendable {
    case float(Float)
    case vec2(RenderVec2)
    case vec3(RenderVec3)
    case vec4(RenderVec4)
    case texture(TextureHandle)
}

public struct SurfaceShaderDescriptor: Sendable {
    public let vertexPath: String?
    public let fragmentPath: String
    public let parameters: [String: ShaderParameterValue]

    public init(vertexPath: String?, fragmentPath: String, parameters: [String: ShaderParameterValue]) {
        self.vertexPath = vertexPath
        self.fragmentPath = fragmentPath
        self.parameters = parameters
    }
}

public struct ScreenShaderDescriptor: Sendable {
    public let fragmentPath: String
    public let parameters: [String: ShaderParameterValue]

    public init(fragmentPath: String, parameters: [String: ShaderParameterValue]) {
        self.fragmentPath = fragmentPath
        self.parameters = parameters
    }
}

public struct ProceduralSkyDescriptor: Sendable {
    public let color: RenderColor
    public let resolution: Int

    public init(color: RenderColor, resolution: Int) {
        self.color = color
        self.resolution = resolution
    }
}

public enum AntiAliasing: UInt8, Sendable {
    case off, smaaMedium, smaaHigh
}

public enum UpscaleMode: UInt8, Sendable {
    case linear, lanczos
}

public struct PostChainDescriptor: Sendable {
    public let screenShaders: [ScreenShaderHandle]
    public let casSharpness: Float
    public let tiltShiftEnabled: Bool

    public init(screenShaders: [ScreenShaderHandle], casSharpness: Float, tiltShiftEnabled: Bool) {
        self.screenShaders = screenShaders
        self.casSharpness = casSharpness
        self.tiltShiftEnabled = tiltShiftEnabled
    }

    public static let empty = Self(screenShaders: [], casSharpness: 0, tiltShiftEnabled: false)
}

@MainActor
public protocol RenderResourceBackend: AnyObject {
    func loadTexture(path: String) throws -> TextureHandle
    func createTexture(_ descriptor: TextureDescriptor, bytes: UnsafeRawBufferPointer?) throws -> TextureHandle
    func updateTexture(_ handle: TextureHandle, bytes: UnsafeRawBufferPointer) throws
    func destroyTexture(_ handle: TextureHandle)
    func loadFont(_ descriptor: FontDescriptor) throws -> FontHandle
    func destroyFont(_ handle: FontHandle)
    func createRenderTarget(_ descriptor: RenderTargetDescriptor) throws -> RenderTargetHandle
    func colorTexture(of target: RenderTargetHandle) throws -> TextureHandle
    func destroyRenderTarget(_ handle: RenderTargetHandle)
    func loadModel(path: String, flags: ModelImportFlags) throws -> ModelHandle
    func modelBounds(_ handle: ModelHandle) throws -> RenderBounds3
    func destroyModel(_ handle: ModelHandle)
    func createMesh(_ descriptor: MeshDescriptor, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws -> MeshHandle
    func updateMesh(_ handle: MeshHandle, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws
    func destroyMesh(_ handle: MeshHandle)
    func createMaterial(_ descriptor: MaterialDescriptor) throws -> MaterialHandle
    func updateMaterial(_ handle: MaterialHandle, descriptor: MaterialDescriptor) throws
    func destroyMaterial(_ handle: MaterialHandle)
    func createInstanceBuffer(_ descriptor: InstanceBufferDescriptor) throws -> InstanceBufferHandle
    func uploadInstances(_ handle: InstanceBufferHandle, instances: UnsafeRawBufferPointer) throws
    func destroyInstanceBuffer(_ handle: InstanceBufferHandle)
    func loadAnimationLibrary(path: String) throws -> AnimationLibraryHandle
    func destroyAnimationLibrary(_ handle: AnimationLibraryHandle)
    func createAnimationPlayer(_ descriptor: AnimationPlayerDescriptor) throws -> AnimationPlayerHandle
    func updateAnimationPlayer(_ handle: AnimationPlayerHandle, deltaSeconds: Float) throws
    func setAnimation(_ handle: AnimationPlayerHandle, name: String, loop: Bool, time: Float?) throws
    func destroyAnimationPlayer(_ handle: AnimationPlayerHandle)
    func createAnimationTree(_ descriptor: AnimationTreeDescriptor) throws -> AnimationTreeHandle
    func updateAnimationTree(_ handle: AnimationTreeHandle, deltaSeconds: Float) throws
    func setAnimationTreeParameter(_ handle: AnimationTreeHandle, name: String, value: Float) throws
    func destroyAnimationTree(_ handle: AnimationTreeHandle)
    func createLight(_ descriptor: LightDescriptor) throws -> LightHandle
    func updateLight(_ handle: LightHandle, descriptor: LightDescriptor) throws
    func destroyLight(_ handle: LightHandle)
    func createProceduralSky(_ descriptor: ProceduralSkyDescriptor) throws -> CubemapHandle
    func createAmbientMap(from cubemap: CubemapHandle) throws -> AmbientMapHandle
    func destroyCubemap(_ handle: CubemapHandle)
    func destroyAmbientMap(_ handle: AmbientMapHandle)
    func createSurfaceShader(_ descriptor: SurfaceShaderDescriptor) throws -> SurfaceShaderHandle
    func updateSurfaceShader(_ handle: SurfaceShaderHandle, parameter: String, value: ShaderParameterValue) throws
    func destroySurfaceShader(_ handle: SurfaceShaderHandle)
    func createScreenShader(_ descriptor: ScreenShaderDescriptor) throws -> ScreenShaderHandle
    func updateScreenShader(_ handle: ScreenShaderHandle, parameter: String, value: ShaderParameterValue) throws
    func destroyScreenShader(_ handle: ScreenShaderHandle)
}
