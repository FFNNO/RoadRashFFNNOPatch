# Feature: RR95 HUD key icons with GpuWorld

Locator: `odd/tasks/rr95-gpu-hud-icons.md` (Engram mirror topic `odd/rr95-gpu-hud-icons/tasks`).

## Objective
With `[World] GpuWorld=1`, in-race HUD messages show their key and button icons (for example the ESC icon in the pause prompt "Hit [ESC] again to quit"), placed and blended exactly as in software mode.

## Problem
The game queues 32-bit icons for the message codes `\x01`-`\x05` (`FUN_00463e20`, up to 8, count at `0x005ce068`) and only `FUN_00464140(uint32_t* dib, int width, int height)` alpha-blends them into the 32bpp DIB. The race present detour (`renderer/src/hook_sites.cpp`, `Detour_FUN_0046e630`) calls it only on the BGRA software path. With GpuWorld it calls `PresenterPresent8bpp`, which builds the HUD layer from the 8bpp surface, so the icons are lost.

## Findings (disassembly of `RoadRash95.exe`, 2026-11-11)
- `FUN_00464140` blends premultiplied BGRA texels: alpha 0 leaves the pixel, alpha 255 copies the colour, otherwise each channel is `min(255, dst * (255 - a) / 255 + src)`. It writes the top DIB byte as 0.
- It sets the queue count `0x005ce068` to 0 on return (`0x00464345`), so a second call needs the count restored.
- The original race present `FUN_0046e630` calls it through the thunk `0x0040268A`, so the GDI fallback still blends the icons while the queue is kept.

## Scope
- GPU world frames with a non-empty icon queue: run `FUN_00464140` over a black and a white scratch buffer, recover a premultiplied BGRA layer (`renderer/src/hud_icons.*`), upload it to a dynamic texture and draw it after the HUD layer with blend (ONE, INV_SRC_ALPHA), at the HUD layer position (centred 640 rect in widescreen).
- Premultiplied output instead of straight colour: the game's blend is the premultiplied "over", so (ONE, INV_SRC_ALPHA) reproduces it without a division and without losing precision.
- The queue count is cleared after a successful GPU present, as the software path does; on a failed present it is kept for the GDI fallback.

## Constraints
- Software mode (GpuWorld off, Scroll Lock) stays byte-identical; the game's own DIB is never passed to `FUN_00464140` in GPU mode.
- Nothing is allocated or uploaded when the queue is empty.
- A failure of the icon layer is logged once and skips the icons; it never fails the frame.

## Tasks
- [x] I1 Pure alpha-recovery helper `RecoverHudIconLayer` with unit tests (transparent, opaque, 50% blend of a known colour, clamping, malformed pair, empty). Route: delegated writer. Check: RED observed (hook_test failed to build: missing `hud_icons.h`), then GREEN.
- [x] I2 GPU glue: icon layer build in the race present detour, presenter texture, pixel shader, premultiplied blend and draw over the HUD layer. Route: delegated writer (2+ non-trivial files). Check: build `/W4 /WX` OK; no automated test for the D3D glue.
- [ ] I3 In-game check with GpuWorld=1: pause prompt shows the ESC icon, in 4:3 and widescreen; software mode unchanged.
- [x] I4 Localize the widescreen option label: the graphics menu item text follows the game's language global `DAT_00595968` (0 English, 0x10 French, 0x20 German, 0x30 Italian, 0x40 Spanish, other values fall back to English), read on every draw in the `FUN_0045b930` detour. Labels: "Widescreen", "Écran large", "Breitbild", "Schermo panoramico", "Pantalla panorámica". Pure mapping `WidescreenMenuLabel(language)` in `renderer/src/options_menu.*`. Route: inline (one mechanical mapping plus call site). Check: RED observed (hook_test failed to build: `kGameLanguageSpanish` undeclared), then GREEN.

## Acceptance criteria
- The pause prompt and other icon messages show their icons with GpuWorld=1, at the same place as in software mode.
- Software mode output is unchanged.

## Checks
- `cmd //c "renderer\build.bat"`
- `renderer/build/Release/hook_test.exe` (must end with `OK (0 failure(s))`)
- `renderer/build/Release/forward_test.exe`
- `python renderer/tools/check_hook_sites.py` (needs the git-ignored game executables)

## Progress
- 2026-11-11 I1+I2 (worktree `fix/gpu-hud-icons`, base `e038520`):
  - `cmd //c "renderer\build.bat"`: `build.bat: OK` (only MSB8029 warnings about the build dir under %TEMP%).
  - `renderer/build/Release/hook_test.exe`: `OK (0 failure(s))`, 431 PASS lines, 14 new `hud icons:` checks.
  - `renderer/build/Release/forward_test.exe`: `OK (0 failure(s))`.
  - `check_hook_sites.py`: skipped, the original/patched executables are not in the worktree. No hook site bytes changed.
  - Commit: recorded in the delegation report (this document is part of that commit).

- 2026-11-11 I4 (worktree `fix/gpu-hud-icons`, base `b620584`):
  - `cmd //c "rendereruild.bat"`: `build.bat: OK`.
  - `renderer/build/Release/hook_test.exe`: `OK (0 failure(s))`, 439 PASS lines, 8 new `menu I4:` checks; the two W3 Spanish label checks now pass the Spanish index explicitly.
  - `renderer/build/Release/forward_test.exe`: `OK (0 failure(s))`.
  - `check_hook_sites.py`: skipped, the game executables are not in the worktree. No hook site bytes changed.
  - Not tested: the live read of `0x00595968` in the game and the on-screen fit of the French/Italian labels in the 180 px button.
  - Commit: recorded in the delegation report (this document is part of that commit).

## Next step
I3 in-game check by the user (also check the I4 label in the game's language).
