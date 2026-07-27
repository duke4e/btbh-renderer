import BTBHRenderer
import Foundation
import XCTest
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUPlatformTests: XCTestCase {
    func testWindowsPlatformOwnsWindowTimingAndCleanShutdown() throws {
        #if os(Windows)
        let platform = GPUPlatform(
            request: .vulkan,
            debugMode: true
        )
        try platform.openWindow(
            .init(
                title: "BTBH GPU platform test",
                width: 640,
                height: 360,
                mode: .windowed,
                frameRatePolicy: .uncapped,
                drawableScalePolicy: .oneToOne
            )
        )

        platform.pollEvents()
        let firstFrame = platform.beginFrame()
        XCTAssertEqual(firstFrame.logicalSize, .init(width: 640, height: 360))
        XCTAssertEqual(firstFrame.drawableSize, .init(width: 640, height: 360))
        XCTAssertGreaterThanOrEqual(firstFrame.elapsedSeconds, 0)
        XCTAssertGreaterThanOrEqual(firstFrame.deltaSeconds, 0)
        XCTAssertFalse(platform.shouldClose)
        let timingStarted = platform.monotonicElapsedSeconds
        Thread.sleep(forTimeInterval: 0.01)
        XCTAssertGreaterThan(platform.monotonicElapsedSeconds, timingStarted)
        let secondFrame = platform.beginFrame()
        XCTAssertGreaterThanOrEqual(secondFrame.deltaSeconds, 0)

        try platform.setClipboardText("BTBH GPU platform")
        platform.setCursorVisible(false)
        XCTAssertFalse(platform.isCursorVisible)
        platform.setCursorVisible(true)
        XCTAssertTrue(platform.isCursorVisible)

        platform.closeWindow()
        platform.closeWindow()
        #else
        throw XCTSkip("The Windows Vulkan platform test runs only on Windows")
        #endif
    }

    func testCursorVisibilityMutationPreservesTransientFrameState() {
        let original = GPUPlatformState(
            elapsedSeconds: 42,
            deltaSeconds: 0.016,
            mousePosition: .init(x: 10, y: 20),
            mouseDelta: .init(x: 3, y: -2),
            mouseWheelDelta: 1,
            refreshRate: 144,
            framesPerSecond: 120,
            shouldClose: false,
            isWindowFocused: true,
            isCursorVisible: true
        )

        let hidden = original.settingCursorVisible(false)

        XCTAssertEqual(hidden.elapsedSeconds, original.elapsedSeconds)
        XCTAssertEqual(hidden.deltaSeconds, original.deltaSeconds)
        XCTAssertEqual(hidden.mousePosition, original.mousePosition)
        XCTAssertEqual(hidden.mouseDelta, original.mouseDelta)
        XCTAssertEqual(hidden.mouseWheelDelta, original.mouseWheelDelta)
        XCTAssertEqual(hidden.refreshRate, original.refreshRate)
        XCTAssertEqual(hidden.framesPerSecond, original.framesPerSecond)
        XCTAssertEqual(hidden.shouldClose, original.shouldClose)
        XCTAssertEqual(hidden.isWindowFocused, original.isWindowFocused)
        XCTAssertFalse(hidden.isCursorVisible)
    }

    func testInvalidDimensionsAndFrameRatesFailBeforeWindowCreation() {
        let platform = GPUPlatform(
            request: .platformDefault,
            debugMode: true
        )

        XCTAssertThrowsError(try platform.openWindow(.init(
            title: "Invalid",
            width: Int.max,
            height: 360,
            mode: .windowed
        )))
        XCTAssertThrowsError(try platform.openWindow(.init(
            title: "Invalid",
            width: 640,
            height: 360,
            mode: .windowed,
            frameRatePolicy: .fixed(0)
        )))
    }
}
