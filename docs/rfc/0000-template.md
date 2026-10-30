# RFC NNNN: Title

- **Status:** Draft | Accepted | Rejected | Implemented in vX.Y.Z
- **Requested by:** game(s)
- **Affects:** games that must change or re-check their look
- **Issue:** link

## Need

What a game cannot do today, in its own terms (what the player should see). Screenshots or a reference
welcome.

## Proposal

The public API: new or changed declarations in `include/btbh_gpu/btbh_gpu.h`, written out. Say what the
zero value of every new field means (it must be "as before").

```c
/* the declarations */
```

## Minimal example

The smallest host code that uses it.

```c
```

## Implementation

Passes, shaders (new files, uniform layout changes), resources, cost (GPU time at 1080p, memory), and
device capabilities it needs (`BTBH_GPU_FEATURE_*`) with the fallback.

## Compatibility

- Versioning: minor (additive, opt-in) or breaking, and why.
- What each consumer must change: Be The Bullet Hell (Swift layer), CHIMERA (Rust bindings), Tyrian3D.

## Platforms and testing

Windows/Vulkan, macOS/Metal, Linux/Vulkan: what is verified where. The smoke scene or test that covers it.

## Alternatives

What else was considered and why not.

## Open questions
