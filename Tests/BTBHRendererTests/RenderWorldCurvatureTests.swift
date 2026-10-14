import XCTest
@testable import BTBHRenderer

final class RenderWorldCurvatureTests: XCTestCase {
    private let center = RenderVec3(x: 10, y: 0, z: -4)

    func testFlatCurvatureLeavesPointsAlone() {
        let point = RenderVec3(x: 30, y: 2, z: 7)
        XCTAssertEqual(RenderWorldCurvature.bend(point, curvature: 0, center: center), point)
        XCTAssertEqual(RenderWorldCurvature.bend(point, curvature: .nan, center: center), point)
    }

    func testPlanetKeepsTheGroundUnderItsPointAndSinksIt() {
        let k: Float = 1 / 40
        let ground = RenderVec3(x: 40, y: 0, z: -4)
        let bent = RenderWorldCurvature.bend(ground, curvature: k, center: center)
        XCTAssertEqual(bent.x, ground.x, accuracy: 0.0001)
        XCTAssertEqual(bent.z, ground.z, accuracy: 0.0001)
        XCTAssertEqual(bent.y, -(1 - cos(30 * k)) / k, accuracy: 0.0001)
        // A rooftop over it leans out along the sphere's normal.
        let roof = RenderWorldCurvature.bend(RenderVec3(x: 40, y: 5, z: -4), curvature: k, center: center)
        XCTAssertGreaterThan(roof.x, bent.x)
        XCTAssertGreaterThan(roof.y, bent.y)
    }

    func testHyperbolicLookFitsTheWholePlaneInsideItsRing() {
        let k: Float = -1 / 20
        let span: Float = 40
        var previous: Float = 0
        // Float tanh reaches the ring itself far out; within a few spans it still grows.
        for distance in stride(from: Float(5), through: 160, by: 15) {
            let bent = RenderWorldCurvature.bend(
                RenderVec3(x: center.x + distance, y: 3, z: center.z), curvature: k, center: center)
            let compressed = bent.x - center.x
            XCTAssertLessThan(compressed, span)
            XCTAssertGreaterThan(compressed, previous)
            XCTAssertLessThanOrEqual(compressed, distance)
            XCTAssertLessThanOrEqual(bent.y, 3)
            previous = compressed
        }
    }

    func testGroundHitFindsTheFlatPointDrawnUnderTheRay() throws {
        let eye = RenderVec3(x: 12, y: 18, z: 14)
        for k: Float in [0, 1 / 35, 1 / 90, -1 / 22, -1 / 60] {
            for flat in [RenderVec3(x: 14, y: 0, z: -2), RenderVec3(x: 30, y: 0, z: -20), RenderVec3(x: -6, y: 0, z: -12)] {
                let drawn = RenderWorldCurvature.bend(flat, curvature: k, center: center)
                let ray = RenderRay(origin: eye, direction: (drawn - eye).normalized())
                let hit = try XCTUnwrap(RenderWorldCurvature.flatGroundHit(ray, curvature: k, center: center), "k=\(k)")
                XCTAssertEqual(hit.x, flat.x, accuracy: 0.05, "k=\(k)")
                XCTAssertEqual(hit.z, flat.z, accuracy: 0.05, "k=\(k)")
                XCTAssertEqual(hit.y, 0)
            }
        }
    }

    func testDrawnBoundsHoldEveryBentCorner() {
        let boxes = [
            RenderBounds3(minimum: RenderVec3(x: 20, y: 0, z: -10), maximum: RenderVec3(x: 26, y: 12, z: -2)),
            RenderBounds3(minimum: RenderVec3(x: -80, y: 0, z: 40), maximum: RenderVec3(x: -60, y: 30, z: 70)),
            RenderBounds3(minimum: RenderVec3(x: 5, y: -1, z: -8), maximum: RenderVec3(x: 15, y: 4, z: 0)),
        ]
        for k: Float in [1 / 25, 1 / 70, -1 / 15, -1 / 50] {
            for box in boxes {
                let drawn = RenderWorldCurvature.drawnBounds(box, curvature: k, center: center)
                for x in [box.minimum.x, box.maximum.x] {
                    for y in [box.minimum.y, box.maximum.y] {
                        for z in [box.minimum.z, box.maximum.z] {
                            let bent = RenderWorldCurvature.bend(RenderVec3(x: x, y: y, z: z), curvature: k, center: center)
                            XCTAssertGreaterThanOrEqual(bent.x, drawn.minimum.x - 0.001, "k=\(k)")
                            XCTAssertLessThanOrEqual(bent.x, drawn.maximum.x + 0.001, "k=\(k)")
                            XCTAssertGreaterThanOrEqual(bent.y, drawn.minimum.y - 0.001, "k=\(k)")
                            XCTAssertLessThanOrEqual(bent.y, drawn.maximum.y + 0.001, "k=\(k)")
                            XCTAssertGreaterThanOrEqual(bent.z, drawn.minimum.z - 0.001, "k=\(k)")
                            XCTAssertLessThanOrEqual(bent.z, drawn.maximum.z + 0.001, "k=\(k)")
                        }
                    }
                }
            }
        }
    }

    func testProjectionShowsAPointWhereItIsDrawn() {
        let camera = RenderCamera(
            position: RenderVec3(x: 0, y: 20, z: 20),
            target: center,
            up: .up,
            verticalFOVDegrees: 45,
            projection: .perspective,
            worldCurvature: 1 / 30
        )
        let flat = RenderVec3(x: 25, y: 0, z: -10)
        var flatCamera = camera
        flatCamera.worldCurvature = 0
        XCTAssertEqual(
            RenderCameraProjection.projectNormalized(flat, camera: camera, aspectRatio: 16.0 / 9.0),
            RenderCameraProjection.projectNormalized(
                RenderWorldCurvature.bend(flat, camera: camera), camera: flatCamera, aspectRatio: 16.0 / 9.0)
        )
    }
}
