public struct NormalizedViewport: Hashable, Sendable {
    public let x: Float
    public let y: Float
    public let width: Float
    public let height: Float

    public init(x: Float, y: Float, width: Float, height: Float) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
    }

    public static let full = Self(x: 0, y: 0, width: 1, height: 1)
}
