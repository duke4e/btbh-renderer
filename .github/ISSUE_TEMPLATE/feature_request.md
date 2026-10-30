---
name: Feature request
about: Ask the renderer for something a game needs
title: "[Game] ..."
labels: request
assignees: ''

---

<!-- One request per issue. The coordinator answers here and links the release that delivers it.
     Changes that affect several games or are breaking need an RFC (docs/rfc/). -->

## Consumer

Which game asks (Be The Bullet Hell / CHIMERA / Tyrian3D), its current pinned renderer version, and
which session or machine to answer.

## Need

What the game cannot do today, in its own terms (what the player should see). A screenshot or reference
helps.

## Proposed API

What you would like in `include/btbh_gpu/btbh_gpu.h` (new function, struct fields, enum values). Say
what the zero value of each new field should mean: new features are opt-in, so zero must keep today's
behaviour.

```c
```

## Minimal example

The smallest host code (C, Swift or Rust) that would use it.

```c
```

## Platforms

Which must work: Windows/Vulkan, macOS/Metal, Linux/Vulkan. Any device capability it depends on.

## Urgency

When the game needs it, and whether an emergency game branch (COORDINATION.md) is needed.
