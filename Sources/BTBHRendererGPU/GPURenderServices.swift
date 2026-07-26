import BTBHRenderer

@MainActor
public enum GPURenderServices {
    public static func make(
        request: GPUBackendRequest = .platformDefault,
        debugMode: Bool
    ) -> GPURenderServicesBundle {
        GPURenderServicesBundle(
            request: request,
            debugMode: debugMode
        )
    }
}

@MainActor
public final class GPURenderServicesBundle: RenderBackendBundle {
    public let gpuPlatform: GPUPlatform
    private let gpuCanvas: GPUCanvas
    private let gpuResources: GPUResourceBackend
    private let gpuScene: GPUSceneBackend

    public private(set) var isRendererShutdown = false
    public private(set) var isShutdown = false

    public var platform: any GamePlatform { gpuPlatform }
    public var canvas: any Canvas2DBackend { gpuCanvas }
    public var resources: any RenderResourceBackend { gpuResources }
    public var scene: any SceneBackend { gpuScene }

    fileprivate init(
        request: GPUBackendRequest,
        debugMode: Bool
    ) {
        let platform = GPUPlatform(
            request: request,
            debugMode: debugMode
        )
        gpuPlatform = platform
        gpuCanvas = GPUCanvas(platform: platform)
        gpuResources = GPUResourceBackend()
        gpuScene = GPUSceneBackend(platform: platform)
    }

    public func shutdownRenderer() {
        guard !isRendererShutdown else { return }
        gpuScene.shutdown()
        isRendererShutdown = true
    }

    public func shutdown() {
        guard !isShutdown else { return }
        shutdownRenderer()
        gpuPlatform.closeWindow()
        isShutdown = true
    }
}
