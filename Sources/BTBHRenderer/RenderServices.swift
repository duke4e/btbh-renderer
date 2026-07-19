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
    func shutdown()
}

@MainActor
public enum RenderServices {
    private static var installedBundle: (any RenderBackendBundle)?

    public static func install(_ bundle: any RenderBackendBundle) throws {
        guard installedBundle == nil else {
            throw RenderServicesError.alreadyInstalled
        }
        installedBundle = bundle
    }

    public static func uninstall() {
        let bundle = installedBundle
        installedBundle = nil
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
        return RenderServicesAccess(
            platform: bundle.platform,
            canvas: bundle.canvas,
            resources: bundle.resources,
            scene: bundle.scene
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
}
