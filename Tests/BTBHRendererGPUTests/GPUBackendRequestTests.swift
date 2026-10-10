import XCTest
@testable import BTBHRendererGPU

final class GPUBackendRequestTests: XCTestCase {
    func testPlatformDefaultRequestsTheShippingBackendExactly() {
        #if os(Windows) || os(Linux)
        XCTAssertEqual(GPUBackendRequest.platformDefault, .vulkan)
        XCTAssertEqual(GPUBackendRequest.platformDefault.driverName, "vulkan")
        #elseif os(macOS)
        XCTAssertEqual(GPUBackendRequest.platformDefault, .metal)
        XCTAssertEqual(GPUBackendRequest.platformDefault.driverName, "metal")
        #else
        XCTFail("The production GPU renderer supports Windows, Linux and macOS only")
        #endif
    }

    func testExplicitBackendNamesRejectFallbackAliases() {
        XCTAssertEqual(GPUBackendRequest(exactDriverName: "vulkan"), .vulkan)
        XCTAssertEqual(GPUBackendRequest(exactDriverName: "metal"), .metal)
        XCTAssertNil(GPUBackendRequest(exactDriverName: "d3d12"))
        XCTAssertNil(GPUBackendRequest(exactDriverName: "opengl"))
        XCTAssertNil(GPUBackendRequest(exactDriverName: "legacy"))
        XCTAssertNil(GPUBackendRequest(exactDriverName: "VULKAN"))
    }
}
