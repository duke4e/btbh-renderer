import XCTest
@testable import BTBHRenderer

final class RenderViewportTests: XCTestCase {
    func testNormalizedViewportPointPreservesTopLeftCoordinates() {
        XCTAssertEqual(
            NormalizedViewportPoint(x: 0.25, y: 0.75),
            .init(x: 0.25, y: 0.75)
        )
        XCTAssertEqual(NormalizedViewportPoint.zero, .init(x: 0, y: 0))
    }

    func testFullViewportUsesTopLeftNormalizedBounds() {
        XCTAssertEqual(
            NormalizedViewport.full,
            .init(x: 0, y: 0, width: 1, height: 1)
        )
    }

    func testFrameDescriptorDefaultsToFullViewport() {
        let descriptor = FrameDescriptor(
            camera: .init(
                position: .init(x: 0, y: 10, z: -10),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 45,
                projection: .perspective
            ),
            environment: .default
        )
        XCTAssertEqual(descriptor.viewport, .full)
    }

    func testFrameDescriptorPreservesExplicitViewport() {
        let viewport = NormalizedViewport(
            x: 0.125,
            y: 0.25,
            width: 0.5,
            height: 0.625
        )
        let descriptor = FrameDescriptor(
            camera: .init(
                position: .init(x: 0, y: 10, z: -10),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 45,
                projection: .perspective
            ),
            environment: .default,
            viewport: viewport
        )
        XCTAssertEqual(descriptor.viewport, viewport)
    }
}
