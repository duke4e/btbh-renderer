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

        // A fight passes about 4 000 sources every frame (2026-10-17: this function was 5 % of the main thread):
        // the sort moves small keys, not whole emitters, and cells live in `BudgetCellTable`, not in SipHash
        // dictionaries. Every sum is added in the same order as before, so the result is bit for bit the same.
        var entries: [BudgetEntry] = []
        entries.reserveCapacity(emitters.count)
        for (index, emitter) in emitters.enumerated() {
            entries.append(BudgetEntry(
                originalIndex: index,
                cell: cell(for: emitter.position, lightClass: emitter.lightClass),
                stableID: emitter.stableID,
                positionXBits: emitter.position.x.bitPattern,
                positionZBits: emitter.position.z.bitPattern,
                energyBits: emitter.energy.bitPattern
            ))
        }
        entries.sort(by: BudgetEntry.precedes)

        // The sort puts each cell's sources next to each other (the cell leads the order), so a cell's energy is
        // added up in a local, in the same order as before, and stored once.
        var cellRanges: [(cell: BudgetCell, range: Range<Int>)] = []
        var cellEnergy = BudgetCellTable(minimumCapacity: entries.count)
        var start = 0
        while start < entries.count {
            let cell = entries[start].cell
            var end = start + 1
            while end < entries.count, entries[end].cell == cell { end += 1 }
            cellRanges.append((cell, start..<end))
            let budget = budget(for: cell.lightClass)
            if budget.isUsable {
                var total: Float = 0
                var counted = false
                for entry in entries[start..<end] {
                    let energy = emitters[entry.originalIndex].energy
                    guard energy.isFinite, energy > 0 else { continue }
                    total += min(energy, budget.maximumEmitterEnergy)
                    counted = true
                }
                if counted { cellEnergy.set(cell, total) }
            }
            start = end
        }

        // A 3x3 window cannot exceed its budget unless at least one cell's
        // energy, added nine times, exceeds it. Use repeated Float additions
        // to keep this bound conservative at rounding and saturated-coordinate
        // boundaries. Authored city windows are mostly below this bound.
        var candidateWindows = BudgetCellTable(minimumCapacity: 64)
        for occupiedIndex in 0..<cellEnergy.count {
            let occupiedCell = cellEnergy.cells[occupiedIndex]
            let energy = cellEnergy.values[occupiedIndex]
            var upperBound: Float = 0
            for _ in 0..<9 { upperBound += energy }
            guard upperBound > budget(for: occupiedCell.lightClass)
                .maximumNeighborhoodEnergy else { continue }
            for centerZOffset in Int64(-1)...Int64(1) {
                for centerXOffset in Int64(-1)...Int64(1) {
                    candidateWindows.insert(BudgetCell(
                        lightClass: occupiedCell.lightClass,
                        x: Self.offset(occupiedCell.x, by: centerXOffset),
                        z: Self.offset(occupiedCell.z, by: centerZOffset)
                    ))
                }
            }
            // Dense inputs are cheaper with the original scatter pass.
            if candidateWindows.count > cellEnergy.count { break }
        }

        var windowEnergy = BudgetCellTable(minimumCapacity: 64)
        if candidateWindows.count <= cellEnergy.count {
            for center in candidateWindows.cells {
                let total = Self.windowEnergy(at: center, from: cellEnergy)
                if total > budget(for: center.lightClass).maximumNeighborhoodEnergy {
                    windowEnergy.set(center, total)
                }
            }
        } else {
            let order = (0..<cellEnergy.count).sorted {
                BudgetCell.precedes(cellEnergy.cells[$0], cellEnergy.cells[$1])
            }
            for occupiedIndex in order {
                let occupiedCell = cellEnergy.cells[occupiedIndex]
                let energy = cellEnergy.values[occupiedIndex]
                for centerZOffset in Int64(-1)...Int64(1) {
                    for centerXOffset in Int64(-1)...Int64(1) {
                        let center = BudgetCell(
                            lightClass: occupiedCell.lightClass,
                            x: Self.offset(occupiedCell.x, by: centerXOffset),
                            z: Self.offset(occupiedCell.z, by: centerZOffset)
                        )
                        windowEnergy.add(energy, to: center)
                    }
                }
            }
        }

        var result = emitters
        for (cell, range) in cellRanges {
            let budget = budget(for: cell.lightClass)
            guard budget.isUsable else { continue }
            var neighborhoodScale: Float = 1
            // Every 3x3 window containing this cell's sources contributes a bound.
            // Taking the strictest one makes every occupied neighborhood obey
            // the ceiling, including sources sitting on cell boundaries. The
            // windows are the cell's, so every source in it shares the bound.
            if !windowEnergy.isEmpty {
                for centerZOffset in Int64(-1)...Int64(1) {
                    for centerXOffset in Int64(-1)...Int64(1) {
                        let center = BudgetCell(
                            lightClass: cell.lightClass,
                            x: Self.offset(cell.x, by: centerXOffset),
                            z: Self.offset(cell.z, by: centerZOffset)
                        )
                        let total = windowEnergy.value(for: center) ?? 0
                        if total > budget.maximumNeighborhoodEnergy {
                            neighborhoodScale = min(
                                neighborhoodScale,
                                budget.maximumNeighborhoodEnergy / total
                            )
                        }
                    }
                }
            }

            for entry in entries[range] {
                let emitter = emitters[entry.originalIndex]
                let energy = emitter.energy
                guard energy.isFinite, energy > 0 else { continue }
                let emitterScale = min(1, budget.maximumEmitterEnergy / energy)
                let scale = emitterScale * neighborhoodScale
                guard scale < 1 else { continue }
                result[entry.originalIndex] = emitter.scaledEnergy(by: scale)
            }
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

    private static func windowEnergy(
        at center: BudgetCell,
        from cellEnergy: BudgetCellTable
    ) -> Float {
        var total: Float = 0
        // Match the scatter pass's sorted source-cell accumulation exactly.
        for x in offset(center.x, by: -1)...offset(center.x, by: 1) {
            for z in offset(center.z, by: -1)...offset(center.z, by: 1) {
                let cell = BudgetCell(lightClass: center.lightClass, x: x, z: z)
                guard let energy = cellEnergy.value(for: cell) else { continue }
                // At Int64 limits, two source offsets collapse onto one center.
                let xCopies = x == center.x && (x == .min || x == .max) ? 2 : 1
                let zCopies = z == center.z && (z == .min || z == .max) ? 2 : 1
                for _ in 0..<(xCopies * zCopies) { total += energy }
            }
        }
        return total
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
    let cell: BudgetCell
    let stableID: UInt64
    let positionXBits: UInt32
    let positionZBits: UInt32
    let energyBits: UInt32

    static func precedes(_ lhs: Self, _ rhs: Self) -> Bool {
        let leftClass = lhs.cell.lightClass.rawValue
        let rightClass = rhs.cell.lightClass.rawValue
        if leftClass != rightClass { return leftClass < rightClass }
        if lhs.cell.x != rhs.cell.x { return lhs.cell.x < rhs.cell.x }
        if lhs.cell.z != rhs.cell.z { return lhs.cell.z < rhs.cell.z }
        if lhs.stableID != rhs.stableID { return lhs.stableID < rhs.stableID }
        if lhs.positionXBits != rhs.positionXBits { return lhs.positionXBits < rhs.positionXBits }
        if lhs.positionZBits != rhs.positionZBits { return lhs.positionZBits < rhs.positionZBits }
        if lhs.energyBits != rhs.energyBits { return lhs.energyBits < rhs.energyBits }
        return lhs.originalIndex < rhs.originalIndex
    }
}

/// Cells to values, open addressing with a cheap integer mix instead of a `Dictionary`'s SipHash. Iterates in
/// insertion order (`cells`, `values`); nothing in the policy depends on that order but the dense/sparse switch.
private struct BudgetCellTable {
    private(set) var cells: [BudgetCell] = []
    private(set) var values: [Float] = []
    private var slots: [Int32]
    private var mask: Int

    init(minimumCapacity: Int) {
        var capacity = 16
        while capacity < minimumCapacity * 2 { capacity <<= 1 }
        slots = Array(repeating: -1, count: capacity)
        mask = capacity - 1
        cells.reserveCapacity(minimumCapacity)
        values.reserveCapacity(minimumCapacity)
    }

    var count: Int { cells.count }
    var isEmpty: Bool { cells.isEmpty }

    func value(for cell: BudgetCell) -> Float? {
        var slot = Self.hash(cell) & mask
        while true {
            let stored = Int(slots[slot])
            if stored < 0 { return nil }
            if cells[stored] == cell { return values[stored] }
            slot = (slot + 1) & mask
        }
    }

    mutating func set(_ cell: BudgetCell, _ value: Float) {
        let slot = findSlot(cell)
        let stored = Int(slots[slot])
        if stored >= 0 {
            values[stored] = value
        } else {
            append(cell, value, at: slot)
        }
    }

    /// `value += energy`, starting from 0 as `Dictionary`'s `[cell, default: 0] += energy` does.
    mutating func add(_ energy: Float, to cell: BudgetCell) {
        let slot = findSlot(cell)
        let stored = Int(slots[slot])
        if stored >= 0 {
            values[stored] += energy
        } else {
            var value: Float = 0
            value += energy
            append(cell, value, at: slot)
        }
    }

    mutating func insert(_ cell: BudgetCell) {
        let slot = findSlot(cell)
        if slots[slot] < 0 { append(cell, 0, at: slot) }
    }

    private func findSlot(_ cell: BudgetCell) -> Int {
        var slot = Self.hash(cell) & mask
        while true {
            let stored = Int(slots[slot])
            if stored < 0 || cells[stored] == cell { return slot }
            slot = (slot + 1) & mask
        }
    }

    private mutating func append(_ cell: BudgetCell, _ value: Float, at slot: Int) {
        slots[slot] = Int32(cells.count)
        cells.append(cell)
        values.append(value)
        if cells.count * 2 > slots.count { grow() }
    }

    private mutating func grow() {
        let capacity = slots.count * 2
        slots = Array(repeating: -1, count: capacity)
        mask = capacity - 1
        for (index, cell) in cells.enumerated() {
            var slot = Self.hash(cell) & mask
            while slots[slot] >= 0 { slot = (slot + 1) & mask }
            slots[slot] = Int32(index)
        }
    }

    private static func hash(_ cell: BudgetCell) -> Int {
        var value = UInt64(bitPattern: cell.x) &* 0x9E37_79B9_7F4A_7C15
        value ^= (UInt64(bitPattern: cell.z) &+ UInt64(cell.lightClass.rawValue) &* 0x632B_E59B_D9B4_E019)
            &* 0xD6E8_FEB8_6659_FD93
        value ^= value >> 32
        value &*= 0x9E37_79B9_7F4A_7C15
        value ^= value >> 29
        return Int(truncatingIfNeeded: value)
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
