import BTBHRenderer

@MainActor
final class GPUFrameCoordinator {
    private var nextClearToken: UInt64 = 1
    private var pendingScreenClear: (token: UInt64, color: RenderColor)?
    private(set) var claimedScreenClearToken: UInt64?

    func beginCanvasFrame() {
        pendingScreenClear = nil
        claimedScreenClearToken = nil
    }

    func stageScreenClear(_ color: RenderColor) -> UInt64 {
        let token = nextClearToken
        nextClearToken &+= 1
        pendingScreenClear = (token, color)
        return token
    }

    func claimScreenClearForScene() -> RenderColor? {
        guard let pendingScreenClear else { return nil }
        claimedScreenClearToken = pendingScreenClear.token
        return pendingScreenClear.color
    }
}

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
        let frameCoordinator = GPUFrameCoordinator()
        let resources = GPUResourceBackend(platform: platform)
        gpuResources = resources
        gpuCanvas = GPUCanvas(
            platform: platform,
            resources: resources,
            frameCoordinator: frameCoordinator
        )
        gpuScene = GPUSceneBackend(
            platform: platform,
            resources: resources,
            frameCoordinator: frameCoordinator
        )
    }

    public func shutdownRenderer() {
        guard !isRendererShutdown else { return }
        gpuScene.shutdown()
        gpuResources.shutdown()
        isRendererShutdown = true
    }

    public func shutdown() {
        guard !isShutdown else { return }
        shutdownRenderer()
        gpuPlatform.closeWindow()
        isShutdown = true
    }
}
