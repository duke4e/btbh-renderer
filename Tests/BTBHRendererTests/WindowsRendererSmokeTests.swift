import Testing
@testable import BTBHRenderer

@Test("Windows renderer ABI and installed bundle lifecycle")
@MainActor
func windowsRendererABIAndInstalledBundleLifecycle() throws {
    RenderServices.uninstall()
    defer { RenderServices.uninstall() }

    #expect(RendererABI.version == 1)
    #expect(RendererABI.cVersion == RendererABI.version)

    let recording = RecordingRenderBundle()
    try RenderServices.install(recording)
    try RenderServices.platform.openWindow(.init(title: "smoke", width: 1280, height: 720, mode: .windowed))
    RenderServices.platform.pollEvents()
    RenderServices.uninstall()

    #expect(recording.events == [.openWindow, .pollEvents, .shutdown])
}
