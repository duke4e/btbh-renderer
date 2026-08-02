import XCTest
@testable import BTBHRenderer

final class CameraProjectionTests: XCTestCase {
    func testPerspectiveProjectionRoundTripsNormalizedViewportPoints() {
        let camera = RenderCamera(
            position: .init(x: 18, y: 42, z: -31),
            target: .init(x: 4, y: 0, z: 7),
            up: .up,
            verticalFOVDegrees: 45,
            projection: .perspective
        )
        let aspect: Float = 16.0 / 9.0

        for point in [
            NormalizedViewportPoint(x: 0, y: 0),
            .init(x: 1, y: 0),
            .init(x: 1, y: 1),
            .init(x: 0, y: 1),
            .init(x: 0.5, y: 0.5),
        ] {
            let ray = RenderCameraProjection.unprojectRay(
                point,
                camera: camera,
                aspectRatio: aspect
            )
            let world = ray.origin + ray.direction * 100
            let projected = RenderCameraProjection.projectNormalized(
                world,
                camera: camera,
                aspectRatio: aspect
            )

            XCTAssertEqual(projected.x, point.x, accuracy: 0.000_01)
            XCTAssertEqual(projected.y, point.y, accuracy: 0.000_01)
        }
    }

    func testProjectionFailsClosedForInvalidCameraOrAspect() {
        let camera = RenderCamera(
            position: .zero,
            target: .forward,
            up: .up,
            verticalFOVDegrees: 60,
            projection: .perspective
        )

        XCTAssertEqual(
            RenderCameraProjection.projectNormalized(
                .forward,
                camera: camera,
                aspectRatio: 0
            ),
            .zero
        )
        XCTAssertEqual(
            RenderCameraProjection.unprojectRay(
                .zero,
                camera: camera,
                aspectRatio: .nan
            ),
            .init(origin: .zero, direction: .zero)
        )
    }
}
