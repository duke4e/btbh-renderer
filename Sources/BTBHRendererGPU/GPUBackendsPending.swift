import BTBHRenderer

public enum GPUBackendUnsupportedError: Error, Equatable {
    case operation(String)
}

@MainActor
final class GPUResourceBackend: RenderResourceBackend {
    func loadTexture(_ descriptor: TextureLoadDescriptor) throws -> TextureHandle { throw unsupported() }
    func createTexture(_ descriptor: TextureDescriptor, bytes: UnsafeRawBufferPointer?) throws -> TextureHandle { throw unsupported() }
    func updateTexture(_ handle: TextureHandle, bytes: UnsafeRawBufferPointer) throws { throw unsupported() }
    func destroyTexture(_ handle: TextureHandle) {}
    func loadFont(_ descriptor: FontDescriptor) throws -> FontHandle { throw unsupported() }
    func destroyFont(_ handle: FontHandle) {}
    func createRenderTarget(_ descriptor: RenderTargetDescriptor) throws -> RenderTargetHandle { throw unsupported() }
    func colorTexture(of target: RenderTargetHandle) throws -> TextureHandle { throw unsupported() }
    func destroyRenderTarget(_ handle: RenderTargetHandle) {}
    func loadModel(path: String, flags: ModelImportFlags) throws -> ModelHandle { throw unsupported() }
    func modelMetadata(_ handle: ModelHandle) throws -> ModelMetadata { throw unsupported() }
    func destroyModel(_ handle: ModelHandle) {}
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
    func createScreenShader(_ descriptor: ScreenShaderDescriptor) throws -> ScreenShaderHandle { throw unsupported() }
    func updateScreenShader(_ handle: ScreenShaderHandle, parameter: String, value: ShaderParameterValue) throws { throw unsupported() }
    func destroyScreenShader(_ handle: ScreenShaderHandle) {}

    private func unsupported(
        _ function: String = #function
    ) -> GPUBackendUnsupportedError {
        .operation("resources.\(function)")
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
