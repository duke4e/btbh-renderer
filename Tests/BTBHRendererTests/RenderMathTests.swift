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

    func testVectorPODsRoundTripThroughTheCABI() {
        let vec2 = RenderVec2(x: -1.25, y: 2.5)
        let vec3 = RenderVec3(x: -3.5, y: 4.75, z: 5.25)
        let vec4 = RenderVec4(x: 6.5, y: -7.75, z: 8.25, w: 9.5)

        XCTAssertEqual(RenderVec2(core: vec2.core), vec2)
        XCTAssertEqual(RenderVec3(core: vec3.core), vec3)
        XCTAssertEqual(RenderVec4(core: vec4.core), vec4)
    }

    func testQuaternionColorRectAndBoundsRoundTripThroughTheCABI() {
        let quaternion = RenderQuaternion(x: 0.1, y: 0.2, z: 0.3, w: 0.4)
        let color = RenderColor(r: 1, g: 2, b: 3, a: 4)
        let rect = RenderRect(x: -5, y: 6, width: 7, height: 8)
        let bounds = RenderBounds3(
            minimum: RenderVec3(x: -1, y: -2, z: -3),
            maximum: RenderVec3(x: 4, y: 5, z: 6)
        )

        XCTAssertEqual(RenderQuaternion(core: quaternion.core), quaternion)
        XCTAssertEqual(RenderColor(core: color.core), color)
        XCTAssertEqual(RenderRect(core: rect.core), rect)
        XCTAssertEqual(RenderBounds3(core: bounds.core), bounds)
    }

    func testMatrixRoundTripPreservesAllColumnMajorElements() {
        let matrix = RenderMatrix4(
            c0: SIMD4(1, 2, 3, 4),
            c1: SIMD4(5, 6, 7, 8),
            c2: SIMD4(9, 10, 11, 12),
            c3: SIMD4(13, 14, 15, 16)
        )

        let core = matrix.core
        XCTAssertEqual(
            [
                core.elements.0, core.elements.1, core.elements.2, core.elements.3,
                core.elements.4, core.elements.5, core.elements.6, core.elements.7,
                core.elements.8, core.elements.9, core.elements.10, core.elements.11,
                core.elements.12, core.elements.13, core.elements.14, core.elements.15,
            ],
            (1...16).map(Float.init)
        )
        XCTAssertEqual(RenderMatrix4(core: core), matrix)
    }

    func testCameraRoundTripPreservesBothProjectionValues() {
        for projection in [RenderProjection.perspective, .orthographic] {
            let camera = RenderCamera(
                position: RenderVec3(x: 0, y: 10, z: 10),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 45,
                projection: projection
            )

            XCTAssertEqual(RenderCamera(core: camera.core), camera)
        }
    }
}
