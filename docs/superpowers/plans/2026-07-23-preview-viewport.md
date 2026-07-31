# Direct Preview Viewport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the level-up/player-type 3D preview directly into its card viewport so no Canvas scissor or high-DPI offset can expose a white edge.

**Architecture:** `FrameDescriptor` carries a backend-neutral normalized top-left viewport. The legacy backend converts its four edges to drawable pixels, sizes R3D's internal targets from that viewport, and passes the absolute destination to `R3D_View`; the preview then uses its normal follow camera and no longer mutates global scissor state.

**Tech Stack:** Swift 5.10, SwiftPM/XCTest, project-owned BTBHRenderer seam, legacy R3D/OpenGL adapter, raylib drawable metrics, macOS high-DPI window verification.

## Global Constraints

- Public viewport coordinates are normalized, top-left, and backend-neutral.
- `.full` remains the default for every existing gameplay/editor `FrameDescriptor`.
- Invalid, nonfinite, empty, or out-of-bounds viewports fail before the raw driver begins.
- Pixel conversion rounds the four edges independently and derives width/height from the rounded edges.
- R3D internal resolution is based on the resolved viewport size times `renderScale`, not the whole drawable.
- The preview path must not submit Canvas `pushScissor` or `popScissor`.
- Canvas2D scissor remains available for unrelated 2D call sites.
- The preview camera controller still owns camera angle/mutator behavior; presentation viewport does not encode camera movement.
- No offscreen render target is introduced unless the direct viewport fails the approved visual acceptance test.

---

## File Map

### Renderer repository

- `Sources/BTBHRenderer/RenderViewport.swift`: normalized viewport value type and `.full`.
- `Sources/BTBHRenderer/SceneSubmission.swift`: adds the viewport to `FrameDescriptor`.
- `Tests/BTBHRendererTests/RenderViewportTests.swift`: locks default and explicit descriptor behavior.

### Game repository

- `City Generator (r3d)/Vendor/BTBHRenderer`: advances to the renderer commit containing the seam.
- `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyConversions.swift`: validates and converts normalized edges to drawable pixels.
- `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyRawSceneDriver.swift`: uses the resolved viewport for internal resolution and `R3D_View`.
- `City Generator (r3d)/Sources/Main/Sources/Classes/Game/LevelUpPreview3D.swift`: supplies the normalized preview viewport and removes clipping/camera compensation.
- `City Generator (r3d)/Sources/Main/Sources/Classes/ParticleSystem/ParticleClassicPreviewHost.swift`: follows the simplified queued-frame lifecycle signature.
- `City Generator (r3d)/Tests/MainTests/LegacyRenderAdapterTests.swift`: tests high-DPI edge conversion and validation.
- `City Generator (r3d)/Tests/MainTests/FusionPreviewPolicyTests.swift`: tests preview descriptor propagation and absence of scissor.
- `City Generator (r3d)/Tests/MainTests/SceneRendererRuntimeSafetyTests.swift`: locks the simplified scene lifecycle.

---

### Task 1: Add the Backend-Neutral Viewport Seam

**Files:**
- Create: `Sources/BTBHRenderer/RenderViewport.swift`
- Modify: `Sources/BTBHRenderer/SceneSubmission.swift`
- Create: `Tests/BTBHRendererTests/RenderViewportTests.swift`

**Interfaces:**
- Consumes: `FrameDescriptor`.
- Produces: `NormalizedViewport` and `FrameDescriptor.viewport`.

- [ ] **Step 1: Write the failing public-seam tests**

Create `Tests/BTBHRendererTests/RenderViewportTests.swift`:

```swift
import XCTest
@testable import BTBHRenderer

final class RenderViewportTests: XCTestCase {
    func testFullViewportUsesTopLeftNormalizedBounds() {
        XCTAssertEqual(
            NormalizedViewport.full,
            .init(x: 0, y: 0, width: 1, height: 1)
        )
    }

    func testFrameDescriptorDefaultsToFullViewport() {
        let descriptor = FrameDescriptor(
            camera: .init(
                position: .init(x: 0, y: 10, z: -10),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 45,
                projection: .perspective
            ),
            environment: .default
        )
        XCTAssertEqual(descriptor.viewport, .full)
    }

    func testFrameDescriptorPreservesExplicitViewport() {
        let viewport = NormalizedViewport(
            x: 0.125,
            y: 0.25,
            width: 0.5,
            height: 0.625
        )
        let descriptor = FrameDescriptor(
            camera: .init(
                position: .init(x: 0, y: 10, z: -10),
                target: .zero,
                up: .up,
                verticalFOVDegrees: 45,
                projection: .perspective
            ),
            environment: .default,
            viewport: viewport
        )
        XCTAssertEqual(descriptor.viewport, viewport)
    }
}
```

- [ ] **Step 2: Prove the renderer test is red**

Run:

```bash
swift test --filter RenderViewportTests
```

Expected: compilation fails because `NormalizedViewport` and `FrameDescriptor.viewport` do not exist.

- [ ] **Step 3: Add the value type**

Create `Sources/BTBHRenderer/RenderViewport.swift`:

```swift
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
```

- [ ] **Step 4: Extend `FrameDescriptor` without changing defaults**

In `Sources/BTBHRenderer/SceneSubmission.swift`, add:

```swift
public let viewport: NormalizedViewport
```

Add the final initializer parameter:

```swift
viewport: NormalizedViewport = .full
```

Assign it:

```swift
self.viewport = viewport
```

- [ ] **Step 5: Run renderer tests**

Run:

```bash
swift test --filter RenderViewportTests
swift test
```

Expected: focused tests and the complete renderer Swift suite pass.

- [ ] **Step 6: Commit the seam**

```bash
git add \
  Sources/BTBHRenderer/RenderViewport.swift \
  Sources/BTBHRenderer/SceneSubmission.swift \
  Tests/BTBHRendererTests/RenderViewportTests.swift
git commit -m "feat: add normalized scene presentation viewport"
```

---

### Task 2: Convert Viewport Edges in the Legacy Backend

**Files:**
- Modify: `City Generator (r3d)/Vendor/BTBHRenderer`
- Modify: `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyConversions.swift`
- Modify: `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyRawSceneDriver.swift`
- Test: `City Generator (r3d)/Tests/MainTests/LegacyRenderAdapterTests.swift`

**Interfaces:**
- Consumes: `FrameDescriptor.viewport` from Task 1.
- Produces: `LegacyDrawableViewport` and `LegacyRenderValidation.drawableViewport(_:drawableSize:)`.

- [ ] **Step 1: Advance the local renderer submodule**

Run from `City Generator (r3d)`:

```bash
git -C "Vendor/BTBHRenderer" fetch \
  /Users/duke4e/Desktop/r3d/r3d \
  refs/heads/codex/renderer-phase-0-1
git -C "Vendor/BTBHRenderer" checkout --detach FETCH_HEAD
```

- [ ] **Step 2: Add failing conversion and validation tests**

In `LegacyRenderAdapterTests.swift`, add:

```swift
func testNormalizedViewportRoundsEdgesInDrawableSpace() throws {
    let viewport = NormalizedViewport(
        x: 0.125,
        y: 0.25,
        width: 0.5,
        height: 0.5
    )

    XCTAssertEqual(
        try LegacyRenderValidation.drawableViewport(
            viewport,
            drawableSize: .init(width: 1_280, height: 720)
        ),
        .init(x: 160, y: 180, width: 640, height: 360)
    )
    XCTAssertEqual(
        try LegacyRenderValidation.drawableViewport(
            viewport,
            drawableSize: .init(width: 2_048, height: 1_152)
        ),
        .init(x: 256, y: 288, width: 1_024, height: 576)
    )

    let thirds = try LegacyRenderValidation.drawableViewport(
        .init(x: 1.0 / 3.0, y: 0, width: 1.0 / 3.0, height: 1),
        drawableSize: .init(width: 100, height: 100)
    )
    XCTAssertEqual(thirds.x, 33)
    XCTAssertEqual(thirds.width, 34)
}

func testInvalidNormalizedViewportsFailBeforeRawSceneBegin() throws {
    let drawable = PixelSize2D(width: 1_280, height: 720)
    for viewport in [
        NormalizedViewport(x: .nan, y: 0, width: 1, height: 1),
        NormalizedViewport(x: 0, y: 0, width: 0, height: 1),
        NormalizedViewport(x: -0.01, y: 0, width: 1, height: 1),
        NormalizedViewport(x: 0.75, y: 0, width: 0.5, height: 1),
    ] {
        XCTAssertThrowsError(
            try LegacyRenderValidation.drawableViewport(
                viewport,
                drawableSize: drawable
            )
        )
    }
}
```

Update the existing default framebuffer view test to resolve `.full` first:

```swift
let drawableViewport = try! LegacyRenderValidation.drawableViewport(
    .full,
    drawableSize: .init(width: 1_920, height: 1_080)
)
let view = legacyDefaultFramebufferView(
    camera: camera,
    viewport: drawableViewport
)
```

- [ ] **Step 3: Prove the adapter tests are red**

Run:

```bash
swift test --filter LegacyRenderAdapterTests
```

Expected: compilation fails because the drawable viewport type and conversion do not exist.

- [ ] **Step 4: Add one focused drawable viewport type**

In `LegacyConversions.swift`, near `LegacyRenderValidation`, add:

```swift
struct LegacyDrawableViewport: Equatable {
    let x: Int32
    let y: Int32
    let width: Int32
    let height: Int32

    var size: PixelSize2D {
        .init(width: Int(width), height: Int(height))
    }
}
```

Inside `LegacyRenderValidation`, add:

```swift
static func drawableViewport(
    _ viewport: NormalizedViewport,
    drawableSize: PixelSize2D
) throws -> LegacyDrawableViewport {
    try validatePixelSize(drawableSize, name: "Drawable")

    let x = Double(viewport.x)
    let y = Double(viewport.y)
    let width = Double(viewport.width)
    let height = Double(viewport.height)
    let right = x + width
    let bottom = y + height

    guard x.isFinite,
          y.isFinite,
          width.isFinite,
          height.isFinite,
          x >= 0,
          y >= 0,
          width > 0,
          height > 0,
          right <= 1,
          bottom <= 1 else {
        throw LegacyAdapterError.rawOperationFailed(
            "Frame viewport must be finite, nonempty, and inside normalized bounds"
        )
    }

    let drawableWidth = Double(drawableSize.width)
    let drawableHeight = Double(drawableSize.height)
    let leftPixel = try checkedInt32(
        (x * drawableWidth).rounded(),
        name: "Viewport left edge"
    )
    let topPixel = try checkedInt32(
        (y * drawableHeight).rounded(),
        name: "Viewport top edge"
    )
    let rightPixel = try checkedInt32(
        (right * drawableWidth).rounded(),
        name: "Viewport right edge"
    )
    let bottomPixel = try checkedInt32(
        (bottom * drawableHeight).rounded(),
        name: "Viewport bottom edge"
    )
    let resolved = LegacyDrawableViewport(
        x: leftPixel,
        y: topPixel,
        width: rightPixel - leftPixel,
        height: bottomPixel - topPixel
    )
    guard resolved.width > 0, resolved.height > 0 else {
        throw LegacyAdapterError.rawOperationFailed(
            "Frame viewport resolves to an empty drawable region"
        )
    }
    return resolved
}
```

In `validateFrameDescriptor`, resolve the viewport before validating scaled resolution:

```swift
if let drawableSize {
    let viewport = try drawableViewport(
        descriptor.viewport,
        drawableSize: drawableSize
    )
    _ = try scaledResolution(
        drawableSize: viewport.size,
        renderScale: descriptor.renderScale
    )
}
```

- [ ] **Step 5: Make the raw driver use the resolved viewport**

Change `legacyDefaultFramebufferView` to:

```swift
func legacyDefaultFramebufferView(
    camera: RenderCamera,
    viewport: LegacyDrawableViewport
) -> R3D_View {
    var view = R3D_View()
    view.camera = R3D_CameraFromRL(camera.legacy)
    view.viewport = Rectangle(
        x: Float(viewport.x),
        y: Float(viewport.y),
        width: Float(viewport.width),
        height: Float(viewport.height)
    )
    return view
}
```

At the start of `R3DLegacyRawSceneDriver.begin`, after descriptor validation, resolve once:

```swift
let viewport = try LegacyRenderValidation.drawableViewport(
    frame.descriptor.viewport,
    drawableSize: drawableSize
)
let resolution = try LegacyRenderValidation.scaledResolution(
    drawableSize: viewport.size,
    renderScale: frame.descriptor.renderScale
)
```

Pass that same value to R3D:

```swift
R3D_BeginPro(legacyDefaultFramebufferView(
    camera: frame.descriptor.camera,
    viewport: viewport
))
```

- [ ] **Step 6: Run focused and renderer-adapter suites**

Run:

```bash
swift test --filter LegacyRenderAdapterTests
swift test --filter FusionPreviewPolicyTests
```

Expected: adapter tests pass; preview policy tests still pass because the preview has not yet opted into a subviewport.

- [ ] **Step 7: Commit the backend conversion**

From the game repository root:

```bash
git add \
  "City Generator (r3d)/Vendor/BTBHRenderer" \
  "City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyConversions.swift" \
  "City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyRawSceneDriver.swift" \
  "City Generator (r3d)/Tests/MainTests/LegacyRenderAdapterTests.swift"
git commit -m "feat: resolve scene viewports in drawable space"
```

---

### Task 3: Move the Preview from Scissor to Scene Viewport

**Files:**
- Modify: `City Generator (r3d)/Sources/Main/Sources/Classes/Game/LevelUpPreview3D.swift`
- Modify: `City Generator (r3d)/Sources/Main/Sources/Classes/ParticleSystem/ParticleClassicPreviewHost.swift`
- Modify: `City Generator (r3d)/Tests/MainTests/FusionPreviewPolicyTests.swift`
- Modify: `City Generator (r3d)/Tests/MainTests/SceneRendererRuntimeSafetyTests.swift`

**Interfaces:**
- Consumes: `NormalizedViewport` and `FrameDescriptor.viewport`.
- Produces: `LevelUpPreview3D.normalizedViewport(_:logicalSize:)` and an unclipped preview scene frame.

- [ ] **Step 1: Replace scissor expectations with viewport expectations**

In `FusionPreviewPolicyTests.swift`, replace `testPreviewKeepsScissorEnabledUntilQueuedFrameFlushCompletes` with:

```swift
func testPreviewUsesSceneViewportWithoutCanvasScissor() throws {
    let source = try source(
        named: "Sources/Main/Sources/Classes/Game/LevelUpPreview3D.swift"
    )
    let teardown = try XCTUnwrap(
        source.range(of: "private func finishQueuedPreviewFrame(")
    )
    let teardownSource = String(source[teardown.lowerBound...])

    XCTAssertTrue(teardownSource.contains(
        "scene.beginFrame(previewFrameDescriptor)"
    ))
    XCTAssertTrue(teardownSource.contains("try scene.endFrame()"))
    XCTAssertFalse(teardownSource.contains(".pushScissor"))
    XCTAssertFalse(teardownSource.contains(".popScissor"))
}
```

Extend the preview descriptor test:

```swift
let viewport = NormalizedViewport(
    x: 0.02,
    y: 0.19,
    width: 0.54,
    height: 0.78
)
let previewDescriptor = LevelUpPreview3D.previewFrameDescriptor(
    camera: previewCamera,
    viewport: viewport,
    inheriting: gameplayDescriptor
)
XCTAssertEqual(previewDescriptor.viewport, viewport)
```

Add:

```swift
func testPreviewRectNormalizesAgainstLogicalCanvas() {
    XCTAssertEqual(
        LevelUpPreview3D.normalizedViewport(
            .init(x: 24, y: 144, width: 680, height: 552),
            logicalSize: .init(width: 1_280, height: 720)
        ),
        .init(
            x: 24.0 / 1_280.0,
            y: 144.0 / 720.0,
            width: 680.0 / 1_280.0,
            height: 552.0 / 720.0
        )
    )
}
```

In `SceneRendererRuntimeSafetyTests.swift`, replace the clipped lifecycle test with:

```swift
func testQueuedSceneFrameOwnsBeginDrawAndFlushInOrder() {
    var lifecycle = QueuedSceneFrameLifecycle()
    var actions: [String] = []

    lifecycle.run(
        claimFrameOwnership: { actions.append("owner") },
        beginScene: { actions.append("begin") },
        queueDraws: { actions.append("draw") },
        flushQueuedDraws: { actions.append("flush") }
    )

    XCTAssertEqual(actions, ["owner", "begin", "draw", "flush"])
    XCTAssertEqual(lifecycle.phase, .finished)
}
```

- [ ] **Step 2: Prove preview tests are red**

Run:

```bash
swift test --filter FusionPreviewPolicyTests
swift test --filter SceneRendererRuntimeSafetyTests
```

Expected: compilation/assertion failures because preview descriptors do not carry a viewport and the lifecycle still exposes clip phases.

- [ ] **Step 3: Simplify the queued lifecycle**

Change `QueuedSceneFrameLifecycle.run` to:

```swift
mutating func run(
    claimFrameOwnership: () -> Void,
    beginScene: () -> Void,
    queueDraws: () -> Void,
    flushQueuedDraws: () -> Void
) {
    precondition(phase == .idle || phase == .finished)
    phase = .idle

    claimFrameOwnership()
    phase = .frameOwned
    beginScene()
    phase = .sceneActive
    queueDraws()
    phase = .drawsQueued
    flushQueuedDraws()
    phase = .flushed
    phase = .finished
}
```

Remove `isClipActive`, `activateClip`, `releaseClip`, and the boolean flush argument.

In `ParticleClassicPreviewHost.runOwnedQueuedFrame`, forward the closure directly:

```swift
lifecycle.run(
    claimFrameOwnership: claimFrameOwnership,
    beginScene: beginScene,
    queueDraws: queueDraws,
    flushQueuedDraws: flushQueuedDraws
)
```

- [ ] **Step 4: Normalize the preview rectangle**

Add to `LevelUpPreview3D`:

```swift
static func normalizedViewport(
    _ viewport: RenderRect,
    logicalSize: PixelSize2D
) -> NormalizedViewport {
    let width = max(1, Float(logicalSize.width))
    let height = max(1, Float(logicalSize.height))
    return .init(
        x: viewport.x / width,
        y: viewport.y / height,
        width: viewport.width / width,
        height: viewport.height / height
    )
}
```

Change `previewFrameDescriptor` to accept and propagate:

```swift
static func previewFrameDescriptor(
    camera: RenderCamera,
    viewport: NormalizedViewport,
    inheriting descriptor: FrameDescriptor
) -> FrameDescriptor {
    FrameDescriptor(
        camera: camera,
        environment: descriptor.environment,
        renderScale: descriptor.renderScale,
        antiAliasing: descriptor.antiAliasing,
        upscaleMode: descriptor.upscaleMode,
        postChain: descriptor.postChain,
        viewport: viewport
    )
}
```

- [ ] **Step 5: Remove camera compensation and clip submission**

Delete `centeredPreviewCamera(for:logicalSize:)`.

In `renderFusionScreen`, remove `clipRect` creation and use:

```swift
let previewCamera = previewCameraController.camera
let normalizedViewport = Self.normalizedViewport(
    viewport,
    logicalSize: logicalSize
)
```

Pass `normalizedViewport` into `finishQueuedPreviewFrame`.

Change `finishQueuedPreviewFrame` to accept:

```swift
viewport: NormalizedViewport
```

Build the descriptor with:

```swift
let previewFrameDescriptor = Self.previewFrameDescriptor(
    camera: previewCamera,
    viewport: viewport,
    inheriting: frameDescriptor
)
```

Run the lifecycle without clip callbacks:

```swift
lifecycle.run(
    claimFrameOwnership: { SceneRenderer.beginFrame() },
    beginScene: { scene.beginFrame(previewFrameDescriptor) },
    queueDraws: { drawFusionPreviewScene(player: player) },
    flushQueuedDraws: {
        do {
            try scene.endFrame()
        } catch {
            preconditionFailure(
                "Level-up preview scene backend failed to end its frame: \(error)"
            )
        }
    }
)
```

- [ ] **Step 6: Run focused and full game tests**

Run:

```bash
swift test --filter FusionPreviewPolicyTests
swift test --filter SceneRendererRuntimeSafetyTests
swift test --filter LegacyRenderAdapterTests
swift test
```

Expected: all focused and complete XCTest suites pass.

- [ ] **Step 7: Commit the preview migration**

From the game repository root:

```bash
git add \
  "City Generator (r3d)/Sources/Main/Sources/Classes/Game/LevelUpPreview3D.swift" \
  "City Generator (r3d)/Sources/Main/Sources/Classes/ParticleSystem/ParticleClassicPreviewHost.swift" \
  "City Generator (r3d)/Tests/MainTests/FusionPreviewPolicyTests.swift" \
  "City Generator (r3d)/Tests/MainTests/SceneRendererRuntimeSafetyTests.swift"
git commit -m "fix: render card preview through scene viewport"
```

---

### Task 4: Verify High-DPI Presentation and Build Integrity

**Files:**
- Verify only unless an acceptance criterion fails.

**Interfaces:**
- Consumes: direct preview viewport implementation.
- Produces: build/test evidence and a macOS high-DPI visual check.

- [ ] **Step 1: Verify clean renderer and game builds**

Run:

```bash
git -C /Users/duke4e/Desktop/r3d/r3d status --short
swift test
swift package clean
swift build
xcodebuild \
  -scheme Main \
  -destination 'platform=macOS' \
  -derivedDataPath /private/tmp/btbh-preview-derived-data \
  clean build
```

Run renderer `swift test` from `/Users/duke4e/Desktop/r3d/r3d`; run the remaining commands from `City Generator (r3d)`.

Expected: both test suites and both clean game builds pass.

- [ ] **Step 2: Launch the interactive macOS build**

Run from `City Generator (r3d)`:

```bash
.build/debug/Main --game
```

Open the player-type selection screen at the normal system drawable scale.

- [ ] **Step 3: Check the approved visual criteria**

Confirm all of the following in the live window:

- no white strip exists along the left or bottom inside the preview frame;
- the 3D image reaches all four inner frame edges;
- 3D content does not overwrite the preview frame or right-side UI;
- the preview player remains centered under the existing camera angle;
- resizing or moving the window between Retina and non-Retina displays does not recreate an offset.

If any item fails, stop and capture the framebuffer/logical sizes plus resolved `R3D_View.viewport`; do not add a hardcoded pixel offset. Use the design's offscreen render-target fallback only after proving the direct viewport cannot satisfy these criteria.

- [ ] **Step 4: Run legacy baseline comparison**

Run:

```bash
./tools/capture-render-baselines.sh \
  --candidate .build/render-baselines/preview-viewport
```

Expected: no regression outside the intentionally changed preview area. Any changed approved cue requires a screenshot review before acceptance.

- [ ] **Step 5: Record final state**

Run:

```bash
git -C /Users/duke4e/Desktop/r3d/r3d status --short
git -C /Users/duke4e/Documents/GitHub/Be-The-Bullet-Hell status --short
```

Expected: both repositories are clean and each task's commits are present on `codex/renderer-phase-0-1`.
