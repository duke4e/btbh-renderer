import Foundation

public struct RadianceEmitterClassBudget: Sendable, Equatable {
    public let maximumEmitterEnergy: Float
    public let maximumNeighborhoodEnergy: Float

    public init(
        maximumEmitterEnergy: Float,
        maximumNeighborhoodEnergy: Float
    ) {
        self.maximumEmitterEnergy = maximumEmitterEnergy
        self.maximumNeighborhoodEnergy = maximumNeighborhoodEnergy
    }
}

/// A deterministic, count-preserving spatial energy policy for RC sources.
public struct RadianceEmitterBudgetPolicy: Sendable, Equatable {
    public let cellSize: Float
    public let hero: RadianceEmitterClassBudget
    public let world: RadianceEmitterClassBudget
    public let pickup: RadianceEmitterClassBudget
    public let transient: RadianceEmitterClassBudget

    public static let production = RadianceEmitterBudgetPolicy(
        cellSize: 1.5,
        hero: .init(
            maximumEmitterEnergy: 30,
            maximumNeighborhoodEnergy: 45
        ),
        world: .init(
            maximumEmitterEnergy: 30,
            maximumNeighborhoodEnergy: 90
        ),
        pickup: .init(
            maximumEmitterEnergy: 30,
            maximumNeighborhoodEnergy: 30
        ),
        transient: .init(
            maximumEmitterEnergy: 18,
            maximumNeighborhoodEnergy: 30
        )
    )

    public init(
        cellSize: Float,
        hero: RadianceEmitterClassBudget,
        world: RadianceEmitterClassBudget,
        pickup: RadianceEmitterClassBudget,
        transient: RadianceEmitterClassBudget
    ) {
        self.cellSize = cellSize
        self.hero = hero
        self.world = world
        self.pickup = pickup
        self.transient = transient
    }

    public func applying(to emitters: [RadianceEmitter]) -> [RadianceEmitter] {
        guard !emitters.isEmpty,
              cellSize.isFinite,
              cellSize > 0 else {
            return emitters
        }

        let entries = emitters.enumerated().map { index, emitter in
            BudgetEntry(
                originalIndex: index,
                emitter: emitter,
                cell: cell(
                    for: emitter.position,
                    lightClass: emitter.lightClass
                )
            )
        }.sorted(by: BudgetEntry.precedes)

        var cellEnergy: [BudgetCell: Float] = [:]
        cellEnergy.reserveCapacity(entries.count)
        for entry in entries {
            let energy = entry.emitter.energy
            guard energy.isFinite, energy > 0 else { continue }
            let budget = budget(for: entry.emitter.lightClass)
            guard budget.isUsable else { continue }
            cellEnergy[entry.cell, default: 0] += min(
                energy,
                budget.maximumEmitterEnergy
            )
        }

        var windowEnergy: [BudgetCell: Float] = [:]
        windowEnergy.reserveCapacity(cellEnergy.count * 4)
        for occupiedCell in cellEnergy.keys.sorted(by: BudgetCell.precedes) {
            let energy = cellEnergy[occupiedCell, default: 0]
            for centerZOffset in Int64(-1)...Int64(1) {
                for centerXOffset in Int64(-1)...Int64(1) {
                    let center = BudgetCell(
                        lightClass: occupiedCell.lightClass,
                        x: Self.offset(occupiedCell.x, by: centerXOffset),
                        z: Self.offset(occupiedCell.z, by: centerZOffset)
                    )
                    windowEnergy[center, default: 0] += energy
                }
            }
        }

        var result = emitters
        for entry in entries {
            let emitter = entry.emitter
            let energy = emitter.energy
            let budget = budget(for: emitter.lightClass)
            guard energy.isFinite, energy > 0, budget.isUsable else { continue }

            let emitterScale = min(1, budget.maximumEmitterEnergy / energy)
            var neighborhoodScale: Float = 1
            // Every 3x3 window containing this source contributes a bound.
            // Taking the strictest one makes every occupied neighborhood obey
            // the ceiling, including sources sitting on cell boundaries.
            for centerZOffset in Int64(-1)...Int64(1) {
                for centerXOffset in Int64(-1)...Int64(1) {
                    let center = BudgetCell(
                        lightClass: entry.cell.lightClass,
                        x: Self.offset(entry.cell.x, by: centerXOffset),
                        z: Self.offset(entry.cell.z, by: centerZOffset)
                    )
                    let total = windowEnergy[center, default: 0]
                    if total > budget.maximumNeighborhoodEnergy {
                        neighborhoodScale = min(
                            neighborhoodScale,
                            budget.maximumNeighborhoodEnergy / total
                        )
                    }
                }
            }

            let scale = emitterScale * neighborhoodScale
            guard scale < 1 else { continue }
            result[entry.originalIndex] = emitter.scaledEnergy(by: scale)
        }
        return result
    }

    private func budget(
        for lightClass: RadianceEmitterClass
    ) -> RadianceEmitterClassBudget {
        switch lightClass {
        case .hero: hero
        case .world: world
        case .pickup: pickup
        case .transient: transient
        }
    }

    private func cell(
        for position: RenderVec3,
        lightClass: RadianceEmitterClass
    ) -> BudgetCell {
        BudgetCell(
            lightClass: lightClass,
            x: Self.coordinate(position.x, cellSize: cellSize),
            z: Self.coordinate(position.z, cellSize: cellSize)
        )
    }

    private static func coordinate(_ value: Float, cellSize: Float) -> Int64 {
        guard value.isFinite else { return 0 }
        let coordinate = floor(Double(value) / Double(cellSize))
        if coordinate >= Double(Int64.max) { return Int64.max }
        if coordinate <= Double(Int64.min) { return Int64.min }
        return Int64(coordinate)
    }

    private static func offset(_ value: Int64, by offset: Int64) -> Int64 {
        let (result, overflow) = value.addingReportingOverflow(offset)
        guard overflow else { return result }
        return offset > 0 ? Int64.max : Int64.min
    }
}

private struct BudgetCell: Hashable {
    let lightClass: RadianceEmitterClass
    let x: Int64
    let z: Int64

    static func precedes(_ lhs: Self, _ rhs: Self) -> Bool {
        if lhs.lightClass.rawValue != rhs.lightClass.rawValue {
            return lhs.lightClass.rawValue < rhs.lightClass.rawValue
        }
        if lhs.x != rhs.x { return lhs.x < rhs.x }
        return lhs.z < rhs.z
    }
}

private struct BudgetEntry {
    let originalIndex: Int
    let emitter: RadianceEmitter
    let cell: BudgetCell

    static func precedes(_ lhs: Self, _ rhs: Self) -> Bool {
        let leftClass = lhs.emitter.lightClass.rawValue
        let rightClass = rhs.emitter.lightClass.rawValue
        if leftClass != rightClass { return leftClass < rightClass }
        if lhs.cell.x != rhs.cell.x { return lhs.cell.x < rhs.cell.x }
        if lhs.cell.z != rhs.cell.z { return lhs.cell.z < rhs.cell.z }
        if lhs.emitter.stableID != rhs.emitter.stableID {
            return lhs.emitter.stableID < rhs.emitter.stableID
        }
        let leftPosition = lhs.emitter.position
        let rightPosition = rhs.emitter.position
        if leftPosition.x.bitPattern != rightPosition.x.bitPattern {
            return leftPosition.x.bitPattern < rightPosition.x.bitPattern
        }
        if leftPosition.z.bitPattern != rightPosition.z.bitPattern {
            return leftPosition.z.bitPattern < rightPosition.z.bitPattern
        }
        if lhs.emitter.energy.bitPattern != rhs.emitter.energy.bitPattern {
            return lhs.emitter.energy.bitPattern < rhs.emitter.energy.bitPattern
        }
        return lhs.originalIndex < rhs.originalIndex
    }
}

private extension RadianceEmitterClassBudget {
    var isUsable: Bool {
        maximumEmitterEnergy.isFinite && maximumEmitterEnergy > 0 &&
            maximumNeighborhoodEnergy.isFinite && maximumNeighborhoodEnergy > 0
    }
}

private extension RadianceEmitter {
    func scaledEnergy(by scale: Float) -> RadianceEmitter {
        RadianceEmitter(
            stableID: stableID,
            position: position,
            color: color,
            energy: energy * scale,
            directEnergy: directEnergy * scale,
            radius: radius,
            height: height,
            shape: shape,
            lightClass: lightClass,
            direction: direction,
            innerCutoff: innerCutoff,
            outerCutoff: outerCutoff
        )
    }
}
