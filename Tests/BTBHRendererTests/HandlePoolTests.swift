import XCTest
@testable import BTBHRenderer

final class HandlePoolTests: XCTestCase {
    func testReleasedSlotChangesGenerationBeforeReuse() throws {
        let pool = try HandlePool(initialCapacity: 2)
        let first: ModelHandle = try pool.acquire(ModelHandle.self)
        XCTAssertTrue(pool.contains(first))
        XCTAssertTrue(pool.release(first))
        XCTAssertFalse(pool.contains(first))

        let replacement: ModelHandle = try pool.acquire(ModelHandle.self)
        XCTAssertNotEqual(first.rawValue, replacement.rawValue)
        XCTAssertFalse(pool.contains(first))
        XCTAssertTrue(pool.contains(replacement))
    }

    func testHandleKindsCannotBeMixedAtCompileTime() throws {
        let pool = try HandlePool(initialCapacity: 1)
        let texture: TextureHandle = try pool.acquire(TextureHandle.self)
        XCTAssertTrue(pool.contains(texture))
        XCTAssertNotEqual(texture, .invalid)
    }
}
