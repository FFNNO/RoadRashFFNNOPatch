# Feature: RR95 fixed save path and on-screen save feedback

Locator: `odd/tasks/rr95-save-feedback.md` (Engram mirror topic `odd/rr95-save-feedback/tasks`).

## Objective
Saving writes the game to one default file without a file dialog, loading reads that same file, and a short "game saved" message appears on screen (in the side margin when there is one).

## Problem
Road Rash 95 asks for a file name in a Windows dialog on the first save, on every "Salvar Juego Como..." and on every load. After a successful save it shows nothing, so the player gets no feedback.

## Why
The user wants saving and loading to be one step and wants visual confirmation that the save worked.

## Findings (read-only map, 2026-11-09)
- Save routine `FUN_0043f7c0` and load routine `FUN_0043f580` (`out/decomp.c:32522`, `:32433`) take no arguments and use the global file name buffer at `0x604320` (MAX 0x103 bytes). The save file is 160 bytes (`0xa0`), magic `0xCAFEDEAD` plus checksum. On success the save only clears the dirty flag `0x5cd47e`; errors go through `FUN_00458a30` (MessageBoxA).
- Dialogs: COMDLG32 `GetSaveFileNameA` (IAT slot `0x61c0b8`, call sites `0x45ae07` Salvar Juego, `0x45aef6` Salvar Juego Como, `0x45b77e` save-before-quit) and `GetOpenFileNameA` (IAT slot `0x61c0b4`, call site `0x45ae8f` Cargar Juego). `lpstrFile` is `0x604320`, default extension `RRS`, initial folder from `HKLM\SOFTWARE\Electronic Arts\RoadRash 95\Path` (the game folder).
- Existing save on this machine: `<game folder>\Partida 1.RRS`.
- The mod has one IAT helper (`InstallIatStretchDIBitsHook`, `renderer/src/hook_sites.cpp:80`) and no text drawing in the presenter.

## Scope
- Default save file: `<game folder>\Partida 1.RRS`, configurable with `[Save] File=` in `rr95gpu.ini` (relative paths resolve against the game folder). Using the existing name keeps the player's current progress loadable.
- Every save dialog (Salvar Juego, Salvar Juego Como, save-before-quit) is answered with the default file without showing the dialog.
- The load dialog is answered with the default file when it exists; when it does not exist, the real dialog opens as before.
- After a successful save, the text "Partida guardada" is drawn for about 3 s; after a successful load, "Partida cargada". It goes in the side margin (pillarbox) when the margin is wide enough, otherwise in a corner of the image over a translucent background. It works in the menu present path and the race present path.
- `[Save] Enabled=0` restores the original dialogs and shows no message.

## Constraints
- No change to the save file format or to the game's own save and load routines.
- Hooks follow the existing `HookSite`/IAT patterns, are verified against the expected bytes, and are restored on uninstall.
- Toast text is Spanish to match the game's language; code and comments stay in English.
- No teardown at process exit (leaked singletons, as in the renderer).

## Tasks
- [x] S1 Redirect the COMDLG32 save and open dialogs to the default file (generalized IAT helper, config, path resolution) with unit tests. Route: delegated writer (2+ non-trivial files).
- [x] S2 Detect a successful save and load and draw the toast in both present paths (GDI-rendered text texture, margin placement) with unit tests for placement and timing. Route: delegated writer, same work unit as S1 if it stays coherent.
- [ ] S3 In-game check: save from the menu without a dialog, file written at the default path, toast visible; load from the menu without a dialog; missing file falls back to the dialog.

## Acceptance criteria
- Saving from any menu entry shows no dialog and updates `<game folder>\Partida 1.RRS` (size 160, new timestamp).
- Loading shows no dialog when the file exists and restores that game.
- "Partida guardada" / "Partida cargada" is visible on screen after the action and disappears after about 3 s.
- `[Save] Enabled=0` gives the original behaviour.
- hook_test, forward_test and check_hook_sites pass.

## Checks
- `cmd //c "renderer\\build.bat"`, `renderer/build/Release/hook_test.exe`, `renderer/build/Release/forward_test.exe`, `python renderer/tools/check_hook_sites.py`.
- In-game: Steam game folder, screenshots in `out/screens/2026-11-09-save/`.

## Progress
- 2026-11-09: S1+S2 in one work unit `b595d61` (delegated writer; no clean file split, S1 alone does not compile). Generalized IAT helper; COMDLG32 GetSaveFileNameA/GetOpenFileNameA detours; inline hooks on FUN_0043f7c0 (`55 8B EC 83 EC 0C`) and FUN_0043f580 (`55 8B EC 83 EC 10`), bytes verified in original, patched and Steam exes. Success rule: the detours clear the block magic at 0x5c4d0c first; save succeeds when the block is valid afterwards, dirty flag 0x5cd47e is 0, and the file is 160 bytes and new or re-timestamped; load succeeds when the block is valid and the dirty flag is 0. Toast: GDI text into a BGRA texture, alpha-blended quad after the final pass, right side margin (36 px font down to 20 px) or image corner on a 60% dark box; a 50 ms TIMERPROC on the game thread re-presents the last frame in static menus. RED 272 PASS / 34 FAIL with stubs; GREEN hook_test 306 PASS / 0 failures, forward_test OK, check_hook_sites 16 OK. Native review approved (1 lens, lineage review-f2175d6fb565d047) and acknowledged; reviewed boundary `b595d61`. Advisories: R3-001 toast leaves PS CB0/sampler 0/viewport/shaders bound (benign: the world pass and the final pass rebind them every frame); R3-002 no tests for the presenter toast glue and the dialog/save detours; R3-003 coarse timestamps can hide a toast for two saves in the same tick. Installed in Steam (DLL backup ddraw.dll.b6-19a3d34; save backup backups/saves/Partida 1.RRS.2026-11-09). S3 pending: automated menu navigation could not reach the save menu (intro and attract videos), so the in-game check needs the user.
- 2026-11-09: Feature document created after a read-only map of the save and load flow.

## Next step
S3: user saves and loads from the in-game menu and reports dialog, file and toast.
