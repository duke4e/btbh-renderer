import BTBHRenderer
import XCTest

@preconcurrency @MainActor
final class PlatformContractTests: XCTestCase {
    func testWindowDescriptorCarriesEveryFrameRatePolicy() {
        let policies: [FrameRatePolicy] = [
            .displayRefresh(minimum: 0),
            .displayRefresh(minimum: Int(Int32.max)),
            .fixed(1),
            .fixed(Int(Int32.max)),
            .uncapped,
        ]

        for policy in policies {
            let descriptor = WindowDescriptor(
                title: "policy",
                width: 1280,
                height: 720,
                mode: .windowed,
                frameRatePolicy: policy,
                drawableScalePolicy: .system
            )

            XCTAssertEqual(descriptor.frameRatePolicy, policy)
        }
    }

    func testWindowDescriptorCarriesEveryDrawableScalePolicy() {
        let policies: [DrawableScalePolicy] = [.system, .oneToOne]

        for policy in policies {
            let descriptor = WindowDescriptor(
                title: "scale",
                width: 640,
                height: 480,
                mode: .windowed,
                frameRatePolicy: .uncapped,
                drawableScalePolicy: policy
            )

            XCTAssertEqual(descriptor.drawableScalePolicy, policy)
        }
    }

    func testSourceCompatibleWindowDescriptorDefaultsPreserveLegacyPlatformBehavior() {
        let descriptor = WindowDescriptor(
            title: "legacy-default",
            width: 800,
            height: 600,
            mode: .borderless
        )

        XCTAssertEqual(descriptor.frameRatePolicy, .displayRefresh(minimum: 30))
        XCTAssertEqual(descriptor.drawableScalePolicy, .system)
    }

    func testGamePlatformExposesCurrentSizesMonotonicTimeAndMouseMutation() throws {
        let recording = RecordingRenderBundle()
        let platform: any GamePlatform = recording

        XCTAssertEqual(platform.logicalSize, PixelSize2D(width: 640, height: 360))
        XCTAssertEqual(platform.drawableSize, PixelSize2D(width: 1280, height: 720))
        XCTAssertEqual(platform.monotonicElapsedSeconds, 42.5)

        try platform.setMousePosition(RenderVec2(x: 320, y: 180))
        XCTAssertEqual(recording.mousePositions, [RenderVec2(x: 320, y: 180)])
    }

    func testFileDialogRequestCarriesOpenAndSaveOwnershipWithoutBackendTypes() {
        let directory = URL(fileURLWithPath: "/tmp/particle-presets", isDirectory: true)
        let open = FileDialogRequest(
            kind: .open,
            title: "Load Particle JSON",
            allowedExtensions: ["json"],
            allowsMultipleSelection: true,
            initialDirectory: directory
        )
        XCTAssertEqual(open.kind, .open)
        XCTAssertEqual(open.initialDirectory, directory)
        XCTAssertNil(open.suggestedFileName)

        let save = FileDialogRequest(
            kind: .save,
            title: "Save Particle JSON",
            allowedExtensions: ["json"],
            initialDirectory: directory,
            suggestedFileName: "effect.json"
        )
        XCTAssertEqual(save.kind, .save)
        XCTAssertEqual(save.initialDirectory, directory)
        XCTAssertEqual(save.suggestedFileName, "effect.json")
        XCTAssertFalse(save.allowsMultipleSelection)
    }

    func testSourceCompatibleFileDialogRequestDefaultsToOpenWithoutInitialLocation() {
        let request = FileDialogRequest(
            title: "Choose data",
            allowedExtensions: ["json"]
        )

        XCTAssertEqual(request.kind, .open)
        XCTAssertNil(request.initialDirectory)
        XCTAssertNil(request.suggestedFileName)
    }

    func testFileDialogErrorsPreserveExactProjectFacingMessages() {
        XCTAssertEqual(FileDialogError.invalidRequest("invalid").message, "invalid")
        XCTAssertEqual(FileDialogError.backendFailure("backend").message, "backend")
    }
}
