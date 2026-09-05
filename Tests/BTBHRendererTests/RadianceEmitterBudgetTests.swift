import XCTest
@testable import BTBHRenderer

final class RadianceEmitterBudgetTests: XCTestCase {
    func testNeighborhoodUpperBoundDoesNotPruneRoundingAboveCeiling() {
        for energy: Float in [(30.0 / 9).nextDown, 30.0 / 9, (30.0 / 9).nextUp] {
            var input: [RadianceEmitter] = []
            for x in -1...1 {
                for z in -1...1 {
                    input.append(RadianceEmitter(
                        position: .init(x: Float(x) * 1.5, y: 0, z: Float(z) * 1.5),
                        color: .white,
                        energy: energy,
                        radius: 1,
                        lightClass: .pickup
                    ))
                }
            }
            XCTAssertEqual(
                RadianceEmitterBudgetPolicy.production.applying(to: input),
                ReferenceRadianceEmitterBudgetPolicy.production.applying(to: input)
            )
        }
    }

    func testSparseAndDenseBudgetsExactlyMatchOriginalScatterAlgorithm() {
        for spacing: Float in [0.1, 1.49, 3, 30] {
            var input: [RadianceEmitter] = []
            for index in 0..<256 {
                let lightClass = RadianceEmitterClass.allCases[index % 4]
                let position = RenderVec3(
                    x: Float(index % 17 - 8) * spacing,
                    y: 1,
                    z: Float(index / 17 - 8) * spacing
                )
                input.append(RadianceEmitter(
                    stableID: UInt64(index % 37),
                    position: position,
                    color: .white,
                    energy: index % 23 == 0 ? 50 : Float(index % 11) * 0.1,
                    directEnergy: 3,
                    radius: 1,
                    lightClass: lightClass
                ))
            }
            for ordered in [input, Array(input.reversed())] {
                XCTAssertEqual(
                    RadianceEmitterBudgetPolicy.production.applying(to: ordered),
                    ReferenceRadianceEmitterBudgetPolicy.production.applying(to: ordered)
                )
            }
        }
    }

    func testSparseWindowGatherPreservesSaturatedCoordinatesAndRounding() {
        var input: [RadianceEmitter] = []
        for x: Float in [-Float.greatestFiniteMagnitude, 0, Float.greatestFiniteMagnitude] {
            for z: Float in [-Float.greatestFiniteMagnitude, 0, Float.greatestFiniteMagnitude] {
                for energy: Float in [0.1, (30.0 / 9).nextDown, 30.0 / 9, (30.0 / 9).nextUp, 100] {
                    input.append(RadianceEmitter(
                        position: .init(x: x, y: 0, z: z),
                        color: .white,
                        energy: energy,
                        directEnergy: 7,
                        radius: 1,
                        lightClass: .pickup
                    ))
                }
            }
        }
        // Ensure the sparse gather branch remains selected at coordinate limits.
        for index in 0..<256 {
            input.append(makeEmitter(id: UInt64(index), x: Float(index) * 10 + 10, energy: 0.01))
        }
        XCTAssertEqual(
            RadianceEmitterBudgetPolicy.production.applying(to: input),
            ReferenceRadianceEmitterBudgetPolicy.production.applying(to: input)
        )
    }

    func testEmitterDefaultsToWorldClass() {
        XCTAssertEqual(makeEmitter(id: 1, energy: 1).lightClass, .world)
    }

    func testDensePickupNeighborhoodPreservesEverySourceAndCapsEnergy() {
        let input = (0..<3).map {
            makeEmitter(
                id: UInt64($0 + 1),
                energy: 20,
                directEnergy: 10,
                lightClass: .pickup
            )
        }

        let output = RadianceEmitterBudgetPolicy.production.applying(to: input)

        XCTAssertEqual(output.map(\.stableID), input.map(\.stableID))
        XCTAssertEqual(output.count, input.count)
        XCTAssertEqual(output.reduce(0) { $0 + $1.energy }, 30, accuracy: 0.001)
        XCTAssertTrue(output.allSatisfy { abs($0.directEnergy - 5) < 0.001 })
    }

    func testPerEmitterCapScalesTransientDirectEnergyBySameFactor() {
        let input = makeEmitter(
            id: 7,
            energy: 40,
            directEnergy: 20,
            lightClass: .transient
        )

        let output = RadianceEmitterBudgetPolicy.production.applying(to: [input])[0]

        XCTAssertEqual(output.energy, 18, accuracy: 0.001)
        XCTAssertEqual(output.directEnergy, 9, accuracy: 0.001)
        XCTAssertEqual(output.lightClass, .transient)
    }

    func testClassBudgetsAreIsolated() {
        let hero = makeEmitter(id: 1, energy: 30, lightClass: .hero)
        let pickups = [
            makeEmitter(id: 2, energy: 30, lightClass: .pickup),
            makeEmitter(id: 3, energy: 30, lightClass: .pickup),
        ]

        let output = RadianceEmitterBudgetPolicy.production.applying(
            to: [hero] + pickups
        )

        XCTAssertEqual(output[0].energy, 30, accuracy: 0.001)
        XCTAssertEqual(output.dropFirst().reduce(0) { $0 + $1.energy }, 30, accuracy: 0.001)
    }

    func testBoundaryNeighborhoodUsesStrictestContainingWindow() {
        let input = [
            makeEmitter(id: 1, x: -1.4, energy: 30, lightClass: .pickup),
            makeEmitter(id: 2, x: 1.6, energy: 30, lightClass: .pickup),
            makeEmitter(id: 3, x: 0.1, energy: 0.1, lightClass: .pickup),
        ]

        let output = RadianceEmitterBudgetPolicy.production.applying(to: input)

        XCTAssertLessThanOrEqual(
            output.reduce(0) { $0 + $1.energy },
            30.001
        )
    }

    func testBudgetResultIsIndependentOfInputPermutation() {
        let input = [
            makeEmitter(id: 30, x: 0.2, energy: 11, lightClass: .pickup),
            makeEmitter(id: 10, x: 0.4, energy: 13, lightClass: .pickup),
            makeEmitter(id: 20, x: 0.6, energy: 17, lightClass: .pickup),
            makeEmitter(id: 40, x: 0.8, energy: 19, lightClass: .pickup),
        ]
        let policy = RadianceEmitterBudgetPolicy.production

        let forward = Dictionary(uniqueKeysWithValues: policy.applying(to: input).map {
            ($0.stableID, $0.energy)
        })
        let reversed = Dictionary(uniqueKeysWithValues: policy.applying(to: Array(input.reversed())).map {
            ($0.stableID, $0.energy)
        })

        XCTAssertEqual(forward, reversed)
    }

    func testTenThousandLogicalSourcesArePreserved() {
        let input = (0..<10_000).map { index in
            makeEmitter(
                id: UInt64(index + 1),
                energy: 1,
                lightClass: .pickup
            )
        }

        let output = RadianceEmitterBudgetPolicy.production.applying(to: input)

        XCTAssertEqual(output.count, input.count)
        XCTAssertEqual(output.first?.stableID, 1)
        XCTAssertEqual(output.last?.stableID, 10_000)
        XCTAssertEqual(output.reduce(0) { $0 + $1.energy }, 30, accuracy: 0.01)
    }

    private func makeEmitter(
        id: UInt64,
        x: Float = 0,
        energy: Float,
        directEnergy: Float = 0,
        lightClass: RadianceEmitterClass = .world
    ) -> RadianceEmitter {
        RadianceEmitter(
            stableID: id,
            position: .init(x: x, y: 0, z: 0),
            color: .white,
            energy: energy,
            directEnergy: directEnergy,
            radius: 1,
            lightClass: lightClass
        )
    }
}
