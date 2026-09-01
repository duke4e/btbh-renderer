# RC Light Classes and Density Budget Plan

Status: complete

Date: 2026-08-23

Design:
`docs/superpowers/specs/2026-08-23-rc-light-classes-density-budget-design.md`

1. [x] Add failing core tests for class metadata and deterministic spatial
   energy budgets.
2. [x] Implement the reusable renderer policy and apply it after engine/gameplay
   emitter merging.
3. [x] Propagate class metadata through the C ABI and 64-byte GPU source record.
4. [x] Migrate player, XP, loot, medi-charge, shrine, and impact sources.
5. [x] Mirror public/native changes into the game Vendor copy and run focused
   renderer/game/native tests.
6. [x] Run the production Release build and a Windows Vulkan smoke/performance
   gate, then close roadmap item 4.
