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
- [ ] D2b Diagnostic counters, no behaviour change: log every 5 s why thin textured quads on the GPU path are kept or culled (zero 16.16 height, no exact corner match, pdatPtr == 0, clip rect or x-span, KeepThinQuads/SubPixel off), and how many road slices the engine's horizon rule (FUN_00459cb0, flag byte slice+0xC == 0) drops per race frame and how near they are. Route: delegated writer. Check: unit tests for the reason classification and the slice-flag summary; in game, the user's log tells which cause remains. Open until that log is analysed.
- [ ] D1 Distance fade for the farthest segments. Route: delegated writer. Check: unit tests for the fade mapping; in game, no popping at the far end, 60 fps.
- [ ] D2 Crest occlusion with depth. Route: delegated writer. Check: in game, the finish line and crests in La Ciudad no longer look cut, compared with Scroll Lock.
- [ ] D3 Larger draw distance (only if needed). Route: decided after D0. Check: full race with AI and traffic, no crash.

## Acceptance criteria
- Distant ground no longer pops in or looks cut on La Ciudad at 1920x1080, compared with the software renderer.
- 60 fps; GpuWorld=0 unchanged.

## Delivery
Branch `feat/draw-distance` from `main` (worktree, because another session edits the main checkout). Push and release stay the user's decision. Strategy: ask-on-risk.

## Progress
- 2026-11-11: D2b implemented (delegated writer), log analysis pending, so the box stays open. ClassifyThinTexturedQuadForGpu returns the reason (NotThin, Kept, CulledDisabled, CulledNoPdat, CulledZeroHeight, CulledNoCornerMatch, CulledClip) and ShouldCullTexturedQuadForGpu now derives its decision from it (same results; a test checks they agree for every case and flag combination). CaptureTexturedQuadAfter counts the verdict of every GPU-path textured quad into fixed per-frame counters, folded at CaptureEndFrame. At each race present (FinishRaceFrameAndPollHotkeys, only with world hooks installed) the flag bytes of slices 0..DAT_0059ec90 in DAT_0060bd00 are read after checking the count is in [1, 0x30]; nothing is read otherwise. New 5 s log lines "ThinQuads: ..." and "RoadSlices: ...". Crack counter (adjacent quads with different shared 16.16 edges) skipped: it needs neighbour matching across quads, not a small helper. RED: stubs, hook_test FAILED (14 failure(s)). GREEN: build.bat OK, hook_test OK (0 failure(s)), forward_test OK (0 failure(s)). check_hook_sites.py skipped (exes not in the worktree); no hook site changed. Commit: see git log (feat(renderer): log why thin far quads and road slices are dropped).
- 2026-11-11: D2a in-game check by the user (ed070cb build, La Ciudad): the missing far rows are much less frequent but still appear now and then. D2a stays open until the remaining cases are explained.
- 2026-11-11: D2a implemented (delegated writer), in-game check pending, so the box stays open. Under GpuWorld (software rasterizer skipped) with [World] SubPixel=1 and the new [World] KeepThinQuads=1 (default), ShouldCullTexturedQuadForGpu keeps a textured quad whose snapped vertices lie on one scanline when its resolved 16.16 corners span a height, and ResolveSubPixelVertices now resolves corners for such textured quads. Kept: zero 16.16 height, pdatPtr == 0, clip rect and x-span culls; software path (rasterizer ran, Scroll Lock or GpuWorld=0) culls as before; KeepThinQuads=0 restores the old cull. Mip: level 0 like every GPU-drawn quad (the engine chose none); with MipFilter=trilinear the level comes from the analytic gradients of the thin 16.16 extent, which mirrors the engine's extent rule; MipFilter=chosen samples level 0 as before. RED: stubbed decision, hook_test FAILED (2 failure(s)). GREEN: build.bat OK, hook_test OK (0 failure(s)), forward_test OK (0 failure(s)). check_hook_sites.py skipped (exes not in the worktree); no hook site changed. Commit: see git log (fix(renderer): keep thin far quads that the GPU can draw).
- 2026-11-11: D0 done (delegated explorer, parent spot check of capture.cpp:588-602 and 1380-1395). Verified: the engine draws far to near with no depth test (FUN_0045ac50, VA 0x45ad2a-0x45ad77); the crest rule is the per-slice horizon DAT_0060bce0 in FUN_00459cb0 (decomp 48918-48955), which only drops whole slices of road tiles; the segment count is the constant push 0x31 at VA 0x45aca3, clamped to [1, 0x30]; rows sit inline in each slice (+0x1c, +0x120) with z at row+0x18. Inferred: the cut look comes from the renderer's minY == maxY cull, not from the engine. D3 needs the slice array (8 absolute refs), DAT_00608320, DAT_0059ec98, two row caches and the reciprocal table DAT_00597c90 extended; no AI or collision code references slices directly. Order changed: D2a (cheap, likely cause) before D1.
- 2026-11-11: Feature document created. Steps 1 to 3 approved by the user in order, stopping when the result is good enough.

## Next step
In game (La Ciudad, GpuWorld=1, SubPixel=1): race past the far finish line and crests, then send the "ThinQuads:" and "RoadSlices:" log lines; they decide whether D2a needs another fix, D2 (crest rule) or D1 comes next.
