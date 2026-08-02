import XCTest
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUDeviceProbeTests: XCTestCase {
    func testPlatformProbeCreatesOnlyTheRequestedGPUDevice() throws {
        let request = GPUBackendRequest.platformDefault
        let probe = try GPUDeviceProbe(request: request, debugMode: true)
        XCTAssertEqual(probe.diagnostics.requestedDriver, request.driverName)
        XCTAssertEqual(probe.diagnostics.actualDriver, request.driverName)
        XCTAssertTrue(probe.diagnostics.hardwareAccelerated)
        XCTAssertEqual(probe.diagnostics.missingFeatureMask, 0)
        XCTAssertNotEqual(probe.diagnostics.supportedFeatureMask, 0)

        probe.shutdown()
        probe.shutdown()
        XCTAssertTrue(probe.isShutdown)
    }
}
