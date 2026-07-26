import BTBHRenderer
import Foundation
import XCTest
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPURenderServicesTests: XCTestCase {
    func testBundleInstallsPresentsClearAndShutsDownExactlyOnce() throws {
        #if os(Windows)
        RenderServices.uninstall()
        let bundle = GPURenderServices.make(
            request: .vulkan,
            debugMode: true
        )
        try RenderServices.install(bundle)

        let access = RenderServices.access
        try access.platform.openWindow(.init(
            title: "BTBH GPU bundle test",
            width: 640,
            height: 360,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        access.platform.pollEvents()
        _ = access.platform.beginFrame()
        access.canvas.beginFrame()
        access.canvas.submit(.clear(.init(
            r: 12,
            g: 24,
            b: 48,
            a: 255
        )))
        access.canvas.endFrame()
        XCTAssertFalse(access.platform.shouldClose)

        RenderServices.uninstall()
        RenderServices.uninstall()
        XCTAssertTrue(bundle.isShutdown)
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testTextureLoadCreatesARealGPUResourceAndDestroyKeepsWindowAlive() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU texture resource test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let handle = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path,
            generateMipmaps: true,
            filter: .trilinear,
            wrap: .repeatMode
        ))
        XCTAssertTrue(handle.isValid)
        RenderServices.resources.destroyTexture(handle)
        XCTAssertFalse(RenderServices.platform.shouldClose)
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testTextureHandleFinishesSafelyAfterOwnerWindowClosesAndReopens() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
            debugMode: true
        )
        try RenderServices.install(bundle)
        let descriptor = WindowDescriptor(
            title: "BTBH GPU texture owner test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        )
        try RenderServices.platform.openWindow(descriptor)
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let handle = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path
        ))

        RenderServices.platform.closeWindow()
        try RenderServices.platform.openWindow(descriptor)
        RenderServices.resources.destroyTexture(handle)
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.canvas.endFrame()

        XCTAssertFalse(RenderServices.platform.shouldClose)
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testTextureWrapperFinishesSafelyAfterOwnerWindowCloses() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU closed texture owner test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let handle = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path
        ))

        RenderServices.platform.closeWindow()
        RenderServices.resources.destroyTexture(handle)
        RenderServices.uninstall()

        XCTAssertTrue(bundle.isShutdown)
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }

    func testUnsupportedResourceCallFailsClosedWithExactOperation() throws {
        #if os(Windows)
        RenderServices.uninstall()
        defer { RenderServices.uninstall() }
        let bundle = GPURenderServices.make(
            request: .vulkan,
            debugMode: true
        )
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: "BTBH GPU unsupported resource test",
            width: 320,
            height: 180,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let retainedTexture = try RenderServices.resources.loadTexture(.init(
            path: packageRoot
                .appendingPathComponent("external/SDL/test/icon.png")
                .path
        ))
        XCTAssertTrue(retainedTexture.isValid)

        XCTAssertThrowsError(try RenderServices.resources.loadFont(
            .init(path: "not-yet-supported.ttf", baseSize: 16, glyphs: [65])
        )) { error in
            XCTAssertEqual(
                error as? GPUBackendUnsupportedError,
                .operation("resources.loadFont(_:)")
            )
        }
        XCTAssertTrue(RenderServices.platform.shouldClose)
        RenderServices.uninstall()
        XCTAssertTrue(bundle.isShutdown)
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }
}
