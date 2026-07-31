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

    func testRenderVec2SupportsScalarAndComponentArithmetic() {
        let lhs = RenderVec2(x: 6, y: -8)
        let rhs = RenderVec2(x: 2, y: 4)
        var accumulated = lhs

        XCTAssertEqual(lhs + rhs, RenderVec2(x: 8, y: -4))
        XCTAssertEqual(lhs - rhs, RenderVec2(x: 4, y: -12))
        XCTAssertEqual(lhs * 2, RenderVec2(x: 12, y: -16))
        XCTAssertEqual(2 * lhs, RenderVec2(x: 12, y: -16))
        XCTAssertEqual(lhs / 2, RenderVec2(x: 3, y: -4))
        XCTAssertEqual(lhs * rhs, RenderVec2(x: 12, y: -32))
        XCTAssertEqual(lhs / rhs, RenderVec2(x: 3, y: -2))
        accumulated += rhs
        XCTAssertEqual(accumulated, RenderVec2(x: 8, y: -4))
        accumulated -= rhs
        XCTAssertEqual(accumulated, lhs)
    }

    func testRenderVec2DistanceNormalizationRotationAndLerpMatchRaymath() {
        let value = RenderVec2(x: 3, y: 4)

        XCTAssertEqual(value.lengthSquared, 25)
        XCTAssertEqual(value.distance(to: RenderVec2(x: 6, y: 8)), 5, accuracy: 0.0001)
        XCTAssertEqual(value.distanceSquared(to: RenderVec2(x: 6, y: 8)), 25)
        XCTAssertEqual(value.normalized().length, 1, accuracy: 0.0001)
        assertEqual(
            RenderVec2(x: 1, y: 0).rotated(by: .pi / 2),
            RenderVec2(x: 0, y: 1)
        )
        XCTAssertEqual(
            value.lerp(to: RenderVec2(x: 7, y: 12), amount: 0.25),
            RenderVec2(x: 4, y: 6)
        )
    }

    func testRenderVec2NormalizationOnlyTreatsComputedZeroLengthAsDegenerate() {
        XCTAssertEqual(RenderVec2.zero.normalized(), .zero)

        let microscopic = RenderVec2(x: 1e-8, y: 0).normalized()
        XCTAssertEqual(microscopic, RenderVec2(x: 1, y: 0))
    }

    func testRenderVec3SupportsCallerArithmeticAndVectorOperations() {
        let lhs = RenderVec3(x: 6, y: -8, z: 10)
        let rhs = RenderVec3(x: 2, y: 4, z: -5)
        var accumulated = lhs

        XCTAssertEqual(lhs + rhs, RenderVec3(x: 8, y: -4, z: 5))
        XCTAssertEqual(lhs - rhs, RenderVec3(x: 4, y: -12, z: 15))
        XCTAssertEqual(lhs * 2, RenderVec3(x: 12, y: -16, z: 20))
        XCTAssertEqual(2 * lhs, RenderVec3(x: 12, y: -16, z: 20))
        XCTAssertEqual(lhs / 2, RenderVec3(x: 3, y: -4, z: 5))
        XCTAssertEqual(lhs * rhs, RenderVec3(x: 12, y: -32, z: -50))
        XCTAssertEqual(lhs / rhs, RenderVec3(x: 3, y: -2, z: -2))
        accumulated += rhs
        XCTAssertEqual(accumulated, RenderVec3(x: 8, y: -4, z: 5))
        XCTAssertEqual(lhs.dot(rhs), -70)
        XCTAssertEqual(
            RenderVec3(x: 1, y: 0, z: 0).cross(RenderVec3(x: 0, y: 1, z: 0)),
            RenderVec3(x: 0, y: 0, z: 1)
        )
    }

    func testRenderVec3PreservesExistingGameDirections() {
        XCTAssertEqual(RenderVec3.one, RenderVec3(x: 1, y: 1, z: 1))
        XCTAssertEqual(RenderVec3.forward, RenderVec3(x: 0, y: 0, z: 1))
        XCTAssertEqual(RenderVec3.backward, RenderVec3(x: 0, y: 0, z: -1))
        XCTAssertEqual(RenderVec3.left, RenderVec3(x: 1, y: 0, z: 0))
        XCTAssertEqual(RenderVec3.right, RenderVec3(x: -1, y: 0, z: 0))
    }

    func testRenderVec3DistanceNormalizationAndLerpMatchRaymath() {
        let value = RenderVec3(x: 2, y: 3, z: 6)

        XCTAssertEqual(value.length, 7, accuracy: 0.0001)
        XCTAssertEqual(value.lengthSquared, 49)
        XCTAssertEqual(value.distance(to: RenderVec3(x: 4, y: 6, z: 12)), 7, accuracy: 0.0001)
        XCTAssertEqual(value.distanceSquared(to: RenderVec3(x: 4, y: 6, z: 12)), 49)
        XCTAssertEqual(value.normalized().length, 1, accuracy: 0.0001)
        XCTAssertEqual(
            value.lerp(to: RenderVec3(x: 6, y: 7, z: 10), amount: 0.25),
            RenderVec3(x: 3, y: 4, z: 7)
        )
    }

    func testRenderVec3NormalizationOnlyTreatsComputedZeroLengthAsDegenerate() {
        XCTAssertEqual(RenderVec3.zero.normalized(), .zero)

        let microscopic = RenderVec3(x: 0, y: 1e-8, z: 0).normalized()
        XCTAssertEqual(microscopic, RenderVec3(x: 0, y: 1, z: 0))
    }

    func testQuaternionAxisAngleMultiplicationAndVectorRotationMatchRaymath() {
        let rotation = RenderQuaternion(axis: RenderVec3(x: 0, y: 2, z: 0), radians: .pi / 2)
        assertEqual(
            RenderVec3(x: 0, y: 0, z: 1).rotated(by: rotation),
            RenderVec3(x: 1, y: 0, z: 0)
        )

        XCTAssertEqual(
            RenderQuaternion(x: 1, y: 2, z: 3, w: 4)
                * RenderQuaternion(x: 5, y: 6, z: 7, w: 8),
            RenderQuaternion(x: 24, y: 48, z: 48, w: -6)
        )
    }

    func testQuaternionAxisAngleReturnsIdentityForZeroAxis() {
        XCTAssertEqual(RenderQuaternion(axis: .zero, radians: .pi), .identity)
    }

    func testMatrixTranslationRotationAndScaleMatchRaymathColumnLayout() {
        let translation = RenderMatrix4.translation(RenderVec3(x: 2, y: 3, z: 4))
        XCTAssertEqual(translation.c3, SIMD4(2, 3, 4, 1))

        let scale = RenderMatrix4.scale(RenderVec3(x: 5, y: 6, z: 7))
        XCTAssertEqual(scale.c0, SIMD4(5, 0, 0, 0))
        XCTAssertEqual(scale.c1, SIMD4(0, 6, 0, 0))
        XCTAssertEqual(scale.c2, SIMD4(0, 0, 7, 0))

        let rotation = RenderMatrix4.rotation(axis: .up, radians: .pi / 2)
        assertEqual(rotation.c0, SIMD4(0, 0, -1, 0))
        assertEqual(rotation.c2, SIMD4(1, 0, 0, 0))
    }

    func testMatrixRotationPreservesRaymathZeroAxisDegenerateResult() {
        let angle: Float = 0.75
        let rotation = RenderMatrix4.rotation(axis: .zero, radians: angle)
        let expectedDiagonal = cos(angle)

        XCTAssertEqual(rotation.c0.x, expectedDiagonal, accuracy: 0.0001)
        XCTAssertEqual(rotation.c1.y, expectedDiagonal, accuracy: 0.0001)
        XCTAssertEqual(rotation.c2.z, expectedDiagonal, accuracy: 0.0001)
        XCTAssertEqual(rotation.c3.w, 1)
    }

    func testMatrixMultiplicationMatchesRaymathOrderAndColumnMajorCABI() {
        let translation = RenderMatrix4.translation(RenderVec3(x: 2, y: 3, z: 4))
        let scale = RenderMatrix4.scale(RenderVec3(x: 5, y: 6, z: 7))

        let translationThenScale = translation * scale
        XCTAssertEqual(translationThenScale.c0, SIMD4(5, 0, 0, 0))
        XCTAssertEqual(translationThenScale.c1, SIMD4(0, 6, 0, 0))
        XCTAssertEqual(translationThenScale.c2, SIMD4(0, 0, 7, 0))
        XCTAssertEqual(translationThenScale.c3, SIMD4(10, 18, 28, 1))

        let scaleThenTranslation = scale * translation
        XCTAssertEqual(scaleThenTranslation.c3, SIMD4(2, 3, 4, 1))
    }

    func testScalarLerpAndRemapMatchRaymathIncludingUnclampedAndDegenerateRanges() {
        XCTAssertEqual(renderLerp(10, 20, amount: 1.5), 25)
        XCTAssertEqual(
            renderRemap(0.25, inputStart: 0, inputEnd: 1, outputStart: 10, outputEnd: 30),
            15
        )
        XCTAssertTrue(
            renderRemap(2, inputStart: 1, inputEnd: 1, outputStart: 0, outputEnd: 10).isInfinite
        )
    }

    func testRectangleAndCircleCollisionsMatchRaylibBoundaryRules() {
        let rect = RenderRect(x: 10, y: 20, width: 30, height: 40)

        XCTAssertTrue(rect.contains(RenderVec2(x: 10, y: 20)))
        XCTAssertFalse(rect.contains(RenderVec2(x: 40, y: 60)))
        XCTAssertTrue(rect.intersects(RenderRect(x: 39, y: 59, width: 2, height: 2)))
        XCTAssertFalse(rect.intersects(RenderRect(x: 40, y: 20, width: 2, height: 2)))
        XCTAssertTrue(RenderVec2(x: 3, y: 4).isInsideCircle(center: .zero, radius: 5))
        XCTAssertTrue(
            RenderVec2.circlesIntersect(
                center1: .zero,
                radius1: 2,
                center2: RenderVec2(x: 5, y: 0),
                radius2: 3
            )
        )
        XCTAssertTrue(rect.intersectsCircle(center: RenderVec2(x: 5, y: 20), radius: 5))
        XCTAssertFalse(rect.intersectsCircle(center: RenderVec2(x: 4.99, y: 20), radius: 5))
    }

    func testRenderColorAlphaReplacementPreservesRGBBytes() {
        XCTAssertEqual(
            RenderColor(r: 10, g: 20, b: 30, a: 40).withAlpha(50),
            RenderColor(r: 10, g: 20, b: 30, a: 50)
        )
    }

    private func assertEqual(
        _ actual: RenderVec2,
        _ expected: RenderVec2,
        accuracy: Float = 0.0001,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        XCTAssertEqual(actual.x, expected.x, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.y, expected.y, accuracy: accuracy, file: file, line: line)
    }

    private func assertEqual(
        _ actual: RenderVec3,
        _ expected: RenderVec3,
        accuracy: Float = 0.0001,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        XCTAssertEqual(actual.x, expected.x, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.y, expected.y, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.z, expected.z, accuracy: accuracy, file: file, line: line)
    }

    private func assertEqual(
        _ actual: SIMD4<Float>,
        _ expected: SIMD4<Float>,
        accuracy: Float = 0.0001,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        XCTAssertEqual(actual.x, expected.x, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.y, expected.y, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.z, expected.z, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.w, expected.w, accuracy: accuracy, file: file, line: line)
    }
}
