import XCTest
@testable import BTBHRenderer

final class RendererVersionTests: XCTestCase {
    func testSwiftAndCABIReportVersionOne() {
        XCTAssertEqual(RendererABI.version, 1)
        XCTAssertEqual(RendererABI.cVersion, RendererABI.version)
    }
}
