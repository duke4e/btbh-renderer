import BTBHRenderer
import BTBHRendererGPUCore
import Foundation
import XCTest
@testable import BTBHRendererGPU

@preconcurrency @MainActor
final class GPUPlatformTests: XCTestCase {
    func testFramePacingComputesResidualDelayAtHighRefreshRate() {
        let delay = GPUFramePacing.delayNanoseconds(
            targetFramesPerSecond: 240,
            previousFrameStart: 10,
            currentTime: 10.0036
        )

        XCTAssertEqual(delay, 566_666)
    }

    func testFramePacingDoesNotDelayFirstLateOrUncappedFrames() {
        XCTAssertNil(GPUFramePacing.delayNanoseconds(
            targetFramesPerSecond: 240,
            previousFrameStart: nil,
            currentTime: 10
        ))
        XCTAssertNil(GPUFramePacing.delayNanoseconds(
            targetFramesPerSecond: 240,
            previousFrameStart: 10,
            currentTime: 10.005
        ))
        XCTAssertNil(GPUFramePacing.delayNanoseconds(
            targetFramesPerSecond: 0,
            previousFrameStart: 10,
            currentTime: 10.001
        ))
    }

    func testPreciseDelayBridgeAcceptsZeroDuration() {
        btbh_gpu_delay_precise(0)
    }

    func testSceneShaderPathsResolveOnceFromOverride() {
        let paths = GPUSceneShaderPaths(
            request: .vulkan,
            environment: ["BTBH_GPU_SHADER_DIR": "/tmp/btbh-shaders"],
            executablePath: "/tmp/game/Main"
        )
        XCTAssertEqual(
            paths.vertexShaderPath,
            URL(fileURLWithPath: "/tmp/btbh-shaders")
                .appendingPathComponent("production_model.vert.spv")
                .path
        )
        XCTAssertEqual(
            paths.postFragmentShaderPath,
            URL(fileURLWithPath: "/tmp/btbh-shaders")
                .appendingPathComponent("production_post.frag.spv")
                .path
        )
        XCTAssertEqual(
            paths.canvasVertexShaderPath,
            URL(fileURLWithPath: "/tmp/btbh-shaders")
                .appendingPathComponent("production_canvas.vert.spv")
                .path
        )
        XCTAssertEqual(
            paths.canvasFragmentShaderPath,
            URL(fileURLWithPath: "/tmp/btbh-shaders")
                .appendingPathComponent("production_canvas.frag.spv")
                .path
        )
        XCTAssertEqual(
            paths.canvasTexturedFragmentShaderPath,
            URL(fileURLWithPath: "/tmp/btbh-shaders")
                .appendingPathComponent("production_canvas_textured.frag.spv")
                .path
        )
        XCTAssertEqual(paths.shaderFormat, BTBH_GPU_SHADER_FORMAT_SPIRV)
    }

    func testSceneShaderPathsFallBackBesideExecutable() {
        let executablePath = "/tmp/game/Main"
        let paths = GPUSceneShaderPaths(
            request: .metal,
            environment: [:],
            executablePath: executablePath
        )
        let expectedDirectory = URL(fileURLWithPath: executablePath)
            .standardizedFileURL
            .deletingLastPathComponent()
            .appendingPathComponent("GPUShaders")
        XCTAssertEqual(
            paths.animatedVertexShaderPath,
            expectedDirectory
                .appendingPathComponent(
                    "production_animated_model.vert.metallib"
                )
                .path
        )
        // The canvas used to resolve this directory on every present; it must match.
        XCTAssertEqual(
            paths.canvasTexturedFragmentShaderPath,
            URL(fileURLWithPath: executablePath)
                .standardizedFileURL
                .deletingLastPathComponent()
                .appendingPathComponent("GPUShaders")
                .appendingPathComponent(
                    "production_canvas_textured.frag.metallib"
                )
                .path
        )
        XCTAssertEqual(
            paths.shaderFormat,
            BTBH_GPU_SHADER_FORMAT_METALLIB
        )
    }

    func testFileDialogConfigurationMapsOpenFiltersAndLocationExactly() throws {
        let directory = URL(
            fileURLWithPath: "C:/tmp/particle-presets",
            isDirectory: true
        )

        let configuration = try GPUFileDialogConfiguration(
            request: .init(
                title: "Choose city data",
                allowedExtensions: ["json", "yaml"],
                allowsMultipleSelection: true,
                initialDirectory: directory
            )
        )

        XCTAssertEqual(configuration.kind, .open)
        XCTAssertEqual(configuration.title, "Choose city data")
        XCTAssertEqual(configuration.filterNames, ["JSON", "YAML"])
        XCTAssertEqual(configuration.filterPatterns, ["json", "yaml"])
        XCTAssertTrue(configuration.allowsMultipleSelection)
        XCTAssertEqual(configuration.location, directory.path)
    }

    func testFileDialogConfigurationMapsSaveSuggestedLocationAndWildcard() throws {
        let directory = URL(
            fileURLWithPath: "C:/tmp/particle-presets",
            isDirectory: true
        )

        let configuration = try GPUFileDialogConfiguration(
            request: .init(
                kind: .save,
                title: "Save particle",
                allowedExtensions: [],
                initialDirectory: directory,
                suggestedFileName: "effect.json"
            )
        )

        XCTAssertEqual(configuration.kind, .save)
        XCTAssertEqual(configuration.filterNames, ["*"])
        XCTAssertEqual(configuration.filterPatterns, ["*"])
        XCTAssertFalse(configuration.allowsMultipleSelection)
        XCTAssertEqual(
            configuration.location,
            directory.appendingPathComponent("effect.json").path
        )
    }

    func testFileDialogConfigurationRejectsSaveMultipleSelection() {
        XCTAssertThrowsError(try GPUFileDialogConfiguration(
            request: .init(
                kind: .save,
                title: "Invalid save",
                allowedExtensions: ["json"],
                allowsMultipleSelection: true
            )
        )) { error in
            XCTAssertEqual(
                error as? FileDialogError,
                .invalidRequest(
                    "Save dialogs cannot select multiple files"
                )
            )
        }
    }

    func testFileDialogCallbackMapsCancellationSelectionAndBackendFailure() {
        let cancellations = LockedDialogResults()
        GPUFileDialogCallbackBox { cancellations.append($0) }
            .complete(paths: [], errorMessage: nil)
        XCTAssertEqual(cancellations.values, [.success([])])

        let selections = LockedDialogResults()
        GPUFileDialogCallbackBox { selections.append($0) }
            .complete(
                paths: [
                    "C:/tmp/one.json",
                    "C:/tmp/two.json",
                ],
                errorMessage: nil
            )
        XCTAssertEqual(
            selections.values,
            [.success([
                URL(fileURLWithPath: "C:/tmp/one.json"),
                URL(fileURLWithPath: "C:/tmp/two.json"),
            ])]
        )

        let failures = LockedDialogResults()
        GPUFileDialogCallbackBox { failures.append($0) }
            .complete(
                paths: nil,
                errorMessage: "Injected SDL dialog failure"
            )
        XCTAssertEqual(
            failures.values,
            [.failure(.backendFailure(
                "Injected SDL dialog failure"
            ))]
        )
    }

    func testFileDialogWithoutOpenWindowFailsDeterministically() {
        let platform = GPUPlatform(
            request: .platformDefault,
            debugMode: true
        )
        let results = LockedDialogResults()

        platform.showOpenFileDialog(
            .init(title: "No window", allowedExtensions: ["json"])
        ) { results.append($0) }

        XCTAssertEqual(
            results.values,
            [.failure(.backendFailure(
                "GPU window is not open"
            ))]
        )
    }

    func testPlatformOwnsWindowTimingAndCleanShutdown() throws {
        let platform = GPUPlatform(
            request: .platformDefault,
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
    }

    func testProductionFileDialogCancellationSmoke() throws {
        guard ProcessInfo.processInfo.environment[
            "BTBH_RUN_GPU_FILE_DIALOG_SMOKE"
        ] == "1" else {
            throw XCTSkip(
                "Set BTBH_RUN_GPU_FILE_DIALOG_SMOKE=1 for the interactive smoke"
            )
        }
        let platform = GPUPlatform(
            request: .platformDefault,
            debugMode: true
        )
        try platform.openWindow(
            .init(
                title: "BTBH GPU file-dialog smoke",
                width: 640,
                height: 360,
                mode: .windowed,
                frameRatePolicy: .uncapped,
                drawableScalePolicy: .oneToOne
            )
        )
        defer { platform.closeWindow() }
        let completed = expectation(
            description: "production SDL file-dialog callback"
        )
        let results = LockedDialogResults()

        platform.showOpenFileDialog(
            .init(
                title: "BTBH GPU file-dialog smoke - press Cancel",
                allowedExtensions: ["json"]
            )
        ) {
            results.append($0)
            completed.fulfill()
        }

        wait(for: [completed], timeout: 120)
        XCTAssertEqual(results.values, [.success([])])
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

private final class LockedDialogResults: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [Result<[URL], FileDialogError>] = []

    var values: [Result<[URL], FileDialogError>] {
        lock.withLock { storage }
    }

    func append(_ value: Result<[URL], FileDialogError>) {
        lock.withLock {
            storage.append(value)
        }
    }
}
