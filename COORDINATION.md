# Coordinating btbh-renderer across games

Three games build on this renderer (see [`CONSUMERS.md`](CONSUMERS.md)). They live on two machines
(a Windows PC and a Mac) whose Claude sessions cannot message each other, so the repository itself is
the shared board. These rules keep one renderer that every game can trust.

## One master, one coordinator

- There is one long-lived branch: `master`. Feature work happens on short-lived branches that merge
  back into it.
- One coordinator session (on the Windows PC) is the only one that changes the renderer: it reviews
  requests, implements or merges them, runs the release checks and cuts releases. Game sessions do not commit to this
  repository; they open issues (below).
- The coordinator's main working copy (`btbh-renderer/` next to the games) stays on `master` and clean.
  Branch work happens in separate worktrees (`git worktree add ../btbh-renderer-wt-<name> -b <branch>`).
  Until every game pins a tag, some builds still compile from that working copy.

## Versioning: the C header is the contract

Releases are semver tags `vMAJOR.MINOR.PATCH` on `master`. The contract is the public C API:
`include/btbh_gpu/btbh_gpu.h` and `include/btbh_gpu/btbh_gpu_resources.h`, plus the behaviour they
document and the shader file names the host loads.

| Change | Bump |
| --- | --- |
| New function, new enum value appended, new struct field whose zero value keeps the old behaviour, new shader file | minor |
| Bug fix, performance, a shader change that keeps the documented look | patch |
| Removed or renamed symbol or field, a changed enum value, a changed default, a changed meaning of an existing field, a renamed shader file | major |

Before 1.0.0 the same table applies with every bump moved one place to the right (breaking → minor,
additive → minor or patch), as semver allows, but every breaking change is still listed under
**Breaking** in [`CHANGELOG.md`](CHANGELOG.md) with what each game must change.

The Swift wrapper (`Sources/`) and the Rust bindings (generated in CHIMERA) follow the C header; they are
not separately versioned.

## New features are opt-in

- A new capability is off unless the host asks for it. Every new struct field must mean "as before" when
  zero, because all hosts zero-initialise these structs (`SDL_zero`, `= {0}`, `Default`/`zeroed()`,
  Swift's `init()`).
- New enum values are appended, never inserted, and existing values never change.
- When a feature needs a device capability, report it through the existing
  `BTBH_GPU_FEATURE_*` flags or a new one, and fall back rather than fail.
- A feature that only one game uses is fine, as long as it stays opt-in and is covered by that game's
  smoke scene (below).

## Requesting a feature or a fix

1. The game's session opens a GitHub issue with the **Feature request** template
   (`.github/ISSUE_TEMPLATE/feature_request.md`): the need, the proposed API, a minimal example, the
   platforms and the consumer. Bugs use the bug template.
2. Small additive requests: the coordinator implements them, adds a CHANGELOG line and releases a minor
   version. The issue gets the tag that contains the change.
3. Anything that touches more than one game, changes the uniform/G-buffer layout, adds a render pass or is
   breaking needs an RFC first: `docs/rfc/NNNN-title.md` from [`docs/rfc/0000-template.md`](docs/rfc/0000-template.md),
   linked from the issue. The coordinator merges it once each affected game's session agreed on the issue.
4. The game's session bumps its pin when it is ready (below). Nobody bumps another game's pin.

## Pinning

Every game pins an exact tag, never a branch or a live working copy:

- **Be The Bullet Hell**: the git submodule `City Generator (r3d)/Vendor/BTBHRenderer` checked out at the tag.
- **CHIMERA**: `BTBH_RENDERER_DIR` points at a checkout of the tag (for example a
  `btbh-renderer-v0.1.0` worktree or a vendored copy), not at the coordinator's working copy.
- **Tyrian3D**: the git submodule `external/btbh-renderer` checked out at the tag.

A bump is a commit in the game's repository with the tag in its message, after the game builds and its
smoke scene matches its reference image.

## Smoke scenes and reference images

Each game contributes one small scene that exercises the features it uses (its lighting, surfaces, post
chain), with a reference PNG per platform it ships on (`Tests/Fixtures/consumers/<game>/`). The release
check renders them on each machine; a release that changes a reference image says so in the CHANGELOG. Until
these exist, each game's session checks its own look before bumping.

## Emergency game branches

If a game cannot wait for a release (a demo, a crash), its session may ask the coordinator for a branch
`game/<game>/<topic>` cut from the game's current tag. Rules:

- It carries the minimum fix, and the game pins its commit for the time being.
- It has a merge deadline of **two weeks**: by then the coordinator merges it into `master` (opt-in, per
  the rules above) and releases a version, and the game moves back to a tag.
- Long-lived game branches (as `tyrian3d` was) are not allowed any more: they drift (`tyrian3d` and
  `master` added competing bloom implementations and the same surface-variant numbers).

## Machines

- Windows PC: coordinator, Be The Bullet Hell, CHIMERA. Builds and tests Windows (Vulkan).
- Mac: Tyrian3D. The only place Metal is verified.

## Release checks (local, no hosted CI)

Checks run on the machines, not on a hosted service. Before every tag the coordinator runs
`tools/check.ps1` on the Windows PC (C core, all shaders to SPIR-V and MSL, native tests including the GPU smoke
tests, the public headers on their own as C11 and C++17), compiles CHIMERA against the candidate
(`BTBH_RENDERER_DIR` pointed at it, a separate `CARGO_TARGET_DIR`) and the Swift layer
(`swift build --target BTBHRendererGPU`). When a release touches shaders or anything platform-specific, the
Mac session runs `tools/check.sh` (adds metallib) on the candidate and reports on its issue before the tag.
The tag's message records what ran where.
- Handoffs between the machines are files in `docs/handoff/` plus a GitHub issue that links them.
