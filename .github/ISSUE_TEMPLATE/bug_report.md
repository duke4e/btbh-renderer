---
name: Bug report
about: Report a reproducible renderer bug a game hit
title: "[Game][BUG] ..."
labels: bug
assignees: ''

---

<!-- The coordinator answers here and links the release that fixes it. -->

## Consumer

Which game (Be The Bullet Hell / CHIMERA / Tyrian3D), its pinned renderer version (tag or commit), and
which session or machine to answer.

## What happens

What you see, and what you expected instead. Screenshots or a presented-frame PNG
(`btbh_gpu_window_readback_presented_frame` + `btbh_gpu_write_png_rgba8`) help most.

## Steps to reproduce

The smallest host code or game steps that trigger it, and the `BTBHGPUSceneReport` /
`BTBHGPUWindowReport` result and error text if a call failed.

```c
```

## Environment

- Platform and backend: Windows/Vulkan, macOS/Metal, Linux/Vulkan
- GPU and driver:
- Debug or release build:

## Since when

The last renderer version that worked, if known.
