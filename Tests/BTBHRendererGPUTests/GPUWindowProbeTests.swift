import XCTest
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUWindowProbeTests: XCTestCase {
    func testWindowsProbePresentsResizesAndRestoresARealVulkanWindow() throws {
        #if os(Windows)
        let probe = try GPUWindowProbe(
            request: .vulkan,
            title: "BTBH production GPU package probe",
            width: 640,
            height: 360,
            hidden: false,
            debugMode: true
        )
        XCTAssertEqual(probe.deviceDiagnostics.actualDriver, "vulkan")
        XCTAssertTrue(probe.windowDiagnostics.claimed)
        XCTAssertEqual(probe.windowDiagnostics.logicalWidth, 640)
        XCTAssertEqual(probe.windowDiagnostics.logicalHeight, 360)

        try probe.presentClear(red: 18, green: 52, blue: 86, alpha: 255)
        try probe.resize(width: 800, height: 450)
        XCTAssertEqual(probe.windowDiagnostics.logicalWidth, 800)
        XCTAssertEqual(probe.windowDiagnostics.logicalHeight, 450)

        try probe.minimize()
        XCTAssertTrue(probe.windowDiagnostics.minimized)
        try probe.restore()
        XCTAssertFalse(probe.windowDiagnostics.minimized)
        XCTAssertTrue(probe.windowDiagnostics.claimed)
        try probe.presentClear(red: 86, green: 52, blue: 18, alpha: 255)

        probe.shutdown()
        probe.shutdown()
        XCTAssertTrue(probe.isShutdown)
        #else
        throw XCTSkip("The Windows Vulkan window probe runs only on Windows")
        #endif
    }
}
