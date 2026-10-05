import BTBHRenderer
import Foundation
import XCTest
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUSceneViewportTests: XCTestCase {
    func testFullViewportPlacementIsTheIdentity() {
        XCTAssertEqual(
            GPUSceneBackend.viewportPlacement(.full),
            [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        )
    }

    func testViewportPlacementPutsTheCameraImageInsideItsSubRectangle() {
        // The right half of the drawable, its lower 40 % (normalized, y down).
        let placement = GPUSceneBackend.viewportPlacement(
            NormalizedViewport(x: 0.5, y: 0.6, width: 0.5, height: 0.4)
        )
        func place(_ x: Float, _ y: Float) -> (x: Float, y: Float) {
            (placement[0] * x + placement[4] * y + placement[12],
             placement[1] * x + placement[5] * y + placement[13])
        }
        let topLeft = place(-1, 1)
        let bottomRight = place(1, -1)
        // NDC y is up: the top edge sits 60 % down the drawable, at -0.2.
        XCTAssertEqual(topLeft.x, 0, accuracy: 0.000_001)
        XCTAssertEqual(topLeft.y, -0.2, accuracy: 0.000_001)
        XCTAssertEqual(bottomRight.x, 1, accuracy: 0.000_001)
        XCTAssertEqual(bottomRight.y, -1, accuracy: 0.000_001)
    }
}
