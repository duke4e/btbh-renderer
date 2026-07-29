import BTBHRenderer
import Foundation

struct GPUModelMeshGeometry {
    let positions: [RenderVec3]
    let indices: [UInt32]
}

enum GPUModelFootprintExtractor {
    private struct QuantizedPoint: Hashable {
        let x: Int32
        let y: Int32

        init(_ point: RenderVec2, epsilon: Float) {
            let inverse = 1.0 / max(1e-6, epsilon)
            x = Int32((point.x * inverse).rounded())
            y = Int32((point.y * inverse).rounded())
        }
    }

    private struct EdgeKey: Hashable {
        let a: QuantizedPoint
        let b: QuantizedPoint

        init(_ first: QuantizedPoint, _ second: QuantizedPoint) {
            if first.x < second.x ||
                (first.x == second.x && first.y <= second.y) {
                a = first
                b = second
            } else {
                a = second
                b = first
            }
        }
    }

    private struct GridPoint: Hashable {
        let x: Int
        let y: Int
    }

    private struct GridEdgeKey: Hashable {
        let a: GridPoint
        let b: GridPoint

        init(_ first: GridPoint, _ second: GridPoint) {
            if first.x < second.x ||
                (first.x == second.x && first.y <= second.y) {
                a = first
                b = second
            } else {
                a = second
                b = first
            }
        }
    }

    static func extract(
        bounds: RenderBounds3,
        meshes: [GPUModelMeshGeometry]
    ) -> (points: [RenderVec2], kind: ModelFootprintKind) {
        var minimumY = Float.greatestFiniteMagnitude
        var maximumY = -Float.greatestFiniteMagnitude
        for mesh in meshes {
            for position in mesh.positions {
                if position.y < minimumY { minimumY = position.y }
                if position.y > maximumY { maximumY = position.y }
            }
        }

        guard minimumY != .greatestFiniteMagnitude else {
            return (boundsFootprint(bounds), .bounds)
        }

        let heightRange = max(0.01, maximumY - minimumY)
        let floorThreshold = max(0.65, heightRange * 0.03)
        let floorMaximumY = minimumY + min(floorThreshold, 0.65)
        var floorTriangles: [(RenderVec2, RenderVec2, RenderVec2)] = []
        floorTriangles.reserveCapacity(128)

        var points: [RenderVec2] = []
        var edgeCounts: [EdgeKey: Int] = [:]
        var pointLookup: [QuantizedPoint: RenderVec2] = [:]

        let extentX = abs(bounds.maximum.x - bounds.minimum.x)
        let extentZ = abs(bounds.maximum.z - bounds.minimum.z)
        let maximumExtent = max(0.01, max(extentX, extentZ))
        let quantizationEpsilon = max(0.0005, maximumExtent * 0.0015)

        func addEdge(_ first: RenderVec2, _ second: RenderVec2) {
            let quantizedFirst = QuantizedPoint(
                first,
                epsilon: quantizationEpsilon
            )
            let quantizedSecond = QuantizedPoint(
                second,
                epsilon: quantizationEpsilon
            )
            guard quantizedFirst != quantizedSecond else { return }
            pointLookup[quantizedFirst] =
                pointLookup[quantizedFirst] ?? first
            pointLookup[quantizedSecond] =
                pointLookup[quantizedSecond] ?? second
            edgeCounts[
                EdgeKey(quantizedFirst, quantizedSecond),
                default: 0
            ] += 1
        }

        for mesh in meshes {
            let emitTriangle: (
                RenderVec3,
                RenderVec3,
                RenderVec3
            ) -> Void = { first, second, third in
                guard first.y <= floorMaximumY,
                      second.y <= floorMaximumY,
                      third.y <= floorMaximumY else {
                    return
                }
                let first2D = RenderVec2(x: first.x, y: first.z)
                let second2D = RenderVec2(x: second.x, y: second.z)
                let third2D = RenderVec2(x: third.x, y: third.z)
                floorTriangles.append((first2D, second2D, third2D))
                points.append(first2D)
                points.append(second2D)
                points.append(third2D)
                addEdge(first2D, second2D)
                addEdge(second2D, third2D)
                addEdge(third2D, first2D)
            }

            if mesh.indices.count >= 3 {
                for index in stride(
                    from: 0,
                    to: mesh.indices.count - 2,
                    by: 3
                ) {
                    let first = Int(mesh.indices[index])
                    let second = Int(mesh.indices[index + 1])
                    let third = Int(mesh.indices[index + 2])
                    guard mesh.positions.indices.contains(first),
                          mesh.positions.indices.contains(second),
                          mesh.positions.indices.contains(third) else {
                        continue
                    }
                    emitTriangle(
                        mesh.positions[first],
                        mesh.positions[second],
                        mesh.positions[third]
                    )
                }
            } else if mesh.positions.count >= 3 {
                for index in stride(
                    from: 0,
                    to: mesh.positions.count - 2,
                    by: 3
                ) {
                    emitTriangle(
                        mesh.positions[index],
                        mesh.positions[index + 1],
                        mesh.positions[index + 2]
                    )
                }
            }
        }

        guard !floorTriangles.isEmpty else {
            return legacyConvexFootprint(bounds: bounds, meshes: meshes)
        }

        let raster = rasterFootprint(
            floorTriangles: floorTriangles,
            minimumX: bounds.minimum.x,
            maximumX: bounds.maximum.x,
            minimumZ: bounds.minimum.z,
            maximumZ: bounds.maximum.z
        )
        if raster.count >= 3 {
            return (raster, .raster)
        }

        let boundary = floorTriangleBoundary(
            edgeCounts: edgeCounts,
            pointLookup: pointLookup
        )
        if boundary.count >= 3 {
            return (boundary, .concaveHull)
        }

        if points.count >= 3 {
            return (convexHull(points), .convexHull)
        }

        return legacyConvexFootprint(bounds: bounds, meshes: meshes)
    }

    private static func boundsFootprint(
        _ bounds: RenderBounds3
    ) -> [RenderVec2] {
        [
            .init(x: bounds.minimum.x, y: bounds.minimum.z),
            .init(x: bounds.maximum.x, y: bounds.minimum.z),
            .init(x: bounds.maximum.x, y: bounds.maximum.z),
            .init(x: bounds.minimum.x, y: bounds.maximum.z),
        ]
    }

    private static func legacyConvexFootprint(
        bounds: RenderBounds3,
        meshes: [GPUModelMeshGeometry]
    ) -> (points: [RenderVec2], kind: ModelFootprintKind) {
        var minimumY = Float.greatestFiniteMagnitude
        var maximumY = -Float.greatestFiniteMagnitude
        for mesh in meshes {
            for position in mesh.positions {
                if position.y < minimumY { minimumY = position.y }
                if position.y > maximumY { maximumY = position.y }
            }
        }

        guard minimumY != .greatestFiniteMagnitude else {
            return ([], .convexHull)
        }

        let heightRange = max(0.01, maximumY - minimumY)
        let floorThreshold = max(0.65, heightRange * 0.03)
        let floorMaximumY = minimumY + min(floorThreshold, 0.65)
        var points: [RenderVec2] = []
        for mesh in meshes {
            for position in mesh.positions where position.y <= floorMaximumY {
                points.append(.init(x: position.x, y: position.z))
            }
        }

        guard points.count >= 3 else {
            return (boundsFootprint(bounds), .bounds)
        }
        return (convexHull(points), .convexHull)
    }

    private static func rasterFootprint(
        floorTriangles: [(RenderVec2, RenderVec2, RenderVec2)],
        minimumX: Float,
        maximumX: Float,
        minimumZ: Float,
        maximumZ: Float
    ) -> [RenderVec2] {
        guard !floorTriangles.isEmpty else { return [] }

        let extentX = max(0.01, maximumX - minimumX)
        let extentZ = max(0.01, maximumZ - minimumZ)
        let maximumExtent = max(extentX, extentZ)

        var cell = max(0.02, min(0.12, maximumExtent / 80.0))
        let maximumCellsPerAxis = 196
        if Int(ceil(extentX / cell)) > maximumCellsPerAxis ||
            Int(ceil(extentZ / cell)) > maximumCellsPerAxis {
            cell = max(extentX, extentZ) / Float(maximumCellsPerAxis)
        }

        let width = max(1, Int(ceil(extentX / cell)))
        let height = max(1, Int(ceil(extentZ / cell)))
        var occupied = Set<GridPoint>()

        for triangle in floorTriangles {
            let triangleMinimumX = min(
                triangle.0.x,
                min(triangle.1.x, triangle.2.x)
            )
            let triangleMaximumX = max(
                triangle.0.x,
                max(triangle.1.x, triangle.2.x)
            )
            let triangleMinimumZ = min(
                triangle.0.y,
                min(triangle.1.y, triangle.2.y)
            )
            let triangleMaximumZ = max(
                triangle.0.y,
                max(triangle.1.y, triangle.2.y)
            )

            let minimumCellX = max(
                0,
                Int(floor((triangleMinimumX - minimumX) / cell)) - 1
            )
            let maximumCellX = min(
                width - 1,
                Int(ceil((triangleMaximumX - minimumX) / cell)) + 1
            )
            let minimumCellY = max(
                0,
                Int(floor((triangleMinimumZ - minimumZ) / cell)) - 1
            )
            let maximumCellY = min(
                height - 1,
                Int(ceil((triangleMaximumZ - minimumZ) / cell)) + 1
            )

            guard maximumCellX >= minimumCellX,
                  maximumCellY >= minimumCellY else {
                continue
            }

            for y in minimumCellY...maximumCellY {
                let centerZ = minimumZ + (Float(y) + 0.5) * cell
                for x in minimumCellX...maximumCellX {
                    let centerX = minimumX + (Float(x) + 0.5) * cell
                    if point(
                        .init(x: centerX, y: centerZ),
                        isInside: triangle
                    ) {
                        occupied.insert(.init(x: x, y: y))
                    }
                }
            }
        }

        guard !occupied.isEmpty else { return [] }
        return gridBoundary(
            occupied: occupied,
            minimumX: minimumX,
            minimumZ: minimumZ,
            cell: cell
        )
    }

    private static func point(
        _ point: RenderVec2,
        isInside triangle: (RenderVec2, RenderVec2, RenderVec2)
    ) -> Bool {
        let first = RenderVec2(
            x: triangle.2.x - triangle.0.x,
            y: triangle.2.y - triangle.0.y
        )
        let second = RenderVec2(
            x: triangle.1.x - triangle.0.x,
            y: triangle.1.y - triangle.0.y
        )
        let relative = RenderVec2(
            x: point.x - triangle.0.x,
            y: point.y - triangle.0.y
        )

        let dot00 = first.x * first.x + first.y * first.y
        let dot01 = first.x * second.x + first.y * second.y
        let dot02 = first.x * relative.x + first.y * relative.y
        let dot11 = second.x * second.x + second.y * second.y
        let dot12 = second.x * relative.x + second.y * relative.y

        let denominator = dot00 * dot11 - dot01 * dot01
        guard abs(denominator) >= 1e-8 else { return false }
        let inverse = 1.0 / denominator
        let u = (dot11 * dot02 - dot01 * dot12) * inverse
        let v = (dot00 * dot12 - dot01 * dot02) * inverse
        return u >= -1e-4 && v >= -1e-4 && u + v <= 1.0001
    }

    private static func gridBoundary(
        occupied: Set<GridPoint>,
        minimumX: Float,
        minimumZ: Float,
        cell: Float
    ) -> [RenderVec2] {
        var edgeCounts: [GridEdgeKey: Int] = [:]
        for point in occupied.sorted(by: gridPointLessThan) {
            let bottomLeft = GridPoint(x: point.x, y: point.y)
            let bottomRight = GridPoint(x: point.x + 1, y: point.y)
            let topRight = GridPoint(x: point.x + 1, y: point.y + 1)
            let topLeft = GridPoint(x: point.x, y: point.y + 1)
            addGridEdge(&edgeCounts, bottomLeft, bottomRight)
            addGridEdge(&edgeCounts, bottomRight, topRight)
            addGridEdge(&edgeCounts, topRight, topLeft)
            addGridEdge(&edgeCounts, topLeft, bottomLeft)
        }

        var adjacency: [GridPoint: [GridPoint]] = [:]
        for (edge, _) in edgeCounts
            .filter({ $0.value == 1 })
            .sorted(by: { gridEdgeKeyLessThan($0.key, $1.key) }) {
            adjacency[edge.a, default: []].append(edge.b)
            adjacency[edge.b, default: []].append(edge.a)
        }
        guard !adjacency.isEmpty else { return [] }
        for point in adjacency.keys {
            adjacency[point]!.sort(by: gridPointLessThan)
        }

        var visited = Set<GridEdgeKey>()
        var best: [RenderVec2] = []
        var bestArea: Float = 0

        let startEntries = adjacency.keys
            .sorted(by: gridPointLessThan)
            .compactMap { key in
                adjacency[key].map { (key: key, value: $0) }
            }
        for (start, nextPoints) in startEntries {
            for next in nextPoints {
                let seed = GridEdgeKey(start, next)
                guard !visited.contains(seed) else { continue }

                var loop = [start]
                var previous = start
                var current = next
                let guardLimit = max(32, adjacency.count * 6)
                var guardCounter = 0

                while guardCounter < guardLimit {
                    guardCounter += 1
                    visited.insert(GridEdgeKey(previous, current))
                    loop.append(current)
                    if current == start { break }

                    let candidates = (adjacency[current] ?? []).filter {
                        !visited.contains(GridEdgeKey(current, $0))
                    }
                    guard !candidates.isEmpty else { break }
                    if let back = candidates.first(where: { $0 == start }),
                       loop.count >= 3 {
                        previous = current
                        current = back
                        continue
                    }
                    let step =
                        candidates.first(where: { $0 != previous }) ??
                        candidates[0]
                    previous = current
                    current = step
                }

                guard loop.count >= 4, loop.first == loop.last else {
                    continue
                }
                loop.removeLast()

                let polygon = canonicalOrder(loop.map {
                    .init(
                        x: minimumX + Float($0.x) * cell,
                        y: minimumZ + Float($0.y) * cell
                    )
                })
                let area = polygonArea(polygon)
                if area > bestArea ||
                    (area == bestArea && polygonLessThan(polygon, best)) {
                    bestArea = area
                    best = polygon
                }
            }
        }
        return best.count >= 3 ? best : []
    }

    private static func floorTriangleBoundary(
        edgeCounts: [EdgeKey: Int],
        pointLookup: [QuantizedPoint: RenderVec2]
    ) -> [RenderVec2] {
        var adjacency: [QuantizedPoint: [QuantizedPoint]] = [:]
        for (edge, _) in edgeCounts
            .filter({ $0.value == 1 })
            .sorted(by: { edgeKeyLessThan($0.key, $1.key) }) {
            adjacency[edge.a, default: []].append(edge.b)
            adjacency[edge.b, default: []].append(edge.a)
        }
        guard !adjacency.isEmpty else { return [] }
        for point in adjacency.keys {
            adjacency[point]!.sort(by: quantizedPointLessThan)
        }

        var visited = Set<EdgeKey>()
        var best: [RenderVec2] = []
        var bestArea: Float = 0

        let startEntries = adjacency.keys
            .sorted(by: quantizedPointLessThan)
            .compactMap { key in
                adjacency[key].map { (key: key, value: $0) }
            }
        for (start, neighbors) in startEntries {
            for next in neighbors {
                let seed = EdgeKey(start, next)
                guard !visited.contains(seed) else { continue }

                var loop = [start]
                var previous = start
                var current = next
                let guardLimit = max(32, adjacency.count * 4)
                var guardCounter = 0

                while guardCounter < guardLimit {
                    guardCounter += 1
                    visited.insert(EdgeKey(previous, current))
                    loop.append(current)
                    if current == start { break }

                    let candidates = (adjacency[current] ?? []).filter {
                        !visited.contains(EdgeKey(current, $0))
                    }
                    guard !candidates.isEmpty else { break }
                    if let close = candidates.first(where: { $0 == start }),
                       loop.count >= 3 {
                        previous = current
                        current = close
                        continue
                    }
                    let step =
                        candidates.first(where: { $0 != previous }) ??
                        candidates[0]
                    previous = current
                    current = step
                }

                guard loop.count >= 4, loop.last == loop.first else {
                    continue
                }
                loop.removeLast()
                let polygon = canonicalOrder(
                    loop.compactMap { pointLookup[$0] }
                )
                guard polygon.count >= 3 else { continue }
                let area = polygonArea(polygon)
                if area > bestArea ||
                    (area == bestArea && polygonLessThan(polygon, best)) {
                    bestArea = area
                    best = polygon
                }
            }
        }
        return best
    }

    private static func convexHull(
        _ points: [RenderVec2]
    ) -> [RenderVec2] {
        guard points.count > 2 else { return points }
        let sorted = points.sorted(by: pointLessThan)

        func cross(
            _ origin: RenderVec2,
            _ first: RenderVec2,
            _ second: RenderVec2
        ) -> Float {
            (first.x - origin.x) * (second.y - origin.y) -
                (first.y - origin.y) * (second.x - origin.x)
        }

        var lower: [RenderVec2] = []
        for point in sorted {
            while lower.count >= 2,
                  cross(
                    lower[lower.count - 2],
                    lower[lower.count - 1],
                    point
                  ) <= 0 {
                lower.removeLast()
            }
            lower.append(point)
        }

        var upper: [RenderVec2] = []
        for point in sorted.reversed() {
            while upper.count >= 2,
                  cross(
                    upper[upper.count - 2],
                    upper[upper.count - 1],
                    point
                  ) <= 0 {
                upper.removeLast()
            }
            upper.append(point)
        }

        lower.removeLast()
        upper.removeLast()
        return lower + upper
    }

    private static func canonicalOrder(
        _ points: [RenderVec2]
    ) -> [RenderVec2] {
        var open = points
        if open.count > 1, open.first == open.last {
            open.removeLast()
        }
        guard open.count > 1 else { return open }

        let minimum = open.min(by: pointLessThan)!
        var best: [RenderVec2]?
        for start in open.indices where open[start] == minimum {
            for direction in [1, -1] {
                let candidate = (0..<open.count).map { offset in
                    let index = (
                        start + direction * offset + open.count * offset
                    ) % open.count
                    return open[index]
                }
                if best == nil || polygonLessThan(candidate, best!) {
                    best = candidate
                }
            }
        }
        return best ?? open
    }

    private static func polygonArea(_ points: [RenderVec2]) -> Float {
        guard points.count >= 3 else { return 0 }
        var area: Float = 0
        for index in points.indices {
            let first = points[index]
            let second = points[(index + 1) % points.count]
            area += first.x * second.y - second.x * first.y
        }
        return abs(area) * 0.5
    }

    private static func pointLessThan(
        _ lhs: RenderVec2,
        _ rhs: RenderVec2
    ) -> Bool {
        lhs.x != rhs.x ? lhs.x < rhs.x : lhs.y < rhs.y
    }

    private static func polygonLessThan(
        _ lhs: [RenderVec2],
        _ rhs: [RenderVec2]
    ) -> Bool {
        for (left, right) in zip(lhs, rhs) {
            if left != right { return pointLessThan(left, right) }
        }
        return lhs.count < rhs.count
    }

    private static func quantizedPointLessThan(
        _ lhs: QuantizedPoint,
        _ rhs: QuantizedPoint
    ) -> Bool {
        lhs.x != rhs.x ? lhs.x < rhs.x : lhs.y < rhs.y
    }

    private static func edgeKeyLessThan(
        _ lhs: EdgeKey,
        _ rhs: EdgeKey
    ) -> Bool {
        if lhs.a != rhs.a {
            return quantizedPointLessThan(lhs.a, rhs.a)
        }
        return quantizedPointLessThan(lhs.b, rhs.b)
    }

    private static func gridPointLessThan(
        _ lhs: GridPoint,
        _ rhs: GridPoint
    ) -> Bool {
        lhs.x != rhs.x ? lhs.x < rhs.x : lhs.y < rhs.y
    }

    private static func gridEdgeKeyLessThan(
        _ lhs: GridEdgeKey,
        _ rhs: GridEdgeKey
    ) -> Bool {
        if lhs.a != rhs.a { return gridPointLessThan(lhs.a, rhs.a) }
        return gridPointLessThan(lhs.b, rhs.b)
    }

    private static func addGridEdge(
        _ edgeCounts: inout [GridEdgeKey: Int],
        _ first: GridPoint,
        _ second: GridPoint
    ) {
        edgeCounts[GridEdgeKey(first, second), default: 0] += 1
    }
}
