public enum RenderServicesError: Error, Equatable, Sendable {
    case notInstalled
    case alreadyInstalled
}

@MainActor
public protocol RenderBackendBundle: AnyObject {
    var platform: any GamePlatform { get }
    var canvas: any Canvas2DBackend { get }
    var resources: any RenderResourceBackend { get }
    var scene: any SceneBackend { get }
    func shutdownRenderer()
    func shutdown()
}

@MainActor
public enum RenderServices {
    private static var installedBundle: (any RenderBackendBundle)?
    private static var installedRendererShutdown: RenderServicesRendererShutdown?

    public static func install(_ bundle: any RenderBackendBundle) throws {
        guard installedBundle == nil else {
            throw RenderServicesError.alreadyInstalled
        }
        installedBundle = bundle
        installedRendererShutdown = RenderServicesRendererShutdown {
            bundle.shutdownRenderer()
        }
    }

    public static func uninstall() {
        let bundle = installedBundle
        installedBundle = nil
        installedRendererShutdown = nil
        bundle?.shutdown()
    }

    public static func requireInstalled() throws {
        guard installedBundle != nil else {
            throw RenderServicesError.notInstalled
        }
    }

    public static var platform: any GamePlatform {
        requiredBundle().platform
    }

    public static var canvas: any Canvas2DBackend {
        requiredBundle().canvas
    }

    public static var resources: any RenderResourceBackend {
        requiredBundle().resources
    }

    public static var scene: any SceneBackend {
        requiredBundle().scene
    }

    public static var access: RenderServicesAccess {
        let bundle = requiredBundle()
        guard let installedRendererShutdown else {
            preconditionFailure("RenderServices renderer shutdown authority is unavailable")
        }
        return RenderServicesAccess(
            platform: bundle.platform,
            canvas: bundle.canvas,
            resources: bundle.resources,
            scene: bundle.scene,
            rendererShutdown: installedRendererShutdown
        )
    }

    private static func requiredBundle() -> any RenderBackendBundle {
        guard let installedBundle else {
            preconditionFailure("RenderServices used before install")
        }
        return installedBundle
    }
}

@MainActor
public struct RenderServicesAccess {
    public let platform: any GamePlatform
    public let canvas: any Canvas2DBackend
    public let resources: any RenderResourceBackend
    public let scene: any SceneBackend
    private let rendererShutdown: RenderServicesRendererShutdown

    public init(
        platform: any GamePlatform,
        canvas: any Canvas2DBackend,
        resources: any RenderResourceBackend,
        scene: any SceneBackend,
        shutdownRenderer: @escaping @MainActor () -> Void
    ) {
        self.init(
            platform: platform,
            canvas: canvas,
            resources: resources,
            scene: scene,
            rendererShutdown: RenderServicesRendererShutdown(action: shutdownRenderer)
        )
    }

    fileprivate init(
        platform: any GamePlatform,
        canvas: any Canvas2DBackend,
        resources: any RenderResourceBackend,
        scene: any SceneBackend,
        rendererShutdown: RenderServicesRendererShutdown
    ) {
        self.platform = platform
        self.canvas = canvas
        self.resources = resources
        self.scene = scene
        self.rendererShutdown = rendererShutdown
    }

    public func shutdownRenderer() {
        rendererShutdown.run()
    }
}

@MainActor
fileprivate final class RenderServicesRendererShutdown {
    private var action: (@MainActor () -> Void)?

    init(action: @escaping @MainActor () -> Void) {
        self.action = action
    }

    func run() {
        guard let action else { return }
        self.action = nil
        action()
    }
}
