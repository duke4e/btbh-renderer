import Foundation

/// Canonical camera projection math shared by every renderer backend.
///
/// Keeping projection and unprojection here prevents backend-specific floating-
/// point differences from changing simulation decisions that depend on camera
/// rays, such as spawn positions just outside the visible ground footprint.
public enum RenderCameraProjection {
    /// Where `world` shows on screen, after the camera's world curvature moved it (labels and rings stay on
    /// what they mark).
    public static func projectNormalized(
        _ flatWorld: RenderVec3,
        camera: RenderCamera,
        aspectRatio: Float
    ) -> NormalizedViewportPoint {
        let world = RenderWorldCurvature.bend(flatWorld, camera: camera)
        guard let geometry = geometry(
            camera: camera,
            aspectRatio: aspectRatio
        ) else {
            return .zero
        }

        let relative = world - camera.position
        let horizontal = relative.dot(geometry.right)
        let vertical = relative.dot(geometry.up)
        let depth = relative.dot(geometry.forward)
        let normalizedX: Float
        let normalizedY: Float

        switch camera.projection {
        case .perspective:
            guard depth != 0 else { return .zero }
            normalizedX = horizontal / (depth * geometry.halfWidth)
            normalizedY = vertical / (depth * geometry.halfHeight)
        case .orthographic:
            normalizedX = horizontal / geometry.halfWidth
            normalizedY = vertical / geometry.halfHeight
        }

        guard normalizedX.isFinite, normalizedY.isFinite else {
            return .zero
        }
        return .init(
            x: (normalizedX + 1) * 0.5,
            y: (1 - normalizedY) * 0.5
        )
    }

    public static func unprojectRay(
        _ point: NormalizedViewportPoint,
        camera: RenderCamera,
        aspectRatio: Float
    ) -> RenderRay {
        guard let geometry = geometry(
            camera: camera,
            aspectRatio: aspectRatio
        ),
              point.x.isFinite,
              point.y.isFinite else {
            return .init(origin: .zero, direction: .zero)
        }

        let normalizedX = 2 * point.x - 1
        let normalizedY = 1 - 2 * point.y
        switch camera.projection {
        case .perspective:
            let direction = (
                geometry.forward
                    + geometry.right * (normalizedX * geometry.halfWidth)
                    + geometry.up * (normalizedY * geometry.halfHeight)
            ).normalized()
            return .init(origin: camera.position, direction: direction)
        case .orthographic:
            let origin = camera.position
                + geometry.right * (normalizedX * geometry.halfWidth)
                + geometry.up * (normalizedY * geometry.halfHeight)
                + geometry.forward * 0.01
            return .init(origin: origin, direction: geometry.forward)
        }
    }

    private static func geometry(
        camera: RenderCamera,
        aspectRatio: Float
    ) -> (
        forward: RenderVec3,
        right: RenderVec3,
        up: RenderVec3,
        halfWidth: Float,
        halfHeight: Float
    )? {
        guard aspectRatio.isFinite,
              aspectRatio > 0,
              camera.verticalFOVDegrees.isFinite,
              camera.verticalFOVDegrees > 0 else {
            return nil
        }

        let forward = (camera.target - camera.position).normalized()
        let right = forward.cross(camera.up).normalized()
        let up = right.cross(forward).normalized()
        guard forward.lengthSquared.isFinite,
              right.lengthSquared.isFinite,
              up.lengthSquared.isFinite,
              forward.lengthSquared > 0,
              right.lengthSquared > 0,
              up.lengthSquared > 0 else {
            return nil
        }

        let halfHeight: Float
        switch camera.projection {
        case .perspective:
            guard camera.verticalFOVDegrees < 180 else { return nil }
            halfHeight = tan(camera.verticalFOVDegrees * .pi / 360)
        case .orthographic:
            halfHeight = camera.verticalFOVDegrees * 0.5
        }
        let halfWidth = halfHeight * aspectRatio
        guard halfWidth.isFinite,
              halfHeight.isFinite,
              halfWidth > 0,
              halfHeight > 0 else {
            return nil
        }
        return (forward, right, up, halfWidth, halfHeight)
    }
}

/// The render-only world curvature (`RenderCamera.worldCurvature`), the same formula as `btbh_world_bend` in
/// production_temporal.hlsli. Gameplay keeps the flat world: these map its points to where they are drawn, back
/// for the ground, and widen bounds so culling keeps everything a bend moves into view.
public enum RenderWorldCurvature {
    /// Curvatures smaller than this draw the world flat.
    public static let flatThreshold: Float = 1e-5
    /// How high the hyperbolic ground climbs at its ring, as a share of the ring's radius (BTBH_WORLD_BOWL):
    /// the far city rises around the kaiju, where the planet's falls away.
    public static let hyperbolicBowl: Float = 0.75

    public static func center(of camera: RenderCamera) -> RenderVec3 {
        camera.worldCurvatureCenter ?? camera.target
    }

    public static func isFlat(_ curvature: Float) -> Bool {
        !(abs(curvature) >= flatThreshold) || !curvature.isFinite
    }

    public static func bend(_ point: RenderVec3, camera: RenderCamera) -> RenderVec3 {
        bend(point, curvature: camera.worldCurvature, center: center(of: camera))
    }

    /// Where a flat-world point is drawn.
    public static func bend(_ point: RenderVec3, curvature k: Float, center: RenderVec3) -> RenderVec3 {
        guard !isFlat(k) else { return point }
        let dx = point.x - center.x
        let dz = point.z - center.z
        let r = (dx * dx + dz * dz).squareRoot()
        guard r > 1e-6 else { return point }
        let directionX = dx / r
        let directionZ = dz / r
        if k > 0 {
            let angle = min(r * k, Float.pi)
            let sag = (1 - cos(angle)) / k
            let lean = point.y * sin(angle)
            return RenderVec3(
                x: center.x + directionX * (r + lean),
                y: point.y * cos(angle) - sag,
                z: center.z + directionZ * (r + lean)
            )
        }
        let span = 2 / -k
        let u = r / span
        let compressed = span * tanh(u)
        let shrink = 1 / (cosh(u) * cosh(u))
        let rim = compressed / span
        return RenderVec3(
            x: center.x + directionX * compressed,
            y: point.y * shrink + hyperbolicBowl * span * rim * rim,
            z: center.z + directionZ * compressed
        )
    }

    /// The flat-world ground point (y = 0) drawn where `ray` meets the drawn ground; nil when it misses.
    public static func flatGroundHit(_ ray: RenderRay, camera: RenderCamera) -> RenderVec3? {
        flatGroundHit(ray, curvature: camera.worldCurvature, center: center(of: camera))
    }

    public static func flatGroundHit(_ ray: RenderRay, curvature k: Float, center: RenderVec3) -> RenderVec3? {
        let origin = ray.origin
        let direction = ray.direction
        if isFlat(k) {
            guard abs(direction.y) > 1e-6 else { return nil }
            let t = -origin.y / direction.y
            guard t > 0 else { return nil }
            return RenderVec3(x: origin.x + direction.x * t, y: 0, z: origin.z + direction.z * t)
        }
        if k < 0 {
            // The hyperbolic bowl: y = bowl * s^2 / span over the drawn distance s, a quadratic in t.
            let span = 2 / -k
            let a = hyperbolicBowl / span
            let ox = origin.x - center.x
            let oz = origin.z - center.z
            let qa = a * (direction.x * direction.x + direction.z * direction.z)
            let qb = 2 * a * (ox * direction.x + oz * direction.z) - direction.y
            let qc = a * (ox * ox + oz * oz) - origin.y
            var t: Float
            if abs(qa) < 1e-9 {
                guard abs(qb) > 1e-9 else { return nil }
                t = -qc / qb
            } else {
                let discriminant = qb * qb - 4 * qa * qc
                guard discriminant >= 0 else { return nil }
                let root = discriminant.squareRoot()
                let near = (-qb - root) / (2 * qa)
                let far = (-qb + root) / (2 * qa)
                t = near > 0 ? near : far
            }
            guard t > 0 else { return nil }
            let drawn = RenderVec3(x: origin.x + direction.x * t, y: 0, z: origin.z + direction.z * t)
            let dx = drawn.x - center.x
            let dz = drawn.z - center.z
            let compressed = (dx * dx + dz * dz).squareRoot()
            guard compressed > 1e-6 else { return drawn }
            // Points at the ring's edge are infinitely far in the flat world.
            guard compressed < span * 0.9999 else { return nil }
            let r = span * atanh(compressed / span)
            return RenderVec3(x: center.x + dx / compressed * r, y: 0, z: center.z + dz / compressed * r)
        }
        // The planet's ground keeps its x and z and sinks by the sag: march to the first crossing, then bisect.
        guard direction.y < 0 else { return nil }
        func height(_ t: Float) -> Float {
            let x = origin.x + direction.x * t - center.x
            let z = origin.z + direction.z * t - center.z
            let angle = min((x * x + z * z).squareRoot() * k, Float.pi)
            return origin.y + direction.y * t + (1 - cos(angle)) / k
        }
        let lowest = 2 / k
        let farthest = (origin.y + lowest) / -direction.y
        guard farthest.isFinite, farthest > 0, height(0) > 0 else { return nil }
        let steps = 96
        var previous: Float = 0
        for step in 1...steps {
            let t = farthest * Float(step) / Float(steps)
            if height(t) <= 0 {
                var low = previous
                var high = t
                for _ in 0..<24 {
                    let middle = (low + high) * 0.5
                    if height(middle) > 0 { low = middle } else { high = middle }
                }
                let hit = (low + high) * 0.5
                return RenderVec3(x: origin.x + direction.x * hit, y: 0, z: origin.z + direction.z * hit)
            }
            previous = t
        }
        return nil
    }

    /// Bounds that hold everything a bend can draw from `bounds` (culling must not drop what it moves in).
    public static func drawnBounds(_ bounds: RenderBounds3, curvature k: Float, center: RenderVec3) -> RenderBounds3 {
        guard !isFlat(k) else { return bounds }
        let minimum = bounds.minimum
        let maximum = bounds.maximum
        if k > 0 {
            let nearX = max(0, max(minimum.x - center.x, center.x - maximum.x))
            let nearZ = max(0, max(minimum.z - center.z, center.z - maximum.z))
            let farX = max(abs(minimum.x - center.x), abs(maximum.x - center.x))
            let farZ = max(abs(minimum.z - center.z), abs(maximum.z - center.z))
            let nearAngle = min((nearX * nearX + nearZ * nearZ).squareRoot() * k, Float.pi)
            let farAngle = min((farX * farX + farZ * farZ).squareRoot() * k, Float.pi)
            let tallest = max(abs(minimum.y), abs(maximum.y))
            let lean = tallest * sin(min(farAngle, Float.pi / 2))
            return RenderBounds3(
                minimum: RenderVec3(
                    x: minimum.x - lean,
                    y: -(1 - cos(farAngle)) / k - tallest,
                    z: minimum.z - lean
                ),
                maximum: RenderVec3(
                    x: maximum.x + lean,
                    y: -(1 - cos(nearAngle)) / k + max(0, maximum.y),
                    z: maximum.z + lean
                )
            )
        }
        // The hyperbolic map never stretches a distance: the box lands within its half-diagonal of where its
        // centre goes, and heights only shrink.
        let middle = bend(
            RenderVec3(x: (minimum.x + maximum.x) * 0.5, y: 0, z: (minimum.z + maximum.z) * 0.5),
            curvature: k,
            center: center
        )
        let halfX = (maximum.x - minimum.x) * 0.5
        let halfZ = (maximum.z - minimum.z) * 0.5
        let reach = (halfX * halfX + halfZ * halfZ).squareRoot()
        let span = 2 / -k
        return RenderBounds3(
            minimum: RenderVec3(x: middle.x - reach, y: min(0, minimum.y), z: middle.z - reach),
            maximum: RenderVec3(x: middle.x + reach, y: max(0, maximum.y) + hyperbolicBowl * span, z: middle.z + reach)
        )
    }
}
