# Feature: RR95 draw distance and distant-ground artifacts

Locator: `odd/tasks/rr95-draw-distance.md` (Engram mirror topic `odd/rr95-draw-distance/tasks`).

## Objective
Remove the visible engine limits on distant ground: track segments that pop in at the far end of the view, and ground that looks cut where the road crosses a crest (for example the checkered finish line seen from far away in La Ciudad, where squares look missing).

## Problem
The engine projects at most 48 track segments ahead (`DAT_0059ec90`, cap `0x30`, slices in the fixed array `DAT_0060bd00`, stride 0x224). Each segment appears whole when it enters that range. Captured polygons carry only screen positions; per-row depth (`+0x18`) and 2^24/z (`+0x20`) exist only in the row structs projected by `FUN_00459170` (decomp.c:48364-48370). The cause of the cut look over crests is not confirmed yet (hypothesis: the engine's own hill culling or painter order).

## Scope
1. GPU renderer only: capture per-row depth and fade the farthest segments into the horizon instead of popping.
2. GPU renderer with depth: resolve occlusion over crests on the GPU, once the engine's crest rule is understood.
3. Only if 1 and 2 are not enough: raise the 48-segment cap by relocating the slice array and every reference to it.

## Constraints
- Hooks verify expected bytes on both known exe builds; no change to game data files.
- GpuWorld=0 and software mode stay exactly as today; the new behaviour can be turned off from `rr95gpu.ini`.
- 60 fps.
- Step 3 changes engine data; it needs its own risk review (AI, collisions, traffic) before any write.

## Tasks
- [x] D0 Read-only map: row structs and their link to captured quads, the crest culling rule, the far-segment cutoff, all references to `DAT_0060bd00` / `DAT_0059ec90`. Route: delegated explorer (mapping trigger). Check: path:line evidence, one parent spot check.
- [ ] D2a Draw thin far quads instead of culling them: the capture culls textured quads whose snapped minY == maxY (main:renderer/src/capture.cpp:594 and :1388), copying FUN_0042e850's early-out. At 1080p with sub-pixel corners the neighbouring rows no longer cover that scanline, which likely leaves the missing rows of squares. Under GpuWorld with sub-pixel corners, keep quads whose 16.16 corners have height > 0. Route: delegated writer. Check: unit tests for the cull decision; in game, compare the far finish line with Scroll Lock.
- [ ] D1 Distance fade for the farthest segments. Route: delegated writer. Check: unit tests for the fade mapping; in game, no popping at the far end, 60 fps.
- [ ] D2 Crest occlusion with depth. Route: delegated writer. Check: in game, the finish line and crests in La Ciudad no longer look cut, compared with Scroll Lock.
- [ ] D3 Larger draw distance (only if needed). Route: decided after D0. Check: full race with AI and traffic, no crash.

## Acceptance criteria
- Distant ground no longer pops in or looks cut on La Ciudad at 1920x1080, compared with the software renderer.
- 60 fps; GpuWorld=0 unchanged.

## Delivery
Branch `feat/draw-distance` from `main` (worktree, because another session edits the main checkout). Push and release stay the user's decision. Strategy: ask-on-risk.

## Progress
- 2026-11-11: D0 done (delegated explorer, parent spot check of capture.cpp:588-602 and 1380-1395). Verified: the engine draws far to near with no depth test (FUN_0045ac50, VA 0x45ad2a-0x45ad77); the crest rule is the per-slice horizon DAT_0060bce0 in FUN_00459cb0 (decomp 48918-48955), which only drops whole slices of road tiles; the segment count is the constant push 0x31 at VA 0x45aca3, clamped to [1, 0x30]; rows sit inline in each slice (+0x1c, +0x120) with z at row+0x18. Inferred: the cut look comes from the renderer's minY == maxY cull, not from the engine. D3 needs the slice array (8 absolute refs), DAT_00608320, DAT_0059ec98, two row caches and the reciprocal table DAT_00597c90 extended; no AI or collision code references slices directly. Order changed: D2a (cheap, likely cause) before D1.
- 2026-11-11: Feature document created. Steps 1 to 3 approved by the user in order, stopping when the result is good enough.

## Next step
D2a through a delegated writer.
