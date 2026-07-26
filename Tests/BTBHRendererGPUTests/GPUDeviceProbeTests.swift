import XCTest
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUDeviceProbeTests: XCTestCase {
    func testWindowsProbeCreatesOnlyTheExactVulkanDevice() throws {
        #if os(Windows)
        let probe = try GPUDeviceProbe(request: .vulkan, debugMode: true)
        XCTAssertEqual(probe.diagnostics.requestedDriver, "vulkan")
        XCTAssertEqual(probe.diagnostics.actualDriver, "vulkan")
        XCTAssertTrue(probe.diagnostics.hardwareAccelerated)
        XCTAssertEqual(probe.diagnostics.missingFeatureMask, 0)
        XCTAssertNotEqual(probe.diagnostics.supportedFeatureMask, 0)

        probe.shutdown()
        probe.shutdown()
        XCTAssertTrue(probe.isShutdown)
        #else
        throw XCTSkip("The Windows Vulkan probe runs only on Windows")
        #endif
    }
}
