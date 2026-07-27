import BTBHRenderer
import Foundation

public enum GPUBackendUnsupportedError: Error, Equatable {
    case operation(String)
}

enum GPUResourceStagingError: Error, Equatable {
    case missingAsset(String)
    case invalidTexture(UInt64)
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

private struct GPUScreenShaderResource {
    let fragmentPath: String
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
}

private struct GPUAnimationTreeResource {
    let model: ModelHandle
    let library: AnimationLibraryHandle
    let rootNode: String
    let rootBone: Int?
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

private struct GPUInstanceBufferResource {
    let storage: GPUInstanceBufferProbeResource
    let descriptor: InstanceBufferDescriptor
}

private struct GPUCustomMeshResource {
    let storage: GPUCustomMeshProbeResource
    let descriptor: MeshDescriptor
}

private struct GPUCustomMeshUpload {
    let packedVertices: [Float]
    let indices: [UInt32]
}

private struct GPUAmbientMapResource {
    let cubemap: CubemapHandle
}

@MainActor
final class GPUResourceBackend: RenderResourceBackend {
    private let platform: GPUPlatform
    private let textureHandles = try? HandlePool(initialCapacity: 64)
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
    private var models: [ModelHandle: GPUModelProbeResource] = [:]
    private var meshes: [MeshHandle: GPUCustomMeshResource] = [:]
    private var materials: [MaterialHandle: GPUMaterialResource] = [:]
    private var instanceBuffers: [
        InstanceBufferHandle: GPUInstanceBufferResource
    ] = [:]
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

    init(platform: GPUPlatform) {
        self.platform = platform
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
    func loadFont(_ descriptor: FontDescriptor) throws -> FontHandle { throw unsupported() }
    func destroyFont(_ handle: FontHandle) {}
    func createRenderTarget(_ descriptor: RenderTargetDescriptor) throws -> RenderTargetHandle { throw unsupported() }
    func colorTexture(of target: RenderTargetHandle) throws -> TextureHandle { throw unsupported() }
    func destroyRenderTarget(_ handle: RenderTargetHandle) {}
    func loadModel(
        path: String,
        flags: ModelImportFlags
    ) throws -> ModelHandle {
        trace("loadModel path=\(path)")
        _ = flags
        guard let modelHandles else {
            throw GPUModelProbeError.operationFailed(
                message: "Could not allocate the GPU model handle pool"
            )
        }
        let resource = try platform.loadModel(path: path)
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
        guard instances.count > 0,
              instances.count <= resource.storage.byteCapacity,
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
                (
                    $0.name,
                    GPUAnimationSwitchState(
                        inputs: $0.inputs,
                        crossfadeSeconds: $0.crossfadeSeconds,
                        synchronized: $0.synchronized,
                        activeInput: $0.activeInput,
                        previousInput: 0
                    )
                )
            }
        )
        let handle = try animationTreeHandles.acquire(
            AnimationTreeHandle.self
        )
        animationTrees[handle] = .init(
            model: descriptor.model,
            library: descriptor.library,
            rootNode: descriptor.rootNode,
            rootBone: descriptor.rootBone,
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
            cubemaps[cubemap] != nil
        else {
            throw GPUResourceStagingError.invalidCubemap(cubemap.rawValue)
        }
        guard let ambientMapHandles else {
            throw GPUResourceStagingError.invalidAmbientMap(0)
        }
        let handle = try ambientMapHandles.acquire(AmbientMapHandle.self)
        ambientMaps[handle] = .init(cubemap: cubemap)
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
            fragmentPath: descriptor.fragmentPath,
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
            let duration = Double(clip.duration)
            let advancedTime = Double(clip.time) +
                Double(deltaSeconds) * Double(clip.speed)
            let crossedBoundary =
                (clip.speed < 0 && advancedTime <= 0) ||
                (clip.speed > 0 && advancedTime >= duration)
            if crossedBoundary {
                if clip.loop {
                    if duration > 0 {
                        var wrappedTime = advancedTime
                            .truncatingRemainder(dividingBy: duration)
                        if wrappedTime < 0 {
                            wrappedTime += duration
                        }
                        clip.time = Float(wrappedTime)
                    } else {
                        clip.time = 0
                    }
                } else {
                    clip.play = false
                    clip.time = Float(min(max(0, advancedTime), duration))
                }
            } else {
                clip.time = Float(advancedTime)
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
            let scalars = [
                vertex.position.x,
                vertex.position.y,
                vertex.position.z,
                vertex.normal.x,
                vertex.normal.y,
                vertex.normal.z,
                vertex.textureCoordinate.x,
                vertex.textureCoordinate.y,
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
            indexValues = values
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
            descriptor.range.isFinite,
            descriptor.range >= 0,
            descriptor.shadowSoftness.isFinite,
            descriptor.shadowSoftness >= 0
        else {
            throw GPUResourceStagingError.invalidLightDescriptor(
                "Light vectors and nonnegative scalar fields must be finite"
            )
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

    private func trace(_ operation: String) {
        guard ProcessInfo.processInfo.environment[
            "BTBH_GPU_RESOURCE_TRACE"
        ] == "1" else {
            return
        }
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
        for resource in meshes.values {
            platform.destroyCustomMesh(resource.storage.pointer)
        }
        meshes.removeAll()
        for resource in instanceBuffers.values {
            platform.destroyInstanceBuffer(resource.storage.pointer)
        }
        instanceBuffers.removeAll()
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

@MainActor
final class GPUSceneBackend: SceneBackend {
    private enum PendingModelDraw {
        case instances(ModelInstancesDraw)
        case transformStreams(ModelTransformStreamsDraw)
    }

    private let platform: GPUPlatform
    private let resources: GPUResourceBackend
    private var unsupportedOperation: String?
    private var drawableSize = PixelSize2D(width: 0, height: 0)
    private var currentFrame: FrameDescriptor?
    private var pendingModelDraws: [PendingModelDraw] = []

    init(platform: GPUPlatform, resources: GPUResourceBackend) {
        self.platform = platform
        self.resources = resources
    }

    func initialize(drawableSize: PixelSize2D) throws {
        self.drawableSize = drawableSize
    }

    func resize(drawableSize: PixelSize2D) throws {
        self.drawableSize = drawableSize
    }

    func beginFrame(_ descriptor: FrameDescriptor) {
        currentFrame = descriptor
        unsupportedOperation = nil
        pendingModelDraws.removeAll(keepingCapacity: true)
    }

    func submit(_ draw: SceneDraw) {
        switch draw {
        case .modelInstances(let modelInstances):
            pendingModelDraws.append(.instances(modelInstances))
        case .modelTransformStreams(let streams):
            pendingModelDraws.append(.transformStreams(streams))
        default:
            guard unsupportedOperation == nil else { return }
            unsupportedOperation = Self.submitOperation(draw)
        }
    }

    func endFrame() throws {
        if let unsupportedOperation {
            platform.failClosed(operation: unsupportedOperation)
            throw GPUBackendUnsupportedError.operation(unsupportedOperation)
        }
        guard !pendingModelDraws.isEmpty else { return }
        guard let currentFrame,
              let viewProjection = viewProjection(
                camera: currentFrame.camera,
                viewport: currentFrame.viewport
              ) else {
            throw GPUSceneProbeError.operationFailed(
                message: "Invalid GPU scene camera"
            )
        }
        var packedInstances: [Float] = []
        var batches: [GPUModelInstancesProbeBatch] = []
        for pendingDraw in pendingModelDraws {
            switch pendingDraw {
            case .instances(let draw):
                guard draw.sourceStart >= 0,
                      draw.count > 0,
                      draw.sourceStart <= draw.instances.count,
                      draw.count <=
                        draw.instances.count - draw.sourceStart else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Invalid GPU model instance range"
                    )
                }
                let firstInstance = packedInstances.count / 20
                for instance in draw.instances[
                    draw.sourceStart..<(draw.sourceStart + draw.count)
                ] {
                    Self.append(
                        matrix: instance.transform.matrix,
                        color: instance.color,
                        to: &packedInstances
                    )
                }
                batches.append(.init(
                    model: try resources.modelPointer(draw.model),
                    firstInstance: firstInstance,
                    instanceCount: draw.count
                ))
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
                      ) else {
                    throw GPUSceneProbeError.operationFailed(
                        message: "Invalid GPU model transform stream range"
                    )
                }
                if let bounds = draw.clusterBounds,
                   !intersectsFrustum(bounds) {
                    continue
                }
                let firstInstance = packedInstances.count / 20
                for index in draw.sourceStart..<(draw.sourceStart + draw.count) {
                    Self.append(
                        matrix: Self.transformMatrix(
                            position: draw.positions[index],
                            rotation: draw.rotations[index],
                            scale: draw.scales[index]
                        ),
                        color: draw.colors?[index] ?? .white,
                        to: &packedInstances
                    )
                }
                batches.append(.init(
                    model: try resources.modelPointer(draw.model),
                    firstInstance: firstInstance,
                    instanceCount: draw.count
                ))
            }
        }
        guard !batches.isEmpty else { return }
        try platform.presentModelInstances(
            viewProjection: viewProjection,
            instances: packedInstances,
            batches: batches,
            clearColor: currentFrame.environment.backgroundColor
        )
    }

    func project(
        _ world: RenderVec3,
        camera: RenderCamera
    ) -> RenderVec2 {
        guard let geometry = cameraGeometry(camera) else {
            return .zero
        }
        let relative = world - camera.position
        let horizontal = relative.dot(geometry.right)
        let vertical = relative.dot(geometry.up)
        let depth = relative.dot(geometry.forward)
        let normalizedX: Float
        let normalizedY: Float
        switch camera.projection {
        case .perspective:
            guard depth != 0 else { return .zero }
            normalizedX =
                horizontal / (depth * geometry.halfWidth)
            normalizedY =
                vertical / (depth * geometry.halfHeight)
        case .orthographic:
            normalizedX = horizontal / geometry.halfWidth
            normalizedY = vertical / geometry.halfHeight
        }
        guard normalizedX.isFinite, normalizedY.isFinite else {
            return .zero
        }
        return .init(
            x: (normalizedX + 1) * 0.5 * Float(drawableSize.width),
            y: (1 - normalizedY) * 0.5 * Float(drawableSize.height)
        )
    }

    func unprojectRay(
        _ screen: RenderVec2,
        camera: RenderCamera
    ) -> RenderRay {
        guard let geometry = cameraGeometry(camera),
              screen.x.isFinite,
              screen.y.isFinite else {
            return .init(origin: .zero, direction: .zero)
        }
        let normalizedX =
            2 * screen.x / Float(drawableSize.width) - 1
        let normalizedY =
            1 - 2 * screen.y / Float(drawableSize.height)
        switch camera.projection {
        case .perspective:
            let direction = (
                geometry.forward +
                geometry.right *
                    (normalizedX * geometry.halfWidth) +
                geometry.up *
                    (normalizedY * geometry.halfHeight)
            ).normalized()
            return .init(origin: camera.position, direction: direction)
        case .orthographic:
            let origin =
                camera.position +
                geometry.right *
                    (normalizedX * geometry.halfWidth) +
                geometry.up *
                    (normalizedY * geometry.halfHeight) +
                geometry.forward * 0.01
            return .init(origin: origin, direction: geometry.forward)
        }
    }

    func intersectsFrustum(_ bounds: RenderBounds3) -> Bool {
        let minimum = bounds.minimum
        let maximum = bounds.maximum
        let boundsScalars = [
            minimum.x, minimum.y, minimum.z,
            maximum.x, maximum.y, maximum.z,
        ]
        guard boundsScalars.allSatisfy(\.isFinite),
              minimum.x <= maximum.x,
              minimum.y <= maximum.y,
              minimum.z <= maximum.z else {
            return false
        }
        guard let frame = currentFrame,
              drawableSize.width > 0,
              drawableSize.height > 0 else {
            return true
        }

        let camera = frame.camera
        let forward = (camera.target - camera.position).normalized()
        let right = forward.cross(camera.up).normalized()
        let correctedUp = right.cross(forward).normalized()
        guard forward.lengthSquared.isFinite,
              right.lengthSquared.isFinite,
              correctedUp.lengthSquared.isFinite,
              forward.lengthSquared > 0,
              right.lengthSquared > 0,
              correctedUp.lengthSquared > 0 else {
            return true
        }

        let center = (minimum + maximum) * 0.5
        let extents = (maximum - minimum) * 0.5
        let relativeCenter = center - camera.position
        let centerDepth = relativeCenter.dot(forward)
        let centerRight = relativeCenter.dot(right)
        let centerUp = relativeCenter.dot(correctedUp)
        let depthRadius = Self.projectedRadius(extents, on: forward)
        let rightRadius = Self.projectedRadius(extents, on: right)
        let upRadius = Self.projectedRadius(extents, on: correctedUp)
        guard centerDepth + depthRadius >= 0 else {
            return false
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
            return true
        }
        let aspect = viewportWidth / viewportHeight
        switch camera.projection {
        case .perspective:
            guard camera.verticalFOVDegrees < 180 else {
                return true
            }
            let verticalTangent = tan(
                camera.verticalFOVDegrees * .pi / 360
            )
            let horizontalTangent = verticalTangent * aspect
            guard verticalTangent.isFinite,
                  horizontalTangent.isFinite,
                  verticalTangent > 0,
                  horizontalTangent > 0 else {
                return true
            }
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
        case .orthographic:
            let halfHeight = camera.verticalFOVDegrees * 0.5
            let halfWidth = halfHeight * aspect
            guard abs(centerRight) <= halfWidth + rightRadius,
                  abs(centerUp) <= halfHeight + upRadius else {
                return false
            }
        }
        return true
    }

    func readbackWorldFrame() throws -> FrameReadback {
        throw GPUBackendUnsupportedError.operation(
            "scene.readbackWorldFrame"
        )
    }

    func captureWorldFrameTexture() throws -> TextureHandle {
        throw GPUBackendUnsupportedError.operation(
            "scene.captureWorldFrameTexture"
        )
    }

    func shutdown() {
        unsupportedOperation = nil
        currentFrame = nil
        pendingModelDraws.removeAll(keepingCapacity: false)
        drawableSize = .init(width: 0, height: 0)
    }

    private static func projectedRadius(
        _ extents: RenderVec3,
        on axis: RenderVec3
    ) -> Float {
        abs(axis.x) * extents.x +
            abs(axis.y) * extents.y +
            abs(axis.z) * extents.z
    }

    private static func submitOperation(_ draw: SceneDraw) -> String {
        switch draw {
        case .model:
            "scene.submit.model"
        case .modelInstances:
            "scene.submit.modelInstances"
        case .modelTransformStreams:
            "scene.submit.modelTransformStreams"
        case .mesh:
            "scene.submit.mesh"
        case .meshInstances:
            "scene.submit.meshInstances"
        case .meshTransformStreams:
            "scene.submit.meshTransformStreams"
        case .animatedModel:
            "scene.submit.animatedModel"
        case .animatedModelInstances:
            "scene.submit.animatedModelInstances"
        case .animatedModelTransformStreams:
            "scene.submit.animatedModelTransformStreams"
        case .debug:
            "scene.submit.debug"
        }
    }

    private func cameraGeometry(
        _ camera: RenderCamera
    ) -> (
        forward: RenderVec3,
        right: RenderVec3,
        up: RenderVec3,
        halfWidth: Float,
        halfHeight: Float
    )? {
        guard drawableSize.width > 0,
              drawableSize.height > 0,
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
        let aspect =
            Float(drawableSize.width) / Float(drawableSize.height)
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

    private func viewProjection(
        camera: RenderCamera,
        viewport: NormalizedViewport
    ) -> [Float]? {
        guard let geometry = cameraGeometry(camera) else { return nil }
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
        let near: Float = 0.01
        let far: Float = 1_000_000
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
        return Self.multiply(projection, view)
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

    private static func append(
        matrix: RenderMatrix4,
        color: RenderColor,
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
        ])
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
