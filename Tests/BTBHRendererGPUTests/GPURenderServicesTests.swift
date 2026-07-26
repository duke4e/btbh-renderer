import BTBHRenderer
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

        XCTAssertThrowsError(try RenderServices.resources.loadTexture(
            .init(path: "not-yet-supported.png")
        )) { error in
            XCTAssertEqual(
                error as? GPUBackendUnsupportedError,
                .operation("resources.loadTexture(_:)")
            )
        }
        XCTAssertTrue(RenderServices.platform.shouldClose)
        #else
        throw XCTSkip("The Windows Vulkan bundle test runs only on Windows")
        #endif
    }
}
