# Renderer Phase 0–1 Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish reproducible legacy visual/performance/API baselines, then move Be The Bullet Hell behind project-owned platform, Canvas2D, resource, and scene seams while the existing raylib/r3d renderer remains the implementation.

**Architecture:** The private `duke4e/btbh-renderer` repository is the renderer source of truth and is pinned in the game as `City Generator (r3d)/Vendor/BTBHRenderer`. That package owns portable value types, generational opaque handles, and narrow Swift interfaces backed by a C11 handle core. The game supplies a `Renderer/Legacy` adapter bundle that is the only code allowed to import raylib, r3d, rlgl, or SDL3; no SDL_GPU, Metal, Vulkan, Radiance Cascades, or new G-buffer work occurs in this plan.

**Tech Stack:** C11, Swift 5.10, SwiftPM, XCTest, CMake, zsh profiling/capture scripts, legacy raylib/r3d/OpenGL inside the adapter only, SDL3 inside the platform adapter only.

## Global Constraints

- Renderer work starts from renderer commit `92d83c9`; game work starts from current authoritative game commit `cf8e740a`, which includes the completed projectile pipeline work after the original `b13cb809` planning snapshot.
- Execute in isolated worktrees created with `superpowers:using-git-worktrees`; preserve the unrelated `.DS_Store` changes already present in the game repository.
- Use branch `codex/renderer-phase-0-1` in both repositories so the renderer submodule can pin a named remote branch during development and a detached commit at each game commit.
- The renderer private remote is `git@github.com:duke4e/btbh-renderer.git`; retain `https://github.com/Bigfoot71/r3d.git` as read-only remote `upstream`.
- The game records the renderer as a relative submodule URL `../btbh-renderer.git` at `City Generator (r3d)/Vendor/BTBHRenderer`.
- First release platforms remain macOS/Metal and Windows/Vulkan; Phase 1 continues to execute the legacy OpenGL renderer internally and does not claim backend migration.
- All five launch modes remain supported: game, combat lab, particle editor, city-block editor, and unified-city.
- Reference output remains 1920×1080. The approved final target is p95 presented frame time at or below 16.67 ms on Apple M1 and GTX 1060/RX 580-class hardware; Phase 0 records the legacy baseline without weakening this target.
- Visual baselines use deterministic inputs, approved golden images, cue-region manifests, and SSIM threshold `0.98`; exact hash equality is recorded but is not the cross-backend gate.
- A performance change greater than 5% in either median or p95 requires a comparable-load capture and written explanation.
- No render-loop general heap allocation is introduced. Reusable command and capture buffers retain capacity after warm-up.
- The only Swift files allowed to import `raylib`, `r3d`, or `SDL3` at the end of Task 11 are files under `Sources/Main/Sources/Renderer/Legacy/`.
- `rlgl` calls are allowed only inside `LegacyCanvasBackend.swift` or `LegacySceneBackend.swift`.
- Public renderer package declarations must not mention raylib, r3d, rlgl, SDL, OpenGL, Metal, or Vulkan types.
- Existing miniaudio ownership is unchanged.
- Runtime Assimp behavior is unchanged in Phase 1; the legacy resource adapter hides it behind project handles.
- Phase 2 SDL_GPU device creation, shader compilation, G-buffer work, and compute work are explicitly outside this plan.
- Phase 5 Radiance Cascades and CameraProfile behavior are explicitly outside this plan.

## Repository And File Map

Paths marked **Renderer** are relative to the `btbh-renderer` repository. Paths marked **Game** are relative to the `Be-The-Bullet-Hell` repository root.

### Renderer files

- `Package.swift`: SwiftPM product consumed by the game submodule.
- `Sources/BTBHRendererCore/include/btbh_renderer_core.h`: C ABI version and generational handle pool.
- `Sources/BTBHRendererCore/btbh_renderer_core.c`: C11 handle packing, validation, release, and growth.
- `Sources/BTBHRenderer/RenderMath.swift`: project vector, quaternion, matrix, rectangle, bounds, color, and camera POD values.
- `Sources/BTBHRenderer/RenderHandles.swift`: strongly typed opaque resource handles.
- `Sources/BTBHRenderer/Platform.swift`: platform/window/input interface.
- `Sources/BTBHRenderer/Canvas2D.swift`: 2D/text/image interface.
- `Sources/BTBHRenderer/RenderResources.swift`: asset/material/animation/environment interface.
- `Sources/BTBHRenderer/SceneSubmission.swift`: immutable frame and scene draw commands.
- `Sources/BTBHRenderer/RenderServices.swift`: process-scoped backend bundle and installation lifecycle.
- `Tests/BTBHRendererTests/HandlePoolTests.swift`: generation and stale-handle tests.
- `Tests/BTBHRendererTests/RenderMathTests.swift`: math/projection tests.
- `Tests/BTBHRendererTests/RenderServicesTests.swift`: interface lifecycle and recording-backend tests.
- `Tests/BTBHRendererTests/PublicDependencyBoundaryTests.swift`: prevents backend/vendor types from entering the public package.

### Game baseline files

- `City Generator (r3d)/Sources/RenderBaselineSupport/RenderAPIAudit.swift`: deterministic source scanner and manifest model.
- `City Generator (r3d)/Sources/RenderBaselineSupport/RenderBaselineCapture.swift`: capture configuration, state machine, metrics, and cue regions.
- `City Generator (r3d)/Tests/MainTests/RenderAPIAuditTests.swift`: locks the starting dependency surface.
- `City Generator (r3d)/Tests/MainTests/RenderBaselineCaptureTests.swift`: deterministic capture/metric state tests.
- `City Generator (r3d)/Tests/RenderGoldens/legacy/manifest.json`: approved mode, resolution, hash, SSIM, and cue-region data.
- `City Generator (r3d)/Tests/RenderGoldens/legacy/*.png`: approved legacy images.
- `City Generator (r3d)/tools/audit-render-api.sh`: writes the source/API manifest.
- `City Generator (r3d)/tools/capture-render-baselines.sh`: exact-PID five-mode capture runner.
- `City Generator (r3d)/Sources/RenderBaselineSupport/QOI.swift`: dependency-free lossless RGBA baseline encoding.
- `City Generator (r3d)/Sources/RenderBaselineComparator/main.swift`: deterministic global and cue-region SSIM comparator.
- `City Generator (r3d)/docs/rendering/legacy-api-usage.txt`: committed Phase 0 API inventory.
- `City Generator (r3d)/docs/rendering/legacy-baseline-report.md`: committed M1 and Windows reference measurements.

### Game seam and adapter files

- `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyConversions.swift`: project ↔ raw C value conversion only.
- `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyPlatformBackend.swift`: raylib window/input plus SDL3 gamepad/dialog implementation.
- `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyCanvasBackend.swift`: raylib/rlgl 2D, text, texture, and scissor implementation.
- `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyResourceBackend.swift`: raw r3d/raylib asset ownership behind handles.
- `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacySceneBackend.swift`: r3d scene submission, instancing, lights, environment, and debug draws.
- `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyRenderServices.swift`: constructs one coherent backend bundle and owns shutdown order.
- `City Generator (r3d)/Sources/Main/Sources/Renderer/RenderBootstrap.swift`: installs the selected bundle before a launch mode runs.
- `City Generator (r3d)/Tests/MainTests/RendererDependencyBoundaryTests.swift`: enforces the final raw-dependency boundary.
- `City Generator (r3d)/Tests/MainTests/LegacyRenderAdapterTests.swift`: conversion, lifetime, and recording tests.

---

### Task 1: Establish The Private Renderer Repository And SwiftPM Product

**Files:**
- Create (Renderer): `Package.swift`
- Create (Renderer): `Sources/BTBHRendererCore/include/btbh_renderer_core.h`
- Create (Renderer): `Sources/BTBHRendererCore/btbh_renderer_core.c`
- Create (Renderer): `Sources/BTBHRenderer/RendererVersion.swift`
- Create (Renderer): `Tests/BTBHRendererTests/RendererVersionTests.swift`
- Modify (Renderer): `.gitignore`

**Interfaces:**
- Consumes: renderer commit `92d83c9` and existing CMake r3d target unchanged.
- Produces: SwiftPM library product `BTBHRenderer`, C function `btbh_renderer_abi_version() -> UInt32`, and Swift constant `RendererABI.version == 1`.

- [ ] **Step 1: Create the private remote without losing the upstream remote**

From the renderer worktree, run:

```bash
git remote rename origin upstream
gh repo create duke4e/btbh-renderer --private --source . --remote origin
git remote set-url origin git@github.com:duke4e/btbh-renderer.git
git remote -v
```

Expected: `origin` points to exactly `git@github.com:duke4e/btbh-renderer.git`, while `upstream` points to `https://github.com/Bigfoot71/r3d.git`. Do not push until the package test passes.

- [ ] **Step 2: Add the failing ABI version test**

Create `Tests/BTBHRendererTests/RendererVersionTests.swift`:

```swift
import XCTest
@testable import BTBHRenderer

final class RendererVersionTests: XCTestCase {
    func testSwiftAndCABIReportVersionOne() {
        XCTAssertEqual(RendererABI.version, 1)
        XCTAssertEqual(RendererABI.cVersion, RendererABI.version)
    }
}
```

- [ ] **Step 3: Run the test and verify the package does not exist yet**

Run:

```bash
swift test --filter RendererVersionTests
```

Expected: failure because the repository has no `Package.swift` or `BTBHRenderer` module.

- [ ] **Step 4: Add the package and minimal ABI implementation**

Create `Package.swift`:

```swift
// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "BTBHRenderer",
    platforms: [.macOS(.v11)],
    products: [
        .library(name: "BTBHRenderer", targets: ["BTBHRenderer"])
    ],
    targets: [
        .target(
            name: "BTBHRendererCore",
            path: "Sources/BTBHRendererCore",
            publicHeadersPath: "include"
        ),
        .target(
            name: "BTBHRenderer",
            dependencies: ["BTBHRendererCore"],
            path: "Sources/BTBHRenderer"
        ),
        .testTarget(
            name: "BTBHRendererTests",
            dependencies: ["BTBHRenderer"],
            path: "Tests/BTBHRendererTests"
        )
    ]
)
```

Create `Sources/BTBHRendererCore/include/btbh_renderer_core.h`:

```c
#ifndef BTBH_RENDERER_CORE_H
#define BTBH_RENDERER_CORE_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define BTBH_RENDERER_ABI_VERSION UINT32_C(1)

uint32_t btbh_renderer_abi_version(void);

#if defined(__cplusplus)
}
#endif

#endif
```

Create `Sources/BTBHRendererCore/btbh_renderer_core.c`:

```c
#include "btbh_renderer_core.h"

uint32_t btbh_renderer_abi_version(void) {
    return BTBH_RENDERER_ABI_VERSION;
}
```

Create `Sources/BTBHRenderer/RendererVersion.swift`:

```swift
import BTBHRendererCore

public enum RendererABI {
    public static let version: UInt32 = 1
    public static var cVersion: UInt32 { btbh_renderer_abi_version() }
}
```

Append these entries to `.gitignore`:

```text
.build/
.swiftpm/
```

- [ ] **Step 5: Verify SwiftPM and the unchanged legacy CMake build**

Run:

```bash
swift test --filter RendererVersionTests
cmake --preset macos-debug
cmake --build out/build/macos-debug --target r3d
```

Expected: `RendererVersionTests` passes and the existing `r3d` target builds. On Windows, replace the preset with `x64-debug`.

- [ ] **Step 6: Commit and publish the package foundation**

```bash
git add Package.swift .gitignore Sources/BTBHRendererCore Sources/BTBHRenderer Tests/BTBHRendererTests/RendererVersionTests.swift
git commit -m "build: add BTBH renderer package foundation"
git push -u origin HEAD
```

### Task 2: Add Project POD Types And Generational Handles

**Files:**
- Modify (Renderer): `Sources/BTBHRendererCore/include/btbh_renderer_core.h`
- Modify (Renderer): `Sources/BTBHRendererCore/btbh_renderer_core.c`
- Create (Renderer): `Sources/BTBHRenderer/RenderMath.swift`
- Create (Renderer): `Sources/BTBHRenderer/RenderHandles.swift`
- Create (Renderer): `Tests/BTBHRendererTests/HandlePoolTests.swift`
- Create (Renderer): `Tests/BTBHRendererTests/RenderMathTests.swift`

**Interfaces:**
- Consumes: `BTBHRendererCore` ABI version 1.
- Produces: `BTBHHandlePool`, `btbh_handle_pool_create/acquire/contains/release/destroy`, project math values, and strongly typed handles whose raw value packs generation in bits 32–63 and `index + 1` in bits 0–31.

- [ ] **Step 1: Write failing handle-generation and math tests**

Create `Tests/BTBHRendererTests/HandlePoolTests.swift`:

```swift
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
```

Create `Tests/BTBHRendererTests/RenderMathTests.swift`:

```swift
import XCTest
@testable import BTBHRenderer

final class RenderMathTests: XCTestCase {
    func testVectorAndRectangleOperationsAreBackendIndependent() {
        XCTAssertEqual(RenderVec2(x: 3, y: 4).length, 5, accuracy: 0.0001)
        XCTAssertEqual(
            RenderVec3(x: 1, y: 0, z: 0).cross(RenderVec3(x: 0, y: 1, z: 0)),
            RenderVec3(x: 0, y: 0, z: 1)
        )
        XCTAssertTrue(RenderRect(x: 10, y: 20, width: 30, height: 40).contains(RenderVec2(x: 25, y: 30)))
    }

    func testCameraUsesExplicitPerspectiveProjection() {
        let camera = RenderCamera(
            position: RenderVec3(x: 0, y: 10, z: 10),
            target: .zero,
            up: .up,
            verticalFOVDegrees: 45,
            projection: .perspective
        )
        XCTAssertEqual(camera.projection, .perspective)
        XCTAssertEqual(camera.verticalFOVDegrees, 45)
    }

    func testSwiftPODRoundTripsThroughTheCABI() {
        let value = RenderBounds3(
            minimum: RenderVec3(x: -1, y: -2, z: -3),
            maximum: RenderVec3(x: 4, y: 5, z: 6)
        )
        XCTAssertEqual(RenderBounds3(core: value.core), value)
    }
}
```

- [ ] **Step 2: Confirm the new interfaces are missing**

Run:

```bash
swift test --filter HandlePoolTests
swift test --filter RenderMathTests
```

Expected: compilation fails on `HandlePool`, handle types, and render math types.

- [ ] **Step 3: Add the C handle-pool contract**

Add to `btbh_renderer_core.h`:

```c
typedef struct BTBHVec2 { float x, y; } BTBHVec2;
typedef struct BTBHVec3 { float x, y, z; } BTBHVec3;
typedef struct BTBHVec4 { float x, y, z, w; } BTBHVec4;
typedef struct BTBHQuaternion { float x, y, z, w; } BTBHQuaternion;
typedef struct BTBHMatrix4 { float elements[16]; } BTBHMatrix4;
typedef struct BTBHColor { uint8_t r, g, b, a; } BTBHColor;
typedef struct BTBHRect { float x, y, width, height; } BTBHRect;
typedef struct BTBHBounds3 { BTBHVec3 minimum, maximum; } BTBHBounds3;
typedef enum BTBHProjection { BTBH_PROJECTION_PERSPECTIVE = 0, BTBH_PROJECTION_ORTHOGRAPHIC = 1 } BTBHProjection;
typedef struct BTBHCamera {
    BTBHVec3 position;
    BTBHVec3 target;
    BTBHVec3 up;
    float vertical_fov_degrees;
    BTBHProjection projection;
} BTBHCamera;

typedef struct BTBHHandlePool BTBHHandlePool;

typedef enum BTBHHandleResult {
    BTBH_HANDLE_OK = 0,
    BTBH_HANDLE_INVALID_ARGUMENT = 1,
    BTBH_HANDLE_OUT_OF_MEMORY = 2,
    BTBH_HANDLE_EXHAUSTED = 3,
    BTBH_HANDLE_STALE = 4
} BTBHHandleResult;

BTBHHandlePool *btbh_handle_pool_create(uint32_t initial_capacity);
void btbh_handle_pool_destroy(BTBHHandlePool *pool);
BTBHHandleResult btbh_handle_pool_acquire(BTBHHandlePool *pool, uint64_t *out_handle);
int btbh_handle_pool_contains(const BTBHHandlePool *pool, uint64_t handle);
BTBHHandleResult btbh_handle_pool_release(BTBHHandlePool *pool, uint64_t handle);
```

Implement `btbh_renderer_core.c` with these exact invariants:

```c
#include "btbh_renderer_core.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct BTBHHandleSlot {
    uint32_t generation;
    uint32_t next_free;
    bool occupied;
} BTBHHandleSlot;

struct BTBHHandlePool {
    BTBHHandleSlot *slots;
    uint32_t capacity;
    uint32_t free_head;
};

static const uint32_t BTBH_NO_SLOT = UINT32_MAX;

static uint64_t btbh_pack_handle(uint32_t index, uint32_t generation) {
    return ((uint64_t)generation << 32u) | ((uint64_t)index + 1u);
}

static bool btbh_unpack_handle(uint64_t handle, uint32_t *index, uint32_t *generation) {
    const uint32_t encoded_index = (uint32_t)(handle & UINT64_C(0xffffffff));
    if (encoded_index == 0u) return false;
    *index = encoded_index - 1u;
    *generation = (uint32_t)(handle >> 32u);
    return *generation != 0u;
}

static bool btbh_handle_pool_grow(BTBHHandlePool *pool) {
    const uint32_t old_capacity = pool->capacity;
    const uint32_t new_capacity = old_capacity > 0u ? old_capacity * 2u : 8u;
    if (new_capacity < old_capacity) return false;
    BTBHHandleSlot *grown = realloc(pool->slots, sizeof(*grown) * new_capacity);
    if (!grown) return false;
    pool->slots = grown;
    memset(pool->slots + old_capacity, 0, sizeof(*grown) * (new_capacity - old_capacity));
    for (uint32_t index = old_capacity; index < new_capacity; ++index) {
        pool->slots[index].generation = 1u;
        pool->slots[index].next_free = index + 1u < new_capacity ? index + 1u : pool->free_head;
    }
    pool->free_head = old_capacity;
    pool->capacity = new_capacity;
    return true;
}

BTBHHandlePool *btbh_handle_pool_create(uint32_t initial_capacity) {
    BTBHHandlePool *pool = calloc(1u, sizeof(*pool));
    if (!pool) return NULL;
    pool->free_head = BTBH_NO_SLOT;
    while (pool->capacity < initial_capacity) {
        if (!btbh_handle_pool_grow(pool)) {
            btbh_handle_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

void btbh_handle_pool_destroy(BTBHHandlePool *pool) {
    if (!pool) return;
    free(pool->slots);
    free(pool);
}

BTBHHandleResult btbh_handle_pool_acquire(BTBHHandlePool *pool, uint64_t *out_handle) {
    if (!pool || !out_handle) return BTBH_HANDLE_INVALID_ARGUMENT;
    if (pool->free_head == BTBH_NO_SLOT && !btbh_handle_pool_grow(pool)) return BTBH_HANDLE_OUT_OF_MEMORY;
    const uint32_t index = pool->free_head;
    BTBHHandleSlot *slot = &pool->slots[index];
    pool->free_head = slot->next_free;
    slot->next_free = BTBH_NO_SLOT;
    slot->occupied = true;
    *out_handle = btbh_pack_handle(index, slot->generation);
    return BTBH_HANDLE_OK;
}

int btbh_handle_pool_contains(const BTBHHandlePool *pool, uint64_t handle) {
    uint32_t index = 0u;
    uint32_t generation = 0u;
    if (!pool || !btbh_unpack_handle(handle, &index, &generation) || index >= pool->capacity) return 0;
    const BTBHHandleSlot *slot = &pool->slots[index];
    return slot->occupied && slot->generation == generation;
}

BTBHHandleResult btbh_handle_pool_release(BTBHHandlePool *pool, uint64_t handle) {
    uint32_t index = 0u;
    uint32_t generation = 0u;
    if (!pool || !btbh_unpack_handle(handle, &index, &generation) || index >= pool->capacity) return BTBH_HANDLE_INVALID_ARGUMENT;
    BTBHHandleSlot *slot = &pool->slots[index];
    if (!slot->occupied || slot->generation != generation) return BTBH_HANDLE_STALE;
    slot->occupied = false;
    slot->generation += 1u;
    if (slot->generation == 0u) slot->generation = 1u;
    slot->next_free = pool->free_head;
    pool->free_head = index;
    return BTBH_HANDLE_OK;
}
```

- [ ] **Step 4: Add strongly typed Swift handles and pool ownership**

Create `RenderHandles.swift` with `RenderHandle`, `HandlePool`, and these concrete handle types:

```swift
import BTBHRendererCore

public protocol RenderHandle: Hashable, Sendable {
    init(rawValue: UInt64)
    var rawValue: UInt64 { get }
    static var invalid: Self { get }
}

public extension RenderHandle {
    static var invalid: Self { Self(rawValue: 0) }
    var isValid: Bool { rawValue != 0 }
}

public struct TextureHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct FontHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct RenderTargetHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct MeshHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct ModelHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct MaterialHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct InstanceBufferHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct AnimationLibraryHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct AnimationPlayerHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct AnimationTreeHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct LightHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct CubemapHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct AmbientMapHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct SurfaceShaderHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }
public struct ScreenShaderHandle: RenderHandle { public let rawValue: UInt64; public init(rawValue: UInt64) { self.rawValue = rawValue } }

public enum HandlePoolError: Error { case allocationFailed; case exhausted }

public final class HandlePool {
    private let raw: OpaquePointer

    public init(initialCapacity: UInt32) throws {
        guard let raw = btbh_handle_pool_create(initialCapacity) else { throw HandlePoolError.allocationFailed }
        self.raw = raw
    }

    deinit { btbh_handle_pool_destroy(raw) }

    public func acquire<H: RenderHandle>(_ type: H.Type) throws -> H {
        var value: UInt64 = 0
        guard btbh_handle_pool_acquire(raw, &value) == BTBH_HANDLE_OK else { throw HandlePoolError.exhausted }
        return H(rawValue: value)
    }

    public func contains<H: RenderHandle>(_ handle: H) -> Bool {
        btbh_handle_pool_contains(raw, handle.rawValue) != 0
    }

    @discardableResult public func release<H: RenderHandle>(_ handle: H) -> Bool {
        btbh_handle_pool_release(raw, handle.rawValue) == BTBH_HANDLE_OK
    }
}
```

- [ ] **Step 5: Add backend-neutral render math values**

Create `RenderMath.swift` with the tested declarations and no imported graphics module:

```swift
import Foundation

public struct RenderVec2: Hashable, Sendable {
    public var x: Float
    public var y: Float
    public init(x: Float, y: Float) { self.x = x; self.y = y }
    public static let zero = Self(x: 0, y: 0)
    public static let one = Self(x: 1, y: 1)
    public static let up = Self(x: 0, y: -1)
    public static let down = Self(x: 0, y: 1)
    public static let left = Self(x: -1, y: 0)
    public static let right = Self(x: 1, y: 0)
    public var length: Float { sqrt(x * x + y * y) }
    public func normalized(epsilon: Float = 1e-6) -> Self {
        let value = length
        return value > epsilon ? self / value : .zero
    }
    public static func + (lhs: Self, rhs: Self) -> Self { Self(x: lhs.x + rhs.x, y: lhs.y + rhs.y) }
    public static func - (lhs: Self, rhs: Self) -> Self { Self(x: lhs.x - rhs.x, y: lhs.y - rhs.y) }
    public static func * (lhs: Self, rhs: Float) -> Self { Self(x: lhs.x * rhs, y: lhs.y * rhs) }
    public static func / (lhs: Self, rhs: Float) -> Self { Self(x: lhs.x / rhs, y: lhs.y / rhs) }
}

public struct RenderVec3: Hashable, Sendable {
    public var x: Float
    public var y: Float
    public var z: Float
    public init(x: Float, y: Float, z: Float) { self.x = x; self.y = y; self.z = z }
    public static let zero = Self(x: 0, y: 0, z: 0)
    public static let up = Self(x: 0, y: 1, z: 0)
    public func dot(_ rhs: Self) -> Float { x * rhs.x + y * rhs.y + z * rhs.z }
    public func cross(_ rhs: Self) -> Self {
        Self(x: y * rhs.z - z * rhs.y, y: z * rhs.x - x * rhs.z, z: x * rhs.y - y * rhs.x)
    }
}

public struct RenderVec4: Hashable, Sendable {
    public var x: Float; public var y: Float; public var z: Float; public var w: Float
    public init(x: Float, y: Float, z: Float, w: Float) { self.x = x; self.y = y; self.z = z; self.w = w }
}

public struct RenderQuaternion: Hashable, Sendable {
    public var x: Float; public var y: Float; public var z: Float; public var w: Float
    public init(x: Float, y: Float, z: Float, w: Float) { self.x = x; self.y = y; self.z = z; self.w = w }
    public static let identity = Self(x: 0, y: 0, z: 0, w: 1)
}

public struct RenderMatrix4: Hashable, Sendable {
    public var c0: SIMD4<Float>; public var c1: SIMD4<Float>; public var c2: SIMD4<Float>; public var c3: SIMD4<Float>
    public init(c0: SIMD4<Float>, c1: SIMD4<Float>, c2: SIMD4<Float>, c3: SIMD4<Float>) {
        self.c0 = c0; self.c1 = c1; self.c2 = c2; self.c3 = c3
    }
    public static let identity = Self(
        c0: SIMD4(1, 0, 0, 0), c1: SIMD4(0, 1, 0, 0),
        c2: SIMD4(0, 0, 1, 0), c3: SIMD4(0, 0, 0, 1)
    )
}

public struct RenderColor: Hashable, Sendable {
    public var r: UInt8; public var g: UInt8; public var b: UInt8; public var a: UInt8
    public init(r: UInt8, g: UInt8, b: UInt8, a: UInt8 = 255) { self.r = r; self.g = g; self.b = b; self.a = a }
    public static let white = Self(r: 255, g: 255, b: 255, a: 255)
    public static let black = Self(r: 0, g: 0, b: 0, a: 255)
    public static let clear = Self(r: 0, g: 0, b: 0, a: 0)
}

public struct RenderRect: Hashable, Sendable {
    public var x: Float; public var y: Float; public var width: Float; public var height: Float
    public init(x: Float, y: Float, width: Float, height: Float) { self.x = x; self.y = y; self.width = width; self.height = height }
    public func contains(_ point: RenderVec2) -> Bool {
        point.x >= x && point.y >= y && point.x < x + width && point.y < y + height
    }
}

public struct RenderBounds3: Hashable, Sendable {
    public var minimum: RenderVec3; public var maximum: RenderVec3
    public init(minimum: RenderVec3, maximum: RenderVec3) { self.minimum = minimum; self.maximum = maximum }
}

public enum RenderProjection: UInt8, Hashable, Sendable { case perspective; case orthographic }

public struct RenderCamera: Hashable, Sendable {
    public var position: RenderVec3; public var target: RenderVec3; public var up: RenderVec3
    public var verticalFOVDegrees: Float; public var projection: RenderProjection
    public init(position: RenderVec3, target: RenderVec3, up: RenderVec3, verticalFOVDegrees: Float, projection: RenderProjection) {
        self.position = position; self.target = target; self.up = up
        self.verticalFOVDegrees = verticalFOVDegrees; self.projection = projection
    }
}
```

Add field-by-field internal `core` properties and `init(core:)` initializers for every Swift POD above. The conversion is explicit; do not use `unsafeBitCast` or assume that Swift SIMD and C array layouts are identical. `RenderMatrix4.core` writes all 16 values in column-major order, matching the documented renderer convention.

- [ ] **Step 6: Run all renderer package tests and commit**

```bash
swift test
git add Sources/BTBHRendererCore Sources/BTBHRenderer Tests/BTBHRendererTests
git commit -m "feat: add renderer POD types and generational handles"
git push
```

Expected: all tests pass; stale handles remain invalid after slot reuse.

### Task 3: Add The Renderer Submodule And Lock The Legacy API Inventory

**Files:**
- Modify (Game): `.gitmodules`
- Add gitlink (Game): `City Generator (r3d)/Vendor/BTBHRenderer`
- Modify (Game): `City Generator (r3d)/Package.swift`
- Create (Game): `City Generator (r3d)/Sources/RenderBaselineSupport/RenderAPIAudit.swift`
- Create (Game): `City Generator (r3d)/Sources/RenderAPIAuditTool/main.swift`
- Create (Game): `City Generator (r3d)/Tests/MainTests/RenderAPIAuditTests.swift`
- Create (Game): `City Generator (r3d)/tools/audit-render-api.sh`
- Create (Game): `City Generator (r3d)/docs/rendering/legacy-api-usage.txt`

**Interfaces:**
- Consumes: renderer package commit from Task 2.
- Produces: local SwiftPM dependency `BTBHRenderer`, deterministic `RenderAPIInventory`, and committed baseline showing 85 raw-import files, 98 called r3d functions, 116 raylib/raymath candidates, five rlgl functions, and five SDL3-import files on game commit `cf8e740a`.

- [ ] **Step 1: Add the submodule at the approved path**

From the `Be-The-Bullet-Hell` worktree root:

```bash
git submodule add ../btbh-renderer.git 'City Generator (r3d)/Vendor/BTBHRenderer'
git -C 'City Generator (r3d)/Vendor/BTBHRenderer' fetch origin codex/renderer-phase-0-1
git -C 'City Generator (r3d)/Vendor/BTBHRenderer' checkout --detach origin/codex/renderer-phase-0-1
```

Record the full detached commit printed by `git -C 'City Generator (r3d)/Vendor/BTBHRenderer' rev-parse HEAD` in the game commit message body. The `.gitmodules` entry must be:

```ini
[submodule "City Generator (r3d)/Vendor/BTBHRenderer"]
	path = City Generator (r3d)/Vendor/BTBHRenderer
	url = ../btbh-renderer.git
```

- [ ] **Step 2: Write the failing inventory tests**

Create `RenderAPIAuditTests.swift`:

```swift
import XCTest
@testable import RenderBaselineSupport

final class RenderAPIAuditTests: XCTestCase {
    func testLegacyInventoryMatchesTheApprovedStartingPoint() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let inventory = try RenderAPIAudit.scan(projectRoot: root)

        XCTAssertEqual(inventory.rawRaylibOrR3DImportFiles.count, 85)
        XCTAssertEqual(inventory.r3dFunctionCalls.count, 98)
        XCTAssertEqual(inventory.raylibFunctionCandidates.count, 116)
        XCTAssertEqual(inventory.rlglFunctionCalls, [
            "rlDisableScissorTest", "rlDrawRenderBatchActive", "rlEnableScissorTest", "rlScissor", "rlSetBlendMode"
        ])
        XCTAssertEqual(inventory.sdlImportFiles.count, 5)
    }

    func testInventoryRenderingIsSortedAndStable() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let inventory = try RenderAPIAudit.scan(projectRoot: root)
        let first = inventory.text(commit: "cf8e740a")
        let second = inventory.text(commit: "cf8e740a")
        XCTAssertEqual(first, second)
        XCTAssertTrue(first.hasPrefix("BTBH LEGACY RENDER API INVENTORY\ncommit=cf8e740a\n"))
    }
}
```

- [ ] **Step 3: Run the test and confirm the scanner is absent**

Run from `City Generator (r3d)`:

```bash
swift test --filter RenderAPIAuditTests
```

Expected: compilation fails because `RenderAPIAudit` does not exist.

- [ ] **Step 4: Implement the deterministic scanner and package dependency**

Add to the package dependencies, targets, `Main` dependencies, and `MainTests` dependencies:

```swift
dependencies: [
    .package(path: "Vendor/BTBHRenderer")
],
```

```swift
// Add before the Main executable target.
.target(
    name: "RenderBaselineSupport",
    path: "Sources/RenderBaselineSupport"
),
.executableTarget(
    name: "RenderAPIAuditTool",
    dependencies: ["RenderBaselineSupport"],
    path: "Sources/RenderAPIAuditTool"
),
```

```swift
// Main target dependencies.
dependencies: [
    "miniaudio_impl",
    "FusionCore",
    "RenderBaselineSupport",
    .product(name: "BTBHRenderer", package: "BTBHRenderer")
],
```

```swift
// MainTests target dependencies.
dependencies: ["Main", "FusionCore", "RenderBaselineSupport"]
```

`RenderAPIAudit.scan(projectRoot:)` must enumerate only `Sources/Main/**/*.swift`, sort normalized relative paths, parse exact `import raylib`, `import r3d`, and `import SDL3` lines, collect `R3D_*(` and `rl*(` calls, and intersect uppercase call candidates with declarations found in `include/raylib/raylib.h`, `raymath.h`, and `rcamera.h`. It must reject missing roots instead of returning an empty inventory. Expose:

```swift
struct RenderAPIInventory: Equatable {
    let rawRaylibOrR3DImportFiles: [String]
    let sdlImportFiles: [String]
    let r3dFunctionCalls: [String]
    let raylibFunctionCandidates: [String]
    let rlglFunctionCalls: [String]
    func text(commit: String) -> String
}

enum RenderAPIAudit {
    static func scan(projectRoot: URL) throws -> RenderAPIInventory
}
```

Use `NSRegularExpression` with these exact patterns:

```swift
let r3dCall = #"\b(R3D_[A-Za-z0-9_]+)\s*\("#
let rlglCall = #"\b(rl[A-Za-z0-9_]+)\s*\("#
let uppercaseCall = #"\b([A-Z][A-Za-z0-9_]+)\s*\("#
```

- [ ] **Step 5: Add the manifest writer and record the baseline**

Create `tools/audit-render-api.sh`:

```zsh
#!/bin/zsh
set -euo pipefail
project_dir="${0:A:h:h}"
output="${1:-$project_dir/docs/rendering/legacy-api-usage.txt}"
cd "$project_dir"
swift run RenderAPIAuditTool --root "$project_dir" --commit cf8e740a --output "$output"
```

`RenderAPIAuditTool` accepts exactly `--root`, `--commit`, and `--output`, rejects unknown/missing arguments with exit code 64, calls `RenderAPIAudit.scan(projectRoot:)`, and writes atomically through `Data.write(options: .atomic)`.

Run:

```bash
swift run RenderAPIAuditTool --root "$PWD" --commit cf8e740a --output docs/rendering/legacy-api-usage.txt
swift test --filter RenderAPIAuditTests
```

Expected: tests pass and the committed inventory contains the approved counts.

- [ ] **Step 6: Commit the submodule and inventory**

```bash
git add .gitmodules 'City Generator (r3d)/Vendor/BTBHRenderer' 'City Generator (r3d)/Package.swift' 'City Generator (r3d)/Sources/RenderBaselineSupport' 'City Generator (r3d)/Sources/RenderAPIAuditTool' 'City Generator (r3d)/Tests/MainTests/RenderAPIAuditTests.swift' 'City Generator (r3d)/tools/audit-render-api.sh' 'City Generator (r3d)/docs/rendering/legacy-api-usage.txt'
git commit -m "test: lock legacy renderer API inventory"
```

### Task 4: Build Deterministic Five-Mode Visual And Performance Baselines

**Files:**
- Create (Game): `City Generator (r3d)/Sources/RenderBaselineSupport/RenderBaselineCapture.swift`
- Create (Game): `City Generator (r3d)/Sources/RenderBaselineSupport/RenderBaselineMetrics.swift`
- Create (Game): `City Generator (r3d)/Sources/RenderBaselineSupport/QOI.swift`
- Create (Game): `City Generator (r3d)/Sources/RenderBaselineComparator/main.swift`
- Create (Game): `City Generator (r3d)/Tests/MainTests/RenderBaselineCaptureTests.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/main.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/ParticleSystem/ParticleDemo.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditorRunner.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/UnifiedCityGenerator/UnifiedCityGeneratorRunner.swift`
- Create (Game): `City Generator (r3d)/tools/capture-render-baselines.sh`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/manifest.json`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/game.png`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/game.qoi`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/combat-lab.png`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/combat-lab.qoi`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/particle-editor.png`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/particle-editor.qoi`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/city-block-editor.png`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/city-block-editor.qoi`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/unified-city.png`
- Create (Game): `City Generator (r3d)/Tests/RenderGoldens/legacy/unified-city.qoi`
- Create (Game): `City Generator (r3d)/docs/rendering/legacy-baseline-report.md`

**Interfaces:**
- Consumes: `MainLaunchMode`, existing Growth 5 deterministic setup, each launch loop, `TakeScreenshot`, and `SceneRenderer.currentFramePerformanceDiagnostics`.
- Produces: `RenderBaselineConfiguration`, `RenderBaselineCaptureController`, `RenderBaselineMetrics`, five approved 1920×1080 goldens, cue-region JSON, exact image hashes, median/p95/worst metrics, and a comparator exit status.

- [ ] **Step 1: Write failing capture-state and metric tests**

```swift
import XCTest
import RenderBaselineSupport

final class RenderBaselineCaptureTests: XCTestCase {
    func testConfigurationRequiresAnApprovedModeAndOutputDirectory() throws {
        let value = try RenderBaselineConfiguration.parse(environment: [
            "BTBH_RENDER_BASELINE_MODE": "unified-city",
            "BTBH_RENDER_BASELINE_OUTPUT": "/tmp/btbh-render-baseline",
            "BTBH_RENDER_BASELINE_SEED": "424242"
        ])
        XCTAssertEqual(value?.mode, .unifiedCity)
        XCTAssertEqual(value?.seed, 424_242)
        XCTAssertEqual(value?.resolution, .init(width: 1920, height: 1080))
        XCTAssertThrowsError(try RenderBaselineConfiguration.parse(environment: [
            "BTBH_RENDER_BASELINE_MODE": "unknown",
            "BTBH_RENDER_BASELINE_OUTPUT": "/tmp/out"
        ]))
    }

    func testControllerCapturesAfterWarmupAndExactlyOneHundredTwentySamples() throws {
        let configuration = try XCTUnwrap(RenderBaselineConfiguration.parse(environment: [
            "BTBH_RENDER_BASELINE_MODE": "particle-editor",
            "BTBH_RENDER_BASELINE_OUTPUT": "/tmp/out"
        ]))
        var controller = RenderBaselineCaptureController(configuration: configuration)
        for _ in 0..<30 { XCTAssertEqual(controller.recordPresentedFrame(milliseconds: 10), .continue) }
        XCTAssertEqual(controller.recordPresentedFrame(milliseconds: 11), .capture)
        for _ in 0..<118 { XCTAssertEqual(controller.recordPresentedFrame(milliseconds: 12), .continue) }
        XCTAssertEqual(controller.recordPresentedFrame(milliseconds: 13), .finish)
        XCTAssertEqual(controller.metrics.sampleCount, 120)
    }

    func testMetricsUseMidpointMedianAndNearestRankP95() {
        let metrics = RenderBaselineMetrics(milliseconds: [1, 3, 5, 7, 9, 11, 13, 15, 17, 19])
        XCTAssertEqual(metrics.medianMilliseconds, 10, accuracy: 0.001)
        XCTAssertEqual(metrics.p95Milliseconds, 19, accuracy: 0.001)
        XCTAssertEqual(metrics.worstMilliseconds, 19, accuracy: 0.001)
    }

    func testQOIRoundTripPreservesExactRGBABytes() throws {
        let image = RGBAImage(
            width: 2,
            height: 2,
            bytes: [255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 255, 10, 20, 30, 40]
        )
        XCTAssertEqual(try QOI.decode(try QOI.encode(image)), image)
    }
}
```

- [ ] **Step 2: Verify the baseline types are absent**

Run:

```bash
swift test --filter RenderBaselineCaptureTests
```

Expected: compilation fails on the baseline types.

- [ ] **Step 3: Implement the deterministic state machine**

Use these exact public contracts:

```swift
public enum RenderBaselineMode: String, Codable, CaseIterable {
    case game, combatLab = "combat-lab", particleEditor = "particle-editor"
    case cityBlockEditor = "city-block-editor", unifiedCity = "unified-city"
}

public struct PixelSize: Codable, Equatable {
    public let width: Int; public let height: Int
    public init(width: Int, height: Int) { self.width = width; self.height = height }
}

public struct RenderCueRegion: Codable, Equatable {
    public let name: String
    public let bounds: PixelRect
    public let minimumLumaCoverage: Double
}

public struct RGBAImage: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public let bytes: [UInt8]
    public init(width: Int, height: Int, bytes: [UInt8]) {
        self.width = width; self.height = height; self.bytes = bytes
    }
}

public enum QOI {
    public static func encode(_ image: RGBAImage) throws -> Data
    public static func decode(_ data: Data) throws -> RGBAImage
}

public struct PixelRect: Codable, Equatable {
    public let x: Int; public let y: Int; public let width: Int; public let height: Int
}

public struct RenderBaselineConfiguration: Equatable {
    public let mode: RenderBaselineMode
    public let outputDirectory: URL
    public let seed: UInt64
    public let resolution: PixelSize
    public let warmupFrames: Int
    public let measuredFrames: Int
    public var outputPNGPath: String { outputDirectory.appendingPathComponent("\(mode.rawValue).png").path }
    public var outputQOIURL: URL { outputDirectory.appendingPathComponent("\(mode.rawValue).qoi") }
    public var outputCueURL: URL { outputDirectory.appendingPathComponent("\(mode.rawValue)-cues.json") }
    public var outputMetricsURL: URL { outputDirectory.appendingPathComponent("\(mode.rawValue)-metrics.json") }
    public func writeQOI(_ image: RGBAImage) throws { try QOI.encode(image).write(to: outputQOIURL, options: .atomic) }
    public static func parse(environment: [String: String]) throws -> Self?
}

public struct RenderCueRecorder {
    public private(set) var regions: [RenderCueRegion]
    public mutating func record(name: String, bounds: PixelRect, minimumLumaCoverage: Double)
    public func writeManifestFragment(to url: URL) throws
}

public enum RenderBaselineAction: Equatable { case `continue`, capture, finish }

public struct RenderBaselineMetrics: Codable, Equatable {
    public let milliseconds: [Double]
    public init(milliseconds: [Double])
    public var sampleCount: Int { milliseconds.count }
    public var medianMilliseconds: Double { get }
    public var p95Milliseconds: Double { get }
    public var worstMilliseconds: Double { get }
}

public struct RenderBaselineCaptureController {
    public private(set) var metrics: RenderBaselineMetrics
    public init(configuration: RenderBaselineConfiguration)
    public mutating func recordPresentedFrame(milliseconds: Double) -> RenderBaselineAction
    public func writeMetricsAtomically(to url: URL) throws
}
```

Every public baseline value type gets an explicit `public init` listing its stored properties in declaration order; no initializer relies on Swift's internal memberwise initializer. `RenderBaselineMetrics` preconditions that every sample is finite and nonnegative, exposes `sampleCount`, midpoint median, nearest-rank p95 using `ceil(0.95 * count) - 1`, and worst; all three aggregate values are `0` for an empty accumulator.

Defaults are seed `424242`, resolution `1920×1080`, 30 warm-up frames, one capture on frame 31, and 120 measured frames. Invalid mode, seed, resolution, or non-writable output path fails closed and prints `BTBH_RENDER_BASELINE_FAILED`.

Implement QOI exactly per the public QOI v1 byte format: magic `qoif`, big-endian width/height, four channels, sRGB colorspace byte, 64-entry RGBA hash index `(r*3 + g*5 + b*7 + a*11) % 64`, and the required eight-byte end marker. Reject zero dimensions, byte-count mismatch, truncated opcodes, integer overflow, and trailing pixels. This keeps the comparator cross-platform without ImageIO, WIC, Python, or a PNG decoder.

- [ ] **Step 4: Integrate the controller into every launch loop**

Add one optional controller at launch initialization. Game and combat lab reuse the existing Growth 5 coordinator with seed `424242`; capture starts only after `BTBH_PROFILE_READY`. Editors use their fixed seeds and freeze camera/input when baseline mode is active. Particle baseline sets a dedicated deterministic particle seed before creating the default system.

Immediately before each `EndDrawing()`, handle actions exactly as follows:

```swift
switch baselineController?.recordPresentedFrame(milliseconds: Double(dt) * 1_000) {
case .capture:
    do {
        TakeScreenshot(baselineConfiguration.outputPNGPath)
        try baselineConfiguration.writeQOI(readCurrentFrameRGBA())
        try baselineCueRecorder.writeManifestFragment(to: baselineConfiguration.outputCueURL)
        print("BTBH_RENDER_BASELINE_CAPTURED mode=\(baselineConfiguration.mode.rawValue)")
        fflush(stdout)
    } catch {
        print("BTBH_RENDER_BASELINE_FAILED mode=\(baselineConfiguration.mode.rawValue) error=\(error)")
        fflush(stdout)
        shouldTerminateBaseline = true
    }
case .finish:
    do {
        try baselineController?.writeMetricsAtomically(to: baselineConfiguration.outputMetricsURL)
        print("BTBH_RENDER_BASELINE_FINISHED mode=\(baselineConfiguration.mode.rawValue)")
        fflush(stdout)
        shouldTerminateBaseline = true
    } catch {
        print("BTBH_RENDER_BASELINE_FAILED mode=\(baselineConfiguration.mode.rawValue) error=\(error)")
        fflush(stdout)
        shouldTerminateBaseline = true
    }
case .continue, .none:
    break
}
```

For this legacy-only capture, implement `readCurrentFrameRGBA()` beside the launch loop with `LoadImageFromScreen()`, convert a local copy to `PIXELFORMAT_UNCOMPRESSED_R8G8B8A8`, copy exactly `width * height * 4` bytes into `RGBAImage`, and always call `UnloadImage`. Reject any size other than the configured size. Task 9 replaces this helper with `Canvas2DBackend.readbackPresentedFrame()`; it must not survive the final dependency-boundary scan.

Record explicit cue regions for the player/world viewport, critical HUD strip, card or tool panel, and mode-specific primary content. Regions are written from actual layout/projected coordinates; no hand-guessed pixel positions enter source.

- [ ] **Step 5: Implement the exact-PID runner and SSIM comparator**

Add this package target:

```swift
.executableTarget(
    name: "RenderBaselineComparator",
    dependencies: ["RenderBaselineSupport"],
    path: "Sources/RenderBaselineComparator"
),
```

`capture-render-baselines.sh` must:

1. build `swift build -c release`;
2. launch each of the five modes with its baseline environment;
3. store the exact child PID;
4. wait for `BTBH_RENDER_BASELINE_FINISHED` with a 180-second timeout;
5. terminate and `wait` only that PID on every exit path;
6. reject images not exactly 1920×1080;
7. run `swift run RenderBaselineComparator` against the approved QOI files and manifest;
8. write hardware, OS, commit, mode, seed, median, p95, worst, entity counts, and image SHA-256.

The comparator decodes QOI through `RenderBaselineSupport` and converts each sRGB component `c` to linear with `c/12.92` when `c <= 0.04045`, otherwise `((c + 0.055)/1.055)^2.4`; luminance is `0.2126R + 0.7152G + 0.0722B`. Compute SSIM in 8×8 windows with stride 4, sample variance/covariance denominator `n - 1`, `C1 = 0.01^2`, `C2 = 0.03^2`, and `(2*muX*muY+C1)*(2*covXY+C2) / ((muX^2+muY^2+C1)*(varX+varY+C2))`; the image score is the arithmetic mean of all windows. Repeat on windows clipped to every cue region. Luma coverage is the fraction of region pixels whose linear luminance is at least `0.02`. Exit 0 requires global SSIM ≥0.98, per-region SSIM ≥0.98, and every cue region's observed luma coverage at or above its manifest minimum. Exit 1 reports each failed mode/region. PNG files exist only for human review; QOI is the machine-comparison source.

- [ ] **Step 6: Capture, manually approve, and commit the legacy goldens**

Run on the M1 reference machine:

```bash
./tools/capture-render-baselines.sh --approve Tests/RenderGoldens/legacy
```

Run the same command on the Windows GTX 1060/RX 580-class reference machine without `--approve`; store the metrics in the report and compare against the approved images.

Manually inspect all five first goldens before staging. The report must identify the exact Windows CPU and freeze it as the reference rig. Phase 0 is not complete until both hardware rows exist.

- [ ] **Step 7: Run tests and commit the baseline harness and approved evidence**

```bash
swift test --filter RenderBaselineCaptureTests
swift test --filter MainLaunchModeTests
git add 'City Generator (r3d)/Package.swift' 'City Generator (r3d)/Sources/RenderBaselineSupport' 'City Generator (r3d)/Sources/RenderBaselineComparator' 'City Generator (r3d)/Sources/Main' 'City Generator (r3d)/Tests/MainTests/RenderBaselineCaptureTests.swift' 'City Generator (r3d)/Tests/RenderGoldens/legacy' 'City Generator (r3d)/tools/capture-render-baselines.sh' 'City Generator (r3d)/docs/rendering/legacy-baseline-report.md'
git commit -m "test: capture legacy renderer baselines"
```

### Task 5: Define The Project Platform, Canvas, Resource, And Scene Interfaces

**Files:**
- Create (Renderer): `Sources/BTBHRenderer/Platform.swift`
- Create (Renderer): `Sources/BTBHRenderer/Canvas2D.swift`
- Create (Renderer): `Sources/BTBHRenderer/RenderResources.swift`
- Create (Renderer): `Sources/BTBHRenderer/SceneSubmission.swift`
- Create (Renderer): `Sources/BTBHRenderer/RenderServices.swift`
- Create (Renderer): `Tests/BTBHRendererTests/RenderServicesTests.swift`
- Create (Renderer): `Tests/BTBHRendererTests/RecordingRenderBundle.swift`
- Create (Renderer): `Tests/BTBHRendererTests/PublicDependencyBoundaryTests.swift`

**Interfaces:**
- Consumes: render math and handle types from Task 2.
- Produces: the complete Phase 1 project seam used by the game and implemented by the legacy adapter in Task 6.

For every public struct declared in Steps 3–5 whose initializer is not printed explicitly, implement a `public init` with one parameter per stored property in declaration order. Defaults exist only where this plan prints them. Never rely on Swift's internal memberwise initializer across the package boundary.

- [ ] **Step 1: Write failing recording-backend lifecycle tests**

```swift
import XCTest
@testable import BTBHRenderer

@MainActor
final class RenderServicesTests: XCTestCase {
    func testInstalledBundleReceivesOneOrderedFrame() throws {
        let recording = RecordingRenderBundle()
        try RenderServices.install(recording)
        defer { RenderServices.uninstall() }

        try RenderServices.platform.openWindow(.init(title: "test", width: 1280, height: 720, mode: .windowed))
        RenderServices.platform.pollEvents()
        RenderServices.scene.beginFrame(.init(
            camera: RenderCamera(
                position: .init(x: 0, y: 10, z: 10), target: .zero, up: .up,
                verticalFOVDegrees: 45, projection: .perspective
            ),
            environment: .default
        ))
        RenderServices.scene.submit(.model(.init(model: ModelHandle(rawValue: 7), transform: .identity)))
        try RenderServices.scene.endFrame()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.fillRect(.init(rect: .init(x: 0, y: 0, width: 10, height: 10), color: .white)))
        RenderServices.canvas.endFrame()

        XCTAssertEqual(recording.events, [.openWindow, .pollEvents, .sceneBegin, .model, .sceneEnd, .canvasBegin, .fillRect, .canvasEnd])
    }

    func testDoubleInstallAndUseAfterUninstallFailClosed() throws {
        let recording = RecordingRenderBundle()
        try RenderServices.install(recording)
        XCTAssertThrowsError(try RenderServices.install(recording))
        RenderServices.uninstall()
        XCTAssertThrowsError(try RenderServices.requireInstalled())
    }
}
```

`RecordingRenderBundle.swift` defines test-local `RecordingRenderEvent` cases `openWindow`, `pollEvents`, `sceneBegin`, `model`, `sceneEnd`, `canvasBegin`, `fillRect`, `canvasEnd`, and `shutdown`. Implement one `@MainActor final class RecordingRenderBundle` conforming to `RenderBackendBundle`, `GamePlatform`, `Canvas2DBackend`, `RenderResourceBackend`, and `SceneBackend`; its four bundle properties return `self`. The eight exercised methods append the matching event. Deterministic query defaults are zero/false/empty. Every unexercised mutating resource or readback method throws `RecordingRenderError.unexpectedCall(function:)`; void destroy methods record no event. This makes the fake exhaustive when a protocol gains a requirement and prevents an untested backend call from silently succeeding.

- [ ] **Step 2: Run the test and verify the seam is absent**

```bash
swift test --filter RenderServicesTests
```

Expected: compilation fails on the platform, scene, canvas, and services declarations.

Add `PublicDependencyBoundaryTests` that recursively scans only `Sources/BTBHRendererCore` and `Sources/BTBHRenderer`, strips comments/string literals, and fails on the case-insensitive tokens `raylib`, `r3d_`, `rlgl`, `sdl`, `opengl`, `metal`, and `vulkan`. The permitted explanatory word `backend` is not a vendor token. This test must stay green through every later phase.

- [ ] **Step 3: Add the exact platform contract**

`Platform.swift` must declare:

```swift
public struct WindowDescriptor: Equatable, Sendable {
    public enum Mode: Equatable, Sendable { case windowed; case fullscreen; case borderless }
    public let title: String; public let width: Int; public let height: Int; public let mode: Mode
}

public enum InputKey: UInt16, CaseIterable, Sendable {
    case up, down, left, right, enter, keypadEnter, escape, tab, space, backspace, pause
    case leftControl, rightControl, leftShift, rightShift, leftSuper, rightSuper
    case f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12
    case keypad0, keypad1, keypad2, keypad3, keypadAdd, keypadSubtract
    case one, two, three, four, five, six, seven, eight
    case a, b, c, d, e, g, h, i, j, k, m, n, o, p, q, r, s, u, v, w, x, y, z
    case apostrophe, semicolon, leftBracket, rightBracket, minus, equal, comma, period
}

public enum MouseButton: UInt8, Sendable { case left, right, middle }
public enum GamepadButton: UInt8, Sendable {
    case south, east, west, north, leftShoulder, rightShoulder
    case leftStick, rightStick, start, back, guide, dpadUp, dpadDown, dpadLeft, dpadRight
}
public enum NavigationDirection: UInt8, Sendable { case up, down, left, right }
public struct GamepadSnapshot: Equatable, Sendable {
    public let isConnected: Bool
    public let leftStick: RenderVec2
    public let rightStick: RenderVec2
    public let dpad: RenderVec2
    public let leftTrigger: Float
    public let rightTrigger: Float
    public let hasActiveInput: Bool
}

public struct PlatformFrame: Equatable, Sendable {
    public let deltaSeconds: Float; public let elapsedSeconds: Double
    public let drawableSize: PixelSize2D; public let logicalSize: PixelSize2D
}

public struct PixelSize2D: Equatable, Sendable { public let width: Int; public let height: Int }

public struct FileDialogRequest: Equatable, Sendable {
    public let title: String
    public let allowedExtensions: [String]
    public let allowsMultipleSelection: Bool
    public init(title: String, allowedExtensions: [String], allowsMultipleSelection: Bool = false) {
        self.title = title
        self.allowedExtensions = allowedExtensions
        self.allowsMultipleSelection = allowsMultipleSelection
    }
}

@MainActor public protocol GamePlatform: AnyObject {
    func openWindow(_ descriptor: WindowDescriptor) throws
    func closeWindow()
    func pollEvents()
    func beginFrame() -> PlatformFrame
    var shouldClose: Bool { get }
    var isWindowFocused: Bool { get }
    var refreshRate: Int { get }
    var framesPerSecond: Int { get }
    var mousePosition: RenderVec2 { get }
    var mouseDelta: RenderVec2 { get }
    var mouseWheelDelta: Float { get }
    func isKeyDown(_ key: InputKey) -> Bool
    func wasKeyPressed(_ key: InputKey) -> Bool
    func isMouseButtonDown(_ button: MouseButton) -> Bool
    func wasMouseButtonPressed(_ button: MouseButton) -> Bool
    func wasMouseButtonReleased(_ button: MouseButton) -> Bool
    var gamepad: GamepadSnapshot { get }
    func isGamepadButtonDown(_ button: GamepadButton) -> Bool
    func wasGamepadButtonPressed(_ button: GamepadButton) -> Bool
    func wasGamepadTriggerPressed(left: Bool, threshold: Float) -> Bool
    func wasNavigationPressed(_ direction: NavigationDirection) -> Bool
    func setCursorVisible(_ visible: Bool)
    var isCursorVisible: Bool { get }
    func setClipboardText(_ text: String) throws
    func showOpenFileDialog(_ request: FileDialogRequest, completion: @escaping @Sendable ([URL]) -> Void)
}
```

- [ ] **Step 4: Add the exact Canvas2D contract**

`Canvas2D.swift` defines these exact project-only commands:

```swift
public enum TextAnchor: UInt8, Sendable { case topLeft, top, center, bottomLeft }
public enum CanvasBlendMode: UInt8, Sendable { case alpha, additive, multiplied, premultipliedAlpha }
public enum TextureFilter: UInt8, Sendable { case nearest, bilinear, trilinear }
public enum TextureWrap: UInt8, Sendable { case repeatMode, clamp, mirrorRepeat, mirrorClamp }

public struct FillRectCommand: Sendable { public let rect: RenderRect; public let color: RenderColor }
public struct StrokeRectCommand: Sendable { public let rect: RenderRect; public let thickness: Float; public let color: RenderColor }
public struct RoundedRectCommand: Sendable { public let rect: RenderRect; public let roundness: Float; public let segments: Int; public let color: RenderColor }
public struct HorizontalGradientCommand: Sendable { public let rect: RenderRect; public let left: RenderColor; public let right: RenderColor }
public struct Line2DCommand: Sendable { public let start: RenderVec2; public let end: RenderVec2; public let thickness: Float; public let color: RenderColor }
public struct Triangle2DCommand: Sendable { public let a: RenderVec2; public let b: RenderVec2; public let c: RenderVec2; public let color: RenderColor; public let outline: Bool }
public struct Circle2DCommand: Sendable { public let center: RenderVec2; public let radius: Float; public let color: RenderColor; public let outline: Bool }
public struct Ring2DCommand: Sendable { public let center: RenderVec2; public let innerRadius: Float; public let outerRadius: Float; public let startDegrees: Float; public let endDegrees: Float; public let segments: Int; public let color: RenderColor }
public struct TexturedQuadCommand: Sendable { public let texture: TextureHandle; public let source: RenderRect; public let destination: RenderRect; public let origin: RenderVec2; public let rotationDegrees: Float; public let tint: RenderColor }
public struct TextCommand: Sendable { public let text: String; public let font: FontHandle; public let position: RenderVec2; public let size: Float; public let spacing: Float; public let color: RenderColor; public let anchor: TextAnchor }

public enum CanvasCommand: Sendable {
    case clear(RenderColor)
    case fillRect(FillRectCommand)
    case strokeRect(StrokeRectCommand)
    case roundedRect(RoundedRectCommand)
    case horizontalGradient(HorizontalGradientCommand)
    case line(Line2DCommand)
    case triangle(Triangle2DCommand)
    case circle(Circle2DCommand)
    case ring(Ring2DCommand)
    case texturedQuad(TexturedQuadCommand)
    case text(TextCommand)
    case pushScissor(RenderRect)
    case popScissor
    case setBlendMode(CanvasBlendMode)
    case pushRenderTarget(RenderTargetHandle)
    case popRenderTarget
}
```

Every command uses project values and handles. The interface is:

```swift
@MainActor public protocol Canvas2DBackend: AnyObject {
    func beginFrame()
    func submit(_ command: CanvasCommand)
    func measureText(_ text: String, font: FontHandle, size: Float, spacing: Float) -> RenderVec2
    func endFrame()
    func readbackPresentedFrame() throws -> FrameReadback
    func writePresentedFramePNG(to path: String) throws
}
```

- [ ] **Step 5: Add resource and scene contracts**

`RenderResources.swift` owns load/create/update/destroy methods for every handle declared in Task 2. Paths are UTF-8 strings, data uploads use `UnsafeRawBufferPointer` only for the duration of a call, and returned raw C objects are impossible. Define these values:

```swift
public enum PixelFormat: UInt8, Sendable { case r8, rg8, rgba8, rgba8SRGB, rgba16Float, depth32Float }
public struct TextureUsage: OptionSet, Sendable {
    public let rawValue: UInt8
    public init(rawValue: UInt8) { self.rawValue = rawValue }
    public static let sampled = Self(rawValue: 1 << 0)
    public static let renderTarget = Self(rawValue: 1 << 1)
    public static let storage = Self(rawValue: 1 << 2)
}
public struct TextureDescriptor: Sendable { public let size: PixelSize2D; public let mipCount: Int; public let format: PixelFormat; public let usage: TextureUsage; public let filter: TextureFilter; public let wrap: TextureWrap }
public struct FontDescriptor: Sendable { public let path: String; public let baseSize: Int; public let glyphs: [UInt32] }
public struct RenderTargetDescriptor: Sendable { public let size: PixelSize2D; public let colorFormat: PixelFormat; public let hasDepth: Bool }

public enum PrimitiveTopology: UInt8, Sendable { case triangles, lines, points }
public enum MeshStorage: UInt8, Sendable { case immutable, streamed }
public struct MeshDescriptor: Sendable { public let topology: PrimitiveTopology; public let storage: MeshStorage; public let vertexCount: Int; public let indexCount: Int; public let bounds: RenderBounds3 }
public struct ModelImportFlags: OptionSet, Sendable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }
    public static let meshData = Self(rawValue: 1 << 0)
    public static let highQuality = Self(rawValue: 1 << 1)
}

public enum CompareFunction: UInt8, Hashable, Sendable { case always, lessEqual, greater, notEqual }
public enum CullMode: UInt8, Hashable, Sendable { case none, front, back }
public enum TransparencyMode: UInt8, Hashable, Sendable { case disabled, alpha }
public enum BillboardMode: UInt8, Hashable, Sendable { case disabled, front, yAxis }
public enum MaterialBlendMode: UInt8, Hashable, Sendable { case mix, additive, multiplied, premultipliedAlpha }
public struct MaterialDescriptor: Hashable, Sendable {
    public let albedoTexture: TextureHandle; public let albedoColor: RenderColor
    public let emissionTexture: TextureHandle; public let emissionColor: RenderColor; public let emissionEnergy: Float
    public let normalTexture: TextureHandle; public let normalScale: Float
    public let ormTexture: TextureHandle; public let occlusion: Float; public let roughness: Float; public let metalness: Float
    public let uvOffset: RenderVec2; public let uvScale: RenderVec2; public let alphaCutoff: Float
    public let depthCompare: CompareFunction; public let cullMode: CullMode
    public let transparency: TransparencyMode; public let billboard: BillboardMode; public let blend: MaterialBlendMode
    public let unlit: Bool; public let surfaceShader: SurfaceShaderHandle
}

public struct InstanceBufferDescriptor: Sendable { public let capacity: Int; public let stride: Int; public let includesColor: Bool; public let customFloatCount: Int }
public struct AnimationPlayerDescriptor: Sendable { public let model: ModelHandle; public let library: AnimationLibraryHandle; public let initialAnimation: String }
public struct AnimationTreeDescriptor: Sendable { public let model: ModelHandle; public let library: AnimationLibraryHandle; public let rootAnimation: String }

public enum LightKind: UInt8, Sendable { case directional, spot }
public struct LightDescriptor: Sendable { public let kind: LightKind; public let active: Bool; public let position: RenderVec3; public let direction: RenderVec3; public let color: RenderColor; public let energy: Float; public let range: Float; public let castsShadow: Bool; public let shadowSoftness: Float }
public enum TonemapMode: UInt8, Sendable { case reinhard }
public struct EnvironmentDescriptor: Sendable {
    public let backgroundColor: RenderColor; public let backgroundEnergy: Float
    public let ambientColor: RenderColor; public let ambientEnergy: Float
    public let ambientMap: AmbientMapHandle; public let skyCubemap: CubemapHandle
    public let ssaoEnabled: Bool; public let tonemap: TonemapMode; public let tonemapWhite: Float
    public static let `default` = Self(
        backgroundColor: .black, backgroundEnergy: 0,
        ambientColor: .white, ambientEnergy: 0,
        ambientMap: .invalid, skyCubemap: .invalid,
        ssaoEnabled: false, tonemap: .reinhard, tonemapWhite: 1
    )
}

public enum ShaderParameterValue: Sendable { case float(Float), vec2(RenderVec2), vec3(RenderVec3), vec4(RenderVec4), texture(TextureHandle) }
public struct SurfaceShaderDescriptor: Sendable { public let vertexPath: String?; public let fragmentPath: String; public let parameters: [String: ShaderParameterValue] }
public struct ScreenShaderDescriptor: Sendable { public let fragmentPath: String; public let parameters: [String: ShaderParameterValue] }
public struct ProceduralSkyDescriptor: Sendable { public let color: RenderColor; public let resolution: Int }

public enum AntiAliasing: UInt8, Sendable { case off, smaaMedium, smaaHigh }
public enum UpscaleMode: UInt8, Sendable { case linear, lanczos }
public struct PostChainDescriptor: Sendable {
    public let screenShaders: [ScreenShaderHandle]
    public let casSharpness: Float
    public let tiltShiftEnabled: Bool
    public static let empty = Self(screenShaders: [], casSharpness: 0, tiltShiftEnabled: false)
}
```

Required protocols:

```swift
@MainActor public protocol RenderResourceBackend: AnyObject {
    func loadTexture(path: String) throws -> TextureHandle
    func createTexture(_ descriptor: TextureDescriptor, bytes: UnsafeRawBufferPointer?) throws -> TextureHandle
    func updateTexture(_ handle: TextureHandle, bytes: UnsafeRawBufferPointer) throws
    func destroyTexture(_ handle: TextureHandle)
    func loadFont(_ descriptor: FontDescriptor) throws -> FontHandle
    func destroyFont(_ handle: FontHandle)
    func createRenderTarget(_ descriptor: RenderTargetDescriptor) throws -> RenderTargetHandle
    func colorTexture(of target: RenderTargetHandle) throws -> TextureHandle
    func destroyRenderTarget(_ handle: RenderTargetHandle)
    func loadModel(path: String, flags: ModelImportFlags) throws -> ModelHandle
    func destroyModel(_ handle: ModelHandle)
    func createMesh(_ descriptor: MeshDescriptor, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws -> MeshHandle
    func updateMesh(_ handle: MeshHandle, vertices: UnsafeRawBufferPointer, indices: UnsafeRawBufferPointer?) throws
    func destroyMesh(_ handle: MeshHandle)
    func createMaterial(_ descriptor: MaterialDescriptor) throws -> MaterialHandle
    func updateMaterial(_ handle: MaterialHandle, descriptor: MaterialDescriptor) throws
    func destroyMaterial(_ handle: MaterialHandle)
    func createInstanceBuffer(_ descriptor: InstanceBufferDescriptor) throws -> InstanceBufferHandle
    func uploadInstances(_ handle: InstanceBufferHandle, instances: UnsafeRawBufferPointer) throws
    func destroyInstanceBuffer(_ handle: InstanceBufferHandle)
    func loadAnimationLibrary(path: String) throws -> AnimationLibraryHandle
    func destroyAnimationLibrary(_ handle: AnimationLibraryHandle)
    func createAnimationPlayer(_ descriptor: AnimationPlayerDescriptor) throws -> AnimationPlayerHandle
    func updateAnimationPlayer(_ handle: AnimationPlayerHandle, deltaSeconds: Float) throws
    func setAnimation(_ handle: AnimationPlayerHandle, name: String, loop: Bool, time: Float?) throws
    func destroyAnimationPlayer(_ handle: AnimationPlayerHandle)
    func createAnimationTree(_ descriptor: AnimationTreeDescriptor) throws -> AnimationTreeHandle
    func updateAnimationTree(_ handle: AnimationTreeHandle, deltaSeconds: Float) throws
    func setAnimationTreeParameter(_ handle: AnimationTreeHandle, name: String, value: Float) throws
    func destroyAnimationTree(_ handle: AnimationTreeHandle)
    func createLight(_ descriptor: LightDescriptor) throws -> LightHandle
    func updateLight(_ handle: LightHandle, descriptor: LightDescriptor) throws
    func destroyLight(_ handle: LightHandle)
    func createProceduralSky(_ descriptor: ProceduralSkyDescriptor) throws -> CubemapHandle
    func createAmbientMap(from cubemap: CubemapHandle) throws -> AmbientMapHandle
    func destroyCubemap(_ handle: CubemapHandle)
    func destroyAmbientMap(_ handle: AmbientMapHandle)
    func createSurfaceShader(_ descriptor: SurfaceShaderDescriptor) throws -> SurfaceShaderHandle
    func updateSurfaceShader(_ handle: SurfaceShaderHandle, parameter: String, value: ShaderParameterValue) throws
    func destroySurfaceShader(_ handle: SurfaceShaderHandle)
    func createScreenShader(_ descriptor: ScreenShaderDescriptor) throws -> ScreenShaderHandle
    func updateScreenShader(_ handle: ScreenShaderHandle, parameter: String, value: ShaderParameterValue) throws
    func destroyScreenShader(_ handle: ScreenShaderHandle)
}
```

`SceneSubmission.swift` declares immutable frame/draw values:

```swift
public struct RenderTransform: Hashable, Sendable {
    public let matrix: RenderMatrix4
    public init(matrix: RenderMatrix4) { self.matrix = matrix }
    public static let identity = Self(matrix: .identity)
}
public struct MaterialOverride: Hashable, Sendable { public let slot: Int; public let material: MaterialHandle }
public struct RenderInstance: Hashable, Sendable { public let transform: RenderTransform; public let color: RenderColor; public let custom: RenderVec4 }
public struct ModelDraw: Sendable {
    public let model: ModelHandle; public let transform: RenderTransform; public let overrides: [MaterialOverride]
    public init(model: ModelHandle, transform: RenderTransform, overrides: [MaterialOverride] = []) {
        self.model = model; self.transform = transform; self.overrides = overrides
    }
}
public struct ModelInstancesDraw: Sendable { public let model: ModelHandle; public let instances: [RenderInstance]; public let overrides: [MaterialOverride]; public let persistentBuffer: InstanceBufferHandle? }
public struct MeshDraw: Sendable { public let mesh: MeshHandle; public let material: MaterialHandle; public let transform: RenderTransform }
public struct MeshInstancesDraw: Sendable { public let mesh: MeshHandle; public let material: MaterialHandle; public let instances: [RenderInstance]; public let persistentBuffer: InstanceBufferHandle? }
public struct AnimatedModelDraw: Sendable { public let model: ModelHandle; public let player: AnimationPlayerHandle?; public let tree: AnimationTreeHandle?; public let transform: RenderTransform; public let overrides: [MaterialOverride] }
public struct AnimatedModelInstancesDraw: Sendable { public let model: ModelHandle; public let player: AnimationPlayerHandle?; public let tree: AnimationTreeHandle?; public let instances: [RenderInstance]; public let overrides: [MaterialOverride] }

public enum DebugDraw: Sendable {
    case line(start: RenderVec3, end: RenderVec3, color: RenderColor)
    case grid(slices: Int, spacing: Float)
    case plane(center: RenderVec3, size: RenderVec2, color: RenderColor)
    case sphere(center: RenderVec3, radius: Float, color: RenderColor)
    case bounds(RenderBounds3, color: RenderColor)
    case wireCube(center: RenderVec3, size: RenderVec3, color: RenderColor)
}

public enum SceneDraw: Sendable {
    case model(ModelDraw)
    case modelInstances(ModelInstancesDraw)
    case mesh(MeshDraw)
    case meshInstances(MeshInstancesDraw)
    case animatedModel(AnimatedModelDraw)
    case animatedModelInstances(AnimatedModelInstancesDraw)
    case debug(DebugDraw)
}

public struct FrameDescriptor: Sendable {
    public let camera: RenderCamera
    public let environment: EnvironmentDescriptor
    public let renderScale: Float
    public let antiAliasing: AntiAliasing
    public let upscaleMode: UpscaleMode
    public let postChain: PostChainDescriptor
    public init(
        camera: RenderCamera,
        environment: EnvironmentDescriptor,
        renderScale: Float = 1,
        antiAliasing: AntiAliasing = .smaaMedium,
        upscaleMode: UpscaleMode = .linear,
        postChain: PostChainDescriptor = .empty
    ) {
        self.camera = camera; self.environment = environment; self.renderScale = renderScale
        self.antiAliasing = antiAliasing; self.upscaleMode = upscaleMode; self.postChain = postChain
    }
}

public struct FrameReadback: Sendable { public let size: PixelSize2D; public let rgba8: [UInt8] }
public struct RenderRay: Hashable, Sendable { public let origin: RenderVec3; public let direction: RenderVec3 }
```

Then declare `SceneBackend`:

```swift
@MainActor public protocol SceneBackend: AnyObject {
    func initialize(drawableSize: PixelSize2D) throws
    func resize(drawableSize: PixelSize2D) throws
    func beginFrame(_ descriptor: FrameDescriptor)
    func submit(_ draw: SceneDraw)
    func endFrame() throws
    func project(_ world: RenderVec3, camera: RenderCamera) -> RenderVec2
    func unprojectRay(_ screen: RenderVec2, camera: RenderCamera) -> RenderRay
    func intersectsFrustum(_ bounds: RenderBounds3) -> Bool
    func readbackWorldFrame() throws -> FrameReadback
    func captureWorldFrameTexture() throws -> TextureHandle
    func shutdown()
}
```

Place `RenderRay` in `RenderMath.swift`. The defaulted `FrameDescriptor` initializer shown above is the initializer used by the recording test.

- [ ] **Step 6: Implement install/uninstall with one coherent bundle**

`RenderServices.swift` defines:

```swift
@MainActor public protocol RenderBackendBundle: AnyObject {
    var platform: any GamePlatform { get }
    var canvas: any Canvas2DBackend { get }
    var resources: any RenderResourceBackend { get }
    var scene: any SceneBackend { get }
    func shutdown()
}

@MainActor public enum RenderServices {
    public static func install(_ bundle: any RenderBackendBundle) throws
    public static func uninstall()
    public static func requireInstalled() throws
    public static var platform: any GamePlatform { get }
    public static var canvas: any Canvas2DBackend { get }
    public static var resources: any RenderResourceBackend { get }
    public static var scene: any SceneBackend { get }
    public static var access: RenderServicesAccess { get }
}

@MainActor public struct RenderServicesAccess {
    public let platform: any GamePlatform
    public let canvas: any Canvas2DBackend
    public let resources: any RenderResourceBackend
    public let scene: any SceneBackend
}
```

Controlled startup calls `requireInstalled()` and propagates `RenderServicesError.notInstalled`. The nonthrowing convenience properties are legal only after successful bootstrap and call `preconditionFailure("RenderServices used before install")` in every build configuration when violated; there is no debug/release semantic split. Double install throws `RenderServicesError.alreadyInstalled`.

- [ ] **Step 7: Run package tests, commit, push, and update the game submodule pin**

Renderer:

```bash
swift test
git add Sources/BTBHRenderer Tests/BTBHRendererTests
git commit -m "feat: define project renderer seams"
git push
```

Game:

```bash
git -C 'City Generator (r3d)/Vendor/BTBHRenderer' fetch origin
git -C 'City Generator (r3d)/Vendor/BTBHRenderer' fetch origin codex/renderer-phase-0-1
git -C 'City Generator (r3d)/Vendor/BTBHRenderer' checkout --detach origin/codex/renderer-phase-0-1
git add 'City Generator (r3d)/Vendor/BTBHRenderer'
git commit -m "build: update renderer seam package"
```

Verify that `git submodule status` prints the exact Task 5 commit before committing the game gitlink.

### Task 6: Build The Legacy Backend Bundle Behind The New Seams

**Files:**
- Create (Game): `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyConversions.swift`
- Create (Game): `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyPlatformBackend.swift`
- Create (Game): `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyCanvasBackend.swift`
- Create (Game): `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyResourceBackend.swift`
- Create (Game): `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacySceneBackend.swift`
- Create (Game): `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyRawSceneDriver.swift`
- Create (Game): `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyRenderServices.swift`
- Create (Game): `City Generator (r3d)/Sources/Main/Sources/Renderer/RenderBootstrap.swift`
- Create (Game): `City Generator (r3d)/Tests/MainTests/LegacyRenderAdapterTests.swift`

**Interfaces:**
- Consumes: all Task 5 protocols and existing raw raylib/r3d/SDL APIs.
- Produces: `LegacyRenderServices.make() -> RenderBackendBundle` and `RenderBootstrap.installLegacy() throws` without exposing raw values.

- [ ] **Step 1: Write failing conversion, stale-handle, and shutdown-order tests**

```swift
import XCTest
import BTBHRenderer
@testable import Main

@MainActor
final class LegacyRenderAdapterTests: XCTestCase {
    func testColorAndCameraRoundTripPreserveBits() {
        let color = RenderColor(r: 11, g: 22, b: 33, a: 44)
        XCTAssertEqual(RenderColor(legacy: color.legacy), color)

        let camera = RenderCamera(
            position: .init(x: 1, y: 2, z: 3), target: .zero, up: .up,
            verticalFOVDegrees: 45, projection: .perspective
        )
        XCTAssertEqual(RenderCamera(legacy: camera.legacy), camera)
    }

    func testDestroyedLegacyResourceRejectsTheStaleProjectHandle() throws {
        let resources = LegacyResourceBackend(validationEnabled: true)
        let handle = try resources.insertTestTexture(width: 4, height: 4)
        XCTAssertTrue(resources.contains(handle))
        resources.destroyTexture(handle)
        XCTAssertFalse(resources.contains(handle))
        XCTAssertThrowsError(try resources.requireTexture(handle))
    }

    func testBundleShutdownOrderIsSceneCanvasResourcesPlatform() throws {
        let log = LegacyShutdownLog()
        let bundle = LegacyRenderServices.makeForTesting(log: log)
        bundle.shutdown()
        XCTAssertEqual(log.events, [.scene, .canvas, .resources, .platform])
    }

    func testDestroyDuringOpenFrameInvalidatesHandleAndDefersRawRelease() throws {
        let log = LegacyResourceReleaseLog()
        let context = LegacyFrameLifetimeContext(resourceReleaseLog: log)
        let resources = LegacyResourceBackend(context: context, validationEnabled: true)
        let scene = LegacySceneBackend(
            context: context,
            resources: resources,
            driver: RecordingLegacyRawSceneDriver()
        )
        let model = try resources.insertTestModel()
        scene.beginFrame(.init(
            camera: RenderCamera(
                position: .init(x: 0, y: 10, z: 10), target: .zero, up: .up,
                verticalFOVDegrees: 45, projection: .perspective
            ),
            environment: .default
        ))
        scene.submit(.model(.init(model: model, transform: .identity)))

        resources.destroyModel(model)
        XCTAssertFalse(resources.contains(model))
        XCTAssertEqual(log.events, [])

        try scene.endFrame()
        XCTAssertEqual(log.events, [.model])
    }
}
```

- [ ] **Step 2: Run the focused test and verify adapters are absent**

```bash
swift test --filter LegacyRenderAdapterTests
```

Expected: compilation fails on the legacy adapter types and conversions.

- [ ] **Step 3: Implement conversions with no shared unsafe storage**

`LegacyConversions.swift` is the only place that converts `RenderVec2/3/4`, `RenderQuaternion`, `RenderMatrix4`, `RenderColor`, `RenderRect`, `RenderBounds3`, and `RenderCamera` to raw C structs. Every conversion copies fields. No `unsafeBitCast` is permitted because layout equality is not a public contract.

Required signatures:

```swift
extension RenderVec3 { init(legacy: Vector3); var legacy: Vector3 { get } }
extension RenderColor { init(legacy: Color); var legacy: Color { get } }
extension RenderMatrix4 { init(legacy: Matrix); var legacy: Matrix { get } }
extension RenderCamera { init(legacy: Camera3D); var legacy: Camera3D { get } }
```

- [ ] **Step 4: Implement platform and Canvas adapters using the mapping table**

`LegacyPlatformBackend` owns window initialization, monitor refresh, timing, keyboard, mouse, cursor, clipboard, SDL gamepad, and SDL file-dialog lifecycle. It calls `SDL_Init` once with `SDL_INIT_GAMEPAD`, and `SDL_Quit` only after the renderer/window has shut down.

`LegacyCanvasBackend.submit(_:)` exhaustively switches over every `CanvasCommand`. Scissor push/pop uses `rlEnableScissorTest`, `rlScissor`, and `rlDisableScissorTest`; blend mode uses `rlSetBlendMode`; `rlDrawRenderBatchActive` is internal to snapshot capture and never callable by game code.

- [ ] **Step 5: Implement typed legacy resource stores**

Use one `LegacyHandleStore<Value, Handle>` per resource kind. It owns a `HandlePool`, a dense `[Value?]` store indexed from the handle's low 32 bits minus one, and validates generation before access. Destroy order is child resources before parent models, then materials/textures, then shared shaders/sky. Invalid handles throw in validation builds and return the magenta checker/default material fallback in release.

The shared `LegacyFrameLifetimeContext` opens an epoch at `SceneBackend.beginFrame`. Scene translation retains every raw model, mesh, material, texture, animation, shader, and environment object referenced by a submitted command until that epoch closes. A public destroy invalidates and advances the project handle generation immediately; if its raw object is retained by the open epoch, enqueue its destructor. `SceneBackend.endFrame` calls `R3D_End` first, then drains queued destructors in dependency order. Shutdown drains an open epoch before global resource teardown. The focused test above must observe no raw model release before `endFrame` and exactly one after it.

No public method returns `R3D_Model`, `R3D_Mesh`, `R3D_Material`, `Texture2D`, `Font`, or another raw type.

- [ ] **Step 6: Implement scene translation and bundle ownership**

`LegacySceneBackend` converts immutable scene commands at submission time, queues existing R3D draws, and executes exactly one `R3D_Begin`/`R3D_End` pair per world frame. Keep a package-internal `LegacyRawSceneDriver` inside `Renderer/Legacy` with only initialization, begin/end, translated draw, resize, and shutdown operations; `R3DLegacyRawSceneDriver` is the production implementation and `RecordingLegacyRawSceneDriver` makes lifecycle tests deterministic without opening a GPU context. This is not a public renderer abstraction and may traffic in raw values because it cannot escape the adapter directory. `LegacyRenderServices` constructs platform, resource, canvas, and scene adapters over one shared lifetime context. `RenderBootstrap.installLegacy()` installs the bundle and returns startup errors instead of continuing partially initialized.

- [ ] **Step 7: Run tests and commit the isolated legacy implementation**

```bash
swift test --filter LegacyRenderAdapterTests
swift test --filter SceneRendererRuntimeSafetyTests
git add 'City Generator (r3d)/Sources/Main/Sources/Renderer' 'City Generator (r3d)/Tests/MainTests/LegacyRenderAdapterTests.swift'
git commit -m "feat: add legacy renderer backend bundle"
```

### Task 7: Replace Raylib Math And Data Types Outside The Adapter

**Files:**
- Modify (Renderer): `Sources/BTBHRenderer/RenderMath.swift`
- Modify (Renderer): `Tests/BTBHRendererTests/RenderMathTests.swift`
- Modify (Game): all files in Appendix A, batch `Math/Data`.
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/Math/Vector.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/RaylibColors.swift`
- Create (Game): `City Generator (r3d)/Tests/MainTests/RenderValueTypeTests.swift`

**Interfaces:**
- Consumes: `RenderVec2/3/4`, `RenderQuaternion`, `RenderMatrix4`, `RenderColor`, `RenderRect`, `RenderBounds3`, and `RenderCamera`.
- Produces: game/domain data that compiles without raylib math types; raw conversions remain adapter-private.

- [ ] **Step 1: Add failing project-math behavior tests**

Port the current vector helper expectations to project types:

```swift
import XCTest
import BTBHRenderer
@testable import Main

final class RenderValueTypeTests: XCTestCase {
    func testProjectVectorHelpersPreserveScreenSpaceDirections() {
        XCTAssertEqual(RenderVec2.up, RenderVec2(x: 0, y: -1))
        XCTAssertEqual(RenderVec2.right, RenderVec2(x: 1, y: 0))
        XCTAssertEqual(RenderVec2(x: 3, y: 4).normalized().length, 1, accuracy: 0.0001)
    }

    func testProjectColorsRetainExactRGBAValues() {
        XCTAssertEqual(GAME_AMBER, RenderColor(r: 246, g: 184, b: 72, a: 255))
    }
}
```

- [ ] **Step 2: Run tests and confirm project extensions/constants are absent**

```bash
swift test --filter RenderValueTypeTests
```

Expected: compilation fails until vector directions/helpers and color constants use project values.

- [ ] **Step 3: Move all math operations to project methods**

Use the exact substitutions from Appendix B. Remove the raylib conversion initializer and `asRaylib` property from `Math/Vector.swift`. Simulation `Vec2` remains a game value; add `var render: RenderVec2` and `init(_ value: RenderVec2)` instead. Remove all extensions on raw `Vector2` and `Vector3`.

Implement quaternion axis-angle, multiplication, vector rotation, matrix translation/rotation/scale/multiply, lerp, remap, and collision helpers in `BTBHRenderer/RenderMath.swift`; add a focused renderer package test for each before migrating the caller.

- [ ] **Step 4: Replace raw stored types in game models**

Convert every stored `Vector2`, `Vector3`, `Vector4`, `Quaternion`, `Matrix`, `Color`, `Rectangle`, `Camera3D`, `BoundingBox`, `Texture2D`, `RenderTexture2D`, `Font`, and `Image` outside `Renderer/Legacy` to the corresponding project value or opaque handle. A source file may not retain `import raylib` merely for a constant or constructor.

Resource values become handles; do not embed raw IDs in a new Swift wrapper.

- [ ] **Step 5: Commit and publish the expanded renderer math API**

From the renderer worktree:

```bash
swift test --filter RenderMathTests
git add Sources/BTBHRenderer/RenderMath.swift Tests/BTBHRendererTests/RenderMathTests.swift
git commit -m "feat: complete project render math operations"
git push
```

From the game worktree, pin that exact remote commit before compiling migrated callers:

```bash
git -C 'City Generator (r3d)/Vendor/BTBHRenderer' fetch origin codex/renderer-phase-0-1
git -C 'City Generator (r3d)/Vendor/BTBHRenderer' checkout --detach origin/codex/renderer-phase-0-1
git add 'City Generator (r3d)/Vendor/BTBHRenderer'
git diff --cached --submodule=log
```

The staged submodule log must contain the `feat: complete project render math operations` commit and no later renderer commit.

- [ ] **Step 6: Run the math/data batch and commit**

```bash
swift test --filter RenderValueTypeTests
swift test --filter ParticleEditorCoordinateTests
swift test --filter UnifiedCityGeneratorTests
git diff --check
git add 'City Generator (r3d)/Sources/Main/Sources/Classes' 'City Generator (r3d)/Tests/MainTests/RenderValueTypeTests.swift' 'City Generator (r3d)/Vendor/BTBHRenderer'
git commit -m "refactor: replace raylib render value types"
```

### Task 8: Route Window, Timing, Input, Gamepad, And Dialogs Through GamePlatform

**Files:**
- Modify (Game): `City Generator (r3d)/Sources/Main/main.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/Input/GamepadManager.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/Game/Game+GrowthTrial.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/ParticleSystem/ParticleDemo.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/ParticleSystem/ParticleFileDialogState.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditorRunner.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/UnifiedCityGenerator/UnifiedCityGeneratorRunner.swift`
- Create (Game): `City Generator (r3d)/Tests/MainTests/GamePlatformRoutingTests.swift`

**Interfaces:**
- Consumes: `RenderBootstrap.installLegacy()` and `RenderServices.platform`.
- Produces: all five launch modes using the platform seam; only `LegacyPlatformBackend.swift` imports SDL3 or calls raw raylib window/input functions.

- [ ] **Step 1: Write failing launch lifecycle tests with a recording platform**

Test that each `MainLaunchMode` maps to its exact `WindowDescriptor`, calls `openWindow` once, polls once per frame, and closes once even when startup fails after window creation. Test fullscreen game/combat title and fixed editor dimensions.

- [ ] **Step 2: Run the focused lifecycle tests**

```bash
swift test --filter GamePlatformRoutingTests
```

Expected: failure because launch functions call global raylib/SDL symbols.

- [ ] **Step 3: Extract launch functions that accept installed services**

Change entrypoints to:

```swift
@MainActor func runGame(labMode: Bool, services: RenderServicesAccess) throws
@MainActor func runParticleEditor(services: RenderServicesAccess) throws
@MainActor func runCityBlockEditor(services: RenderServicesAccess) throws
@MainActor func runUnifiedCity(services: RenderServicesAccess) throws
```

The top-level entry installs the legacy bundle once, dispatches the selected mode, then uninstalls in `defer`. `RenderServicesAccess` is an immutable view of the installed platform/canvas/resources/scene protocols so tests can inject recording implementations without mutating global state.

- [ ] **Step 4: Replace every platform/input function using Appendix B**

Move `GamepadManager` implementation under `Renderer/Legacy` or make it an internal collaborator of `LegacyPlatformBackend`; game code reads project input queries only. Route SDL file dialogs through `showOpenFileDialog`. Preserve miniaudio startup outside the renderer bundle.

- [ ] **Step 5: Prove raw platform imports are isolated and commit**

```bash
swift test --filter GamePlatformRoutingTests
swift test --filter MainLaunchModeTests
swift test --filter ParticlePersistenceTests
rg -n '^import SDL3$|\b(InitWindow|CloseWindow|WindowShouldClose|GetFrameTime|IsKeyPressed|IsMouseButtonPressed)\b' Sources/Main --glob '*.swift' --glob '!Sources/Main/Sources/Renderer/Legacy/**'
```

Expected: the final search prints nothing.

```bash
git add 'City Generator (r3d)/Sources/Main' 'City Generator (r3d)/Tests/MainTests/GamePlatformRoutingTests.swift'
git commit -m "refactor: route launch modes through game platform"
```

### Task 9: Route All 2D, Text, Image, And Snapshot Work Through Canvas2D

**Files:**
- Modify (Game): all files in Appendix A, batch `Canvas`.
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/GameText.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/UIScale.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/main.swift`
- Create (Game): `City Generator (r3d)/Tests/MainTests/CanvasCommandRoutingTests.swift`

**Interfaces:**
- Consumes: `Canvas2DBackend`, `TextureHandle`, `FontHandle`, `RenderTargetHandle`, and project geometry/color.
- Produces: UI/editor draw code that submits semantic Canvas commands and no longer calls raylib/rlgl.

- [ ] **Step 1: Write failing command-order and text-measurement tests**

Use a recording Canvas backend to assert that a representative card emits scissor push, background, image, text, and scissor pop in order; a level-up snapshot emits render-target/texture commands without raw texture IDs; `GameText` measures and draws through the same font handle.

- [ ] **Step 2: Run targeted Canvas/UI tests**

```bash
swift test --filter CanvasCommandRoutingTests
swift test --filter CardsOverlayLayoutTests
swift test --filter CombatLabPanelLayoutTests
```

Expected: the new routing test fails before Canvas migration; existing layout tests remain green.

- [ ] **Step 3: Migrate Canvas calls with semantic commands**

Apply every Canvas mapping in Appendix B. `GameText` becomes a project frontend holding `FontHandle` values loaded through `RenderResourceBackend`. `UIScale` reads logical size from `GamePlatform`. Level-up snapshot uses `RenderTargetHandle` and `CanvasCommand.texturedQuad`; no caller observes a raw `Texture2D.id`.

Replace direct collision helpers with `RenderRect.contains` and explicit circle-distance math. Replace raw color constants with project constants.

- [ ] **Step 4: Remove direct rlgl and raylib 2D access outside the adapter**

```bash
rg -n '\b(BeginDrawing|EndDrawing|Draw[A-Z]|BeginScissorMode|EndScissorMode|BeginTextureMode|EndTextureMode|rl[A-Z])' Sources/Main --glob '*.swift' --glob '!Sources/Main/Sources/Renderer/Legacy/**'
```

Expected: no matches.

- [ ] **Step 5: Run UI/editor tests and commit**

```bash
swift test --filter CanvasCommandRoutingTests
swift test --filter CardsOverlayLayoutTests
swift test --filter CombatLabPanelLayoutTests
swift test --filter ParticleEditorCoordinateTests
swift test --filter UnifiedCityGeneratorTests
git add 'City Generator (r3d)/Sources/Main' 'City Generator (r3d)/Tests/MainTests/CanvasCommandRoutingTests.swift'
git commit -m "refactor: route 2D rendering through Canvas2D"
```

### Task 10: Replace Raw Resources, Materials, Animation, Environment, And Post State With Handles

**Files:**
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/AssetsManager.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/PostProcessor.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/Player.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/Enemy/EnemyRenderCatalog.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/Fusion/FusionRenderMapping.swift`
- Modify (Game): renderer/resource consumers listed in Appendix A, batch `Resources`.
- Create (Game): `City Generator (r3d)/Tests/MainTests/RenderResourceHandleTests.swift`

**Interfaces:**
- Consumes: typed handles and `RenderResourceBackend`.
- Produces: asset catalogs, materials, shaders, sky, lights, animation players/trees, post state, and instance buffers represented only by project descriptors/handles outside the adapter.

- [ ] **Step 1: Write failing handle ownership and stale-access tests**

Test that `AssetsManager` returns stable project handles, repeated cache lookup returns the same live handle, `unloadAll` invalidates every handle, material override fingerprints use `MaterialDescriptor` rather than raw pointer/texture IDs, and animation tree ownership destroys child players before their library/model.

- [ ] **Step 2: Run resource ownership tests before migration**

```bash
swift test --filter RenderResourceHandleTests
swift test --filter SceneRendererOwnershipTests
swift test --filter PlayerAnimationTreeTests
```

Expected: the new test fails because public asset APIs expose raw r3d/raylib values.

- [ ] **Step 3: Convert AssetsManager and material signatures**

All cache dictionaries store project handles. Raw load/unload calls move into `LegacyResourceBackend`. `MaterialSignature` hashes the complete project `MaterialDescriptor`, including texture handle raw values, colors, emission energy, normal scale, ORM values, UV transform, alpha cutoff, depth/stencil modes, transparency, billboard, blend, cull, unlit, and surface shader handle.

The hash must not use pointer identity or Swift's per-process randomized `Hasher` for persisted/golden diagnostics; use stable FNV-1a over explicit fields.

- [ ] **Step 4: Convert animation, environment, sky, light, and post state**

Player/enemy animation state stores `AnimationPlayerHandle` and `AnimationTreeHandle`. Environment changes submit a validated `EnvironmentDescriptor` each frame. Light updates use `LightDescriptor`; no caller mutates `R3D_GetEnvironment().pointee`. PostProcessor returns a `PostChainDescriptor` consumed by `FrameDescriptor`, and shader sampler/uniform updates go through typed resource methods.

- [ ] **Step 5: Prove raw resources are adapter-private and commit**

```bash
rg -n '\b(R3D_(Model|Mesh|Material|Animation|Cubemap|AmbientMap|ScreenShader|SurfaceShader|InstanceBuffer)|Texture2D|RenderTexture2D|Font|Image)\b' Sources/Main --glob '*.swift' --glob '!Sources/Main/Sources/Renderer/Legacy/**'
```

Expected: no matches.

```bash
swift test --filter RenderResourceHandleTests
swift test --filter SceneRendererOwnershipTests
swift test --filter PlayerAnimationTreeTests
swift test --filter RenderMaterialContractTests
git add 'City Generator (r3d)/Sources/Main' 'City Generator (r3d)/Tests/MainTests/RenderResourceHandleTests.swift'
git commit -m "refactor: hide renderer resources behind handles"
```

### Task 11: Route 3D Scene Submission And Debug Drawing Through SceneBackend

**Files:**
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/SceneRenderer.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/CityRenderer.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/Combat/ProjectileRenderSystem.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/Fusion/FusionConstructRenderSystem.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/ParticleSystem/ParticleClassicPreviewHost.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/ParticleSystem/ParticleGamePreviewHost.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/ParticleSystem/Renderer.swift`
- Modify (Game): `City Generator (r3d)/Sources/Main/Sources/Classes/World/World+Debug.swift`
- Modify (Game): all remaining Appendix A files in batch `Scene`.
- Create (Game): `City Generator (r3d)/Tests/MainTests/SceneSubmissionSeamTests.swift`
- Create (Game): `City Generator (r3d)/Tests/MainTests/RendererDependencyBoundaryTests.swift`

**Interfaces:**
- Consumes: `SceneBackend`, immutable `FrameDescriptor`, handle-based `SceneDraw`, and existing persistent instance ownership rules.
- Produces: all 3D callers backend-neutral; raw r3d/raylib/SDL imports only under `Renderer/Legacy`.

- [ ] **Step 1: Write failing scene-order, instance-lifetime, and boundary tests**

Test one begin/end pair, opaque-before-transparent ordering, persistent city buffers uploading zero bytes on an unchanged frame, distinct instance storage until scene end, animation handles in animated draws, and debug commands as project values.

Boundary test:

```swift
func testRawGraphicsImportsExistOnlyInsideLegacyAdapter() throws {
    let violations = try RendererDependencyBoundary.scan().filter {
        !$0.path.hasPrefix("Sources/Main/Sources/Renderer/Legacy/")
    }
    XCTAssertEqual(violations, [])
}
```

Also scan forbidden symbols `R3D_`, `rl[A-Z]`, and the raw types from Task 10 outside the adapter. Comments and string literals are stripped before symbol scanning so diagnostics text does not create false positives.

- [ ] **Step 2: Run focused tests and record the expected failures**

```bash
swift test --filter SceneSubmissionSeamTests
swift test --filter RendererDependencyBoundaryTests
```

Expected: failures identify every remaining direct caller; do not weaken the scanner allowlist.

- [ ] **Step 3: Convert SceneRenderer into a backend-neutral deep frontend**

`SceneRenderer` retains batching, signatures, persistent chunk ownership, dirty ranges, frustum decisions, and performance diagnostics. Replace every raw draw with `SceneDraw`. Preserve the rule that instance backing storage remains valid until `SceneBackend.endFrame()` returns. Reserve arrays during scene preparation and reset with `removeAll(keepingCapacity: true)`.

- [ ] **Step 4: Convert world, editor, particle, and debug callers**

Use `SceneDraw.model`, `.modelInstances`, `.mesh`, `.meshInstances`, `.animatedModel`, `.animatedModelInstances`, and `.debug` exactly. Debug lines, planes, grids, spheres, bounds, and wire cubes submit project geometry. No caller opens a raw R3D cluster or render batch.

- [ ] **Step 5: Make the dependency boundary green without compatibility shims**

Remove `import raylib`, `import r3d`, and `import SDL3` from every non-adapter Swift file. Do not add project functions named after raylib/R3D solely to avoid call-site changes. Keep the existing raw C modules available to the `Main` target during Phase 1, but enforce with the source scanner that only `Renderer/Legacy` Swift files import them; module-map restructuring is deferred because it does not strengthen the checked source boundary.

- [ ] **Step 6: Run renderer ownership, hot-path, and full tests**

```bash
swift test --filter SceneSubmissionSeamTests
swift test --filter RendererDependencyBoundaryTests
swift test --filter SceneRendererOwnershipTests
swift test --filter SceneRendererRuntimeSafetyTests
swift test --filter RuntimeHotPathAllocationTests
swift test
```

Expected: all pass; boundary scan reports zero violations.

- [ ] **Step 7: Commit the completed Phase 1 seam migration**

```bash
git add 'City Generator (r3d)/Sources/Main' 'City Generator (r3d)/Tests/MainTests/SceneSubmissionSeamTests.swift' 'City Generator (r3d)/Tests/MainTests/RendererDependencyBoundaryTests.swift'
git commit -m "refactor: route 3D submission through renderer seam"
```

### Task 12: Run Five-Mode Parity Gates And Close Phase 0–1

**Files:**
- Modify (Game): `City Generator (r3d)/docs/rendering/legacy-baseline-report.md`
- Create (Game): `City Generator (r3d)/docs/rendering/phase-1-parity-report.md`
- Modify (Game): `City Generator (r3d)/docs/rendering/legacy-api-usage.txt`
- Modify (Game): `City Generator (r3d)/Vendor/BTBHRenderer` gitlink if the final renderer fix changes its commit.

**Interfaces:**
- Consumes: all Phase 0 goldens/metrics, the final legacy adapter, and five launch modes.
- Produces: a committed Phase 1 report proving visual/function/performance parity and a clean raw-dependency boundary.

- [ ] **Step 1: Run fresh static and unit verification**

Renderer:

```bash
swift test
git diff --check
```

Game:

```bash
swift test
swift build -c release
./tools/audit-render-api.sh /tmp/btbh-phase1-api-usage.txt
git diff --check
```

Expected: all tests/builds pass; the audit reports raw imports only in `Renderer/Legacy` and no new public dependency names.

- [ ] **Step 2: Run five-mode smoke checks**

Run each mode with its bounded baseline runner so exact PIDs are cleaned up:

```bash
./tools/capture-render-baselines.sh --candidate /tmp/btbh-phase1-baselines
```

Expected for every mode: startup, 1920×1080 capture, resize/high-DPI probe, asset load/unload, and clean shutdown complete without validation errors.

- [ ] **Step 3: Compare visual output and gameplay cue regions**

```bash
swift run RenderBaselineComparator \
  --manifest Tests/RenderGoldens/legacy/manifest.json \
  --candidate /tmp/btbh-phase1-baselines
```

Expected: global SSIM ≥0.98 for every mode, every cue region meets its minimum coverage, and no baseline golden is replaced. A failed comparison is a regression to diagnose, not permission to approve a new golden in Phase 1.

- [ ] **Step 4: Compare performance on both reference rigs**

Run two warmed captures per mode on M1 and the frozen Windows reference rig. The report lists both runs, comparable entity counts, median, p95, worst, and upload diagnostics. A regression above 5% requires a capture and fix before closure. Phase 1 does not need to hit the future 16.67 ms target because the GPU backend is still legacy, but it may not make the baseline worse without an approved reason.

- [ ] **Step 5: Write the parity report with exact evidence**

`phase-1-parity-report.md` must contain:

- renderer and game commit hashes;
- pinned submodule commit;
- five-mode test/capture table;
- SSIM and cue-region results;
- M1 and Windows median/p95 results;
- raw-import file list, which must contain only `Renderer/Legacy` paths;
- known legacy limitations explicitly carried into Phase 2;
- confirmation that SDL_GPU, Metal/Vulkan backend code, RC, and CameraProfile were not started.

- [ ] **Step 6: Commit the Phase 0–1 evidence**

```bash
git add 'City Generator (r3d)/docs/rendering/legacy-baseline-report.md' 'City Generator (r3d)/docs/rendering/phase-1-parity-report.md' 'City Generator (r3d)/docs/rendering/legacy-api-usage.txt' 'City Generator (r3d)/Vendor/BTBHRenderer'
git commit -m "docs: close renderer phases zero and one"
```

## Appendix A: Migration Batches

Every starting raw-import file is assigned to at least one batch. Remove its raw import only after all of its assigned uses have moved.

### Math/Data batch

- `Sources/Main/Sources/Classes/BlockGenerators/BuildingBlockGenerator.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/CityGenerator.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/GameCityBuildFactory.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/LandscapeGenerator.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/ParkBlockGenerator.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/ResidentialBlockGenerator.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/WaterBlockGenerator.swift`
- `Sources/Main/Sources/Classes/CameraController.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockDecoratedAssetCatalog.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditorCatalog.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditorPreviewCache.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditorState.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockProceduralTemplateGenerator.swift`
- `Sources/Main/Sources/Classes/Combat/ProjectileRenderItem.swift`
- `Sources/Main/Sources/Classes/Combat/ProjectileRendererBenchmark.swift`
- `Sources/Main/Sources/Classes/Combat/ProjectileRendererBenchmarkRunner.swift`
- `Sources/Main/Sources/Classes/Enemy/Enemy.swift`
- `Sources/Main/Sources/Classes/Enemy/EnemyOrchestrator.swift`
- `Sources/Main/Sources/Classes/Fusion/FusionRenderMapping.swift`
- `Sources/Main/Sources/Classes/Game/Game+ConvoyObjective.swift`
- `Sources/Main/Sources/Classes/Game/Game+Destructables.swift`
- `Sources/Main/Sources/Classes/Game/Game+ReactorCollapse.swift`
- `Sources/Main/Sources/Classes/Game/Game+SideObjectives.swift`
- `Sources/Main/Sources/Classes/Game/Game+UndergroundShelter.swift`
- `Sources/Main/Sources/Classes/Game/Game+WorldEncounters.swift`
- `Sources/Main/Sources/Classes/Game/Game.swift`
- `Sources/Main/Sources/Classes/Math/Automation.swift`
- `Sources/Main/Sources/Classes/Math/Misc.swift`
- `Sources/Main/Sources/Classes/Math/OkHSL.swift`
- `Sources/Main/Sources/Classes/Math/PeriodicFunctions.swift`
- `Sources/Main/Sources/Classes/Math/Vector.swift`
- `Sources/Main/Sources/Classes/MetaProgression.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/CoreTypes.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/ParticleEditorPreviewState.swift`
- `Sources/Main/Sources/Classes/UnifiedCityGenerator/UnifiedCitySceneFactory.swift`
- `Sources/Main/Sources/Classes/UniqueRewards.swift`
- `Sources/Main/Sources/Classes/World/World+Cars.swift`
- `Sources/Main/Sources/Classes/World/World+Collision.swift`
- `Sources/Main/Sources/Classes/World/World+Combat.swift`
- `Sources/Main/Sources/Classes/World/World+Contact.swift`
- `Sources/Main/Sources/Classes/World/World+Effects.swift`
- `Sources/Main/Sources/Classes/World/World+Enemies.swift`
- `Sources/Main/Sources/Classes/World/World+EnemiesNav.swift`
- `Sources/Main/Sources/Classes/World/World+EnemyAbilities.swift`
- `Sources/Main/Sources/Classes/World/World+Mutators.swift`
- `Sources/Main/Sources/Classes/World/World+Obstacles.swift`
- `Sources/Main/Sources/Classes/World/World+Orbs.swift`
- `Sources/Main/Sources/Classes/World/World+RenderEntities.swift`
- `Sources/Main/Sources/Classes/World/World+Status.swift`
- `Sources/Main/Sources/Classes/World/World+Types.swift`
- `Sources/Main/Sources/Classes/World/World.swift`

### Canvas batch

- `Sources/Main/main.swift`
- `Sources/Main/Sources/Classes/Cards.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditor.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditorRunner.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditorUI.swift`
- `Sources/Main/Sources/Classes/CombatLab/CombatLabPanel.swift`
- `Sources/Main/Sources/Classes/Combat/ProjectileRendererBenchmarkRunner.swift`
- `Sources/Main/Sources/Classes/Game/Game+Progression.swift`
- `Sources/Main/Sources/Classes/Game/Game+UIRender.swift`
- `Sources/Main/Sources/Classes/Game/LevelUpPreview3D.swift`
- `Sources/Main/Sources/Classes/GameText.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/CurveEditor.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/EditorLayout.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/GradientEditor.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/ParticleDemo.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/UIComponents.swift`
- `Sources/Main/Sources/Classes/RaylibColors.swift`
- `Sources/Main/Sources/Classes/UIScale.swift`
- `Sources/Main/Sources/Classes/UnifiedCityGenerator/UnifiedCityGeneratorRunner.swift`
- `Sources/Main/Sources/Classes/UnifiedCityGenerator/UnifiedCityGeneratorUI.swift`

### Resources batch

- `Sources/Main/Sources/Classes/AssetsManager.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/BuildingBlockGenerator.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/LandscapeGenerator.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/ResidentialBlockGenerator.swift`
- `Sources/Main/Sources/Classes/BlockGenerators/WaterBlockGenerator.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockDecoratedAssetCatalog.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditor.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditorCatalog.swift`
- `Sources/Main/Sources/Classes/Enemy/EnemyRenderCatalog.swift`
- `Sources/Main/Sources/Classes/Fusion/FusionConstructRenderSystem.swift`
- `Sources/Main/Sources/Classes/Fusion/FusionRenderMapping.swift`
- `Sources/Main/Sources/Classes/Game/Game+EncounterRender.swift`
- `Sources/Main/Sources/Classes/Game/Game+GrowthTrial.swift`
- `Sources/Main/Sources/Classes/Game/LevelUpPreview3D.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/ParticleClassicPreviewHost.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/ParticleGamePreviewHost.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/Renderer.swift`
- `Sources/Main/Sources/Classes/Player.swift`
- `Sources/Main/Sources/Classes/PostProcessor.swift`
- `Sources/Main/Sources/Classes/SceneRenderer.swift`
- `Sources/Main/Sources/Classes/CityRenderer.swift`

### Scene batch

- `Sources/Main/main.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditor.swift`
- `Sources/Main/Sources/Classes/CityBlockEditor/CityBlockEditorRunner.swift`
- `Sources/Main/Sources/Classes/CityRenderer.swift`
- `Sources/Main/Sources/Classes/Combat/ProjectileRenderSystem.swift`
- `Sources/Main/Sources/Classes/Combat/ProjectileRendererBenchmarkRunner.swift`
- `Sources/Main/Sources/Classes/Fusion/FusionConstructRenderSystem.swift`
- `Sources/Main/Sources/Classes/Game/Game+EncounterRender.swift`
- `Sources/Main/Sources/Classes/Game/Game+GrowthTrial.swift`
- `Sources/Main/Sources/Classes/Game/LevelUpPreview3D.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/ParticleClassicPreviewHost.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/ParticleDemo.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/ParticleGamePreviewHost.swift`
- `Sources/Main/Sources/Classes/ParticleSystem/Renderer.swift`
- `Sources/Main/Sources/Classes/Player.swift`
- `Sources/Main/Sources/Classes/SceneRenderer.swift`
- `Sources/Main/Sources/Classes/UnifiedCityGenerator/UnifiedCityGeneratorRunner.swift`
- `Sources/Main/Sources/Classes/UnifiedCityGenerator/UnifiedCityGeneratorUI.swift`
- `Sources/Main/Sources/Classes/World/World+Debug.swift`
- `Sources/Main/Sources/Classes/World/World+Render.swift`
- `Sources/Main/Sources/Classes/World/World+RenderEntities.swift`

## Appendix B: Required Legacy-to-Project Mapping

### Platform/input

- `InitWindow`, `ToggleFullscreen`, `ToggleBorderlessWindowed`, `CloseWindow` → `GamePlatform.openWindow/closeWindow`.
- `WindowShouldClose`, `IsWindowFocused` → platform properties.
- `GetFrameTime`, `GetTime`, `GetFPS`, monitor refresh and screen/render sizes → `GamePlatform.beginFrame`, `framesPerSecond`, `refreshRate`, and frame sizes.
- `IsKeyDown`, `IsKeyPressed` → `isKeyDown`, `wasKeyPressed` with `InputKey`.
- mouse position/delta/wheel/button calls → matching project mouse queries.
- `HideCursor`, `ShowCursor`, `IsCursorHidden` → `setCursorVisible` and `isCursorVisible`.
- `SetClipboardText` → throwing `setClipboardText`.
- SDL gamepad/event/dialog calls → `LegacyPlatformBackend` internal collaborators.

### Math/geometry

- `Vector2*`, `Vector3*`, quaternion, matrix, `Lerp`, and `Remap` calls → methods/operators on project render math values.
- `CheckCollisionPointRec` → `RenderRect.contains`.
- `CheckCollisionPointCircle` → squared-distance comparison.
- `GetWorldToScreen`, `GetScreenToWorldRay` → `SceneBackend.project/unprojectRay` project-value queries.

### Canvas/resources

- `BeginDrawing/EndDrawing` → `Canvas2DBackend.beginFrame/endFrame`.
- every 2D `Draw*` call → a semantic `CanvasCommand`.
- scissor, blend, texture-mode, and batch flush → adapter-internal Canvas commands.
- font/image/texture/render-target load, filter, wrap, mipmap, and unload → `RenderResourceBackend` and typed handles.
- `LoadImageFromScreen` snapshot → `SceneBackend.captureWorldFrameTexture()` for level-up and Canvas presented-frame readback for baselines.

### Scene/resources

- `R3D_Init/Close/SetResolution` → `SceneBackend.initialize/shutdown/resize`.
- `R3D_Begin/End` → `SceneBackend.beginFrame/endFrame`.
- model/mesh/animated/instanced draw calls → matching `SceneDraw` cases.
- clusters remain a private optimization inside `LegacySceneBackend`; no public cluster command.
- model/mesh/material/instance/animation/shader/sky/ambient load/update/unload → `RenderResourceBackend` typed methods.
- `R3D_GetEnvironment` pointer mutation → immutable `EnvironmentDescriptor` in `FrameDescriptor`.
- light creation/mutation/look-at/shadow calls → `LightDescriptor` creation/update.
- frustum query → project bounds query on `SceneBackend` or frontend culling state.
- primitive generation → explicit mesh descriptors returned through resources.
- raw surface/screen shader uniforms and chains → typed shader parameter descriptors and `PostChainDescriptor`.

## Completion Definition

Phase 0–1 is complete only when:

1. the renderer package and game each have green full tests;
2. both repos have clean intended diffs and the game pins an exact renderer commit;
3. all five launch modes pass capture, resize/high-DPI, asset lifecycle, and clean shutdown;
4. every visual comparison is SSIM ≥0.98 and every cue region passes;
5. M1 and Windows reference reports contain median and p95 with comparable loads;
6. no >5% unapproved regression exists;
7. raw graphics/platform imports and symbols exist only in `Renderer/Legacy`;
8. the public renderer package contains no raylib/r3d/SDL/backend types;
9. the legacy renderer remains selectable and visually equivalent;
10. no SDL_GPU, Metal/Vulkan renderer, Radiance Cascades, or CameraProfile implementation has leaked into this phase.
