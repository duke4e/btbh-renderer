import XCTest
@testable import BTBHRenderer
import BTBHRendererCore

@_silgen_name("btbh_handle_generation_advance")
private func btbh_handle_generation_advance(
    _ generation: UInt32,
    _ outGeneration: UnsafeMutablePointer<UInt32>
) -> BTBHHandleResult

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

    func testAcquiredTypedHandleIsValid() throws {
        let pool = try HandlePool(initialCapacity: 1)
        let texture: TextureHandle = try pool.acquire(TextureHandle.self)
        XCTAssertTrue(pool.contains(texture))
        XCTAssertNotEqual(texture, .invalid)
    }

    func testPoolGrowsAndPacksGenerationAndIndexBits() throws {
        let pool = try HandlePool(initialCapacity: 1)
        let handles: [TextureHandle] = try (0..<9).map { _ in
            try pool.acquire(TextureHandle.self)
        }

        XCTAssertEqual(handles.map { UInt32($0.rawValue >> 32) }, Array(repeating: 1, count: 9))
        XCTAssertEqual(handles.map { UInt32(truncatingIfNeeded: $0.rawValue) }, Array(1...9))
        XCTAssertTrue(handles.allSatisfy(pool.contains))
    }

    func testInvalidHandleAndDoubleReleaseAreRejected() throws {
        let pool = try HandlePool(initialCapacity: 1)
        XCTAssertFalse(pool.release(TextureHandle.invalid))

        let handle: TextureHandle = try pool.acquire(TextureHandle.self)
        XCTAssertTrue(pool.release(handle))
        XCTAssertFalse(pool.release(handle))
    }

    func testCoreReportsInvalidAndStaleReleases() throws {
        let pool = try XCTUnwrap(btbh_handle_pool_create(1))
        defer { btbh_handle_pool_destroy(pool) }

        XCTAssertEqual(btbh_handle_pool_release(pool, 0), BTBH_HANDLE_INVALID_ARGUMENT)
        var handle: UInt64 = 0
        XCTAssertEqual(btbh_handle_pool_acquire(pool, &handle), BTBH_HANDLE_OK)
        XCTAssertEqual(btbh_handle_pool_release(pool, handle), BTBH_HANDLE_OK)
        XCTAssertEqual(btbh_handle_pool_release(pool, handle), BTBH_HANDLE_STALE)
    }

    func testAcquireResultErrorsMapDistinctly() {
        XCTAssertEqual(HandlePoolError(coreResult: BTBH_HANDLE_OUT_OF_MEMORY), .allocationFailed)
        XCTAssertEqual(HandlePoolError(coreResult: BTBH_HANDLE_EXHAUSTED), .exhausted)
    }

    func testGenerationAdvanceIncrementsOrdinaryValue() {
        var nextGeneration: UInt32 = 0
        XCTAssertEqual(btbh_handle_generation_advance(41, &nextGeneration), BTBH_HANDLE_OK)
        XCTAssertEqual(nextGeneration, 42)
    }

    func testGenerationAdvanceExhaustsInsteadOfWrapping() {
        var nextGeneration: UInt32 = 0
        XCTAssertEqual(btbh_handle_generation_advance(UInt32.max, &nextGeneration), BTBH_HANDLE_EXHAUSTED)
        XCTAssertEqual(nextGeneration, 0)
    }
}
