import BTBHRenderer
import Foundation

public enum GPUBackendUnsupportedError: Error, Equatable {
    case operation(String)
}

enum GPUResourceStagingError: Error, Equatable {
    case missingAsset(String)
    case invalidTexture(UInt64)
    case invalidModel(UInt64)
    case invalidAnimationLibrary(UInt64)
    case invalidAnimationTree(UInt64)
    case invalidAnimationTreeDescriptor(String)
    case invalidAnimationTreeParameter(String)
    case invalidScreenShader(UInt64)
}

private struct GPUScreenShaderResource {
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

@MainActor
final class GPUResourceBackend: RenderResourceBackend {
    private let platform: GPUPlatform
    private let textureHandles = try? HandlePool(initialCapacity: 64)
    private let modelHandles = try? HandlePool(initialCapacity: 32)
    private let animationLibraryHandles = try? HandlePool(
        initialCapacity: 8
    )
    private let animationTreeHandles = try? HandlePool(initialCapacity: 16)
    private let screenShaderHandles = try? HandlePool(initialCapacity: 8)
    private var textures: [TextureHandle: GPUTextureProbeResource] = [:]
    private var models: [ModelHandle: GPUModelProbeResource] = [:]
    private var animationLibraries: [
        AnimationLibraryHandle: GPUAnimationLibraryProbeResource
    ] = [:]
    private var animationTrees: [
        AnimationTreeHandle: GPUAnimationTreeResource
    ] = [:]
    private var screenShaders: [
        ScreenShaderHandle: GPUScreenShaderResource
    ] = [:]
    private var pendingTextureDestruction: Set<TextureHandle> = []
    private var pendingModelDestruction: Set<ModelHandle> = []
    private var pendingAnimationLibraryDestruction:
        Set<AnimationLibraryHandle> = []

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
        guard
            !pendingModelDestruction.contains(handle),
            let resource = models[handle]
        else {
            throw GPUResourceStagingError.invalidModel(handle.rawValue)
        }
        return resource.metadata
    }

    func destroyModel(_ handle: ModelHandle) {
        if animationTrees.values.contains(where: { $0.model == handle }) {
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
    func createMesh(_ descriptor: MeshDescriptor, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws -> MeshHandle { throw unsupported() }
    func updateMesh(_ handle: MeshHandle, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws { throw unsupported() }
    func destroyMesh(_ handle: MeshHandle) {}
    func createMaterial(_ descriptor: MaterialDescriptor) throws -> MaterialHandle { throw unsupported() }
    func updateMaterial(_ handle: MaterialHandle, descriptor: MaterialDescriptor) throws { throw unsupported() }
    func destroyMaterial(_ handle: MaterialHandle) {}
    func createInstanceBuffer(_ descriptor: InstanceBufferDescriptor) throws -> InstanceBufferHandle { throw unsupported() }
    func uploadInstances(_ handle: InstanceBufferHandle, instances: UnsafeRawBufferPointer) throws { throw unsupported() }
    func destroyInstanceBuffer(_ handle: InstanceBufferHandle) {}
    func loadAnimationLibrary(
        path: String
    ) throws -> AnimationLibraryHandle {
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

    private func collectPendingModel(_ handle: ModelHandle) {
        guard
            pendingModelDestruction.contains(handle),
            !animationTrees.values.contains(where: { $0.model == handle })
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

    func shutdown() {
        screenShaders.removeAll()
        pendingTextureDestruction.removeAll()
        animationTrees.removeAll()
        pendingAnimationLibraryDestruction.removeAll()
        pendingModelDestruction.removeAll()
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
