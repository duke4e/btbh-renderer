import XCTest
@testable import BTBHRenderer

final class RenderMathTests: XCTestCase {
    func testVectorAndRectangleOperationsAreBackendIndependent() {
        XCTAssertEqual(RenderVec2(x: 3, y: 4).length, 5, accuracy: 0.0001)
        XCTAssertEqual(
            RenderVec3(x: 1, y: 0, z: 0).cross(RenderVec3(x: 0, y: 1, z: 0)),
            RenderVec3(x: 0, y: 0, z: 1)
        )
        XCTAssertTrue(RenderRect(x: 10, y: 20, width: 30, height: 40).contains(RenderVec2(x: 25, y: 30)))
    }

    func testCameraUsesExplicitPerspectiveProjection() {
        let camera = RenderCamera(
            position: RenderVec3(x: 0, y: 10, z: 10),
            target: .zero,
            up: .up,
            verticalFOVDegrees: 45,
            projection: .perspective
        )
        XCTAssertEqual(camera.projection, .perspective)
        XCTAssertEqual(camera.verticalFOVDegrees, 45)
    }

    func testSwiftPODRoundTripsThroughTheCABI() {
        let value = RenderBounds3(
            minimum: RenderVec3(x: -1, y: -2, z: -3),
            maximum: RenderVec3(x: 4, y: 5, z: 6)
        )
        XCTAssertEqual(RenderBounds3(core: value.core), value)
    }
}
