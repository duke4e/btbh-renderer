# RC Roadmap 1-7 Windows Plan

Status: implemented and verified on Windows; native Mac acceptance deferred

Date: 2026-08-23

Design:
`docs/superpowers/specs/2026-08-23-rc-roadmap-1-7-windows-design.md`

1. [x] Close the remaining camera-origin/occlusion upload hitch and retain the
   completed pass-level HRC profiling evidence.
2. [x] Implement short-history HRC reprojection, deterministic temporal phasing,
   disocclusion rejection, invalidation, and pass telemetry.
3. [x] Add depth/normal/visibility-guided HRC reconstruction and compare it against
   the same deterministic High and Low captures.
4. [x] Introduce explicit light classes plus spatial energy-density policies, then
   migrate the existing player, XP, loot, shrine, and hit sources.
5. [x] Audit and complete the HRC material response contract with focused BRDF/AO/
   emission tests and final-indirect captures.
6. [x] Add stable authored city lights plus enemy-attack and hazard emitters using
   authoritative game lifecycles and presentation colors.
7. [x] Tune and lock Windows Low/High/Auto presets, run repeated performance and
   visual gates, and record the remaining Mac-only acceptance separately.

Execution is continuous and dependency ordered. No commits or worktrees are
part of this plan.
