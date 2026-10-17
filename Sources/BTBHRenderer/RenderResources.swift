public enum PixelFormat: UInt8, Sendable {
    case r8, rg8, rgba8, rgba8SRGB, rgba16Float, depth32Float
}

public enum TextureColorSpace: UInt8, Sendable, Equatable {
    case linear
    case sRGB
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

public struct TextureLoadDescriptor: Sendable {
    public let path: String
    public let generateMipmaps: Bool
    public let filter: TextureFilter?
    public let wrap: TextureWrap?
    public let colorSpace: TextureColorSpace

    public init(
        path: String,
        generateMipmaps: Bool = false,
        filter: TextureFilter? = nil,
        wrap: TextureWrap? = nil,
        colorSpace: TextureColorSpace = .linear
    ) {
        self.path = path
        self.generateMipmaps = generateMipmaps
        self.filter = filter
        self.wrap = wrap
        self.colorSpace = colorSpace
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
    public let filter: TextureFilter

    public init(
        size: PixelSize2D,
        colorFormat: PixelFormat,
        hasDepth: Bool,
        filter: TextureFilter = .bilinear
    ) {
        self.size = size
        self.colorFormat = colorFormat
        self.hasDepth = hasDepth
        self.filter = filter
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
    /// Preserve model geometry, material slots, and scalar material values,
    /// but do not resolve or upload texture references embedded in the model.
    /// Use this when the caller supplies complete material overrides.
    public static let ignoreMaterialTextures = Self(rawValue: 1 << 2)
}

public enum ModelFootprintKind: UInt8, Hashable, Sendable {
    case bounds
    case convexHull
    case concaveHull
    case raster
}

public struct ModelMetadata: Hashable, Sendable {
    public let bounds: RenderBounds3
    public let meshCount: Int
    public let materialCount: Int
    public let meshMaterialSlots: [Int]
    public let baseFootprint: [RenderVec2]
    public let footprintKind: ModelFootprintKind

    public init(
        bounds: RenderBounds3,
        meshCount: Int,
        materialCount: Int,
        meshMaterialSlots: [Int],
        baseFootprint: [RenderVec2] = [],
        footprintKind: ModelFootprintKind = .bounds
    ) {
        self.bounds = bounds
        self.meshCount = meshCount
        self.materialCount = materialCount
        self.meshMaterialSlots = meshMaterialSlots
        self.baseFootprint = baseFootprint
        self.footprintKind = footprintKind
    }
}

/// Backend-neutral vertex payload consumed by `RenderResourceBackend` mesh APIs.
/// Index buffers use 32-bit unsigned indices.
public struct RenderVertex: Hashable, Sendable {
    public var position: RenderVec3
    public var textureCoordinate: RenderVec2
    public var normal: RenderVec3
    public var tangent: RenderVec4
    public var color: RenderColor

    public init(
        position: RenderVec3,
        textureCoordinate: RenderVec2,
        normal: RenderVec3,
        tangent: RenderVec4,
        color: RenderColor
    ) {
        self.position = position
        self.textureCoordinate = textureCoordinate
        self.normal = normal
        self.tangent = tangent
        self.color = color
    }
}

public enum CompareFunction: UInt8, Hashable, Sendable {
    case always = 0
    case lessEqual = 1
    case greater = 2
    case notEqual = 3
    case less = 4
    case equal = 5
    case greaterEqual = 6
    case never = 7
}

public enum StencilOperation: UInt8, Hashable, Sendable {
    case keep, zero, replace, incrementClamp, decrementClamp
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

/// Procedural world-space surface detail drawn by the scene shader without textures: an albedo
/// variation and a bump from value noise. It fades out where a noise cell gets smaller than a few
/// pixels, so distant surfaces do not shimmer.
public struct SurfaceDetail: Hashable, Sendable {
    public enum Pattern: UInt8, Hashable, Sendable {
        case none
        /// Fine grain and broad stains on the ground plane (asphalt, paving, sand, soil).
        case ground
        /// Ground grain plus a slow drift between warm and cool greens (lawns).
        case grass
        /// Triplanar grain for walls and roofs, darkened toward the ground.
        case facade
        /// World-aligned slabs in running bond, each a shade lighter or darker, with recessed
        /// joints (footpaths, sidewalks, plazas). A slab is one cell wide and two long.
        case paving
        /// Flat glass (windows, curtain walls): a stronger, sharper sky reflection, the glass's own colour
        /// half as strong, and on walls a pane per cell, each a shade apart within thin frames, with
        /// `litShare` of them lit from inside at night (`EnvironmentDescriptor.windowGlow`). No bump.
        case glass
        /// A lamp's lens (car head and tail lights, beacons): its own colour shines at night, as strongly as the
        /// windows' light (`EnvironmentDescriptor.windowGlow`) rises. Nothing else.
        case lamp
    }

    public var pattern: Pattern
    /// The albedo varies by up to plus or minus this fraction.
    public var albedoVariation: Float
    /// Bump height in render units.
    public var bumpHeight: Float
    /// Noise cells per render unit.
    public var frequency: Float
    /// Glass only: the share of panes lit from inside at night (0...1).
    public var litShare: Float

    public init(pattern: Pattern, albedoVariation: Float, bumpHeight: Float, frequency: Float, litShare: Float = 0) {
        self.pattern = pattern
        self.albedoVariation = albedoVariation
        self.bumpHeight = bumpHeight
        self.frequency = frequency
        self.litShare = litShare
    }

    public static let none = Self(pattern: .none, albedoVariation: 0, bumpHeight: 0, frequency: 1)

    /// Glass changes the surface's reflection even without variation or lit panes, and a lens shines at night.
    public var isEnabled: Bool {
        pattern != .none && frequency > 0 && frequency.isFinite
            && albedoVariation.isFinite && bumpHeight.isFinite && litShare.isFinite
            && (albedoVariation != 0 || bumpHeight != 0 || pattern == .glass || pattern == .lamp)
    }

    /// A lamp's lens (`Pattern.lamp`).
    public static let lampLens = Self(pattern: .lamp, albedoVariation: 0, bumpHeight: 0, frequency: 1)
}

/// Cloud shadows: a layer of soft clouds `height` render units up, drifted `offset` render units by the wind so
/// far, takes up to `strength` (0...1) of the sun's light where its shadow falls. `coverage` is the sky's share
/// under clouds (0...1), `softness` their edges' width, `size` a cloud's width (render units).
public struct CloudShadowDescriptor: Hashable, Sendable {
    public var strength: Float
    public var coverage: Float
    public var softness: Float
    public var size: Float
    public var height: Float
    public var offset: RenderVec2

    public init(strength: Float, coverage: Float, softness: Float, size: Float, height: Float, offset: RenderVec2) {
        self.strength = strength
        self.coverage = coverage
        self.softness = softness
        self.size = size
        self.height = height
        self.offset = offset
    }

    public static let off = Self(strength: 0, coverage: 0, softness: 0, size: 1, height: 0, offset: .zero)

    /// Off unless every value is finite, the strength and size positive.
    public var isEnabled: Bool {
        strength > 0 && size > 0 && strength.isFinite && size.isFinite && coverage.isFinite && softness.isFinite
            && height.isFinite && offset.x.isFinite && offset.y.isFinite
    }

    /// Strength and coverage clamped to 0...1, the softness never negative; off when not enabled.
    var sanitized: Self {
        guard isEnabled else { return .off }
        return Self(strength: min(strength, 1), coverage: min(max(coverage, 0), 1), softness: max(softness, 0),
                    size: size, height: height, offset: offset)
    }
}

/// Screen-space reflections: glossy surfaces (no rougher than `maxRoughness`) reflect the opaque scene their
/// reflected ray meets within `maxDistance` render units, passing behind a surface by less than `thickness`.
/// `intensity` multiplies their Fresnel reflectance (0 off).
public struct ScreenSpaceReflectionDescriptor: Hashable, Sendable {
    public var intensity: Float
    public var maxRoughness: Float
    public var maxDistance: Float
    public var thickness: Float

    public init(intensity: Float, maxRoughness: Float, maxDistance: Float, thickness: Float) {
        self.intensity = intensity
        self.maxRoughness = maxRoughness
        self.maxDistance = maxDistance
        self.thickness = thickness
    }

    public static let off = Self(intensity: 0, maxRoughness: 0, maxDistance: 0, thickness: 0)

    /// Off unless the intensity, roughness and reach are positive and every value finite.
    public var isEnabled: Bool {
        intensity > 0 && maxRoughness > 0 && maxDistance > 0 && intensity.isFinite && maxRoughness.isFinite
            && maxDistance.isFinite && thickness.isFinite
    }

    /// The roughness clamped to 1 and the thickness never negative; off when not enabled.
    var sanitized: Self {
        guard isEnabled else { return .off }
        return Self(intensity: intensity, maxRoughness: min(maxRoughness, 1), maxDistance: maxDistance,
                    thickness: max(thickness, 0))
    }
}

/// Ambient occlusion (`EnvironmentDescriptor.ssaoEnabled`): occluders within `radius` render units darken a
/// surface by up to `intensity`; with the radiance cascades the occlusion takes `contactStrength` (0...1) of the
/// indirect light, and direct light (the sun's, the spot's) keeps `directShare` (0...1) of that.
public struct AmbientOcclusionDescriptor: Hashable, Sendable {
    public var radius: Float
    public var intensity: Float
    public var contactStrength: Float
    public var directShare: Float

    public init(radius: Float, intensity: Float, contactStrength: Float, directShare: Float) {
        self.radius = radius
        self.intensity = intensity
        self.contactStrength = contactStrength
        self.directShare = directShare
    }

    /// The renderer's occlusion until 2026-10-17: a one-unit radius at 0.4.
    public static let standard = Self(radius: 1, intensity: 0.4, contactStrength: 0.75, directShare: 0.2)

    /// Every value finite and in its range; the standard occlusion's value stands in for one that is not.
    var sanitized: Self {
        let standard = Self.standard
        func unit(_ value: Float, _ fallback: Float) -> Float { value.isFinite ? min(max(value, 0), 1) : fallback }
        return Self(
            radius: radius.isFinite && radius > 0 ? radius : standard.radius,
            intensity: intensity.isFinite ? max(intensity, 0) : standard.intensity,
            contactStrength: unit(contactStrength, standard.contactStrength),
            directShare: unit(directShare, standard.directShare)
        )
    }
}

public struct MaterialDescriptor: Hashable, Sendable {
    public var albedoTexture: TextureHandle
    public var albedoColor: RenderColor
    public var emissionTexture: TextureHandle
    public var emissionColor: RenderColor
    public var emissionEnergy: Float
    public var normalTexture: TextureHandle
    public var normalScale: Float
    public var ormTexture: TextureHandle
    public var occlusion: Float
    public var roughness: Float
    public var metalness: Float
    public var uvOffset: RenderVec2
    public var uvScale: RenderVec2
    public var alphaCutoff: Float
    public var depthCompare: CompareFunction
    public var depthOffsetFactor: Float
    public var depthOffsetUnits: Float
    public var depthRangeNear: Float
    public var depthRangeFar: Float
    public var stencilCompare: CompareFunction
    public var stencilReference: UInt8
    public var stencilMask: UInt8
    public var stencilFail: StencilOperation
    public var stencilDepthFail: StencilOperation
    public var stencilPass: StencilOperation
    public var cullMode: CullMode
    public var transparency: TransparencyMode
    public var billboard: BillboardMode
    public var blend: MaterialBlendMode
    public var unlit: Bool
    public var surfaceShader: SurfaceShaderHandle
    /// false keeps the geometry out of the directional and spot shadow maps (small fast objects such
    /// as projectiles, which draw their own ground shadow).
    public var castsShadow: Bool
    /// Procedural surface detail (off by default).
    public var detail: SurfaceDetail

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
        depthOffsetFactor: Float = 0,
        depthOffsetUnits: Float = 0,
        depthRangeNear: Float = 0,
        depthRangeFar: Float = 1,
        stencilCompare: CompareFunction = .always,
        stencilReference: UInt8 = 0,
        stencilMask: UInt8 = 0xFF,
        stencilFail: StencilOperation = .keep,
        stencilDepthFail: StencilOperation = .keep,
        stencilPass: StencilOperation = .replace,
        cullMode: CullMode,
        transparency: TransparencyMode,
        billboard: BillboardMode,
        blend: MaterialBlendMode,
        unlit: Bool,
        surfaceShader: SurfaceShaderHandle,
        castsShadow: Bool = true,
        detail: SurfaceDetail = .none
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
        self.depthOffsetFactor = depthOffsetFactor
        self.depthOffsetUnits = depthOffsetUnits
        self.depthRangeNear = depthRangeNear
        self.depthRangeFar = depthRangeFar
        self.stencilCompare = stencilCompare
        self.stencilReference = stencilReference
        self.stencilMask = stencilMask
        self.stencilFail = stencilFail
        self.stencilDepthFail = stencilDepthFail
        self.stencilPass = stencilPass
        self.cullMode = cullMode
        self.transparency = transparency
        self.billboard = billboard
        self.blend = blend
        self.unlit = unlit
        self.surfaceShader = surfaceShader
        self.castsShadow = castsShadow
        self.detail = detail
    }

    public static let `default` = Self(
        albedoTexture: .invalid,
        albedoColor: .white,
        emissionTexture: .invalid,
        emissionColor: .white,
        emissionEnergy: 0,
        normalTexture: .invalid,
        normalScale: 1,
        ormTexture: .invalid,
        occlusion: 1,
        roughness: 1,
        metalness: 0,
        uvOffset: .zero,
        uvScale: .init(x: 1, y: 1),
        alphaCutoff: 0.01,
        depthCompare: .less,
        cullMode: .back,
        transparency: .disabled,
        billboard: .disabled,
        blend: .mix,
        unlit: false,
        surfaceShader: .invalid
    )
}

/// Backend-neutral edits applied to a material copied from a model slot.
///
/// Unspecified fields preserve the imported material. The returned material
/// remains an independently managed `MaterialHandle`.
public struct ModelMaterialPatch: Hashable, Sendable {
    public var albedoTexture: TextureHandle?
    public var albedoColor: RenderColor?
    public var normalTexture: TextureHandle?
    public var ormTexture: TextureHandle?
    public var clearEmissionTexture: Bool
    public var emissionColor: RenderColor?
    public var emissionEnergy: Float?
    public var minimumEmissionEnergy: Float?
    public var stencilReference: UInt8?
    public var cullMode: CullMode?
    public var surfaceShader: SurfaceShaderHandle?
    /// nil keeps the imported material's shadow casting.
    public var castsShadow: Bool?
    /// nil keeps the imported material's transparency; `.alpha` blends the model by its instance alpha.
    public var transparency: TransparencyMode?
    /// nil keeps the imported material's surface detail.
    public var detail: SurfaceDetail?
    /// nil keeps the imported material's depth test. `.lessEqual` lets a second draw of the same
    /// model at the same transforms (a tint over its body) pass where the body already wrote depth.
    public var depthCompare: CompareFunction?

    public init(
        albedoTexture: TextureHandle? = nil,
        albedoColor: RenderColor? = nil,
        normalTexture: TextureHandle? = nil,
        ormTexture: TextureHandle? = nil,
        clearEmissionTexture: Bool = false,
        emissionColor: RenderColor? = nil,
        emissionEnergy: Float? = nil,
        minimumEmissionEnergy: Float? = nil,
        stencilReference: UInt8? = nil,
        cullMode: CullMode? = nil,
        surfaceShader: SurfaceShaderHandle? = nil,
        castsShadow: Bool? = nil,
        transparency: TransparencyMode? = nil,
        detail: SurfaceDetail? = nil,
        depthCompare: CompareFunction? = nil
    ) {
        self.albedoTexture = albedoTexture
        self.albedoColor = albedoColor
        self.normalTexture = normalTexture
        self.ormTexture = ormTexture
        self.clearEmissionTexture = clearEmissionTexture
        self.emissionColor = emissionColor
        self.emissionEnergy = emissionEnergy
        self.minimumEmissionEnergy = minimumEmissionEnergy
        self.stencilReference = stencilReference
        self.cullMode = cullMode
        self.surfaceShader = surfaceShader
        self.castsShadow = castsShadow
        self.transparency = transparency
        self.detail = detail
        self.depthCompare = depthCompare
    }

    public static let identity = Self()
}

public enum RenderResourceBackendError: Error, Equatable, Sendable {
    case unsupportedOperation(String)
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

public struct AnimationClipDescriptor: Hashable, Sendable {
    public let name: String
    public let animationIndex: Int
    public let loop: Bool
    public let speed: Float

    public init(name: String, animationIndex: Int, loop: Bool, speed: Float) {
        self.name = name
        self.animationIndex = animationIndex
        self.loop = loop
        self.speed = speed
    }
}

public struct AnimationSwitchDescriptor: Hashable, Sendable {
    public let name: String
    public let inputs: [String]
    public let activeInput: Int
    public let crossfadeSeconds: Float
    public let synchronized: Bool

    public init(
        name: String,
        inputs: [String],
        activeInput: Int,
        crossfadeSeconds: Float,
        synchronized: Bool
    ) {
        self.name = name
        self.inputs = inputs
        self.activeInput = activeInput
        self.crossfadeSeconds = crossfadeSeconds
        self.synchronized = synchronized
    }
}

public struct AnimationTreeDescriptor: Sendable {
    public let model: ModelHandle
    public let library: AnimationLibraryHandle
    public let clips: [AnimationClipDescriptor]
    public let switches: [AnimationSwitchDescriptor]
    public let rootNode: String
    public let rootBone: Int?

    public var rootAnimation: String { rootNode }

    public init(
        model: ModelHandle,
        library: AnimationLibraryHandle,
        clips: [AnimationClipDescriptor],
        switches: [AnimationSwitchDescriptor],
        rootNode: String,
        rootBone: Int?
    ) {
        self.model = model
        self.library = library
        self.clips = clips
        self.switches = switches
        self.rootNode = rootNode
        self.rootBone = rootBone
    }

    public init(model: ModelHandle, library: AnimationLibraryHandle, rootAnimation: String) {
        self.model = model
        self.library = library
        clips = []
        switches = []
        rootNode = rootAnimation
        rootBone = nil
    }
}

public enum LightKind: UInt8, Sendable {
    case directional, spot, omni
}

/// Where a directional light spends its shadow map: a sphere of `radius` around the camera
/// target, moved `forwardShift` along the camera's forward direction flattened onto the ground
/// (a tilted view sees more ground ahead of its target than behind it), instead of the camera
/// frustum up to the light's `range`. The centre snaps to whole shadow texels, so shadows stay
/// still while the camera follows something.
public struct ShadowFocus: Sendable, Equatable {
    public let radius: Float
    public let forwardShift: Float

    public init(radius: Float, forwardShift: Float = 0) {
        self.radius = radius
        self.forwardShift = forwardShift
    }
}

public struct LightDescriptor: Sendable {
    public let kind: LightKind
    public let active: Bool
    public let position: RenderVec3
    public let direction: RenderVec3
    public let color: RenderColor
    public let energy: Float
    /// Energy contributed to Radiance Cascades indirect lighting.
    /// This is independent from direct-light `energy`.
    public let indirectEnergy: Float
    public let range: Float
    public let castsShadow: Bool
    public let shadowSoftness: Float
    /// Requested number of presented frames between shadow-map updates.
    ///
    /// A value of one requests continuous updates. Backends may update more
    /// frequently, but must not retain a shadow map beyond this frame budget.
    public let shadowUpdatePeriodFrames: UInt32
    /// Directional lights only: when set, the shadow covers this sphere and `range` is unused.
    public let shadowFocus: ShadowFocus?
    /// Spot lights only: the share (0-1) of the sun, fill and sky light that the spot's shadow also
    /// takes where its occluder lies within `occluderShadowReach` of the light. A lamp carried by a
    /// character then shadows the ground with the character: its own light alone is a small part of
    /// daylight. Farther occluders keep the classic spot shadow. 0 turns it off.
    public let occluderShadowStrength: Float
    /// Spot lights only: how far from the light an occluder counts for `occluderShadowStrength`.
    public let occluderShadowReach: Float

    public init(
        kind: LightKind,
        active: Bool,
        position: RenderVec3,
        direction: RenderVec3,
        color: RenderColor,
        energy: Float,
        range: Float,
        castsShadow: Bool,
        shadowSoftness: Float,
        shadowUpdatePeriodFrames: UInt32 = 1,
        indirectEnergy: Float = 0,
        shadowFocus: ShadowFocus? = nil,
        occluderShadowStrength: Float = 0,
        occluderShadowReach: Float = 0
    ) {
        self.kind = kind
        self.active = active
        self.position = position
        self.direction = direction
        self.color = color
        self.energy = energy
        self.indirectEnergy = indirectEnergy
        self.range = range
        self.castsShadow = castsShadow
        self.shadowSoftness = shadowSoftness
        self.shadowUpdatePeriodFrames = shadowUpdatePeriodFrames
        self.shadowFocus = shadowFocus
        self.occluderShadowStrength = occluderShadowStrength
        self.occluderShadowReach = occluderShadowReach
    }
}

public enum TonemapMode: UInt8, Sendable, Equatable {
    case reinhard
    case pbrNeutral
    case aces
}

public enum RadianceCascadesTier: UInt8, Sendable, Equatable {
    case off
    case low
    case high
    case auto
}

/// Holographic Radiance Cascades are the only implementation. The raw value
/// stays 1 so frame telemetry reads the same as before classic RC was removed.
public enum RadianceCascadesBackend: UInt8, Sendable, Equatable {
    case holographic = 1
}

public enum RadianceCascadesDebugView: UInt8, Sendable, Equatable {
    case none
    case cascade0
    case cascade1
    case cascade2
    case cascade3
    case hitDistance
    case confidence
    case finalIndirect
    case fallbackReason
    case directionalSource
    case directLight
}

public enum RadianceCascadesFallbackReason: UInt8, Sendable, Equatable {
    case none
    case disabled
    case capabilityUnavailable
    case shaderUnavailable
    case pipelineCreationFailed
    case resourceAllocationFailed
    case dispatchFailed
    case invalidConfiguration
}

public struct RadianceCascadesPassTelemetry: Sendable, Equatable {
    public let cpuPrepareMilliseconds: Double
    public let cpuPipelinePrepareMilliseconds: Double
    public let cpuResourcePrepareMilliseconds: Double
    public let cpuTraceEncodeMilliseconds: Double
    public let cpuMergeUpEncodeMilliseconds: Double
    public let cpuMergeDownEncodeMilliseconds: Double
    public let cpuReprojectEncodeMilliseconds: Double
    public let cpuFinishEncodeMilliseconds: Double
    public let cpuBlurEncodeMilliseconds: Double
    public let cpuResolveEncodeMilliseconds: Double
    public let cpuReconstructEncodeMilliseconds: Double
    public let cpuComposeEncodeMilliseconds: Double
    public let traceDispatchCount: Int
    public let mergeUpDispatchCount: Int
    public let mergeDownDispatchCount: Int
    public let reprojectDispatchCount: Int
    public let finishDispatchCount: Int
    public let blurDispatchCount: Int
    public let resolveDispatchCount: Int
    public let reconstructDispatchCount: Int
    public let composeDrawCount: Int
    public let temporalHistoryValid: Bool
    public let temporalPhase: Int
    public let freshQuadrantCount: Int

    public init(
        cpuPrepareMilliseconds: Double = 0,
        cpuPipelinePrepareMilliseconds: Double = 0,
        cpuResourcePrepareMilliseconds: Double = 0,
        cpuTraceEncodeMilliseconds: Double = 0,
        cpuMergeUpEncodeMilliseconds: Double = 0,
        cpuMergeDownEncodeMilliseconds: Double = 0,
        cpuReprojectEncodeMilliseconds: Double = 0,
        cpuFinishEncodeMilliseconds: Double = 0,
        cpuBlurEncodeMilliseconds: Double = 0,
        cpuResolveEncodeMilliseconds: Double = 0,
        cpuReconstructEncodeMilliseconds: Double = 0,
        cpuComposeEncodeMilliseconds: Double = 0,
        traceDispatchCount: Int = 0,
        mergeUpDispatchCount: Int = 0,
        mergeDownDispatchCount: Int = 0,
        reprojectDispatchCount: Int = 0,
        finishDispatchCount: Int = 0,
        blurDispatchCount: Int = 0,
        resolveDispatchCount: Int = 0,
        reconstructDispatchCount: Int = 0,
        composeDrawCount: Int = 0,
        temporalHistoryValid: Bool = false,
        temporalPhase: Int = 0,
        freshQuadrantCount: Int = 0
    ) {
        self.cpuPrepareMilliseconds = cpuPrepareMilliseconds
        self.cpuPipelinePrepareMilliseconds =
            cpuPipelinePrepareMilliseconds
        self.cpuResourcePrepareMilliseconds =
            cpuResourcePrepareMilliseconds
        self.cpuTraceEncodeMilliseconds = cpuTraceEncodeMilliseconds
        self.cpuMergeUpEncodeMilliseconds = cpuMergeUpEncodeMilliseconds
        self.cpuMergeDownEncodeMilliseconds = cpuMergeDownEncodeMilliseconds
        self.cpuReprojectEncodeMilliseconds =
            cpuReprojectEncodeMilliseconds
        self.cpuFinishEncodeMilliseconds = cpuFinishEncodeMilliseconds
        self.cpuBlurEncodeMilliseconds = cpuBlurEncodeMilliseconds
        self.cpuResolveEncodeMilliseconds = cpuResolveEncodeMilliseconds
        self.cpuReconstructEncodeMilliseconds =
            cpuReconstructEncodeMilliseconds
        self.cpuComposeEncodeMilliseconds = cpuComposeEncodeMilliseconds
        self.traceDispatchCount = traceDispatchCount
        self.mergeUpDispatchCount = mergeUpDispatchCount
        self.mergeDownDispatchCount = mergeDownDispatchCount
        self.reprojectDispatchCount = reprojectDispatchCount
        self.finishDispatchCount = finishDispatchCount
        self.blurDispatchCount = blurDispatchCount
        self.resolveDispatchCount = resolveDispatchCount
        self.reconstructDispatchCount = reconstructDispatchCount
        self.composeDrawCount = composeDrawCount
        self.temporalHistoryValid = temporalHistoryValid
        self.temporalPhase = temporalPhase
        self.freshQuadrantCount = freshQuadrantCount
    }

    public static let zero = Self()
}

public struct RadianceCascadesStatus: Sendable, Equatable {
    public let requestedTier: RadianceCascadesTier
    public let activeTier: RadianceCascadesTier
    public let pendingTier: RadianceCascadesTier
    public let requestedBackend: RadianceCascadesBackend
    public let activeBackend: RadianceCascadesBackend
    public let fallbackReason: RadianceCascadesFallbackReason
    public let internalSize: PixelSize2D
    public let generation: UInt32
    public let sampleCount: Int
    public let cpuEncodeMilliseconds: Double
    public let passTelemetry: RadianceCascadesPassTelemetry

    public init(
        requestedTier: RadianceCascadesTier,
        activeTier: RadianceCascadesTier,
        pendingTier: RadianceCascadesTier,
        requestedBackend: RadianceCascadesBackend = .holographic,
        activeBackend: RadianceCascadesBackend = .holographic,
        fallbackReason: RadianceCascadesFallbackReason,
        internalSize: PixelSize2D,
        generation: UInt32,
        sampleCount: Int,
        cpuEncodeMilliseconds: Double,
        passTelemetry: RadianceCascadesPassTelemetry = .zero
    ) {
        self.requestedTier = requestedTier
        self.activeTier = activeTier
        self.pendingTier = pendingTier
        self.requestedBackend = requestedBackend
        self.activeBackend = activeBackend
        self.fallbackReason = fallbackReason
        self.internalSize = internalSize
        self.generation = generation
        self.sampleCount = sampleCount
        self.cpuEncodeMilliseconds = cpuEncodeMilliseconds
        self.passTelemetry = passTelemetry
    }
}

public enum RadianceEmitterShape: Int, Sendable, Equatable {
    case disk
    case rectangle
    case segment
}

/// Semantic ownership used by renderer-level energy-density policies.
public enum RadianceEmitterClass: Int, Sendable, Equatable, CaseIterable {
    case hero
    case world
    case pickup
    case transient
}

public struct RadianceEmitter: Sendable, Equatable {
    public let stableID: UInt64
    public let position: RenderVec3
    public let color: RenderColor
    public let energy: Float
    /// Analytic energy used by the RC-native occluded direct-light field.
    /// Zero keeps a source indirect-only.
    public let directEnergy: Float
    /// Physical source radius. Illumination range is derived by RC transport.
    public let radius: Float
    /// Vertical source extent in the 2.5D world field. For a directional
    /// emitter with spot cutoffs, this is the cone length.
    public let height: Float
    public let shape: RadianceEmitterShape
    public let lightClass: RadianceEmitterClass
    /// Direction in which the source emits. The default cutoffs below make
    /// this direction irrelevant and preserve the original omni behavior.
    public let direction: RenderVec3
    public let innerCutoff: Float
    public let outerCutoff: Float

    public init(
        stableID: UInt64 = 0,
        position: RenderVec3,
        color: RenderColor,
        energy: Float,
        directEnergy: Float = 0,
        radius: Float,
        height: Float = 0,
        shape: RadianceEmitterShape = .disk,
        lightClass: RadianceEmitterClass = .world,
        direction: RenderVec3 = .forward,
        innerCutoff: Float = -1,
        outerCutoff: Float = -2
    ) {
        self.stableID = stableID
        self.position = position
        self.color = color
        self.energy = energy
        self.directEnergy = directEnergy
        self.radius = radius
        self.height = height
        self.shape = shape
        self.lightClass = lightClass
        self.direction = direction
        self.innerCutoff = innerCutoff
        self.outerCutoff = outerCutoff
    }

    @available(*, deprecated, message: "Use radius; RC transport has no analytic light range")
    public init(
        position: RenderVec3,
        color: RenderColor,
        energy: Float,
        range: Float,
        direction: RenderVec3 = .forward,
        innerCutoff: Float = -1,
        outerCutoff: Float = -2
    ) {
        self.init(
            position: position,
            color: color,
            energy: energy,
            directEnergy: 0,
            radius: range,
            lightClass: .world,
            direction: direction,
            innerCutoff: innerCutoff,
            outerCutoff: outerCutoff
        )
    }

    @available(*, deprecated, renamed: "radius")
    public var range: Float { radius }
}

@available(*, deprecated, renamed: "RadianceEmitter")
public typealias RadianceCascadesLightSource = RadianceEmitter

/// A top-down 2.5D visibility field consumed by Radiance Cascades.
///
/// Each cell stores two interleaved floats: signed horizontal distance to the
/// nearest occluder and the occluder's top height in world space.
public struct RadianceCascadesOcclusionCascade: Sendable, Equatable {
    public let originWorldXZ: RenderVec2
    public let cellSize: Float
    public let width: Int
    public let height: Int
    public let revision: UInt64
    public let samples: [Float]

    public init(
        originWorldXZ: RenderVec2,
        cellSize: Float,
        width: Int,
        height: Int,
        revision: UInt64,
        samples: [Float]
    ) {
        self.originWorldXZ = originWorldXZ
        self.cellSize = cellSize
        self.width = width
        self.height = height
        self.revision = revision
        self.samples = samples
    }
}

public struct RadianceCascadesDescriptor: Sendable, Equatable {
    public static let maximumOcclusionCascades = 3

    public let tier: RadianceCascadesTier
    public let backend: RadianceCascadesBackend
    public let debugView: RadianceCascadesDebugView
    public let autoFrameBudgetMilliseconds: Float
    public let indirectIntensity: Float
    public let emitters: [RadianceEmitter]
    public let occlusionCascades: [RadianceCascadesOcclusionCascade]
    public let allowAutoTransition: Bool
    public let invalidate: Bool

    public init(
        tier: RadianceCascadesTier,
        backend: RadianceCascadesBackend = .holographic,
        debugView: RadianceCascadesDebugView = .none,
        autoFrameBudgetMilliseconds: Float = 1_000 / 60,
        indirectIntensity: Float = 0.90,
        emitters: [RadianceEmitter] = [],
        occlusionCascades: [RadianceCascadesOcclusionCascade] = [],
        allowAutoTransition: Bool = false,
        invalidate: Bool = false
    ) {
        self.tier = tier
        self.backend = backend
        self.debugView = debugView
        self.autoFrameBudgetMilliseconds = autoFrameBudgetMilliseconds
        self.indirectIntensity = indirectIntensity
        self.emitters = emitters
        self.occlusionCascades = occlusionCascades
        self.allowAutoTransition = allowAutoTransition
        self.invalidate = invalidate
    }

    @available(*, deprecated, message: "Use emitters")
    public init(
        tier: RadianceCascadesTier,
        debugView: RadianceCascadesDebugView = .none,
        autoFrameBudgetMilliseconds: Float = 1_000 / 60,
        indirectIntensity: Float = 0.90,
        lightSources: [RadianceCascadesLightSource],
        occlusionCascades: [RadianceCascadesOcclusionCascade] = [],
        allowAutoTransition: Bool = false,
        invalidate: Bool = false
    ) {
        self.init(
            tier: tier,
            backend: .holographic,
            debugView: debugView,
            autoFrameBudgetMilliseconds: autoFrameBudgetMilliseconds,
            indirectIntensity: indirectIntensity,
            emitters: lightSources,
            occlusionCascades: occlusionCascades,
            allowAutoTransition: allowAutoTransition,
            invalidate: invalidate
        )
    }

    @available(*, deprecated, renamed: "emitters")
    public var lightSources: [RadianceCascadesLightSource] { emitters }

    public static let off = Self(tier: .off)
}

/// The world XZ rectangle a fog closes over: across `fadeDistance` inside its border the fog
/// rises to full, so the end of the world dissolves into the fog colour instead of a hard line.
/// Surfaces nearer than `clearDistance` (view depth) keep a crisp edge, and the fade ramps in over
/// the next `fadeDistance`, so the play area around the camera target stays readable.
public struct FogEdgeDescriptor: Sendable, Equatable {
    public let minimumX: Float
    public let minimumZ: Float
    public let maximumX: Float
    public let maximumZ: Float
    public let fadeDistance: Float
    public let clearDistance: Float

    public init(
        minimumX: Float,
        minimumZ: Float,
        maximumX: Float,
        maximumZ: Float,
        fadeDistance: Float,
        clearDistance: Float = 0
    ) {
        self.minimumX = minimumX
        self.minimumZ = minimumZ
        self.maximumX = maximumX
        self.maximumZ = maximumZ
        self.fadeDistance = fadeDistance
        self.clearDistance = clearDistance
    }
}

/// Distance fog toward a horizon colour, applied to the HDR image before grading.
///
/// The amount is `1 - exp(-((viewDistance - startDistance) * density)^2)`, capped at
/// `maximumOpacity`; the cleared background counts as infinitely far, so with an opacity of 1 it
/// is cleared to the fog colour and geometry fades into it (transparent effects drawn over the
/// background keep their colour). An `edge` also fades the border of the world fully into the
/// fog, and then the background is fully fog too.
public struct FogDescriptor: Sendable, Equatable {
    public let color: RenderColor
    public let density: Float
    public let startDistance: Float
    public let maximumOpacity: Float
    public let edge: FogEdgeDescriptor?

    public init(
        color: RenderColor,
        density: Float,
        startDistance: Float,
        maximumOpacity: Float,
        edge: FogEdgeDescriptor? = nil
    ) {
        self.color = color
        self.density = density
        self.startDistance = startDistance
        self.maximumOpacity = maximumOpacity
        self.edge = edge
    }

    public var isEnabled: Bool { density > 0 && maximumOpacity > 0 }

    public static let disabled = Self(color: .black, density: 0, startDistance: 0, maximumOpacity: 0)
}

/// A street lamp the scene lights every lit pixel with (`EnvironmentDescriptor.lamps`): a small warm point light whose
/// light ends at `reach` (render units).
public struct LampLightDescriptor: Hashable, Sendable {
    public let position: RenderVec3
    public let reach: Float
    public let color: RenderColor
    public let energy: Float

    public init(position: RenderVec3, reach: Float, color: RenderColor, energy: Float) {
        self.position = position
        self.reach = reach.isFinite ? max(reach, 0) : 0
        self.color = color
        self.energy = energy.isFinite ? max(energy, 0) : 0
    }

    /// The most lamps a frame lights with; more are dropped (pass the nearest first).
    public static let maximumCount = 40
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
    public let exposure: Float
    public let contrast: Float
    public let saturation: Float
    public let radianceCascades: RadianceCascadesDescriptor
    public let fog: FogDescriptor
    /// 0 dry ... 1 soaked: the ground and paving surface details turn glossy and a shade darker, in puddles
    /// more than between them.
    public let surfaceWetness: Float
    /// Street lamps, nearest first (`LampLightDescriptor.maximumCount` at most), and the radius within which each
    /// lamp's light stays near full (render units).
    public let lamps: [LampLightDescriptor]
    public let lampCoreRadius: Float
    /// The energy of the light in the glass surface detail's lit panes (`SurfaceDetail.Pattern.glass`): 0 by day.
    public let windowGlow: Float
    /// Clouds drifting between the sun and the ground.
    public let cloudShadows: CloudShadowDescriptor
    /// Screen-space reflections on glossy surfaces.
    public let reflections: ScreenSpaceReflectionDescriptor
    /// How strongly and how far the ambient occlusion (`ssaoEnabled`) darkens.
    public let ambientOcclusion: AmbientOcclusionDescriptor

    public init(
        backgroundColor: RenderColor,
        backgroundEnergy: Float,
        ambientColor: RenderColor,
        ambientEnergy: Float,
        ambientMap: AmbientMapHandle,
        skyCubemap: CubemapHandle,
        ssaoEnabled: Bool,
        tonemap: TonemapMode,
        tonemapWhite: Float,
        exposure: Float = 1,
        contrast: Float = 1,
        saturation: Float = 1,
        radianceCascades: RadianceCascadesDescriptor = .off,
        fog: FogDescriptor = .disabled,
        surfaceWetness: Float = 0,
        lamps: [LampLightDescriptor] = [],
        lampCoreRadius: Float = 0.3,
        windowGlow: Float = 0,
        cloudShadows: CloudShadowDescriptor = .off,
        reflections: ScreenSpaceReflectionDescriptor = .off,
        ambientOcclusion: AmbientOcclusionDescriptor = .standard
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
        self.exposure = exposure
        self.contrast = contrast
        self.saturation = saturation
        self.radianceCascades = radianceCascades
        self.fog = fog
        self.surfaceWetness = surfaceWetness.isFinite ? min(max(surfaceWetness, 0), 1) : 0
        self.lamps = lamps.count > LampLightDescriptor.maximumCount
            ? Array(lamps.prefix(LampLightDescriptor.maximumCount)) : lamps
        self.lampCoreRadius = lampCoreRadius.isFinite ? max(lampCoreRadius, 0) : 0
        self.windowGlow = windowGlow.isFinite ? max(windowGlow, 0) : 0
        self.cloudShadows = cloudShadows.sanitized
        self.reflections = reflections.sanitized
        self.ambientOcclusion = ambientOcclusion.sanitized
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
        tonemapWhite: 1,
        exposure: 1,
        contrast: 1,
        saturation: 1,
        radianceCascades: .off
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
    public let groundBottomColor: RenderColor
    public let groundEnergy: Float
    public let groundHorizonColor: RenderColor
    public let groundHorizonCurve: Float
    public let skyEnergy: Float
    public let skyHorizonColor: RenderColor
    public let skyHorizonCurve: Float
    public let skyTopColor: RenderColor
    public let sunColor: RenderColor
    public let sunCurve: Float
    public let sunDirection: RenderVec3
    public let sunEnergy: Float
    public let sunSizeRadians: Float

    public init(color: RenderColor, resolution: Int) {
        self.color = color
        self.resolution = resolution
        groundBottomColor = color
        groundEnergy = 1
        groundHorizonColor = color
        groundHorizonCurve = 0.02
        skyEnergy = 1
        skyHorizonColor = color
        skyHorizonCurve = 0.15
        skyTopColor = color
        sunColor = color
        sunCurve = 0.15
        sunDirection = RenderVec3(x: 0, y: -1, z: 0)
        sunEnergy = 0
        sunSizeRadians = 0
    }

    public init(
        resolution: Int,
        groundBottomColor: RenderColor,
        groundEnergy: Float,
        groundHorizonColor: RenderColor,
        groundHorizonCurve: Float,
        skyEnergy: Float,
        skyHorizonColor: RenderColor,
        skyHorizonCurve: Float,
        skyTopColor: RenderColor,
        sunColor: RenderColor,
        sunCurve: Float,
        sunDirection: RenderVec3,
        sunEnergy: Float,
        sunSizeRadians: Float
    ) {
        color = skyTopColor
        self.resolution = resolution
        self.groundBottomColor = groundBottomColor
        self.groundEnergy = groundEnergy
        self.groundHorizonColor = groundHorizonColor
        self.groundHorizonCurve = groundHorizonCurve
        self.skyEnergy = skyEnergy
        self.skyHorizonColor = skyHorizonColor
        self.skyHorizonCurve = skyHorizonCurve
        self.skyTopColor = skyTopColor
        self.sunColor = sunColor
        self.sunCurve = sunCurve
        self.sunDirection = sunDirection
        self.sunEnergy = sunEnergy
        self.sunSizeRadians = sunSizeRadians
    }
}

public enum AntiAliasing: UInt8, Sendable {
    case off, smaaMedium, smaaHigh, fxaa
}

public enum UpscaleMode: UInt8, Sendable {
    /// `fsr1`: AMD FidelityFX Super Resolution 1 (EASU, then RCAS sharpening); spatial, so it runs on every backend.
    /// `fsr3`: AMD FidelityFX Super Resolution 3.1, temporal (no frame generation): the scene renders with a
    /// sub-pixel jitter and motion vectors and the upscaler accumulates frames at the output size, so a render
    /// scale of 50-67 % keeps most of the native detail. It replaces the anti-aliasing and also runs at a render
    /// scale of 1 (native anti-aliasing). Draws that move give their motion since the previous frame (`motions`,
    /// `motion`); anything that moves without it smears. Backends without it, an orthographic camera or a
    /// partial viewport use `fsr1`.
    case linear, lanczos, fsr1, fsr3
}

/// Soft-threshold bloom on the HDR image before grading. `threshold` is in exposed linear units
/// (scene colour times the environment exposure), so it holds when the exposure changes;
/// `softKnee` (0...1) widens the transition below it, and `scatter` (0...1) moves weight from the
/// tight to the wide blur levels.
public struct BloomDescriptor: Sendable, Equatable {
    public let intensity: Float
    public let threshold: Float
    public let softKnee: Float
    public let scatter: Float

    public init(intensity: Float, threshold: Float, softKnee: Float, scatter: Float) {
        self.intensity = intensity
        self.threshold = threshold
        self.softKnee = softKnee
        self.scatter = scatter
    }

    public var isEnabled: Bool { intensity > 0 }

    public static let disabled = Self(intensity: 0, threshold: 1, softKnee: 0.5, scatter: 0.7)
}

/// A round vignette that darkens the HDR image toward the corners by up to `intensity` (0...1);
/// `smoothness` (above 0, at most 1) is how far from the corners the falloff starts.
public struct VignetteDescriptor: Sendable, Equatable {
    public let intensity: Float
    public let smoothness: Float

    public init(intensity: Float, smoothness: Float) {
        self.intensity = intensity
        self.smoothness = smoothness
    }

    public var isEnabled: Bool { intensity > 0 }

    public static let disabled = Self(intensity: 0, smoothness: 0.5)
}

public struct PostChainDescriptor: Sendable {
    public let screenShaders: [ScreenShaderHandle]
    public let casSharpness: Float
    public let tiltShiftEnabled: Bool
    /// Flips the final world image left-right after anti-aliasing. The canvas pass draws on top of
    /// the flipped image unchanged, and the world geometry, culling and handedness are untouched.
    public let mirrorHorizontally: Bool
    public let bloom: BloomDescriptor
    public let vignette: VignetteDescriptor

    public init(
        screenShaders: [ScreenShaderHandle],
        casSharpness: Float,
        tiltShiftEnabled: Bool,
        mirrorHorizontally: Bool = false,
        bloom: BloomDescriptor = .disabled,
        vignette: VignetteDescriptor = .disabled
    ) {
        self.screenShaders = screenShaders
        self.casSharpness = casSharpness
        self.tiltShiftEnabled = tiltShiftEnabled
        self.mirrorHorizontally = mirrorHorizontally
        self.bloom = bloom
        self.vignette = vignette
    }

    public static let empty = Self(screenShaders: [], casSharpness: 0, tiltShiftEnabled: false)
}

@MainActor
public protocol RenderResourceBackend: AnyObject {
    func loadTexture(_ descriptor: TextureLoadDescriptor) throws -> TextureHandle
    func createTexture(_ descriptor: TextureDescriptor, bytes: UnsafeRawBufferPointer?) throws -> TextureHandle
    func updateTexture(_ handle: TextureHandle, bytes: UnsafeRawBufferPointer) throws
    func destroyTexture(_ handle: TextureHandle)
    func loadFont(_ descriptor: FontDescriptor) throws -> FontHandle
    func destroyFont(_ handle: FontHandle)
    func createRenderTarget(_ descriptor: RenderTargetDescriptor) throws -> RenderTargetHandle
    func colorTexture(of target: RenderTargetHandle) throws -> TextureHandle
    func destroyRenderTarget(_ handle: RenderTargetHandle)
    func loadModel(path: String, flags: ModelImportFlags) throws -> ModelHandle
    func modelMetadata(_ handle: ModelHandle) throws -> ModelMetadata
    func destroyModel(_ handle: ModelHandle)
    func createMesh(_ descriptor: MeshDescriptor, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws -> MeshHandle
    func updateMesh(_ handle: MeshHandle, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws
    func destroyMesh(_ handle: MeshHandle)
    func createMaterial(_ descriptor: MaterialDescriptor) throws -> MaterialHandle
    func createMaterialOverride(
        from model: ModelHandle,
        materialIndex: Int,
        patch: ModelMaterialPatch
    ) throws -> MaterialHandle
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
    func animationTreeParameter(_ handle: AnimationTreeHandle, name: String) throws -> Float
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

public extension RenderResourceBackend {
    func animationTreeParameter(
        _ handle: AnimationTreeHandle,
        name: String
    ) throws -> Float {
        _ = handle
        _ = name
        throw RenderResourceBackendError.unsupportedOperation(
            "This backend cannot query animation-tree parameters"
        )
    }

    func createMaterialOverride(
        from model: ModelHandle,
        materialIndex: Int,
        patch: ModelMaterialPatch
    ) throws -> MaterialHandle {
        _ = model
        _ = materialIndex
        _ = patch
        throw RenderResourceBackendError.unsupportedOperation(
            "This backend cannot derive a material override from an imported model"
        )
    }

    func loadTexture(path: String) throws -> TextureHandle {
        try loadTexture(.init(path: path))
    }

    func createMesh(
        _ descriptor: MeshDescriptor,
        vertices: [RenderVertex],
        indices: [UInt32]? = nil
    ) throws -> MeshHandle {
        try vertices.withUnsafeBytes { vertexBytes in
            guard let indices else {
                return try createMesh(descriptor, vertices: vertexBytes, indices: nil)
            }
            return try indices.withUnsafeBytes { indexBytes in
                try createMesh(descriptor, vertices: vertexBytes, indices: indexBytes)
            }
        }
    }

    func updateMesh(
        _ handle: MeshHandle,
        vertices: [RenderVertex],
        indices: [UInt32]? = nil
    ) throws {
        try vertices.withUnsafeBytes { vertexBytes in
            guard let indices else {
                return try updateMesh(handle, vertices: vertexBytes, indices: nil)
            }
            try indices.withUnsafeBytes { indexBytes in
                try updateMesh(handle, vertices: vertexBytes, indices: indexBytes)
            }
        }
    }

    func modelBounds(_ handle: ModelHandle) throws -> RenderBounds3 {
        try modelMetadata(handle).bounds
    }
}
