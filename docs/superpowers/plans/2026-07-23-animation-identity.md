# Stable Animation Identity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve full imported animation names and make game animation-tree leaves bind the exact zero-based catalog index without a lossy name round-trip.

**Architecture:** The native renderer owns full Assimp animation names for the lifetime of `R3D_AnimationLib` and adds an additive by-index animation-node constructor. The legacy Swift adapter validates the catalog index and calls that constructor directly; existing name-based R3D callers remain supported through canonical library-owned names.

**Tech Stack:** C11, Assimp C API, raylib allocation functions, R3D animation player/tree, CMake/CTest, Swift 5.10, SwiftPM/XCTest.

## Global Constraints

- Animation indices remain zero-based; do not add or subtract one anywhere.
- `R3D_AnimationLib` owns every imported animation name and frees it in `R3D_UnloadAnimationLib`.
- An animation-tree node stores the canonical library-owned name pointer, never a caller-owned temporary pointer.
- Existing name-based `R3D_CreateAnimationNode` and `R3D_CreateAnimationNodeEx` remain available.
- Invalid indices return `NULL` in R3D and become a structured `LegacyAdapterError` in the game adapter.
- The renderer C ABI intentionally changes; native library, mirrored game headers, and Swift adapter must be rebuilt and committed together.
- Player catalog values `idle=17`, `walk=24`, `destruct=1`, and `shelterStomp=0` remain unchanged.
- No game or renderer code may infer animation identity from a truncated prefix.

---

## File Map

### Renderer repository

- `include/r3d/r3d_animation.h`: changes animation-name ownership from fixed storage to a library-owned pointer.
- `include/r3d/r3d_animation_tree.h`: changes node-name parameters to canonical pointers and declares the by-index constructor.
- `src/importer/r3d_importer_animation.c`: allocates full Assimp names and unwinds them on every failure path.
- `src/r3d_animation.c`: frees full names and performs full-name lookup.
- `src/r3d_animation_tree.c`: resolves name/index once and builds nodes from an exact `R3D_Animation*`.
- `examples/animtree.c`: updates assignment syntax for pointer-based node names.
- `tests/r3d_animation_identity_test.c`: native regression for long-prefix collisions and by-index selection.
- `CMakeLists.txt`: registers the focused CTest target.

### Game repository

- `City Generator (r3d)/include/r3d/r3d_animation.h`: mirrors the renderer ABI.
- `City Generator (r3d)/include/r3d/r3d_animation_tree.h`: mirrors the renderer ABI.
- `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyResourceBackend.swift`: creates tree leaves by validated index.
- `City Generator (r3d)/Tests/MainTests/AnimationSelectionContractTests.swift`: locks the adapter and catalog contract.
- `City Generator (r3d)/lib/libr3d.a`: rebuilt arm64/macOS 11 native archive.
- `City Generator (r3d)/Vendor/BTBHRenderer`: advances to the renderer commit.

---

### Task 1: Add a Native Red Test for Stable Animation Identity

**Files:**
- Create: `tests/r3d_animation_identity_test.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: current `R3D_Animation`, `R3D_AnimationNodeParams`, and name-based tree API.
- Produces: CTest executable `r3d_animation_identity_test` and test name `r3d_animation_identity`.

- [ ] **Step 1: Add the failing native test**

Create `tests/r3d_animation_identity_test.c`:

```c
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <r3d/r3d_animation.h>
#include <r3d/r3d_animation_tree.h>

int main(void)
{
    static char idleName[] = "Mechagodzilla_Legendary_skeleton|idle";
    static char walkName[] = "Mechagodzilla_Legendary_skeleton|walk";

    R3D_Animation animations[2] = {
        {.name = idleName, .ticksPerSecond = 1.0f, .duration = 1.0f},
        {.name = walkName, .ticksPerSecond = 1.0f, .duration = 2.0f},
    };
    R3D_AnimationLib library = {
        .animations = animations,
        .count = 2,
    };

    assert(strcmp(library.animations[0].name, idleName) == 0);
    assert(strcmp(library.animations[1].name, walkName) == 0);
    assert(R3D_GetAnimationIndex(library, idleName) == 0);
    assert(R3D_GetAnimationIndex(library, walkName) == 1);

    R3D_AnimationPlayer player = {.animLib = library};
    R3D_AnimationTree tree = R3D_LoadAnimationTree(player, 2);
    assert(tree.nodePool != NULL);

    R3D_AnimationNodeParams parameters = {
        .state = {
            .speed = 1.0f,
            .play = true,
            .loop = true,
        },
        .looper = true,
    };
    R3D_AnimationTreeNode* walk = R3D_CreateAnimationNodeByIndex(&tree, 1, parameters);
    assert(walk != NULL);
    assert(strcmp(R3D_GetAnimationNodeParams(walk).name, walkName) == 0);
    assert(R3D_CreateAnimationNodeByIndex(&tree, -1, parameters) == NULL);
    assert(R3D_CreateAnimationNodeByIndex(&tree, 2, parameters) == NULL);

    R3D_UnloadAnimationTree(tree);
    return 0;
}
```

In `CMakeLists.txt`, after the existing project options, add:

```cmake
option(R3D_BUILD_TESTS "Build focused r3d regression tests" ${R3D_IS_MAIN})
```

After the `r3d` target is fully declared and linked, add:

```cmake
if(R3D_BUILD_TESTS)
    enable_testing()
    add_executable(r3d_animation_identity_test
        "${R3D_ROOT_PATH}/tests/r3d_animation_identity_test.c"
    )
    target_link_libraries(r3d_animation_identity_test PRIVATE r3d)
    add_test(NAME r3d_animation_identity COMMAND r3d_animation_identity_test)
endif()
```

- [ ] **Step 2: Configure and prove the test is red**

Run:

```bash
cmake -S . -B build \
  -DR3D_RAYLIB_VENDORED=ON \
  -DR3D_ASSIMP_VENDORED=ON \
  -DR3D_BUILD_EXAMPLES=OFF \
  -DR3D_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build --target r3d_animation_identity_test -j 8
```

Expected: compilation fails because fixed arrays cannot accept `char*` initializers and `R3D_CreateAnimationNodeByIndex` is undeclared.

---

### Task 2: Preserve Full Names and Add By-Index Node Creation

**Files:**
- Modify: `include/r3d/r3d_animation.h`
- Modify: `include/r3d/r3d_animation_tree.h`
- Modify: `src/importer/r3d_importer_animation.c`
- Modify: `src/r3d_animation.c`
- Modify: `src/r3d_animation_tree.c`
- Modify: `examples/animtree.c`
- Test: `tests/r3d_animation_identity_test.c`

**Interfaces:**
- Consumes: `R3D_AnimationLib`, `R3D_AnimationPlayer`, and `R3D_AnimationNodeParams`.
- Produces: `R3D_CreateAnimationNodeByIndex(R3D_AnimationTree*, int, R3D_AnimationNodeParams)` and full library-owned names.

- [ ] **Step 1: Change the public ownership types**

In `include/r3d/r3d_animation.h`, replace the fixed name field with:

```c
char* name;  ///< Full null-terminated animation name owned by R3D_AnimationLib.
```

In `include/r3d/r3d_animation_tree.h`, replace the fixed node parameter name with:

```c
const char* name;  ///< Canonical library-owned animation name; NULL for by-index creation input.
```

Declare the additive API immediately after `R3D_CreateAnimationNodeEx`:

```c
/**
 * @brief Creates an animation leaf node from an exact zero-based library index.
 *
 * The returned node stores the canonical library-owned animation name.
 *
 * @param tree Animation tree whose player owns the animation library.
 * @param animationIndex Zero-based index in tree->player.animLib.
 * @param params Playback, loop, and callback parameters; params.name is ignored.
 * @return Created node, or NULL when tree/library/index is invalid.
 */
R3DAPI R3D_AnimationTreeNode* R3D_CreateAnimationNodeByIndex(
    R3D_AnimationTree* tree,
    int animationIndex,
    R3D_AnimationNodeParams params
);
```

- [ ] **Step 2: Allocate and unwind full importer names**

At the beginning of `load_animation` in `src/importer/r3d_importer_animation.c`, zero the destination:

```c
*animation = (R3D_Animation) {0};
```

Replace the truncating copy with:

```c
size_t nameLen = aiAnim->mName.length;
animation->name = MemAlloc(nameLen + 1);
if (!animation->name) {
    R3D_TRACELOG(LOG_ERROR, "Failed to allocate animation name");
    return false;
}
memcpy(animation->name, aiAnim->mName.data, nameLen);
animation->name[nameLen] = '\0';
```

On every later `return false` path in `load_animation`, free and clear the name after freeing any channel storage:

```c
MemFree(animation->name);
animation->name = NULL;
```

The channel-allocation failure and zero-successful-channel branches must both execute that cleanup.

- [ ] **Step 3: Free names with the library**

In `R3D_UnloadAnimationLib` in `src/r3d_animation.c`, add this before freeing each animation's channels:

```c
MemFree(anim->name);
anim->name = NULL;
```

Keep `R3D_GetAnimationIndex` and `R3D_GetAnimation` as exact `strcmp` lookups; they now operate on full strings.

- [ ] **Step 4: Resolve animation identity once inside the tree**

Replace the current name-resolving `atree_anim_create` body in `src/r3d_animation_tree.c` with a resolved helper and a thin name wrapper:

```c
static R3D_AnimationTreeNode* atree_anim_create_resolved(
    R3D_AnimationTree* atree,
    const R3D_Animation* animation,
    R3D_AnimationNodeParams params,
    bool reset
)
{
    if (!atree || !animation) return NULL;

    R3D_AnimationTreeNode* anode = anode_create(
        atree,
        R3D_ANIMTREE_ANIM,
        sizeof(r3d_animtree_anim_t)
    );
    if (!anode) return NULL;

    params.name = animation->name;
    r3d_animtree_anim_t* anim = anode->anim;
    anim->animation = animation;
    anim->params = params;
    if (reset) anode_reset_anim(anim);

    int boneIdx = atree->rootBone;
    if (valid_root_bone(boneIdx)) {
        const R3D_AnimationState* state = &anim->params.state;
        const R3D_AnimationChannel* channel = r3d_anim_channel_find(animation, boneIdx);
        if (channel) {
            anim->root.last = r3d_anim_channel_lerp(
                channel,
                state->currentTime * animation->ticksPerSecond,
                &anim->root.rest0,
                &anim->root.restN
            );
        }
        else {
            Transform bind;
            MatrixDecompose(
                atree->player.skeleton.localBind[boneIdx],
                &bind.translation,
                &bind.rotation,
                &bind.scale
            );
            anim->root.last = bind;
            anim->root.rest0 = bind;
            anim->root.restN = bind;
        }
        anim->root.loops = -1;
    }

    return anode;
}

static R3D_AnimationTreeNode* atree_anim_create(
    R3D_AnimationTree* atree,
    R3D_AnimationNodeParams params,
    bool reset
)
{
    if (!atree || !params.name) {
        R3D_TRACELOG(LOG_WARNING, "Failed to create animation node: missing tree or name");
        return NULL;
    }
    const R3D_Animation* animation = R3D_GetAnimation(atree->player.animLib, params.name);
    if (!animation) {
        R3D_TRACELOG(
            LOG_WARNING,
            "Failed to create animation node: animation \"%s\" not found",
            params.name
        );
        return NULL;
    }
    return atree_anim_create_resolved(atree, animation, params, reset);
}
```

Add the public function beside the existing creation entry points:

```c
R3D_AnimationTreeNode* R3D_CreateAnimationNodeByIndex(
    R3D_AnimationTree* tree,
    int animationIndex,
    R3D_AnimationNodeParams params
)
{
    if (!tree ||
        !tree->player.animLib.animations ||
        animationIndex < 0 ||
        animationIndex >= tree->player.animLib.count) {
        R3D_TRACELOG(
            LOG_WARNING,
            "Failed to create animation node: invalid animation index %d",
            animationIndex
        );
        return NULL;
    }
    return atree_anim_create_resolved(
        tree,
        &tree->player.animLib.animations[animationIndex],
        params,
        true
    );
}
```

In `R3D_SetAnimationNodeParams`, canonicalize the incoming pointer before storing:

```c
if (!node || node->base->type != R3D_ANIMTREE_ANIM) return;
params.name = node->anim->animation->name;
node->anim->params = params;
```

- [ ] **Step 5: Update the example to pointer assignment**

In `examples/animtree.c`, replace each `TextCopy(loopingAnimParams.name, "...")` with direct assignment:

```c
loopingAnimParams.name = "walk left";
loopingAnimParams.name = "walk right";
loopingAnimParams.name = "walk forward";
loopingAnimParams.name = "walk backward";
```

The compound literal `.name = "idle"` remains valid.

- [ ] **Step 6: Run the native test and build**

Run:

```bash
cmake --build build --target r3d_animation_identity_test -j 8
ctest --test-dir build -R '^r3d_animation_identity$' --output-on-failure
cmake --build build --target r3d -j 8
```

Expected: one focused CTest passes and `build/lib/libr3d.a` is produced.

- [ ] **Step 7: Commit the renderer change**

```bash
git add \
  CMakeLists.txt \
  include/r3d/r3d_animation.h \
  include/r3d/r3d_animation_tree.h \
  src/importer/r3d_importer_animation.c \
  src/r3d_animation.c \
  src/r3d_animation_tree.c \
  examples/animtree.c \
  tests/r3d_animation_identity_test.c
git commit -m "fix: preserve animation identity through tree creation"
```

---

### Task 3: Migrate the Game Adapter to Exact Indices

**Files:**
- Create: `City Generator (r3d)/Tests/MainTests/AnimationSelectionContractTests.swift`
- Modify: `City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyResourceBackend.swift`
- Modify: `City Generator (r3d)/include/r3d/r3d_animation.h`
- Modify: `City Generator (r3d)/include/r3d/r3d_animation_tree.h`
- Modify: `City Generator (r3d)/lib/libr3d.a`
- Modify: `City Generator (r3d)/Vendor/BTBHRenderer`

**Interfaces:**
- Consumes: `R3D_CreateAnimationNodeByIndex` from Task 2.
- Produces: a legacy adapter that preserves `AnimationClipDescriptor.animationIndex` exactly.

- [ ] **Step 1: Add the failing game contract tests**

Create `City Generator (r3d)/Tests/MainTests/AnimationSelectionContractTests.swift`:

```swift
import Foundation
import XCTest

final class AnimationSelectionContractTests: XCTestCase {
    func testLegacyAnimationTreeUsesValidatedZeroBasedIndexDirectly() throws {
        let source = try projectSource(
            "Sources/Main/Sources/Renderer/Legacy/LegacyResourceBackend.swift"
        )
        XCTAssertTrue(source.contains("R3D_CreateAnimationNodeByIndex("))
        XCTAssertFalse(source.contains(
            "parameters.name = rawLibrary.animations[clip.animationIndex].name"
        ))
    }

    func testPlayerCatalogIndicesRemainZeroBasedAssetIndices() throws {
        let source = try projectSource("Sources/Main/Sources/Classes/AssetsManager.swift")
        XCTAssertTrue(source.contains(
            #".init(name: "idle", animationIndex: 17"#
        ))
        XCTAssertTrue(source.contains(
            #".init(name: "walk", animationIndex: 24"#
        ))
        XCTAssertTrue(source.contains(
            #".init(name: "destruct", animationIndex: 1"#
        ))
        XCTAssertTrue(source.contains(
            #".init(name: "shelterStomp", animationIndex: 0"#
        ))
    }

    private func projectSource(_ relativePath: String) throws -> String {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        return try String(
            contentsOf: packageRoot.appendingPathComponent(relativePath),
            encoding: .utf8
        )
    }
}
```

- [ ] **Step 2: Prove the game test is red**

Run from `City Generator (r3d)`:

```bash
swift test --filter AnimationSelectionContractTests
```

Expected: `testLegacyAnimationTreeUsesValidatedZeroBasedIndexDirectly` fails because the adapter still converts the index to a truncated name.

- [ ] **Step 3: Advance the local renderer submodule and mirror headers**

Run:

```bash
git -C "Vendor/BTBHRenderer" fetch \
  /Users/duke4e/Desktop/r3d/r3d \
  refs/heads/codex/renderer-phase-0-1
git -C "Vendor/BTBHRenderer" checkout --detach FETCH_HEAD
```

Apply the exact Task 2 declarations to:

- `include/r3d/r3d_animation.h`
- `include/r3d/r3d_animation_tree.h`

Verify the mirrors:

```bash
diff -u \
  /Users/duke4e/Desktop/r3d/r3d/include/r3d/r3d_animation.h \
  include/r3d/r3d_animation.h
diff -u \
  /Users/duke4e/Desktop/r3d/r3d/include/r3d/r3d_animation_tree.h \
  include/r3d/r3d_animation_tree.h
```

Expected: both commands print no diff.

- [ ] **Step 4: Call the by-index API**

In `LegacyResourceBackend.createAnimationTree`, keep the existing range guard, remove:

```swift
parameters.name = rawLibrary.animations[clip.animationIndex].name
```

Replace the node creation with:

```swift
guard let node = R3D_CreateAnimationNodeByIndex(
    &tree,
    Int32(clip.animationIndex),
    parameters
) else {
    throw LegacyAdapterError.rawOperationFailed(
        "Could not create animation clip node \(clip.name) at index \(clip.animationIndex)"
    )
}
```

- [ ] **Step 5: Install the rebuilt native archive**

From the renderer repository, verify and copy the exact build product:

```bash
file build/lib/libr3d.a
lipo -info build/lib/libr3d.a
cp build/lib/libr3d.a \
  "/Users/duke4e/Documents/GitHub/Be-The-Bullet-Hell/City Generator (r3d)/lib/libr3d.a"
```

Expected: the archive reports `arm64`; it must not report an x86-only object.

- [ ] **Step 6: Run the focused test and full game tests**

Run from `City Generator (r3d)`:

```bash
swift test --filter AnimationSelectionContractTests
swift test
```

Expected: the focused tests and full XCTest suite pass.

- [ ] **Step 7: Commit the game integration**

From the game repository root:

```bash
git add \
  "City Generator (r3d)/Vendor/BTBHRenderer" \
  "City Generator (r3d)/include/r3d/r3d_animation.h" \
  "City Generator (r3d)/include/r3d/r3d_animation_tree.h" \
  "City Generator (r3d)/lib/libr3d.a" \
  "City Generator (r3d)/Sources/Main/Sources/Renderer/Legacy/LegacyResourceBackend.swift" \
  "City Generator (r3d)/Tests/MainTests/AnimationSelectionContractTests.swift"
git commit -m "fix: bind model animations by exact asset index"
```

---

### Task 4: Verify the Real Player Asset and Clean Builds

**Files:**
- Verify only; no source changes expected.

**Interfaces:**
- Consumes: rebuilt archive and game player GLB.
- Produces: evidence that the importer retains full names at indices 17 and 24.

- [ ] **Step 1: Compile a throwaway asset probe from standard input**

Run from `City Generator (r3d)`:

```bash
printf '%s\n' \
  '#include "r3d/r3d.h"' \
  '#include <assert.h>' \
  '#include <string.h>' \
  'int main(void) {' \
  '  R3D_AnimationLib lib = R3D_LoadAnimationLib("Sources/Main/Assets/Players/mecha_godzilla_rig.glb");' \
  '  assert(lib.count == 25);' \
  '  assert(strcmp(lib.animations[17].name, "Mechagodzilla_Legendary_skeleton|idle") == 0);' \
  '  assert(strcmp(lib.animations[24].name, "Mechagodzilla_Legendary_skeleton|walk") == 0);' \
  '  assert(R3D_GetAnimationIndex(lib, lib.animations[17].name) == 17);' \
  '  assert(R3D_GetAnimationIndex(lib, lib.animations[24].name) == 24);' \
  '  R3D_UnloadAnimationLib(lib);' \
  '  return 0;' \
  '}' \
  | clang -x c - \
      -Iinclude -Iinclude/raylib -Iinclude/r3d \
      -Llib -lr3d -lraylib -lassimp -lSDL3 -lz -lc++ \
      -framework Cocoa -framework IOKit -framework CoreVideo \
      -framework CoreAudio -framework AudioToolbox \
      -framework GameController -framework CoreHaptics \
      -framework ForceFeedback \
      -o /private/tmp/btbh-animation-identity-probe
/private/tmp/btbh-animation-identity-probe
```

Expected: exit status 0 with no assertion failure.

- [ ] **Step 2: Verify clean package and Xcode builds**

Run:

```bash
swift package clean
swift build
xcodebuild \
  -scheme Main \
  -destination 'platform=macOS' \
  -derivedDataPath /private/tmp/btbh-animation-derived-data \
  clean build
```

Expected: both builds finish successfully.

- [ ] **Step 3: Record final repository state**

Run:

```bash
git -C /Users/duke4e/Desktop/r3d/r3d status --short
git -C /Users/duke4e/Documents/GitHub/Be-The-Bullet-Hell status --short
```

Expected: both worktrees are clean before starting the preview viewport plan.
