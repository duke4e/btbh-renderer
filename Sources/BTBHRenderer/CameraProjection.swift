import Foundation

/// Canonical camera projection math shared by every renderer backend.
///
/// Keeping projection and unprojection here prevents backend-specific floating-
/// point differences from changing simulation decisions that depend on camera
/// rays, such as spawn positions just outside the visible ground footprint.
public enum RenderCameraProjection {
    public static func projectNormalized(
        _ world: RenderVec3,
        camera: RenderCamera,
        aspectRatio: Float
    ) -> NormalizedViewportPoint {
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
