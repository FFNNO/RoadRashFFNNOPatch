# Feature: RR95 widescreen race view with an in-game option

Locator: `odd/tasks/rr95-widescreen.md` (Engram mirror topic `odd/rr95-widescreen/tasks`).

## Objective
Render races at the window's aspect ratio (16:9 at 1920x1080) with a wider horizontal field of view, not a stretched image, and let the player turn it on or off from the game's own graphics options menu.

## Problem
The engine projects and culls the world for a 320x240 (640x480 in HiRes) 4:3 view, so the GPU renderer draws a 1440x1080 image with black side bars.

## Findings (read-only map, 2026-11-09)
- `DAT_00591c0c` (initial 320) is the single projection width: screen centre X = c0c/2 (`FUN_00433730`, `decomp.c:26020-26082`; `FUN_00459170`), every object/road screen-X cull tests against `c0c<<16` (`FUN_00433b30`, `FUN_0041c9e0`, `FUN_0044e870`, `FUN_0044f070`, road functions `FUN_0041cbe0..FUN_0041f6c0`, `FUN_004666a0..FUN_00467670`), and race init `FUN_00457ef0` (0x457f4a) passes `[0x591c0c]`, `[0x591c08]` to `FUN_0046de40`, which sets the clip rect to `(0, 0, w<<hr, (h<<hr)-1)`. `0x591C04` (320) feeds the clip in `FUN_0046d620`.
- Projection: `sx = dx * 2^24/z + c0c/2`, `sy = 90 + dy * 2^24/z` in 320x240 16.16 space (scale table `DAT_00597c90`, filled in `FUN_0045aa90`); HiRes doubles at snap time. Current horizontal FOV about 64 degrees; c0c = 426 gives about 79.5 degrees with the same vertical scale.
- `FUN_004688e0` rejects a quad only when its bounding box is fully outside the clip rect; partly visible quads reach the capture with unclipped vertices.
- Fixed 640-wide pieces: surface `DAT_005cd4b0` 640x480 (`FUN_0045f7f0`), `DAT_005a2934`/`DAT_005a2938` 640x480, horizon blit `FUN_00469770` (from the 720-wide horizon surfaces `DAT_005cd4c4`/`c8`, covers 640 px), HUD blits `FUN_00469460`/`FUN_00469530`/`FUN_00469990`, overlay `FUN_0045d6d0`. With a wider clip the CPU rasterizers would write past the 640 pitch, so widescreen requires GpuWorld with the software world rasterizer and flat fills skipped.
- Options menu id 19 "Opciones Gráficas y de Sonido": items at 0x5a08a8 (0x38 bytes each, count 8 at 0x5a0a70); slot 5 at 0x5a09c0 is empty (label 0, handler 0) between "Pantalla" (slot 4) and "Ya está" (slot 6). Item layout: +4 id, +8 label string id, +0xC..+0x18 rect, +0x1C..+0x21 navigation, +0x24 flags, +0x28 enabled, +0x2C select handler, +0x30 help id. On/off items use flag 0x100000 and value text from `FUN_0045b930` ("On"/"Off"). Strings come from `DAT_005cd49c[id]` (ESP.DLL ids 1..993, no free id). Settings persist with `RegSetValueExA` under `HKCU\SOFTWARE\Electronic Arts\RoadRash 95` and load in `FUN_0045fe20`.

## Scope
- `[World] Widescreen=auto|off|<projection width>` in `rr95gpu.ini`; `auto` derives c0c from the window aspect (even number, `round(240 * aspect)`, 426 at 16:9), only when GpuWorld is on. Applied at race init, never mid-race.
- GPU renderer: source width `c0c << HiRes` mapped to the full window width; HUD and dashboard stay a centred 4:3 layer; horizon drawn across the full width.
- In-game option "Pantalla panorámica" (Sí/No) in graphics menu slot 5, persisted as a `Widescreen` registry value next to the game's own settings; takes effect at the next race.

## Constraints
- Hooks and patches verify expected bytes on both known exe builds; no change to game data files.
- Never let the CPU rasterizer write outside the 640x480 surface.
- 60 fps; GpuWorld=0 or Widescreen=off keeps today's behaviour exactly.
- Record any registry change made during testing in `backups/CONTROLS-RESTORE.md`.

## Tasks
- [ ] W1 Engine widescreen: config, patch `0x591C0C`/`0x591C04` at race init, guarantee the software world rasterizer and flat fills are skipped for every quad, presenter source-width mapping, centred 4:3 HUD layer. Route: delegated writer (2+ non-trivial files). Check: unit tests for width derivation and mapping; in game at 1920x1080 the world fills the window, 60 fps, no crash, HUD centred.
- [ ] W2 Horizon across the full width (GPU-drawn from the 720-wide horizon sources with wrap, or an extended blit). Route: delegated writer. Check: in game, no gaps or seams at the sides while turning.
- [ ] W3 In-game option in graphics menu slot 5 with registry persistence. Route: delegated explorer + writer. Check: in game the item shows and toggles, the value survives a restart, the next race uses it.

## Acceptance criteria
- At 1920x1080 with the option on, races fill the window with a wider field of view, no stretching, HUD centred, 60 fps.
- With the option off, or GpuWorld=0, the game looks exactly as before.
- The graphics options menu shows "Pantalla panorámica" and remembers the choice.

## Delivery
Commits on `feat/gpu-renderer` (tracks the public `origin/main`); pushing stays the user's decision. No pull-request slicing (no PR workflow on this repo).

## Progress
- 2026-11-09: Feature document created after a read-only map of projection, culling and the options menu.

## Next step
W1 through a delegated writer.
