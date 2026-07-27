import BTBHRenderer
import Foundation

public enum GPUBackendUnsupportedError: Error, Equatable {
    case operation(String)
}

enum GPUResourceStagingError: Error, Equatable {
    case missingAsset(String)
    case invalidTexture(UInt64)
    case invalidModel(UInt64)
    case invalidScreenShader(UInt64)
}

private struct GPUScreenShaderResource {
    let fragmentPath: String
    var parameters: [String: ShaderParameterValue]
}

@MainActor
final class GPUResourceBackend: RenderResourceBackend {
    private let platform: GPUPlatform
    private let textureHandles = try? HandlePool(initialCapacity: 64)
    private let modelHandles = try? HandlePool(initialCapacity: 32)
    private let screenShaderHandles = try? HandlePool(initialCapacity: 8)
    private var textures: [TextureHandle: GPUTextureProbeResource] = [:]
    private var models: [ModelHandle: GPUModelProbeResource] = [:]
    private var screenShaders: [
        ScreenShaderHandle: GPUScreenShaderResource
    ] = [:]
    private var pendingTextureDestruction: Set<TextureHandle> = []

    init(platform: GPUPlatform) {
        self.platform = platform
    }

    func loadTexture(
        _ descriptor: TextureLoadDescriptor
    ) throws -> TextureHandle {
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
        if screenShaders.values.contains(where: {
            Self.textureDependencies(in: $0.parameters).contains(handle)
        }) {
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
        return handle
    }

    func modelMetadata(_ handle: ModelHandle) throws -> ModelMetadata {
        guard let resource = models[handle] else {
            throw GPUResourceStagingError.invalidModel(handle.rawValue)
        }
        return resource.metadata
    }

    func destroyModel(_ handle: ModelHandle) {
        guard
            let modelHandles,
            let resource = models.removeValue(forKey: handle),
            modelHandles.release(handle)
        else {
            return
        }
        platform.destroyModel(resource.pointer)
    }
    func createMesh(_ descriptor: MeshDescriptor, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws -> MeshHandle { throw unsupported() }
    func updateMesh(_ handle: MeshHandle, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws { throw unsupported() }
    func destroyMesh(_ handle: MeshHandle) {}
    func createMaterial(_ descriptor: MaterialDescriptor) throws -> MaterialHandle { throw unsupported() }
    func updateMaterial(_ handle: MaterialHandle, descriptor: MaterialDescriptor) throws { throw unsupported() }
    func destroyMaterial(_ handle: MaterialHandle) {}
    func createInstanceBuffer(_ descriptor: InstanceBufferDescriptor) throws -> InstanceBufferHandle { throw unsupported() }
    func uploadInstances(_ handle: InstanceBufferHandle, instances: UnsafeRawBufferPointer) throws { throw unsupported() }
    func destroyInstanceBuffer(_ handle: InstanceBufferHandle) {}
    func loadAnimationLibrary(path: String) throws -> AnimationLibraryHandle { throw unsupported() }
    func destroyAnimationLibrary(_ handle: AnimationLibraryHandle) {}
    func createAnimationPlayer(_ descriptor: AnimationPlayerDescriptor) throws -> AnimationPlayerHandle { throw unsupported() }
    func updateAnimationPlayer(_ handle: AnimationPlayerHandle, deltaSeconds: Float) throws { throw unsupported() }
    func setAnimation(_ handle: AnimationPlayerHandle, name: String, loop: Bool, time: Float?) throws { throw unsupported() }
    func destroyAnimationPlayer(_ handle: AnimationPlayerHandle) {}
    func createAnimationTree(_ descriptor: AnimationTreeDescriptor) throws -> AnimationTreeHandle { throw unsupported() }
    func updateAnimationTree(_ handle: AnimationTreeHandle, deltaSeconds: Float) throws { throw unsupported() }
    func setAnimationTreeParameter(_ handle: AnimationTreeHandle, name: String, value: Float) throws { throw unsupported() }
    func animationTreeParameter(_ handle: AnimationTreeHandle, name: String) throws -> Float { throw unsupported() }
    func destroyAnimationTree(_ handle: AnimationTreeHandle) {}
    func createLight(_ descriptor: LightDescriptor) throws -> LightHandle { throw unsupported() }
    func updateLight(_ handle: LightHandle, descriptor: LightDescriptor) throws { throw unsupported() }
    func destroyLight(_ handle: LightHandle) {}
    func createProceduralSky(_ descriptor: ProceduralSkyDescriptor) throws -> CubemapHandle { throw unsupported() }
    func createAmbientMap(from cubemap: CubemapHandle) throws -> AmbientMapHandle { throw unsupported() }
    func destroyCubemap(_ handle: CubemapHandle) {}
    func destroyAmbientMap(_ handle: AmbientMapHandle) {}
    func createSurfaceShader(_ descriptor: SurfaceShaderDescriptor) throws -> SurfaceShaderHandle { throw unsupported() }
    func updateSurfaceShader(_ handle: SurfaceShaderHandle, parameter: String, value: ShaderParameterValue) throws { throw unsupported() }
    func destroySurfaceShader(_ handle: SurfaceShaderHandle) {}
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

    private func unsupported(
        _ function: String = #function
    ) -> GPUBackendUnsupportedError {
        let operation = "resources.\(function)"
        platform.failClosed(operation: operation)
        return .operation(operation)
    }

    func shutdown() {
        screenShaders.removeAll()
        pendingTextureDestruction.removeAll()
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
        where textures[handle] == nil {
            throw GPUResourceStagingError.invalidTexture(handle.rawValue)
        }
    }

    private func collectPendingTextures(
        _ candidates: Set<TextureHandle>
    ) {
        for handle in candidates
        where pendingTextureDestruction.contains(handle) &&
            !screenShaders.values.contains(where: {
                Self.textureDependencies(in: $0.parameters).contains(handle)
            }) {
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
    private let platform: GPUPlatform
    private var unsupportedOperation: String?

    init(platform: GPUPlatform) {
        self.platform = platform
    }

    func initialize(drawableSize: PixelSize2D) throws {
        _ = drawableSize
    }

    func resize(drawableSize: PixelSize2D) throws {
        _ = drawableSize
    }

    func beginFrame(_ descriptor: FrameDescriptor) {
        _ = descriptor
        unsupportedOperation = nil
    }

    func submit(_ draw: SceneDraw) {
        _ = draw
        if unsupportedOperation == nil {
            unsupportedOperation = "scene.submit"
        }
    }

    func endFrame() throws {
        if let unsupportedOperation {
            platform.failClosed(operation: unsupportedOperation)
            throw GPUBackendUnsupportedError.operation(unsupportedOperation)
        }
    }

    func project(
        _ world: RenderVec3,
        camera: RenderCamera
    ) -> RenderVec2 {
        _ = world
        _ = camera
        platform.failClosed(operation: "scene.project")
        return .zero
    }

    func unprojectRay(
        _ screen: RenderVec2,
        camera: RenderCamera
    ) -> RenderRay {
        _ = screen
        _ = camera
        platform.failClosed(operation: "scene.unprojectRay")
        return .init(origin: .zero, direction: .zero)
    }

    func intersectsFrustum(_ bounds: RenderBounds3) -> Bool {
        _ = bounds
        platform.failClosed(operation: "scene.intersectsFrustum")
        return false
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
    }
}
